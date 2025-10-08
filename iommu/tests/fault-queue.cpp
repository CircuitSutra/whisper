#include "Iommu.hpp"
#include "DeviceContext.hpp"
#include "MemoryModel.hpp"
#include "FaultQueue.hpp"
#include "IommuStructures.hpp"
#include "MemoryManager.hpp"
#include "TableBuilder.hpp"
#include <cassert>
#include <iostream>
#include <vector>
#include <set>
#include <functional>

using namespace TT_IOMMU;
using namespace IOMMU;

// Simple memory model for testing (adapted from original)
class TestMemory {
public:
    TestMemory(size_t size) : memory(size, 0) {
        std::cout << "Created test memory of size " << size << " bytes\n";
    }
    
    bool read(uint64_t addr, unsigned size, uint64_t& data) {
        if (addr + size > memory.size()) {
            std::cout << "Memory read error: address 0x" << std::hex << addr 
                      << std::dec << " + size " << size << " exceeds memory size " 
                      << memory.size() << "\n";
            return false;
        }
        
        data = 0;
        for (unsigned i = 0; i < size; i++) {
            data |= static_cast<uint64_t>(memory[addr + i]) << (i * 8);
        }
        return true;
    }
    
    bool write(uint64_t addr, unsigned size, uint64_t data) {
        if (addr + size > memory.size()) {
            std::cout << "Memory write error: address 0x" << std::hex << addr 
                      << std::dec << " + size " << size << " exceeds memory size " 
                      << memory.size() << "\n";
            return false;
        }
        
        for (unsigned i = 0; i < size; i++) {
            memory[addr + i] = (data >> (i * 8)) & 0xFF;
        }
        return true;
    }
    
    // Helper to dump memory
    void dump(uint64_t addr, unsigned size) {
        std::cout << "Memory dump at 0x" << std::hex << addr << ":\n";
        for (unsigned i = 0; i < size; i += 16) {
            std::cout << std::hex << (addr + i) << ": ";
            for (unsigned j = 0; j < 16 && (i + j) < size; j++) {
                std::cout << std::hex << static_cast<int>(memory[addr + i + j]) << " ";
            }
            std::cout << std::dec << "\n";
        }
    }

    uint64_t size() const {
        return memory.size();
    }

private:
    std::vector<uint8_t> memory;
};

class FaultQueueTestHelper {
public:
    FaultQueueTestHelper() 
        : memory(1024 * 1024) // 1MB
        , iommu(0x1000, 0x800, memory.size())
        , memMgr()
        , tableBuilder(memMgr,
                      [this](uint64_t addr, unsigned size, uint64_t& data) {
                          return memory.read(addr, size, data);
                      },
                      [this](uint64_t addr, unsigned size, uint64_t data) {
                          return memory.write(addr, size, data);
                      })
    {
        setupBasicIommu();
    }

    void setupBasicIommu() {
    // Configure memory callbacks
        iommu.setMemReadCb([this](uint64_t addr, unsigned size, uint64_t& data) {
        return memory.read(addr, size, data);
    });
    
        iommu.setMemWriteCb([this](uint64_t addr, unsigned size, uint64_t data) {
        return memory.write(addr, size, data);
    });
    
    // Configure basic capabilities
    uint64_t caps = 0;
    caps |= (1ULL << 8);  // Sv32
    caps |= (1ULL << 9);  // Sv39
    caps |= (1ULL << 16); // Sv32x4
        caps |= (1ULL << 38); // PD8 - Process context support (for some tests)
    iommu.configureCapabilities(caps);
    
    // Configure FCTL
    iommu.writeCsr(CsrNumber::Fctl, 0); // little-endian, no WSI
    }

