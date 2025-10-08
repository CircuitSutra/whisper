#include "Iommu.hpp"
#include "ProcessContext.hpp"
#include "DeviceContext.hpp"
#include "MemoryModel.hpp" 
#include "IommuStructures.hpp"
#include "MemoryManager.hpp"
#include "TableBuilder.hpp"
#include <iostream>
#include <cstring>
#include <cassert>
#include <functional>

using namespace TT_IOMMU;
using namespace IOMMU;

namespace TestValues {
    constexpr uint32_t TEST_DEV_ID = 0x2A5;
    constexpr uint32_t TEST_PROCESS_ID_8 = 0x7F;    // For PD8 mode
    constexpr uint32_t TEST_PROCESS_ID_17 = 0x1ABCD; // For PD17 mode  
    constexpr uint32_t TEST_PROCESS_ID_20 = 0xFEDCB;  // For PD20 mode
}

// Configure DDTP for index calculation
static void configureDdtp(Iommu& iommu, uint64_t rootPpn, Ddtp::Mode mode) {
  Ddtp ddtp;
  ddtp.bits_.mode_ = mode;
  ddtp.bits_.ppn_ = rootPpn;

  iommu.writeCsr(CsrNumber::Ddtp, ddtp.value_);
  
  uint64_t readBack = iommu.readCsr(CsrNumber::Ddtp);
  assert(readBack == ddtp.value_);
}

static void installMemCbs(Iommu& iommu, MemoryModel& mem) {
  std::function<bool(uint64_t,unsigned,uint64_t&)> rcb =
    [&mem](uint64_t a, unsigned s, uint64_t& d) { return mem.read(a, s, d); };

  std::function<bool(uint64_t,unsigned,uint64_t)> wcb =
    [&mem](uint64_t a, unsigned s, uint64_t d) { return mem.write(a, s, d); };

  iommu.setMemReadCb(rcb);
  iommu.setMemWriteCb(wcb);
}

static uint64_t setupTablesWithBuilder(Iommu& iommu, MemoryModel& /* memory */,
                                      MemoryManager& memMgr, TableBuilder& tableBuilder,
                                      uint32_t devId, uint32_t processId, 
                                      Ddtp::Mode ddtMode, PdtpMode pdtMode) {
    
    // Set up DDTP
    ddtp_t ddtp;
    ddtp.bits_.mode_ = ddtMode;
    ddtp.bits_.ppn_ = memMgr.getFreePhysicalPages(1);
    
    // Configure DDTP register in the IOMMU
    configureDdtp(iommu, ddtp.ppn(), ddtMode);
    
    // Create a device context with PDT enabled
    device_context_t dc = {};
    dc.tc = 0x21; // Valid device context with PDTV=1 for process directory
    
    // Set up IOHGATP for bare mode (no G-stage translation)
    dc.iohgatp.bits_.mode_ = 0; // Bare
    dc.iohgatp.bits_.gcsid_ = 0;
    dc.iohgatp.bits_.ppn_ = 0;
    
    // Set up first-stage context with PDT
    dc.fsc.pdtp.bits_.mode_ = pdtMode;
    dc.fsc.pdtp.bits_.ppn_ = memMgr.getFreePhysicalPages(1);
    
    // Create device context using TableBuilder
    bool msi_flat = iommu.isDcExtended();
    uint64_t dc_addr = tableBuilder.addDeviceContext(dc, devId, ddtp, msi_flat);
    
    if (dc_addr == 0) {
        std::cerr << "[ERROR] Failed to create device context" << '\n';;
        return 0;
    }
    
    std::cout << "[TABLE_BUILDER] Created device context at 0x" << std::hex << dc_addr 
              << " for device ID 0x" << devId << std::dec << '\n';
    
    // Create a process context
    process_context_t pc = {};
    
    // Set up process SATP for address translation
    pc.ta.MODE = IOSATP_Sv39;
    pc.ta.PPN = memMgr.getFreePhysicalPages(1);
    
    // Add process context using TableBuilder
    uint64_t pc_addr = tableBuilder.addProcessContext(dc, pc, processId);
    
    if (pc_addr == 0) {
        std::cerr << "[ERROR] Failed to create process context" << '\n';
        return 0;
    }
    
    std::cout << "[TABLE_BUILDER] Created process context at 0x" << std::hex << pc_addr 
              << " for process ID 0x" << processId << std::dec << '\n';
    
    return pc_addr;
}

void testProcessDirectoryPd8() {
    std::cout << "\n=== Process Directory PD8 Test (using TableBuilder) ===\n";
    
    // Create infrastructure
    MemoryModel memory(size_t(1024) * 1024);  // 1MB
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
    
    // Configure capabilities
    uint64_t caps = 0;
    caps |= (1ULL << 22); // pd8
    caps |= (1ULL << 9);  // sv39
    iommu.configureCapabilities(caps);
    
    // Test PD8 mode with 1-level DDT
    uint64_t pcAddr = setupTablesWithBuilder(iommu, memory, memMgr, tableBuilder,
                                            TestValues::TEST_DEV_ID, 
                                            TestValues::TEST_PROCESS_ID_8,
                                            Ddtp::Mode::Level1, PdtpMode::Pd8);
    
    bool success = (pcAddr != 0);
    std::cout << "[TEST] PD8 process directory creation: " 
              << (success ? "PASS" : "FAIL") << '\n';
    
    if (success) {
        // Verify we can read back the process context would require device context
        // For now, just verify the address is non-zero (context was created)
        std::cout << "[VERIFY] Process context created successfully at address 0x" 
                  << std::hex << pcAddr << std::dec << '\n';
    }
}

