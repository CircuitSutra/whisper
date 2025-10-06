#pragma once

#include "IommuStructures.hpp"
#include "MemoryManager.hpp"
#include "MemoryModel.hpp"
#include <iostream>
#include <functional>

namespace IOMMU {

class TableBuilder {
public:
    using MemoryReadFunc = std::function<bool(uint64_t, unsigned, uint64_t&)>;
    using MemoryWriteFunc = std::function<bool(uint64_t, unsigned, uint64_t)>;

    TableBuilder(MemoryManager& memMgr, MemoryReadFunc readFunc, MemoryWriteFunc writeFunc)
        : mem_mgr_(memMgr), read_func_(readFunc), write_func_(writeFunc) {}

    // Build Device Directory Table entry (adapted from add_dev_context)
    uint64_t addDeviceContext(const device_context_t& dc, uint32_t device_id, 
                             const ddtp_t& ddtp, bool msi_flat = false) {
        uint16_t ddi[3];
        uint8_t dc_size;
        
        // Calculate device directory indexes based on MSI format
        if (!msi_flat) {
            ddi[0] = get_bits(6, 0, device_id);
            ddi[1] = get_bits(15, 7, device_id);
            ddi[2] = get_bits(23, 16, device_id);
            dc_size = BASE_FORMAT_DC_SIZE;
        } else {
            ddi[0] = get_bits(5, 0, device_id);
            ddi[1] = get_bits(14, 6, device_id);
            ddi[2] = get_bits(23, 15, device_id);
            dc_size = EXT_FORMAT_DC_SIZE;
        }

        uint64_t addr = ddtp.ppn * PAGESIZE;
        uint8_t levels;
        
        switch (ddtp.iommu_mode) {
            case DDT_3LVL: levels = 3; break;
            case DDT_2LVL: levels = 2; break;
            case DDT_1LVL: levels = 1; break;
            default:
                std::cerr << "[TABLE] Invalid DDT mode: " << ddtp.iommu_mode << std::endl;
                return 0;
        }

        // Walk down the directory levels
        for (int i = levels - 1; i > 0; i--) {
            ddte_t ddte;
            uint64_t entry_addr = addr + (ddi[i] * 8);
            
            if (!read_func_(entry_addr, 8, ddte.raw)) {
                std::cerr << "[TABLE] Failed to read DDTE at 0x" << std::hex << entry_addr << std::endl;
                return 0;
            }

            if (ddte.V == 0) {
                // Allocate new page for next level
                ddte.V = 1;
                ddte.PPN = mem_mgr_.getFreePhysicalPages(1);
                
                if (!write_func_(entry_addr, 8, ddte.raw)) {
                    std::cerr << "[TABLE] Failed to write DDTE at 0x" << std::hex << entry_addr << std::endl;
                    return 0;
                }
                
                std::cout << "[TABLE] Created DDT level " << i << " entry at 0x" 
                          << std::hex << entry_addr << " -> PPN 0x" << ddte.PPN << std::dec << std::endl;
            }
            
            addr = ddte.PPN * PAGESIZE;
        }

        // Write device context at leaf level
        uint64_t dc_addr = addr + (ddi[0] * dc_size);
        if (!write_func_(dc_addr, dc_size, reinterpret_cast<const uint64_t&>(dc))) {
            std::cerr << "[TABLE] Failed to write device context at 0x" << std::hex << dc_addr << std::endl;
            return 0;
        }

        std::cout << "[TABLE] Added device context for device_id 0x" << std::hex << device_id 
                  << " at address 0x" << dc_addr << std::dec << std::endl;
        
        return dc_addr;
    }

