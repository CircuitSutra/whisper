// Copyright 2020 Western Digital Corporation or its affiliates.
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


#include <iostream>
#include <fstream>
#include <span>
#include <sstream>
#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>
#include "CsRegs.hpp"
#include "DecodedInst.hpp"
#include "Interactive.hpp"
#include "Hart.hpp"
#include "linenoise.hpp"
#include "System.hpp"
#include "WhisperMessage.h"


using namespace WdRiscv;
using std::cerr;

/// Return format string suitable for printing an integer of type URV
/// in hexadecimal form.
template <typename URV>
static
const char*
getHexForm()
{
  if (sizeof(URV) == 4)
    return "0x%08x";
  if (sizeof(URV) == 8)
    return "0x%016x";
  if (sizeof(URV) == 16)
    return "0x%032x";
  return "0x%x";
}


static
bool
parseCmdLineBool(const std::string& option,
		 const std::string& boolStr,
		 bool& flag)
{
  bool good = not boolStr.empty();

  if (good)
    {
      if (boolStr == "true" or boolStr == "t" or boolStr == "1")
	flag = true;
      else if (boolStr == "false" or boolStr == "f" or boolStr == "0")
	flag = false;
      else
	good = false;
    }

  if (not good)
    cerr << "Error: Invalid command line " << option << " value: " << boolStr << '\n';

  return good;
}


/// Convert the command line string numberStr to a number using
/// strotull and a base of zero (prefixes 0 and 0x are
/// honored). Return true on success and false on failure (string does
/// not represent a number). TYPE is an integer type (e.g
/// uint32_t). Option is the command line option associated with the
/// string and is used for diagnostic messages.
template <typename TYPE>
static
bool
parseCmdLineNumber(const std::string& option,
		   const std::string& numberStr,
		   TYPE& number)
{
  bool good = not numberStr.empty();

  if (good)
    {
      char* end = nullptr;
      uint64_t value = strtoull(numberStr.c_str(), &end, 0);
      number = static_cast<TYPE>(value);
      if (number != value)
	{
	  cerr << "Error: parseCmdLineNumber: Number too large: " << numberStr << '\n';
	  return false;
	}
      if (end and *end)
	good = false;  // Part of the string are non parseable.
    }

  if (not good)
    cerr << "Error: Invalid command line " << option << " value: " << numberStr << '\n';
  return good;
}


static
bool
parseCmdLineVecData(std::string_view option,
		    std::string_view valStr,
		    std::vector<uint8_t>& val)
{
  val.clear();

  if (not (valStr.starts_with("0x") or valStr.starts_with("0X")))
    {
      cerr << "Error: Value of vector " << option << " must begin with 0x: " << valStr << '\n';
      return false;
    }

  std::string_view trimmed = valStr.substr(2); // Remove leading 0x
  if (trimmed.empty())
    {
      cerr << "Error: Empty value for vector " << option << ": " << valStr << '\n';
      return false;
    }

  if (trimmed.size() == 1 and trimmed.at(0) == '0')
    {
      val.push_back(0);
      return true;
    }

  if ((trimmed.size() & 1) != 0)
    {
      cerr << "Error: Value for vector " << option << " must have an even"
	   << " number of hex digits: " << valStr << '\n';
      return false;
    }

  for (size_t i = 0; i < trimmed.size(); i += 2)
    {
      std::string byteStr(trimmed.substr(i, 2));
      char* end = nullptr;
      unsigned value = strtoul(byteStr.c_str(), &end, 16);
      if (end and *end)
	{
	  cerr << "Error: Invalid hex digit(s) in vector " << option << ": "
	       << byteStr << '\n';
	  return false;
	}
      val.push_back(value);
    }

  return true;
}


template <typename URV>
Interactive<URV>::Interactive(System<URV>& system, std::ostream& out)
  : system_(system), out_(out)
{
  // In interactive mode the user will issue a cancel-lr explcitly for wrs instructions.
  // This is done to be able to replay server mode command log.
  for (unsigned i = 0; i < system.hartCount(); ++i)
    {
      auto& hart = *(system_.ithHart(i));
      hart.setWrsCancelsLr(false);
    }
}


template <typename URV>
bool
Interactive<URV>::untilCommand(Hart<URV>& hart, const std::string& line,
			       const std::vector<std::string>& tokens,
			       FILE* traceFile)
{
  if (tokens.size() != 2)
    {
      cerr << "Error: Invalid until command: " << line << '\n';
      cerr << "Error: Expecting: until address\n";
      return false;
    }

  size_t addr = 0;
  if (not parseCmdLineNumber("address", tokens.at(1), addr))
    return false;

  if (addr >= hart.memorySize())
    {
      cerr << "Error: Address outside memory range: " << line << ".\n";
      return false;
    }

  if (hart.inDebugMode())
    {
      hart.exitDebugMode();   // Resume from halt.
      if (hart.hasDcsrStep())
	{
	  hart.singleStep(traceFile);
	  hart.enterDebugMode(hart.peekPc());
	  return true;
	}
    }

  return hart.untilAddress(addr, traceFile);
}


template <typename URV>
bool
Interactive<URV>::runCommand(Hart<URV>& hart, const std::string& /*line*/,
			     const std::vector<std::string>& /*tokens*/,
			     FILE* traceFile)
{
  if (hart.inDebugMode())
    {
      hart.exitDebugMode();
      if (hart.hasDcsrStep())
	{
	  hart.singleStep(traceFile);
	  hart.enterDebugMode(hart.peekPc());
	  return true;
	}
    }
  return hart.run(traceFile);
}


template <typename URV>
bool
Interactive<URV>::stepCommand(Hart<URV>& hart, const std::string& /*line*/,
			      const std::vector<std::string>& tokens,
			      FILE* traceFile)
{
  uint64_t count = 1;

  if (tokens.size() > 1)
    {
      if (not parseCmdLineNumber("instruction-count", tokens.at(1), count))
	return false;
      if (count == 0)
	return true;
    }

  uint64_t tag = 0;
  bool hasTag = false;
  if (tokens.size() > 2)
    {
      if (not parseCmdLineNumber("instruction-tag", tokens.at(2), tag))
	return false;
      hasTag = true;
    }

  bool wasInDebug = false;
  if (not hart.hasDebugParkLoop())
    {
      wasInDebug = hart.inDebugMode();
      if (wasInDebug)
	hart.exitDebugMode();
    }

  for (uint64_t i = 0; i < count; ++i)
    {
      if (hasTag)
	{
	  DecodedInst di;
	  hart.setInstructionCount(tag-1);
	  hart.singleStep(di, traceFile);
	  if (not di.isValid())
	    assert(hart.lastInstructionCancelled());
	  system_.mcmRetire(hart, this->time_, tag++, di, hart.lastInstructionCancelled());
	}
      else
	hart.singleStep(traceFile);
    }

  if (wasInDebug)
    hart.enterDebugMode(hart.peekPc());

  return true;
}


template <typename URV>
void
Interactive<URV>::peekAllFpRegs(Hart<URV>& hart, std::ostream& out)
{
  for (unsigned i = 0; i < hart.fpRegCount(); ++i)
    {
      uint64_t val = 0;
      if (hart.peekFpReg(i, val))
	{
	  out << "f" << i << ": "
              << (boost::format("0x%016x") % val) << '\n';
	}
    }
}


template <typename URV>
void
Interactive<URV>::peekAllVecRegs(Hart<URV>& hart, std::ostream& out)
{
  for (unsigned i = 0; i < hart.vecRegCount(); ++i)
    {
      std::vector<uint8_t> val;
      if (hart.peekVecReg(i, val))
	{
	  out << "v" << i << ": 0x";
	  for (unsigned byte : val)
	    out << (boost::format("%02x") % byte);
	  out << '\n';
	}
    }
}


template <typename URV>
void
Interactive<URV>::peekAllIntRegs(Hart<URV>& hart, std::ostream& out)
{
  bool abiNames = hart.abiNames();
  auto hexForm = getHexForm<URV>(); // Format string for printing a hex val

  for (unsigned i = 0; i < hart.intRegCount(); ++i)
    {
      std::string_view name;
      URV val = 0;
      if (hart.peekIntReg(i, val, name))
	{
	  std::string tag = std::string(name);
	  if (abiNames)
	    tag += "(" + std::to_string(i) + ")";
	  tag += ":";

          out << (boost::format("%-9s") % tag)
              << (boost::format(hexForm) % val) << '\n';
	}
    }
}


template <typename URV>
void
Interactive<URV>::peekAllCsrs(Hart<URV>& hart, std::ostream& out)
{
  auto hexForm = getHexForm<URV>(); // Format string for printing a hex val

  out << (boost::format("%-23s") % "csr");
  if (sizeof(URV) == 4)
    out << (boost::format("%-10s %-10s %-10s %-10s\n") % "value" %
            "reset" % "mask" % "pokemask");
  else
    out << (boost::format("%-18s %-18s %-18s %-10s\n") % "value" %
            "reset" % "mask" % "pokemask");

  for (size_t i = 0; i <= size_t(CsrNumber::MAX_CSR_); ++i)
    {
      CsrNumber csr = CsrNumber(i);
      std::string_view name;
      URV val = 0;
      if (hart.peekCsr(csr, val, name))
	{
	  std::ostringstream oss;
	  oss << name << "(0x" << std::hex << i << "):"  << std::dec;

	  out << (boost::format("%-23s") % oss.str())
              << (boost::format(hexForm) % val);

	  URV reset = 0, writeMask = 0, pokeMask = 0, readMask = 0;
	  if (hart.peekCsr(csr, val, reset, writeMask, pokeMask, readMask))
	    {
	      out << ' ' << (boost::format(hexForm) % reset);
	      out << ' ' << (boost::format(hexForm) % writeMask);
	      out << ' ' << (boost::format(hexForm) % pokeMask);
	      out << ' ' << (boost::format(hexForm) % readMask);
	    }
	  out << '\n';
	}
    }

  out << '\n';

  PrivilegeMode pm = hart.privilegeMode();
  out << "Privilege mode: ";
  switch(pm)
    {
    case PrivilegeMode::User:       out << "user\n";       break;
    case PrivilegeMode::Supervisor: out << "supervisor\n"; break;
    case PrivilegeMode::Reserved:   out << "reserved\n";   break;
    case PrivilegeMode::Machine:    out << "machine\n";    break;
    }

  out << '\n';

  out << "pmpaddr  type mode locked low                high\n";

  uint64_t low = 0, high = 0;
  Pmp::Type type = Pmp::Type::Off;
  Pmp::Mode mode = Pmp::Mode::None;
  bool locked = false;

  for (unsigned ix = 0; ix < 16; ++ix)
    {
      if (not hart.unpackMemoryProtection(ix, type, mode, locked, low, high))
        continue;

      std::string typeStr = Pmp::toString(type);
      std::string modeStr = Pmp::toString(mode);
      const char* lockStr = locked? "y" : "n";
      out << 
        (boost::format("%7d %5s %4s %6s 0x%016x 0x%016x") % ix % typeStr %
         modeStr % lockStr % low % high) << '\n';
    }
}


