#include "Iommu.hpp"
#include "DeviceContext.hpp"
#include "MemoryModel.hpp"
#include <cassert>
#include <iostream>
#include <vector>
#include <functional>

using namespace TT_IOMMU;

// -----------------------------------------------------------------------------
// Constants for common test values
// -----------------------------------------------------------------------------
namespace TestValues {
    // Common PPN values
    constexpr uint64_t ROOT_PPN = 0x100;
    constexpr uint64_t LEAF_PPN = 0x200;
    constexpr uint64_t IOSATP_PPN = 0x500;
    constexpr uint64_t IOHGATP_PPN = 0x300;
    constexpr uint64_t PDTP_PPN = 0x400;
    constexpr uint64_t MSI_PPN = 0x600;
    
    // Common test device IDs
    constexpr uint32_t SIMPLE_DEV_ID = 0x2A;
    constexpr uint32_t TWO_LEVEL_DEV_ID = 0x1FFF;
    constexpr uint32_t THREE_LEVEL_DEV_ID = 0xABCDEF;
    
    // MSI values
    constexpr uint64_t MSI_ADDR_MASK = 0xFFFFF000ULL;
    constexpr uint64_t MSI_ADDR_PATTERN = 0xFEDC1000ULL;
}

// -----------------------------------------------------------------------------
// Helper functions
// -----------------------------------------------------------------------------

static void installMemCbs(TT_IOMMU::Iommu& iommu, MemoryModel& mem) {
    std::function<bool(uint64_t,unsigned,uint64_t&)> rcb =
        [&mem](uint64_t a, unsigned s, uint64_t& d) { return mem.read(a, s, d); };
    std::function<bool(uint64_t,unsigned,uint64_t)> wcb =
        [&mem](uint64_t a, unsigned s, uint64_t d) { return mem.write(a, s, d); };

    iommu.setMemReadCb(rcb);
    iommu.setMemWriteCb(wcb);
}

static void configureCapabilities(TT_IOMMU::Iommu& iommu) {
    Capabilities caps;

    // Set all required capabilities
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
    caps.bits_.amoHwad_ = 1;
    caps.bits_.msiFlat_ = 1;  // For extended format tests
    caps.bits_.end_ = 1;      // Support for endianness control

    iommu.configureCapabilities(caps.value_);
}

// Configure FCTL register - critical for SXL tests
static void configureFctl(TT_IOMMU::Iommu& iommu, bool gxl = false, bool be = false, bool wsi = false) {
    uint32_t fctlVal = 0;
    if (gxl) fctlVal |= (1 << 2);  // GXL bit
    if (be) fctlVal |= (1 << 0);   // BE bit
    if (wsi) fctlVal |= (1 << 1);  // WSI bit
    
    iommu.writeCsr(CsrNumber::Fctl, fctlVal);
    
    // Verify
    uint64_t readback = iommu.readCsr(CsrNumber::Fctl);
    std::cout << "[CONFIG] FCTL configured: GXL=" << (gxl ? "1" : "0")
              << ", BE=" << (be ? "1" : "0")
              << ", WSI=" << (wsi ? "1" : "0")
              << ", readback=0x" << std::hex << readback << std::dec << "\n";
}

/// Setup a device table for the given mode and device ID
/// Returns the address of the leaf device context entry
static uint64_t setupDeviceTable(Iommu& iommu, uint32_t devId, 
                              uint64_t rootPpn, Ddtp::Mode mode) {
    // Configure DDTP register with the root PPN and mode
    Ddtp ddtp;
    ddtp.bits_.mode_ = mode;
    ddtp.bits_.ppn_ = rootPpn;
    iommu.writeCsr(CsrNumber::Ddtp, ddtp.value_);
    
    bool extended = iommu.isDcExtended();
    uint64_t pageSize = iommu.pageSize();
    
    // Setup directory levels based on mode
    unsigned levels = 0;
    if (mode == Ddtp::Mode::Level3) levels = 3;
    else if (mode == Ddtp::Mode::Level2) levels = 2;
    else if (mode == Ddtp::Mode::Level1) levels = 1;
    
    Devid dId{devId};
    uint64_t currentPpn = rootPpn;
    
    // Setup non-leaf levels
    assert(levels > 0);
    for (unsigned level = levels - 1; level > 0; level--) {
        unsigned ddi = dId.ithDdi(level, extended);
        
        // Create a non-leaf entry
        Ddte ddte(0); // Initialize with 0
        ddte.bits_.v_ = 1;
        uint64_t nextPpn = TestValues::LEAF_PPN + (levels - level);
        ddte.bits_.ppn_ = nextPpn;
        
        // Write the entry using IOMMU
        uint64_t entryAddr = currentPpn * pageSize + ddi * uint64_t(8);
        iommu.writeDevDirTableEntry(entryAddr, ddte.value_);
        
        // Move to next level
        currentPpn = nextPpn;
    }
    
    // Return the address of the leaf entry
    uint64_t ddi0 = dId.ithDdi(0, extended);
    unsigned leafSize = Iommu::devDirTableLeafSize(extended);
    return currentPpn * pageSize + ddi0 * leafSize;
}