    // Build Process Directory Table entry (adapted from add_process_context)
    uint64_t addProcessContext(const device_context_t& dc, const process_context_t& pc, 
                              uint32_t process_id) {
        uint16_t pdi[3];
        
        pdi[0] = get_bits(7, 0, process_id);
        pdi[1] = get_bits(16, 8, process_id);
        pdi[2] = get_bits(19, 17, process_id);

        uint8_t levels;
        switch (dc.fsc.pdtp.MODE) {
            case PD20: levels = 3; break;
            case PD17: levels = 2; break;
            case PD8: levels = 1; break;
            default:
                std::cerr << "[TABLE] Invalid PDT mode: " << dc.fsc.pdtp.MODE << std::endl;
                return 0;
        }

        uint64_t addr = dc.fsc.pdtp.PPN * PAGESIZE;
        
        // Walk down the process directory levels
        for (int i = levels - 1; i > 0; i--) {
            // Translate through G-stage if needed
            if (dc.iohgatp.MODE != IOHGATP_Bare) {
                uint64_t spa;
                if (!translateGPA(dc.iohgatp, addr, spa)) {
                    std::cerr << "[TABLE] G-stage translation failed for addr 0x" 
                              << std::hex << addr << std::endl;
                    return 0;
                }
                addr = spa;
            }

            pdte_t pdte;
            uint64_t entry_addr = addr + (pdi[i] * 8);
            
            if (!read_func_(entry_addr, 8, pdte.raw)) {
                std::cerr << "[TABLE] Failed to read PDTE at 0x" << std::hex << entry_addr << std::endl;
                return 0;
            }

            if (pdte.V == 0) {
                pdte.V = 1;
                
                if (dc.iohgatp.MODE != IOHGATP_Bare) {
                    // Allocate guest page and map it
                    pdte.PPN = mem_mgr_.getFreeGuestPages(1, dc.iohgatp);
                    
                    // Create G-stage mapping for the allocated page
                    gpte_t gpte;
                    gpte.V = 1;
                    gpte.R = 1;
                    gpte.W = 0;
                    gpte.X = 0;
                    gpte.U = 1;
                    gpte.G = 0;
                    gpte.A = 0;
                    gpte.D = 0;
                    gpte.PBMT = PMA;
                    gpte.PPN = mem_mgr_.getFreePhysicalPages(1);
                    
                    if (!addGStagePageTableEntry(dc.iohgatp, PAGESIZE * pdte.PPN, gpte, 0)) {
                        std::cerr << "[TABLE] Failed to create G-stage mapping" << std::endl;
                        return 0;
                    }
                } else {
                    pdte.PPN = mem_mgr_.getFreePhysicalPages(1);
                }
                
                if (!write_func_(entry_addr, 8, pdte.raw)) {
                    std::cerr << "[TABLE] Failed to write PDTE at 0x" << std::hex << entry_addr << std::endl;
                    return 0;
                }
                
                std::cout << "[TABLE] Created PDT level " << i << " entry at 0x" 
                          << std::hex << entry_addr << " -> PPN 0x" << pdte.PPN << std::dec << std::endl;
            }
            
            addr = pdte.PPN * PAGESIZE;
        }

        // Translate final address if needed
        if (dc.iohgatp.MODE != IOHGATP_Bare) {
            uint64_t spa;
            if (!translateGPA(dc.iohgatp, addr, spa)) {
                std::cerr << "[TABLE] Final G-stage translation failed" << std::endl;
                return 0;
            }
            addr = spa;
        }

        // Write process context at leaf level
        uint64_t pc_addr = addr + (pdi[0] * 16);
        if (!write_func_(pc_addr, 16, reinterpret_cast<const uint64_t&>(pc))) {
            std::cerr << "[TABLE] Failed to write process context at 0x" << std::hex << pc_addr << std::endl;
            return 0;
        }

        std::cout << "[TABLE] Added process context for process_id 0x" << std::hex << process_id 
                  << " at address 0x" << pc_addr << std::dec << std::endl;
        
        return pc_addr;
    }