template <typename URV>
void
Interactive<URV>::peekAllTriggers(Hart<URV>& hart, std::ostream& out)
{
  auto hexForm = getHexForm<URV>(); // Format string for printing a hex val

  out << (boost::format("%-12s") % "trigger");
  if (sizeof(URV) == 4)
    out << (boost::format("%-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s %-10s\n") %
            "value1" % "value2" % "value3" %
            "mask1" % "mask2" % "mask3" %
            "poke-mask1" % "poke-mask2"  % "poke-mask3");
  else
    out << (boost::format("%-18s %-18s %-18s %-18s %-18s %-18s %-18s %-18s %-18s\n") %
            "value1" % "value2" % "value3" %
            "mask1" % "mask2" % "mask3" %
            "poke-mask1" % "poke-mask2"  % "poke-mask3");


  // value/reset/write-mask/poke-mask
  URV tselVal = 0, tselReset, tselWm = 0, tselPm = 0, tselRm;

  if (hart.peekCsr(CsrNumber::TSELECT, tselVal, tselReset, tselWm, tselPm, tselRm))
    {
      URV maxTrigger = tselWm;
      for (URV trigger = 0; trigger <= maxTrigger; ++trigger)
	{
	  uint64_t v1(0), v2(0), v3(0), wm1(0), wm2(0), wm3(0);
	  uint64_t pm1(0), pm2(0), pm3(0);

	  if (hart.peekTrigger(trigger, v1, v2, v3, wm1, wm2, wm3,
			       pm1, pm2, pm3))
	    {
	      std::string name = "trigger" + std::to_string(trigger) + ":";
	      out << (boost::format("%-11s") % name);
	      out << ' ' << (boost::format(hexForm) % v1);
	      out << ' ' << (boost::format(hexForm) % v2);
	      out << ' ' << (boost::format(hexForm) % v3);
	      out << ' ' << (boost::format(hexForm) % wm1);
	      out << ' ' << (boost::format(hexForm) % wm2);
	      out << ' ' << (boost::format(hexForm) % wm3);
	      out << ' ' << (boost::format(hexForm) % pm1);
	      out << ' ' << (boost::format(hexForm) % pm2);
	      out << ' ' << (boost::format(hexForm) % pm3);
	      out << '\n';
	    }
	  else
	    break;
	}
    }
}


template <typename URV>
static
bool
peekMemory(Hart<URV>& hart, uint64_t addr0, uint64_t addr1, std::ostream& out)
{
  auto hexForm = getHexForm<URV>(); // Format string for printing a hex val

  uint32_t word = 0;
  bool usePma = false;

  for (uint64_t addr = addr0; addr <= addr1; addr += 4)
    {
      if (not hart.peekMemory(addr, word, usePma))
        {
          cerr << "Error: Peek memory address out of bounds: 0x"
	       << std::hex << addr << std::dec << '\n';
          return false;
        }
      out << (boost::format(hexForm) % addr) << ": ";
      out << (boost::format("0x%08x") % word) << std::endl;
    }

  return true;
}


template <typename URV>
bool
Interactive<URV>::peekCommand(Hart<URV>& hart, const std::string& line,
			      const std::vector<std::string>& tokens,
                              std::ostream& out)
{
  if (tokens.size() < 2)
    {
      cerr << "Error: Invalid peek command: " << line << '\n';
      cerr << "Error: Expecting: peek <item> <addr>  or  peek pc  or  peek all\n";
      cerr << "Error:   Item is one of r, f, c, v, t , pc, m, or s for integer, floating point,\n";
      cerr << "Error:   CSR, vector, trigger register, program counter, memory location, or special respectively\n";

      cerr << "Error:   example:  peek r x3\n";
      cerr << "Error:   example:  peek f f4\n";
      cerr << "Error:   example:  peek c mtval\n";
      cerr << "Error:   example:  peek c mtval 1\n";
      cerr << "Error:   example:  peek v v2\n";
      cerr << "Error:   example:  peek m 0x4096\n";
      cerr << "Error:   example:  peek t 0\n";
      cerr << "Error:   example:  peek pc\n";
      cerr << "Error:   example:  peek s pm\n";
      return false;
    }

  auto hexForm = getHexForm<URV>(); // Format string for printing a hex val
  URV val = 0;

  const std::string& resource = tokens.at(1);

  if (resource == "all")
    {
      out << "pc: " << (boost::format(hexForm) % hart.peekPc()) << '\n';
      out << "\n";

      peekAllIntRegs(hart, out);
      out << "\n";

      peekAllFpRegs(hart, out);
      out << "\n";

      peekAllVecRegs(hart, out);
      out << "\n";

      peekAllCsrs(hart, out);
      out << "\n";

      peekAllTriggers(hart, out);
      return true;

    }

  if (resource == "pc")
    {
      URV pc = hart.peekPc();
      out << (boost::format(hexForm) % pc) << std::endl;
      return true;
    }

  if (tokens.size() < 3)
    {
      cerr << "Error: Invalid peek command: " << line << '\n';
      cerr << "Error: Expecting: peek <resource> <address>\n";
      return false;
    }

  const std::string& addrStr = tokens.at(2);

  if (resource == "m")
    {
      uint64_t addr0 = 0;
      if (not parseCmdLineNumber("memory-address", addrStr, addr0))
	return false;

      uint64_t addr1 = addr0;
      if (tokens.size() >= 4)
	if (not parseCmdLineNumber("memory-address", tokens.at(3), addr1))
	  return false;

      if (tokens.size() >= 5)
        {
          std::ofstream out(tokens.at(4));
          if (not out)
            {
              cerr << "Error: Failed to open " << tokens.at(4) << " for write of peek command output\n";
              return false;
            }
          return peekMemory(hart, addr0, addr1, out);
        }

      return peekMemory(hart, addr0, addr1, out);
    }

  if (resource == "r")
    {
      if (addrStr == "all")
	{
	  peekAllIntRegs(hart, out);
	  return true;
	}

      unsigned intReg = 0;
      if (not hart.findIntReg(addrStr, intReg))
	{
	  cerr << "Error: No such integer register: " << addrStr << '\n';
	  return false;
	}
      if (hart.peekIntReg(intReg, val))
	{
	  out << (boost::format(hexForm) % val) << std::endl;
	  return true;
	}
      cerr << "Error: Failed to read integer register: " << addrStr << '\n';
      return false;
    }

  if (resource == "f")
    {
      if (not hart.isRvf())
	{
	  cerr << "Error: Floating point extension is no enabled\n";
	  return false;
	}

      if (addrStr == "all")
	{
	  peekAllFpRegs(hart, out);
	  return true;
	}

      unsigned fpReg = 0;
      if (not hart.findFpReg(addrStr, fpReg))
	{
	  cerr << "Error: No such integer register: " << addrStr << '\n';
	  return false;
	}
      uint64_t fpVal = 0;
      if (hart.peekFpReg(fpReg, fpVal))
	{
	  out << (boost::format("0x%016x") % fpVal) << std::endl;
	  return true;
	}
      cerr << "Error: Failed to read fp register: " << addrStr << '\n';
      return false;
    }

  if (resource == "c")
    {
      if (addrStr == "all")
	{
	  peekAllCsrs(hart, out);
	  return true;
	}

      auto csr = hart.findCsr(addrStr);
      if (not csr)
	{
	  cerr << "Error: No such CSR: " << addrStr << '\n';
	  return false;
	}

      bool virtMode = false;
      if (tokens.size() > 3)
        if (not parseCmdLineNumber("peek-csr-virt-mode", tokens.at(3), virtMode))
          return false;

      if (hart.peekCsr(csr->getNumber(), val, virtMode))
        {
          out << (boost::format(hexForm) % val) << std::endl;
	  return true;
	}
      cerr << "Failed to read CSR: " << addrStr << '\n';
      return false;
    }

  if (resource == "v")
    {
      if (not hart.isRvv())
	{
	  cerr << "Error: Vector extension is no enabled\n";
	  return false;
	}

      if (addrStr == "all")
	{
	  peekAllVecRegs(hart, out);
	  return true;
	}

      unsigned vecReg = 0;
      if (not hart.findVecReg(addrStr, vecReg))
	{
	  cerr << "Error: No such vector register: " << addrStr << '\n';
	  return false;
	}
      std::vector<uint8_t> data;
      if (hart.peekVecReg(vecReg, data))
	{
	  // Print data in reverse order (most significant byte first).
	  out << "0x";
	  for (unsigned byte : data)
	    out << (boost::format("%02x") % byte);
	  out << '\n';
	  return true;
	}
      cerr << "Error: Failed to read vector register: " << addrStr << '\n';
      return false;
    }

  if (resource == "t")
    {
      if (addrStr == "all")
	{
	  peekAllTriggers(hart, out);
	  return true;
	}

      URV trigger = 0;
      if (not parseCmdLineNumber("trigger-number", addrStr, trigger))
	return false;
      uint64_t v1(0), v2(0), v3(0);
      if (hart.peekTrigger(trigger, v1, v2, v3))
	{
	  out << (boost::format(hexForm) % v1) << ' '
              << (boost::format(hexForm) % v2) << ' '
              << (boost::format(hexForm) % v3) << std::endl;
	  return true;
	}
      cerr << "Error: Trigger number out of bounds: " << addrStr << '\n';
      return false;
    }

  if (resource == "s")
    {
      bool ok = true;
      if (addrStr == "pm")
	out << unsigned(hart.privilegeMode()) << std::endl;
      else if (addrStr == "ppm")
	out << unsigned(hart.lastPrivMode()) << std::endl;
      else if (addrStr == "iff")
	out << (boost::format("0x%x") % hart.lastFpFlags()) << std::endl;
      else if (addrStr == "iv")
        {
          std::vector<uint8_t> fpFlags; std::vector<uint8_t> vxsat;
          std::vector<VecRegs::Step> steps;
          hart.lastIncVec(fpFlags, vxsat, steps);
          std::reverse(fpFlags.begin(), fpFlags.end());
          std::reverse(vxsat.begin(), vxsat.end());

          std::string name = (not fpFlags.empty())? "fflags" : "vxsat";
          auto& buf = (not fpFlags.empty())? fpFlags : vxsat;

          std::string sep = "";
          out << name << ": [";
          for (uint8_t element : buf)
            {
              out << sep << (boost::format("0x%x") % unsigned(element));
              sep = ",";
            }
          out << "]\n";

          using VS = VecRegs::Step;
          VS::Operation op = VS::Operation::None;
          if (not steps.empty())
            {
              out << "\nsteps:\n";
              for (auto step : steps)
                {
                  if (op != step.op_)
                    out << VS::opToStr(step.op_) << "\n";

                  out << "[e1: " << (boost::format("0x%x") % step.operands_.at(0))
                      << " e2: " << (boost::format("0x%x") % step.operands_.at(1))
                      << " result: " << (boost::format("0x%x") % step.result_)
                      << "]\n";
                  op = step.op_;
                }
            }
        }
      else if (addrStr == "trap")
	out << (hart.lastInstructionTrapped() ? "1" : "0") << std::endl;
      else if (addrStr == "defi")
	out << (boost::format("0x%x") % hart.deferredInterrupts()) << std::endl;
      else if (addrStr == "seipin")
	out << (boost::format("%d") % hart.getSeiPin()) << std::endl;
      else if (addrStr == "effma")
	{
	  uint64_t va = 0, pa = 0;
	  if (hart.lastLdStAddress(va, pa))
	    {
	      auto pma = hart.getPma(pa);
              auto& virtMem = hart.virtMem();
	      auto effpbmt = virtMem.lastEffectivePbmt();
	      pma = hart.overridePmaWithPbmt(pma, effpbmt);
              out << (boost::format("0x%x") % pma.attributesToInt()) << std::endl;
	    }
	}
      else if (addrStr == "lastldst")
    {
      uint64_t va = 0, pa = 0;
      if (hart.lastLdStAddress(va, pa))
	out << (boost::format("0x%x") % pa) << std::endl;
    }
      else
	ok = false;

      if (ok)
	return true;

      cerr << "Error: Invalid special resource: " << addrStr << '\n';
      return false;
    }

  cerr << "Error: No such resource: " << resource << " -- expecting r, f, v, c, t, m , s, or pc\n";
  return false;
}


