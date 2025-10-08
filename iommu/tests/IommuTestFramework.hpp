#pragma once

#include "Iommu.hpp"
#include "MemoryModel.hpp"
#include "IommuStructures.hpp"
#include "MemoryManager.hpp"
#include "TableBuilder.hpp"
#include <functional>
#include <memory>
#include <iostream>

using namespace TT_IOMMU;
using namespace IOMMU;

// -----------------------------------------------------------------------------
// Unified IOMMU Test Framework
// -----------------------------------------------------------------------------

/// Capability preset configurations for common test scenarios
enum class CapabilityPreset {
    Basic,          // Basic translation capabilities
    ATS,            // ATS-enabled configuration  
    MSI,            // MSI translation capabilities
    Full,           // All capabilities enabled
    Custom          // User-defined capabilities
};

/// Test memory sizes for different scenarios
enum class TestMemorySize {
    Small = 1024 * 1024,      // 1MB
    Medium = 4 * 1024 * 1024, // 4MB  
    Large = 16 * 1024 * 1024, // 16MB
    XLarge = 64 * 1024 * 1024 // 64MB
};

/// Unified IOMMU test helper that combines all common functionality
class IommuTestFramework {
public:
    /// Constructor with preset configuration
    IommuTestFramework(CapabilityPreset preset = CapabilityPreset::Basic,
                      TestMemorySize memSize = TestMemorySize::Medium,
                      uint64_t iommuAddr = 0x1000,
                      uint64_t iommuSize = 0x800)
        : memory_(static_cast<size_t>(memSize)), memMgr_(),
          readFunc_([this](uint64_t addr, unsigned size, uint64_t& data) {
              return memory_.read(addr, size, data);
          }),
          writeFunc_([this](uint64_t addr, unsigned size, uint64_t data) {
              return memory_.write(addr, size, data);
          }),
          tableBuilder_(memMgr_, readFunc_, writeFunc_) {
        
        // Configure capabilities based on preset
        uint64_t caps = getCapabilitiesForPreset(preset);
        
        // Create IOMMU with configured capabilities
        iommu_ = std::make_unique<Iommu>(iommuAddr, iommuSize, memory_.size(), caps);
        
        // Install standard memory callbacks
        installStandardCallbacks();
        
        std::cout << "[FRAMEWORK] IOMMU test framework initialized with " 
                  << getPresetName(preset) << " capabilities" << std::endl;
    }
    
    /// Constructor with custom capabilities
    IommuTestFramework(uint64_t customCaps,
                      TestMemorySize memSize = TestMemorySize::Medium,
                      uint64_t iommuAddr = 0x1000,
                      uint64_t iommuSize = 0x800)
        : memory_(static_cast<size_t>(memSize)), memMgr_(),
          readFunc_([this](uint64_t addr, unsigned size, uint64_t& data) {
              return memory_.read(addr, size, data);
          }),
          writeFunc_([this](uint64_t addr, unsigned size, uint64_t data) {
              return memory_.write(addr, size, data);
          }),
          tableBuilder_(memMgr_, readFunc_, writeFunc_) {
        
        iommu_ = std::make_unique<Iommu>(iommuAddr, iommuSize, memory_.size(), customCaps);
        installStandardCallbacks();
        
        std::cout << "[FRAMEWORK] IOMMU test framework initialized with custom capabilities 0x" 
                  << std::hex << customCaps << std::dec << std::endl;
    }
    
    // Accessors
    Iommu& getIommu() { return *iommu_; }
    MemoryModel& getMemory() { return memory_; }
    MemoryManager& getMemoryManager() { return memMgr_; }
    TableBuilder& getTableBuilder() { return tableBuilder_; }
    
    // -----------------------------------------------------------------------------
    // High-level test setup functions
    // -----------------------------------------------------------------------------
    
    /// Setup a basic device with translation enabled
    uint64_t setupBasicDevice(uint32_t deviceId, 
                             DDTMode ddtMode = DDT_2LVL,
                             bool enableAts = false,
                             bool enableT2gpa = false) {
        
        ddtp_t ddtp;
        ddtp.iommu_mode = ddtMode;
        ddtp.ppn = memMgr_.getFreePhysicalPages(1);
        
        // Configure DDTP register
        Ddtp iommuDdtp;
        iommuDdtp.bits_.mode_ = convertDdtMode(ddtMode);
        iommuDdtp.bits_.ppn_ = ddtp.ppn;
        iommu_->writeCsr(CsrNumber::Ddtp, iommuDdtp.value_);
        
        // Create device context
        device_context_t dc = {};
        dc.tc = 0x1; // Valid
        if (enableAts) dc.tc |= 0x2;     // ATS
        if (enableT2gpa) dc.tc |= 0x8;   // T2GPA
        
        // Configure G-stage translation
        if (enableT2gpa) {
            dc.iohgatp.MODE = IOHGATP_Sv39x4;
            dc.iohgatp.GSCID = 0;
            dc.iohgatp.PPN = memMgr_.getFreePhysicalPages(1);
        } else {
            dc.iohgatp.MODE = IOHGATP_Bare;
            dc.iohgatp.GSCID = 0;
            dc.iohgatp.PPN = 0;
        }
        
        // Configure S-stage translation
        dc.fsc.iosatp.MODE = IOSATP_Sv39;
        dc.fsc.iosatp.PPN = memMgr_.getFreePhysicalPages(1);
        
        bool msi_flat = iommu_->isDcExtended();
        uint64_t dc_addr = tableBuilder_.addDeviceContext(dc, deviceId, ddtp, msi_flat);
        
        std::cout << "[FRAMEWORK] Setup basic device 0x" << std::hex << deviceId 
                  << " at address 0x" << dc_addr << std::dec << std::endl;
        
        return dc_addr;
    }
    
