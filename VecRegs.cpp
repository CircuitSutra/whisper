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
#include <charconv>
#include <cmath>
#include <cstring>
#include "VecRegs.hpp"


using namespace WdRiscv;


VecRegs::VecRegs()
{
  // Intialize structure (vector of vectors) defining legal
  // element-width/group-multiplier combinations to all false: No
  // combination is supported. Configuration code will later change
  // this.
  legalConfigs_.resize(size_t(VecEnums::WidthLimit));
  for (auto& groupFlags : legalConfigs_)
    groupFlags.resize(size_t(VecEnums::GroupLimit));

  // Start by making all combinations of width/grouping legal. This
  // gets adjusted when config method is called.
  for (auto& groupFlags : legalConfigs_)
    groupFlags.assign(groupFlags.size(), true);

  // Operands effective group multiplier is used for tracing/logging.
  // At most we have 3 vector operands.
  opsEmul_.resize(3);
  opsEmul_.assign(opsEmul_.size(), 1);

  // Per-SEW controlled unordered fpsum reduction.
  fpUnorderedSumTreeRed_.resize(unsigned(VecEnums::WidthLimit), false);
  fpUnorderedSumCanonical_.resize(unsigned(VecEnums::WidthLimit), false);
}


VecRegs::~VecRegs()
{
  regCount_ = 0;
  bytesPerReg_ = 0;
  bytesInRegFile_ = 0;
}


void
VecRegs::config(unsigned bytesPerReg, unsigned minBytesPerElem,
		unsigned maxBytesPerElem,
		std::unordered_map<GroupMultiplier, unsigned>* minSewPerLmul,
		std::unordered_map<GroupMultiplier, unsigned>* maxSewPerLmul)
{
  if (bytesPerReg > 4096)
    {
      std::cerr << "Error: VecRegs::configure: bytes-per-register too large (" << bytesPerReg
                << ") -- using 4096\n";
      bytesPerReg = 4096;
    }

  if (bytesPerReg <= 4)
    {
      std::cerr << "Error: VecRegs::configure: bytes-per-register too small (" << bytesPerReg
                << ") -- using 4\n";
      bytesPerReg = 4;
    }

  unsigned l2BytesPerReg = std::bit_width(bytesPerReg) - 1;
  unsigned p2BytesPerReg = uint32_t(1) << l2BytesPerReg;
  if (p2BytesPerReg != bytesPerReg)
    {
      std::cerr << "Error: VecRegs::configure: bytes-per-register (" << bytesPerReg
                << ") not a power of 2 -- using " << p2BytesPerReg << "\n";
      bytesPerReg = p2BytesPerReg;
    }

  if (minBytesPerElem < 1)
    {
      std:: cerr << "Error: VecRegd::configure: zero min-bytes-per-element -- using 1\n";
      minBytesPerElem = 1;
    }

  if (maxBytesPerElem < 1)
    {
      std:: cerr << "Error: VecRegd::configure: zero max-bytes-per-element -- using 1\n";
      maxBytesPerElem = 1;
    }

  if (minBytesPerElem > maxBytesPerElem)
    {
      std:: cerr << "Error: VecRegd::configure: min-bytes-per-elem larger than max -- using max\n";
      minBytesPerElem = maxBytesPerElem;
    }

  unsigned l2BytesPerElem = std::bit_width(maxBytesPerElem) - 1;
  unsigned p2BytesPerElem = uint32_t(1) << l2BytesPerElem;
  if (p2BytesPerElem != maxBytesPerElem)
    {
      std::cerr << "Error: VecRegs::configure: max-bytes-per-element (" << maxBytesPerElem
                << ") not a power of 2 -- using " << p2BytesPerElem << "\n";
      maxBytesPerElem = p2BytesPerElem;
    }

  if (maxBytesPerElem > bytesPerReg)
    {
      std::cerr << "Error: VecRegs::configure: max-bytes-per-element (" << maxBytesPerElem
                << ") is greater than the bytes-per-register (" << bytesPerReg
                << " -- using " << bytesPerReg << "\n";
      maxBytesPerElem = bytesPerReg;
    }

  l2BytesPerElem = std::bit_width(minBytesPerElem) - 1;
  p2BytesPerElem = uint32_t(1) << l2BytesPerElem;
  if (p2BytesPerElem != minBytesPerElem)
    {
      std::cerr << "Error: VecRegs::configure: min-bytes-per-element (" << minBytesPerElem
                << ") not a power of 2 -- using " << p2BytesPerElem << "\n";
      minBytesPerElem = p2BytesPerElem;
    }

  if (minBytesPerElem > bytesPerReg)
    {
      std::cerr << "Error: VecRegs::configure: min-bytes-per-element (" << minBytesPerElem
                << ") is greater than the bytes-per-register (" << bytesPerReg
                << " -- using " << bytesPerReg << "\n";
      minBytesPerElem = bytesPerReg;
    }

  regCount_ = 32;
  bytesPerReg_ = bytesPerReg;
  minBytesPerElem_ = minBytesPerElem;
  maxBytesPerElem_ = maxBytesPerElem;
  bytesInRegFile_ = regCount_ * bytesPerReg_;
  uint32_t minLmul = minBytesPerElem_*8/maxBytesPerElem_;

  // Make illegal all group entries for element-widths greater than
  // the max-element-width (which is in bytesPerElem_) or smaller
  // than the min-element-width.
  for (unsigned i = 0; i <= unsigned(ElementWidth::Word32); ++i)
    {
      ElementWidth ew = ElementWidth(i);
      unsigned bytes = VecRegs::elemWidthInBytes(ew);
      auto& groupFlags = legalConfigs_.at(size_t(ew));
      if (bytes > maxBytesPerElem_ or bytes < minBytesPerElem_ )
	{
	  groupFlags.assign(groupFlags.size(), false);
          continue;
	}

      // Make current elem width illegal for LMUL < SEWmin/ELEN.
      for (unsigned lmulx8 = 1; lmulx8 < minLmul; lmulx8 *= 2)
	{
	  GroupMultiplier group = GroupMultiplier::One;
	  if (not groupNumberX8ToSymbol(lmulx8, group))
	    assert(0 && "Error: Assertion failed");
	  groupFlags.at(size_t(group)) = false;
	}

      // Allow user to set a greater minimum sew for an LMUL setting than minBytesPerElem
      if (minSewPerLmul)
	for (auto const& [group, min] : *minSewPerLmul)
	  {
	    assert(min >= minBytesPerElem_ and min <= maxBytesPerElem_);
	    if (min > bytes)
	      groupFlags.at(size_t(group)) = false;
	  }

      // Allow user to set a smaller maximu sew for an LMUL setting tan maxBytesPerElem
      if (maxSewPerLmul)
	for (auto const& [group, max] : *maxSewPerLmul)
	  {
	    assert(max >= minBytesPerElem_ and max <= maxBytesPerElem_);
	    if (max < bytes)
	      groupFlags.at(size_t(group)) = false;
	  }
    }


  data_.resize(bytesInRegFile_);
  std::fill(data_.begin(), data_.end(), 0);

  lastWrittenRegData_.reserve(regCount_ * bytesPerReg_);
}


