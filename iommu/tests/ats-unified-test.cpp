// Copyright 2024 Tenstorrent Corporation.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "Iommu.hpp"
#include "DeviceContext.hpp"
#include "ProcessContext.hpp"
#include "Ats.hpp"
#include "MemoryModel.hpp"
#include "IommuStructures.hpp"
#include "MemoryManager.hpp"
#include "TableBuilder.hpp"
#include <iostream>
#include <cassert>
#include <cstring>
#include <map>
#include <memory>

using namespace TT_IOMMU;
using namespace IOMMU;

class AtsTestHelper
{
public:
  AtsTestHelper() : mem_(256 * 1024 * 1024), memMgr_(), // 256MB memory
                    readFunc_([this](uint64_t addr, unsigned size, uint64_t& data) {
                        return mem_.read(addr, size, data);
                    }),
                    writeFunc_([this](uint64_t addr, unsigned size, uint64_t data) {
                        return mem_.write(addr, size, data);
                    }),
                    tableBuilder_(memMgr_, readFunc_, writeFunc_)
  {
    setupIommu();
  }

  Iommu& getIommu() { return *iommu_; }
  MemoryModel& getMemory() { return mem_; }
  TableBuilder& getTableBuilder() { return tableBuilder_; }
  MemoryManager& getMemoryManager() { return memMgr_; }

  // Create a device context using TableBuilder
  void setupDeviceContextWithBuilder(uint32_t devId, bool enableAts = true, bool enableT2gpa = false)
  {
    std::cout << "[ATS_HELPER] Setting up device context for ID 0x" << std::hex << devId 
              << " with ATS=" << (enableAts ? "enabled" : "disabled") << std::dec << std::endl;
    
    // Set up DDTP for 2-level DDT
    ddtp_t ddtp;
    ddtp.iommu_mode = DDT_2LVL;
    ddtp.ppn = memMgr_.getFreePhysicalPages(1);
    
    // Configure DDTP register in the IOMMU
    Ddtp iommuDdtp;
    iommuDdtp.bits_.mode_ = Ddtp::Mode::Level2;
    iommuDdtp.bits_.ppn_ = ddtp.ppn;
    iommu_->writeCsr(CsrNumber::Ddtp, iommuDdtp.value_);
    
    // Create device context with ATS configuration
    device_context_t dc = {};
    dc.tc = 0x1; // Valid device context
    
    // Configure ATS
    if (enableAts) {
        dc.tc |= 0x2; // Enable ATS bit
    }
    
    // Configure T2GPA  
    if (enableT2gpa) {
        dc.tc |= 0x8; // Enable T2GPA bit
        
        // Set up IOHGATP for G-stage translation
        dc.iohgatp.MODE = IOHGATP_Sv39x4;
        dc.iohgatp.GSCID = 0;
        dc.iohgatp.PPN = memMgr_.getFreePhysicalPages(1);
    } else {
        // Bare mode - no G-stage translation
        dc.iohgatp.MODE = IOHGATP_Bare;
        dc.iohgatp.GSCID = 0;
        dc.iohgatp.PPN = 0;
    }
    
    // Set up first-stage context - direct IOSATP mode (PDTV=0)
    dc.fsc.pdtp.MODE = PD_OFF; // Not used when PDTV=0
    dc.fsc.pdtp.PPN = 0;
    
    // Set up S-stage translation
    dc.fsc.iosatp.MODE = IOSATP_Sv39;
    dc.fsc.iosatp.PPN = memMgr_.getFreePhysicalPages(1);
    
    // Use TableBuilder to create the device context
    bool msi_flat = iommu_->isDcExtended();
    uint64_t dc_addr = tableBuilder_.addDeviceContext(dc, devId, ddtp, msi_flat);
    
    if (dc_addr == 0) {
        std::cerr << "[ATS_HELPER] Failed to create device context!" << std::endl;
        return;
    }
    
    // Store the device context address for later use
    deviceContextAddrs_[devId] = dc_addr;
    
    std::cout << "[ATS_HELPER] Device context created at address 0x" << std::hex << dc_addr << std::dec << std::endl;
  }

  // Create an ATS request
  IommuRequest createAtsRequest(uint32_t devId, uint64_t iova, Ttype type = Ttype::PcieAts)
  {
    IommuRequest req;
    req.devId = devId;
    req.type = type;
    req.iova = iova;
    req.size = 4;
    req.privMode = PrivilegeMode::User;
    req.hasProcId = false;
    return req;
  }