    // Add G-stage page table entry (adapted from add_g_stage_pte)
    bool addGStagePageTableEntry(const iohgatp_t& iohgatp, uint64_t gpa, 
                                const gpte_t& gpte, uint8_t add_level) {
        uint16_t vpn[5];
        uint8_t levels, pte_size = 8;
        
        // Determine levels and VPN extraction based on mode
        switch (iohgatp.MODE) {
            case IOHGATP_Sv32x4:
                vpn[0] = get_bits(21, 12, gpa);
                vpn[1] = get_bits(34, 22, gpa);
                levels = 2;
                pte_size = 4; // 32-bit PTEs
                break;
            case IOHGATP_Sv39x4:
                vpn[0] = get_bits(20, 12, gpa);
                vpn[1] = get_bits(29, 21, gpa);
                vpn[2] = get_bits(40, 30, gpa);
                levels = 3;
                break;
            case IOHGATP_Sv48x4:
                vpn[0] = get_bits(20, 12, gpa);
                vpn[1] = get_bits(29, 21, gpa);
                vpn[2] = get_bits(38, 30, gpa);
                vpn[3] = get_bits(49, 39, gpa);
                levels = 4;
                break;
            case IOHGATP_Sv57x4:
                vpn[0] = get_bits(20, 12, gpa);
                vpn[1] = get_bits(29, 21, gpa);
                vpn[2] = get_bits(38, 30, gpa);
                vpn[3] = get_bits(47, 39, gpa);
                vpn[4] = get_bits(58, 48, gpa);
                levels = 5;
                break;
            default:
                std::cerr << "[TABLE] Invalid IOHGATP mode: " << iohgatp.MODE << std::endl;
                return false;
        }

        uint64_t addr = iohgatp.PPN * PAGESIZE;
        
        // Walk down page table levels
        for (int i = levels - 1; i > add_level; i--) {
            gpte_t nl_gpte;
            uint64_t entry_addr = addr | (vpn[i] * pte_size);
            
            if (!read_func_(entry_addr, pte_size, nl_gpte.raw)) {
                std::cerr << "[TABLE] Failed to read G-stage PTE at 0x" << std::hex << entry_addr << std::endl;
                return false;
            }

            if (nl_gpte.V == 0) {
                nl_gpte.V = 1;
                nl_gpte.PPN = mem_mgr_.getFreePhysicalPages(1);
                
                if (!write_func_(entry_addr, pte_size, nl_gpte.raw)) {
                    std::cerr << "[TABLE] Failed to write G-stage PTE at 0x" << std::hex << entry_addr << std::endl;
                    return false;
                }
                
                std::cout << "[TABLE] Created G-stage PT level " << i << " entry at 0x" 
                          << std::hex << entry_addr << " -> PPN 0x" << nl_gpte.PPN << std::dec << std::endl;
            }
            
            addr = nl_gpte.PPN * PAGESIZE;
        }

        // Write leaf PTE
        uint64_t leaf_addr = addr | (vpn[add_level] * pte_size);
        if (!write_func_(leaf_addr, pte_size, gpte.raw)) {
            std::cerr << "[TABLE] Failed to write G-stage leaf PTE at 0x" << std::hex << leaf_addr << std::endl;
            return false;
        }

        std::cout << "[TABLE] Added G-stage PTE for GPA 0x" << std::hex << gpa 
                  << " at address 0x" << leaf_addr << std::dec << std::endl;
        
        return true;
    }