// Creates a device context with the specified configuration
static DeviceContext createDeviceContext(
    bool valid = true, 
    bool enable_ats = false,
    bool enable_pri = false,
    bool t2gpa = false,
    bool dtf = false,
    bool pdtv = false,
    bool prpr = false,
    bool gade = false,
    bool sade = false,
    bool dpe = false,
    bool sbe = false,
    bool sxl = false,
    IohgatpMode iohgatp_mode = IohgatpMode::Bare,
    uint16_t gscid = 0,
    uint64_t iohgatp_ppn = 0,
    uint32_t pscid = 0,
    IosatpMode iosatp_mode = IosatpMode::Bare,
    uint64_t iosatp_ppn = 0,
    PdtpMode pdtp_mode = PdtpMode::Bare,
    uint64_t pdtp_ppn = 0,
    MsiptpMode msi_mode = MsiptpMode::Off,
    uint64_t msi_ppn = 0,
    uint64_t msi_addr_mask = 0,
    uint64_t msi_addr_pattern = 0)
{
    // Create the Translation Control field
    TransControl tc;
    tc.bits_.v_ = valid ? 1 : 0;
    tc.bits_.ats_ = enable_ats ? 1 : 0;
    tc.bits_.pri_ = enable_pri ? 1 : 0;
    tc.bits_.t2gpa_ = t2gpa ? 1 : 0;
    tc.bits_.dtf_ = dtf ? 1 : 0;
    tc.bits_.pdtv_ = pdtv ? 1 : 0;
    tc.bits_.prpr_ = prpr ? 1 : 0;
    tc.bits_.gade_ = gade ? 1 : 0;
    tc.bits_.sade_ = sade ? 1 : 0;
    tc.bits_.dpe_ = dpe ? 1 : 0;
    tc.bits_.sbe_ = sbe ? 1 : 0;
    tc.bits_.sxl_ = sxl ? 1 : 0;
    
    // Create IOHGATP field
    Iohgatp iohgatp;
    iohgatp.bits_.mode_ = iohgatp_mode;
    iohgatp.bits_.gcsid_ = gscid;
    iohgatp.bits_.ppn_ = iohgatp_ppn;
    uint64_t iohgatpVal = iohgatp.value_;
    
    // Create TA field with PSCID
    uint64_t ta = pscid << 12; // PSCID is in bits 12-31
    
    // Create FSC field based on PDTV
    Fsc fsc;
    if (pdtv) {
      // If PDTV is set, FSC holds PDTP
      fsc.bits_.ppn_ = pdtp_ppn; fsc.bits_.mode_ = uint32_t(pdtp_mode);
    } else {
      // Otherwise, FSC holds IOSATP
      fsc.bits_.ppn_ = iosatp_ppn; fsc.bits_.mode_ = uint32_t(iosatp_mode);
    }
    
    // Create the base DeviceContext
    DeviceContext dc(tc.value_, iohgatpVal, ta, fsc.value_);
    
    // If MSI fields are needed, create extended DeviceContext
    if (msi_mode != MsiptpMode::Off || msi_ppn != 0 || 
        msi_addr_mask != 0 || msi_addr_pattern != 0) {
        uint64_t msiptp = (uint64_t(msi_mode) << 60) | msi_ppn;
        dc = DeviceContext(tc.value_, iohgatpVal, ta, fsc.value_,
                          msiptp, msi_addr_mask, msi_addr_pattern);
    }
    
    return dc;
}

// Test a device context configuration
static bool testDeviceContextConfig(
    Iommu& iommu, uint32_t devId, 
    Ddtp::Mode mode, const char* testName,
    const DeviceContext& dc, bool expectedOk, unsigned expectedCause) 
{
    // Setup device table and get address of the leaf entry
    uint64_t dcAddr = setupDeviceTable(iommu, devId, TestValues::ROOT_PPN, mode);
    
    // Write the device context to memory using IOMMU
    iommu.writeDeviceContext(dcAddr, dc);
    
    // Test loadDeviceContext
    unsigned cause = 0;
    DeviceContext loadedDc;
    bool ok = iommu.loadDeviceContext(devId, loadedDc, cause);
    
    std::cout << "[TEST:" << testName << "] Result: ok=" << ok << " cause=" << cause 
              << " (expected ok=" << expectedOk << " cause=" << expectedCause << ")" << '\n';
    
    if (ok == expectedOk && cause == expectedCause) {
        std::cout << "  ✓ " << testName << " passed\n";
        return true;
    }
    std::cout << "  ✗ " << testName << " failed\n";
    return false;
}

