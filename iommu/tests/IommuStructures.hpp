#pragma once

#include <array>
#include <cstdint>
#include <cstring>

// -----------------------------------------------------------------------------
// RISC-V IOMMU Data Structures
// -----------------------------------------------------------------------------

namespace IOMMU {

// Constants
constexpr uint64_t PAGESIZE = 4096;
constexpr uint8_t BASE_FORMAT_DC_SIZE = 64;
constexpr uint8_t EXT_FORMAT_DC_SIZE = 128;
constexpr uint8_t CQ_ENTRY_SZ = 16;
constexpr uint8_t FQ_ENTRY_SZ = 32;

// Helper function to extract bits
inline uint64_t get_bits(uint8_t msb, uint8_t lsb, uint64_t value) {
    uint64_t mask = ((1ULL << (msb - lsb + 1)) - 1);
    return (value >> lsb) & mask;
}

// DDT modes
enum DDTMode : uint8_t {
    DDT_OFF = 0,
    DDT_1LVL = 1,
    DDT_2LVL = 2,
    DDT_3LVL = 3
};

// PDT modes  
enum PDTMode : uint8_t {
    PD_OFF = 0,
    PD8 = 1,
    PD17 = 2,
    PD20 = 3
};

// IOHGATP modes
enum IOHGATPMode : uint8_t {
    IOHGATP_Bare = 0,
    IOHGATP_Sv32x4 = 1,
    IOHGATP_Sv39x4 = 8,
    IOHGATP_Sv48x4 = 9,
    IOHGATP_Sv57x4 = 10
};

// IOSATP modes
enum IOSATPMode : uint8_t {
    IOSATP_Bare = 0,
    IOSATP_Sv32 = 1,
    IOSATP_Sv39 = 8,
    IOSATP_Sv48 = 9,
    IOSATP_Sv57 = 10
};

// PBMT values
enum PBMT : uint8_t {
    PMA = 0,
    NC = 1,
    IO = 2
};

// Device Directory Table Pointer
struct ddtp_t {
    union {
        uint64_t raw = 0;
        struct {
            uint64_t iommu_mode : 4;
            uint64_t busy : 1;
            uint64_t reserved : 7;
            uint64_t ppn : 52;
        };
    };
    ddtp_t() {}
    ddtp_t(uint64_t val) : raw(val) {}
};

// Device Directory Table Entry
struct ddte_t {
    union {
        uint64_t raw = 0;
        struct {
            uint64_t V : 1;
            uint64_t reserved : 11;
            uint64_t PPN : 52;
        };
    };
    ddte_t() {}
    ddte_t(uint64_t val) : raw(val) {}
};

// Process Directory Table Pointer
struct pdtp_t {
    union {
        uint64_t raw = 0;
        struct {
            uint64_t MODE : 4;
            uint64_t reserved : 8;
            uint64_t PPN : 52;
        };
    };
    pdtp_t() {}
    pdtp_t(uint64_t val) : raw(val) {}
};

// Process Directory Table Entry
struct pdte_t {
    union {
        uint64_t raw = 0;
        struct {
            uint64_t V : 1;
            uint64_t reserved0 : 11;
            uint64_t PPN : 52;
        };
    };
    pdte_t() {}
    pdte_t(uint64_t val) : raw(val) {}
};

// IOHGATP register
struct iohgatp_t {
    union {
        uint64_t raw = 0;
        struct {
            uint64_t MODE : 4;
            uint64_t reserved0 : 12;
            uint64_t GSCID : 16;
            uint64_t PPN : 32;
        };
    };
    iohgatp_t() {}
    iohgatp_t(uint64_t val) : raw(val) {}
};

// IOSATP register
struct iosatp_t {
    union {
        uint64_t raw = 0;
        struct {
            uint64_t MODE : 4;
            uint64_t reserved : 60;
            uint64_t PPN : 44;
        };
    };
    iosatp_t() {}
    iosatp_t(uint64_t val) : raw(val) {}
};

// G-stage Page Table Entry
struct gpte_t {
    union {
        uint64_t raw = 0;
        struct {
            uint64_t V : 1;
            uint64_t R : 1;
            uint64_t W : 1;
            uint64_t X : 1;
            uint64_t U : 1;
            uint64_t G : 1;
            uint64_t A : 1;
            uint64_t D : 1;
            uint64_t reserved0 : 2;
            uint64_t PPN : 44;
            uint64_t reserved1 : 7;
            uint64_t PBMT : 2;
            uint64_t N : 1;
        };
    };
    gpte_t() {}
    gpte_t(uint64_t val) : raw(val) {}
};

// S-stage Page Table Entry  
struct pte_t {
    union {
        uint64_t raw = 0;
        struct {
            uint64_t V : 1;
            uint64_t R : 1;
            uint64_t W : 1;
            uint64_t X : 1;
            uint64_t U : 1;
            uint64_t G : 1;
            uint64_t A : 1;
            uint64_t D : 1;
            uint64_t reserved0 : 2;
            uint64_t PPN : 44;
            uint64_t reserved1 : 10;
        };
    };
    pte_t() {}
    pte_t(uint64_t val) : raw(val) {}
};

// First Stage Context
struct fsc_t {
    pdtp_t pdtp{};
    iosatp_t iosatp{};
    std::array<uint64_t, 6> reserved{};
};

// Device Context
struct device_context_t {
    uint64_t tc{};
    iohgatp_t iohgatp{};
    fsc_t fsc{};
    uint64_t msiptp{};
    uint64_t msi_addr_mask{};
    uint64_t msi_addr_pattern{};
    std::array<uint64_t, 2> reserved{};
};

// Process Context
struct process_context_t {
    iosatp_t ta{};
    std::array<uint64_t, 1> reserved{};
};

// Capabilities
struct capabilities_t {
    uint8_t version;
    uint8_t Sv32 : 1;
    uint8_t Sv39 : 1; 
    uint8_t Sv48 : 1;
    uint8_t Sv57 : 1;
    uint8_t Sv39x4 : 1;
    uint8_t Sv48x4 : 1;
    uint8_t Sv57x4 : 1;
    uint8_t amo_hwad : 1;
    uint8_t ats : 1;
    uint8_t t2gpa : 1;
    uint8_t hpm : 1;
    uint8_t dbg : 1;
    uint8_t msi_flat : 1;
    uint8_t msi_mrif : 1;
    uint8_t amo_mrif : 1;
    uint8_t pas;
    uint8_t pd8 : 1;
    uint8_t pd17 : 1;
    uint8_t pd20 : 1;
    uint8_t Svrsw60t59b : 1;
    
    capabilities_t() { std::memset(this, 0, sizeof(*this)); }
};

// Feature Control
struct fctl_t {
    uint8_t be : 1;
    uint8_t wsi : 1;
    uint8_t gxl : 1;
    uint8_t reserved : 5;
    
    fctl_t() { std::memset(this, 0, sizeof(*this)); }
};

} // namespace IOMMU

