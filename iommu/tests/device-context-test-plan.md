# Device Context Table Traversal Test Plan  
## Overview

This test plan verifies the structure, traversal, and interpretation of Device Context Table (DDT) entries within the RISC-V IOMMU. Focus is placed on:

- Device ID-based multi-level DDT lookup
- Field-by-field validation of 32-byte and 64-byte leaf entries
- Enforcement of legal combinations in translation control (`tc`)
- Compliance with Section 3.1.3 field semantics
- Correct fault signaling (cause = 259) on misconfiguration

## Leaf Format
Each DDT leaf contains either 4 or 8 doublewords (64-bit entries), depending on the `capabilities.MSI_FLAT` bit:

| Offset (DW) | Field Name                 | Required When         |
|-------------|----------------------------|-----------------------|
| 0           | Translation Control (`tc`) | Always                |
| 1           | IOHGATP                    | Always                |
| 2           | Translation Attributes     | Always                |
| 3           | First-Stage Context (`fsc`)| Always                |
| 4           | MSI Page Table Pointer     | If `MSI_FLAT = 1`     |
| 5           | MSI Address Mask           | If `MSI_FLAT = 1`     |
| 6           | MSI Address Pattern        | If `MSI_FLAT = 1`     |
| 7           | Reserved                   | If `MSI_FLAT = 1`     |

## Device Context Table Traversal

| Test Case               | Description                                         | Expected Output                           |
|-------------------------|-----------------------------------------------------|-------------------------------------------|
| `ddt_walk_single_level` | ddtp.MODE = 2, device ID uses only `ddi[0]` (6 bits)| Correct DC located at level 0             |
| `ddt_walk_two_level`    | ddtp.MODE = 3, device ID uses `ddi[1:0]` (15 bits)  | DC located via 2-level traversal          |
| `ddt_walk_three_level`  | ddtp.MODE = 4, full 24-bit Device ID used           | DC found after 3-level walk               |
| `ddt_walk_invalid_entry`| Non-leaf entry has `V=0`                            | Translation fails with cause = 258        |
| `ddt_walk_leaf_invalid` | Leaf found, but format/fields invalid               | Fault raised with cause = 259             |

## Translation Control (`tc`) Validation

| Test Case                   | Description                                                       | Expected Output                           |
|-----------------------------|-------------------------------------------------------------------|-------------------------------------------|
| `tc_valid_bit_unset`        | `tc.V = 0`                                                        | Entry skipped, no fault                   |
| `tc_en_ats_wo_cap`          | `EN_ATS = 1` and `capabilities.ATS = 0`                           | Fault, cause = 259                        |
| `tc_t2gpa_wo_en_ats`        | `T2GPA = 1` but `EN_ATS = 0`                                      | Fault, cause = 259                        |
| `tc_en_pri_wo_en_ats`       | `EN_PRI = 1` but `EN_ATS = 0`                                     | Fault, cause = 259                        |
| `tc_prpr_wo_en_pri`         | `PRPR = 1` but `EN_PRI = 0`                                       | Fault, cause = 259                        |
| `tc_gade_sade_wo_cap`       | `GADE=1` or `SADE=1` but `capabilities.AMO_HWAD = 0`              | Fault, cause = 259                        |
| `tc_invalid_sxl_encoding`   | `SXL` set to reserved encoding                                    | Fault, cause = 259                        |
| `tc_sxl_gxl_mismatch`       | `tc.SXL != fctl.GXL`                                              | Fault, cause = 259                        |
| `tc_sbe_mismatch_wo_end`    | `tc.SBE != fctl.BE` and `capabilities.END = 0`                    | Fault, cause = 259                        |
| `tc_dpe_set_wo_pdtv`        | `DPE = 1` but `PDTV = 0`                                          | Fault, cause = 259                        |
| `tc_dtf_suppresses_fault`   | Set `DTF = 1`, introduce otherwise fatal misconfig in tc          | No fault triggered; proceeds with null/partial translation |