// -----------------------------------------------------------------------------
// Test cases for single-level DDT
// -----------------------------------------------------------------------------
static void testDdtWalkSingleLevel() {
    using namespace TestValues;
    
    MemoryModel mem(size_t(4) * 1024 * 1024);
    Iommu iommu(0x1000, 0x800, mem.size());
    configureCapabilities(iommu);
    installMemCbs(iommu, mem);
    
    // Test base format (32-byte device context)
    std::cout << "\n[TEST] Testing base format (32-byte) device contexts\n";
    
    // 1. Basic valid configuration
    {
        configureFctl(iommu, false); // GXL=0, BE=0, WSI=0
        
        // Create a basic valid device context
        DeviceContext dc = createDeviceContext(true); // valid=true
        
        testDeviceContextConfig(
            iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1, 
            "basic_valid_config", dc, true, 0
        );
    }
    
    // 2. Test with SXL=1 (translating 32-bit addresses)
    {
        // For SXL=1 to be valid, GXL must be 1 or GXL must be writable
        configureFctl(iommu, true); // Set GXL=1
        
        // Create device context with SXL=1 and Sv32 mode
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            true,   // sxl=true
            IohgatpMode::Bare,
            0,      // gscid
            0,      // iohgatp_ppn
            0,      // pscid
            IosatpMode::Sv32,
            IOSATP_PPN
        );
        
        testDeviceContextConfig(
            iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
            "sxl_enabled_sv32", dc, true, 0
        );
    }
    
    // 3. Test with PDTV=1 (Process Directory Table Valid)
    {
        // Create device context with PDTV=1 and PD17 mode
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            true,   // pdtv=true
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            true,   // sxl - must be true since GXL=1
            IohgatpMode::Bare,
            0,      // gscid
            0,      // iohgatp_ppn
            0,      // pscid
            IosatpMode::Bare,
            0,      // iosatp_ppn
            PdtpMode::Pd17,
            PDTP_PPN
        );
        
        testDeviceContextConfig(
            iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
            "pdtv_enabled", dc, true, 0
        );
    }
    
    // 4. Test with second-stage translation (Sv39x4)
    {
        // Reset GXL to 0 for Sv39x4 test
        configureFctl(iommu, false); // GXL=0
        
        // Create device context with second-stage translation
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl - must be false since GXL=0
            IohgatpMode::Sv39x4,
            0x5678, // gscid
            IOHGATP_PPN
        );
        
        testDeviceContextConfig(
            iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
            "second_stage_sv39x4", dc, true, 0
        );
    }
    
    // 5. Test invalid configuration (T2GPA=1 but IOHGATP.MODE=Bare)
    {
        // Create device context with T2GPA=1 and EN_ATS=1 but IOHGATP.MODE=Bare
        DeviceContext dc = createDeviceContext(
            true,   // valid
            true,   // enable_ats=true
            false,  // enable_pri
            true,   // t2gpa=true
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false   // sxl
            // All other fields default to 0/Bare
        );
        
        testDeviceContextConfig(
            iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
            "invalid_t2gpa_bare", dc, false, 259   // Should fail with DDT entry misconfigured
        );
    }
    
    // 6. Test endianness configurations - Enable big-endian mode
    {
        // Configure capabilities with END=1
        Capabilities caps;
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
        caps.bits_.amoHwad_ = 1;
        caps.bits_.msiFlat_ = 1;
        caps.bits_.end_ = 1;  // Support for endianness control
        iommu.configureCapabilities(caps.value_);
        
        // Set big-endian mode in FCTL
        configureFctl(iommu, false, true); // GXL=0, BE=1
        
        // Create device context with SBE=1 (matching BE=1)
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            true,   // sbe=true
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0       // iohgatp_ppn
        );
        
        testDeviceContextConfig(
            iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
            "big_endian_config", dc, true, 0
        );
    }
    
    // 7. Test invalid endianness configuration (mismatched BE and SBE)
    {
        // Set FCTL.BE=1
        configureFctl(iommu, false, true); // GXL=0, BE=1
        
        // Create device context with SBE=0 (mismatched with BE=1 when END=0)
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe=false (mismatched)
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0       // iohgatp_ppn
        );
        
        // Temporarily disable END capability to test mismatch
        Capabilities caps = iommu.readCsr(CsrNumber::Capabilities);
        Capabilities newCaps(caps);
        newCaps.bits_.end_ = 0;  // Disable endianness support
        iommu.configureCapabilities(newCaps.value_);
        
        testDeviceContextConfig(
            iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
            "mismatched_endianness", dc, false, 259   // Should fail with DDT entry misconfigured
        );
        
        // Restore original capabilities
        iommu.configureCapabilities(caps.value_);
    }
    
    // 8. Test with disable-translation-fault (DTF) flag
    {
        configureFctl(iommu, false, false); // GXL=0, BE=0
        
        // Create device context with DTF=1
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            true,   // dtf=true
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0       // iohgatp_ppn
        );
        
        testDeviceContextConfig(
            iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
            "dtf_enabled", dc, true, 0
        );
    }
    
    // 9. Test with A/D Bit Updates enabled
    {
        // Configure capabilities with AMO_HWAD=1
        Capabilities caps;
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
        caps.bits_.amoHwad_ = 1;  // A/D bit updates support
        caps.bits_.msiFlat_ = 1;
        caps.bits_.end_ = 1;
        iommu.configureCapabilities(caps.value_);
        
        // Create device context with GADE=1 and SADE=1
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            true,   // gade=true
            true,   // sade=true
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Sv39x4,
            0x5678, // gscid
            IOHGATP_PPN,
            0,      // pscid
            IosatpMode::Sv39,
            IOSATP_PPN
        );
        
        testDeviceContextConfig(
            iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
            "ad_bit_updates_enabled", dc, true, 0
        );
    }
    
    // 10. Test with invalid A/D bit update configuration (AMO_HWAD not supported)
    {
        // Temporarily disable AMO_HWAD capability
        Capabilities caps = iommu.readCsr(CsrNumber::Capabilities);
        Capabilities newCaps(caps);
        newCaps.bits_.amoHwad_ = 0;  // Disable A/D bit updates support
        iommu.configureCapabilities(newCaps.value_);
        
        // Create device context with GADE=1 and SADE=1 but AMO_HWAD=0
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            true,   // gade=true (invalid without AMO_HWAD)
            true,   // sade=true (invalid without AMO_HWAD)
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Sv39x4,
            0x5678, // gscid
            IOHGATP_PPN
        );
        
        testDeviceContextConfig(
            iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
            "invalid_ad_bit_config", dc, false, 259   // Should fail with DDT entry misconfigured
        );
        
        // Restore original capabilities
        iommu.configureCapabilities(caps.value_);
    }
    
    // 11. Test with DPE (Default Process Enable) flag
    {
        configureFctl(iommu, false); // Reset FCTL
        
        // Create device context with PDTV=1 and DPE=1
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            true,   // pdtv=true
            false,  // prpr
            false,  // gade
            false,  // sade
            true,   // dpe=true
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0,      // iohgatp_ppn
            0,      // pscid
            IosatpMode::Bare,
            0,      // iosatp_ppn
            PdtpMode::Pd17,
            PDTP_PPN
        );
        
        testDeviceContextConfig(
            iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
            "dpe_with_pdtv", dc, true, 0
        );
    }
    
    // 12. Test invalid configuration (DPE=1 but PDTV=0)
    {
        // Create device context with PDTV=0 and DPE=1 (invalid)
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv=false
            false,  // prpr
            false,  // gade
            false,  // sade
            true,   // dpe=true (invalid without PDTV)
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0       // iohgatp_ppn
        );
        
        testDeviceContextConfig(
            iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
            "invalid_dpe_without_pdtv", dc, false, 259   // Should fail with DDT entry misconfigured
        );
    }
    
    // 13. Test with ATS capability enabled
    {
        // Configure capabilities with ATS support
        Capabilities caps;
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
        caps.bits_.amoHwad_ = 1;
        caps.bits_.msiFlat_ = 1;
        caps.bits_.end_ = 1;
        caps.bits_.ats_ = 1;  // Enable ATS capability
        iommu.configureCapabilities(caps.value_);
        
        // Create device context with ATS enabled
        DeviceContext dc = createDeviceContext(
            true,   // valid
            true,   // enable_ats=true
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0       // iohgatp_ppn
        );
        
        testDeviceContextConfig(
            iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
            "ats_enabled", dc, true, 0
        );
    }
    
    // 14. Test ATS with PRI enabled
    {
        // Create device context with ATS and PRI enabled
        DeviceContext dc = createDeviceContext(
            true,   // valid
            true,   // enable_ats=true
            true,   // enable_pri=true
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            true,   // prpr=true
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0       // iohgatp_ppn
        );
        
        testDeviceContextConfig(
            iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
            "ats_pri_enabled", dc, true, 0
        );
    }
    
    // 15. Test invalid configuration (PRI enabled but ATS disabled)
    {
        // Create device context with PRI enabled but ATS disabled (invalid)
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats=false
            true,   // enable_pri=true (invalid without ATS)
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0       // iohgatp_ppn
        );
        
        testDeviceContextConfig(
            iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
            "invalid_pri_without_ats", dc, false, 259   // Should fail with DDT entry misconfigured
        );
    }
    
    // Only run MSI tests if extended format is supported
    if (iommu.isDcExtended()) {
        std::cout << "\n[TEST] Testing extended format (64-byte) device contexts\n";
        
        // 16. Basic extended format test
        {
            DeviceContext dc = createDeviceContext();
            
            testDeviceContextConfig(
                iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
                "basic_extended_format", dc, true, 0
            );
        }
        
        // 17. Extended format with MSI flat translation enabled
        {
            DeviceContext dc = createDeviceContext(
                true,   // valid
                false,  // enable_ats
                false,  // enable_pri
                false,  // t2gpa
                false,  // dtf
                false,  // pdtv
                false,  // prpr
                false,  // gade
                false,  // sade
                false,  // dpe
                false,  // sbe
                false,  // sxl
                IohgatpMode::Bare,
                0,      // gscid
                0,      // iohgatp_ppn
                0,      // pscid
                IosatpMode::Bare,
                0,      // iosatp_ppn
                PdtpMode::Bare,
                0,      // pdtp_ppn
                MsiptpMode::Flat,
                MSI_PPN,
                TestValues::MSI_ADDR_MASK,
                TestValues::MSI_ADDR_PATTERN
            );
            
            testDeviceContextConfig(
                iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
                "msi_flat_enabled", dc, true, 0
            );
        }
        
        // 18. Test invalid MSI mode
        {
            // Create device context with invalid MSI mode (3)
            DeviceContext dc = createDeviceContext(
                true,   // valid
                false,  // enable_ats
                false,  // enable_pri
                false,  // t2gpa
                false,  // dtf
                false,  // pdtv
                false,  // prpr
                false,  // gade
                false,  // sade
                false,  // dpe
                false,  // sbe
                false,  // sxl
                IohgatpMode::Bare,
                0,      // gscid
                0,      // iohgatp_ppn
                0,      // pscid
                IosatpMode::Bare,
                0,      // iosatp_ppn
                PdtpMode::Bare,
                0,      // pdtp_ppn
                static_cast<MsiptpMode>(3),  // Invalid mode
                MSI_PPN,
                TestValues::MSI_ADDR_MASK,
                TestValues::MSI_ADDR_PATTERN
            );
            
            testDeviceContextConfig(
                iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
                "invalid_msi_mode", dc, false, 259   // Should fail with DDT entry misconfigured
            );
        }
        
        // 19. Combined test: MSI + first-stage + second-stage
        {
            // Create device context with all features enabled
            DeviceContext dc = createDeviceContext(
                true,    // valid
                true,    // enable_ats
                true,    // enable_pri
                false,   // t2gpa
                false,   // dtf
                true,    // pdtv
                true,    // prpr
                true,    // gade
                true,    // sade
                true,    // dpe
                false,   // sbe
                false,   // sxl
                IohgatpMode::Sv39x4,
                0x5678,  // gscid
                IOHGATP_PPN,
                0x1234,  // pscid
                IosatpMode::Bare,
                0,       // iosatp_ppn
                PdtpMode::Pd17,
                PDTP_PPN,
                MsiptpMode::Flat,
                MSI_PPN,
                TestValues::MSI_ADDR_MASK,
                TestValues::MSI_ADDR_PATTERN
            );
            
            testDeviceContextConfig(
                iommu, SIMPLE_DEV_ID, Ddtp::Mode::Level1,
                "full_features_enabled", dc, true, 0
            );
        }
    }

    std::cout << "ddt_walk_single_level tests completed\n";
}