  // Setup command queue for command testing
  void setupCommandQueue()
  {
    uint64_t cqbAddr = 0x1000000;
    
    // Configure CQB using Qbase
    Qbase cqb{0};
    cqb.bits_.ppn_ = cqbAddr >> 12;
    cqb.bits_.logszm1_ = 11; // 4KB
    iommu_->writeCsr(CsrNumber::Cqb, cqb.value_);
    
    iommu_->writeCsr(CsrNumber::Cqh, 0);
    iommu_->writeCsr(CsrNumber::Cqt, 0);
    
    uint32_t cqcsrValue = 1; // Enable
    iommu_->writeCsr(CsrNumber::Cqcsr, cqcsrValue);
    
    std::cout << "[ATS_HELPER] Command queue configured at PPN 0x" << std::hex << (cqbAddr >> 12) << std::dec << std::endl;
  }

  // Check if device context exists
  bool hasDeviceContext(uint32_t devId) const {
    return deviceContextAddrs_.find(devId) != deviceContextAddrs_.end();
  }

  // Get device context address
  uint64_t getDeviceContextAddr(uint32_t devId) const {
    auto it = deviceContextAddrs_.find(devId);
    return (it != deviceContextAddrs_.end()) ? it->second : 0;
  }

private:
  MemoryModel mem_;
  MemoryManager memMgr_;
  std::function<bool(uint64_t,unsigned,uint64_t&)> readFunc_;
  std::function<bool(uint64_t,unsigned,uint64_t)> writeFunc_;
  TableBuilder tableBuilder_;
  std::unique_ptr<Iommu> iommu_;
  std::map<uint32_t, uint64_t> deviceContextAddrs_; // devId -> context address

  void setupIommu()
  {
    iommu_ = std::make_unique<Iommu>(0x1000, 0x800, mem_.size());
    
    // Install memory callbacks
    iommu_->setMemReadCb(readFunc_);
    iommu_->setMemWriteCb(writeFunc_);
    
    // Configure capabilities
    uint64_t caps = 0;
    caps |= (1ULL << 0);  // version 1.0
    caps |= (1ULL << 8);  // Sv32
    caps |= (1ULL << 9);  // Sv39
    caps |= (1ULL << 10); // Sv48  
    caps |= (1ULL << 16); // Sv32x4
    caps |= (1ULL << 17); // Sv39x4
    caps |= (1ULL << 18); // Sv48x4
    caps |= (1ULL << 19); // ATS
    caps |= (1ULL << 20); // T2GPA
    caps |= (1ULL << 22); // PD8
    caps |= (1ULL << 23); // PD17
    caps |= (1ULL << 24); // PD20
    
    iommu_->configureCapabilities(caps);
    
    // Configure FCTL for little-endian operation
    iommu_->writeCsr(CsrNumber::Fctl, 0);
    
    std::cout << "[ATS_HELPER] IOMMU configured with capabilities 0x" << std::hex << caps << std::dec << std::endl;
  }
};

void testBasicAtsTranslation() {
    std::cout << "\n=== Basic ATS Translation Test (using TableBuilder) ===\n";
  
  AtsTestHelper helper;
    
    // Test device ID
    constexpr uint32_t devId = 0x123;
    
    // Set up device context with ATS enabled
    helper.setupDeviceContextWithBuilder(devId, true, false);
    
    // Verify device context was created
    bool contextExists = helper.hasDeviceContext(devId);
    std::cout << "[TEST] Device context creation: " << (contextExists ? "PASS" : "FAIL") << std::endl;
    
    if (!contextExists) {
        return;
    }
    
    // Create ATS translation request
    IommuRequest atsReq = helper.createAtsRequest(devId, 0x1000);
    
    // Process the ATS request
    auto& iommu = helper.getIommu();
    Iommu::AtsResponse resp;
  unsigned cause = 0;
  
    bool success = iommu.atsTranslate(atsReq, resp, cause);
    std::cout << "[TEST] ATS translation request: " << (success ? "PASS" : "FAIL") << std::endl;
    
    if (success && resp.success) {
        std::cout << "[RESULT] ATS translation: IOVA 0x" << std::hex << atsReq.iova 
                  << " -> PA 0x" << resp.translatedAddr << std::dec << std::endl;
    }
}

void testAtsWithT2gpa() {
    std::cout << "\n=== ATS with T2GPA Test (using TableBuilder) ===\n";
  
  AtsTestHelper helper;
  
    constexpr uint32_t devId = 0x456;
    
    // Set up device context with both ATS and T2GPA enabled
    helper.setupDeviceContextWithBuilder(devId, true, true);
    
    bool contextExists = helper.hasDeviceContext(devId);
    std::cout << "[TEST] Device context with T2GPA creation: " << (contextExists ? "PASS" : "FAIL") << std::endl;
    
    if (!contextExists) {
        return;
    }
    
    // Create ATS request for a higher address that will go through G-stage translation
    IommuRequest atsReq = helper.createAtsRequest(devId, 0x10000000);
    
    auto& iommu = helper.getIommu();
    Iommu::AtsResponse resp;
  unsigned cause = 0;
  
    bool success = iommu.atsTranslate(atsReq, resp, cause);
    std::cout << "[TEST] ATS with T2GPA request: " << (success ? "PASS" : "FAIL") << std::endl;
    
    if (success && resp.success) {
        std::cout << "[RESULT] ATS+T2GPA translation: IOVA 0x" << std::hex << atsReq.iova 
                  << " -> PA 0x" << resp.translatedAddr << std::dec << std::endl;
    }
}