void
VecRegs::reset()
{
  std::fill(data_.begin(), data_.end(), 0);
  lastWrittenReg_ = -1;
  lastWrittenRegData_.clear();
  lastGroupX8_ = 8;
}


bool
VecRegs::findReg(std::string_view name, unsigned& ix)
{
  if (name.empty())
    return false;

  std::string_view numStr = name;
  if (name.at(0) == 'v')
     numStr = numStr.substr(1);

  int base = 10;
  if (numStr.starts_with("0x"))
    {
      numStr = numStr.substr(2);
      base = 16;
    }

  unsigned n;
  if (auto result = std::from_chars(numStr.cbegin(), numStr.cend(), n, base);
      result.ec != std::errc{} or result.ptr != numStr.cend())
    return false;

  ix = n;
  return true;
}


bool
VecRegs::validateForEgs(unsigned egs, unsigned& vl, unsigned& vstart) const
{
  if (legalizeForEgs_)
    {
      if ((vl % egs) != 0)
        vl = vl - (vl % egs);

      if ((vstart % egs) != 0)
        vstart = vstart - (vstart % egs);
      return true;
    }

  return (vl % egs) == 0  and  (vstart % egs) == 0;
}


uint64_t
VecRegs::readIndexReg(uint32_t vecReg, uint32_t elemIx, ElementWidth eew, uint32_t groupX8) const
{
  switch(eew)
    {
    case ElementWidth::Byte:
      {
        uint8_t temp = 0;
        read(vecReg, elemIx, groupX8, temp);
        return temp;
      }
    case ElementWidth::Half:
      {
        uint16_t temp = 0;
        read(vecReg, elemIx, groupX8, temp);
        return temp;
      }
    case ElementWidth::Word:
      {
        uint32_t temp = 0;
        read(vecReg, elemIx, groupX8, temp);
        return temp;
      }
    case ElementWidth::Word2:
      {
        uint64_t temp = 0;
        read(vecReg, elemIx, groupX8, temp);
        return  temp;
      }
    default:
      assert(0 && "Error: Assertion failed");
    }

  return 0;
}