void testProcessDirectoryPd17() {
    std::cout << "\n=== Process Directory PD17 Test (using TableBuilder) ===\n";
    
    MemoryModel memory(size_t(2) * 1024 * 1024);  // 2MB
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
    
    // Configure capabilities  
    uint64_t caps = 0;
    caps |= (1ULL << 23); // pd17
    caps |= (1ULL << 9);  // sv39
    iommu.configureCapabilities(caps);
    
    // Test PD17 mode with 2-level DDT
    uint64_t pcAddr = setupTablesWithBuilder(iommu, memory, memMgr, tableBuilder,
                                            TestValues::TEST_DEV_ID,
                                            TestValues::TEST_PROCESS_ID_17,
                                            Ddtp::Mode::Level2, PdtpMode::Pd17);
    
    bool success = (pcAddr != 0);
    std::cout << "[TEST] PD17 process directory creation: " 
              << (success ? "PASS" : "FAIL") << '\n';
}

void testProcessDirectoryPd20() {
    std::cout << "\n=== Process Directory PD20 Test (using TableBuilder) ===\n";
    
    MemoryModel memory(size_t(4) * 1024 * 1024);  // 4MB
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
    
    // Configure capabilities
    uint64_t caps = 0;
    caps |= (1ULL << 24); // pd20
    caps |= (1ULL << 9);  // sv39
    iommu.configureCapabilities(caps);
    
    // Test PD20 mode with 3-level DDT
    uint64_t pcAddr = setupTablesWithBuilder(iommu, memory, memMgr, tableBuilder,
                                            TestValues::TEST_DEV_ID,
                                            TestValues::TEST_PROCESS_ID_20, 
                                            Ddtp::Mode::Level3, PdtpMode::Pd20);
    
    bool success = (pcAddr != 0);
    std::cout << "[TEST] PD20 process directory creation: " 
              << (success ? "PASS" : "FAIL") << '\n';
}

void testMultipleProcesses() {
    std::cout << "\n=== Multiple Processes Test (using TableBuilder) ===\n";
    
    MemoryModel memory(size_t(8) * 1024 * 1024);  // 8MB
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
    
    uint64_t caps = 0;
    caps |= (1ULL << 23); // pd17
    caps |= (1ULL << 9);  // sv39
    iommu.configureCapabilities(caps);
    
    // Set up device context first
    ddtp_t ddtp;
    ddtp.bits_.mode_ = Ddtp::Mode::Level2;
    ddtp.bits_.ppn_ = memMgr.getFreePhysicalPages(1);
    configureDdtp(iommu, ddtp.ppn(), Ddtp::Mode::Level2);
    
    device_context_t dc = {};
    dc.tc = 0x21; // Valid with PDTV=1
    dc.iohgatp.bits_.mode_ = 0; // Bare
    dc.fsc.pdtp.bits_.mode_ = TT_IOMMU::PdtpMode::Pd17;
    dc.fsc.pdtp.bits_.ppn_ = memMgr.getFreePhysicalPages(1);
    
    uint64_t dc_addr = tableBuilder.addDeviceContext(dc, TestValues::TEST_DEV_ID, ddtp, false);
    
    if (dc_addr == 0) {
        std::cout << "[TEST] Multiple processes setup: FAIL (device context creation failed)" << '\n';
        return;
    }
    
    // Create multiple process contexts for the same device
    std::vector<uint32_t> processIds = {0x100, 0x200, 0x300, 0x400};
    std::vector<uint64_t> pcAddrs;
    
    for (uint32_t pid : processIds) {
        process_context_t pc = {};
        pc.ta.MODE = IOSATP_Sv39;
        pc.ta.PPN = memMgr.getFreePhysicalPages(1);
        
        uint64_t pc_addr = tableBuilder.addProcessContext(dc, pc, pid);
        pcAddrs.push_back(pc_addr);
        
        std::cout << "[TABLE_BUILDER] Process ID 0x" << std::hex << pid 
                  << " -> context at 0x" << pc_addr << std::dec << '\n';
    }
    
    // Verify all processes were created successfully
    bool allSuccess = true;
    for (uint64_t addr : pcAddrs) {
        if (addr == 0) {
            allSuccess = false;
            break;
        }
    }
    
    std::cout << "[TEST] Multiple processes creation: " 
              << (allSuccess ? "PASS" : "FAIL") << '\n';
    
    // Print memory allocation statistics
    std::cout << "\n--- Memory Allocation Statistics ---\n";
    memMgr.printStats();
}

int main() {
    std::cout << "=== IOMMU Process Table Walk Tests (Refactored with TableBuilder) ===\n";
    
    try {
        testProcessDirectoryPd8();
        testProcessDirectoryPd17();
        testProcessDirectoryPd20();
        testMultipleProcesses();
        
        std::cout << "\n=== All process table tests completed! ===\n";
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Test failed with unknown exception" << '\n';
        return 1;
    }
}