    /// Setup MSI-enabled device  
    uint64_t setupMsiDevice(uint32_t deviceId,
                           uint64_t msiAddrMask = 0xFFFFF000ULL,
                           uint64_t msiAddrPattern = 0xFEDC1000ULL,
                           uint64_t msiTargetPpn = 0x500) {
        
        ddtp_t ddtp;
        ddtp.iommu_mode = DDT_1LVL;
        ddtp.ppn = memMgr_.getFreePhysicalPages(1);
        
        Ddtp iommuDdtp;
        iommuDdtp.bits_.mode_ = Ddtp::Mode::Level1;
        iommuDdtp.bits_.ppn_ = ddtp.ppn;
        iommu_->writeCsr(CsrNumber::Ddtp, iommuDdtp.value_);
        
        // MSI-enabled device context
        device_context_t dc = {};
        dc.tc = 0x7; // Valid + ATS + T2GPA
        dc.iohgatp.MODE = IOHGATP_Sv39x4;
        dc.iohgatp.PPN = memMgr_.getFreePhysicalPages(1);
        dc.fsc.iosatp.MODE = IOSATP_Bare;
        
        // MSI configuration
        uint64_t msi_ppn = memMgr_.getFreePhysicalPages(1);
        uint64_t msiptp = (3ULL << 60) | msi_ppn; // Flat mode
        
        bool msi_flat = iommu_->isDcExtended();
        uint64_t dc_addr = tableBuilder_.addMsiDeviceContext(dc, deviceId, ddtp, msi_flat,
                                                           msiAddrMask, msiAddrPattern, msiptp);
        
        if (dc_addr != 0) {
            tableBuilder_.setupMsiPageTable(msi_ppn, msiTargetPpn);
        }
        
        std::cout << "[FRAMEWORK] Setup MSI device 0x" << std::hex << deviceId 
                  << " at address 0x" << dc_addr << std::dec << std::endl;
        
        return dc_addr;
    }
    
    /// Setup command queue with reasonable defaults
    bool setupCommandQueue(uint64_t queueAddr = 0x1000000,
                          unsigned logSize = 6) { // 64 entries
        
        Qbase cqb{0};
        cqb.bits_.ppn_ = queueAddr >> 12;
        cqb.bits_.logszm1_ = logSize;
        iommu_->writeCsr(CsrNumber::Cqb, cqb.value_);
        
        iommu_->writeCsr(CsrNumber::Cqh, 0);
        iommu_->writeCsr(CsrNumber::Cqt, 0);
        
        uint32_t cqcsrValue = 1; // Enable
        iommu_->writeCsr(CsrNumber::Cqcsr, cqcsrValue);
        
        std::cout << "[FRAMEWORK] Command queue setup at 0x" << std::hex << queueAddr 
                  << " with " << std::dec << (1 << (logSize + 1)) << " entries" << std::endl;
        
        return true;
    }
    
    /// Setup fault queue with reasonable defaults  
    bool setupFaultQueue(uint64_t queueAddr = 0x2000000,
                        unsigned logSize = 4) { // 16 entries
        
        uint64_t fqPpn = queueAddr >> 12;
        uint64_t fqb = (logSize << 0) | (fqPpn << 10);
        iommu_->writeCsr(CsrNumber::Fqb, fqb);
        iommu_->writeCsr(CsrNumber::Fqh, 0);
        
        uint32_t fqcsr = 0x3; // Enable + interrupt enable
        iommu_->writeCsr(CsrNumber::Fqcsr, fqcsr);
        
        std::cout << "[FRAMEWORK] Fault queue setup at 0x" << std::hex << queueAddr 
                  << " with " << std::dec << (1 << (logSize + 1)) << " entries" << std::endl;
        
        return true;
    }
    
    /// Print memory allocation statistics
    void printMemoryStats() {
        std::cout << "\n--- Framework Memory Statistics ---" << std::endl;
        memMgr_.printStats();
    }
    