        // Create MSI-enabled device context (extension of addDeviceContext)
    uint64_t addMsiDeviceContext(const device_context_t& dc, uint32_t device_id,
                                const ddtp_t& ddtp, bool msi_flat,
                                uint64_t msi_addr_mask, uint64_t msi_addr_pattern,
                                uint64_t msiptp) {
        
        // First create the basic device context structure
        uint64_t dc_addr = addDeviceContext(dc, device_id, ddtp, msi_flat);
        
        if (dc_addr == 0) {
            std::cerr << "[TABLE] Failed to create basic device context for MSI device" << std::endl;
            return 0;
        }
        
        // If using extended format, write additional MSI fields
        if (msi_flat) {
            // Write MSI-specific fields to extended device context
            // Extended format: tc, iohgatp, ta, fsc, msiptp, msi_addr_mask, msi_addr_pattern, reserved
            
            uint64_t msiptp_addr = dc_addr + 32; // After base format fields
            uint64_t msi_mask_addr = dc_addr + 40;
            uint64_t msi_pattern_addr = dc_addr + 48;
            
            if (!write_func_(msiptp_addr, 8, msiptp) ||
                !write_func_(msi_mask_addr, 8, msi_addr_mask) ||
                !write_func_(msi_pattern_addr, 8, msi_addr_pattern)) {
                std::cerr << "[TABLE] Failed to write MSI fields to device context" << std::endl;
                return 0;
            }
            
            std::cout << "[TABLE] Added MSI fields to device context at 0x" << std::hex << dc_addr
                      << ": MSIPTP=0x" << msiptp << ", mask=0x" << msi_addr_mask 
                      << ", pattern=0x" << msi_addr_pattern << std::dec << std::endl;
        }
        
        return dc_addr;
    }

    // Setup MSI page table for Flat mode
    bool setupMsiPageTable(uint64_t msi_ppn, uint64_t target_ppn, 
                          uint16_t num_entries = 16) {
        uint64_t pageSize = 4096;
        uint64_t msiTableAddr = msi_ppn * pageSize;
        
        std::cout << "[TABLE] Setting up MSI page table at PPN 0x" << std::hex << msi_ppn 
                  << " with " << std::dec << num_entries << " entries" << std::endl;
        
        // Create valid MSI PTEs for each entry
        for (uint16_t i = 0; i < num_entries; i++) {
            // Basic translate mode PTE (mode 3)
            // Format: V=1, M=3 (basic translate), PPN=target
            uint64_t pte = 0;
            pte |= 0x1;                    // V bit (valid)
            pte |= (0x3ULL << 1);          // M bits (mode 3 = basic translate)  
            pte |= (target_ppn << 10);     // PPN field
            
            uint64_t pte_addr = msiTableAddr + (i * 8);
            if (!write_func_(pte_addr, 8, pte)) {
                std::cerr << "[TABLE] Failed to write MSI PTE " << i 
                          << " at address 0x" << std::hex << pte_addr << std::endl;
                return false;
            }
        }
        
        std::cout << "[TABLE] MSI page table setup complete: " << num_entries 
                  << " entries pointing to PPN 0x" << std::hex << target_ppn << std::dec << std::endl;
        
        return true;
    }