template <typename URV>
bool
Interactive<URV>::pokeCommand(Hart<URV>& hart, const std::string& line,
			      const std::vector<std::string>& tokens)
{
  if (tokens.size() < 3)
    {
      cerr << "Error: Invalid poke command: " << line << '\n';
      cerr << "Error:   Expecting: poke pc <value>\n";
      cerr << "Error:     or       poke <resource> <address> <value>\n";
      cerr << "Error:     or       poke c <address> <value> <virt>\n";
      cerr << "Error:     or       poke t <number> <value1> <value2> <value3>\n";
      cerr << "Error:   where <resource> is one of r, f, c, t, pc or m\n";
      return false;
    }

  const std::string& resource = tokens.at(1);

  uint64_t value = 0;

  if (resource == "pc")
    {
      if (not parseCmdLineNumber("pc", tokens.at(2), value))
	return false;
      hart.pokePc(value);
      return true;
    }

  size_t count = tokens.size();
  if ((resource == "t" and count < 6) or (resource != "t" and count < 4))
    {
      cerr << "Error: Invalid poke command: " << line << '\n';
      cerr << "Error:   Expecting: poke <resource> <address> <value>\n";
      cerr << "Error:     or       poke t <number> <value1> <value2> <value3>\n";
      cerr << "Error:   where <resource> is one of r, f, c, t, pc, or m\n";
      return false;
    }

  const std::string& addrStr = tokens.at(2);
  const std::string& valueStr = tokens.at(3);

  std::vector<uint8_t> vecVal;

  if (resource == "v")
    {
      if (not parseCmdLineVecData("poke", valueStr, vecVal))
	{
	  cerr << "Error:   " << line << '\n';
	  return false;
	}
    }
  else
    {
      if (not parseCmdLineNumber("poke", valueStr, value))
	{
	  cerr << "Error:  " << line << '\n';
	  return false;
	}
    }

  if (resource == "r")
    {
      unsigned intReg = 0;
      if (hart.findIntReg(addrStr, intReg))
	{
	  if (hart.pokeIntReg(intReg, value))
	    return true;
	  cerr << "Error: Failed to write integer register " << addrStr << '\n';
	  return false;
	}

      cerr << "Error: No such integer register " << addrStr << '\n';
      return false;
    }

  if (resource == "f")
    {
      unsigned fpReg = 0;
      if (hart.findFpReg(addrStr, fpReg))
	{
	  if (hart.pokeFpReg(fpReg, value))
	    return true;
	  cerr << "Error: Failed to write FP register " << addrStr << '\n';
	  return false;
	}

      cerr << "Error: No such FP register " << addrStr << '\n';
      return false;
    }

  if (resource == "v")
    {
      unsigned vecReg = 0;
      if (hart.findVecReg(addrStr, vecReg))
	{
	  if (hart.pokeVecReg(vecReg, vecVal))
	    return true;
	  cerr << "Error: Failed to write vec register " << addrStr << '\n';
	  return false;
	}

      cerr << "Error: No such vector register " << addrStr << '\n';
      return false;
    }

  if (resource == "c")
    {
      auto csr = hart.findCsr(addrStr);
      if (csr)
	{
          bool virtMode = false;
          if (tokens.size() > 4)
            if (not parseCmdLineNumber("poke-csr-virt-mode", tokens.at(4), virtMode))
              return false;

	  if (hart.externalPokeCsr(csr->getNumber(), value, virtMode))
	    return true;
	  cerr << "Error: Failed to write CSR " << addrStr << '\n';
	  return false;
	}

      cerr << "Error: No such CSR " << addrStr << '\n';
      return false;
    }

  if (resource == "t")
    {
      URV trigger = 0, v1 = 0, v2 = 0, v3 = 0;
      if (not parseCmdLineNumber("trigger", addrStr, trigger))
	return false;
      if (not parseCmdLineNumber("value1", tokens.at(3), v1))
	return false;
      if (not parseCmdLineNumber("value2", tokens.at(4), v2))
	return false;
      if (not parseCmdLineNumber("value3", tokens.at(5), v3))
	return false;
      if (hart.pokeTrigger(trigger, v1, v2, v3))
	return true;
      cerr << "Error: Trigger out of bounds: " << addrStr << '\n';
      return false;
    }

  if (resource == "m")
    {
      unsigned size = 4;  // Default size is 4 bytes.
      if (tokens.size() > 4)
	if (not parseCmdLineNumber("size", tokens.at(4), size))
	  return false;
      bool cache = false;
      if (tokens.size() > 5)
        if (not parseCmdLineNumber("cache", tokens.at(5), cache))
          return false;
      bool skipMem = false;
      if (tokens.size() > 5)
        if (not parseCmdLineNumber("skipMem", tokens.at(5), skipMem))
          return false;
      size_t addr = 0;
      if (not parseCmdLineNumber("address", addrStr, addr))
	return false;
      bool usePma = false; // Ignore physical memory attributes.
      switch (size)
	{
	case 1:
	  if (hart.pokeMemory(addr, uint8_t(value), usePma, false, not cache, skipMem))
	    return true;
	  break;
	case 2:
	  if (hart.pokeMemory(addr, uint16_t(value), usePma, false, not cache, skipMem))
	    return true;
	  break;
	case 4:
	  if (hart.pokeMemory(addr, uint32_t(value), usePma, false, not cache, skipMem))
	    return true;
	  break;
	case 8:
	  if (hart.pokeMemory(addr, value, usePma, false, not cache, skipMem))
	    return true;
	  break;
	default:
	  cerr << "Error: Invalid poke memory size " << size << '\n';
	  return false;
	}
      cerr << "Error: Address out of bounds: " << addrStr << '\n';
      return false;
    }

  if (resource == "s")
    {
      size_t addr = 0;
      URV val = 0;
      if (addrStr == "defi" or
	  (parseCmdLineNumber("special-resoure", addrStr, addr) and
	   addr == WhisperSpecialResource::DeferredInterrupts))
	{
	  if (not parseCmdLineNumber("value1", tokens.at(3), val))
	    return false;
	  hart.setDeferredInterrupts(val);
	}
      else if (addrStr == "seipin")
	{
	  if (not parseCmdLineNumber("value1", tokens.at(3), val))
	    return false;
	  hart.setSeiPin(val);
	}
      return true;
    }

  cerr << "Error: No such resource: " << resource << " -- expecting r, c, m, s, or pc\n";
  return false;
}


template <typename URV>
bool
Interactive<URV>::disassCommand(Hart<URV>& hart, const std::string& line,
				const std::vector<std::string>& tokens)
{
  if (tokens.size() >= 2 and tokens.at(1) == "opcode")
    {
      for (size_t i = 2; i < tokens.size(); ++i)
	{
	  uint32_t code = 0;
	  if (not parseCmdLineNumber("opcode", tokens[i], code))
	    return false;
	  std::string str;
	  hart.disassembleInst(code, str);
	  out_ << "  " << tokens[i] << ":  " << str << '\n';
	}
      return true;
    }

  auto hexForm = getHexForm<URV>(); // Format string for printing a hex val

  if (tokens.size() == 3 and (tokens.at(1) == "func" or
			      tokens.at(1) == "function"))
    {
      const std::string& item = tokens.at(2);
      std::string name;  // item (if a symbol) or function name containing item
      ElfSymbol symbol;
      if (system_.findElfSymbol(item, symbol))
	name = item;
      else
	{
	  // See if address falls in a function, then disassemble function.
	  URV addr = 0;
	  if (not parseCmdLineNumber("address", item, addr))
	    return false;

	  // Find function containing address.
	  hart.findElfFunction(addr, name, symbol);
	}

      if (name.empty())
	{
	  cerr << "Error: Not a function or an address withing a function: " << item << '\n';
	  return false;
	}

      out_ << "disassemble function " << name << ":\n";

      size_t start = symbol.addr_, end = symbol.addr_ + symbol.size_;
      for (size_t addr = start; addr < end; )
	{
	  uint32_t inst = 0;
          bool usePma = false;
	  if (not hart.peekMemory(addr, inst, usePma))
	    {
	      cerr << "Error: Address out of bounds: 0x" << std::hex << addr
		   << '\n' << std::dec;
	      return false;
	    }

	  unsigned instSize = instructionSize(inst);
	  if (instSize == 2)
	    inst = (inst << 16) >> 16; // Clear top 16 bits.

	  std::string str;
	  hart.disassembleInst(inst, str);
	  out_ << "  " << (boost::format(hexForm) % addr) << ' '
               << (boost::format(hexForm) % inst) << ' ' << str << '\n';

	  addr += instSize;
	}
      return true;
    }

  if (tokens.size() != 3)
    {
      cerr << "Error: Invalid disass command: " << line << '\n';
      cerr << "Error: Expecting: disass opcode <number> ...\n";
      cerr << "Error:        or: disass function <name>\n";
      cerr << "Error:        or: disass function <addr>\n";
      cerr << "Error:        or: disass <addr1> <addr2>\n";
      return false;
    }

  URV addr1, addr2;
  if (not parseCmdLineNumber("address", tokens[1], addr1))
    return false;

  if (not parseCmdLineNumber("address", tokens[2], addr2))
    return false;

  for (URV addr = addr1; addr <= addr2; )
    {
      uint32_t inst = 0;
      bool usePma = false;
      if (not hart.peekMemory(addr, inst, usePma))
	{
	  cerr << "Error: Address out of bounds: 0x" << std::hex << addr
	       << '\n' << std::dec;
	  return false;
	}

      unsigned instSize = instructionSize(inst);
      if (instSize == 2)
	inst = (inst << 16) >> 16; // Clear top 16 bits.

      std::string str;
      hart.disassembleInst(inst, str);
      out_ << (boost::format(hexForm) % addr) << ' '
           << (boost::format(hexForm) % inst) << ' '
           << str << '\n';

      addr += instSize;
    }

  return true;
}