    /// Verify queue state (useful for testing)
    bool verifyCommandQueueState(uint64_t expectedHead, uint64_t expectedTail) {
        uint64_t actualHead = iommu_->readCsr(CsrNumber::Cqh);
        uint64_t actualTail = iommu_->readCsr(CsrNumber::Cqt);
        
        bool headCorrect = (actualHead == expectedHead);
        bool tailCorrect = (actualTail == expectedTail);
        
        std::cout << "[VERIFY] Command queue: Head=" << actualHead 
                  << (headCorrect ? " ✓" : " ✗") << ", Tail=" << actualTail 
                  << (tailCorrect ? " ✓" : " ✗") << std::endl;
        
        return headCorrect && tailCorrect;
    }

private:
    std::unique_ptr<Iommu> iommu_;
    MemoryModel memory_;
    MemoryManager memMgr_;
    std::function<bool(uint64_t,unsigned,uint64_t&)> readFunc_;
    std::function<bool(uint64_t,unsigned,uint64_t)> writeFunc_;
    TableBuilder tableBuilder_;
    
    uint64_t getCapabilitiesForPreset(CapabilityPreset preset) {
        Capabilities caps;
        caps.value_ = 0;
        
        switch (preset) {
            case CapabilityPreset::Basic:
                caps.bits_.sv39_ = 1;
                caps.bits_.sv39x4_ = 1;
                caps.bits_.pd8_ = 1;
                caps.bits_.pd17_ = 1;
                break;
                
            case CapabilityPreset::ATS:
                caps.bits_.sv39_ = 1;
                caps.bits_.sv39x4_ = 1;
                caps.bits_.pd8_ = 1;
                caps.bits_.pd17_ = 1;
                caps.bits_.ats_ = 1;
                caps.bits_.t2gpa_ = 1;
                break;
                
            case CapabilityPreset::MSI:
                caps.bits_.sv39_ = 1;
                caps.bits_.sv39x4_ = 1;
                caps.bits_.pd8_ = 1;
                caps.bits_.pd17_ = 1;
                caps.bits_.ats_ = 1;
                caps.bits_.t2gpa_ = 1;
                caps.bits_.msiFlat_ = 1;
                caps.bits_.msiMrif_ = 1;
                break;
                
            case CapabilityPreset::Full:
                caps.bits_.pd8_ = 1;
                caps.bits_.pd17_ = 1;
                caps.bits_.pd20_ = 1;
                caps.bits_.sv32_ = 1;
                caps.bits_.sv39_ = 1;
                caps.bits_.sv48_ = 1;
                caps.bits_.sv57_ = 1;
                caps.bits_.sv32x4_ = 1;
                caps.bits_.sv39x4_ = 1;
                caps.bits_.sv48x4_ = 1;
                caps.bits_.sv57x4_ = 1;
                caps.bits_.ats_ = 1;
                caps.bits_.t2gpa_ = 1;
                caps.bits_.msiFlat_ = 1;
                caps.bits_.msiMrif_ = 1;
                caps.bits_.amoHwad_ = 1;
                caps.bits_.end_ = 1;
                break;
                
            case CapabilityPreset::Custom:
                // Will be set by caller
                break;
        }
        
        return caps.value_;
    }
    
    const char* getPresetName(CapabilityPreset preset) {
        switch (preset) {
            case CapabilityPreset::Basic: return "Basic";
            case CapabilityPreset::ATS: return "ATS";
            case CapabilityPreset::MSI: return "MSI";
            case CapabilityPreset::Full: return "Full";
            case CapabilityPreset::Custom: return "Custom";
        }
        return "Unknown";
    }
    
    Ddtp::Mode convertDdtMode(DDTMode mode) {
        switch (mode) {
            case DDT_3LVL: return Ddtp::Mode::Level3;
            case DDT_2LVL: return Ddtp::Mode::Level2;  
            case DDT_1LVL: return Ddtp::Mode::Level1;
            default: return Ddtp::Mode::Off;
        }
    }
    
    void installStandardCallbacks() {
        iommu_->setMemReadCb(readFunc_);
        iommu_->setMemWriteCb(writeFunc_);
        
        // Identity translation callbacks
        iommu_->setStage1Cb([](uint64_t va, unsigned, bool, bool, bool, uint64_t& gpa, unsigned& cause) {
            gpa = va;
            cause = 0;
            return true;
        });
        
        iommu_->setStage2Cb([](uint64_t gpa, unsigned, bool, bool, bool, uint64_t& pa, unsigned& cause) {
            pa = gpa;
            cause = 0;
            return true;
        });
        
        iommu_->setStage2TrapInfoCb([](uint64_t&, bool&, bool&) {});
        iommu_->setStage1ConfigCb([](unsigned, unsigned, uint64_t, bool) {});
        iommu_->setStage2ConfigCb([](unsigned, unsigned, uint64_t) {});
    }
};

