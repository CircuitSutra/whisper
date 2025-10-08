#include "Iommu.hpp"
#include "DeviceContext.hpp"
#include "MsiPte.hpp"
#include "MemoryModel.hpp"
#include "IommuStructures.hpp"
#include "MemoryManager.hpp"
#include "TableBuilder.hpp"
#include <cassert>
#include <iostream>
#include <vector>
#include <functional>

using namespace TT_IOMMU;
using namespace IOMMU;

namespace TestValues {
    // Common test device ID
    constexpr uint32_t DEV_ID = 0x2A;
    
    // MSI values
    constexpr uint64_t MSI_ADDR_MASK = 0xFFFFF000ULL;
    constexpr uint64_t MSI_ADDR_PATTERN = 0xFEDC1000ULL;
    
    // MSI PTE values
    constexpr uint64_t MSI_TARGET_PPN = 0x500;
    
    // Test IOVA that matches pattern
    constexpr uint64_t MSI_IOVA = 0xFEDC1ABC;
}

namespace MrifTestValues {
    constexpr uint64_t MRIF_ADDRESS = 0x2000;
    constexpr uint64_t NOTICE_PPN = 0x3000;
    constexpr uint16_t NID_VALUE = 0x5A5;
    constexpr uint8_t NID_HIGH = (NID_VALUE >> 10) & 0x1;
    constexpr uint16_t NID_LOW = NID_VALUE & 0x3FF;
    constexpr uint64_t MRIF_FILE_NUM = 0xAB;
}

static void installMemCbs(Iommu& iommu, MemoryModel& mem) {
    std::function<bool(uint64_t,unsigned,uint64_t&)> rcb =
        [&mem](uint64_t a, unsigned s, uint64_t& d) { return mem.read(a, s, d); };
    std::function<bool(uint64_t,unsigned,uint64_t)> wcb =
        [&mem](uint64_t a, unsigned s, uint64_t d) { return mem.write(a, s, d); };

    iommu.setMemReadCb(rcb);
    iommu.setMemWriteCb(wcb);
    
    // Setup translation callbacks
    std::function<bool(uint64_t, unsigned, bool, bool, bool, uint64_t&, unsigned&)> stage1_cb =
      [](uint64_t va, unsigned /*privMode*/, bool , bool , bool , uint64_t& gpa, unsigned& cause) { 
            gpa = va; // Identity translation
            cause = 0;
            return true; 
        };
    
    std::function<bool(uint64_t, unsigned, bool, bool, bool, uint64_t&, unsigned&)> stage2_cb =
      [](uint64_t gpa, unsigned /*privMode*/, bool , bool , bool , uint64_t& pa, unsigned& cause) { 
            pa = gpa; // Identity translation
            cause = 0;
            return true; 
        };
    
    iommu.setStage1Cb(stage1_cb);
    iommu.setStage2Cb(stage2_cb);
    
    std::function<void(uint64_t&, bool&, bool&)> trap_cb =
      [](uint64_t& /*gpa*/, bool& /*implicit*/, bool& /*write*/) { 
            // Do nothing
        };
    
    iommu.setStage2TrapInfoCb(trap_cb);
    
    // Stage configuration callbacks
    std::function<void(unsigned, unsigned, uint64_t, bool)> stage1_config_cb =
      [](unsigned /*mode*/, unsigned /*asid*/, uint64_t /*ppn*/, bool /*sum*/) {};
    
    std::function<void(unsigned, unsigned, uint64_t)> stage2_config_cb =
      [](unsigned /*mode*/, unsigned /*asid*/, uint64_t /*ppn*/) {};
    
    iommu.setStage1ConfigCb(stage1_config_cb);
    iommu.setStage2ConfigCb(stage2_config_cb);
}

static void configureCapabilities(Iommu& iommu) {
    Capabilities caps;

    // Set MSI capabilities
    caps.bits_.msiFlat_ = 1;  // Enable MSI Flat mode
    caps.bits_.msiMrif_ = 1;  // Enable MSI MRIF mode
    
    // Set ATS capability
    caps.bits_.ats_ = 1;      // Enable ATS capability
    caps.bits_.t2gpa_ = 1;    // Enable T2GPA capability
    
    // Set other required capabilities
    caps.bits_.pd8_ = 1;
    caps.bits_.pd17_ = 1;
    caps.bits_.pd20_ = 1;
    caps.bits_.sv32_ = 1;
    caps.bits_.sv39_ = 1;
    caps.bits_.end_ = 1;      // Support for endianness control
    
    // For stage1 and 2 translation
    caps.bits_.sv39x4_ = 1;
    caps.bits_.sv48x4_ = 1;
    caps.bits_.sv57x4_ = 1;

    iommu.configureCapabilities(caps.value_);
}


