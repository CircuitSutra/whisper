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
#include "MsiPte.hpp"
#include <iostream>
#include <algorithm>

using namespace TT_IOMMU;


bool
Iommu::read(uint64_t addr, unsigned size, uint64_t& data) const
{
  // Size must be 4 or 8. Address must be aligned.
  if (size != 4 and size != 8 and (addr & (size-1)) != 0)
    return false;

  const IommuCsr* csr = findCsrByAddr(addr);
  if (not csr or size > csr->size())
    return false;

  uint64_t offset = addr - addr_;
  if (offset > csr->offset() and size == 8)
    return false;   // Crossing a CSR boundary

  data = csr->read();

  if (offset > csr->offset())
    data >>= 32;   // Reading upper 32 bits of a 64-bit register.

  if (size == 4)
    data = (data << 32) >> 32;   // Clear top 32 bits.

  return true;
}


bool
Iommu::write(uint64_t addr, unsigned size, uint64_t data)
{
  // Size must be 4 or 8. Address must be aligned.
  if (size != 4 and size != 8 and (addr & (size-1)) != 0)
    return false;

  IommuCsr* csr = findCsrByAddr(addr);
  if (not csr or size > csr->size())
    return false;

  uint64_t offset = addr - addr_;
  if (offset > csr->offset() and size == 8)
    return false;   // Crossing a CSR boundary

  if (size == 4)
    data = (data << 32) >> 32;   // Clear upper 32 bits.

  uint64_t value = csr->read();

  if (offset > csr->offset())
    {
      value = (value << 32) >> 32;
      value |= data << 32;  // Writing upper 32 bits of a 64-bit register.
    }
  else
    value = data;

  writeCsr(csr->number(), value);
  return true;
}


void
Iommu::defineCsrs()
{
  using CN = CsrNumber;

  csrs_.resize(size_t(CN::MsiCfgTbl31) + 1);

  uint64_t ones = ~uint64_t(0);       // All ones value.
  uint64_t qbm = 0x003ffffffffffc1f;  // Queue base mask

  //                      define(name, offset, size, reset, mask, rw1c=0, rw1s=0)

  csrAt(CN::Capabilities).define("capabilities", 0,   8, 0, 0,    0,     0);
  csrAt(CN::Fctl)        .define("fctl",         8,   4, 0, ones, 0,     0);
  csrAt(CN::Custom0)     .define("cusotm0",      12,  4, 0, ones, 0,     0);
  csrAt(CN::Ddtp)        .define("ddtp",         16,  8, 0, ones, 0,     0);
  csrAt(CN::Cqb)         .define("cqb",          24,  8, 0, qbm,  0,     0);
  csrAt(CN::Cqh)         .define("cqh",          32,  4, 0, ones, 0,     0);
  csrAt(CN::Cqt)         .define("cqt",          36,  4, 0, ones, 0,     0);
  csrAt(CN::Fqb)         .define("fqb",          40,  8, 0, qbm,  0,     0);
  csrAt(CN::Fqh)         .define("fqh",          48,  4, 0, ones, 0,     0);
  csrAt(CN::Fqt)         .define("fqt",          52,  4, 0, ones, 0,     0);
  csrAt(CN::Pqb)         .define("pqb",          56,  8, 0, qbm,  0,     0);
  csrAt(CN::Pqh)         .define("pqh",          64,  4, 0, ones, 0,     0);
  csrAt(CN::Pqt)         .define("pqt",          68,  4, 0, ones, 0,     0);
  csrAt(CN::Cqcsr)       .define("cqcsr",        72,  4, 0, ones, 0xf00, 0);
  csrAt(CN::Fqcsr)       .define("fqcsr",        76,  4, 0, ones, 0x300, 0);
  csrAt(CN::Pqcsr)       .define("pqcsr",        80,  4, 0, ones, 0x300, 0);
  csrAt(CN::Ipsr)        .define("ipsr",         84,  4, 0, ones, 0xf,   0);
  csrAt(CN::Iocntovf)    .define("iocntovf",     88,  4, 0, ones, 0,     0);
  csrAt(CN::Iocntinh)    .define("iocntinh",     92,  4, 0, ones, 0,     0);
  csrAt(CN::Iohpmcycles) .define("iohpmcycles",  96,  8, 0, ones, 0,     0);

  unsigned offset = 104;
  unsigned size = 8;
  std::string base = "iohpmctr";
  for (unsigned i = 0; i < 31; ++i)
    {
      CN num = CN{unsigned(CN::Iohpmctr1) + i};
      std::string name = base + std::to_string(i+1);
      csrAt(num).define(name, offset + i*size, size, 0, ones, 0, 0);
    }

  offset = 352;
  size = 8;
  base = "iohpmevt";
  for (unsigned i = 0; i < 31; ++i)
    {
      CN num{unsigned(CN::Iohpmevt1) + i};
      std::string name = base + std::to_string(i+1);
      csrAt(num).define(name, offset + i*size, size, 0, ones, 0, 0);
    }

  csrAt(CN::TrReqIova)    .define("tr_req_iova",  600, 8, 0, ones, 0, 0);
  csrAt(CN::TrReqCtl)     .define("tr_req_ctl",   608, 8, 0, ones, 0, 1);
  csrAt(CN::TrResponse)   .define("tr_response",  616, 8, 0, ones, 0, 0);
  csrAt(CN::IommuQosid)   .define("iommu_qosid",  624, 4, 0, ones, 0, 0);
  csrAt(CN::Reserved0)    .define("reserved0",    628, 8, 0, ones, 0, 0);
  csrAt(CN::Reserved1)    .define("reserved1",    636, 8, 0, ones, 0, 0);
  csrAt(CN::Reserved2)    .define("reserved2",    644, 8, 0, ones, 0, 0);
  csrAt(CN::Reserved3)    .define("reserved3",    652, 8, 0, ones, 0, 0);
  csrAt(CN::Reserved4)    .define("reserved4",    660, 8, 0, ones, 0, 0);
  csrAt(CN::Reserved5)    .define("reserved5",    668, 8, 0, ones, 0, 0);
  csrAt(CN::Reserved6)    .define("reserved6",    676, 8, 0, ones, 0, 0);
  csrAt(CN::Reserved7)    .define("rseserve7",    684, 4, 0, ones, 0, 0);
  csrAt(CN::Custom1)      .define("custom1",      688, 8, 0, ones, 0, 0);
  csrAt(CN::Custom1)      .define("custom2",      696, 8, 0, ones, 0, 0);
  csrAt(CN::Custom1)      .define("custom3",      704, 8, 0, ones, 0, 0);
  csrAt(CN::Custom1)      .define("custom4",      712, 8, 0, ones, 0, 0);
  csrAt(CN::Custom1)      .define("custom5",      720, 8, 0, ones, 0, 0);
  csrAt(CN::Custom1)      .define("custom6",      728, 8, 0, ones, 0, 0);
  csrAt(CN::Custom1)      .define("custom7",      736, 8, 0, ones, 0, 0);
  csrAt(CN::Custom1)      .define("custom8",      744, 8, 0, ones, 0, 0);
  csrAt(CN::Custom9)      .define("custom9",      752, 8, 0, ones, 0, 0);

  csrAt(CN::Icvec)        .define("icvec",        760, 8, 0, ones, 0, 0);

  offset = 768;
  size = 8;
  base = "msi_cfg_tbl";
  for (unsigned i = 0; i < 32; ++i, offset += size)
    {
      CN num{unsigned(CN::MsiCfgTbl0) + i};
      std::string name = base + std::to_string(i);
      csrAt(num).define(name, offset, size, 0, ones, 0, 0);
    }

  if (offset > size_)
    {
      std::cerr << "Error: Iommu memory region size (" << size_ << ") is smaller "
                << "than the size required for it CSRs (" << offset << ")\n";
    }

  // Setup wordToCsr_ to map a ward number to a CSR. Assign each CSR a number.
  unsigned number = 0;
  for (auto& csr : csrs_)
    {
      unsigned startWord = csr.offset() / 4;
      unsigned wordCount = csr.size() / 4;

      for (unsigned i = 0; i < wordCount; ++i)
        wordToCsr_.at(startWord + i) = &csr;

      csr.setNumber(CsrNumber{number++});
    }
}


bool
Iommu::loadDeviceContext(unsigned devId, DeviceContext& dc, unsigned& cause)
{
  using CN = CsrNumber;

  deviceDirWalk_.clear();

  cause = 0;
  Capabilities caps(csrAt(CN::Capabilities).read());
  bool extended = caps.bits_.msiFlat_; // Extended or base format
  bool bigEnd = csrAt(CN::Fctl).read() & 1;

  // 1. Identify device tree address and levels.
  Ddtp ddtp(csrAt(CN::Ddtp).read());
  uint64_t addr = ddtp.ppn() * pageSize_;
  unsigned levels = ddtp.levels();
  if (levels == 0)
    return false;
  unsigned ii = levels - 1;

  Devid idFields(devId);

  // 2. If i == 0 go to step 8.
  while (ii != 0)
    {
      // 3. Let ddte be the value of the eight bytes at address a + DDI[i] x 8. If
      //    accessing ddte violates a PMA or PMP check, then stop and report "DDT entry
      //    load access fault" (cause = 257).
      uint64_t ddteVal = 0;
      uint64_t ddteAddr = addr + idFields.ithDdi(ii, extended)*8;
      if (not memReadDouble(ddteAddr, bigEnd, ddteVal))
	{
	  cause = 257;
	  return false;
	}

      deviceDirWalk_.push_back(std::pair<uint64_t, uint64_t>(ddteAddr, ddteVal));

      // PMA and PMP should be covered by the memory read callback.

      // 4. If ddte access detects a data corruption (a.k.a. poisoned data), then stop and
      //    report "DDT data corruption" (cause = 268).
      if (false)
	{
          cause = 268;
          return false;
	}
	
      // 5. If ddte.V == 0, stop and report "DDT entry not valid" (cause = 258).
      Ddte ddte(ddteVal);
      if (ddte.bits_.v_ == 0)
      {
        cause = 258;
        return false;
      }

      // 6. If any bits or encoding that are reserved for future standard use are set
      //    within ddte, stop and report "DDT entry misconfigured" (cause = 259).
      if (ddte.bits_.reserved_ != 0 or ddte.bits_.reserved2_ != 0)
	{
	  cause = 259;
	  return false;
	}

      // 7. Let i = i - 1 and let a = ddte.PPN x pageSize. Go to step 2.
      --ii;
      addr = ddte.bits_.ppn_ * pageSize_;
    }

  // 8. Let DC be the value of DC_SIZE bytes at address a + DDI[0] * DC_SIZE. If
  //    capabilities.MSI_FLAT is 1 then DC_SIZE is 64-bytes else it is 32-bytes. If
  //    accessing DC violates a PMA or PMP check, then stop and report "DDT entry load
  //    access fault" (cause = 257). If DC access detects a data corruption
  //    (a.k.a. poisoned data), then stop and report "DDT data corruption" (cause = 268).
  unsigned dcSize = extended? 64 : 32;
  uint64_t dcAddr = addr + idFields.ithDdi(0, extended) * dcSize;
  unsigned dwordCount = dcSize / 8;  // Double word count.
  std::vector<uint64_t> dcd(dwordCount);  // Device context data.
  for (unsigned i = 0; i < dwordCount; ++i)
    if (not memReadDouble(dcAddr + i*8, bigEnd, dcd.at(i)))
      {
	cause = 257;
	return false;
      }

  // Check for poisoned data. This would require a test-bench API.

  if (dwordCount == 4)
    dc = DeviceContext(dcd.at(0), dcd.at(1), dcd.at(2), dcd.at(3));
  else if (dwordCount == 8)
    dc = DeviceContext(dcd.at(0), dcd.at(1), dcd.at(2), dcd.at(3),
                       dcd.at(4), dcd.at(5), dcd.at(6), dcd.at(7));
  else
    assert(0);

  // 9. If DC.tc.V == 0, stop and report "DDT entry not valid" (cause = 258).
  if (not dc.valid())
    {
      cause = 258;
      return false;
    }

  // 10. If the DC is misconfigured as determined by rules outlined in Section 2.1.4 then
  //     stop and report "DDT entry misconfigured" (cause = 259).
  if (misconfiguredDc(dc))
    {
      cause = 259;
      return false;
    }

  // 11. The device-context has been successfully located.
  return true;
}