## Additional Test Cases for Section 3.1.4 Compliance
| Test Case                        | Description                                                                                   | Expected Output               |
|----------------------------------|-----------------------------------------------------------------------------------------------|-------------------------------|
| `tc_reserved_fields_set`         | Any reserved field in the Device Context is set                                               | Fault, cause = 259            |
| `tc_t2gpa_set_bare_mode`         | `iohgatp.MODE = 0` (Bare), but `tc.T2GPA = 1`                                                 | Fault, cause = 259            |
| `fsc_mode_invalid_pdvt1`         | `tc.PDTV = 1`, but `fsc.MODE` not in {PD8, PD17, PD20}                                        | Fault, cause = 259            |
| `fsc_mode_invalid_pdvt0_sxl0`    | `tc.PDTV = 0`, `tc.SXL = 0`, but `fsc.MODE` not in {0, 8, 9, 10}                              | Fault, cause = 259            |
| `fsc_mode_invalid_pdvt0_sxl1`    | `tc.PDTV = 0`, `tc.SXL = 1`, but `fsc.MODE` not in {0, 8}                                     | Fault, cause = 259            |
| `fsc_mode_invalid_pdvt0_sxl2`    | `tc.PDTV = 0`, `tc.SXL = 2`, but `fsc.MODE` not in {0, 9}                                     | Fault, cause = 259            |
| `iohgatp_mode_invalid_gxl0`      | `fctl.GXL = 0`, but `iohgatp.MODE` not in {0, 8, 9, 10}                                       | Fault, cause = 259            |
| `iohgatp_mode_invalid_gxl1`      | `fctl.GXL = 1`, but `iohgatp.MODE` not in {0, 8}                                              | Fault, cause = 259            |
| `iohgatp_mode_invalid_gxl2`      | `fctl.GXL = 2`, but `iohgatp.MODE` not in {0, 9}                                              | Fault, cause = 259            |
| `msiptp_mode_reserved`           | `capabilities.MSI_FLAT = 1`, but `msiptp.MODE` not in {0, 1}                                  | Fault, cause = 259            |
| `iohgatp_ppn_unaligned`          | `iohgatp.PPN` is not 16 KiB aligned                                                           | Fault, cause = 259            |
| `ta_rcid_mcid_overflow`          | `RCID` or `MCID` exceeds implementation-defined width                                         | Fault, cause = 259            |

## IOHGATP Field Tests
### Cases for fctl.GXL = 0

| Test Case                      | Description                                                                                  | Expected Output               |
|--------------------------------|----------------------------------------------------------------------------------------------|-------------------------------|
| `iohgatp_mode_valid_gxl0_bare` | Set `fctl.GXL = 0`, `iohgatp.MODE = 0` (Bare). No second-stage translation.                  | Device context accepted       |
| `iohgatp_mode_valid_gxl0_sv39` | Set `fctl.GXL = 0`, `iohgatp.MODE = 8` (Sv39x4). 41-bit guest virtual addressing.            | Device context accepted       |
| `iohgatp_mode_valid_gxl0_sv48` | Set `fctl.GXL = 0`, `iohgatp.MODE = 9` (Sv48x4). 50-bit guest virtual addressing.            | Device context accepted       |
| `iohgatp_mode_valid_gxl0_sv57` | Set `fctl.GXL = 0`, `iohgatp.MODE = 10` (Sv57x4). 59-bit guest virtual addressing.           | Device context accepted       |
| `iohgatp_mode_reserved_gxl0`   | Set `fctl.GXL = 0`, `iohgatp.MODE = 1–7 or 11–15`. Reserved encodings.                       | Fault, cause = 259            |


### Cases for fctl.GXL = 1

