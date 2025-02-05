// Copyright 2025 Tenstorrent Corporation.
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

#pragma once

namespace WdRiscv
{

  /// Pointer masking manager.
  class PmaskManager
  {
  public:

    /// Pointer masking modes.
    enum class Mode : uint32_t
      {
        Off = 0,
        Reserved = 1,
        Pm57 = 2,
        Pm48 = 3,
        Limit_ = 4
      };


    /// Constructor.
    PmaskManager()
    {
      supportedPmms_.resize(unsigned(Mode::Limit_));
      setSupportedModes({Mode::Off, Mode::Pm57, Mode::Pm48});
    }

    /// Mark items in the pmms array as supported pointer masking (PMM) modes.
    void setSupportedModes(const std::vector<Mode>& pmms)
    {
      std::fill(supportedPmms_.begin(), supportedPmms_.end(), false);
      for (auto pmm : pmms)
	{
	  if (pmm != Mode::Reserved)
	    {
	      unsigned ix = unsigned(pmm);
	      if (ix < supportedPmms_.size())
		supportedPmms_.at(ix) = true;
	    }
	}
    }

    /// Return true if given pointer masking mode (PMM) is supported.
    bool isSupported(Mode pmm)
    {
      unsigned ix = unsigned(pmm);
      return ix < supportedPmms_.size() ? supportedPmms_.at(ix) : false;
    }

    /// Apply pointer masking to the given address returning the result.
    uint64_t applyPointerMask(uint64_t addr, PrivilegeMode priv, bool twoStage, bool load,
                              bool bare) const
    {
      if (execReadable_)
        return addr;

      if (priv == PrivilegeMode::Machine)
        return applyPointerMaskPa(addr, priv, twoStage);

      bool exec = load and xForR_;
      if (not exec)
        {
          if (twoStage)
            {
              if (not bare)
                {
                  if (s1ExecReadable_)
                    return addr;
                  return applyPointerMaskVa(addr, priv, twoStage);
                }
              return applyPointerMaskPa(addr, priv, twoStage);
            }

          if (not bare)
            return applyPointerMaskVa(addr, priv, twoStage);
          return applyPointerMaskPa(addr, priv, twoStage);
        }
      return addr;
    }

    /// Make executable pages also readable (supports MXR bit in MSTATUS/SSTATUS).  This
    /// affects both stages of translation in virtual mode.
    void setExecReadable(bool flag)
    { execReadable_ = flag; }

    /// Use exec access permission for read permission.
    void useExecForRead(bool flag)
    { xForR_ = flag; }

    /// Make executable pages also readable (supports MXR bit in VSSTATUS).
    /// This only affects stage1 translation.
    void setStage1ExecReadable(bool flag)
    { s1ExecReadable_ = flag; }

    /// Return string representing pointer masking mode. Example: Pm57 yields "pm57".
    static constexpr std::string_view to_string(Mode pmm)
    {
      using namespace std::string_view_literals;
      constexpr auto vec =
        std::array{"off"sv, "reserved"sv, "pm57"sv, "pm48"sv};
      return size_t(pmm) < vec.size()? vec.at(size_t(pmm)) : "pm?";
    }

    /// Set pmm to the translation pointer masking mode corresponding to pmmStr returning
    /// true if successful. Return false leaving pmm unmodified if pmmStr does not
    /// correspond to a pmm.
    static bool to_pmm(std::string_view pmmStr, Mode& pmm)
    {
      static const std::unordered_map<std::string_view, Mode> map(
         { {"off", Mode::Off }, {"pm57", Mode::Pm57 }, {"pm48", Mode::Pm48 } }
      );

      auto iter = map.find(pmmStr);
      if (iter != map.end())
	{
	  pmm = iter->second;
	  return true;
	}
      return false;
    }

    /// Pointer mask bits associated with each mode.
    static constexpr unsigned pointerMaskBits(Mode pmm)
    {
      switch(pmm)
      {
        case Mode::Off: return 0;
        case Mode::Pm57: return 7;
        case Mode::Pm48: return 16;
        default: assert(0); return 0;
      }
    }