bool
Iommu::loadProcessContext(const DeviceContext& dc, uint32_t pid,
			  ProcessContext& pc, unsigned& cause)
{
  cause = 0;
  bool bigEnd = dc.sbe();
  Procid procid(pid);

  processDirWalk_.clear();

  // 1. Let a be pdtp.PPN x pageSize and let i = LEVELS-1. When pdtp.MODE is PD20, LEVELS is
  //    three. When pdtp.MODE is PD17, LEVELS is two. When pdtp.MODE is PD8, LEVELS is
  //    one.
  uint64_t aa = dc.pdtpPpn() * pageSize_;
  unsigned levels = dc.processTableLevels();
  if (levels == 0)
    return false;
  unsigned ii = levels - 1;

  while (true)
    {
      // 2. If DC.iohgatp.mode != Bare, then A (here aa) is a GPA. Invoke the process to
      //    translate A to an SPA as an implicit memory access. If faults occur during
      //    second-stage address translation of a then stop and report the fault detected
      //    by the second-stage address translation process. The translated A is used in
      //    subsequent steps.
      if (dc.iohgatpMode() != IohgatpMode::Bare)
	{
          // FIX double check that the privilege mode is User. Should it be the mode of
          // the initiating IommuRequest?
	  uint64_t pa = 0;
	  if (not stage2Translate(dc.iohgatp(), PrivilegeMode::User,  true, false, false,
                                  aa, pa, cause))
	    return false;
	  aa = pa;
	}

      // 3. If i == 0 go to step 9.
      if (ii == 0)
	break;

      // 4. Let pdte be the value of the eight bytes at address a + PDI[i] x 8. If
      //    accessing pdte violates a PMA or PMP check, then stop and report "PDT entry
      //    load access fault" (cause = 265).
      uint64_t pdte = 0;
      uint64_t pdteAddr = aa + procid.ithPdi(ii) * 8;
      if (not memReadDouble(pdteAddr, bigEnd, pdte))
	{
	  cause = 265;
	  return false;
	}

      processDirWalk_.push_back(std::pair<uint64_t, uint64_t>(pdteAddr, pdte));

      // 5. If pdte access detects a data corruption (a.k.a. poisoned data), then stop and
      //    report "PDT data corruption" (cause = 269).

      // 6. If pdte.V == 0, stop and report "PDT entry not valid" (cause = 266).
      if (Pdte{pdte}.bits_.v_ == 0)
	{
	  cause = 266;
	  return false;
	}

      // 7. If any bits or encoding that are reserved for future standard use are set
      //    within pdte, stop and report "PDT entry misconfigured" (cause = 267).
      uint64_t reserved = pdte & 0xff00'0000'0000'03feLL;
      if (reserved != 0)
	{
	  cause = 267;
	  return false;
	}

      // 8. Let i = i - 1 and let a = pdte.PPN x pageSize. Go to step 2.
      --ii;
      aa = Pdte{pdte}.bits_.ppn_ * pageSize_;
    }

  // 9. Let PC be the value of the 16-bytes at address a + PDI[0] x 16. If accessing PC
  //    violates a PMA or PMP check, then stop and report "PDT entry load access fault"
  //    (cause = 265). If PC access detects a data corruption (a.k.a. poisoned data), then
  //    stop and report "PDT data corruption" (cause = 269).
  uint64_t pca = aa + procid.ithPdi(0)*16;
  if (not readProcessContext(dc, pca, pc))
    {
      cause = 265;
      return false;
    }

  // Check for poisoned data. This would require a test-bench API.
  
  // 10. If PC.ta.V == 0, stop and report "PDT entry not valid" (cause = 266).
  if (not pc.valid())
    {
      cause = 266;
      return false;
    }

  // 11. If the PC is misconfigured as determined by rules outlined in Section 2.2.4 then
  //     stop and report "PDT entry misconfigured" (cause = 267).
  if (misconfiguredPc(pc, dc.sxl()))
    return false;

  // 12. The Process-context has been successfully located.
  return true;
}


bool
Iommu::misconfiguredDc(const DeviceContext& dc) const
{
  using CN = CsrNumber;

  Capabilities caps(csrAt(CN::Capabilities).read());
  bool extended = caps.bits_.msiFlat_; // Extended or base format

  // 1. If any bits or encodings that are reserved for future standard use are set.
  if (dc.nonZeroReservedBits(extended)){
    return true;
  }
    
  
  // 2. capabilities.ATS is 0 and DC.tc.EN_ATS, or DC.tc.EN_PRI, or DC.tc.PRPR is 1
  if (caps.bits_.ats_ == 0 and (dc.ats() or dc.pri() or dc.prpr())){
    return true;
  }
    

  // 3. DC.tc.EN_ATS is 0 and DC.tc.T2GPA is 1
  // 4. DC.tc.EN_ATS is 0 and DC.tc.EN_PRI is 1
  if (not dc.ats() and (dc.t2gpa() or dc.pri())){
    return true;
  }

  // 5. DC.tc.EN_PRI is 0 and DC.tc.PRPR is 1
  if (not dc.pri() and dc.prpr()){
    return true;
  }

  // 6. capabilities.T2GPA is 0 and DC.tc.T2GPA is 1
  if (not caps.bits_.t2gpa_ and dc.t2gpa()){
    return true;
  }

  // 7. DC.tc.T2GPA is 1 and DC.iohgatp.MODE is Bare
  if (dc.t2gpa() and dc.iohgatpMode() == IohgatpMode::Bare){
    return true;
  }

  // 8. DC.tc.PDTV is 1 and DC.fsc.pdtp.MODE is not a supported mode
  //    a. capabilities.PD20 is 0 and DC.fsc.pdtp.MODE is PD20
  //    b. capabilities.PD17 is 0 and DC.fsc.pdtp.MODE is PD17
  //    c. capabilities.PD8 is 0 and DC.fsc.pdtp.MODE is PD8
  if (dc.pdtv())
    {
      auto mode = dc.pdtpMode();
      if (mode != PdtpMode::Bare and mode != PdtpMode::Pd8 and mode != PdtpMode::Pd17 and
          mode != PdtpMode::Pd20){
            return true;
          }
      if (not caps.bits_.pd20_ and mode == PdtpMode::Pd20){
        return true;
      }
      if (not caps.bits_.pd17_ and mode == PdtpMode::Pd17){
        return true;
      }
      if (not caps.bits_.pd8_ and mode == PdtpMode::Pd8){
        return true;
      }
    }

  // 9. DC.tc.PDTV is 0 and DC.fsc.iosatp.MODE encoding is not a valid encoding as
  //    determined by Table 3
  if (not dc.pdtv())
    {
      auto mode = dc.iosatpMode();

      if (dc.sxl())
        {
          if (mode != IosatpMode::Bare and mode != IosatpMode::Sv32){
            return true;
          }
	}
      else
	if (mode != IosatpMode::Bare and mode != IosatpMode::Sv39 and
	    mode != IosatpMode::Sv48 and mode != IosatpMode::Sv57){
        return true;
      }
    }

  // 10. DC.tc.PDTV is 0 and DC.tc.SXL is 0 DC.fsc.iosatp.MODE is not one of the supported
  //     modes
  //     a. capabilities.Sv39 is 0 and DC.fsc.iosatp.MODE is Sv39
  //     b. capabilities.Sv48 is 0 and DC.fsc.iosatp.MODE is Sv48
  //     c. capabilities.Sv57 is 0 and DC.fsc.iosatp.MODE is Sv57
  if (not dc.pdtv() and not dc.sxl())
    {
      auto mode = dc.iosatpMode();
      if (not caps.bits_.sv39_ and mode == IosatpMode::Sv39){
        return true;
      }
       
      if (not caps.bits_.sv48_ and mode == IosatpMode::Sv48){
        return true;
      }
      if (not caps.bits_.sv57_ and mode == IosatpMode::Sv57){
        return true;
      }
    }

  // 11. DC.tc.PDTV is 0 and DC.tc.SXL is 1 DC.fsc.iosatp.MODE is not one of the supported
  //     modes
  //     a. capabilities.Sv32 is 0 and DC.fsc.iosatp.MODE is Sv32
  if (not dc.pdtv() and dc.sxl())
    {
      auto mode = dc.iosatpMode();

      if (not caps.bits_.sv32_ and mode == IosatpMode::Sv32){
        return true;
      }
    }

  // 12. DC.tc.PDTV is 0 and DC.tc.DPE is 1
  if (not dc.pdtv() and dc.dpe()){
    return true;
  }

  // 13. DC.iohgatp.MODE encoding is not a valid encoding as determined by Table 2
  auto gmode = dc.iohgatpMode();

  // Check valid IOHGATP modes based on fctl.GXL
  Fctl fctl(csrAt(CN::Fctl).read());
  if (fctl.bits_.gxl_)
    {
      // When GXL=1, only Bare and Sv32x4 are valid
      if (gmode != IohgatpMode::Bare && gmode != IohgatpMode::Sv32x4){
        return true;
      }
    }
  else
    {
      // When GXL=0, only Bare, Sv39x4, Sv48x4, and Sv57x4 are valid
      if (gmode != IohgatpMode::Bare && gmode != IohgatpMode::Sv39x4 && 
          gmode != IohgatpMode::Sv48x4 && gmode != IohgatpMode::Sv57x4){
        return true;
      }
    }

  // 14. fctl.GXL is 0 and DC.iohgatp.MODE is not a supported mode
  //     a. capabilities.Sv39x4 is 0 and DC.iohgatp.MODE is Sv39x4
  //     b. capabilities.Sv48x4 is 0 and DC.iohgatp.MODE is Sv48x4
  //     c. capabilities.Sv57x4 is 0 and DC.iohgatp.MODE is Sv57x4
  // Fctl fctl(csrAt(CN::Fctl).read());
  if (not fctl.bits_.gxl_)
    {
      if (not caps.bits_.sv39x4_ and gmode == IohgatpMode::Sv39x4){
        return true;
      }
      if (not caps.bits_.sv48x4_ and gmode == IohgatpMode::Sv48x4){
        return true;
      }
      if (not caps.bits_.sv57x4_ and gmode == IohgatpMode::Sv57x4){
        return true;
      }
    }

  // 15. fctl.GXL is 1 and DC.iohgatp.MODE is not a supported mode
  //     a. capabilities.Sv32x4 is 0 and DC.iohgatp.MODE is Sv32x4
  if (fctl.bits_.gxl_)
    if (not caps.bits_.sv32x4_ and gmode == IohgatpMode::Sv32x4){
      return true;
    }

  // 16. capabilities.MSI_FLAT is 1 and DC.msiptp.MODE is not Off and not Flat
  bool msiFlat = caps.bits_.msiFlat_;
  if (msiFlat)
    {
      auto msiMode = dc.msiMode();
      if (msiMode != MsiptpMode::Off and msiMode != MsiptpMode::Flat){
        return true;
    }

  // 17. DC.iohgatp.MODE is not Bare and the root page table determined by DC.iohgatp.PPN
  // is not aligned to a 16-KiB boundary.
  if (gmode != IohgatpMode::Bare and (dc.iohgatpPpn() & 0x3) != 0)
    return true;
  }

  // 18. capabilities.AMO_HWAD is 0 and DC.tc.SADE or DC.tc.GADE is 1
  if (not caps.bits_.amoHwad_ and (dc.sade() or dc.gade())){
    return true;
  }

  // 19. capabilities.END is 0 and fctl.BE != DC.tc.SBE
  if (not caps.bits_.end_ and fctl.bits_.be_ != dc.sbe()){
    return true;
  }

  // 20. DC.tc.SXL value is not a legal value. If fctl.GXL is 1, then DC.tc.SXL must be
  // 1. If fctl.GXL is 0 and is writable, then DC.tc.SXL may be 0 or 1. If fctl.GXL is 0
  // and is not writable then DC.tc.SXL must be 0.
  if (fctl.bits_.gxl_ and not dc.sxl()){
    return true;
  }
  uint32_t fctlMask = csrAt(CN::Fctl).mask();
  if (not fctl.bits_.gxl_) {
    if (not Fctl{fctlMask}.bits_.gxl_)  // fctl.GXL not writeable
      if (dc.sxl() != 0){
        return true;
      }
  }

  // 21. DC.tc.SBE value is not a legal value. If fctl.BE is writable then DC.tc.SBE may
  // be 0 or 1. If fctl.BE is not writable then DC.tc.SBE must be the same as fctl.BE.
  if (not Fctl{fctlMask}.bits_.be_)   // fctl.BE not writable
    if (dc.sbe() != fctl.bits_.be_)
      return true;

  return false;
}