void testMultipleDevicesAts() {
    std::cout << "\n=== Multiple Devices ATS Test (using TableBuilder) ===\n";
  
  AtsTestHelper helper;
  
    // Set up multiple devices with different configurations
    std::vector<std::pair<uint32_t, bool>> devices = {
        {0x100, true},   // ATS enabled
        {0x200, false},  // ATS disabled  
        {0x300, true},   // ATS enabled
        {0x400, true}    // ATS enabled
    };
    
    // Create device contexts for all devices
    for (auto [devId, atsEnabled] : devices) {
        helper.setupDeviceContextWithBuilder(devId, atsEnabled, false);
        
        bool contextExists = helper.hasDeviceContext(devId);
        std::cout << "[SETUP] Device 0x" << std::hex << devId << std::dec 
                  << " context: " << (contextExists ? "PASS" : "FAIL") << std::endl;
    }
    
    // Test ATS requests for ATS-enabled devices
    auto& iommu = helper.getIommu();
    int successCount = 0;
    int totalAtsDevices = 0;
    
    for (auto [devId, atsEnabled] : devices) {
        if (!atsEnabled) continue; // Skip non-ATS devices
        
        totalAtsDevices++;
        IommuRequest atsReq = helper.createAtsRequest(devId, 0x2000 + (devId << 12));
        Iommu::AtsResponse resp;
        unsigned cause = 0;
        
        bool success = iommu.atsTranslate(atsReq, resp, cause);
        if (success && resp.success) {
            successCount++;
            std::cout << "[ATS] Device 0x" << std::hex << devId << ": IOVA 0x" << atsReq.iova 
                      << " -> PA 0x" << resp.translatedAddr << std::dec << std::endl;
        }
    }
    
    std::cout << "[TEST] Multiple devices ATS: " << successCount << "/" << totalAtsDevices 
              << " successful (" << (successCount == totalAtsDevices ? "PASS" : "FAIL") << ")" << std::endl;
}

void testAtsCommandQueue() {
    std::cout << "\n=== ATS Command Queue Test (using TableBuilder) ===\n";
  
  AtsTestHelper helper;
  
    // Set up command queue
    helper.setupCommandQueue();
    
    constexpr uint32_t devId = 0x789;
    
    // Set up device context with ATS
    helper.setupDeviceContextWithBuilder(devId, true, false);
    
    bool contextExists = helper.hasDeviceContext(devId);
    std::cout << "[TEST] ATS command queue setup: " << (contextExists ? "PASS" : "FAIL") << std::endl;
    
    // For a more comprehensive test, we would issue ATS invalidation commands
    // through the command queue, but that requires more complex setup
    
    auto& iommu = helper.getIommu();
    
    // Check that command queue is enabled
    uint64_t cqcsr = iommu.readCsr(CsrNumber::Cqcsr);
    bool cqEnabled = (cqcsr & 0x1) != 0;
    
    std::cout << "[TEST] Command queue enabled: " << (cqEnabled ? "PASS" : "FAIL") << std::endl;
}

void testTableBuilderStats() {
    std::cout << "\n=== TableBuilder Memory Statistics ===\n";
  
  AtsTestHelper helper;
    
    // Set up several devices to show memory allocation
    for (uint32_t devId = 0x1000; devId <= 0x1005; devId++) {
        helper.setupDeviceContextWithBuilder(devId, true, devId % 2 == 0); // Alternate T2GPA
    }
    
    // Print allocation statistics
    helper.getMemoryManager().printStats();
}

// -----------------------------------------------------------------------------
// Main function
// -----------------------------------------------------------------------------

int main() {
    std::cout << "=== IOMMU ATS Unified Tests (Refactored with TableBuilder) ===\n";
  
  try {
    testBasicAtsTranslation();
        testAtsWithT2gpa();
        testMultipleDevicesAts();
        testAtsCommandQueue();
        testTableBuilderStats();
        
        std::cout << "\n=== All ATS tests completed! ===\n";
        return 0;
    
  } catch (const std::exception& e) {
        std::cerr << "ATS test failed with exception: " << e.what() << std::endl;
    return 1;
  } catch (...) {
        std::cerr << "ATS test failed with unknown exception" << std::endl;
    return 1;
  }
} 