| Test Case                       | Description                                                                  | Expected Output               |
|---------------------------------|------------------------------------------------------------------------------|-------------------------------|
| `iohgatp_mode_valid_gxl1_bare`  | Set `fctl.GXL = 1`, `iohgatp.MODE = 0` (Bare). No second-stage translation.  | Device context accepted       |
| `iohgatp_mode_valid_gxl1_sv32`  | Set `fctl.GXL = 1`, `iohgatp.MODE = 8` (Sv32x4). 34-bit guest virtual addr.  | Device context accepted       |
| `iohgatp_mode_invalid_gxl1_sv39`| Set `fctl.GXL = 1`, `iohgatp.MODE = 9` (Sv48x4). Not valid for GXL = 1.      | Fault, cause = 259            |
| `iohgatp_mode_invalid_gxl1_sv57`| Set `fctl.GXL = 1`, `iohgatp.MODE = 10` (Sv57x4). Not valid for GXL = 1.     | Fault, cause = 259            |
| `iohgatp_mode_reserved_gxl1`    | Set `fctl.GXL = 1`, `iohgatp.MODE = 1–7 or 11–15`. Reserved encodings.       | Fault, cause = 259            |

### Shared Field Constraint Tests

| Test Case                     | Description                                                                      | Expected Output               |
|-------------------------------|----------------------------------------------------------------------------------|-------------------------------|
| `iohgatp_t2gpa_bare_mode`     | Set `T2GPA = 1` and `iohgatp.MODE = 0` (Bare). T2GPA only allowed with paging.   | Fault, cause = 259            |
| `iohgatp_ppn_misaligned`      | Set `iohgatp.PPN` to a value not aligned to 16 KiB.                              | Fault, cause = 259            |


## Translation Attributes (`ta`) Tests
### DC Valid Test Cases

| Test Case                     | Description                                                                           | Expected Output                         |
|-------------------------------|---------------------------------------------------------------------------------------|-----------------------------------------|
| `ta_pscid_used_iosatp`        | `tc.PDTV = 0`, `iosatp.MODE ≠ Bare`. `PSCID` is used as the process ID                | First-stage uses PSCID; valid context   |
| `ta_pscid_ignored_pdtp`       | `tc.PDTV = 1`. `PSCID` is ignored regardless of value                                 | Context uses pdtp; PSCID has no effect  |
| `ta_rcid_mcid_used_qosid1`    | `capabilities.QOSID = 1`. `RCID` and `MCID` are used to tag internal IOMMU accesses   | Values passed with memory transactions  |
| `ta_rcid_mcid_zero_qosid0`    | `capabilities.QOSID = 0`, `RCID` and `MCID` are 0                                     | Valid context                           |

### Misconfiguration Tests

| Test Case                      | Description                                                                             | Expected Output            |
|--------------------------------|-----------------------------------------------------------------------------------------|----------------------------|
| `ta_rcid_nonzero_qosid0`       | `capabilities.QOSID = 0`, but `RCID` ≠ 0                                                | Fault, cause = 259         |
| `ta_mcid_nonzero_qosid0`       | `capabilities.QOSID = 0`, but `MCID` ≠ 0                                                | Fault, cause = 259         |

## First-Stage Context (`fsc`) Tests
### Test Cases for `PDTV = 0` (fsc holds iosatp)

| Test Case                        | Description                                                                | Expected Output                |
|----------------------------------|----------------------------------------------------------------------------|--------------------------------|
| `fsc_iosatp_mode_valid_sv39`     | `tc.PDTV = 0`, `tc.SXL = 0`, `iosatp.MODE = 8` (Sv39)                      | Device context accepted        |
| `fsc_iosatp_mode_valid_sv48`     | `tc.PDTV = 0`, `tc.SXL = 0`, `iosatp.MODE = 9` (Sv48)                      | Device context accepted        |
| `fsc_iosatp_mode_valid_sv57`     | `tc.PDTV = 0`, `tc.SXL = 0`, `iosatp.MODE = 10` (Sv57)                     | Device context accepted        |
| `fsc_iosatp_mode_valid_sv32`     | `tc.PDTV = 0`, `tc.SXL = 1`, `iosatp.MODE = 8` (Sv32)                      | Device context accepted        |
| `fsc_iosatp_mode_valid_bare`     | `tc.PDTV = 0`, any `SXL`, `iosatp.MODE = 0` (Bare mode)                    | Device context accepted        |
| `fsc_iosatp_mode_invalid_sxl0`   | `tc.PDTV = 0`, `tc.SXL = 0`, `iosatp.MODE = 1–7 or 11–15`                        | Fault, cause = 259       |
| `fsc_iosatp_mode_invalid_sxl1`   | `tc.PDTV = 0`, `tc.SXL = 1`, `iosatp.MODE ≠ 0 or 8`                              | Fault, cause = 259       |
| `fsc_dpe_reserved_with_pdvt0`    | `tc.PDTV = 0`, `tc.DPE = 1` (default PASID enable only allowed when `PDTV = 1`)  | Fault, cause = 259       |