bool
Iommu::misconfiguredPc(const ProcessContext& pc, bool sxl) const
{
  // 1. If any bits or encoding that are reserved for future standard use are set
  if (pc.nonZeroReservedBits())
    return true;

  // 2. PC.fsc.MODE encoding is not valid as determined by Table 3
  auto mode = pc.iosatpMode();
  if (sxl)
    {
      if (mode != IosatpMode::Bare and mode != IosatpMode::Sv39)
	return true;
    }
  else
    if (mode != IosatpMode::Bare and mode != IosatpMode::Sv39 and
	mode != IosatpMode::Sv48 and mode != IosatpMode::Sv57)
      return true;

  
  // 3. DC.tc.SXL is 0 and PC.fsc.MODE is not one of the supported modes
  //    a. capabilities.Sv39 is 0 and PC.fsc.MODE is Sv39
  //    b. capabilities.Sv48 is 0 and PC.fsc.MODE is Sv48
  //    c. capabilities.Sv57 is 0 and PC.fsc.MODE is Sv57
  using CN = CsrNumber;
  Capabilities caps(csrAt(CN::Capabilities).read());
  if (not sxl)
    {
      if (not caps.bits_.sv39_ and mode == IosatpMode::Sv39)
	return true;
      if (not caps.bits_.sv48_ and mode == IosatpMode::Sv48)
	return true;
      if (not caps.bits_.sv57_ and mode == IosatpMode::Sv57)
	return true;
    }

  // 4. DC.tc.SXL is 1 and PC.fsc.MODE is not one of the supported modes
  //    a. capabilities.Sv32 is 0 and PC.fsc.MODE is Sv32
  if (sxl)
    if (not caps.bits_.sv32_ and mode == IosatpMode::Sv32)
      return true;

  return false;
}


bool
Iommu::translate(const IommuRequest& req, uint64_t& pa, unsigned& cause)
{
  cause = 0;

  bool repFault = true;  // Should fault be reported?

  if (translate_(req, pa, cause, repFault))
    return true;

  if (repFault)
    {
      FaultRecord record;
      record.cause = cause;
      record.ttyp = unsigned(req.type);

      if (req.type != Ttype::None)    // TTYP != 0
        {   // Section 4.2.
          record.did = req.devId;
          record.pv = req.hasProcId;   // Process id valid.
          if (record.pv)
            {
              record.pid = req.procId;
              record.priv = req.privMode == PrivilegeMode::Supervisor? 1 : 0;
            }
        }

      if (req.isMessage())
        {
          assert(0 && "PCIE message requests not yet supported");
        }
      else if (req.type == Ttype::TransExec or req.type == Ttype::TransRead
               or req.type == Ttype::TransWrite)
        {
          record.iotval = req.iova;
          if (cause == 21 or cause == 22 or cause == 23)   // Guest page fault
            {
              uint64_t gpa = 0;
              bool implicit = false, write = false;
              stage2TrapInfo_(gpa, implicit, write);
              uint64_t iotval2 = (gpa >> 2) << 2;  // Clear least sig 2 bits.
              if (implicit)
                {
                  iotval2 |= 1;     // Set bit 0
                  if (write)
                    iotval2 |= 2;   // Set bit 1
                }
              record.iotval2 = iotval2;
            }
        }
      else if (req.type == Ttype::None)
        {
          // Spec says that the values of iotval and iotval2 are "as defined by the CAUSE".
          // Spec does not say how the CAUSE defineds the values.
          assert(0);
        }

      if (not queueFull(CsrNumber::Fqb, CsrNumber::Fqh, CsrNumber::Fqt))
        {
          writeFaultRecord(record);

          // Signal interrupt pending in Ipsr.
          Ipsr ipsr{uint32_t(readCsr(CsrNumber::Ipsr))};
          ipsr.bits_.fip_ = 1;  // Fault queue interrupt pending.
          pokeCsr(CsrNumber::Ipsr, ipsr.value_);
        }
      else
        {
          Fqcsr fqcsr{uint32_t(readCsr(CsrNumber::Fqcsr))};
          fqcsr.bits_.fqof_ = 1;
          pokeCsr(CsrNumber::Fqcsr, fqcsr.value_);
        }
    }

  return false;
}


bool
Iommu::readForDevice(const IommuRequest& req, uint64_t& data, unsigned& cause)
{
  deviceDirWalk_.clear();
  processDirWalk_.clear();

  cause = 0;
  if (not req.isRead())
    return false;  // Request misconfigured.

  uint64_t pa = 0;
  if (not translate(req,  pa, cause))
    return false;

  // FIX Should we consider device endianness?
  return memRead_(pa, req.size, data);
}


bool
Iommu::writeForDevice(const IommuRequest& req, uint64_t data, unsigned& cause)
{
  deviceDirWalk_.clear();
  processDirWalk_.clear();

  cause = 0;
  if (not req.isWrite())
    return false;  // Request misconfigured.

  uint64_t pa = 0;
  if (not translate(req,  pa, cause))
    return false;

  // FIX Should we consider device endianness?
  return memWrite_(pa, req.size, data);
}


bool
Iommu::translate_(const IommuRequest& req, uint64_t& pa, unsigned& cause, bool& repFault)
{
  deviceDirWalk_.clear();
  processDirWalk_.clear();

  cause = 0;

  // By default all faults are reported (assume DTF is 0 until we determine DTF).
  // Section 4.2. of spec.
  repFault = true;

  unsigned processId = req.procId;

  // 1.  If ddtp.iommu_mode == Off then stop and report "All inbound
  // transactions disallowed" (cause = 256).
  using CN = CsrNumber;
  Ddtp ddtp{csrAt(CN::Ddtp).read()};
  if (ddtp.mode() == Ddtp::Mode::Off)
    {
      cause = 256;
      return false;
    }

  // 2. If ddtp.iommu_mode == Bare and any of the following conditions hold then stop and
  //    report "Transaction type disallowed" (cause = 260); else go to step 20 with
  //    translated address same as the IOVA.
  //    a. Transaction type is a Translated request (read, write/AMO, read-for-execute) or
  //       is a PCIe ATS Translation request.
  Capabilities caps(csrAt(CN::Capabilities).read());
  if (ddtp.mode() == Ddtp::Mode::Bare)
    {
      if (req.isTranslated() or req.isAts())
	{
	  cause = 260;
	  return false;
	}
      pa = req.iova;
      return true;
    }

  // 3. If capabilities.MSI_FLAT is 0 then the IOMMU uses base-format device context. Let
  //    DDI[0] be device_id[6:0], DDI[1] be device_id[15:7], and DDI[2] be
  //    device_id[23:16].
  bool extended = caps.bits_.msiFlat_; // Extended or base format

  // 4. If capabilities.MSI_FLAT is 1 then the IOMMU uses extended-format device
  //    context. Let DDI[0] be device_id[5:0], DDI[1] be device_id[14:6], and DDI[2] be
  //    device_id[23:15].
  Devid devid(req.devId);
  unsigned ddi1 = devid.ithDdi(1, extended);
  unsigned ddi2 = devid.ithDdi(2, extended);

  // 5. If the device_id is wider than that supported by the IOMMU mode, as determined by
  //    the following checks then stop and report "Transaction type disallowed" (cause =
  //    260).
  //    a. ddtp.iommu_mode is 2LVL and DDI[2] is not 0
  //    b. ddtp.iommu_mode is 1LVL and either DDI[2] is not 0 or DDI[1] is not 0
  Ddtp::Mode ddtpMode = ddtp.mode();
  if ((ddtpMode == Ddtp::Mode::Level2 and ddi2 != 0) or
      (ddtpMode == Ddtp::Mode::Level1 and (ddi2 != 0 or ddi1 != 0)))
    {
      cause = 260;
      return false;
    }
  // 6. Use device_id to then locate the device-context (DC) as specified in Section
  //    2.3.1.
  DeviceContext dc;
  if (not loadDeviceContext(req.devId, dc, cause))
    return false;

  bool dtf = dc.dtf();  // Disable translation fault reporting.
  // 7. If any of the following conditions hold then stop and report "Transaction type
  //    disallowed" (cause = 260).
  //    a. Transaction type is a Translated request (read, write/AMO, read-for-execute) or
  //       is a PCIe ATS Translation request and DC.tc.EN_ATS is 0.
  //    b. Transaction has a valid process_id and DC.tc.PDTV is 0.
  //    c. Transaction has a valid process_id and DC.tc.PDTV is 1 and the process_id is
  //       wider than that supported by pdtp.MODE.
  //    d. Transaction type is not supported by the IOMMU.
  if (((req.isTranslated() or req.isAts()) and not dc.ats()) or  // a
      (req.hasProcId and not dc.pdtv()))                 // b
    {
      repFault = not dtf;   // Sec 4.2, table 11.
      cause = 260;
      return false;
    }
  if (req.hasProcId and dc.pdtv())                       // c
    {
      Procid procid(req.procId);
      unsigned pdi1 = procid.ithPdi(1);
      unsigned pdi2 = procid.ithPdi(2);
      Pdtp pdtp(dc.pdtp());
      PdtpMode pdtpMode = pdtp.mode();
      if ((pdtpMode == PdtpMode::Pd17 and pdi2 != 0) or
	  (pdtpMode == PdtpMode::Pd8 and (pdi2 != 0 or pdi1 != 0)))
	{
          repFault = not dtf;   // Sec 4.2, table 11.
	  cause = 260;
	  return false;
	}
    }
  // 8. If request is a Translated request and DC.tc.T2GPA is 0 then the translation
  //    process is complete. Go to step 20.
  if (req.isTranslated() and not dc.t2gpa())
    {
      pa = req.iova;  // Not explicitly in the spec. Implied.
      return true;
    }

  unsigned pscid = dc.pscid();
  bool sum = false;  // Supervisor has access to user pages.

  // 9. If request is a Translated request and DC.tc.T2GPA is 1 then the IOVA is a GPA. Go
  //    to step 17 with following page table information:
  //    a. Let A be the IOVA (the IOVA is a GPA).
  //    b. Let iosatp.MODE be Bare
  //       i. The PSCID value is not used when first-stage is Bare.
  //    c. Let iohgatp be the value in the DC.iohgatp field
  uint64_t iohgatp = dc.iohgatp();
  uint64_t iosatp = not dc.pdtv() ? dc.iosatp() : uint64_t(IosatpMode::Bare) << 60;
  if (req.isTranslated() and dc.t2gpa())
    pscid = 0;
  else if (not dc.pdtv())
    {
      // 10. If DC.tc.PDTV is set to 0 then go to step 17 with the following page table
      //     information:
      //     a. Let iosatp.MODE be the value in the DC.fsc.MODE field
      //     b. Let iosatp.PPN be the value in the DC.fsc.PPN field
      //     c. Let PSCID be the value in the DC.ta.PSCID field
      //     d. Let iohgatp be the value in the DC.iohgatp field
      pscid = dc.pscid();
    }
  else
    {
      // 11. If DPE is 1 and there is no process_id associated with the transaction then
      //     let process_id be the default value of 0.
      if (dc.dpe() and not req.hasProcId)
	processId = 0;

      // 12. If DPE is 0 and there is no process_id associated with the transaction then
      //     then go to step 17 with the following page table information:
      //     a. Let iosatp.MODE be Bare
      //        i. The PSCID value is not used when first-stage is Bare.
      //     b. Let iohgatp be the value in the DC.iohgatp field
      if (not dc.dpe() and not req.hasProcId)
	{
	  iosatp = uint64_t(IosatpMode::Bare) << 60;
	  pscid = 0;
	}
      else
	{
	  // 13. If DC.fsc.pdtp.MODE = Bare then go to step 17 with the following page
	  //     table information:
	  //     a. Let iosatp.MODE be Bare
	  //        i. The PSCID value is not used when first-stage is Bare.
	  //     b. Let iohgatp be value in DC.iohgatp field
	  if (dc.pdtpMode() == PdtpMode::Bare)
	    {
	      iosatp = uint64_t(IosatpMode::Bare) << 60;
	      pscid = 0;
	    }
	  else
	    {
	      // 14. Locate the process-context (PC) as specified in Section 2.3.2.
	      ProcessContext pc;
	      if (not loadProcessContext(dc, processId, pc, cause))
                {
                  // All causes produced by load-process-context are subject to DC.DTF.
                  repFault = not dc.dtf();  // Sec 4.2, table 11.
                  return false;
                }

	      // 15. if any of the following conditions hold then stop and report
	      //     "Transaction type disallowed" (cause = 260).
	      //     a. The transaction requests supervisor privilege but PC.ta.ENS is not
	      //        set.
	      if (req.privMode == PrivilegeMode::Supervisor and not pc.ens())
		{
                  repFault = not dtf;   // Sec 4.2, table 11.                  
		  cause = 260;
		  return false;
		}

	      // 16. Go to step 17 with the following page table information:
	      //     a. Let iosatp.MODE be the value in the PC.fsc.MODE field
	      //     b. Let iosatp.PPN be the value in the PC.fsc.PPN field
	      //     c. Let PSCID be the value in the PC.ta.PSCID field
	      //     d. Let iohgatp be the value in the DC.iohgatp field
              iosatp = pc.fsc();
	      pscid = pc.pscid();
              sum = pc.sum();
	    }
	}
    }
  // 17. Use the process specified in Section "Two-Stage Address Translation" of the
  //     RISC-V Privileged specification [3] to determine the GPA accessed by the
  //     transaction. If a fault is detected by the first stage address translation
  //     process then stop and report the fault. If the translation process is completed
  //     successfully then let A be the translated GPA.
  uint64_t gpa = req.iova;
  if (not stage1Translate(iosatp, iohgatp, req.privMode, pscid, req.isRead(), req.isWrite(),
                          req.isExec(), sum, req.iova, gpa, cause))
    {
      repFault = not dtf;   // Sec 4.2, table 11. Cause range is 1 to 23.
      return false;
    }

  // 18. If MSI address translations using MSI page tables is enabled (i.e.,
  //     DC.msiptp.MODE != Off) then the MSI address translation process specified in
  //     Section 2.3.3 is invoked. If the GPA A is not determined to be the address of a
  //     virtual interrupt file then the process continues at step 19. If a fault is
  //     detected by the MSI address translation process then stop and report the fault
  //     else the process continues at step 20.s
  if (extended and dc.msiMode() != MsiptpMode::Off)
    {
      bool isMrif = false;
      uint64_t mrif = 0;
      uint64_t nppn = 0;
      unsigned nid = 0;
      if (msiTranslate(dc, req, gpa, pa, isMrif, mrif, nppn, nid, cause))
        return true;  // A is address of virtual file and MSI translation successful
      if (cause != 0)
        {
          // All causes produced by MSI translate are subject to DC.DTF.
          repFault = not dtf;  // Sec 4.2, table 11.
          return false;  // A is address of virtual file and MSI translation failed
        }
    }

  // 19. Use the second-stage address translation process specified in Section "Two-Stage
  //     Address Translation" of the RISC-V Privileged specification [3] to translate the
  //     GPA A to determine the SPA accessed by the transaction. If a fault is detected by
  //     the address translation process then stop and report the fault.
  if (not stage2Translate(iohgatp, req.privMode, req.isRead(), req.isWrite(),
                          req.isExec(), gpa, pa, cause))
    {
      repFault = not dtf;   // Sec 4.2, table 11. Cause range is 1 to 23.
      return false;
    }

  // 20. Translation process is complete
  return true;
}