template <typename URV>
bool
Interactive<URV>::elfCommand(Hart<URV>& hart, const std::string& line,
			     const std::vector<std::string>& tokens)
{
  if (tokens.size() != 2)
    {
      cerr << "Error: Invalid elf command: " << line << '\n';
      cerr << "Error: Expecting: elf <file-name>\n";
      return false;
    }

  const std::string& filePath = tokens.at(1);

  uint64_t entryPoint = 0;

  std::vector<std::string> files = { filePath };
  if (not system_.loadElfFiles(files, false /*raw*/, false /*verbose*/))
    return false;

  hart.pokePc(URV(entryPoint));

  return true;
}


template <typename URV>
bool
Interactive<URV>::hexCommand(Hart<URV>& , const std::string& line,
			     const std::vector<std::string>& tokens)
{
  if (tokens.size() != 2)
    {
      cerr << "Error: Invalid hex command: " << line << '\n';
      cerr << "Error: Expecting: hex <file-name>\n";
      return false;
    }

  std::vector<std::string> fileNames = { tokens.at(1) };
  return system_.loadHexFiles(fileNames, false /*verbse*/);
}


#if LZ4_COMPRESS
template <typename URV>
bool
Interactive<URV>::lz4Command(Hart<URV>& , const std::string& line,
			     const std::vector<std::string>& tokens)
{
  if (tokens.size() != 2)
    {
      cerr << "Error: Invalid lz4 command: " << line << '\n';
      cerr << "Error: Expecting: lz4 <file-name>\n";
      return false;
    }

  std::vector<std::string> fileNames = { tokens.at(1) };
  return system_.loadLz4Files(fileNames, 0, false /*verbose*/);
}
#endif


template <typename URV>
bool
Interactive<URV>::resetCommand(Hart<URV>& hart, const std::string& /*line*/,
			       const std::vector<std::string>& tokens)
{
  if (tokens.size() == 1)
    {
      hart.reset(resetMemoryMappedRegs_);
      return true;
    }

  if (tokens.size() == 2)
    {
      URV resetPc = 0;
      if (not parseCmdLineNumber("reset-pc", tokens[1], resetPc))
	return false;

      hart.defineResetPc(resetPc);
      hart.reset(resetMemoryMappedRegs_);
      return true;
    }

  cerr << "Error: Invalid reset command (extra arguments)\n";
  return false;
}


template <typename URV>
bool
Interactive<URV>::replayFileCommand(const std::string& line,
				    const std::vector<std::string>& tokens,
				    std::ifstream& stream)
{
  if (tokens.size() != 2)
    {
      cerr << "Error: Invalid replay_file command: " << line << '\n';
      cerr << "Error: Expecting: replay_file <file-name>\n";
      return false;
    }

  const std::string& fileName = tokens.at(1);

  stream.close();
  stream.open(fileName.c_str());
  if (not stream.good())
    {
      cerr << "Error: Failed to open replay-file '" << fileName << "'\n";
      return false;
    }

  return true;
}


template <typename URV>
bool
Interactive<URV>::dumpMemoryCommand(const std::string& line,
                                    const std::vector<std::string>& tokens)
{
  if (tokens.size() != 2)
    {
      cerr << "Error: Invalid dump_memory command: " << line << '\n';
      cerr << "Error:   Expecting: dump_memory path\n";
      return false;
    }

  return system_.writeAccessedMemory(tokens[1]);
}


/// Remove from the token vector tokens of the form key=value
/// and put them in the given map (which maps a key to a value).
static
void
extractKeywords(std::vector<std::string>& tokens,
		std::unordered_map<std::string, std::string, util::string_hash, std::equal_to<>>& strMap)
{
  size_t newSize = 0;
  for (size_t i = 0; i < tokens.size(); ++i)
    {
      const auto& token = tokens.at(i);
      auto pos = token.find('=');
      if (pos != std::string::npos)
	{
	  auto key = token.substr(0, pos);
	  auto value = token.substr(pos + 1);
	  strMap[key] = value;
	}
      else
	{
	  if (newSize < i)
	    tokens.at(newSize) = token;
	  newSize++;
	}
    }
  tokens.resize(newSize);
}


/// Interactive "help" command.
static
void
printInteractiveHelp(std::ostream& out)
{
  using std::cout;
  out << "The arguments hart=<id> and.or time=<tine> may be used with any command\n";
  out << "to select a hart and specify event time (relevant to memory model)\n";
  out << "They persist until explicitly changed.\n\n";
  out << "help [<command>]\n";
  out << "  Print help for given command or for all commands if no command given.\n\n";
  out << "run\n";
  out << "  Run till interrupted.\n\n";
  out << "until <address>\n";
  out << "  Run until address or interrupted.\n\n";
  out << "step [<n>]\n";
  out << "  Execute n instructions (1 if n is missing).\n\n";
  out << "peek <res> <addr>\n";
  out << "  Print value of resource res (one of r, f, c, v, m) and address addr.\n";
  out << "  For memory (m) up to 2 addresses may be provided to define a range\n";
  out << "  of memory locations to be printed; also, an optional filename after\n";
  out << "  the two addresses writes the command output to that file.\n";
  out << "  examples: peek r x1   peek c mtval   peek m 0x4096\n";
  out << "            peek m 0x10 0x40 out\n\n";
  out << "peek pc\n";
  out << "  Print value of the program counter.\n\n";
  out << "peek all\n";
  out << "  Print value of all non-memory resources\n\n";
  out << "poke res addr value\n";
  out << "  Set value of resource res (one of r, c or m) and address addr\n";
  out << "  Examples: poke r x1 0xff  poke c 0x4096 0xabcd\n\n";
  out << "disass opcode <code> <code> ...\n";
  out << "  Disassemble opcodes. Example: disass opcode 0x3b 0x8082\n\n";
  out << "disass function <name>\n";
  out << "  Disassemble function with given name. Example: disas func main\n\n";
  out << "disass <addr1> <addr2>>\n";
  out << "  Disassemble memory locations between addr1 and addr2.\n\n";
  out << "elf file\n";
  out << "  Load elf file into simulated memory.\n\n";
  out << "hex file\n";
  out << "  Load hex file into simulated memory.\n\n";
  out << "replay_file file\n";
  out << "  Open command file for replay.\n\n";
  out << "replay n\n";
  out << "  Execute the next n commands in the replay file or all the\n";
  out << "  remaining commands if n is missing.\n\n";
  out << "replay step n\n";
  out << "  Execute consecutive commands from the replay file until n\n";
  out << "  step commands are executed or the file is exhausted\n\n";
  out << "reset [<reset_pc>]\n";
  out << "  Reset hart.  If reset_pc is given, then change the reset program\n";
  out << "  counter to the given reset_pc before resetting the hart.\n\n";
  out << "symbols\n";
  out << "  List all the symbols in the loaded ELF file(s).\n\n";
  out << "pagetable\n";
  out << "  Print the entries of the address tanslation table.\n\n";
  out << "nmi [<cause-number>]\n";
  out << "  Post a non-maskable interrupt with a given cause number (default 0).\n\n";
  out << "clear_nmi\n";
  out << "  Clear a pending non-maskable interrupt.\n\n";
  out << "mread tag addr size data [vec-elem [vec-field]]\n";
  out << "  Perform a memory model (out of order) read for load/amo instruction with\n";
  out << "  given tag. Data is the RTL data to be compared with whisper data\n";
  out << "  when instruction is later retired. \n\n";
  out << "mbwrite addr data [[mask] [skip-check]]\n";
  out << "  Perform a memory model merge-buffer-write for given address. Given\n";
  out << "  data (hexadecimal string) is from a different model (RTL) and is compared\n";
  out << "  to whisper data. Addr should be a multiple of cache-line size. If hex\n";
  out << "  string is smaller than twice the cache-line size, it will be padded with\n";
  out << "  zeros on the most significant side.\n\n";
  out << "mbbypass tag addr size data\n";
  out << "  Perform a memory write operation bypassing the merge buffer. Given\n";
  out << "  data (hexadecimal string) is from a different model (RTL) and is compared\n";
  out << "  to whisper data.\n\n";
  out << "pmp [<address>]\n";
  out << "  Print the pmp map (all) or for a matching address\n\n";
  out << "pma [<address>]\n";
  out << "  Print the pma map (all) or for a matching address\n\n";
  out << "translate <va> [<permission> [<privilege>]]\n";
  out << "  Translate given virtual address <va> to a physical address assuming given\n";
  out << "  permission (defaults to read) and privilege mode (defaults to user)\n";
  out << "  Allowed permission: r for read, w for write, or x for execute.\n";
  out << "  Allowed privilege: u, s, vu, or vs for user, supervisor, guest-user, or guest-supervisor\n";
  out << "perf_model_fetch tag vpc\n";
  out << "  Perf model API only command. Constructs and fetches instruction packet\n";
  out << "perf_model_decode tag opcode\n";
  out << "  Perf model API only command. Decodes instruction packet\n";
  out << "perf_model_execute tag\n";
  out << "  Perf model API only command. Executes instruction packet\n";
  out << "perf_model_retire tag\n";
  out << "  Perf model API only command. Retires instruction packet\n";
  out << "perf_model_drain_store tag\n";
  out << "  Perf model API only command. Drains store associated with instruction packet\n";
  out << "perf_model_predict_branch\n";
  out << "  Perf model API only command. Record branch prediction for an instruction\n";
  out << "perf_model_flush tag\n";
  out << "  Perf model API only command. Flushes instruction packet\n";
  out << "perf_model_flush tag\n";
  out << "  Perf model API only command. Determines whether flushing is required\n";
  out << "quit\n";
  out << "  Terminate the simulator\n\n";
}