### Test Cases for `PDTV = 1` (fsc holds pdtp)

| Test Case                     | Description                                                                   | Expected Output               |
|-------------------------------|-------------------------------------------------------------------------------|-------------------------------|
| `fsc_pdtp_mode_valid_bare`    | `tc.PDTV = 1`, `pdtp.MODE = 0` (no translation)                               | Device context accepted       |
| `fsc_pdtp_mode_valid_pd8`     | `tc.PDTV = 1`, `pdtp.MODE = 1` (1-level PDT, 8-bit process ID)                | Device context accepted       |
| `fsc_pdtp_mode_valid_pd17`    | `tc.PDTV = 1`, `pdtp.MODE = 2` (2-level PDT, 17-bit process ID)               | Device context accepted       |
| `fsc_pdtp_mode_valid_pd20`    | `tc.PDTV = 1`, `pdtp.MODE = 3` (3-level PDT, 20-bit process ID)               | Device context accepted       |
| `fsc_pdtp_mode_reserved`     | `tc.PDTV = 1`, `pdtp.MODE` in range 4–15 (reserved or custom)                  | Fault, cause = 259            |


## MSI Translation Fields (if `MSI_FLAT = 1`)
### Valid Test Cases (MSI_FLAT = 1)

| Test Case                  | Description                                                              | Expected Output               |
|----------------------------|--------------------------------------------------------------------------|-------------------------------|
| `msiptp_mode_off`          | `msiptp.MODE = 0` (Off)                                                  | No MSI translation attempted  |
| `msiptp_mode_flat_valid`   | `msiptp.MODE = 1`, with properly aligned `PPN`                           | MSI accesses translated       |
| `msi_addr_match_hit`       | Incoming address matches pattern/mask                                    | Treated as MSI to virtual file|
| `msi_addr_match_miss`      | Address does not match pattern/mask                                      | Access proceeds normally      |

A device memory access to guest physical address **A** is considered an MSI access if:
((A >> 12) & ~msi_addr_mask) == (msi_addr_pattern & ~msi_addr_mask)

### Invalid / Faulting Cases

| Test Case                     | Description                                                               | Expected Output              |
|-------------------------------|---------------------------------------------------------------------------|------------------------------|
| `msiptp_mode_invalid_reserved`| `msiptp.MODE = 2–15` (reserved or custom)                                 | Fault, cause = 259           |
| `msiptp_present_msi_flat0`    | `capabilities.MSI_FLAT = 0` but DC includes `msiptp`                      | Fault, cause = 259           |
| `msiptp_misaligned_ppn`       | `msiptp.PPN` not aligned to 4 KiB                                         | Fault or implementation-defined |


## Edge Cases

| Test Case                  | Description                                                    | Expected Output                     |
|----------------------------|----------------------------------------------------------------|--------------------------------------|
| `ddt_full_population`      | All entries in 3-level DDT populated                           | All DCs reachable, no walk failure  |
| `ddt_entry_live_update`    | Update valid DC entry while walk is in-flight                  | Result reflects most recent state   |
| `tc_field_bit_flip`        | Flip each `tc` bit individually                                | Expected fault or success per spec  |
| `dynamic_format_switch`    | Toggle `MSI_FLAT`, verify entry re-parsed correctly            | Correct format selected             |
| `default_format_mismatch`  | Use 64B entry when `MSI_FLAT = 0`                              | Reserved DWs ignored or zeroed      |


## Fault Behavior

All invalid configurations must return a fault response with:

- Cause = 259 (DDT entry misconfigured)
- Faulting device ID
- The specific field/bit that triggered the violation