bool
Iommu::msiTranslate(const DeviceContext& dc, const IommuRequest& req,
		    uint64_t gpa, uint64_t& pa, bool& isMrif, uint64_t& mrif,
                    uint64_t& nnpn, unsigned& nid, unsigned& cause)
{
  if (not isDcExtended())
    return false;

  cause = 0;

  bool bigEnd = csrAt(CsrNumber::Fctl).read() & 1;

  // 1. Let A be the GPA
  uint64_t aa = gpa;

  // 2. Let DC be the device-context located using the device_id of the device using the
  //     process outlined in Section 2.3.1.

  // Step 2 is already done by the caller.

  // 3. Determine if the address A is an access to a virtual interrupt file as specified
  //    in Section 3.1.3.6.
  if (not dc.isMsiAddress(gpa))
    return false;   // MSI translation does not apply.
  
  // 4. If the address is not determined to be that of a virtual interrupt file then stop
  //    this process and instead use the regular translation data structures to do the
  //    address translation.

  // Step 4 will be handled by the caller when they see cause == 0.

  // 5. Extract an interrupt file number I from A as I = extract(A >> 12,
  //    DC.msi_addr_mask). The bit extract function extract(x, y) discards all bits from x
  //    whose matching bits in the same positions in the mask y are zeros, and packs the
  //    remaining bits from x contiguously at the least- significant end of the result,
  //    keeping the same bit order as x and filling any other bits at the most-significant
  //    end of the result with zeros. For example, if the bits of x and y are:
  //      x = abcdefgh
  //      y = 10100110
  //    then the value of extract(x, y) has bits 0000acfg.
  uint64_t ii = dc.extractMsiBits(aa >> 12, dc.msiMask());

  // 6. Let m be (DC.msiptp.PPN x pageSize).
  uint64_t mm = dc.msiPpn() * pageSize_;

  // 7. Let msipte be the value of sixteen bytes at address (m | (I x 16)).  If accessing
  //    msipte violates a PMA or PMP check, then stop and report "MSI PTE load access
  //    fault" (cause = 261).
  uint64_t pteAddr = mm | (ii * 16);
  uint64_t pte0 = 0, pte1 = 0;
  if (not memReadDouble(pteAddr, bigEnd, pte0) or not memReadDouble(pteAddr+8, bigEnd, pte1))
    {
      cause = 261;
      return false;
    }

  // 8. If msipte access detects a data corruption (a.k.a. poisoned data), then stop and
  //    report "MSI PT data corruption" (cause = 270).
  MsiPte0 msiPte0(pte0);
  
  // 9. If msipte.V == 0, then stop and report "MSI PTE not valid" (cause = 262).
  if (not msiPte0.bits_.v_)
    {
      cause = 262;
      return false;
    }

  // 10. If msipte.C == 1, then further processing to interpret the PTE is implementation
  //     defined.
  if (msiPte0.bits_.c_)
    {
      cause = 263;
      return false;
    }

  // 11. If msipte.C == 0 then the process is outlined in subsequent steps.

  // 12. If msipte.M == 0 or msipte.M == 2, then stop and report "MSI PTE misconfigured"
  //     (cause = 263).
  if (msiPte0.bits_.m_ == 0 or msiPte0.bits_.m_ == 2)
    {
      cause = 263;
      return false;
    }

  // 13. If msipte.M == 3 the PTE is in basic translate mode and the translation process
  //     is as follows:
  //     a. If any bits or encoding that are reserved for future standard use are set
  //        within msipte, stop and report "MSI PTE misconfigured" (cause = 263).
  //     b. Compute the translated address as msipte.PPN << 12 | A[11:0].
  if (msiPte0.bits_.m_ == 3)
    {
      if (msiPte0.bits_.rsrv0_ or msiPte0.bits_.rsrv1_ or pte1)
	{
	  cause = 263;
	  return false;
	}
      pa = (msiPte0.bits_.ppn_ << 12) | (aa & 0xfff);
    }

  // 14. If msipte.M == 1 the PTE is in MRIF mode and the translation process is as
  //     follows:
  //     a. If capabilities.MSI_MRIF == 0, stop and report "MSI PTE misconfigured" (cause
  //        = 263).
  //     b. If any bits or encoding that are reserved for future standard use are set
  //        within msipte, stop and report "MSI PTE misconfigured" (cause = 263).
  //     c. The address of the destination MRIF is msipte.MRIF_Address[55:9] * 512.
  //     d. The destination address of the notice MSI is msipte.NPPN << 12.  e. Let NID be
  //        (msipte.N10 << 10) | msipte.N[9:0]. The data value for notice MSI is the
  //        11-bit NID value zero-extended to 32-bits.
  //     e. Let NID be (msipte.N10 << 10) | msipte.N[9:0]. The data value for notice MSI
  //        is the 11-bit NID value zero-extended to 32-bits.
  if (msiPte0.bits_.m_ == 1)
    {
      Capabilities caps(csrAt(CsrNumber::Capabilities).read());

      if (caps.bits_.msiMrif_ == 0)    // a.
	{
	  cause = 263;
	  return false;
	}

      MsiMrifPte0 mpte0(pte0);    // b.
      MsiMrifPte1 mpte1(pte1);
      if (mpte0.bits_.reserved0_ or mpte0.bits_.reserved1_ or
          mpte1.bits_.reserved0_ or mpte1.bits_.reserved1_)
	{
	  cause = 263;
	  return false;
	}
      
      mrif = mpte0.bits_.addr_ * 512;  // c.
      nnpn = mpte1.bits_.nppn_ << 12;  // d.
      nid = (mpte1.bits_.nidh_ << 10) | (mpte1.bits_.nidl_);  // e.
      isMrif = true;
    }

  // 15. The access permissions associated with the translation determined through this
  //     process are equivalent to that of a regular RISC-V second-stage PTE with R=W=U=1
  //     and X=0. Similar to a second-stage PTE, when checking the U bit, the transaction
  //     is treated as not requesting supervisor privilege.
  //     a. If the transaction is an Untranslated or Translated read-for-execute then stop
  //        and report "Instruction access fault" (cause = 1).
  if (req.isExec())
    {
      cause = 1;
      return false;
    }

  // 16. MSI address translation process is complete.
  return true;
}


bool
Iommu::stage1Translate(uint64_t satpVal, uint64_t hgatpVal, PrivilegeMode pm, unsigned procId,
                       bool r, bool w, bool x, bool sum,
                       uint64_t va, uint64_t& gpa, unsigned& cause)
{
  Iosatp satp{satpVal};
  unsigned privMode = unsigned(pm);
  unsigned transMode = unsigned(satp.bits_.mode_);   // Sv39, Sv48, ...
  uint64_t ppn = satp.bits_.ppn_;
  stage1Config_(transMode, procId, ppn, sum);

  Iohgatp hgatp{hgatpVal};
  transMode = unsigned(hgatp.bits_.mode_);  // Sv39x4, Sv48x4, ...
  unsigned gcsid = hgatp.bits_.gcsid_;
  ppn = hgatp.bits_.ppn_;
  stage2Config_(transMode, gcsid, ppn);

  return stage1_(va, privMode, r, w, x, gpa, cause);
}