// -----------------------------------------------------------------------------
// Test cases for two-level DDT
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// Test cases for two-level DDT
// -----------------------------------------------------------------------------
static void testDdtWalkTwoLevel() {
    using namespace TestValues;
    
    MemoryModel mem(size_t(4) * 1024 * 1024);
    Iommu iommu(0x1000, 0x800, mem.size());
    configureCapabilities(iommu);
    installMemCbs(iommu, mem);
    
    // Test base format (32-byte device context) with two-level DDT
    std::cout << "\n[TEST] Testing base format (32-byte) device contexts with two-level DDT\n";
    
    // 1. Basic valid configuration
    {
        configureFctl(iommu, false); // GXL=0, BE=0, WSI=0
        
        // Create a basic valid device context
        DeviceContext dc = createDeviceContext(true); // valid=true
        
        testDeviceContextConfig(
            iommu, TWO_LEVEL_DEV_ID, Ddtp::Mode::Level2, 
            "two_level_basic_config", dc, true, 0
        );
    }
    
    // 2. Test with SXL=1 and Sv32 (for two-level DDT)
    {
        // For SXL=1 to be valid, GXL must be 1
        configureFctl(iommu, true); // Set GXL=1
        
        // Create device context with SXL=1 and Sv32 mode
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            true,   // sxl=true
            IohgatpMode::Bare,
            0,      // gscid
            0,      // iohgatp_ppn
            0,      // pscid
            IosatpMode::Sv32,
            IOSATP_PPN
        );
        
        testDeviceContextConfig(
            iommu, TWO_LEVEL_DEV_ID, Ddtp::Mode::Level2,
            "two_level_sxl_sv32", dc, true, 0
        );
    }
    
    // 3. Test with PDTV=1 and PDT processing for two-level DDT
    {
        // Create device context with PDTV=1 and PD17 mode
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            true,   // pdtv=true
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            true,   // sxl - must be true since GXL=1
            IohgatpMode::Bare,
            0,      // gscid
            0,      // iohgatp_ppn
            0,      // pscid
            IosatpMode::Bare,
            0,      // iosatp_ppn
            PdtpMode::Pd17,
            PDTP_PPN
        );
        
        testDeviceContextConfig(
            iommu, TWO_LEVEL_DEV_ID, Ddtp::Mode::Level2,
            "two_level_pdtv_enabled", dc, true, 0
        );
    }
    
    // 4. Test with second-stage translation (Sv32x4) for two-level DDT
    {
        // Keep GXL=1 for this test
        
        // Create device context with second-stage translation
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            true,   // sxl - must be true since GXL=1
            IohgatpMode::Sv32x4,
            0x5678, // gscid
            IOHGATP_PPN
        );
        
        testDeviceContextConfig(
            iommu, TWO_LEVEL_DEV_ID, Ddtp::Mode::Level2,
            "two_level_second_stage_sv32x4", dc, true, 0
        );
    }
    
    // 5. Enable ATS for two-level DDT with Capabilities support
    {
        // Configure capabilities with ATS support
        Capabilities caps;
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
        caps.bits_.amoHwad_ = 1;
        caps.bits_.msiFlat_ = 1;
        caps.bits_.end_ = 1;
        caps.bits_.ats_ = 1;  // Enable ATS capability
        caps.bits_.t2gpa_ = 1; // Enable T2GPA capability
        iommu.configureCapabilities(caps.value_);
        
        // Reset GXL to 0 for this test
        configureFctl(iommu, false); // GXL=0
        
        // Create device context with ATS enabled
        DeviceContext dc = createDeviceContext(
            true,   // valid
            true,   // enable_ats=true
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false   // sxl
        );
        
        testDeviceContextConfig(
            iommu, TWO_LEVEL_DEV_ID, Ddtp::Mode::Level2,
            "two_level_ats_enabled", dc, true, 0
        );
    }
    
    // 6. NEW TEST: T2GPA with second-stage translation
    {
        // Create device context with T2GPA=1, ATS=1, and second-stage translation
        DeviceContext dc = createDeviceContext(
            true,   // valid
            true,   // enable_ats=true
            false,  // enable_pri
            true,   // t2gpa=true
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Sv39x4, // Must have non-Bare second stage
            0x1234, // gscid
            IOHGATP_PPN
        );
        
        testDeviceContextConfig(
            iommu, TWO_LEVEL_DEV_ID, Ddtp::Mode::Level2,
            "two_level_t2gpa_enabled", dc, true, 0
        );
    }
    
    // 7. NEW TEST: Little endian configuration (SBE=0)
    {
        // Configure capabilities with END=1
        configureFctl(iommu, false, false); // GXL=0, BE=0 (little endian)
        
        // Create device context with SBE=0 (matching BE=0)
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe=false (little endian)
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0       // iohgatp_ppn
        );
        
        testDeviceContextConfig(
            iommu, TWO_LEVEL_DEV_ID, Ddtp::Mode::Level2,
            "two_level_little_endian", dc, true, 0
        );
    }
    
    // 8. NEW TEST: Test PD8 mode (8-bit process ID)
    {
        // Create device context with PDTV=1 and PD8 mode
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            true,   // pdtv=true
            false,  // prpr
            false,  // gade
            false,  // sade
            true,   // dpe=true
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0,      // iohgatp_ppn
            0,      // pscid
            IosatpMode::Bare,
            0,      // iosatp_ppn
            PdtpMode::Pd8,  // Use PD8 mode (smallest PDT)
            PDTP_PPN
        );
        
        testDeviceContextConfig(
            iommu, TWO_LEVEL_DEV_ID, Ddtp::Mode::Level2,
            "two_level_pd8_mode", dc, true, 0
        );
    }
    
    // 9. NEW TEST: Test PD20 mode (20-bit process ID, largest PDT)
    {
        // Create device context with PDTV=1 and PD20 mode
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            true,   // pdtv=true
            false,  // prpr
            false,  // gade
            false,  // sade
            true,   // dpe=true
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0,      // iohgatp_ppn
            0,      // pscid
            IosatpMode::Bare,
            0,      // iosatp_ppn
            PdtpMode::Pd20, // Use PD20 mode (largest PDT)
            PDTP_PPN
        );
        
        testDeviceContextConfig(
            iommu, TWO_LEVEL_DEV_ID, Ddtp::Mode::Level2,
            "two_level_pd20_mode", dc, true, 0
        );
    }
    
    // 10. NEW TEST: Test with Sv48 first-stage translation
    {
        // Reset to GXL=0
        configureFctl(iommu, false); // GXL=0
        
        // Create device context with Sv48 first-stage
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0,      // iohgatp_ppn
            0x1234, // pscid
            IosatpMode::Sv48, // Use Sv48 mode
            IOSATP_PPN
        );
        
        testDeviceContextConfig(
            iommu, TWO_LEVEL_DEV_ID, Ddtp::Mode::Level2,
            "two_level_sv48_first_stage", dc, true, 0
        );
    }
    
    // 11. NEW TEST: Test with Sv57 first-stage translation
    {
        // Create device context with Sv57 first-stage
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0,      // iohgatp_ppn
            0x1234, // pscid
            IosatpMode::Sv57, // Use Sv57 mode (largest first-stage)
            IOSATP_PPN
        );
        
        testDeviceContextConfig(
            iommu, TWO_LEVEL_DEV_ID, Ddtp::Mode::Level2,
            "two_level_sv57_first_stage", dc, true, 0
        );
    }
    
    // 12. NEW TEST: Test with Sv48x4 second-stage translation
    {
        // Create device context with Sv48x4 second-stage
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Sv48x4, // Use Sv48x4 mode
            0x5678, // gscid
            IOHGATP_PPN
        );
        
        testDeviceContextConfig(
            iommu, TWO_LEVEL_DEV_ID, Ddtp::Mode::Level2,
            "two_level_sv48x4_second_stage", dc, true, 0
        );
    }
    
    // 13. NEW TEST: Test with Sv57x4 second-stage translation
    {
        // Create device context with Sv57x4 second-stage
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Sv57x4, // Use Sv57x4 mode (largest second-stage)
            0x5678, // gscid
            IOHGATP_PPN
        );
        
        testDeviceContextConfig(
            iommu, TWO_LEVEL_DEV_ID, Ddtp::Mode::Level2,
            "two_level_sv57x4_second_stage", dc, true, 0
        );
    }
    
    // Only run MSI tests if extended format is supported
    if (iommu.isDcExtended()) {
        std::cout << "\n[TEST] Testing extended format (64-byte) device contexts with two-level DDT\n";
        
        // 14. Extended format with MSI flat translation enabled for two-level DDT
        {
            DeviceContext dc = createDeviceContext(
                true,   // valid
                false,  // enable_ats
                false,  // enable_pri
                false,  // t2gpa
                false,  // dtf
                false,  // pdtv
                false,  // prpr
                false,  // gade
                false,  // sade
                false,  // dpe
                false,  // sbe
                false,  // sxl
                IohgatpMode::Bare,
                0,      // gscid
                0,      // iohgatp_ppn
                0,      // pscid
                IosatpMode::Bare,
                0,      // iosatp_ppn
                PdtpMode::Bare,
                0,      // pdtp_ppn
                MsiptpMode::Flat,
                MSI_PPN,
                TestValues::MSI_ADDR_MASK,
                TestValues::MSI_ADDR_PATTERN
            );
            
            testDeviceContextConfig(
                iommu, TWO_LEVEL_DEV_ID, Ddtp::Mode::Level2,
                "two_level_msi_flat", dc, true, 0
            );
        }
        
        // 15. NEW TEST: MSI with first and second stage
        {
            DeviceContext dc = createDeviceContext(
                true,    // valid
                false,   // enable_ats
                false,   // enable_pri
                false,   // t2gpa
                false,   // dtf
                false,   // pdtv
                false,   // prpr
                false,   // gade 
                false,   // sade
                false,   // dpe
                false,   // sbe
                false,   // sxl
                IohgatpMode::Sv39x4,
                0x1234,  // gscid
                IOHGATP_PPN,
                0x5678,  // pscid
                IosatpMode::Sv39,
                IOSATP_PPN,
                PdtpMode::Bare,
                0,       // pdtp_ppn
                MsiptpMode::Flat,
                MSI_PPN,
                TestValues::MSI_ADDR_MASK,
                TestValues::MSI_ADDR_PATTERN
            );
            
            testDeviceContextConfig(
                iommu, TWO_LEVEL_DEV_ID, Ddtp::Mode::Level2,
                "two_level_msi_with_stages", dc, true, 0
            );
        }
    }

    std::cout << "ddt_walk_two_level tests completed\n";
}