template <typename URV>
void
Interactive<URV>::helpCommand(const std::vector<std::string>& tokens)
{
  if (tokens.size() <= 1)
    {
      printInteractiveHelp(out_);
      return;
    }

  const auto& tag = tokens.at(1);
  if (tag == "help")
    {
      out_ << "help [<command>]\n"
	   << "  Print information about interactive commands. If a command\n"
	   << "  argument is given, print info about that command.\n";
      return;
    }

  if (tag == "run")
    {
      out_ << "run\n"
	   << "  Run the target program until it exits (in newlib emulation mode),\n"
	   << "  it writes into the \"tohost\" location, or the user interrupts\n"
	   << "  it by pressing control-c on the keyboard.\n";
      return;
    }

  if (tag == "until")
    {
      out_ << "until <address>\n"
	   << "  Same as run but the target program will also stop when the\n"
	   << "  instruction at the given address is reached (but before it is\n"
	   << "  executed).\n";
      return;
    }

  if (tag == "step")
    {
      out_ << "step [<n>]\n"
	   << "  Execute a single instruction. If an integer argument <n> is\n"
	   << "  given, then execute up to n instructions or until a stop\n"
	   << "  condition (see run command) is encountered\n";
      return;
    }

  if (tag == "peek")
    {
      out_ << "peek <res> <addr>\n"
	   << "peek m <addr> [<addr>] [<file>]\n"
	   << "peek pc\n"
	   << "peek s  pm | ppm | iff | trap\n"
	   << "  Show the contents of the item at the given address within the given\n"
	   << "  resource. Possible resources are r, f, c, v, m, or s for integer, FP,\n"
	   << "  CSR, vector register, memory, or special respectively. Addr stands for a\n"
	   << "  register number, register name, or memory address. If resource is\n"
	   << "  memory (m), then an additional address may be provided to define a\n"
	   << "  range of memory locations to be display and an optional filename\n"
	   << "  after 2nd address may be provided to write memory contents to a file.\n"
	   << "  Vector register values are printed just like intger register (most\n"
	   << "  significant byte first). If resource is special (s) then following\n"
	   << "  special items may be queried: pm, ppm, iff, and trap which stand for\n"
	   << "  privilege-mode, previous-privilege-mode, incremental-fp-flags, and\n"
	   << "  whether or not the last executed instruction took a trap.\n"
	   <<"   Examples:\n"
	   << "    peek pc\n"
	   << "    peek r t0\n"
	   << "    peek r x12\n"
	   << "    peek c mtval\n"
	   << "    peek v v2\n"
	   << "    peek s pm\n"
	   << "    peek m 0x80000000\n"
	   << "    peek m 0x80000000 0x80000010\n"
      	   << "    peek m 0x80000000 0x80000010 out\n";
      return;
    }

  if (tag == "poke")
    {
      out_ << "poke <res> <addr> <value>\n"
	   << "poke pc <value>\n"
	   << "  Set the entry with the given address wihinin the given resource to\n"
	   << "  the given value. Possible resources are r, f, c, v, or m for integer,\n"
	   << "  FP, CSR, vector register or for memory respectively. Addr stands for\n"
	   << "  a register number, register name or memory address. Vector Register\n"
	   << "  poke values are expected in most significant byte first order.\n"
	   << "  Values of FP registers are expected in decimal or hexadcecimal notation\n"
	   << "  and they denote the bit patterns to be placed in those registers.\n"
	   << "  The memory poke unit is 1 word (4 byes).  Examples:\n"
	   << "    poke r t0 0\n"
	   << "    poke r x12 0x44\n"
	   << "    poke c mtval 0xff\n"
	   << "    poke m 0x80000000 0xabdcffff\n";
      return;
    }

  if (tag == "disas" or tag == "disass")
    {
      out_ << "disass opcode <op0> <op1> ...\n"
	   << "disass func <address>\n"
	   << "disass <addr1> <addr2>\n"
	   << "  The first form will disassemble the given opcodes.\n"
	   << "  The second form will disassemble the instructions of the\n"
	   << "  function containing the given address.\n"
	   << "  The third form will disassemble the memory contents between\n"
	   << "  addresses addr1 and addr2 inclusive.\n";
      return;
    }

  if (tag == "elf")
    {
      out_ << "elf <file> ...\n"
	   << "  Load into memory the contents of the given ELF file.\n"
	   << "  Set the program counter to the value of the ELF file entry point.\n"
	   << "  If the file contains the symbol \"tohost\" then subsequent writes\n"
	   << "  to the corresponding address will stop the simulation.\n";
      return;
    }

  if (tag == "replay_file")
    {
      out_ << "replay_file <file> ...\n"
	   << "  Define the input replay file to serve as input for the replay\n"
	   << "  command. The user would typically load the commands of a session\n"
	   << "  and replays them in a subsequent session.\n";
      return;
    }

  if (tag == "replay")
    {
      out_ << "replay [step] [<n>]\n"
	   << "  Without any arguments, replay all remaining commands in the\n"
	   << "  replay file (defined by the replay_file command).\n"
	   << "  With the keyword step, key-in on step commands in the replay\n"
	   << "  file. With an integer number n, replay n commands (or n step\n"
	   << "  commands if step keyword is present).\n";
      return;
    }

  if (tag == "reset")
    {
      out_ << "reset [<reset_pc>]\n"
	   << "  Reset simulated processor. If reset_pc is given, then change\n"
	   << "  reset program counter to the given reset_pc before resetting\n"
	   << "  the processor.\n";
      return;
    }

  if (tag == "quit")
    {
      out_ << "quit\n"
	   << "  Terminate the simulator.\n";
      return;
    }

  cerr << "Error: No such command: " << tag	<< '\n';
}


template <typename URV>
bool
Interactive<URV>::processKeywords(const StringMap& strMap)
{
  unsigned errors = 0;
  for (const auto& kv : strMap)
    {
      const auto& key = kv.first;
      const auto& valueStr = kv.second;
      
      if (key == "hart" or key == "h")
	{
	  uint64_t val = 0;
	  if (not parseCmdLineNumber(key, valueStr, val))
	    errors++;
	  hartId_ = val;
	}
      else if (key == "time" or key == "t")
	{
	  uint64_t val = 0;
	  if (not parseCmdLineNumber(key, valueStr, val))
	    errors++;
	  time_ = val;
	}
      else
	{
	  if (key.empty())
	    cerr << "Error: Empty key -- ignored\n";
	  else
	    cerr << "Error: Unknown key: " << key << "  -- ignored\n";
          errors++;
	}
    }

  return errors == 0;
}
  