bool
Iommu::stage2Translate(uint64_t hgatpVal, PrivilegeMode pm, bool r, bool w, bool x,
                       uint64_t gpa, uint64_t& pa, unsigned& cause)
{
  Iohgatp hgatp{hgatpVal};

  unsigned privMode = unsigned(pm);
  unsigned transMode = unsigned(hgatp.bits_.mode_);  // Sv39x4, Sv48x4, ...
  unsigned gcsid = hgatp.bits_.gcsid_;
  uint64_t ppn = hgatp.bits_.ppn_;

  stage2Config_(transMode, gcsid, ppn);
  return stage2_(gpa, privMode, r, w, x, pa, cause);
}


void 
Iommu::configureCapabilities(uint64_t value)
{
  csrAt(CsrNumber::Capabilities).configureReset(value);
}


void 
Iommu::reset()
{
  for (auto& csr : csrs_)
    csr.reset();

  uint64_t caps = readCsr(CsrNumber::Capabilities);
  bigEnd_ = Capabilities{caps}.bits_.end_;

  applyCapabilityRestrictions();
}


void
Iommu::applyCapabilityRestrictions()
{
    using CN = CsrNumber;

    uint64_t capabilities = csrAt(CN::Capabilities).read();
    Capabilities caps(capabilities);

    // If capabilities.ATS == 0, set pqb, pqh, pqt, and pqcsr to 0
    if (caps.bits_.ats_ == 0) {
        csrAt(CN::Pqb).configureMask(0);
        csrAt(CN::Pqh).configureMask(0);
        csrAt(CN::Pqt).configureMask(0);
        csrAt(CN::Pqcsr).configureMask(0);
    }

    // If capabilities.HPM == 0, set iocountovf, iocountinh, iohpmcycles, iohpmctr1-31, iohpmevt1-31 to 0
    if (caps.bits_.hmp_ == 0) {
        csrAt(CN::Iocntovf).configureMask(0);
        csrAt(CN::Iocntinh).configureMask(0);
        csrAt(CN::Iohpmcycles).configureMask(0);
        for (unsigned i = 0; i < 31; ++i) {
            csrAt(static_cast<CN>(static_cast<uint32_t>(CN::Iohpmctr1) + i)).configureMask(0);
            csrAt(static_cast<CN>(static_cast<uint32_t>(CN::Iohpmevt1) + i)).configureMask(0);
        }
    }

    // If capabilities.DBG == 0, set tr_req_iova, tr_req_ctl, and tr_response to 0
    if (caps.bits_.debug_ == 0) {
        csrAt(CN::TrReqIova).configureMask(0);
        csrAt(CN::TrReqCtl).configureMask(0);
        csrAt(CN::TrResponse).configureMask(0);
    }

    // If capabilities.QOSID == 0, set iommu_qosid to 0
    if (caps.bits_.qosid_ == 0) {
        csrAt(CN::IommuQosid).configureMask(0);
    }

    // If capabilities.IGS == WSI, set msi_cfg_tbl to 0
    Fctl fctl(csrAt(CN::Fctl).read());
    if (caps.bits_.igs_ == fctl.bits_.wsi_) {
      for (unsigned i = 0; i < 32; ++i) {
          csrAt(static_cast<CN>(static_cast<uint32_t>(CN::MsiCfgTbl0) + i)).configureMask(0);
      }
    }
}


uint64_t
Iommu::queueCapacity(CsrNumber qbn) const
{
  using CN = CsrNumber;

  assert(qbn == CN::Cqb or qbn == CN::Fqb or qbn == CN::Pqb);

  uint64_t value = csrs_.at(unsigned(qbn)).read();
  Qbase qbase(value);
  uint64_t cap = qbase.bits_.logszm1_;
  cap = uint64_t(1) << (cap + 1);
  return cap;
}


uint64_t
Iommu::queueAddress(CsrNumber qbn) const
{
  using CN = CsrNumber;

  assert(qbn == CN::Cqb or qbn == CN::Fqb or qbn == CN::Pqb);

  uint64_t value = csrs_.at(unsigned(qbn)).read();
  Qbase qbase(value);
  uint64_t addr = qbase.bits_.ppn_;
  addr = addr * 4096;
  return addr;
}


bool
Iommu::queueFull(CsrNumber qbn, CsrNumber qhn, CsrNumber qtn) const
{
  using CN = CsrNumber;

  if (qbn == CN::Cqb)
    assert(qhn == CN::Cqh and qtn == CN::Cqt);
  else if (qbn == CN::Fqb)
    assert(qhn == CN::Fqh and qtn == CN::Fqt);
  else if (qbn == CN::Pqb)
    assert(qhn == CN::Pqh and qtn == CN::Pqt);
  else
    assert(0);

  uint64_t head = readCsr(qhn);
  uint64_t tail = readCsr(qtn);
  uint64_t cap = queueCapacity(qbn);
  
  uint64_t nextTail = (tail + 1) % cap;
  return nextTail == head;
}


bool
Iommu::queueEmpty(CsrNumber qbn, CsrNumber qhn, CsrNumber qtn) const
{
  using CN = CsrNumber;

  if (qbn == CN::Cqb)
    assert(qhn == CN::Cqh and qtn == CN::Cqt);
  else if (qbn == CN::Fqb)
    assert(qhn == CN::Fqh and qtn == CN::Fqt);
  else if (qbn == CN::Pqb)
    assert(qhn == CN::Pqh and qtn == CN::Pqt);
  else
    assert(0);

  uint64_t head = csrs_.at(unsigned(qhn)).read();
  uint64_t tail = csrs_.at(unsigned(qtn)).read();

  return head == tail;
}


void
Iommu::pokeCsr(CsrNumber csrn, uint64_t data)
{
  using CN = CsrNumber;

  auto& csr = csrs_.at(unsigned(csrn));

  if (csrn == CN::Ipsr)
    {
      pokeIpsr(data);
      return;
    }

  if (csrn == CN::Fqcsr)
    {
      csr.poke(data);
      uint32_t next = readCsr(CN::Fqcsr);
      Fqcsr nf{next};
      if (nf.bits_.fqof_ or nf.bits_.fqmf_)
        {
          Ipsr ipsr{uint32_t(readCsr(CN::Ipsr))};
          ipsr.bits_.fip_ = 1;
          pokeIpsr(ipsr.value_);
        }
      return;
    }

  csr.poke(data);
}


void
Iommu::pokeIpsr(uint64_t data)
{
  using CN = CsrNumber;

  auto& csr = csrs_.at(unsigned(CN::Ipsr));

  uint32_t next = data;
  uint32_t prev = csr.read();
  if (next == prev)
    return;

  csr.poke(next);
  next = csr.read();

  Ipsr pi{prev};
  Ipsr ni{next};

  // FIX. Check all interrupt bits.
  if (pi.bits_.fip_ == 0 and ni.bits_.fip_ == 1)
    {
      // FIP bit transitioned from 0 to 1, deliver interrupt.

      if (wiredInterrupts())
        {
          // Wired interrupts. FIX : Need a callback for this to use APLIC.
          std::cerr << "FIX Iommu::pokeIpsr: Need callback for wired deivery.\n";
        }
      else
        {
          // Get interrupt offset for fault queue interrupt from Icvec.
          unsigned vector = Icvec{ readCsr(CN::Icvec) }.bits_.fiv_;

          // Read the corresponding data in MsiCfgTbl. Two 4-byte CSRs for addr, one for
          // data, and one for control. See section 6.29 of IOMMU spec.
          unsigned ix = unsigned(CN::MsiCfgTbl0) + vector;
          uint32_t addr0 = readCsr(CN{ix});
          uint32_t addr1 = readCsr(CN{ix+1});
          uint64_t addr = (uint64_t(addr1) << 32) | addr0;
          uint32_t data = readCsr(CN{ix+2});
          uint32_t control = readCsr(CN{ix+3});

          assert(ix+3 <= unsigned(CN::MsiCfgTbl31));

          if ((control & 1) == 0)
            return;  // Interrupt is currently masked.

          if (not memWrite(addr, sizeof(data), false /*bigEnd*/, data))
            {
              if (not queueFull(CN::Fqb, CN::Fqh, CN::Fqt))
                {
                  FaultRecord record;
                  record.cause = 273;
                  record.ttyp = unsigned(Ttype::None);
                  writeFaultRecord(record);
                }
            }
        }
    }
}


void
Iommu::writeIpsr(uint64_t data)
{
  using CN = CsrNumber;

  auto& csr = csrs_.at(unsigned(CN::Ipsr));
  uint32_t prev = csr.read();

  csr.write(data);
  uint32_t next = csr.read();

  if (next == prev)
    return;

  Ipsr prevFields{prev};  // Prev Ipsr fields
  Ipsr nextFields{next};  // Next Ipsr fields

  // FIX. Check all interrupt bits.
  if (prevFields.bits_.fip_ == 1 and nextFields.bits_.fip_ == 0)
    {
      // Transitioned from 1 to 0. Check FQCSR and transition back if necessary.
      uint32_t fqVal = readCsr(CN::Fqcsr);
      Fqcsr fq{fqVal};
      if (fq.bits_.fqof_ or fq.bits_.fqmf_)
        {
          nextFields.bits_.fip_ = 1;
          pokeIpsr(nextFields.value_);
        }
    }
}


void
Iommu::writeFaultRecord(const FaultRecord& record)
{
  using CN = CsrNumber;

  if (queueFull(CsrNumber::Fqb, CsrNumber::Fqh, CsrNumber::Fqt))
    assert(0);

  // Add fault record at tail.
  uint64_t qcap = queueCapacity(CN::Fqb);
  uint64_t qaddr = queueAddress(CN::Fqb);
  uint64_t qtail = readCsr(CN::Fqt);
  assert(qtail < qcap);

  uint64_t slotAddr = qaddr + qtail * sizeof(record);
  assert((sizeof(record) % 8) == 0);
  unsigned dwords = sizeof(record) / 8;   // Double-word count
  const uint64_t* ptr = reinterpret_cast<const uint64_t*>(&record);

  bool bigEnd = faultQueueBigEnd();

  for (unsigned i = 0; i < dwords; ++i, slotAddr += 8, ++ptr)
    memWriteDouble(slotAddr, bigEnd, *ptr);

  // Move tail.
  ++qtail;
  if (qtail >= qcap)
    qtail = 0;
  writeCsr(CN::Fqt, qtail);
}


bool
Iommu::wiredInterrupts()
{
  Capabilities caps{readCsr(CsrNumber::Capabilities)};

  if (caps.bits_.igs_ == unsigned(IgsMode::Wsi))
    return true;

  if (caps.bits_.igs_ == unsigned(IgsMode::Both))
    {
      uint32_t fctlVal = readCsr(CsrNumber::Fctl);
      Fctl fctl{fctlVal};
      return fctl.bits_.wsi_;
    }

  if (caps.bits_.igs_ == unsigned(IgsMode::Msi))
    return false;

  assert(0);
  return false;
}


