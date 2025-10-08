#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include "DeviceContext.hpp"
#include "IommuCsr.hpp"
#include "ProcessContext.hpp"

namespace IOMMU {

constexpr uint64_t PAGESIZE = 4096;
constexpr uint8_t BASE_FORMAT_DC_SIZE = 64;
constexpr uint8_t EXT_FORMAT_DC_SIZE = 128;
constexpr uint8_t CQ_ENTRY_SZ = 16;
constexpr uint8_t FQ_ENTRY_SZ = 32;

inline uint64_t get_bits(uint8_t msb, uint8_t lsb, uint64_t value) {
    uint64_t mask = ((1ULL << (msb - lsb + 1)) - 1);
    return (value >> lsb) & mask;
}

struct ddtp_t {
    union {
        uint64_t raw = 0;
        struct {
            uint64_t iommu_mode : 4;
            uint64_t busy : 1;
            uint64_t reserved : 5;
            uint64_t ppn : 44;
            uint64_t reserved2 : 10;
        };
    };
    ddtp_t() = default;
    ddtp_t(uint64_t val) : raw(val) {}
};

using ddte_t = TT_IOMMU::Ddte;

struct pdtp_t {
    union {
        uint64_t raw = 0;
        struct {
            uint64_t MODE : 4;
            uint64_t reserved : 16;
            uint64_t PPN : 44;
        };
    };
    pdtp_t() = default;
    pdtp_t(uint64_t val) : raw(val) {}
};

struct pdte_t {
    union {
        uint64_t raw = 0;
        struct {
            uint64_t V : 1;
            uint64_t reserved0 : 9;
            uint64_t PPN : 44;
            uint64_t reserved1 : 10;
        };
    };
    pdte_t() = default;
    pdte_t(uint64_t val) : raw(val) {}
};

struct iohgatp_t {
    union {
        uint64_t value_ = 0;
        struct {
            uint64_t ppn_     : 44;
            unsigned gcsid_   : 16;
            uint32_t mode_    : 4;
        } bits_;
    };
    iohgatp_t() = default;
    iohgatp_t(uint64_t val) : value_(val) {}
};

struct iosatp_t {
    union {
        uint64_t raw = 0;
        struct {
            uint64_t MODE : 4;
            uint64_t reserved : 16;
            uint64_t PPN : 44;
        };
    };
    iosatp_t() = default;
    iosatp_t(uint64_t val) : raw(val) {}
};

struct fsc_t {
    pdtp_t pdtp{};
    iosatp_t iosatp{};
    std::array<uint64_t, 6> reserved{};
};

struct device_context_t {
    uint64_t tc{};
    iohgatp_t iohgatp{};
    fsc_t fsc{};
    uint64_t msiptp{};
    uint64_t msi_addr_mask{};
    uint64_t msi_addr_pattern{};
    std::array<uint64_t, 2> reserved{};
};

struct process_context_t {
    iosatp_t ta{};
    std::array<uint64_t, 1> reserved{};
};

using DdtpMode = TT_IOMMU::Ddtp::Mode;
using PdtpMode = TT_IOMMU::PdtpMode;
using IohgatpMode = TT_IOMMU::IohgatpMode;
using MsiptpMode = TT_IOMMU::MsiptpMode;
using IosatpMode = TT_IOMMU::IosatpMode;

enum DDTMode : uint8_t {
    DDT_OFF = 0,
    DDT_1LVL = 1,
    DDT_2LVL = 2,
    DDT_3LVL = 3
};

enum PDTMode : uint8_t {
    PD_OFF = 0,
    PD8 = 1,
    PD17 = 2,
    PD20 = 3
};

enum IOHGATPMode : uint8_t {
    IOHGATP_Bare = 0,
    IOHGATP_Sv32x4 = 1,
    IOHGATP_Sv39x4 = 8,
    IOHGATP_Sv48x4 = 9,
    IOHGATP_Sv57x4 = 10
};

enum IOSATPMode : uint8_t {
    IOSATP_Bare = 0,
    IOSATP_Sv32 = 1,
    IOSATP_Sv39 = 8,
    IOSATP_Sv48 = 9,
    IOSATP_Sv57 = 10
};

enum PBMT : uint8_t {
    PMA = 0,
    NC = 1,
    IO = 2
};

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

} // namespace IOMMU