/// Command line interpreter: Execute a command line.
template <typename URV>
bool
Interactive<URV>::executeLine(const std::string& inLine, FILE* traceFile,
			      FILE* commandLog,
			      std::ifstream& replayStream, bool& done)
{
  // Remove comments (anything starting with #).
  std::string line = inLine;
  auto sharpIx = line.find_first_of('#');
  if (sharpIx != std::string::npos)
    line = line.substr(0, sharpIx);

  // Remove leading/trailing white space
  boost::algorithm::trim_if(line, boost::is_any_of(" \t"));

  if (line.empty())
    return true;

  // Break line into tokens.
  std::vector<std::string> tokens;
  boost::split(tokens, line, boost::is_any_of(" \t"),
	       boost::token_compress_on);
  if (tokens.empty())
    return true;

  StringMap strMap;
  extractKeywords(tokens, strMap);

  bool ok = processKeywords(strMap);

  if (tokens.empty())
    {
      if (ok and commandLog and not strMap.empty())
	fprintf(commandLog, "%s\n", line.c_str());
      return ok;
    }

  // If there is a quit command ececute it regardless of errors.
  const std::string& command = tokens.front();
  if (command == "q" or command == "quit")
    {
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      done = true;
      return true;
    }

  if (not ok)
    return false;

  auto hartPtr = system_.findHartByHartId(hartId_);
  if (not hartPtr)
    {
      cerr << "Error: Hart id out of bounds: " << hartId_ << '\n';
      return false;
    }

  Hart<URV>& hart = *hartPtr;

  // After the first step/run/until command, a reset command will reset
  // the memory mapped registers.

  if (command == "s" or command == "step")
    {
      resetMemoryMappedRegs_ = true;
      if (not stepCommand(hart, line, tokens, traceFile))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "peek")
    {
      if (not peekCommand(hart, line, tokens, out_))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
       return true;
    }

  if (command == "mread" or command == "memory_model_read")
    {
      if (not mreadCommand(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "mbinsert" or command == "merge_buffer_insert")
    {
      if (not mbinsertCommand(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "run")
    {
      resetMemoryMappedRegs_ = true;
      bool success = runCommand(hart, line, tokens, traceFile);
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return success;
    }

  if (command == "u" or command == "until")
    {
      resetMemoryMappedRegs_ = true;
      bool success = untilCommand(hart, line, tokens, traceFile);
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return success;
    }

  if (command == "poke")
    {
      if (not pokeCommand(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "d" or command == "disas" or command == "disass")
    {
      if (not disassCommand(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "elf")
    {
      if (not elfCommand(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "hex")
    {
      if (not hexCommand(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

#if LZ4_COMPRESS
  if (command == "lz4")
    {
      if (not lz4Command(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }
#endif

  if (command == "reset")
    {
      if (not resetCommand(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "enter_debug")
    {
      hart.enterDebugMode(hart.peekPc());
      if (commandLog)
	{
	  fprintf(commandLog, "%s", line.c_str());
	  if (tokens.size() == 1)
	    fprintf(commandLog, " false");
	  fprintf(commandLog, "\n");
	}
      return true;
    }

  if (command == "exit_debug")
    {
      hart.exitDebugMode();
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "cancel_div")
    {
      if (not hart.cancelLastDiv())
        cerr << "Error: Unexpected cancel_div\n";
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "cancel_lr")
    {
      hart.cancelLr(CancelLrCause::INTERACTIVE);
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "replay_file")
    {
      return replayFileCommand(line, tokens, replayStream);
    }

  if (command == "replay")
    {
      if (not replayStream.is_open())
	{
	  cerr << "Error: No replay file defined. Use the replay_file to define one\n";
	  return false;
	}
      bool replayDone = false;
      return replayCommand(line, tokens, traceFile, commandLog,
                           replayStream, replayDone);
    }

  if (command == "symbols")
    {
      system_.printElfSymbols(out_);
      return true;
    }

  if (command == "pagetable")
    {
      hart.printPageTable(out_);
      return true;
    }

  if (command == "nmi")
    {
      uint32_t cause = 0;
      if (tokens.size() > 1 and not parseCmdLineNumber("nmi-cause", tokens.at(1), cause))
	return false;
      hart.setPendingNmi(NmiCause(cause));
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "clear_nmi")
    {
      hart.clearPendingNmi();
      if (commandLog)
        fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "dump_memory")
    {
      if (not dumpMemoryCommand(line, tokens))
        return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "mbwrite" or command == "merge_buffer_write")
    {
      if (not mbwriteCommand(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "mbypass" or command == "mbbypass" or command == "merge_buffer_bypass")
    {
      if (not mbbypassCommand(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "mifetch")
    {
      if (not mifetchCommand(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "mievict")
    {
      if (not mievictCommand(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "mdfetch")
    {
      if (not mdfetchCommand(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "mdevict")
    {
      if (not mdevictCommand(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "mdwriteback")
    {
      if (not mdwritebackCommand(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "mskipreadchk")
    {
      if (not mskipReadChkCommand(hart, line, tokens))
        return false;
      if (commandLog)
        fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "end_mcm")
    {
      system_.endMcm();
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "translate")
    {
      if (not translateCommand(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "check_interrupt")
    {
      if (not checkInterruptCommand(hart, line, tokens))
	return false;
      if (commandLog)
	fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "pmp")
    {
      if (not pmpCommand(hart, line, tokens))
        return false;
      if (commandLog)
        fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "pma")
    {
      if (not pmaCommand(hart, line, tokens))
        return false;
      if (commandLog)
        fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "inject_exception")
    {
      if (not injectExceptionCommand(hart, line, tokens))
        return false;
      if (commandLog)
        fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "perf_model_fetch")
    {
      if (not perfModelFetchCommand(line, tokens))
        return false;
      if (commandLog)
        fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "perf_model_decode")
    {
      if (not perfModelDecodeCommand(line, tokens))
        return false;
      if (commandLog)
        fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "perf_model_execute")
    {
      if (not perfModelExecuteCommand(line, tokens))
        return false;
      if (commandLog)
        fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "perf_model_retire")
    {
      if (not perfModelRetireCommand(line, tokens))
        return false;
      if (commandLog)
        fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "perf_model_drain_store")
    {
      if (not perfModelDrainStoreCommand(line, tokens))
        return false;
      if (commandLog)
        fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "perf_model_predict_branch")
    {
      if (not perfModelPredictBranch(line, tokens))
        return false;
      if (commandLog)
        fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "perf_model_flush")
    {
      if (not perfModelFlushCommand(line, tokens))
        return false;
      if (commandLog)
        fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "perf_model_should_flush")
    {
      if (not perfModelShouldFlushCommand(line, tokens))
        return false;
      if (commandLog)
        fprintf(commandLog, "%s\n", line.c_str());
      return true;
    }

  if (command == "h" or command == "?" or command == "help")
    {
      helpCommand(tokens);
      return true;
    }

  cerr << "Error: No such command: " << line << '\n';
  return false;
}


/// Interactive "replay" command.
template <typename URV>
bool
Interactive<URV>::replayCommand(const std::string& line,
				const std::vector<std::string>& tokens,
				FILE* traceFile, FILE* commandLog,
				std::ifstream& replayStream, bool& done)
{
  std::string replayLine;
  uint64_t maxCount = ~uint64_t(0);  // Unlimited

  if (tokens.size() <= 2)    // Either replay or replay n.
    {
      if (tokens.size() == 2)
	if (not parseCmdLineNumber("command-count", tokens.at(1), maxCount))
	  return false;

      uint64_t count = 0;
      while (count < maxCount  and  not done  and
	     std::getline(replayStream, replayLine))
	{
	  if (not executeLine(replayLine, traceFile,
			      commandLog, replayStream, done))
	    return false;
	  count++;
	}
      return true;
    }

  if (tokens.size() == 3)
    {
      if (tokens.at(1) != "step")
	{
	  cerr << "Error: Invalid command: " << line << '\n';
	  cerr << "Error: Expecting: replay <step> <count>\n";
	  return false;
	}

      if (not parseCmdLineNumber("step-count", tokens.at(2), maxCount))
	return false;
      
      uint64_t count = 0;
      while (count < maxCount  and  not done   and
	     std::getline(replayStream, replayLine))
	{
	  if (not executeLine(replayLine, traceFile,
			      commandLog, replayStream, done))
	    return false;

	  std::vector<std::string> tokens;
	  boost::split(tokens, replayLine, boost::is_any_of(" \t"),
		       boost::token_compress_on);
          if (std::ranges::any_of(std::span(tokens).first(std::min(tokens.size(), std::size_t{2})),
                                  [](const auto& token) { return token == "step"; }))
	    count++;
	}

      return true;
    }

  cerr << "Error: Invalid command: " << line << '\n';
  cerr << "Error: Expecting: replay, replay <count>, or replay step <count>\n";
  return false;    
}


template <typename URV>
bool
Interactive<URV>::mreadCommand(Hart<URV>& hart, const std::string& line,
			       const std::vector<std::string>& tokens)
{
  // Format: mread <instr-tag> <physical-address> <size> <rtl-data> [<elem> [<field>]]
  if (tokens.size() < 5)
    {
      cerr << "Error: Invalid mread command: " << line << '\n';
      cerr << "Error:   Expecting: mread <tag> <addr> <size> <data>\n";
      return false;
    }

  uint64_t tag = 0;
  if (not parseCmdLineNumber("instruction-tag", tokens.at(1), tag))
    return false;

  uint64_t addr = 0;
  if (not parseCmdLineNumber("address", tokens.at(2), addr))
    return false;

  uint64_t size = 0;
  if (not parseCmdLineNumber("size", tokens.at(3), size))
    return false;

  if (size == 0)
    {
      cerr << "Error: Invalid mread size: 0\n";
      return false;
    }

  unsigned elem = 0, field = 0;
  if (tokens.size() > 5)
    if (not parseCmdLineNumber("element-index", tokens.at(5), elem))
      return false;

  if (tokens.size() > 6)
    if (not parseCmdLineNumber("element-field", tokens.at(6), field))
      return false;

  if (size <= 8)
    {
      uint64_t data = 0;
      if (not parseCmdLineNumber("data", tokens.at(4), data))
	return false;
      return system_.mcmRead(hart, this->time_, tag, addr, size, data, elem, field);
    }

  // mread for a vector load, expected size is half a cache line size (32)
  // or a cache line size (64).

  std::vector<uint8_t> bytes;
  if (not parseCmdLineVecData("data", tokens.at(4), bytes))
    {
      cerr << "Error:   " << line << '\n';
      return false;
    }

  if (bytes.size() != size)
    {
      cerr << "Error: Invalid mread command: size (" << size << ") does not match number "
	   << " of bytes in data (" << bytes.size() << "\n";
      std::cerr << "Error:   " << line << '\n';
      return false;
    }

  unsigned clz = hart.cacheLineSize();
  if (size > clz)
    {
      cerr << "Error: Invalid size for mread command for vector: " << size << ", must be "
	   << "less than cache line size (" << clz << ")\n";
      cerr << "Error:   " << line << '\n';
      return false;
    }

  bool ok = true;

  std::reverse(bytes.begin(), bytes.end());

  // For speed, use double-word mread when possible, else word, else byte.
  if ((size & 0x7) == 0 and (addr & 0x7) == 0)
    {
      const uint64_t* vdata = reinterpret_cast<const uint64_t*> (bytes.data());
      for (unsigned i = 0; i < size and ok; i += 8, ++vdata, addr += 8)
	ok = system_.mcmRead(hart, this->time_, tag, addr, 8, *vdata, elem, field);
    }
  else if ((size & 0x3) == 0 and (addr & 0x3) == 0)
    {
      const uint32_t* vdata = reinterpret_cast<const uint32_t*> (bytes.data());
      for (unsigned i = 0; i < size and ok; i += 4, ++vdata, addr += 4)
	ok = system_.mcmRead(hart, this->time_, tag, addr, 4, *vdata, elem, field);
    }
  else
    {
      const uint8_t* vdata = bytes.data();
      for (unsigned i = 0; i < size and ok; ++i, ++vdata, ++addr)
	ok = system_.mcmRead(hart, this->time_, tag, addr, 1, *vdata, elem, field);
    }

  return ok;
}


template <typename URV>
bool
Interactive<URV>::mbwriteCommand(Hart<URV>& hart, const std::string& line,
				 const std::vector<std::string>& tokens)
{
  // Format: mbwrite <physical-address> <rtl-data> [<mask> [<skip-check>]]
  // Data is up to 64 bytes (each byte is 2 hex digits) with least significant
  // byte (rightmost two hex digits) corresponding to smallest address.
  if (tokens.size() < 3 or tokens.size() > 5)
    {
      cerr << "Error: Invalid mbwrite command: " << line << '\n';
      cerr << "Error:   Expecting: mbwrite <addr> <data> <mask> [<skip-check>]\n";
      return false;
    }

  uint64_t addr = 0;
  if (not parseCmdLineNumber("address", tokens.at(1), addr))
    return false;

  std::vector<uint8_t> data;
  std::string hexDigits = tokens.at(2);

  if (not (hexDigits.starts_with("0x") or
	   hexDigits.starts_with("0X")))
    {
      cerr << "Error: mbwrite data must begin with 0x: " << hexDigits << '\n';
      return false;
    }
  hexDigits = hexDigits.substr(2);

  if ((hexDigits.size() & 1) == 1)
    {
      cerr << "Error: mbwrite hex digit count must be even\n";
      return false;
    }

  for (size_t i = 0; i < hexDigits.size(); i += 2)
    {
      std::string byteStr = hexDigits.substr(i, 2);
      char* end = nullptr;
      unsigned value = strtoul(byteStr.c_str(), &end, 16);
      if (end and *end)
	{
	  cerr << "Error: Invalid hex digit(s) in mbwrite data: " << byteStr << '\n';
	  return false;
	}
      data.push_back(value);
    }

  unsigned lineSize = system_.mergeBufferSize();
  std::reverse(data.begin(), data.end()); // Least sig byte now first
  if (data.size() > lineSize)
    {
      cerr << "Error: Mbwrite data too long -- truncating\n";
      data.resize(lineSize);
    }
		     
  std::vector<bool> mask;
  if (tokens.size() > 3)
    {
      hexDigits = tokens.at(3);
      if (not (hexDigits.starts_with("0x")) or not (hexDigits.starts_with("0x")))
        {
          cerr << "Error: Error mbwrtie mask myst begin with 0x: " << hexDigits << '\n';
          return false;
        }
      hexDigits = hexDigits.substr(2);
      if ((hexDigits.size() & 1) == 1)
        {
          cerr << "Error: mbwrite hex digit count must be even\n";
          return false;
        }
      for (size_t i = 0; i < hexDigits.size(); i += 2)
        {
          std::string byteStr = hexDigits.substr(i, 2);
          char* end = nullptr;
          unsigned value = strtoul(byteStr.c_str(), &end, 16);
          if (end and *end)
            {
              cerr << "Error: Invalid hex digit(s) in mbwrite data: " << byteStr << '\n';
              return false;
            }
          for (unsigned j = 0; j < 8; ++j)
            {
              unsigned bit = (value >> (7-j)) & 1;
              mask.push_back(bit);
            }
        }
      std::reverse(mask.begin(), mask.end());
    }

  bool skipCheck = false;
  if (tokens.size() == 5 and not parseCmdLineBool("skip-check", tokens.at(4), skipCheck))
    return false;

  return system_.mcmMbWrite(hart, this->time_, addr, data, mask, skipCheck);
}


template <typename URV>
bool
Interactive<URV>::mbinsertCommand(Hart<URV>& hart, const std::string& line,
				  const std::vector<std::string>& tokens)
{
  // Format: mbinsert <instr-tag> <physical-address> <size> <rtl-data> [<elem> [<field>]]
  if (tokens.size() < 5)
    {
      cerr << "Error: Invalid mbinsert command. Expecting: mbinsert <tag> <addr> size> <data>\n";
      cerr << "Error:   " << line << '\n';
      return false;
    }

  uint64_t tag = 0;
  if (not parseCmdLineNumber("instruction-tag", tokens.at(1), tag))
    return false;

  uint64_t addr = 0;
  if (not parseCmdLineNumber("address", tokens.at(2), addr))
    return false;

  uint64_t size = 0;
  if (not parseCmdLineNumber("size", tokens.at(3), size))
    return false;

  unsigned elem = 0, field = 0;
  if (tokens.size() > 5)
    if (not parseCmdLineNumber("element-index", tokens.at(5), elem))
      return false;

  if (tokens.size() > 6)
    if (not parseCmdLineNumber("element-field", tokens.at(6), field))
      return false;

  if (size <= 8)
    {
      uint64_t data = 0;
      if (not parseCmdLineNumber("data", tokens.at(4), data))
	return false;
      return system_.mcmMbInsert(hart, this->time_, tag, addr, size, data, elem, field);
    }

  // mbinsert for a vector load, expected size is less than that of cache line (64).
  std::vector<uint8_t> bytes;
  if (not parseCmdLineVecData("data", tokens.at(4), bytes))
    return false;

  // Expand a zero to the required number of bytes. Backward ccompatibility.
  if (bytes.size() == 1 and bytes.at(0) == 0)
    bytes.resize(size);

  if (bytes.size() != size)
    {
      cerr << "Error: Invalid mbinsert command: size (" << size << ") does not match number "
	   << " of bytes in data (" << bytes.size() << ")\n";
      cerr << "Error:   " << line << '\n';
      return false;
    }

  unsigned clz = hart.cacheLineSize();
  if (size > clz)
    {
      cerr << "Error: Invalid size for mbinsert command for vector: " << size << ", must be "
	   << "less than cache line size (" << clz << ")\n";
      cerr << "Error:   " << line << '\n';
      return false;
    }

  bool ok = true;

  std::reverse(bytes.begin(), bytes.end());

  // For speed, use double-word insert when possible, else word, else byte.
  if ((size & 0x7) == 0 and (addr & 0x7) == 0)
    {
      const uint64_t* vdata = reinterpret_cast<const uint64_t*> (bytes.data());
      for (unsigned i = 0; i < size and ok; i += 8, ++vdata, addr += 8)
	ok = system_.mcmMbInsert(hart, this->time_, tag, addr, 8, *vdata, elem, field);
    }
  else if ((size & 0x3) == 0 and (addr & 0x3) == 0)
    {
      const uint32_t* vdata = reinterpret_cast<const uint32_t*> (bytes.data());
      for (unsigned i = 0; i < size and ok; i += 4, ++vdata, addr += 4)
	ok = system_.mcmMbInsert(hart, this->time_, tag, addr, 4, *vdata, elem, field);
    }
  else
    {
      const uint8_t* vdata = bytes.data();
      for (unsigned i = 0; i < size and ok; ++i, ++vdata, ++addr)
	ok = system_.mcmMbInsert(hart, this->time_, tag, addr, 1, *vdata, elem, field);
    }

  return ok;
}


template <typename URV>
bool
Interactive<URV>::mbbypassCommand(Hart<URV>& hart, const std::string& line,
				  const std::vector<std::string>& tokens)
{
  // Format: mbbypass <instr-tag> <physical-address> <size> <data> [<elem> [<field>]]
  if (tokens.size() < 5)
    {
      cerr << "Error: Invalid mbbypass command. Expecting: mbbypass <tag> <addr> size> <data>\n";
      cerr << "Error:   " << line << '\n';
      return false;
    }

  uint64_t tag = 0;
  if (not parseCmdLineNumber("instruction-tag", tokens.at(1), tag))
    return false;

  uint64_t addr = 0;
  if (not parseCmdLineNumber("address", tokens.at(2), addr))
    return false;

  uint64_t size = 0;
  if (not parseCmdLineNumber("size", tokens.at(3), size))
    return false;

  unsigned elem = 0, field = 0;
  if (tokens.size() > 5)
    if (not parseCmdLineNumber("element-index", tokens.at(5), elem))
      return false;

  if (tokens.size() > 6)
    if (not parseCmdLineNumber("element-field", tokens.at(6), field))
      return false;

  bool cache = false;
  if (tokens.size() > 7)
    if (not parseCmdLineNumber("cache", tokens.at(7), cache))
      return false;

  if (size <= 8)
    {
      uint64_t data = 0;
      if (not parseCmdLineNumber("data", tokens.at(4), data))
	return false;
      return system_.mcmBypass(hart, this->time_, tag, addr, size, data, elem, field, cache);
    }

  // mbbypass for a vector load, expected size is less than that of cache line (64).
  std::vector<uint8_t> bytes;
  if (not parseCmdLineVecData("data", tokens.at(4), bytes))
    return false;

  // Expand a zero to the required number of bytes. Backward ccompatibility.
  if (bytes.size() == 1 and bytes.at(0) == 0)
    bytes.resize(size);

  if (bytes.size() != size)
    {
      cerr << "Error: Invalid mbbypass command: size (" << size << ") does not match number "
	   << " of bytes in data (" << bytes.size() << ")\n";
      cerr << "Error:   " << line << '\n';
      return false;
    }

  unsigned clz = hart.cacheLineSize();
  if (size > clz)
    {
      cerr << "Error: Invalid size for mbbbypass command for vector: " << size << ", must be "
	   << "less than cache line size (" << clz << ")\n";
      cerr << "Error:   " << line << '\n';
      return false;
    }

  bool ok = true;

  std::reverse(bytes.begin(), bytes.end());

  // For speed, use double-word bypass when possible, else word, else byte.
  if ((size & 0x7) == 0 and (addr & 0x7) == 0)
    {
      const uint64_t* vdata = reinterpret_cast<const uint64_t*> (bytes.data());
      for (unsigned i = 0; i < size and ok; i += 8, ++vdata, addr += 8)
	ok = system_.mcmBypass(hart, this->time_, tag, addr, 8, *vdata, elem, field, cache);
    }
  else if ((size & 0x3) == 0 and (addr & 0x3) == 0)
    {
      const uint32_t* vdata = reinterpret_cast<const uint32_t*> (bytes.data());
      for (unsigned i = 0; i < size and ok; i += 4, ++vdata, addr += 4)
	ok = system_.mcmBypass(hart, this->time_, tag, addr, 4, *vdata, elem, field, cache);
    }
  else
    {
      const uint8_t* vdata = bytes.data();
      for (unsigned i = 0; i < size and ok; ++i, ++vdata, ++addr)
	ok = system_.mcmBypass(hart, this->time_, tag, addr, 1, *vdata, elem, field, cache);
    }

  return ok;
}


template <typename URV>
bool
Interactive<URV>::mifetchCommand(Hart<URV>& hart, const std::string& line,
				 const std::vector<std::string>& tokens)
{
  // Format: mifetch <physical-address>
  if (tokens.size() != 2)
    {
      cerr << "Error: Invalid mifetch command: " << line << '\n';
      cerr << "Error:   Expecting: mifetch <addr>\n";
      return false;
    }

  uint64_t addr = 0;
  if (not parseCmdLineNumber("address", tokens.at(1), addr))
    return false;

  return system_.mcmIFetch(hart, this->time_, addr);
}


template <typename URV>
bool
Interactive<URV>::mievictCommand(Hart<URV>& hart, const std::string& line,
				 const std::vector<std::string>& tokens)
{
  // Format: mievict <physical-address>
  if (tokens.size() != 2)
    {
      cerr << "Error: Invalid mievict command: " << line << '\n';
      cerr << "Error:   Expecting: mievict <addr>\n";
      return false;
    }

  uint64_t addr = 0;
  if (not parseCmdLineNumber("address", tokens.at(1), addr))
    return false;

  return system_.mcmIEvict(hart, this->time_, addr);
}


template <typename URV>
bool
Interactive<URV>::mdfetchCommand(Hart<URV>& hart, const std::string& line,
				 const std::vector<std::string>& tokens)
{
  // Format: mdfetch <physical-address>
  if (tokens.size() != 2)
    {
      cerr << "Invalid mdfetch command: " << line << '\n';
      cerr << "  Expecting: mdfetch <addr>\n";
      return false;
    }

  uint64_t addr = 0;
  if (not parseCmdLineNumber("address", tokens.at(1), addr))
    return false;

  return system_.mcmDFetch(hart, this->time_, addr);
}


template <typename URV>
bool
Interactive<URV>::mdevictCommand(Hart<URV>& hart, const std::string& line,
				 const std::vector<std::string>& tokens)
{
  // Format: mdevict <physical-address>
  if (tokens.size() != 2)
    {
      cerr << "Invalid mdevict command: " << line << '\n';
      cerr << "  Expecting: mdevict <addr>\n";
      return false;
    }

  uint64_t addr = 0;
  if (not parseCmdLineNumber("address", tokens.at(1), addr))
    return false;

  return system_.mcmDEvict(hart, this->time_, addr);
}


template <typename URV>
bool
Interactive<URV>::mdwritebackCommand(Hart<URV>& hart, const std::string& line,
				      const std::vector<std::string>& tokens)
{
  // Format: mdevict <physical-address> < data>
  if (tokens.size() != 3 and
      tokens.size() != 2)
    {
      cerr << "Invalid mdwriteback command: " << line << '\n';
      cerr << "  Expecting: mdwriteback <addr> [<data>]\n";
      return false;
    }

  uint64_t addr = 0;
  if (not parseCmdLineNumber("address", tokens.at(1), addr))
    return false;


  std::vector<uint8_t> data;
  if (tokens.size() > 2)
    {
      std::string hexDigits = tokens.at(2);

      if (not (hexDigits.starts_with("0x") or
               hexDigits.starts_with("0X")))
        {
          cerr << "Error: mdwriteback data must begin with 0x: " << hexDigits << '\n';
          return false;
        }
      hexDigits = hexDigits.substr(2);

      if ((hexDigits.size() & 1) == 1)
        {
          cerr << "Error: mdwriteback hex digit count must be even\n";
          return false;
        }

      for (size_t i = 0; i < hexDigits.size(); i += 2)
        {
          std::string byteStr = hexDigits.substr(i, 2);
          char* end = nullptr;
          unsigned value = strtoul(byteStr.c_str(), &end, 16);
          if (end and *end)
            {
              cerr << "Error: Invalid hex digit(s) in mdwriteback data: " << byteStr << '\n';
              return false;
            }
          data.push_back(value);
        }

      std::reverse(data.begin(), data.end()); // Least sig byte now first
    }

  return system_.mcmDWriteback(hart, this->time_, addr, data);
}


template <typename URV>
bool
Interactive<URV>::mskipReadChkCommand([[maybe_unused]] Hart<URV>& hart, const std::string& line,
                                      const std::vector<std::string>& tokens)
{
  // Format: mskipreadchk <address> <size> <enable>
  if (tokens.size() != 4)
    {
      cerr << "Error: invalid mskipreadchk command: " << line << '\n';
      cerr << "Error:   Expecting: mskipreadchk <addr> <size> <enable>\n";
      return false;
    }

  uint64_t addr = 0;
  if (not parseCmdLineNumber("address", tokens.at(1), addr))
    return false;
  unsigned size = 0;
  if (not parseCmdLineNumber("size", tokens.at(2), size))
    return false;
  bool enable = false;
  if (not parseCmdLineBool("enable", tokens.at(3), enable))
    return false;
  return system_.mcmSkipReadDataCheck(addr, size, enable);
}


template <typename URV>
bool
Interactive<URV>::translateCommand(Hart<URV>& hart, const std::string& line,
				   const std::vector<std::string>& tokens)
{
  // translate va [r|w|x [s|u]]. Mode defaults to r, privilege to u.
  if (tokens.size() < 3)
    {
      cerr << "Error: Invalid translate command: " << line << '\n';
      cerr << "Error: Expecting: translate <vaddr> [r|w|x [s|u|vs|vu]]\n";
      return false;
    }

  uint64_t va = 0;
  if (not parseCmdLineNumber("virtual-address", tokens.at(1), va))
    return false;

  bool read = false, write = false, exec = false;
  if (tokens.size() > 2)
    {
      if      (tokens.at(2) == "r") read  = true;
      else if (tokens.at(2) == "w") write = true;
      else if (tokens.at(2) == "x") exec  = true;
      else
	{
	  cerr << "Error: Invalid protection mode: " << tokens.at(2) << " -- expecting r, w, or x\n";
	  return false;
	}
    }
  else
    read = true;

  bool twoStage = false;

  PrivilegeMode pm = PrivilegeMode::User;
  if (tokens.size() > 3)
    {
      if      (tokens.at(3) == "u")  { pm = PrivilegeMode::User; }
      else if (tokens.at(3) == "s")  { pm = PrivilegeMode::Supervisor; }
      else if (tokens.at(3) == "vu") { pm = PrivilegeMode::User; twoStage = true; }
      else if (tokens.at(3) == "vs") { pm = PrivilegeMode::Supervisor; twoStage = true; }
      else
	{
	  cerr << "Error: Invalid privilege mode: " << tokens.at(3) << " -- expecting u, s, vu, or vs\n";
	  return false;
	}
    }

  uint64_t pa = 0;
  auto ec = hart.transAddrNoUpdate(va, pm, twoStage, read, write, exec, pa);
  if (ec == ExceptionCause::NONE)
    {
      out_ << "0x" << std::hex << pa << std::dec << '\n';
      return true;
    }

  cerr << "Error: Translation failed -- exception code: " << unsigned(ec) << '\n';
  return false;
}


template <typename URV>
bool
Interactive<URV>::checkInterruptCommand(Hart<URV>& hart, [[maybe_unused]] const std::string& line,
					const std::vector<std::string>& tokens)
{
  // check_interrupt
  if (tokens.size() > 1)
    return false;

  // We want to check for interrupts regardless of deferral.
  URV deferred = hart.deferredInterrupts();
  hart.setDeferredInterrupts(0);

  InterruptCause cause;
  PrivilegeMode nextMode = PrivilegeMode::Machine;
  bool nextVirt = false;
  if (hart.isInterruptPossible(cause, nextMode, nextVirt))
    out_ << unsigned(cause) << '\n';

  hart.setDeferredInterrupts(deferred);

  return true;
}


template <typename URV>
bool
Interactive<URV>::seiPinCommand(Hart<URV>& hart, const std::string& line,
				const std::vector<std::string>& tokens)
{
  // sei_pin [0|1]
  unsigned val = 0;
  if (tokens.size() == 2)
    {
      if (not parseCmdLineNumber("pin-value", tokens.at(1), val))
	return false;
      if (val != 0 and val != 1)
	{
	  cerr << "Error: Invalid pin-value: " << tokens.at(1) << '\n';
	  return false;
	}
      hart.setSeiPin(val);
    }
  else
    {
      cerr << "Error: Invalid sei_pin command: " << line << '\n';
      cerr << "Error: Expecting: sei_pin 0|1\n";
      return false;
    }

  return true;
}


template <typename URV>
bool
Interactive<URV>::pmpCommand(Hart<URV>& hart, const std::string& line,
			     const std::vector<std::string>& tokens)
{
  // pmp [<address>]
  if (tokens.size() == 1)
    hart.printPmps(out_);
  else if (tokens.size() == 2)
    {
      uint64_t address;
      if (not parseCmdLineNumber("pmp-address", tokens.at(1), address))
        return false;

      hart.printPmps(out_, address);
    }
  else
    {
      cerr << "Error: Invalid pmp command: " << line << '\n';
      cerr << "Error: Expecting: pmp [<address>]\n";
      return false;
    }

  return true;
}


template <typename URV>
bool
Interactive<URV>::pmaCommand(Hart<URV>& hart, const std::string& line,
			     const std::vector<std::string>& tokens)
{
  // pmam [<address>]
  if (tokens.size() == 1)
    hart.printPmas(out_);
  else if (tokens.size() == 2)
    {
      uint64_t address;
      if (not parseCmdLineNumber("pma-address", tokens.at(1), address))
        return false;

      hart.printPmas(out_, address);
    }
  else
    {
      cerr << "Error: Invalid pma command: " << line << '\n';
      cerr << "Error: Expecting: pma [<address>]\n";
      return false;
    }

  return true;
}


template <typename URV>
bool
Interactive<URV>::injectExceptionCommand(Hart<URV>& hart, const std::string& line,
                                         const std::vector<std::string>& tokens)
{
  if (tokens.size() >= 4)
    {
      uint64_t flags = 0, cause = 0, elemIx = 0, addr = 0;
      if (not parseCmdLineNumber("inject-exception-flags", tokens.at(1), flags))
        return false;
      if (not parseCmdLineNumber("inject-exception-cause", tokens.at(2), cause))
        return false;
      if (not parseCmdLineNumber("inject-exception-elem-ix", tokens.at(3), elemIx))
        return false;
      if (tokens.size() == 5)
        if (not parseCmdLineNumber("inject-exception-addr", tokens.at(4), addr))
          return false;
      hart.injectException(flags, cause, elemIx, addr);
    }
  else
    {
      cerr << "Error: Invalid inject_exception command: " << line << '\n';
      return false;
    }

  return true;
}


template <typename URV>
bool
Interactive<URV>::perfModelFetchCommand(const std::string& line,
		                        const std::vector<std::string>& tokens)
{
  if (tokens.size() == 3)
    {
      uint64_t tag, vpc;
      if (not parseCmdLineNumber("perf-model-fetch-tag", tokens.at(1), tag))
        return false;
      if (not parseCmdLineNumber("perf-model-fetch-vpc", tokens.at(2), vpc))
        return false;

      return system_.perfApiFetch(hartId_, time_, tag, vpc);
    }
  else
    {
      cerr << "Error: Invalid perf_model_fetch command: " << line << '\n';
      cerr << "Error: Expecting: perf_model_fetch <tag> <vpc>\n";
      return false;
    }
}


template <typename URV>
bool
Interactive<URV>::perfModelDecodeCommand(const std::string& line,
		                         const std::vector<std::string>& tokens)
{
  if (tokens.size() == 2)
    {
      uint64_t tag;
      if (not parseCmdLineNumber("perf-model-decode-tag", tokens.at(1), tag))
        return false;
      return system_.perfApiDecode(hartId_, time_, tag);
    }
  else
    {
      cerr << "Error: Invalid perf_model_decode command: " << line << '\n';
      cerr << "Error: Expecting: perf_model_decode <tag>\n";
      return false;
    }
}


template <typename URV>
bool
Interactive<URV>::perfModelExecuteCommand(const std::string& line,
		                          const std::vector<std::string>& tokens)
{
  if (tokens.size() == 2)
    {
      uint64_t tag;
      if (not parseCmdLineNumber("perf-model-execute-tag", tokens.at(1), tag))
        return false;

      return system_.perfApiExecute(hartId_, time_, tag);
    }
  else
    {
      cerr << "Error: Invalid perf_model_execute command: " << line << '\n';
      cerr << "Error: Expecting: perf_model_execute <tag>\n";
      return false;
    }
}


template <typename URV>
bool
Interactive<URV>::perfModelRetireCommand(const std::string& line,
		                         const std::vector<std::string>& tokens)
{
  if (tokens.size() == 2)
    {
      uint64_t tag;
      if (not parseCmdLineNumber("perf-model-retire-tag", tokens.at(1), tag))
        return false;

      return system_.perfApiRetire(hartId_, time_, tag);
    }
  else
    {
      cerr << "Error: Invalid perf_model_retire command: " << line << '\n';
      cerr << "Error: Expecting: perf_model_retire <tag>\n";
      return false;
    }
}


template <typename URV>
bool
Interactive<URV>::perfModelDrainStoreCommand(const std::string& line,
		                             const std::vector<std::string>& tokens)
{
  if (tokens.size() == 2)
    {
      uint64_t tag;
      if (not parseCmdLineNumber("perf-model-drain-store-tag", tokens.at(1), tag))
        return false;

      return system_.perfApiDrainStore(hartId_, time_, tag);
    }
  else
    {
      cerr << "Error: Invalid perf_model_drain_store command: " << line << '\n';
      cerr << "Error: Expecting: perf_model_drain_store <tag>\n";
      return false;
    }
}


template <typename URV>
bool
Interactive<URV>::perfModelPredictBranch(const std::string& line,
					 const std::vector<std::string>& tokens)
{
  if (tokens.size() == 4)
    {
      uint64_t tag = 0;
      if (not parseCmdLineNumber("perf-model-predict-branch-tag", tokens.at(1), tag))
        return false;

      bool flag = false;
      if (not parseCmdLineBool("perf-model-predict-branch-taken", tokens.at(2), flag))
	return false;

      uint64_t addr = 0;
      if (not parseCmdLineNumber("perf-model-branch-prediction-target", tokens.at(3), addr))
	return false;

      return system_.perfApiPredictBranch(hartId_, time_, tag, flag, addr);
    }
  else
    {
      cerr << "Error: Invalid perf_model_predict_branch command: " << line << '\n';
      cerr << "Error: Expecting: perf_model_predict_branch <tag> <flag> <addr>\n";
      return false;
    }
}


template <typename URV>
bool
Interactive<URV>::perfModelFlushCommand(const std::string& line,
		                        const std::vector<std::string>& tokens)
{
  if (tokens.size() == 2)
    {
      uint64_t tag;
      if (not parseCmdLineNumber("perf-model-flush-tag", tokens.at(1), tag))
        return false;

      return system_.perfApiFlush(hartId_, time_, tag);
    }
  else
    {
      cerr << "Error: Invalid perf_model_flush command: " << line << '\n';
      cerr << "Error: Expecting: perf_model_flush <tag>\n";
      return false;
    }
}


template <typename URV>
bool
Interactive<URV>::perfModelShouldFlushCommand(const std::string& line,
					      const std::vector<std::string>& tokens)
{
  if (tokens.size() == 2)
    {
      uint64_t tag;
      if (not parseCmdLineNumber("perf-model-should-flush-tag", tokens.at(1), tag))
        return false;

      bool flag = false;
      uint64_t addr = 0;
      bool ok = system_.perfApiShouldFlush(hartId_, time_, tag, flag, addr);
      out_ << flag;
      if (flag)
	out_ << " 0x" << std::hex << addr << std::dec;
      out_ << '\n';
      return ok;
    }
  else
    {
      cerr << "Error: Invalid perf_model_flush command: " << line << '\n';
      cerr << "Error: Expecting: perf_model_flush <tag>\n";
      return false;
    }
}



template <typename URV>
bool
Interactive<URV>::interact(FILE* traceFile, FILE* commandLog)
{
  linenoise::SetHistoryMaxLen(1024);

  uint64_t errors = 0;
  hartId_ = 0;
  std::string replayFile;
  std::ifstream replayStream;

  const char* prompt = isatty(0) ? "whisper> " : "";

  auto hartPtr = system_.ithHart(0);
  if (hartPtr)
    {
      URV value = 0;
      if (hartPtr->peekCsr(CsrNumber::MHARTID, value))
        hartId_ = value;
    }

  bool tty = isatty(STDIN_FILENO);

  std::string line;

  bool done = false;
  while (not done)
    {
      errno = 0;
      line.clear();
      linenoise::Readline(prompt, line);

      if (line.empty())
	{
	  if (std::cin.eof())
	    return true;
	  continue;
	}

      if (tty)
	linenoise::AddHistory(line.c_str());

      if (not executeLine(line, traceFile, commandLog, replayStream, done))
	errors++;
    }

  return errors == 0;
}


template class WdRiscv::Interactive<uint32_t>;
template class WdRiscv::Interactive<uint64_t>;