void
Iommu::writeCsr(CsrNumber csrn, uint64_t data)
{
  if (csrn == CsrNumber::Ipsr)
    {
      writeIpsr(data);
      return;
    }
          
  auto& csr = csrs_.at(unsigned(csrn));
      
  // Handle special registers that require activation
  if (csrn == CsrNumber::Fqcsr) {
    uint32_t value = data & 0xFFFFFFFF;
    uint32_t oldValue = csr.read() & 0xFFFFFFFF;
        
    // Check if fqen bit is being set from 0 to 1
    if ((value & 0x1) && !(oldValue & 0x1)) {
      // Set busy bit
      value |= (1 << 17);
      csr.write(value);
          
      // Validate queue configuration
      uint64_t fqb = readCsr(CsrNumber::Fqb);
      uint64_t queuePpn = (fqb >> 10) & 0x3FFFFFFFFFF; // Extract PPN
      // uint64_t queueSize = 1ULL << ((fqb & 0x1F) + 1); // Extract LOG2SZ-1 and calculate size
          
      // Check if queue base is valid (basic validation)
      if (queuePpn != 0) {
        // Queue validation successful, set fqon bit
        value |= (1 << 16); // Set fqon bit
        value &= ~(1 << 17); // Clear busy bit
        csr.write(value);
        return;
      } else {
        // Queue validation failed, just clear busy bit
        value &= ~(1 << 17); // Clear busy bit
        csr.write(value);
        return;
      }
    }
  } else if (csrn == CsrNumber::Cqcsr) {
    uint32_t value = data & 0xFFFFFFFF;
    uint32_t oldValue = csr.read() & 0xFFFFFFFF;
        
    // Check if cqen bit is being set from 0 to 1
    if ((value & 0x1) && !(oldValue & 0x1)) {
      // Set busy bit
      value |= (1 << 17);
      csr.write(value);
          
      // Validate queue configuration
      uint64_t cqb = readCsr(CsrNumber::Cqb);
      uint64_t queuePpn = (cqb >> 10) & 0x3FFFFFFFFFF; // Extract PPN
      // uint64_t queueSize = 1ULL << ((cqb & 0x1F) + 1); // Extract LOG2SZ-1 and calculate size
          
      // Check if queue base is valid (basic validation)
      if (queuePpn != 0) {
        // Queue validation successful, set cqon bit
        value |= (1 << 16); // Set cqon bit
        value &= ~(1 << 17); // Clear busy bit
        csr.write(value);
        return;
      } else {
        // Queue validation failed, just clear busy bit
        value &= ~(1 << 17); // Clear busy bit
        csr.write(value);
        return;
      }
    }
  } else if (csrn == CsrNumber::Pqcsr) {
    uint32_t value = data & 0xFFFFFFFF;
    uint32_t oldValue = csr.read() & 0xFFFFFFFF;
        
    // Check if pqen bit is being set from 0 to 1
    if ((value & 0x1) && !(oldValue & 0x1)) {
      // Set busy bit
      value |= (1 << 17);
      csr.write(value);
          
      // Validate queue configuration
      uint64_t pqb = readCsr(CsrNumber::Pqb);
      uint64_t queuePpn = (pqb >> 10) & 0x3FFFFFFFFFF; // Extract PPN
      // uint64_t queueSize = 1ULL << ((pqb & 0x1F) + 1); // Extract LOG2SZ-1 and calculate size
          
      // Check if queue base is valid (basic validation)
      if (queuePpn != 0) {
        // Queue validation successful, set pqon bit
        value |= (1 << 16); // Set pqon bit
        value &= ~(1 << 17); // Clear busy bit
        csr.write(value);
        return;
      } else {
        // Queue validation failed, just clear busy bit
        value &= ~(1 << 17); // Clear busy bit
        csr.write(value);
        return;
      }
    }
  }
      
  // Normal write for other registers
  csr.write(data);

  if (csrn == CsrNumber::Fctl)
    {
      // Update cached big endian control in FCTL.
      data = readCsr(csrn);
      fctlBe_ = Fctl{uint32_t(data)}.bits_.be_;
    }
    
  // Process command queue when tail pointer is updated
  if (csrn == CsrNumber::Cqt)
    {
      processCommandQueue();
    }
    
  // Process page request queue when tail pointer is updated
  if (csrn == CsrNumber::Pqt)
    {
      processPageRequestQueue();
    }
}

void
Iommu::processCommandQueue()
{
  using CN = CsrNumber;
  
  // Check if command queue is enabled
  uint32_t cqcsrVal = readCsr(CN::Cqcsr);
  
  // Extract the cqon bit (bit 16) to check if queue is active
  bool cqon = (cqcsrVal >> 16) & 1;
  
  if (!cqon)
    return; // Command queue not active
    
  // Process commands while queue is not empty
  while (!queueEmpty(CN::Cqb, CN::Cqh, CN::Cqt))
  {
    uint64_t qcap = queueCapacity(CN::Cqb);
    uint64_t qaddr = queueAddress(CN::Cqb);
    uint64_t qhead = readCsr(CN::Cqh);
    
    if (qhead >= qcap)
      break; // Invalid head pointer
      
    // Read command from queue
    uint64_t cmdAddr = qaddr + qhead * 16; // Commands are 16 bytes
    AtsCommandData cmdData;
    
    bool bigEnd = false; // Command queue endianness (typically little endian)
    if (!memReadDouble(cmdAddr, bigEnd, cmdData.dw0) ||
        !memReadDouble(cmdAddr + 8, bigEnd, cmdData.dw1))
    {
      // Memory read failed, advance head and continue
      qhead = (qhead + 1) % qcap;
      writeCsr(CN::Cqh, qhead);
      continue;
    }
    
    // Process the command based on its type
    if (isAtsInvalCommand(cmdData))
    {
      executeAtsInvalCommand(cmdData);
    }
    else if (isAtsPrgrCommand(cmdData))
    {
      executeAtsPrgrCommand(cmdData);
    }
    else
    {
      // Unknown command type, potentially log error
      // For now, just skip it
    }
    
    // Advance head pointer
    qhead = (qhead + 1) % qcap;
    writeCsr(CN::Cqh, qhead);
  }
}

void 
Iommu::executeAtsInvalCommand(const AtsCommandData& cmdData)
{
  // Parse ATS.INVAL command
  AtsInvalCommand cmd;
  cmd = *reinterpret_cast<const AtsInvalCommand*>(&cmdData);
  
  // Check if ATS capability is enabled
  Capabilities caps{readCsr(CsrNumber::Capabilities)};
  if (!caps.bits_.ats_)
  {
    // ATS not supported, ignore command
    return;
  }
  
  // Extract command fields
  uint32_t rid = cmd.RID;
  uint32_t pid = cmd.PID;  
  bool pv = cmd.PV;
  bool dsv = cmd.DSV;
  uint32_t dseg = cmd.DSEG;
  uint64_t address = cmd.address;
  bool global = cmd.G;
  
  // Calculate device ID
  uint32_t devId = dsv ? ((dseg << 16) | rid) : rid;
  
  // ========================================================================
  // IMPLEMENTED FUNCTIONALITY
  // ========================================================================
  
  // 1. VALIDATE DEVICE CONTEXT
  DeviceContext dc;
  unsigned cause = 0;
  if (!loadDeviceContext(devId, dc, cause))
  {
    // Device context load failed - log error and complete command
    #ifdef DEBUG_ATS
    printf("ATS.INVAL: Failed to load device context for devId=0x%x, cause=%u\n", devId, cause);
    #endif
    return;
  }
  
  // Verify that ATS is enabled for this device
  if (!dc.ats())
  {
    // ATS not enabled for this device - log error and complete command
    #ifdef DEBUG_ATS
    printf("ATS.INVAL: ATS not enabled for devId=0x%x\n", devId);
    #endif
    return;
  }
  
  // 2. VALIDATE COMMAND PARAMETERS
  // Validate process ID if PV=1
  if (pv)
  {
    // Check if device supports process directory table
    if (!dc.pdtv())
    {
      #ifdef DEBUG_ATS
      printf("ATS.INVAL: Process ID specified but device doesn't support PDT, devId=0x%x\n", devId);
      #endif
      return;
    }
    
    // Validate PID is within supported range based on PDT mode
    Procid procid(pid);
    unsigned pdi1 = procid.ithPdi(1);
    unsigned pdi2 = procid.ithPdi(2);
    PdtpMode pdtpMode = dc.pdtpMode();
    
    if ((pdtpMode == PdtpMode::Pd17 && pdi2 != 0) ||
        (pdtpMode == PdtpMode::Pd8 && (pdi2 != 0 || pdi1 != 0)))
    {
      #ifdef DEBUG_ATS
      printf("ATS.INVAL: PID 0x%x out of range for PDT mode, devId=0x%x\n", pid, devId);
      #endif
      return;
    }
  }
  
  // Validate address alignment if address-specific invalidation
  if (address != 0 && (address & 0xFFF) != 0)
  {
    #ifdef DEBUG_ATS
    printf("ATS.INVAL: Address 0x%lx not page-aligned, devId=0x%x\n", address, devId);
    #endif
    return;
  }
  
  // 3. DETERMINE INVALIDATION SCOPE
  enum class InvalidationScope {
    GlobalDevice,      // G=1: All entries for this device
    ProcessSpecific,   // PV=1: Entries for specific PID
    AddressSpecific,   // address != 0: Specific page/range
    ProcessAndAddress  // PV=1 && address != 0: Process-specific address
  };
  
  InvalidationScope scope;
  if (global)
  {
    scope = InvalidationScope::GlobalDevice;
  }
  else if (pv && address != 0)
  {
    scope = InvalidationScope::ProcessAndAddress;
  }
  else if (pv)
  {
    scope = InvalidationScope::ProcessSpecific;
  }
  else if (address != 0)
  {
    scope = InvalidationScope::AddressSpecific;
  }
  else
  {
    // Invalid combination - no scope specified
    #ifdef DEBUG_ATS
    printf("ATS.INVAL: Invalid invalidation scope, devId=0x%x\n", devId);
    #endif
    return;
  }
  
  // ========================================================================
  // MISSING FUNCTIONALITY - What still needs to be implemented:
  // ========================================================================
  
  // 4. GENERATE PCIE INVALIDATION REQUEST MESSAGE
  //    - Create PCIe "Invalidation Request" message according to PCIe ATS spec
  //    - Message should target the device function identified by RID
  //    - Include appropriate invalidation parameters:
  //      * PASID (if PV=1, use PID as PASID)
  //      * Address range to invalidate
  //      * Invalidation type (global, process-specific, address-specific)
  
  // 5. SEND MESSAGE TO DEVICE
  //    - Use platform-specific PCIe infrastructure to send message
  //    - This would typically involve:
  //      * Formatting the message according to PCIe TLP format
  //      * Routing through PCIe fabric to target device
  //      * Handling any PCIe-level errors or routing failures
  
  // 6. TRACK PENDING INVALIDATION
  //    - Add entry to pending invalidation tracking structure
  //    - Include timeout information for this invalidation request
  //    - Associate with command queue entry for completion tracking
  
  // 7. HANDLE ASYNCHRONOUS COMPLETION
  //    - Set up mechanism to receive "Invalidation Completion" response
  //    - When response received:
  //      * Remove from pending invalidation list
  //      * Update command completion status
  //      * Signal any waiting IOFENCE.C commands
  
  // 8. TIMEOUT HANDLING
  //    - Implement timeout mechanism (platform-specific timeout value)
  //    - If timeout occurs:
  //      * Mark invalidation as timed out
  //      * Report timeout status to subsequent IOFENCE.C commands
  //      * Allow software to detect and retry failed invalidations
  
  // 9. INTEGRATION WITH IOFENCE.C
  //    - Ensure IOFENCE.C commands wait for all pending ATS.INVAL completions
  //    - Report any timeouts or failures to IOFENCE.C completion status
  
  // ========================================================================
  // Placeholder functionality only
  // ========================================================================
  
#ifdef DEBUG_ATS
  printf("ATS.INVAL: devId=0x%x, pid=0x%x, pv=%d, addr=0x%lx, global=%d, scope=%d\n", 
         devId, pid, pv, address, global, static_cast<int>(scope));
  
    (void)devId; (void)pid; (void)pv; (void)address; (void)global; (void)scope;
#endif

  // TODO: Implement actual ATS invalidation functionality as described above
  // This would require:
  // - Platform-specific PCIe message generation and routing
  // - Asynchronous response handling infrastructure  
  // - Timeout and error handling mechanisms
  // - Integration with command queue completion tracking
  (void) scope;
}