    // Add S-stage page table entry (adapted from add_s_stage_pte) 
    bool addSStagePageTableEntry(const iosatp_t& satp, uint64_t va, 
                                const pte_t& pte, uint8_t add_level, uint8_t sxl = 0) {
        uint16_t vpn[5];
        uint8_t levels, pte_size = 8;
        
        // Determine levels and VPN extraction based on mode
        switch (satp.MODE) {
            case IOSATP_Sv32:
                if (sxl == 1) {
                    vpn[0] = get_bits(21, 12, va);
                    vpn[1] = get_bits(31, 22, va);
                    levels = 2;
                    pte_size = 4; // 32-bit PTEs
                } else {
                    std::cerr << "[TABLE] Sv32 requires SXL=1" << std::endl;
                    return false;
                }
                break;
            case IOSATP_Sv39:
                if (sxl == 0) {
                    vpn[0] = get_bits(20, 12, va);
                    vpn[1] = get_bits(29, 21, va);
                    vpn[2] = get_bits(38, 30, va);
                    levels = 3;
                } else {
                    std::cerr << "[TABLE] Sv39 requires SXL=0" << std::endl;
                    return false;
                }
                break;
            case IOSATP_Sv48:
                vpn[0] = get_bits(20, 12, va);
                vpn[1] = get_bits(29, 21, va);
                vpn[2] = get_bits(38, 30, va);
                vpn[3] = get_bits(47, 39, va);
                levels = 4;
                break;
            case IOSATP_Sv57:
                vpn[0] = get_bits(20, 12, va);
                vpn[1] = get_bits(29, 21, va);
                vpn[2] = get_bits(38, 30, va);
                vpn[3] = get_bits(47, 39, va);
                vpn[4] = get_bits(56, 48, va);
                levels = 5;
                break;
            default:
                std::cerr << "[TABLE] Invalid IOSATP mode: " << satp.MODE << std::endl;
                return false;
        }

        uint64_t addr = satp.PPN * PAGESIZE;
        
        // Walk down page table levels
        for (int i = levels - 1; i > add_level; i--) {
            pte_t nl_pte;
            uint64_t entry_addr = addr | (vpn[i] * pte_size);
            
            if (!read_func_(entry_addr, pte_size, nl_pte.raw)) {
                std::cerr << "[TABLE] Failed to read S-stage PTE at 0x" << std::hex << entry_addr << std::endl;
                return false;
            }

            if (nl_pte.V == 0) {
                nl_pte.V = 1;
                nl_pte.PPN = mem_mgr_.getFreePhysicalPages(1);
                
                if (!write_func_(entry_addr, pte_size, nl_pte.raw)) {
                    std::cerr << "[TABLE] Failed to write S-stage PTE at 0x" << std::hex << entry_addr << std::endl;
                    return false;
                }
                
                std::cout << "[TABLE] Created S-stage PT level " << i << " entry at 0x" 
                          << std::hex << entry_addr << " -> PPN 0x" << nl_pte.PPN << std::dec << std::endl;
            }
            
            addr = nl_pte.PPN * PAGESIZE;
        }

        // Write leaf PTE
        uint64_t leaf_addr = addr | (vpn[add_level] * pte_size);
        if (!write_func_(leaf_addr, pte_size, pte.raw)) {
            std::cerr << "[TABLE] Failed to write S-stage leaf PTE at 0x" << std::hex << leaf_addr << std::endl;
            return false;
        }

        std::cout << "[TABLE] Added S-stage PTE for VA 0x" << std::hex << va 
                  << " at address 0x" << leaf_addr << std::dec << std::endl;
        
        return true;
    }

    // Simplified GPA translation (basic version of translate_gpa)
    bool translateGPA(const iohgatp_t& iohgatp, uint64_t gpa, uint64_t& spa) {
        if (iohgatp.MODE == IOHGATP_Bare) {
            spa = gpa;
            return true;
        }
        
        // For now, implement a basic translation - in real implementation
        // this would walk the G-stage page tables
        spa = gpa; // Placeholder - implement full translation as needed
        return true;
    }

    // Create fault-testing device context with specific flags
    uint64_t addFaultTestDevice(uint32_t device_id, const ddtp_t& ddtp,
                               bool dtf_enabled = false, bool sbe_enabled = false,
                               bool pdtv_enabled = false, uint64_t pdtp_value = 0) {
        device_context_t dc = {};
        
        // Set basic fields in tc (translation control) field
        uint64_t tc_value = 0;
        tc_value |= 1ULL;  // V=1 (Valid)
        if (dtf_enabled) tc_value |= (1ULL << 6);  // DTF bit
        if (sbe_enabled) tc_value |= (1ULL << 8);  // SBE bit  
        if (pdtv_enabled) tc_value |= (1ULL << 10); // PDTV bit
        dc.tc = tc_value;
        
        // Set process directory pointer if PDTV is enabled  
        if (pdtv_enabled && pdtp_value != 0) {
            dc.fsc.pdtp.raw = pdtp_value;
        }
        
        return addDeviceContext(dc, device_id, ddtp);
    }

    // Create invalid device context (V=0) for fault testing
    uint64_t addInvalidDevice(uint32_t device_id, const ddtp_t& ddtp) {
        device_context_t dc = {}; // All zeros - V=0 makes it invalid
        return addDeviceContext(dc, device_id, ddtp);
    }

private:
    MemoryManager& mem_mgr_;
    MemoryReadFunc read_func_;
    MemoryWriteFunc write_func_;
};

} // namespace IOMMU