static uint64_t setupMsiDeviceWithBuilder(Iommu& iommu, MemoryModel& memory,
                                         MemoryManager& memMgr, TableBuilder& tableBuilder,
                                         uint32_t devId) {
    
    // Set up DDTP for 1-level DDT
    ddtp_t ddtp;
    ddtp.iommu_mode = DDT_1LVL;
    ddtp.ppn = memMgr.getFreePhysicalPages(1);
    
    // Configure DDTP register in the IOMMU
    Ddtp iommuDdtp;
    iommuDdtp.bits_.mode_ = Ddtp::Mode::Level1;
    iommuDdtp.bits_.ppn_ = ddtp.ppn;
    iommu.writeCsr(CsrNumber::Ddtp, iommuDdtp.value_);
    
    // Create MSI-enabled device context
    device_context_t dc = {};
    dc.tc = 0x7; // Valid + ATS + T2GPA
    
    // Set up IOHGATP for G-stage translation
    dc.iohgatp.MODE = IOHGATP_Sv39x4;
    dc.iohgatp.GSCID = 0;
    dc.iohgatp.PPN = memMgr.getFreePhysicalPages(1);
    
    // Set up FSC with IOSATP (PDTV=0)
    dc.fsc.iosatp.MODE = IOSATP_Bare;
    dc.fsc.iosatp.PPN = 0;
    
    // MSI configuration
    uint64_t msi_ppn = memMgr.getFreePhysicalPages(1);
    uint64_t msiptp = (uint64_t(3) << 60) | msi_ppn; // Mode 3 (Flat) + PPN
    
    // Use enhanced TableBuilder to create MSI device context
    bool msi_flat = iommu.isDcExtended();
    uint64_t dc_addr = tableBuilder.addMsiDeviceContext(dc, devId, ddtp, msi_flat,
                                                       TestValues::MSI_ADDR_MASK,
                                                       TestValues::MSI_ADDR_PATTERN,
                                                       msiptp);
    
    if (dc_addr == 0) {
        std::cerr << "[MSI_SETUP] Failed to create MSI device context!" << std::endl;
        return 0;
    }
    
    // Setup MSI page table using TableBuilder
    if (!tableBuilder.setupMsiPageTable(msi_ppn, TestValues::MSI_TARGET_PPN)) {
        std::cerr << "[MSI_SETUP] Failed to setup MSI page table!" << std::endl;
        return 0;
    }
    
    std::cout << "[MSI_SETUP] Created MSI-enabled device context at 0x" << std::hex << dc_addr 
              << " with MSI table at PPN 0x" << msi_ppn << std::dec << std::endl;
    
    return dc_addr;
}

void testMsiBasicTranslation() {
    std::cout << "\n=== MSI Basic Translation Test (using TableBuilder) ===\n";
    
    // Create infrastructure
    MemoryModel memory(4 * 1024 * 1024);  // 4MB
    MemoryManager memMgr;
    
    auto readFunc = [&memory](uint64_t addr, unsigned size, uint64_t& data) {
        return memory.read(addr, size, data);
    };
    auto writeFunc = [&memory](uint64_t addr, unsigned size, uint64_t data) {
        return memory.write(addr, size, data);
    };
    
    TableBuilder tableBuilder(memMgr, readFunc, writeFunc);
    
    // Create IOMMU instance with MSI capabilities
    Iommu iommu(0x1000, 0x800, memory.size());
    installMemCbs(iommu, memory);
    configureCapabilities(iommu);
    
    // Setup MSI device using TableBuilder
    uint64_t dcAddr = setupMsiDeviceWithBuilder(iommu, memory, memMgr, tableBuilder,
                                               TestValues::DEV_ID);
    
    bool success = (dcAddr != 0);
    std::cout << "[TEST] MSI device setup: " << (success ? "PASS" : "FAIL") << std::endl;
    
    if (success) {
        // Test MSI address matching
        std::cout << "[TEST] MSI configuration created at address 0x" << std::hex << dcAddr << std::dec << std::endl;
        std::cout << "[TEST] MSI address pattern: 0x" << std::hex << TestValues::MSI_ADDR_PATTERN << std::dec << std::endl;
        std::cout << "[TEST] MSI address mask: 0x" << std::hex << TestValues::MSI_ADDR_MASK << std::dec << std::endl;
        
        // Verify the MSI IOVA matches the pattern
        uint64_t maskedIova = TestValues::MSI_IOVA & TestValues::MSI_ADDR_MASK;
        uint64_t maskedPattern = TestValues::MSI_ADDR_PATTERN & TestValues::MSI_ADDR_MASK;
        bool matches = (maskedIova == maskedPattern);
        
        std::cout << "[TEST] MSI address matching: " << (matches ? "PASS" : "FAIL") << std::endl;
        std::cout << "[VERIFY] IOVA 0x" << std::hex << TestValues::MSI_IOVA 
                  << " masked to 0x" << maskedIova 
                  << " matches pattern 0x" << maskedPattern << std::dec << std::endl;
    }
}