void 
Iommu::executeAtsPrgrCommand(const AtsCommandData& cmdData)
{
  // Parse ATS.PRGR command
  AtsPrgrCommand cmd;
  cmd = *reinterpret_cast<const AtsPrgrCommand*>(&cmdData);
  
  // Check if ATS capability is enabled
  Capabilities caps{readCsr(CsrNumber::Capabilities)};
  if (!caps.bits_.ats_)
  {
    // ATS not supported, ignore command
    return;
  }
  
  // Extract command fields
  uint32_t rid = cmd.RID;
  uint32_t pid = cmd.PID;
  uint32_t prgi = cmd.prgi;
  uint32_t resp_code = cmd.responsecode;
  bool dsv = cmd.DSV;
  uint32_t dseg = cmd.DSEG;
  
  // Calculate device ID
  uint32_t devId = dsv ? ((dseg << 16) | rid) : rid;
  
  // ========================================================================
  // IMPLEMENTED FUNCTIONALITY
  // ========================================================================
  
  // 1. VALIDATE DEVICE CONTEXT
  DeviceContext dc;
  unsigned cause = 0;
  if (!loadDeviceContext(devId, dc, cause))
  {
    // Device context load failed - log error and complete command
    #ifdef DEBUG_ATS
    printf("ATS.PRGR: Failed to load device context for devId=0x%x, cause=%u\n", devId, cause);
    #endif
    return;
  }
  
  // Verify that ATS is enabled for this device
  if (!dc.ats())
  {
    // ATS not enabled for this device - log error and complete command
    #ifdef DEBUG_ATS
    printf("ATS.PRGR: ATS not enabled for devId=0x%x\n", devId);
    #endif
    return;
  }
  
  // 2. VALIDATE PRI (PAGE REQUEST INTERFACE) CAPABILITY
  // Check if device supports Page Request Interface
  if (!dc.pri())
  {
    #ifdef DEBUG_ATS
    printf("ATS.PRGR: PRI not enabled for devId=0x%x\n", devId);
    #endif
    return;
  }
  
  // 3. VALIDATE COMMAND PARAMETERS
  // Validate process ID
  if (!dc.pdtv())
  {
    #ifdef DEBUG_ATS
    printf("ATS.PRGR: Process ID specified but device doesn't support PDT, devId=0x%x\n", devId);
    #endif
    return;
  }
  
  // Validate PID is within supported range based on PDT mode
  Procid procid(pid);
  unsigned pdi1 = procid.ithPdi(1);
  unsigned pdi2 = procid.ithPdi(2);
  PdtpMode pdtpMode = dc.pdtpMode();
  
  if ((pdtpMode == PdtpMode::Pd17 && pdi2 != 0) ||
      (pdtpMode == PdtpMode::Pd8 && (pdi2 != 0 || pdi1 != 0)))
  {
    #ifdef DEBUG_ATS
    printf("ATS.PRGR: PID 0x%x out of range for PDT mode, devId=0x%x\n", pid, devId);
    #endif
    return;
  }
  
  // Validate response code is within valid range
  // PCIe spec defines response codes: 0=Success, 1=Invalid Request, 2=Response Failure
  if (resp_code > 2)
  {
    #ifdef DEBUG_ATS
    printf("ATS.PRGR: Invalid response code %u, devId=0x%x\n", resp_code, devId);
    #endif
    return;
  }
  
  // Validate PRGI (Page Request Group Index)
  // PRGI should be non-zero and within reasonable bounds
  if (prgi == 0)
  {
    #ifdef DEBUG_ATS
    printf("ATS.PRGR: Invalid PRGI 0, devId=0x%x\n", devId);
    #endif
    return;
  }
  
  // ========================================================================
  // MISSING FUNCTIONALITY - What still needs to be implemented:
  // ========================================================================
  
  // 4. VALIDATE AND TRACK PRGI (IMPLEMENTED)
  // Check if PRGI corresponds to a previously received Page Request
  auto it = std::find_if(pendingPageRequests_.begin(), pendingPageRequests_.end(),
    [devId, pid, prgi](const PageRequest& req) {
      return req.devId == devId && 
             req.pid == pid && 
             req.prgi == prgi;
    });
  
  if (it == pendingPageRequests_.end())
  {
    // No matching page request found
    #ifdef DEBUG_ATS
    printf("ATS.PRGR: No matching pending request for devId=0x%x, pid=0x%x, prgi=0x%x\n", 
           devId, pid, prgi);
    #endif
    return;
  }
  
  // Found matching page request, remove it from pending list
  #ifdef DEBUG_ATS
  PageRequest matchedRequest = *it;
  printf("ATS.PRGR: Found and removed pending request for addr=0x%lx\n", matchedRequest.address);
  #endif

  pendingPageRequests_.erase(it);
  
  // 5. GENERATE PCIE PAGE REQUEST GROUP RESPONSE MESSAGE
  //    - Create PCIe "Page Request Group Response" message according to PCIe spec
  //    - Message should target the device function identified by RID
  //    - Include appropriate response parameters:
  //      * PASID (use PID as PASID)
  //      * PRGI (Page Request Group Index from command)
  //      * Response Code (Success, Invalid Request, Response Failure)
  
  // 6. SEND MESSAGE TO DEVICE
  //    - Use platform-specific PCIe infrastructure to send response message
  //    - This would typically involve:
  //      * Formatting the message according to PCIe TLP format
  //      * Routing through PCIe fabric to target device
  //      * Handling any PCIe-level errors or routing failures
  
  // 7. COORDINATE WITH MEMORY MANAGEMENT
  //    - If response code is Success:
  //      * Ensure that the requested memory pages are properly mapped
  //      * Update any necessary memory management structures
  //      * Coordinate with OS virtual memory subsystem
  //    - If response code indicates failure:
  //      * Log the failure reason for debugging
  //      * May need to coordinate with fault handling mechanisms
  
  // 8. INTEGRATION WITH FAULT HANDLING
  //    - Update fault queue entries if this response resolves a fault
  //    - Signal completion of fault handling to software
  //    - Update fault statistics and error tracking
  
  // ========================================================================
  // Placeholder functionality only
  // ========================================================================
  
  #ifdef DEBUG_ATS
  printf("ATS.PRGR: devId=0x%x, pid=0x%x, prgi=0x%x, resp_code=%u (processed)\n", 
         devId, pid, prgi, resp_code);
  
    (void)devId; (void)pid; (void)prgi; (void)resp_code;
#endif

  // TODO: Implement actual ATS page request group response functionality as described above
  // This would require:
  // - Platform-specific PCIe message generation and routing
  // - Integration with OS memory management
  // - Coordination with fault handling mechanisms
}

void
Iommu::processPageRequestQueue()
{
  using CN = CsrNumber;
  
  // Check if page request queue is enabled  
  uint32_t pqcsrVal = readCsr(CN::Pqcsr);
  bool pqon = (pqcsrVal >> 16) & 1;  // Extract pqon bit
  
  if (!pqon)
    return; // Page request queue not active
    
  // Process page requests while queue is not empty
  while (!queueEmpty(CN::Pqb, CN::Pqh, CN::Pqt))
  {
    uint64_t qcap = queueCapacity(CN::Pqb);
    uint64_t qaddr = queueAddress(CN::Pqb);
    uint64_t qhead = readCsr(CN::Pqh);
    
    if (qhead >= qcap)
      break; // Invalid head pointer
      
    // Read page request from queue (32 bytes per entry)
    uint64_t reqAddr = qaddr + qhead * 32;
    uint64_t reqData[4];
    
    // Variables for parsed request data
    uint32_t devId = 0;
    uint32_t pid = 0;
    bool pidValid = false;
    uint64_t address = 0;
    uint32_t prgi = 0;
    
    bool bigEnd = false; // Page request queue endianness
    for (int i = 0; i < 4; i++)
    {
      if (!memReadDouble(reqAddr + i*8, bigEnd, reqData[i]))
      {
        // Memory read failed, advance head and continue
        qhead = (qhead + 1) % qcap;
        writeCsr(CN::Pqh, qhead);
        goto next_request;
      }
    }
    
    // Parse page request (format defined in RISC-V IOMMU spec)
    // This is a simplified parsing - real implementation would need
    // to handle the full page request format
    devId = reqData[0] & 0xFFFFFF;
    pid = (reqData[0] >> 32) & 0xFFFFF;
    pidValid = (reqData[0] >> 52) & 1;
    address = reqData[1] & ~0xFFFULL; // Page aligned
    prgi = reqData[2] & 0xFFFF;
    
    // Send page request to device (this would be implementation specific)
    sendPageRequest(devId, pid, address, prgi);
    
    // Track the pending request
    PageRequest request;
    request.devId = devId;
    request.pid = pid;
    request.pidValid = pidValid;
    request.address = address;
    request.prgi = prgi;
    pendingPageRequests_.push_back(request);
    
    next_request:
    // Advance head pointer
    qhead = (qhead + 1) % qcap;
    writeCsr(CN::Pqh, qhead);
  }
}

void 
Iommu::sendPageRequest(uint32_t devId, uint32_t pid, uint64_t address, uint32_t prgi)
{
  // ========================================================================
  // IMPLEMENTED FUNCTIONALITY
  // ========================================================================
  
  // 1. VALIDATE DEVICE CONTEXT
  DeviceContext dc;
  unsigned cause = 0;
  if (!loadDeviceContext(devId, dc, cause))
  {
    // Device context load failed - log error and return
    #ifdef DEBUG_ATS
    printf("sendPageRequest: Failed to load device context for devId=0x%x, cause=%u\n", devId, cause);
    #endif
    return;
  }
  
  // 2. VALIDATE PRI CAPABILITY
  // Check if device supports Page Request Interface
  if (!dc.pri())
  {
    #ifdef DEBUG_ATS
    printf("sendPageRequest: PRI not enabled for devId=0x%x\n", devId);
    #endif
    return;
  }
  
  // 3. VALIDATE COMMAND PARAMETERS
  // Validate process ID (assuming pid != 0 means process-specific request)
  bool pidValid = (pid != 0);
  if (pidValid)
  {
    // Check if device supports process directory table
    if (!dc.pdtv())
    {
      #ifdef DEBUG_ATS
      printf("sendPageRequest: Process ID specified but device doesn't support PDT, devId=0x%x\n", devId);
      #endif
      return;
    }
    
    // Validate PID is within supported range based on PDT mode
    Procid procid(pid);
    unsigned pdi1 = procid.ithPdi(1);
    unsigned pdi2 = procid.ithPdi(2);
    PdtpMode pdtpMode = dc.pdtpMode();
    
    if ((pdtpMode == PdtpMode::Pd17 && pdi2 != 0) ||
        (pdtpMode == PdtpMode::Pd8 && (pdi2 != 0 || pdi1 != 0)))
    {
      #ifdef DEBUG_ATS
      printf("sendPageRequest: PID 0x%x out of range for PDT mode, devId=0x%x\n", pid, devId);
      #endif
      return;
    }
  }
  
  // Validate address alignment
  if ((address & 0xFFF) != 0)
  {
    #ifdef DEBUG_ATS
    printf("sendPageRequest: Address 0x%lx not page-aligned, devId=0x%x\n", address, devId);
    #endif
    return;
  }
  
  // Validate PRGI (Page Request Group Index)
  // PRGI should be non-zero and within reasonable bounds
  if (prgi == 0)
  {
    #ifdef DEBUG_ATS
    printf("sendPageRequest: Invalid PRGI 0, devId=0x%x\n", devId);
    #endif
    return;
  }
  
  // ========================================================================
  // MISSING FUNCTIONALITY - What still needs to be implemented:
  // ========================================================================
  
  // 4. GENERATE PCIE PAGE REQUEST MESSAGE (PRM)
  //    - Create PCIe "Page Request Message" according to PCIe PRI spec
  //    - Message should target the device function
  //    - Include appropriate request parameters:
  //      * PASID (if pidValid=true, use pid as PASID)
  //      * Virtual Address (page-aligned address)
  //      * PRGI (Page Request Group Index from parameter)
  //      * Permission flags (Read, Write, Execute) - determine from context
  //      * Privilege level (User vs Supervisor) - determine from context
  
  // 5. SEND MESSAGE TO DEVICE
  //    - Use platform-specific PCIe infrastructure to send PRM
  //    - This would typically involve:
  //      * Formatting the message according to PCIe TLP format
  //      * Routing through PCIe fabric to target device
  //      * Handling any PCIe-level errors or routing failures
  
  // 6. COORDINATE WITH OS MEMORY MANAGEMENT
  //    - Notify OS that a page fault has occurred
  //    - Provide fault details (address, permissions, process context)
  //    - Wait for OS to handle the fault (page-in, permission update, etc.)
  //    - Coordinate with virtual memory subsystem for page availability
  
  // 7. HANDLE TIMEOUT AND RETRY
  //    - Implement timeout mechanism (platform-specific timeout value)
  //    - If timeout occurs without response:
  //      * Retry the request (up to maximum retry count)
  //      * If max retries exceeded, report fault to software
  //      * Clean up outstanding request tracking
  
  // 8. INTEGRATION WITH FAULT HANDLING
  //    - If page request fails or times out:
  //      * Generate appropriate fault record in fault queue
  //      * Include fault details for software debugging
  //      * Update fault statistics and error tracking
  
  // ========================================================================
  // Placeholder functionality only
  // ========================================================================
  
  #ifdef DEBUG_ATS
  printf("sendPageRequest: devId=0x%x, pid=0x%x, pidValid=%d, addr=0x%lx, prgi=0x%x\n", 
         devId, pid, pidValid, address, prgi);
  
    (void)devId; (void)pid; (void)pidValid; (void)address; (void)prgi;
#endif

  // ========================================================================
  // Placeholder functionality only
  // ========================================================================
  
  // TRACK OUTSTANDING REQUEST (IMPLEMENTED)
  // Add request to outstanding page request tracking structure
  PageRequest request;
  request.devId = devId;
  request.pid = pid;
  request.address = address;
  request.prgi = prgi;
  request.pidValid = pidValid;
  
  pendingPageRequests_.push_back(request);
  
  #ifdef DEBUG_ATS
  printf("sendPageRequest: devId=0x%x, pid=0x%x, pidValid=%d, addr=0x%lx, prgi=0x%x (tracked)\n", 
         devId, pid, pidValid, address, prgi);
  
    (void)devId; (void)pid; (void)pidValid; (void)address; (void)prgi;
#endif

  // TODO: Implement actual page request functionality as described above
  // This would require:
  // - Platform-specific PCIe message generation and routing
  // - Integration with OS memory management
  // - Timeout and retry mechanisms
  // - Coordination with fault handling
}