    /// Enable/disable pointer masking for corresponding mode.
    void enablePointerMasking(Mode pmm, PrivilegeMode priv, bool twoStage)
    {
      if (priv == PrivilegeMode::Machine)
        mPmBits_ = pointerMaskBits(pmm);
      if (priv == PrivilegeMode::Supervisor and not twoStage)
        sPmBits_ = pointerMaskBits(pmm);
      if (priv == PrivilegeMode::Supervisor and twoStage)
        vsPmBits_ = pointerMaskBits(pmm);
      if (priv == PrivilegeMode::User)
        uPmBits_ = pointerMaskBits(pmm);
    }

    /// Helper for below function
    static uint64_t applyPointerMaskVa(uint64_t va, unsigned shift)
    {
      int64_t transformed = std::bit_cast<int64_t>(va);
      transformed = (transformed << shift) >> shift;
      return std::bit_cast<uint64_t>(transformed);
    }

    /// Transform virtual address by appropriate pointer masking mode. This is
    /// only necessary for the effective address for load/stores.
    uint64_t applyPointerMaskVa(uint64_t va, PrivilegeMode priv, bool twoStage) const
    {
      assert(priv != PrivilegeMode::Machine);
      if (sPmBits_ and priv == PrivilegeMode::Supervisor and not twoStage)
        return applyPointerMaskVa(va, sPmBits_);
      if (vsPmBits_ and priv == PrivilegeMode::Supervisor and twoStage)
        return applyPointerMaskVa(va, vsPmBits_);
      if (uPmBits_ and priv == PrivilegeMode::User)
        return applyPointerMaskVa(va, uPmBits_);
      return va;
    }

    /// Helper for below function
    static uint64_t applyPointerMaskPa(uint64_t pa, unsigned shift)
    { return (pa << shift) >> shift; }

    /// Transform physical address by appropriate pointer masking mode. This
    /// also applies to GPAs (see section 3.5 of the spec).
    uint64_t applyPointerMaskPa(uint64_t pa, PrivilegeMode priv, bool twoStage) const
    {
      if (mPmBits_ and priv == PrivilegeMode::Machine)
        return applyPointerMaskPa(pa, mPmBits_);
      if (sPmBits_ and priv == PrivilegeMode::Supervisor and not twoStage)
        return applyPointerMaskPa(pa, sPmBits_);
      if (vsPmBits_ and priv == PrivilegeMode::Supervisor and twoStage)
        return applyPointerMaskPa(pa, vsPmBits_);
      if (uPmBits_ and priv == PrivilegeMode::User)
        return applyPointerMaskPa(pa, uPmBits_);
      return pa;
    }

    Mode getMode(PrivilegeMode priv, bool twoStage) const
    {
      unsigned bits = 0;
      if (priv == PrivilegeMode::Machine)
        bits = mPmBits_;
      if (priv == PrivilegeMode::Supervisor and not twoStage)
        bits = sPmBits_;
      if (priv == PrivilegeMode::Supervisor and twoStage)
        bits = vsPmBits_;
      if (priv == PrivilegeMode::User)
        bits = uPmBits_;

      switch(bits)
      {
        case 0: return Mode::Off;
        case 7: return Mode::Pm57;
        case 16: return Mode::Pm48;
        default: assert(0); return Mode::Off;
      }
    }

  private:

    std::vector<bool> supportedPmms_;  // Indexed by Pmm.
    bool execReadable_ = false;  // MXR bit
    bool s1ExecReadable_ = false;  // MXR bit of vsstatus
    bool xForR_ = false;  // True for hlvx.hu and hlvx.wu instructions: use exec for read

    unsigned mPmBits_ = 0;          // Pointer asking for M mode.
    unsigned sPmBits_ = 0;          // Pointer masking for HS translation.
    unsigned vsPmBits_ = 0;         // Pointer masking for VS translation.
    unsigned uPmBits_ = 0;          // Pointer masking for U/VU translation.

  };
}