// -----------------------------------------------------------------------------
// Test cases for three-level DDT
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// Test cases for three-level DDT
// -----------------------------------------------------------------------------
static void testDdtWalkThreeLevel() {
    using namespace TestValues;
    MemoryModel mem(size_t(4) * 1024 * 1024);
    Iommu iommu(0x1000,0x800, mem.size());
    configureCapabilities(iommu);
    installMemCbs(iommu, mem);

    std::cout << "\n[TEST] three-level base format\n";
    // 1. Simple valid
    {
        configureFctl(iommu, false); // GXL=0
        DeviceContext dc = createDeviceContext(true); // valid=true
        testDeviceContextConfig(iommu, THREE_LEVEL_DEV_ID,
                               Ddtp::Mode::Level3,
                               "three_level_basic", dc, true, 0);
    }
    
    // 2. SXL + Sv32
    {
        configureFctl(iommu, true); // GXL=1
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            true,   // sxl=true
            IohgatpMode::Bare,
            0,      // gscid
            0,      // iohgatp_ppn
            0,      // pscid
            IosatpMode::Sv32,
            IOSATP_PPN
        );
        testDeviceContextConfig(iommu, THREE_LEVEL_DEV_ID,
                               Ddtp::Mode::Level3,
                               "three_level_sxl_sv32", dc, true, 0);
    }
    
    // 3. PDTV + PD17
    {
        // Keep GXL=1 from previous test
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            true,   // pdtv=true
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            true,   // sxl - must be true since GXL=1
            IohgatpMode::Bare,
            0,      // gscid
            0,      // iohgatp_ppn
            0,      // pscid
            IosatpMode::Bare,
            0,      // iosatp_ppn
            PdtpMode::Pd17,
            PDTP_PPN
        );
        testDeviceContextConfig(iommu, THREE_LEVEL_DEV_ID,
                               Ddtp::Mode::Level3,
                               "three_level_pdtv", dc, true, 0);
    }
    
    // 4. Second-stage translation (Sv39x4)
    {
        // Reset to GXL=0 for Sv39x4 test
        configureFctl(iommu, false); // GXL=0
        
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl - must be false since GXL=0
            IohgatpMode::Sv39x4,
            0x5678, // gscid
            IOHGATP_PPN
        );
        testDeviceContextConfig(iommu, THREE_LEVEL_DEV_ID,
                               Ddtp::Mode::Level3,
                               "three_level_second_stage", dc, true, 0);
    }
    
    // 5. Both stages (Sv39x4 + Sv39)
    {
        // Keep GXL=0 for this test
        
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl - must be false since GXL=0
            IohgatpMode::Sv39x4,
            0x5678, // gscid
            IOHGATP_PPN,
            1234,   // pscid
            IosatpMode::Sv39,
            IOSATP_PPN
        );
        testDeviceContextConfig(iommu, THREE_LEVEL_DEV_ID,
                               Ddtp::Mode::Level3,
                               "three_level_both_stages", dc, true, 0);
    }
    
    // 6. NEW TEST: Little endian configuration (SBE=0)
    {
        // Configure little endian
        configureFctl(iommu, false, false); // GXL=0, BE=0 (little endian)
        
        // Create device context with SBE=0 (matching BE=0)
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe=false (little endian)
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0       // iohgatp_ppn
        );
        
        testDeviceContextConfig(
            iommu, THREE_LEVEL_DEV_ID, Ddtp::Mode::Level3,
            "three_level_little_endian", dc, true, 0
        );
    }
    
    // 7. NEW TEST: Big endian configuration (SBE=1)
    {
        // Configure capabilities with END=1
        Capabilities caps;
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
        caps.bits_.amoHwad_ = 1;
        caps.bits_.msiFlat_ = 1;
        caps.bits_.end_ = 1;  // Support for endianness control
        iommu.configureCapabilities(caps.value_);
        
        // Set big-endian mode in FCTL
        configureFctl(iommu, false, true); // GXL=0, BE=1
        
        // Create device context with SBE=1 (matching BE=1)
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            true,   // sbe=true (big endian)
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0       // iohgatp_ppn
        );
        
        testDeviceContextConfig(
            iommu, THREE_LEVEL_DEV_ID, Ddtp::Mode::Level3,
            "three_level_big_endian", dc, true, 0
        );
    }
    
    // 8. NEW TEST: Test with DTF flag (Disable Translation Fault)
    {
        configureFctl(iommu, false, false); // Reset to GXL=0, BE=0
        
        // Create device context with DTF=1
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            true,   // dtf=true
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0       // iohgatp_ppn
        );
        
        testDeviceContextConfig(
            iommu, THREE_LEVEL_DEV_ID, Ddtp::Mode::Level3,
            "three_level_dtf_enabled", dc, true, 0
        );
    }
    
    // 9. NEW TEST: Test with Sv48 first-stage translation
    {
        // Create device context with Sv48 first-stage
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0,      // iohgatp_ppn
            0x1234, // pscid
            IosatpMode::Sv48, // Use Sv48 mode
            IOSATP_PPN
        );
        
        testDeviceContextConfig(
            iommu, THREE_LEVEL_DEV_ID, Ddtp::Mode::Level3,
            "three_level_sv48_first_stage", dc, true, 0
        );
    }
    
    // 10. NEW TEST: Test with Sv57 first-stage translation
    {
        // Create device context with Sv57 first-stage
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0,      // iohgatp_ppn
            0x1234, // pscid
            IosatpMode::Sv57, // Use Sv57 mode (largest first-stage)
            IOSATP_PPN
        );
        
        testDeviceContextConfig(
            iommu, THREE_LEVEL_DEV_ID, Ddtp::Mode::Level3,
            "three_level_sv57_first_stage", dc, true, 0
        );
    }
    
    // 11. NEW TEST: Test with PD8 mode
    {
        // Create device context with PDTV=1 and PD8 mode
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            true,   // pdtv=true
            false,  // prpr
            false,  // gade
            false,  // sade
            true,   // dpe=true
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0,      // iohgatp_ppn
            0,      // pscid
            IosatpMode::Bare,
            0,      // iosatp_ppn
            PdtpMode::Pd8,  // Use PD8 mode (smallest PDT)
            PDTP_PPN
        );
        
        testDeviceContextConfig(
            iommu, THREE_LEVEL_DEV_ID, Ddtp::Mode::Level3,
            "three_level_pd8_mode", dc, true, 0
        );
    }
    
    // 12. NEW TEST: Test with PD20 mode
    {
        // Create device context with PDTV=1 and PD20 mode
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            true,   // pdtv=true
            false,  // prpr
            false,  // gade
            false,  // sade
            true,   // dpe=true
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0,      // iohgatp_ppn
            0,      // pscid
            IosatpMode::Bare,
            0,      // iosatp_ppn
            PdtpMode::Pd20, // Use PD20 mode (largest PDT)
            PDTP_PPN
        );
        
        testDeviceContextConfig(
            iommu, THREE_LEVEL_DEV_ID, Ddtp::Mode::Level3,
            "three_level_pd20_mode", dc, true, 0
        );
    }
    
    // 13. NEW TEST: Configure ATS capability and enable ATS
    {
        // Configure capabilities with ATS support
        Capabilities caps;
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
        caps.bits_.amoHwad_ = 1;
        caps.bits_.msiFlat_ = 1;
        caps.bits_.end_ = 1;
        caps.bits_.ats_ = 1;  // Enable ATS capability
        caps.bits_.t2gpa_ = 1; // Enable T2GPA capability
        iommu.configureCapabilities(caps.value_);
        
        // Create device context with ATS enabled
        DeviceContext dc = createDeviceContext(
            true,   // valid
            true,   // enable_ats=true
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false   // sxl
        );
        
        testDeviceContextConfig(
            iommu, THREE_LEVEL_DEV_ID, Ddtp::Mode::Level3,
            "three_level_ats_enabled", dc, true, 0
        );
    }
    
    // 14. NEW TEST: Test T2GPA with ATS
    {
        // Create device context with T2GPA=1, ATS=1, and second-stage translation
        DeviceContext dc = createDeviceContext(
            true,   // valid
            true,   // enable_ats=true
            false,  // enable_pri
            true,   // t2gpa=true
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Sv39x4, // Must have non-Bare second stage
            0x1234, // gscid
            IOHGATP_PPN
        );
        
        testDeviceContextConfig(
            iommu, THREE_LEVEL_DEV_ID, Ddtp::Mode::Level3,
            "three_level_t2gpa_enabled", dc, true, 0
        );
    }
    
    // 15. NEW TEST: ATS with PRI enabled
    {
        // Create device context with ATS and PRI enabled
        DeviceContext dc = createDeviceContext(
            true,   // valid
            true,   // enable_ats=true
            true,   // enable_pri=true
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            true,   // prpr=true
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0       // iohgatp_ppn
        );
        
        testDeviceContextConfig(
            iommu, THREE_LEVEL_DEV_ID, Ddtp::Mode::Level3,
            "three_level_ats_pri_enabled", dc, true, 0
        );
    }
    
    // 16. NEW TEST: Enable A/D bit updates
    {
        // Configure capabilities with AMO_HWAD=1
        Capabilities caps;
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
        caps.bits_.amoHwad_ = 1;  // A/D bit updates support
        caps.bits_.msiFlat_ = 1;
        caps.bits_.end_ = 1;
        caps.bits_.ats_ = 1;
        iommu.configureCapabilities(caps.value_);
        
        // Create device context with GADE=1 and SADE=1
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            true,   // gade=true
            true,   // sade=true
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Sv39x4,
            0x5678, // gscid
            IOHGATP_PPN,
            0,      // pscid
            IosatpMode::Sv39,
            IOSATP_PPN
        );
        
        testDeviceContextConfig(
            iommu, THREE_LEVEL_DEV_ID, Ddtp::Mode::Level3,
            "three_level_ad_bit_updates", dc, true, 0
        );
    }
    
    // 17. NEW TEST: Invalid configuration (T2GPA=1 but IOHGATP.MODE=Bare)
    {
        // Create device context with T2GPA=1 and EN_ATS=1 but IOHGATP.MODE=Bare
        DeviceContext dc = createDeviceContext(
            true,   // valid
            true,   // enable_ats=true
            false,  // enable_pri
            true,   // t2gpa=true
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false   // sxl
            // All other fields default to 0/Bare
        );
        
        testDeviceContextConfig(
            iommu, THREE_LEVEL_DEV_ID, Ddtp::Mode::Level3,
            "three_level_invalid_t2gpa_bare", dc, false, 259   // Should fail with DDT entry misconfigured
        );
    }
    
    // 18. NEW TEST: Invalid configuration (PRI enabled but ATS disabled)
    {
        // Create device context with PRI enabled but ATS disabled (invalid)
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats=false
            true,   // enable_pri=true (invalid without ATS)
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0       // iohgatp_ppn
        );
        
        testDeviceContextConfig(
            iommu, THREE_LEVEL_DEV_ID, Ddtp::Mode::Level3,
            "three_level_invalid_pri_without_ats", dc, false, 259   // Should fail with DDT entry misconfigured
        );
    }
    
    // MSI flat extended
    if (iommu.isDcExtended()) {
        std::cout << "\n[TEST] three-level extended format\n";
        
        // 19. Basic MSI test
        DeviceContext dc = createDeviceContext(
            true,   // valid
            false,  // enable_ats
            false,  // enable_pri
            false,  // t2gpa
            false,  // dtf
            false,  // pdtv
            false,  // prpr
            false,  // gade
            false,  // sade
            false,  // dpe
            false,  // sbe
            false,  // sxl
            IohgatpMode::Bare,
            0,      // gscid
            0,      // iohgatp_ppn
            0,      // pscid
            IosatpMode::Bare,
            0,      // iosatp_ppn
            PdtpMode::Bare,
            0,      // pdtp_ppn
            MsiptpMode::Flat,
            MSI_PPN,
            TestValues::MSI_ADDR_MASK,
            TestValues::MSI_ADDR_PATTERN
        );
        testDeviceContextConfig(iommu, THREE_LEVEL_DEV_ID,
                               Ddtp::Mode::Level3,
                               "three_level_msi_flat", dc, true, 0);
        
        // 20. NEW TEST: MSI with first and second stage translations
        {
            DeviceContext dc = createDeviceContext(
                true,    // valid
                false,   // enable_ats
                false,   // enable_pri
                false,   // t2gpa
                false,   // dtf
                false,   // pdtv
                false,   // prpr
                false,   // gade 
                false,   // sade
                false,   // dpe
                false,   // sbe
                false,   // sxl
                IohgatpMode::Sv39x4,
                0x1234,  // gscid
                IOHGATP_PPN,
                0x5678,  // pscid
                IosatpMode::Sv39,
                IOSATP_PPN,
                PdtpMode::Bare,
                0,       // pdtp_ppn
                MsiptpMode::Flat,
                MSI_PPN,
                TestValues::MSI_ADDR_MASK,
                TestValues::MSI_ADDR_PATTERN
            );
            
            testDeviceContextConfig(
                iommu, THREE_LEVEL_DEV_ID, Ddtp::Mode::Level3,
                "three_level_msi_with_stages", dc, true, 0
            );
        }
        
        // 21. NEW TEST: Invalid MSI mode
        {
            // Create device context with invalid MSI mode (3)
            DeviceContext dc = createDeviceContext(
                true,   // valid
                false,  // enable_ats
                false,  // enable_pri
                false,  // t2gpa
                false,  // dtf
                false,  // pdtv
                false,  // prpr
                false,  // gade
                false,  // sade
                false,  // dpe
                false,  // sbe
                false,  // sxl
                IohgatpMode::Bare,
                0,      // gscid
                0,      // iohgatp_ppn
                0,      // pscid
                IosatpMode::Bare,
                0,      // iosatp_ppn
                PdtpMode::Bare,
                0,      // pdtp_ppn
                static_cast<MsiptpMode>(3),  // Invalid mode
                MSI_PPN,
                TestValues::MSI_ADDR_MASK,
                TestValues::MSI_ADDR_PATTERN
            );
            
            testDeviceContextConfig(
                iommu, THREE_LEVEL_DEV_ID, Ddtp::Mode::Level3,
                "three_level_invalid_msi_mode", dc, false, 259   // Should fail with DDT entry misconfigured
            );
        }
        
        // 22. NEW TEST: Full feature set with MSI, ATS, PRI, and both stages
        {
            // Create device context with all features enabled
            DeviceContext dc = createDeviceContext(
                true,    // valid
                true,    // enable_ats
                true,    // enable_pri
                false,   // t2gpa
                false,   // dtf
                true,    // pdtv
                true,    // prpr
                true,    // gade
                true,    // sade
                true,    // dpe
                false,   // sbe
                false,   // sxl
                IohgatpMode::Sv39x4,
                0x5678,  // gscid
                IOHGATP_PPN,
                0x1234,  // pscid
                IosatpMode::Bare,
                0,       // iosatp_ppn
                PdtpMode::Pd17,
                PDTP_PPN,
                MsiptpMode::Flat,
                MSI_PPN,
                TestValues::MSI_ADDR_MASK,
                TestValues::MSI_ADDR_PATTERN
            );
            
            testDeviceContextConfig(
                iommu, THREE_LEVEL_DEV_ID, Ddtp::Mode::Level3,
                "three_level_full_features", dc, true, 0
            );
        }
    }
}

int main() {
    testDdtWalkSingleLevel();
    testDdtWalkTwoLevel();
    testDdtWalkThreeLevel();
    return 0;
}