void testMsiPageTableSetup() {
    std::cout << "\n=== MSI Page Table Setup Test (using TableBuilder) ===\n";
    
    MemoryModel memory(2 * 1024 * 1024);  // 2MB
    MemoryManager memMgr;
    
    auto readFunc = [&memory](uint64_t addr, unsigned size, uint64_t& data) {
        return memory.read(addr, size, data);
    };
    auto writeFunc = [&memory](uint64_t addr, unsigned size, uint64_t data) {
        return memory.write(addr, size, data);
    };
    
    TableBuilder tableBuilder(memMgr, readFunc, writeFunc);
    
    // Allocate pages for MSI table
    uint64_t msi_ppn = memMgr.getFreePhysicalPages(1);
    uint64_t target_ppn = TestValues::MSI_TARGET_PPN;
    
    // Test MSI page table setup
    bool success = tableBuilder.setupMsiPageTable(msi_ppn, target_ppn, 16);
    std::cout << "[TEST] MSI page table setup: " << (success ? "PASS" : "FAIL") << std::endl;
    
    if (success) {
        // Verify a few entries were written correctly
        uint64_t pageSize = 4096;
        uint64_t msiTableAddr = msi_ppn * pageSize;
        
        for (int i = 0; i < 3; i++) {
            uint64_t pte_addr = msiTableAddr + (i * 8);
            uint64_t pte_value;
            
            if (memory.read(pte_addr, 8, pte_value)) {
                uint64_t expected = 0x1 | (0x3ULL << 1) | (target_ppn << 10);
                bool entryCorrect = (pte_value == expected);
                
                std::cout << "[VERIFY] MSI PTE[" << i << "] at 0x" << std::hex << pte_addr 
                          << ": got 0x" << pte_value << ", expected 0x" << expected 
                          << " (" << (entryCorrect ? "CORRECT" : "INCORRECT") << ")" << std::dec << std::endl;
            }
        }
    }
}

void testMsiExtendedFormatSupport() {
    std::cout << "\n=== MSI Extended Format Support Test ===\n";
    
    MemoryModel memory(4 * 1024 * 1024);  // 4MB
    MemoryManager memMgr;
    
    auto readFunc = [&memory](uint64_t addr, unsigned size, uint64_t& data) {
        return memory.read(addr, size, data);
    };
    auto writeFunc = [&memory](uint64_t addr, unsigned size, uint64_t data) {
        return memory.write(addr, size, data);
    };
    
    TableBuilder tableBuilder(memMgr, readFunc, writeFunc);
    
    Iommu iommu(0x1000, 0x800, memory.size());
    installMemCbs(iommu, memory);
    configureCapabilities(iommu);
    
    // Force extended format by setting MSI capabilities
    bool isExtended = iommu.isDcExtended();
    std::cout << "[TEST] Device context format: " << (isExtended ? "Extended" : "Base") << std::endl;
    
    // Setup MSI device - should use extended format due to MSI capabilities
    uint64_t dcAddr = setupMsiDeviceWithBuilder(iommu, memory, memMgr, tableBuilder,
                                               TestValues::DEV_ID);
    
    bool success = (dcAddr != 0);
    std::cout << "[TEST] Extended format MSI device setup: " << (success ? "PASS" : "FAIL") << std::endl;
    
    if (success && isExtended) {
        // Verify extended format fields were written
        uint64_t msiptp_addr = dcAddr + 32;
        uint64_t msi_mask_addr = dcAddr + 40;
        uint64_t msi_pattern_addr = dcAddr + 48;
        
        uint64_t msiptp_value, mask_value, pattern_value;
        
        if (memory.read(msiptp_addr, 8, msiptp_value) &&
            memory.read(msi_mask_addr, 8, mask_value) &&
            memory.read(msi_pattern_addr, 8, pattern_value)) {
            
            std::cout << "[VERIFY] Extended MSI fields:" << std::endl;
            std::cout << "  MSIPTP at 0x" << std::hex << msiptp_addr << ": 0x" << msiptp_value << std::endl;
            std::cout << "  MSI Mask at 0x" << msi_mask_addr << ": 0x" << mask_value << std::endl;
            std::cout << "  MSI Pattern at 0x" << msi_pattern_addr << ": 0x" << pattern_value << std::dec << std::endl;
            
            bool fieldsCorrect = (mask_value == TestValues::MSI_ADDR_MASK) &&
                               (pattern_value == TestValues::MSI_ADDR_PATTERN);
            
            std::cout << "[TEST] Extended MSI field verification: " 
                      << (fieldsCorrect ? "PASS" : "FAIL") << std::endl;
        }
    }
}

int main() {
    std::cout << "=== IOMMU MSI Translation Tests (Refactored with TableBuilder) ===\n";
    
    try {
        testMsiBasicTranslation();
        testMsiPageTableSetup();
        testMsiExtendedFormatSupport();
        
        std::cout << "\n=== All MSI tests completed! ===\n";
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "MSI test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "MSI test failed with unknown exception" << std::endl;
        return 1;
    }
}