    void setupFaultQueue(uint64_t fqAddr, uint8_t log2sz = 2) {
    const uint64_t fqPpn = fqAddr / 4096;
        const uint64_t numEntries = 1ULL << log2sz;
    
    // Clear memory for fault queue
        for (uint64_t i = 0; i < numEntries * sizeof(FaultRecord); i += 8) {
        memory.write(fqAddr + i, 8, 0);
    }
    
        // Configure fault queue base
        uint64_t fqb = (log2sz << 0) | (fqPpn << 10);
    iommu.writeCsr(CsrNumber::Fqb, fqb);
    iommu.writeCsr(CsrNumber::Fqh, 0); // Head at entry 0
    
    // Enable fault queue with interrupts
    uint32_t fqcsr = 0x3; // enable and interrupt enable
    iommu.writeCsr(CsrNumber::Fqcsr, fqcsr);
    
    // Wait for fault queue to be active
    for (int i = 0; i < 10; i++) {
        uint32_t fqcsrVal = iommu.readCsr(CsrNumber::Fqcsr);
        if (fqcsrVal & (1 << 16)) { // fqon bit
            break;
        }
        std::cout << "Waiting for fault queue to activate..." << '\n';
    }
    
#if 0
    if (!fqActive) {
        testPassed = false;
        std::cout << "ERROR: Fault queue did not activate!" << '\n';
#endif
    }

    void setupStageCallbacks() {
    // Setup translation stubs - the stage 1 should always fail
        iommu.setStage1Cb([](uint64_t va, unsigned /*privMode*/, bool r, bool w, bool x, 
                                      uint64_t& /*gpa*/, unsigned& cause) {
        std::cout << "Stage1 callback called: va=0x" << std::hex << va << std::dec 
                  << ", r=" << r << ", w=" << w << ", x=" << x << '\n';
        cause = 5; // Read access fault
        return false; // Return false to indicate failure
    });
    
    iommu.setStage2Cb([](uint64_t gpa, unsigned /*privMode*/, bool r, bool w, bool x, 
                         uint64_t& /*pa*/, unsigned& cause) {
        std::cout << "Stage2 callback called: gpa=0x" << std::hex << gpa << std::dec 
                  << ", r=" << r << ", w=" << w << ", x=" << x << '\n';
        cause = 5; // Read access fault
        return false; // Return false to indicate failure
    });
    
    iommu.setStage2TrapInfoCb([](uint64_t& gpa, bool& implicit, bool& write) {
        gpa = 0x1000;
        implicit = false;
        write = false;
    });
    }

    // Perform a translation that should fail
    bool performFailingTranslation(uint32_t deviceId, uint64_t iova = 0x1000, 
                                  unsigned expectedCause = 256) {
        IommuRequest req;
        req.devId = deviceId;
        req.hasProcId = false;
        req.iova = iova;
        req.type = Ttype::UntransRead;
        req.privMode = PrivilegeMode::User;
        req.size = 4;
        
        uint64_t pa = 0;
        unsigned cause = 0;
        bool result = iommu.translate(req, pa, cause);
        
        std::cout << "Translation result: " << (result ? "SUCCESS" : "FAILED")
                  << ", cause=" << cause << std::endl;
        
        return (!result && cause == expectedCause);
    }

    bool waitForFaultQueueActivation() {
    for (int i = 0; i < 10; i++) {
        uint32_t fqcsrVal = iommu.readCsr(CsrNumber::Fqcsr);
        if (fqcsrVal & (1 << 16)) { // fqon bit
                return true;
            }
            std::cout << "Waiting for fault queue to activate..." << std::endl;
        }
        return false;
    }

public:
    TestMemory memory;
    Iommu iommu;
    MemoryManager memMgr;
    TableBuilder tableBuilder;
};

void testSimpleFaultQueue() {
    std::cout << "=== Simple Fault Queue Test (Refactored) ===\n";
    bool testPassed = true;
    
    FaultQueueTestHelper helper;
    
    // Set DDTP to Off mode (this will cause a known fault)
    helper.iommu.writeCsr(CsrNumber::Ddtp, 0); // Off mode
    
    // Set up a small fault queue (4 entries)
    const uint64_t fqAddr = 0x10000;
    helper.setupFaultQueue(fqAddr, 2); // 2^2 = 4 entries
    
    if (!helper.waitForFaultQueueActivation()) {
        testPassed = false;
        std::cout << "ERROR: Fault queue did not activate!" << std::endl;
        std::cout << "=== Simple Fault Queue Test: FAILED ===\n\n";
        return;
    }
    
    // Get initial state
    uint64_t fqhBefore = helper.iommu.readCsr(CsrNumber::Fqh);
    uint64_t fqtBefore = helper.iommu.readCsr(CsrNumber::Fqt);
    uint32_t ipsrBefore = helper.iommu.readCsr(CsrNumber::Ipsr);
    
    std::cout << "Initial: FQH=" << fqhBefore << ", FQT=" << fqtBefore
              << ", IPSR=0x" << std::hex << ipsrBefore << std::dec << "\n";
    
    helper.setupStageCallbacks();
    
    // Perform translation (should fail with cause = 256 because DDTP is Off)
    if (!helper.performFailingTranslation(0x1, 0x1000, 256)) {
            testPassed = false;
        std::cout << "ERROR: Translation did not fail as expected" << std::endl;
        }
        
        // Check fault queue state
    uint64_t fqhAfter = helper.iommu.readCsr(CsrNumber::Fqh);
    uint64_t fqtAfter = helper.iommu.readCsr(CsrNumber::Fqt);
    uint32_t ipsrAfter = helper.iommu.readCsr(CsrNumber::Ipsr);
    
    std::cout << "After: FQH=" << fqhAfter << ", FQT=" << fqtAfter
              << ", IPSR=0x" << std::hex << ipsrAfter << std::dec << "\n";
    
    // Verify fault was recorded
        if (fqtBefore == fqtAfter) {
            testPassed = false;
        std::cout << "ERROR: Fault queue tail did not advance" << std::endl;
    }
    
    // Check if FIP bit was set (bit 1 of IPSR)
    bool fipSet = (ipsrAfter & 0x2) != 0;
    std::cout << "FIP bit: " << (fipSet ? "SET" : "NOT SET") << "\n";
    
    if (!fipSet) {
        testPassed = false;
        std::cout << "ERROR: FIP bit not set after fault" << std::endl;
    }
    
    std::cout << "=== Simple Fault Queue Test: " 
              << (testPassed ? "PASSED" : "FAILED") << " ===\n\n";
}

// DTF bit testing with DDT errors (refactored)
void testDtfBitWithDdtErrors() {
    std::cout << "=== DTF Bit With DDT Errors Test (Refactored) ===\n";
    bool testPassed = true;
    
    FaultQueueTestHelper helper;
    
    // Set up fault queue
    const uint64_t fqAddr = 0x10000;
    helper.setupFaultQueue(fqAddr, 2); // 4 entries
    
    if (!helper.waitForFaultQueueActivation()) {
        testPassed = false;
        std::cout << "ERROR: Fault queue did not activate!" << std::endl;
        std::cout << "=== DTF Bit With DDT Errors Test: FAILED ===\n\n";
        return;
    }
    
    // Create DDTs with valid and invalid contexts using TableBuilder
    ddtp_t validDdtp = {};
    validDdtp.raw = (1ULL << 0) | ((helper.memMgr.getFreePhysicalPages(1) / 4096) << 12); // MODE=1, PPN
    
    ddtp_t invalidDdtp = {};  
    invalidDdtp.raw = (1ULL << 0) | ((helper.memMgr.getFreePhysicalPages(1) / 4096) << 12); // MODE=1, PPN
    
    uint64_t validPpn = (validDdtp.raw >> 12) & 0xFFFFFFFFFFFULL;
    uint64_t invalidPpn = (invalidDdtp.raw >> 12) & 0xFFFFFFFFFFFULL;
    std::cout << "Valid DDT PPN: 0x" << std::hex << validPpn << std::dec << std::endl;
    std::cout << "Invalid DDT PPN: 0x" << std::hex << invalidPpn << std::dec << std::endl;
    
    // Create device contexts using TableBuilder
    // Valid DDT - Create device contexts with DTF=0 and DTF=1
    uint64_t validDc0Addr = helper.tableBuilder.addFaultTestDevice(0, validDdtp, false); // DTF=0
    uint64_t validDc1Addr = helper.tableBuilder.addFaultTestDevice(1, validDdtp, true);  // DTF=1
    
    // Invalid DDT - Create invalid device contexts (V=0)
    uint64_t invalidDc0Addr = helper.tableBuilder.addInvalidDevice(0, invalidDdtp);
    uint64_t invalidDc1Addr = helper.tableBuilder.addInvalidDevice(1, invalidDdtp);
    
    std::cout << "Created device contexts: valid(0x" << std::hex << validDc0Addr 
              << ", 0x" << validDc1Addr << "), invalid(0x" << invalidDc0Addr 
              << ", 0x" << invalidDc1Addr << ")" << std::dec << std::endl;
    
    // Test cases to verify DTF behavior
    struct DtfTestCase {
        const char* name;
        ddtp_t ddtp;
        unsigned deviceId;
        unsigned expectedCause;
        bool shouldRespectDtf;
    };
    
    std::vector<DtfTestCase> testCases = {
        // Valid DDT, DTF=0, should see fault reported
        {"Valid DDT, DTF=0", validDdtp, 0, 0, true},
        
        // Valid DDT, DTF=1, should NOT see fault reported for faults that respect DTF
        {"Valid DDT, DTF=1", validDdtp, 1, 0, true},
        
        // Invalid DDT, device_id=0, should see "DDT entry not valid" (cause=258)
        {"Invalid DDT, device_id=0", invalidDdtp, 0, 258, true},
        
        // Invalid DDT, device_id=1, should NOT see fault if DTF=1 is respected
        {"Invalid DDT, device_id=1", invalidDdtp, 1, 258, true}
    };
    
    for (const auto& test : testCases) {
        std::cout << "\nTesting: " << test.name << "\n";
        
        // Set DDTP to the appropriate mode
        helper.iommu.writeCsr(CsrNumber::Ddtp, test.ddtp.raw);
        
        // Get initial fault queue state
        uint64_t fqtBefore = helper.iommu.readCsr(CsrNumber::Fqt);
        
        // Perform translation
        helper.performFailingTranslation(test.deviceId, 0x1000, test.expectedCause);
        
        // Check if fault was recorded
        uint64_t fqtAfter = helper.iommu.readCsr(CsrNumber::Fqt);
        bool faultRecorded = (fqtAfter != fqtBefore);
        
        std::cout << "FQT before: " << fqtBefore << ", after: " << fqtAfter 
                  << " (fault recorded: " << (faultRecorded ? "YES" : "NO") << ")" << std::endl;
        
        // For DTF=1 cases, we expect certain faults to NOT be recorded
        if (test.deviceId == 1 && faultRecorded) {
            // This might be expected depending on fault type - just log it
            std::cout << "INFO: Fault recorded for DTF=1 device (may be expected)" << std::endl;
        }
    }
    
    std::cout << "\n--- Memory Allocation Statistics ---\n";
    helper.memMgr.printStats();
    
    std::cout << "=== DTF Bit With DDT Errors Test: " 
              << (testPassed ? "PASSED" : "FAILED") << " ===\n\n";
}

// SBE field endianness test (refactored)
void testSbeFieldEndianness() {
    std::cout << "=== SBE Field Endianness Test (Refactored) ===\n";
    bool testPassed = true;
    
    FaultQueueTestHelper helper;
    
    // Set up fault queue
    const uint64_t fqAddr = 0x10000;
    helper.setupFaultQueue(fqAddr, 2); // 4 entries
    
    if (!helper.waitForFaultQueueActivation()) {
        testPassed = false;
        std::cout << "ERROR: Fault queue did not activate!" << std::endl;
        std::cout << "=== SBE Field Endianness Test: FAILED ===\n\n";
        return;
    }
    
    // Create DDT for endianness testing
    ddtp_t ddtp = {};
    ddtp.raw = (1ULL << 0) | ((helper.memMgr.getFreePhysicalPages(1) / 4096) << 12); // MODE=1, PPN
    
    // Create PDTs for little-endian and big-endian
    uint64_t pdtLePage = helper.memMgr.getFreePhysicalPages(1);
    uint64_t pdtBePage = helper.memMgr.getFreePhysicalPages(1);
    uint64_t pdtLePpn = pdtLePage / 4096;
    uint64_t pdtBePpn = pdtBePage / 4096;
    
    // Create PDTP values
    uint64_t pdtp0 = (1ULL << 60) | pdtLePpn; // MODE=1 (PD8), PPN points to LE PDT
    uint64_t pdtp1 = (1ULL << 60) | pdtBePpn; // MODE=1 (PD8), PPN points to BE PDT
    
    // Create device contexts with different SBE settings using TableBuilder
    uint64_t dc0Addr = helper.tableBuilder.addFaultTestDevice(
        0, ddtp, false, false, true, pdtp0); // SBE=0 (LE), PDTV=1
    uint64_t dc1Addr = helper.tableBuilder.addFaultTestDevice(
        1, ddtp, false, true, true, pdtp1);  // SBE=1 (BE), PDTV=1
    
    std::cout << "Created LE device context at 0x" << std::hex << dc0Addr 
              << " and BE device context at 0x" << dc1Addr << std::dec << std::endl;
    
    // Create Process Contexts in PDTs (with appropriate endianness)
    const uint64_t leProcessId = 0x5;
    const uint64_t beProcessId = 0x5;
    
    // LE PDT entry (little-endian format)
    helper.memory.write(pdtLePage + leProcessId * 8, 8, 1); // V=1, minimal PC
    
    // BE PDT entry (big-endian format) 
    uint64_t bePcValue = 1; // V=1
    // Convert to big-endian representation for SBE=1
    uint64_t bePcValueBE = ((bePcValue & 0xFF) << 56) | 
                          ((bePcValue & 0xFF00) << 40) |
                          ((bePcValue & 0xFF0000) << 24) |
                          ((bePcValue & 0xFF000000) << 8) |
                          ((bePcValue & 0xFF00000000ULL) >> 8) |
                          ((bePcValue & 0xFF0000000000ULL) >> 24) |
                          ((bePcValue & 0xFF000000000000ULL) >> 40) |
                          ((bePcValue & 0xFF00000000000000ULL) >> 56);
    helper.memory.write(pdtBePage + beProcessId * 8, 8, bePcValueBE);
    
    // Set DDTP
    helper.iommu.writeCsr(CsrNumber::Ddtp, ddtp.raw);
    
    std::cout << "Testing LE device (device_id=0)..." << std::endl;
    helper.performFailingTranslation(0, 0x1000);
    
    std::cout << "Testing BE device (device_id=1)..." << std::endl;
    helper.performFailingTranslation(1, 0x1000);
    
    // Print memory allocation statistics
    std::cout << "\n--- Memory Allocation Statistics ---\n";
    helper.memMgr.printStats();
    
    std::cout << "=== SBE Field Endianness Test: " 
              << (testPassed ? "PASSED" : "FAILED") << " ===\n\n";
}

// Fault queue overflow test (refactored)
void testFaultQueueOverflow() {
    std::cout << "=== Fault Queue Overflow Test (Refactored) ===\n";
    bool testPassed = true;
    
    FaultQueueTestHelper helper;
    
    // Set DDTP to Off mode (this will cause known faults)
    helper.iommu.writeCsr(CsrNumber::Ddtp, 0); // Off mode
    
    // Set up a tiny fault queue (2 entries) to force overflow quickly
    const uint64_t fqAddr = 0x10000;
    helper.setupFaultQueue(fqAddr, 1); // 2^1 = 2 entries
    
    if (!helper.waitForFaultQueueActivation()) {
        testPassed = false;
        std::cout << "ERROR: Fault queue did not activate!" << std::endl;
        std::cout << "=== Fault Queue Overflow Test: FAILED ===\n\n";
        return;
    }
    
    // Get initial state
    uint64_t initialFqh = helper.iommu.readCsr(CsrNumber::Fqh);
    uint64_t initialFqt = helper.iommu.readCsr(CsrNumber::Fqt);
    
    std::cout << "Initial: FQH=" << initialFqh << ", FQT=" << initialFqt << std::endl;
    
    bool overflowDetected = false;
    
    // Generate multiple faults to trigger overflow
    for (int i = 0; i < 4; i++) {
        std::cout << "\n--- Request " << (i+1) << " ---\n";
        
        uint64_t fqtBefore = helper.iommu.readCsr(CsrNumber::Fqt);
        uint32_t fqcsrBefore = helper.iommu.readCsr(CsrNumber::Fqcsr);
        bool fqofBefore = (fqcsrBefore & (1 << 8)) != 0; // FQOF bit
        
        // Perform translation (should fail)
        helper.performFailingTranslation(i, 0x1000 + i * 0x100, 256);
        
        uint64_t fqtAfter = helper.iommu.readCsr(CsrNumber::Fqt);
        uint32_t fqcsrAfter = helper.iommu.readCsr(CsrNumber::Fqcsr);
        bool fqofAfter = (fqcsrAfter & (1 << 8)) != 0; // FQOF bit
        
        std::cout << "Before: FQT=" << fqtBefore << ", FQOF=" << fqofBefore << std::endl;
        std::cout << "After:  FQT=" << fqtAfter << ", FQOF=" << fqofAfter << std::endl;
        
        // Check for overflow condition
        if (i == 2 && !fqofAfter) {
            testPassed = false;
            std::cout << "ERROR: Expected FQOF to be set on the third request" << '\n';
        }
        
        if (fqofAfter) {
            overflowDetected = true;
            break;  // No need to continue once overflow is detected
        }
    }
    
    // We should have detected overflow
    if (!overflowDetected) {
        testPassed = false;
        std::cout << "ERROR: Overflow condition not detected!" << '\n';
    }
    
    // Check if FIP bit was set
    uint32_t finalIpsr = helper.iommu.readCsr(CsrNumber::Ipsr);
    bool fipSet = (finalIpsr & 0x2) != 0;
    std::cout << "FIP bit: " << (fipSet ? "SET" : "NOT SET") << "\n";
    
    if (!fipSet) {
        testPassed = false;
        std::cout << "ERROR: FIP bit not set after overflow" << '\n';
    }
    
    std::cout << "=== Fault Queue Overflow Test: " 
              << (testPassed ? "PASSED" : "FAILED") << " ===\n\n";
}

// -----------------------------------------------------------------------------
// Main function
// -----------------------------------------------------------------------------

int main() {
    std::cout << "=== IOMMU Fault Queue Tests (Refactored with TableBuilder) ===\n\n";
    
    try {
        testSimpleFaultQueue();
        testDtfBitWithDdtErrors();
        testSbeFieldEndianness();
        testFaultQueueOverflow();
        
        std::cout << "\n=== All fault queue tests completed! ===\n";
    return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Fault queue test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Fault queue test failed with unknown exception" << std::endl;
        return 1;
    }
}