bool
Iommu::isPageRequestPending(uint32_t devId, uint32_t pid, uint32_t prgi) const
{
  return std::any_of(pendingPageRequests_.begin(), pendingPageRequests_.end(),
    [devId, pid, prgi](const PageRequest& req) {
      return req.devId == devId && req.pid == pid && req.prgi == prgi;
    });
}

bool
Iommu::atsTranslate(const IommuRequest& req, AtsResponse& response, unsigned& cause)
{
  deviceDirWalk_.clear();
  processDirWalk_.clear();

  // Initialize response with default values
  response = AtsResponse{};
  cause = 0;

  // Validate that this is an ATS request
  if (not req.isAts())
  {
    cause = 260; // Transaction type disallowed
    response.success = false;
    response.isCompleterAbort = false; // UR for invalid transaction type
    return false;
  }

  using CN = CsrNumber;
  
  // Check if ATS capability is supported
  Capabilities caps(csrAt(CN::Capabilities).read());
  if (not caps.bits_.ats_)
  {
    cause = 256; // All inbound transactions disallowed
    response.success = false;
    response.isCompleterAbort = false; // UR for unsupported capability
    return false;
  }

  // Check IOMMU mode
  Ddtp ddtp{csrAt(CN::Ddtp).read()};
  if (ddtp.mode() == Ddtp::Mode::Off)
  {
    cause = 256; // All inbound transactions disallowed
    response.success = false;
    response.isCompleterAbort = false; // UR for IOMMU off
    return false;
  }

  // For Bare mode, ATS requests are not allowed
  if (ddtp.mode() == Ddtp::Mode::Bare)
  {
    cause = 260; // Transaction type disallowed
    response.success = false;
    response.isCompleterAbort = false; // UR for bare mode
    return false;
  }

  // Load device context
  DeviceContext dc;
  if (not loadDeviceContext(req.devId, dc, cause))
  {
    // Configuration errors result in CA response
    response.success = false;
    response.isCompleterAbort = true; // CA for configuration errors
    return false;
  }

  // Check if ATS is enabled for this device
  if (not dc.ats())
  {
    cause = 260; // Transaction type disallowed
    response.success = false;
    response.isCompleterAbort = false; // UR for ATS disabled
    return false;
  }

  // Determine access permissions requested
  bool r = req.isRead() or req.isExec();
  bool w = req.isWrite();
  bool x = req.isExec();

  // Check for execute-only requests (not compatible with PCIe ATS)
  if (x and not r)
  {
    // Execute-only translations are not compatible with PCIe ATS
    // Grant execute permission only if read permission is also granted
    x = false;
  }

  uint64_t translatedAddr = 0;
  bool isMsiAddr = false;
  
  // Check if this is an MSI address
  if (dc.isMsiAddress(req.iova))
  {
    isMsiAddr = true;
    // Handle MSI address translation
    uint64_t gpa = req.iova; // For MSI, IOVA is typically the GPA
    uint64_t pa = 0;
    bool isMrif = false;
    uint64_t mrif = 0;
    uint64_t nnpn = 0;
    unsigned nid = 0;
    
    if (msiTranslate(dc, req, gpa, pa, isMrif, mrif, nnpn, nid, cause))
    {
      if (isMrif)
      {
        // MRIF mode - set U bit to indicate untranslated access only
        response.success = true;
        response.translatedAddr = gpa; // Return GPA for MRIF mode
        response.readPerm = true;
        response.writePerm = true;
        response.execPerm = false;
        response.untranslatedOnly = true; // U bit set
        response.global = false;
        return true;
      }
      else
      {
        translatedAddr = pa;
      }
    }
    else
    {
      // MSI translation failed - return CA
      response.success = false;
      response.isCompleterAbort = true;
      return false;
    }
  }
  else
  {
    // Regular address translation
    if (dc.t2gpa())
    {
      // T2GPA mode - return GPA instead of SPA
      if (not t2gpaTranslate(req, translatedAddr, cause))
      {
        // Handle different error types for ATS responses
        if (cause == 12 or cause == 13 or cause == 15 or  // Page faults
            cause == 20 or cause == 21 or cause == 23 or  // Guest page faults
            cause == 266 or cause == 262)                 // PDT/MSI PTE not valid
        {
          // Page faults return Success with R=W=0
          response.success = true;
          response.translatedAddr = 0; // UNSPECIFIED per spec
          response.readPerm = false;
          response.writePerm = false;
          response.execPerm = false;
          return true;
        }
        else if (cause == 1 or cause == 5 or cause == 7 or  // Access faults
                 cause == 261 or cause == 263 or            // MSI PTE faults
                 cause == 265 or cause == 267)              // PDT entry faults
        {
          // Configuration errors return CA
          response.success = false;
          response.isCompleterAbort = true;
          return false;
        }
        else
        {
          // Permanent errors return UR
          response.success = false;
          response.isCompleterAbort = false;
          return false;
        }
      }
    }
    else
    {
      // Regular translation (SPA mode)
      if (not translate(req, translatedAddr, cause))
      {
        // Handle different error types for ATS responses
        if (cause == 12 or cause == 13 or cause == 15 or  // Page faults
            cause == 20 or cause == 21 or cause == 23 or  // Guest page faults
            cause == 266 or cause == 262)                 // PDT/MSI PTE not valid
        {
          // Page faults return Success with R=W=0
          response.success = true;
          response.translatedAddr = 0; // UNSPECIFIED per spec
          response.readPerm = false;
          response.writePerm = false;
          response.execPerm = false;
          return true;
        }
        else if (cause == 1 or cause == 5 or cause == 7 or  // Access faults
                 cause == 261 or cause == 263 or            // MSI PTE faults
                 cause == 265 or cause == 267)              // PDT entry faults
        {
          // Configuration errors return CA
          response.success = false;
          response.isCompleterAbort = true;
          return false;
        }
        else
        {
          // Permanent errors return UR
          response.success = false;
          response.isCompleterAbort = false;
          return false;
        }
      }
    }
  }

  // Translation successful - build response
  response.success = true;
  response.translatedAddr = translatedAddr;
  
  // Set permission bits (would need to get actual permissions from page tables)
  // For now, grant requested permissions if translation succeeded
  response.readPerm = r;
  response.writePerm = w;
  response.execPerm = x and r; // Execute only if read is also granted
  
  // Set other response fields per spec section 3.6
  response.privMode = req.hasProcId and (req.privMode == PrivilegeMode::Supervisor);
  response.noSnoop = false; // Always 0 per spec
  response.global = false;  // Would need to get from first-stage page tables
  response.ama = 0;         // Default 000b
  
  // Set CXL.io bit based on device type and memory type
  response.cxlIo = false;   // Default, would need device-specific logic
  if (isMsiAddr)
  {
    response.cxlIo = true;  // MSI addresses set CXL.io = 1
  }
  else if (dc.t2gpa())
  {
    response.cxlIo = true;  // T2GPA mode sets CXL.io = 1
  }

  return true;
}


bool
Iommu::t2gpaTranslate(const IommuRequest& req, uint64_t& gpa, unsigned& cause)
{
  deviceDirWalk_.clear();
  processDirWalk_.clear();

  cause = 0;

  using CN = CsrNumber;
  
  // Check IOMMU mode
  Ddtp ddtp{csrAt(CN::Ddtp).read()};
  if (ddtp.mode() == Ddtp::Mode::Off)
  {
    cause = 256; // All inbound transactions disallowed
    return false;
  }

  if (ddtp.mode() == Ddtp::Mode::Bare)
  {
    // In bare mode, no translation - IOVA is the GPA
    gpa = req.iova;
    return true;
  }

  // Load device context
  DeviceContext dc;
  if (not loadDeviceContext(req.devId, dc, cause))
    return false;

  // T2GPA mode requires ATS to be enabled
  if (not dc.ats() or not dc.t2gpa())
  {
    cause = 260; // Transaction type disallowed
    return false;
  }

  // Determine access permissions
  bool r = req.isRead() or req.isExec();
  bool w = req.isWrite();
  bool x = req.isExec();

  // Perform first-stage translation to get GPA
  if (dc.pdtv())
  {
    // Process directory table mode
    ProcessContext pc;
    unsigned procId = req.hasProcId ? req.procId : 0;
    
    if (req.hasProcId or dc.dpe())
    {
      if (not loadProcessContext(dc, procId, pc, cause))
        return false;
      
      // Use process context for first-stage translation
      uint64_t iosatp = pc.fsc(); // FSC field contains the IOSATP value
      uint64_t iohgatp = dc.iohgatp();
      bool sum = pc.sum();
      
      if (not stage1Translate(iosatp, iohgatp, req.privMode, procId, r, w, x, sum, req.iova, gpa, cause))
        return false;
    }
    else
    {
      // No valid process ID and DPE not enabled
      cause = 260; // Transaction type disallowed
      return false;
    }
  }
  else
  {
    // Direct IOSATP mode
    uint64_t iosatp = dc.iosatp();
    uint64_t iohgatp = dc.iohgatp();
    bool sum = false;
    
    if (not stage1Translate(iosatp, iohgatp, req.privMode, 0, r, w, x, sum, req.iova, gpa, cause))
      return false;
  }

  // In T2GPA mode, we stop here and return the GPA
  // The device will use this GPA in subsequent translated requests
  // which will then undergo second-stage translation
  
  return true;
}
