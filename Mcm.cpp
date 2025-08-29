#include <iostream>
#include <string_view>
#include "Mcm.hpp"
#include "System.hpp"

using namespace WdRiscv;
using std::cerr;


template <typename URV>
Mcm<URV>::Mcm(unsigned hartCount, unsigned pageSize, unsigned mergeBufferSize)
  : pageSize_(pageSize), lineSize_(mergeBufferSize)
{
  sysMemOps_.reserve(200000);

  hartData_.resize(hartCount);

  for (auto& hd : hartData_)
    {
      hd.instrVec_.resize(200000);
      hd.regTime_.resize(totalRegCount_);
      hd.regProducer_.resize(totalRegCount_);
    }

  // If no merge buffer, then memory is updated on insert messages.
  writeOnInsert_ = (lineSize_ == 0);

  // Enable all rules.
  ppoEnabled_.resize(PpoRule::Limit, true);

  // Currently Io PPO is disabled by default.
  ppoEnabled_.at(unsigned(PpoRule::Io)) = false;
}


template <typename URV>
Mcm<URV>::~Mcm() = default;


template <typename URV>
inline
bool
Mcm<URV>::updateTime(const char* method, uint64_t time)
{
  if (time < time_)
    {
      cerr << "Warning: " << method << ": Backward time: "
	   << time << " < " << time_ << "\n";
      return true;
    }
  time_ = time;
  return true;
}



template <typename URV>
bool
Mcm<URV>::referenceModelRead(Hart<URV>& hart, uint64_t pa, unsigned size, uint64_t& data)
{
  data = 0;

  if (hart.isDeviceAddr(pa))
    {
      hart.deviceRead(pa, size, data);
      return true;
    }

  if (uint64_t toHost = 0; hart.getToHostAddress(toHost) && toHost == pa)
    return true;  // Reading from toHost yields 0.

  bool ok = true;

  if (size == 1)
    {
      uint8_t val = 0;
      ok = hart.peekMemory(pa, val, true /*usePma*/);
      data = val;
    }
  else if (size == 2)
    {
      uint16_t val = 0;
      ok = hart.peekMemory(pa, val, true /*usePma*/);
      data = val;
    }
  else if (size == 4)
    {
      uint32_t val = 0;
      ok = hart.peekMemory(pa, val, true /*usePma*/);
      data = val;
    }
  else if (size == 8)
    {
      uint64_t val = 0;
      ok = hart.peekMemory(pa, val, true /*usePma*/);
      data = val;
    }
  else if (size < 8)
    {
      uint8_t val = 0;
      for (unsigned i = 0; i < size; ++i)
	if (not hart.peekMemory(pa + i, val, true))
	  {
	    ok = false;
	    break;
	  }
	else
	  data = data | (uint64_t(val) << (8*i));
    }
  else
    assert(0 && "Error: invalid mcm-read size");

  return ok;
}


template <typename URV>
bool
Mcm<URV>::readOp(Hart<URV>& hart, uint64_t time, uint64_t tag, uint64_t pa, unsigned size,
		 uint64_t rtlData, unsigned elemIx, unsigned field)
{
  if (not updateTime("Mcm::readOp", time))
    return false;

  if (size == 0 or size > 8)
    {
      cerr << "Error: Mcm::readOp: hart-id=" << hart.hartId() << " time=" << time
	   << " tag=" << tag << " invalid read size: " << size << '\n';
      return false;
    }

  // Split in two ops Crossing a line boundary to avoid being too pessimistic
  // when checking for overlap with stores from other harts
  if (lineNum(pa) != lineNum(pa + size - 1))
    {
      unsigned size1 = offsetToNextLine(pa);
      unsigned size2 = size - size1;
      uint64_t pa2 = pa + size1;
      uint64_t rtlData1 = rtlData & ((uint64_t(1) << (size1*8)) - 1);
      uint64_t rtlData2 = rtlData >> (size1*8);
      bool ok = readOp_(hart, time, tag, pa, size1, rtlData1, elemIx, field);
      ok = readOp_(hart, time, tag, pa2, size2, rtlData2, elemIx, field) and ok;
      return ok;
    }

  return readOp_(hart, time, tag, pa, size, rtlData, elemIx, field);
}


template <typename URV>
bool
Mcm<URV>::readOp_(Hart<URV>& hart, uint64_t time, uint64_t tag, uint64_t pa, unsigned size,
                  uint64_t rtlData, unsigned elemIx, unsigned field)
{
  unsigned hartIx = hart.sysHartIndex();

  McmInstr* instr = findOrAddInstr(hartIx, tag);
  if (instr->isCanceled())
    return true;

  if (instr->isRetired() and not hart.getPma(pa).isIo())
    cerr << "Warning: hart-id=" << hart.hartId() << " time=" << time <<
         " tag=" << tag << " read-op seen after instruction retires\n";

  pa = hart.clearSecureAddressSteeBits(pa);

  MemoryOp op = {};
  op.time_ = time;
  op.pa_ = pa;
  op.rtlData_ = rtlData;
  op.tag_ = tag;
  op.hartIx_ = hartIx;
  op.size_ = size;
  op.isRead_ = true;
  op.canceled_ = true; // To be later marked as false if used.
  op.elemIx_ = elemIx;
  op.field_ = field;
  op.isIo_ = hart.getPma(op.pa_).isIo();

  assert(pageNum(pa) == pageNum(pa + size - 1));

  instr->isLoad_ = true;

  // Read Whisper memory and keep it in op, this will be updated when the load is retired
  // by forwarding from preceding stores.
  uint64_t refVal = 0;
  if (referenceModelRead(hart, pa, size, refVal))
    op.data_ = refVal;
  else
    op.failRead_ = true;

  if (sysMemOps_.empty() or sysMemOps_.back().time_ <= time)
    {
      instr->addMemOp(sysMemOps_.size());
      sysMemOps_.push_back(op);
    }
  else
    {
      // Sometimes the test-bench will unexpectedly send us a read-op with an out of order
      // time-stamp. We hack a repair.

      // Find first existing op with time > current time.
      auto iter = std::upper_bound(sysMemOps_.begin(), sysMemOps_.end(), time,
                                   [](const uint64_t& t, const MemoryOp& mop) -> bool
                                   { return t < mop.time_; });

      // Insert new op before the found op.
      assert(iter != sysMemOps_.end());
      size_t ix = iter - sysMemOps_.begin();
      sysMemOps_.insert(iter, op);

      // Adjust indices of all the instructions referencing ops that are now after new op
      // in sysMemOps_.

      // One set of adjusted tags per hart.
      std::vector< std::unordered_set<uint64_t> > adjustedTags(hartData_.size()); 

      bool movedWrite = false;
      for (size_t i = ix + 1; i < sysMemOps_.size(); ++i)
        {
          auto& movedOp = sysMemOps_.at(i);
          auto& instrVec = hartData_.at(movedOp.hartIx_).instrVec_;

          auto& tagSet = adjustedTags.at(movedOp.hartIx_);
          auto tag = movedOp.tag_;
          if (tagSet.contains(tag))
            continue;
          tagSet.insert(tag);

          auto& instr = instrVec.at(tag);
          for (auto& instrOpIx : instr.memOps_)
            if (instrOpIx >= ix)
              {
                instrOpIx++;
                movedWrite = movedWrite or instr.isStore_;
              }
        }

      // Associate new op with instruction. This has to be done after the indices are
      // adjusted otherwise we may get an "op already added" error.
      instr->addMemOp(ix);

      std::cerr << "Warning: hart-id=" << hartIx << " tag=" << op.tag_
                << " detected non-monotonic time read at time=" << time;

      // If there is a write before the read op to the same address, we are
      // forced to use the RTL data. We have no recovery mechanism. Right now,
      // the heuristic is if there is any store.
      if (movedWrite)
        {
          op.data_ = op.rtlData_;
          std::cerr << " using RTL data\n";
        }
      else
        std::cerr << '\n';
    }

  if (instr->retired_)
    instr->complete_ = checkLoadComplete(*instr);

  return true;
}


template <typename URV>
McmInstr*
Mcm<URV>::findInstr(unsigned hartIx, uint32_t tag)
{
  auto& vec = hartData_.at(hartIx).instrVec_;
  if (tag < vec.size() and vec.at(tag).tag_ == tag)
    return &(vec.at(tag));
  return nullptr;
}


template <typename URV>
McmInstr*
Mcm<URV>::findOrAddInstr(unsigned hartIx, uint32_t tag)
{
  auto ptr = findInstr(hartIx, tag);
  if (ptr)
    return ptr;

  auto& vec = hartData_.at(hartIx).instrVec_;
  if (tag >= vec.size())
    {
      if (tag > 500000000)
	{
	  cerr << "Error: MCM: Instruction tag way too large: " << tag << '\n';
	  cerr << "Error: MCM: Code expects dense consecutive tags starting at 0\n";
	  assert(0 && "Error: Assertion failed");
	}
      McmInstr instr;
      instr.tag_ = tag;
      instr.hartIx_ = hartIx;
      vec.resize(tag + 1);
      vec.at(tag) = instr;
      return &vec.at(tag);
    }

  if (vec.at(tag).tag_ != 0)
    {
      cerr << "Mcm::findOrAddInstr: Error: Instr tag already in use\n";
      assert(0 && "Error: Mcm::findOrAddInstr: Instr tag already in use");
    }

  vec.at(tag).tag_ = tag;
  vec.at(tag).hartIx_ = hartIx;
  return &vec.at(tag);
}


template <typename URV>
void
Mcm<URV>::setBranchMemTime(const Hart<URV>& hart, const McmInstr& instr)
{
  const DecodedInst& di = instr.di_;
  if (not di.isBranch())
    return;

  unsigned hartIx = hart.sysHartIndex();

  auto& regTimeVec = hartData_.at(hartIx).regTime_;
  auto& branchTime = hartData_.at(hartIx).branchTime_;

  auto& branchProducer = hartData_.at(hartIx).branchProducer_;
  auto& regProducer = hartData_.at(hartIx).regProducer_;

  std::vector<unsigned> sourceRegs, destRegs;
  identifyRegisters(hart, di, sourceRegs, destRegs);

  branchTime = 0;
  branchProducer = 0;

  if (sourceRegs.empty())
    return;

  for (auto regIx : sourceRegs)
    {
      if (regTimeVec.at(regIx) > branchTime)
	{
	  branchTime = regTimeVec.at(regIx);
	  branchProducer = regProducer.at(regIx);
	}
    }
}


template <typename URV>
unsigned
Mcm<URV>::getLdStDataVectors(const Hart<URV>& hart, const McmInstr& instr,
			     std::array<std::pair<unsigned,VecKind>, 32>& vecs) const
{
  if (not instr.di_.isVectorLoad() and not instr.di_.isVectorStore())
    return 0;

  auto& info = hart.getLastVectorMemory();
  auto& elems = info.elems_;
  unsigned elemSize = info.elemSize_;
  assert(elemSize != 0);
  unsigned elemsPerVec = hart.vecRegSize() / elemSize;

  std::array<bool, 32> referenced;  for (auto& x : referenced) x = false;
  std::array<bool, 32> active;      for (auto& x : active)     x = false;
  std::array<bool, 32> preserve;    for (auto& x : preserve)   x = true;

  bool maskAgn = hart.isVectorMaskAgnostic();
  bool tailAgn = hart.isVectorTailAgnostic();

  for (auto& elem : elems)
    {
      unsigned regNum = info.vec_ + elem.field_*info.group_ + elem.ix_ / elemsPerVec;
      referenced.at(regNum) = true; // Vector covers one or more element of instr.

      if (elem.skip_)
	{
	  bool masked = elem.ix_ < info.elemCount_;
	  bool drop = masked ? maskAgn : tailAgn;
	  preserve.at(regNum) = preserve.at(regNum) and not drop;
	}
      else
	active.at(regNum) = true;   // Vector has one or more active element.
    }

  unsigned count = 0;
  for (unsigned regNum = 0; regNum < 32; ++regNum)
    if (referenced.at(regNum))
      {
	VecKind kind = VecKind::Skip;
	if (active.at(regNum))
	  kind = VecKind::Active;
	else if (preserve.at(regNum))
	  kind = VecKind::Preserve;
	vecs.at(count++) = std::pair<unsigned, VecKind>{regNum, kind};
      }

  return count;
}


template <typename URV>
unsigned
Mcm<URV>::getLdStIndexVectors(const Hart<URV>& hart, const McmInstr& instr,
			      std::array<std::pair<unsigned,VecKind>, 32>& vecs) const
{
  if (not instr.di_.isVectorLoad() and not instr.di_.isVectorStore())
    return 0;

  auto& info = hart.getLastVectorMemory();
  if (not info.isIndexed_)
    return 0;

  auto& elems = info.elems_;

  unsigned ixElemSize = hart.vecLdStIndexElemSize(instr.di_);
  assert(ixElemSize != 0);
  unsigned elemsPerVec = hart.vecRegSize() / ixElemSize;

  std::array<bool, 32> referenced;  for (auto& x : referenced) x = false;
  std::array<bool, 32> active;      for (auto& x : active)     x = false;

  for (auto& elem : elems)
    {
      unsigned regNum = info.ixVec_ + elem.ix_ / elemsPerVec;

      // It's possible tail extends past the number of registers. This is because
      // we do not perform a read for indices part of the tail.
      if (elem.ix_ >= info.elemCount_)
        continue;

      if (not elem.skip_)
        active.at(regNum) = true;

      referenced.at(regNum) = true;
    }

  unsigned count = 0;
  for (unsigned regNum = 0; regNum < 32; ++regNum)
    if (referenced.at(regNum))
      {
	VecKind kind = active.at(regNum)? VecKind::Active : VecKind::Skip;
	vecs.at(count++) = std::pair<unsigned, VecKind>{regNum, kind};
      }

  return count;
}


static bool isUnitStride(const VecLdStInfo& info)
{
  if (info.isIndexed_)
    return false;
  if (not info.isStrided_)
    return true;
  unsigned nf = info.fields_ == 0 ? 1 : info.fields_;
  return nf * info.elemSize_ == info.stride_;
}


template <typename URV>
void
Mcm<URV>::updateVecLoadDependencies(const Hart<URV>& hart, const McmInstr& instr)
{
  assert(instr.di_.isVectorLoad());

  auto& info = hart.getLastVectorMemory();
  auto& elems = info.elems_;
  unsigned elemSize = info.elemSize_;
  assert(elemSize != 0);
  unsigned elemsPerVec = hart.vecRegSize() / elemSize;

  bool unitStride = isUnitStride(info);
  bool stride0 = info.isStrided_ and info.stride_ == 0 and info.fields_ == 0;

  unsigned hartIx = hart.sysHartIndex();

  // Initialize producer/time to 0 unless whole vector is preserved.
  std::array<std::pair<unsigned, VecKind>, 32> dataRegs;
  unsigned count = getLdStDataVectors(hart, instr, dataRegs);
  for (unsigned i = 0; i < count; ++i)
    {
      auto [regNum, kind] = dataRegs.at(i);
      if (kind != VecKind::Preserve)
	{
	  setVecRegTime(hartIx, regNum, 0);
	  setVecRegProducer(hartIx, regNum, 0);
	}
    }

  if (elemSize == 0 or elems.empty())
    return;  // Should not happen.

  for (auto& elem : elems)
    {
      if (elem.skip_)
	continue;  // Non active element

      uint64_t pa1 = elem.pa_, pa2 = elem.pa2_;
      unsigned size = elemSize, size1 = elemSize;
      
      if (pa1 != pa2)
	{
	  size1 = offsetToNextPage(pa1);
	  assert(size1 > 0 and size1 < 8);
	}

      unsigned regNum = info.vec_ + elem.field_*info.group_ + elem.ix_ / elemsPerVec;

      auto regTime = vecRegTime(hartIx, regNum);

      for (unsigned i = 0; i < size; ++i)
	{
	  uint64_t addr = i < size1 ? pa1 + i : pa2 + i - size1;
	  uint64_t byteTime = 0;

          if (unitStride)
            byteTime = latestByteTime(instr, addr);  // Don't use elem ix.
          else
            {
              unsigned elemIx = elem.ix_;

              // For strided with stride=0, test-bench sometimes sends one read for all indices
              // and sometimes sends one read per index. Compensate.
              if (stride0)
                elemIx = effectiveStride0ElemIx(hart, instr, elemIx);

              byteTime = latestByteTime(instr, addr, elemIx);
            }

	  regTime = std::max(byteTime, regTime);
	}

      setVecRegTime(hartIx, regNum, regTime);

      McmInstrIx regProd = (regTime == 0)? 0 : instr.tag_;
      setVecRegProducer(hartIx, regNum, regProd);
    }
}


template <typename URV>
void
Mcm<URV>::updateDependencies(const Hart<URV>& hart, const McmInstr& instr)
{
  assert(instr.retired_);

  const DecodedInst& di = instr.di_;
  assert(di.isValid());
  if (di.operandCount() == 0)
    return;  // No registers.

  if (di.isVectorLoad())
    {
      updateVecLoadDependencies(hart, instr);
      return;
    }

  if ((di.isStore() and not di.isSc()) or di.isVectorStore())
    return; // No destination register.

  uint64_t time = 0, tag = 0, csrTime = 0, csrTag = 0;

  if (di.isSc())
    {
      if (URV val = 0; hart.peekIntReg(di.op0(), val) and val == 1)
	return;  // store-conditional failed.
      if (instr.memOps_.empty())
	time = ~uint64_t(0); // Will be updated when SC drains to memory.
    }

  if (not instr.memOps_.empty())
    {
      // At this point only load/amo/sc instructions should have memory ops.
      assert(di.isLoad() or di.isAmo() or di.isSc());
      for (const auto& opIx : instr.memOps_)
	if (opIx < sysMemOps_.size() and sysMemOps_.at(opIx).time_ > time)
	  time = sysMemOps_.at(opIx).time_;
    }

  unsigned hartIx = hart.sysHartIndex();
  auto& regTimeVec = hartData_.at(hartIx).regTime_;
  auto& regProducer = hartData_.at(hartIx).regProducer_;

  // Load/amo/sc/branch/mop do not carry dependencies to dest register: Update time of dest.
  if (di.isLoad() or di.isAmo() or di.isSc() or di.isBranch() or di.isMop())
    {
      auto regIx = effectiveRegIx(di, 0);
      if (di.ithOperandMode(0) == OperandMode::Write and regIx != 0)
	{
	  regProducer.at(regIx) = instr.tag_;
	  regTimeVec.at(regIx) = time;
	}
      return;
    }

  // Propagate times from source to destination registers.

  auto id = di.instId();
  bool isVset = (id == InstId::vsetvl or id == InstId::vsetvli or id == InstId::vsetivli);
  if (di.isVector() and not isVset)
    {
      updateVecRegTimes(hart, instr);
      return;
    }

  bool updatesVl = (id == InstId::vsetvl or id == InstId::vsetvli) and (di.op0() or di.op1());
  auto& vlTime = hartData_.at(hartIx).vlTime_;
  auto& vlProducer = hartData_.at(hartIx).vlProducer_;

  std::vector<unsigned> sourceRegs, destRegs;
  identifyRegisters(hart, di, sourceRegs, destRegs);

  for (auto regIx : sourceRegs)
    {
      bool isCsr = regIx >= csRegOffset_;
      if (isCsr)
	{
	  csrTime = regTimeVec.at(regIx);
	  csrTag = regProducer.at(regIx);
	  continue;
	}

      if (regTimeVec.at(regIx) > time)
	{
	  time = regTimeVec.at(regIx);
	  tag = regProducer.at(regIx);
	}

      if (updatesVl and regTimeVec.at(regIx) > vlTime)
	{
	  vlTime = regTimeVec.at(regIx);
	  vlProducer = regProducer.at(regIx);
	}
    }

  unsigned vlIx = csRegOffset_ + unsigned(CsrNumber::VL);

  for (auto regIx : destRegs)
    {
      if (regIx == 0)
	continue;  // Destination is X0

      if (not instr.di_.isCsr() or regIx >= csRegOffset_)
	{  // Non-CSR instruction or destination is a CSR register.
	  regTimeVec.at(regIx) = time;
	  regProducer.at(regIx) = tag;

	  if (regIx == vlIx and time > vlTime)
	    {
	      vlTime = time;
	      vlProducer = tag;
	    }
	}
      else
	{  // Integer destination register of a CSR instruction.
	  regTimeVec.at(regIx) = csrTime;
	  regProducer.at(regIx) = csrTag;
	}
    }
}


template <typename URV>
void
Mcm<URV>::updateVecRegTimes(const Hart<URV>& hart, const McmInstr& instr)
{
  auto& di = instr.di_;
  auto opCount = di.operandCount();
  if (opCount == 0)
    return;

  auto id = di.instId();
  bool isVset = (id == InstId::vsetvl or id == InstId::vsetvli or id == InstId::vsetivli);
  if (isVset)
    {
      assert(0 && "Error: Assertion failed");
      return;
    }

  auto hartIx = instr.hartIx_;
  auto& regProducer = hartData_.at(hartIx).regProducer_;
  auto& regTimeVec = hartData_.at(hartIx).regTime_;

  uint64_t time = 0, tag = 0;

  assert(di.ithOperandMode(0) == OperandMode::Write);  // 1st operand must be the destination

  if (di.ithOperandType(0) != OperandType::VecReg)
    {
      // Destination register is scalar: vcpop.m, vfirst.m, vmv.x.s, or vmv.f.s
      assert(id == InstId::vcpop_m or id == InstId::vfirst_m or id == InstId::vmv_x_s or
	     id == InstId::vfmv_f_s);
      assert(opCount == 2);
      assert(di.ithOperandType(1) == OperandType::VecReg);  // 2nd operand must be vec

      auto destIx = effectiveRegIx(di, 0);
      if (destIx == 0)
	return;  // Destination is X0

      // Get group multiplier of vector register.
      unsigned lmul = 1;  // vmv instructions ignore LMUL
      if (id == InstId::vcpop_m or id == InstId::vfirst_m)
	lmul = hart.vecOpEmul(1);

      unsigned baseRegIx = effectiveRegIx(di, 1);
      for (unsigned i = 0; i < lmul; ++i)
	{
	  unsigned srcIx = baseRegIx + i;
	  auto srcTime = regTimeVec.at(srcIx);
	  if (srcTime > time)
	    {
	      time = srcTime;
	      tag = regProducer.at(srcIx);
	    }
	}

      regProducer.at(destIx) = tag;
      regTimeVec.at(destIx) = time;
      return;
    }

  auto instId = di.instId();
  auto destEmul = hart.vecOpEmul(0);

  unsigned baseDestIx = effectiveRegIx(di, 0);

  for (unsigned ii = 0; ii < destEmul; ++ii)
    {
      time = 0;
      tag = 0;

      auto destIx = baseDestIx + ii;

      // Process all source operands.
      for (unsigned so = 1; so < opCount; ++so)
	{
	  if (di.ithOperandType(so) == OperandType::Imm)
	    continue;

	  assert(di.ithOperandMode(so) == OperandMode::Read);

	  if (di.ithOperandType(so) != OperandType::VecReg)
	    {
	      // Scalar source operand. Affects all vec regs in dest group.
	      auto srcIx = effectiveRegIx(di, so);
	      auto srcTime = regTimeVec.at(srcIx);
              if ((instId == InstId::vslide1up_vx or instId == InstId::vfslide1up_vf) and ii != 0)
                continue;
              if ((instId == InstId::vslide1down_vx or instId == InstId::vfslide1down_vf) and ii != destEmul - 1)
                continue;
	      if (srcTime > time)
		{
		  time = srcTime;
		  tag = regProducer.at(srcIx);
		}
	    }
	  else
	    {
	      // Vector source operand.
	      // We propagate times at the vector register level. In the future we will do
	      // this at the element or at the byte level.

	      auto baseSrcIx = effectiveRegIx(di, so);
	      auto srcEmul = hart.vecOpEmul(so);
	      if (srcEmul <= destEmul)
		{
		  assert((destEmul % srcEmul) == 0);

		  // Determine number of dest vec regs per source reg
		  unsigned ndest = destEmul / srcEmul;

		  auto jj = ii / ndest; // jj is the index of the source vec reg in its group.
		  auto srcIx = baseSrcIx + jj; // Source vec reg corresponding to dest.
		  auto srcTime = regTimeVec.at(srcIx);
		  if (srcTime > time)
		    {
		      time = srcTime;
		      tag = regProducer.at(srcIx);
		    }
		}
	      else
		{
		  assert((srcEmul % destEmul) == 0);

		  // Determine number of src vec regs per dest reg.
		  unsigned nsrc = srcEmul / destEmul;

		  unsigned jj = ii*nsrc;  // jj is the index of a source vec reg in its group
		  for (unsigned i = 0; i < nsrc; ++i)
		    {
		      auto srcIx = baseSrcIx + jj + i;
		      auto srcTime = regTimeVec.at(srcIx);
		      if (srcTime > time)
			{
			  time = srcTime;
			  tag = regProducer.at(srcIx);
			}
		    }
		}
	    }
	}

      regProducer.at(destIx) = tag;
      regTimeVec.at(destIx) = time;
    }
}


template <typename URV>
void
Mcm<URV>::setProducerTime(const Hart<URV>& hart, McmInstr& instr)
{
  auto& di = instr.di_;
  unsigned hartIx = hart.sysHartIndex();

  auto& regProducer = hartData_.at(hartIx).regProducer_;
  auto& regTime = hartData_.at(hartIx).regTime_;

  // Set producer of address register.
  if (di.isLoad() or di.isAmo() or di.isStore() or di.isVectorLoad() or di.isVectorStore())
    {
      unsigned addrReg = effectiveRegIx(di, 1);  // Addr reg is operand 1 of instr.
      instr.addrProducer_ = regProducer.at(addrReg);
      instr.addrTime_ = regTime.at(addrReg);
    }

  if (di.isVectorLoadStrided() or di.isVectorStoreStrided())
    {
      unsigned strideReg = effectiveRegIx(di, 2);
      uint64_t addrTime = regTime.at(strideReg);
      if (addrTime >= instr.addrTime_)
        {
          instr.addrProducer_ = regProducer.at(strideReg);
          instr.addrTime_ = addrTime;
        }
    }

  // Set producer of data register.
  if (di.isStore() or di.isAmo())
    {
      unsigned doi = di.isAmo()? 2 : 0;  // Data-register operand index
      unsigned dataReg = effectiveRegIx(di, doi);  // Data operand may be integer/fp/csr
      instr.dataProducer_ = regProducer.at(dataReg);
      instr.dataTime_ = regTime.at(dataReg);
    }

  if (di.isVectorStore())
    {
      std::array<std::pair<unsigned, VecKind>, 32> dataVecs;  // reg-num/masked pairs
      unsigned count = getLdStDataVectors(hart, instr, dataVecs);
      unsigned active = 0;
      for (unsigned i = 0; i < count; ++i)
        {
	  auto [dataReg, kind] = dataVecs.at(i);
	  if (kind != VecKind::Active)
	    continue;  // Preserve does not apply to store.

	  auto time = vecRegTime(hartIx, dataReg);
	  auto producer = vecRegProducer(hartIx, dataReg);
	  if (time >= instr.dataTime_)
	    {
	      instr.dataProducer_ = producer;
	      instr.dataTime_ = time;
	    }

	  instr.vecProdTimes_.at(active++) = McmInstr::VecProdTime{dataReg, producer, time};
        }
    }

  if (di.isVectorLoadIndexed() or di.isVectorStoreIndexed())
    {
      std::array<std::pair<unsigned, VecKind>, 32> ixRegs;  // reg-num/masked pairs
      unsigned count = getLdStIndexVectors(hart, instr, ixRegs);
      unsigned active = 0;
      for (unsigned i = 0; i < count; ++i)
	{
	  auto [ixReg, kind] = ixRegs.at(i);
	  if (kind != VecKind::Active) // If we preserve data reg, index reg is not used.
	    continue;

	  auto time = regTime.at(ixReg);
	  auto producer = regProducer.at(ixReg);

	  // We do not update addrProducer_ and addrTime_: those are for the scalar
	  // address register.
	  instr.ixProdTimes_.at(active++) = McmInstr::VecProdTime{ixReg, producer, time};
        }
    }
}


template <typename URV>
static bool
pokeHartMemory(Hart<URV>& hart, uint64_t physAddr, uint64_t data, unsigned size, bool cache)
{
  bool usePma = true;
  bool skipFetch = true;
  bool skipData = not cache;

  if (size == 1)
    return hart.pokeMemory(physAddr, uint8_t(data), usePma, skipFetch, skipData);

  if (size == 2)
    return hart.pokeMemory(physAddr, uint16_t(data), usePma, skipFetch, skipData);

  if (size == 4)
    return hart.pokeMemory(physAddr, uint32_t(data), usePma, skipFetch, skipData);

  if (size == 8)
    return hart.pokeMemory(physAddr, uint64_t(data), usePma, skipFetch, skipData);

  if (size < 8)
    {
      for (unsigned i = 0; i < size; ++i)
	if (not hart.pokeMemory(physAddr + i, uint8_t(data >> (8*i)), usePma, skipFetch, skipData))
	  return false;
      return true;
    }

  cerr << "Error: MCM pokeHartMemory: " << "Invalid data size (" << size << ")\n";
  return false;
}


template <typename URV>
bool
Mcm<URV>::mergeBufferInsert(Hart<URV>& hart, uint64_t time, uint64_t tag, uint64_t pa,
                            unsigned size, uint64_t rtlData, unsigned elemIx, unsigned field)
{
  if (not updateTime("Mcm::mergeBufferInsert", time))
    return false;

  assert(size <= 8);

  unsigned hartIx = hart.sysHartIndex();

  pa = hart.clearSecureAddressSteeBits(pa);

  MemoryOp op = {};
  op.time_ = time;
  op.insertTime_ = time;
  op.pa_ = pa;
  op.rtlData_ = rtlData;
  op.tag_ = tag;
  op.hartIx_ = hartIx;
  op.size_ = size;
  op.isRead_ = false;
  op.elemIx_ = elemIx;
  op.field_ = field;
  op.isIo_ = hart.getPma(op.pa_).isIo();

  if (not writeOnInsert_)
    hartData_.at(hartIx).pendingWrites_.push_back(op);

  McmInstr* instr = findOrAddInstr(hartIx, tag);
  if (not instr)
    {
      cerr << "Mcm::MergeBufferInsertScalar: Error: Unknown instr tag\n";
      assert(0 && "Error: Mcm::MergeBufferInsertScalar: Unknown instr tag");
      return false;
    }

  auto& undrained = hartData_.at(hartIx).undrainedStores_;
  undrained.insert(tag);

  bool result = true;

  if (writeOnInsert_)
    {
      // Associate write op with instruction.
      instr->addMemOp(sysMemOps_.size());
      sysMemOps_.push_back(op);
      instr->complete_ = checkStoreComplete(hartIx, *instr);
      if (instr->complete_)
	{
	  undrained.erase(tag);
	  checkStoreData(hart, *instr);
	}

      if (not instr->retired_)
	{
	  cerr << "Mcm::MergeBufferInsertScalar: Error: Merge buffer write for a non-retired store\n";
	  return false;
	}

      if (isEnabled(PpoRule::R1))
	result = ppoRule1(hart, *instr) and result;

      if (instr->di_.isAmo() and isEnabled(PpoRule::R3))
	result = ppoRule3(hart, *instr) and result;

      if (isEnabled(PpoRule::R6))
	result = ppoRule6(hart, *instr) and result;

      // We commit the RTL data to memory but we check them against whisper data (in
      // checkStoreData). This is simpler than committing part of whisper instruction
      // data.
      if (not pokeHartMemory(hart, pa, rtlData, op.size_, true))
	result = false;
    }

  return result;
}


template <typename URV>
bool
Mcm<URV>::bypassOp(Hart<URV>& hart, uint64_t time, uint64_t tag, uint64_t pa,
                   unsigned size, uint64_t rtlData, unsigned elemIx, unsigned field, bool cache)
{
  if (not updateTime("Mcm::writeOp", time))
    return false;

  unsigned hartIx = hart.sysHartIndex();
  McmInstr* instr = findOrAddInstr(hartIx, tag);
  if (not instr)
    {
      cerr << "Mcm::bypassOp: Error: hart-id=" << hart.hartId() << " time=" << time
	   << " tag=" << tag << " unknown instr tag\n";
      return false;
    }

  if (instr->canceled_)
    {
      cerr << "Mcm::bypassOp: Error: hart-id=" << hart.hartId() << " time=" << time
           << " tag=" << tag << " bypass op with cancelled instruction\n";
      return false;
    }

  auto& undrained = hartData_.at(hartIx).undrainedStores_;
  if (size > 0)
    undrained.insert(tag);

  bool result = true;

  assert(size <= 8);
  pa = hart.clearSecureAddressSteeBits(pa);

  MemoryOp op = {};
  op.time_ = time;
  op.pa_ = pa;
  op.rtlData_ = rtlData;
  op.tag_ = tag;
  op.hartIx_ = hartIx;
  op.size_ = size;
  op.isRead_ = false;
  op.bypass_ = true;
  op.elemIx_ = elemIx;
  op.field_ = field;
  op.isIo_ = hart.getPma(op.pa_).isIo();

  // Associate write op with instruction.
  instr->addMemOp(sysMemOps_.size());
  sysMemOps_.push_back(op);

  result = pokeHartMemory(hart, pa, rtlData, size, cache) and result;
  
  instr->complete_ = checkStoreComplete(hartIx, *instr);
  if (instr->complete_)
    {
      undrained.erase(tag);

      if (not instr->retired_)
	return result;

      result = checkStoreData(hart, *instr) and result;

      if (isEnabled(PpoRule::R1))
	result = ppoRule1(hart, *instr) and result;

      if (isEnabled(PpoRule::R3))
	result = ppoRule3(hart, *instr) and result;

      if (isEnabled(PpoRule::R6))
	result = ppoRule6(hart, *instr) and result;

      if (instr->di_.extension() == RvExtension::Zicbom)
        result = checkCmo(hart, *instr) and result;
    }

  return result;
}


template <typename URV>
bool
Mcm<URV>::retireStore(Hart<URV>& hart, McmInstr& instr)
{
  auto hartIx = hart.sysHartIndex();

  if (instr.di_.isVectorStore())
    {
      auto& info = hart.getLastVectorMemory();
      auto& elems = info.elems_;
      unsigned elemSize = info.elemSize_;
      if (elemSize == 0)
	return true;   // Not a store.

      instr.size_ = elemSize;
      instr.isStore_ = true;

      auto& vecRefs = hartData_.at(hartIx).vecRefMap_[instr.tag_];

      for (auto& elem : elems)
        {
	  if (elem.skip_)
	    continue;  // Non-active element

	  unsigned dataReg = hart.identifyDataRegister(info, elem);
	  unsigned ixReg = info.isIndexed_ ? dataReg - elem.field_ : 0;

	  uint64_t pa1 = elem.pa_, pa2 = elem.pa2_, value = elem.data_;

	  if (pa1 == pa2)
            {
              vecRefs.add(elem.ix_, pa1, value, elemSize, dataReg, ixReg, elem.field_);
            }
	  else
	    {
	      unsigned size1 = offsetToNextPage(pa1);
	      assert(size1 > 0 and size1 < 8);
	      unsigned size2 = elemSize - size1;
	      uint64_t val1 = (value <<  ((8 - size1)*8)) >> ((8 - size1)*8);
	      uint64_t val2 = (value >> (size1*8));
	      vecRefs.add(elem.ix_, pa1, val1, size1, dataReg, ixReg, elem.field_);
	      vecRefs.add(elem.ix_, pa2, val2, size2, dataReg, ixReg, elem.field_);
	    }
        }

      instr.complete_ = checkStoreComplete(hartIx, instr);

      // This is conservative as it is possible for a non-unit-stride to have no overlap.
      instr.hasOverlap_ = not isUnitStride(info);
    }
  else
    {
      uint64_t vaddr = 0, paddr = 0, paddr2 = 0, value = 0;
      unsigned stSize = hart.lastStore(vaddr, paddr, paddr2, value);

      instr.size_ = stSize;
      instr.virtAddr_ = vaddr;
      instr.physAddr_ = paddr;
      instr.physAddr2_ = paddr2;
      instr.storeData_ = value;
      instr.isStore_ = true;
      instr.complete_ = checkStoreComplete(hartIx, instr);
    }

  bool ok = checkStoreData(hart, instr);

  auto& undrained = hartData_.at(hartIx).undrainedStores_;

  if (not instr.complete_)
    {
      undrained.insert(instr.tag_);
      return ok;
    }

  undrained.erase(instr.tag_);
  return ok;
}


template <typename URV>
bool
Mcm<URV>::retireCmo(Hart<URV>& hart, McmInstr& instrB)
{
  uint64_t vaddr = 0, paddr = 0;
  if (not hart.lastCmo(vaddr, paddr))
    assert(0 && "Error: Assertion failed");

  instrB.size_ = lineSize_;
  instrB.virtAddr_ = vaddr;
  instrB.physAddr_ = paddr;
  instrB.physAddr2_ = paddr;
  instrB.storeData_ = 0;   // Determined at bypass time.

  unsigned hartIx = hart.sysHartIndex();
  auto& undrained = hartData_.at(hartIx).undrainedStores_;

  if (instrB.di_.instId() == InstId::cbo_zero)
    {
      instrB.isStore_ = true;  // To enable forwarding
      instrB.complete_ = checkStoreComplete(hartIx, instrB);
      if (instrB.complete_)
        {
          undrained.erase(instrB.tag_);
          if (isEnabled(PpoRule::R1))
	    return ppoRule1(hart, instrB);
        }
      else
        undrained.insert(instrB.tag_);
    }
  else
    {
      // cbo.clean/flush/inval must complete before retire.
      if (not instrB.complete_)
        std::cerr << "Warning: hart-id=" << hart.hartId() << " tag=" << instrB.tag_
                  << " CMO instruction retires before it is complete (no bypass op seen)\n";
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::checkCmo(Hart<URV>& hart, const McmInstr& instrB) const
{
  assert(instrB.complete_);

  unsigned hartIx = hart.sysHartIndex();

  if (instrB.di_.instId() == InstId::cbo_zero)
    {
      if (isEnabled(PpoRule::R1))
        return ppoRule1(hart, instrB);
      return true;
    }

  // For cbo.flush/clean/inval, all preceding (in program order) overlapping stores/AMOs
  // must have drained. This method (checkCmo) is called at completion time.
  const auto& instrVec = hartData_.at(hartIx).instrVec_;
  auto& undrained = hartData_.at(hartIx).undrainedStores_;

  for (auto storeTag : undrained)
    {
      const auto& instrA =  instrVec.at(storeTag);
      if (instrA.tag_ >= instrB.tag_)
	break;

      if (instrA.isCanceled())
	continue;

      const DecodedInst& di = instrA.di_;
      if ((di.isStore() or di.isAmo()) and overlaps(instrA, instrB))
	{
	  cerr << "Error: PPO rule 1 failed: hart-id=" << hart.hartId() << " tag1="
	       << instrA.tag_ << " tag2=" << instrB.tag_ << " (CMO)\n";
	  return false;
	}
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::isPartialVecLdSt(Hart<URV>& hart, const DecodedInst& di) const
{
  if (not di.isVectorLoad() and not di.isVectorStore())
    return false;

  assert(hart.lastInstructionCancelled());

  URV elems = 0;  // Partially completed elements.
  if (not hart.peekCsr(CsrNumber::VSTART, elems))
    return false;  // Should not happen.

  return elems > 0;
}


template <typename URV>
bool
Mcm<URV>::retire(Hart<URV>& hart, uint64_t time, uint64_t tag,
		 const DecodedInst& di, bool cancelled)
{
  unsigned hartIx = hart.sysHartIndex();
  cancelNonRetired(hart, tag);
  if (not updateTime("Mcm::retire", time))
    return false;

  McmInstr* instr = findOrAddInstr(hartIx, tag);
  if (instr->retired_)
    {
      cerr << "Mcm::retire: Error: Time=" << time << " hart-id=" << hart.hartId()
	   << " tag=" << tag << " Instruction retired multiple times\n";
      return false;
    }

  if (not di.isValid())
    {
      cancelInstr(hart, *instr);  // Instruction took a trap at fetch.
      return true;
    }
  
  instr->priv_ = hart.lastPrivMode();
  instr->virt_ = hart.lastVirtMode();

  // If a partially executed vec ld/st store is cancelled, we commit its results.
  if (cancelled and not isPartialVecLdSt(hart, di))
    {
      cancelInstr(hart, *instr);  // Instruction took a trap.
      return true;
    }

  instr->retired_ = true;
  instr->retireTime_ = time;
  instr->di_ = di;

  bool ok = true;
  if (di.isLoad() or di.isAmo() or di.isVectorLoad())
    instr->isLoad_ = true;
  else if (instr->isLoad_)
    {
      cerr << "Error: Read ops associated with non-load instruction "
           << "hart-id=" << hartIx << " tag="  << tag << " time=" << time << '\n';
      return false;
    }

  if (instr->isLoad_)
    {
      ok = commitReadOps(hart, *instr);
      instr->complete_ = checkLoadComplete(*instr);
    }

  if (instr->di_.instId() == InstId::sfence_vma)
    {
      hartData_.at(hartIx).sinvalVmaTime_ = time;
      hartData_.at(hartIx).sinvalVmaTag_ = tag;
    }

  if (instr->di_.instId() == InstId::sfence_inval_ir)
    return checkSfenceInvalIr(hart, *instr);
  if (instr->di_.instId() == InstId::sfence_w_inval)
    return checkSfenceWInval(hart, *instr);

  if (di.isCmo())
    return retireCmo(hart, *instr);

  // If instruction is a store, save address, size, and written data.
  if (di.isStore() or di.isAmo() or di.isVectorStore())
    ok = retireStore(hart, *instr) and ok;

  // AMO sanity check: Must have both read and write ops.
  if (di.isAmo() and (not instrHasRead(*instr) or not instrHasWrite(*instr)))
    {
      cerr << "Error: Hart-id=" << hart.hartId() << " tag=" << tag
	   << " AMO instruction retired before read/write op\n";
      return false;
    }

  if (di.isAmo())
    instr->isStore_ = true;  // AMO is both load and store.

  // Set data/address producer times (if any) for current instruction.
  setProducerTime(hart, *instr);

  if (instr->isStore_ and instr->complete_)
    {
      if (isEnabled(PpoRule::R1))
	ok = ppoRule1(hart, *instr) and ok;
    }

  if (instr->isLoad_)
    ok = checkLoadVsPriorCmo(hart, *instr) and ok;

  if (isEnabled(PpoRule::R2))
    ok = ppoRule2(hart, *instr) and ok;

  if (isEnabled(PpoRule::R3))
    ok = ppoRule3(hart, *instr) and ok;

  if (isEnabled(PpoRule::R4))
    ok = ppoRule4(hart, *instr) and ok;

  if (isEnabled(PpoRule::R5))
    ok = ppoRule5(hart, *instr) and ok;

  if (isEnabled(PpoRule::R6))
    ok = ppoRule6(hart, *instr) and ok;

  if (isEnabled(PpoRule::R7))
    ok = ppoRule7(hart, *instr) and ok;

  if (isEnabled(PpoRule::R8))
    ok = ppoRule8(hart, *instr) and ok;

  if (isEnabled(PpoRule::R9))
    ok = ppoRule9(hart, *instr) and ok;

  if (isEnabled(PpoRule::R10))
    ok = ppoRule10(hart, *instr) and ok;

  if (isEnabled(PpoRule::R11))
    ok = ppoRule11(hart, *instr) and ok;

  if (isEnabled(PpoRule::R12))
    ok = ppoRule12(hart, *instr) and ok;

  if (isEnabled(PpoRule::R13))
    ok = ppoRule13(hart, *instr) and ok;

  if (isEnabled(PpoRule::Io))
    ok = ioPpoChecks(hart, *instr) and ok;

  updateDependencies(hart, *instr);
  setBranchMemTime(hart, *instr);

  return ok;
}


static bool
checkBufferWriteParams(unsigned hartId, uint64_t time, unsigned lineSize,
		       uint64_t& rtlLineSize, uint64_t physAddr)
{
  if (lineSize == 0)
    {
      cerr << "Error: Merge buffer write attempted when merge buffer is disabled\n";
      return false;
    }

  if (rtlLineSize > lineSize)
    {
      cerr << "Error: Hart-id=" << hartId << " time=" << time
	   << "RTL merge buffer write line size (" << rtlLineSize << ") greater than"
	   << " reference line size (" << lineSize << ")\n";
      return false;
    }

  if ((physAddr % lineSize) + rtlLineSize > lineSize)
    {
      cerr << "Error: Hart-id=" << hartId << " time=" << time
	   << " RTL merge buffer write data at address 0x"
	   << std::hex << physAddr << " crosses buffer boundary" << std::dec
	   << " -- truncating RTL data\n";
	rtlLineSize -= (physAddr % lineSize);
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::collectCoveredWrites(Hart<URV>& hart, uint64_t time, uint64_t rtlAddr,
			       uint64_t rtlLineSize, const std::vector<bool>& rtlMask,
			       MemoryOpVec& coveredWrites)
{
  unsigned hartIx = hart.sysHartIndex();
  auto& pendingWrites = hartData_.at(hartIx).pendingWrites_;
  size_t pendingSize = 0;  // pendingWrite size after removal of matching writes

  uint64_t lineEnd = rtlAddr + rtlLineSize;

  bool ok = true;

  for (size_t i = 0; i < pendingWrites.size(); ++i)
    {
      auto& op = pendingWrites.at(i);  // Write op

      McmInstr* instr = findOrAddInstr(hartIx, op.tag_);

      bool written = false;  // True if op is actually written

      // We don't write if mbinsert happens as the same time as mbwrite.
      if (op.pa_ >= rtlAddr and op.pa_ < lineEnd and op.time_ != time)
	{
	  if (op.pa_ + op.size_  > lineEnd)
	    {
	      cerr << "Error: Pending write address out of line bounds time=" << time
		   << " hart-id=" << hart.hartId() << " addr=0x" << std::hex
		   << op.pa_ << std::dec << "\n";
	      ok = false;
	    }

	  if (not instr or instr->isCanceled())
	    {
	      cerr << "Error: Write for an invalid/speculated store time=" << time
		   << " hart-id=" << hart.hartId() << " tag=" << op.tag_
		   << " addr=0x" << std::hex << op.pa_ << std::dec << "\n";
	      ok = false;
	    }

	  if (rtlMask.empty())
	    written = true;  // No masking
	  else
	    {
	      // Check if op bytes are all masked or all un-masked.
	      unsigned masked = 0;  // Count of masked bytes of op.
	      for (unsigned opIx = 0; opIx < op.size_; ++opIx)   // Scan op bytes
		{
		  unsigned lineIx = opIx + op.pa_ - rtlAddr; // Index in line
		  if (lineIx < rtlMask.size() and rtlMask.at(lineIx))
		    masked++;
		}
	      written = masked != 0;
	      if (written and masked != op.size_)
		{
		  cerr << "Error: hart-id=" << hart.hartId() << " time=" << time
		       << " tag=" << op.tag_ << " addr=0x" << std::hex
		       << op.pa_ << std::dec << " Merge buffer insert operation"
		       << " is only partially covered by a merge buffer write\n";
		  ok = false;
		}
	    }
	}

      if (written)
	{
	  coveredWrites.push_back(op);
	}
      else
	{
	  // Op is not written, keep it in pending writes.
	  if (i != pendingSize)
	    pendingWrites.at(pendingSize) = pendingWrites.at(i);
	  pendingSize++;
	}
    }
  pendingWrites.resize(pendingSize);

  // Check that the collected writes are in instruction order and in time order.
  if (coveredWrites.empty())
    return true;
  for (size_t i = 1; i < coveredWrites.size(); ++i)
    {
      const auto& prev = coveredWrites.at(i-1);
      const auto& op = coveredWrites.at(i);
      if (op.tag_ < prev.tag_)
	{
	  cerr << "Error: hart-id=" << hart.hartId() << " time=" << time
	       << " tag1=" << prev.tag_ << " tag2=" << op.tag_
	       << " time1=" << prev.time_ << " time2=" << op.time_
	       << " merge buffer has instructions not in program order\n";
	  ok = false;
	}
      assert(op.time_ >= prev.time_);
    }

  // Change the times of the collected writes to the current time. This is the global
  // memory time of those operations. Commit collected writes to their instructions and to
  // the global memory operations vector.
  for (auto& op : coveredWrites)
    {
      op.time_ = time;
      McmInstr* instr = findOrAddInstr(hartIx, op.tag_);
      instr->addMemOp(sysMemOps_.size());
      sysMemOps_.push_back(op);
    }

  return ok;
}


template <typename URV>
bool
Mcm<URV>::mergeBufferWrite(Hart<URV>& hart, uint64_t time, uint64_t physAddr,
			   const std::vector<uint8_t>& rtlData,
			   const std::vector<bool>& rtlMask,
                           bool skipCheck)
{
  if (not updateTime("Mcm::mergeBufferWrite", time))
    return false;

  uint64_t rtlSize = rtlData.size();

  // The secure world may have changed before the merge buffer is drained. Clear the STEE
  // bits unconditionally.
  physAddr = hart.clearSteeBits(physAddr);

  if (not checkBufferWriteParams(hart.hartId(), time, lineSize_, rtlSize, physAddr))
    return false;

  uint64_t lineAddr = lineAlign(physAddr);
  hart.cancelOtherHartsLr(physAddr);

  unsigned hartIx = hart.sysHartIndex();

  // Remove from hartPendingWrites_ the writes matching the RTL line
  // address and place them sorted by instr tag in coveredWrites.
  std::vector<MemoryOp> coveredWrites;
  if (not collectCoveredWrites(hart, time, physAddr, rtlSize, rtlMask, coveredWrites))
    return false;

  // Read our memory corresponding to RTL line addresses.
  uint64_t lineEnd = lineAddr + lineSize_;
  std::vector<uint8_t> line;
  line.reserve(lineSize_);
  for (uint64_t addr = physAddr; addr < lineEnd; ++addr)
    {
      uint8_t byte = 0;
      if (not hart.peekMemory(addr, byte, true /*usePma*/))
	{
	  cerr << "Error: Mcm::mergeBufferWrite: Failed to query memory\n";
	  return false;
	}
      line.push_back(byte);
    }

  std::vector<McmInstrIx> insertTags(lineSize_);

  bool result = true;

  if (not skipCheck)
    {
      // Apply pending writes (from mbinsert operations) to our line and to memory.
      for (const auto& write : coveredWrites)
        {
          if ((write.pa_ < physAddr) or (write.pa_ + write.size_ > lineEnd))
            {
              cerr << "Error: Mcm::mergeBufferWrite: Store address out of line bound\n";
              return false;
            }

          assert(write.size_ <= 8);
          pokeHartMemory(hart, write.pa_, write.rtlData_, write.size_, true);

          unsigned ix = write.pa_ - physAddr;
          for (unsigned i = 0; i < write.size_; ++i)
            {
              line.at(ix+i) = ((uint8_t*) &(write.rtlData_))[i];
              insertTags.at(ix+i) = write.tag_;
            }
        }

      // Compare inserted data to written (drained) data.
      size_t count = std::min(line.size(), rtlData.size());
      for (unsigned i = 0; i < count; ++i)
        if ((rtlMask.empty() or rtlMask.at(i)) and (line.at(i) != rtlData.at(i)))
          {
            cerr << "Error: hart-id=" << hart.hartId() << " time=" << time;
            uint64_t addr = physAddr + i;
            if (insertTags.at(i) == 0)
              cerr << " merge-buffer write without corresponding insert addr=0x"
                   << std::hex << addr << std::dec << '\n';
            else
              cerr << " merge-buffer write does not match merge-buffer insert addr=0x"
                   << std::hex << addr << " write-data=0x" << unsigned(rtlData.at(i))
                   << " insert-data=0x" << unsigned(line.at(i)) << std::dec
                   << " insert-tag=" << insertTags.at(i) << '\n';
            result = false;
            break;
          }
    }

  auto& instrVec = hartData_.at(hartIx).instrVec_;
  auto& undrained = hartData_.at(hartIx).undrainedStores_;

  for (size_t i = 0; i < coveredWrites.size(); ++i)
    {
      auto tag = coveredWrites.at(i).tag_;
      if (i > 0 and tag == coveredWrites.at(i-1).tag_)
	continue;
      auto instr = findInstr(hartIx, tag);
      if (not instr)
	{
	  cerr << "Error: Mcm::mergeBufferWrite: Covered instruction tag is invalid\n";
	  return false;
	}
      if (checkStoreComplete(hartIx, *instr))
	{
	  instr->complete_ = true;
	  undrained.erase(instr->tag_);
	  checkStoreData(hart, *instr);
	  if (isEnabled(PpoRule::R1))
	    result = ppoRule1(hart, *instr) and result;
	  if (isEnabled(PpoRule::R3) and instr->di_.isAmo())
	    result = ppoRule1(hart, *instr) and result;
	  if (isEnabled(PpoRule::R6))
	    result = ppoRule6(hart, *instr) and result;
	}
      if (instr->retired_ and instr->di_.isSc())
	{
	  if (not instr->complete_)
	    {
	      cerr << "Error: Mcm::mergeBufferWrite: sc instruction written before complete\n";
	      return false;
	    }
	  for (uint64_t tag = instr->tag_; tag < instrVec.size(); ++tag)
	    if (instrVec.at(tag).retired_)
	      updateDependencies(hart, instrVec.at(tag));
	}
    }

  return result;
}


template <typename URV>
bool
Mcm<URV>::writeToReadForward(const MemoryOp& writeOp, MemoryOp& readOp, uint64_t& mask)
{
  if (mask == 0)
    return true;  // No bytes left to forward.

  if (not readOp.overlaps(writeOp))
    return false;

  unsigned count = 0; // Count of forwarded bytes
  for (unsigned rix = 0; rix < readOp.size_ and mask != 0; ++rix)
    {
      uint64_t byteAddr = readOp.pa_ + rix;
      if (not writeOp.overlaps(byteAddr))
	continue;  // Read-op byte does not overlap write-op.

      uint64_t byteMask = uint64_t(0xff) << (rix * 8);
      if ((byteMask & mask) == 0)
	continue;  // Byte forwarded by another instruction.

      uint8_t byteVal = writeOp.rtlData_ >> (byteAddr - writeOp.pa_)*8;
      uint64_t aligned = uint64_t(byteVal) << 8*rix;
	
      readOp.data_ = (readOp.data_ & ~byteMask) | aligned;
      mask = mask & ~byteMask;
      count++;
    }

  return count > 0;
}


template <typename URV>
void
Mcm<URV>::cancelInstr(Hart<URV>& hart, McmInstr& instr)
{
  if (instr.isCanceled())
    return;

  unsigned hartIx = hart.sysHartIndex();
  auto& undrained = hartData_.at(hartIx).undrainedStores_;

  auto iter = undrained.find(instr.tag_);

  if (iter != undrained.end())
    {
      cerr << "Warning: Hart-id=" << hart.hartId() << " tag=" << instr.tag_
	   << " canceled or trapped instruction has a write operation\n";
      undrained.erase(iter);
    }

  for (auto memIx : instr.memOps_)
    {
      auto& op = sysMemOps_.at(memIx);
      op.cancel();
    }

  instr.cancel();
}


template <typename URV>
void
Mcm<URV>::cancelNonRetired(Hart<URV>& hart, uint64_t tag)
{
  unsigned hartIx = hart.sysHartIndex();
  auto& vec = hartData_.at(hartIx).instrVec_;

  if (vec.empty())
    return;

  if (tag >= vec.size())
    tag = vec.size();

  while (tag)
    {
      auto& instr = vec.at(--tag);
      if (instr.retired_ or instr.canceled_)
	break;
      cancelInstr(hart, instr);
    }
}


template <typename URV>
void
Mcm<URV>::cancelInstruction(Hart<URV>& hart, uint64_t tag)
{
  unsigned hartIx = hart.sysHartIndex();
  McmInstr* instr = findInstr(hartIx, tag);
  if (not instr or instr->isCanceled())
    return;
  cancelInstr(hart, *instr);
}


/// Helper to read-commit methods: commitReadOps & commitVecReadOPs.
template <typename URV>
void
Mcm<URV>::printReadMismatch(Hart<URV>& hart, uint64_t time, uint64_t tag, uint64_t addr,
                            unsigned size, uint64_t rtlData, uint64_t refData) const
{
  cerr << "Error: hart-id=" << hart.hartId() << " tag=" << tag << " time="
       << time << " RTL/whisper read mismatch " << " addr=0x" << std::hex
       << addr << " size=" << size << " rtl=0x" << rtlData
       << " whisper=0x" << refData << std::dec;

  auto pma = hart.getPma(addr);
  pma = hart.overridePmaWithPbmt(pma, hart.virtMem().lastEffectivePbmt());

  const char* type = nullptr;

  if (pma.isIo())
    type = "io";
  else if (not pma.isCacheable())
    type = "nc";
  else
    type = "c";

  if (type)
    cerr << " type=" << type;
  cerr << "\n";
}


template <typename URV>
bool
Mcm<URV>::checkRtlRead(Hart<URV>& hart, const McmInstr& instr,
		       const MemoryOp& op) const
{
  if (op.size_ > instr.size_)
    {
      cerr << "Warning: Read operation size (" << unsigned(op.size_) << ") larger than "
	   << "instruction data size (" << unsigned(instr.size_) << "): Hart-id="
	   << hart.hartId() << " time=" << op.time_ << " tag=" << instr.tag_ << '\n';
    }

  uint64_t addr = op.pa_;
  bool skip = ( hart.isDeviceAddr(addr) or hart.isMemMappedReg(addr) or hart.isHtifAddr(addr) );

  // Major hack (temporary until RTL removes CLINT device).
  skip = skip or (addr >= 0x2000000 and addr < 0x200c000);

  // Major hack (temporary until RTL HTIF addresses are rationalized).
  skip = skip or (addr >= 0x70000000 and addr <= 0x70000008);

  skip = skip or (not isReadDataCheckEnabled(addr));

  if (skip)
    return true;


  if (op.rtlData_ != op.data_)
    {
      printReadMismatch(hart, op.time_, op.tag_, addr, op.size_, op.rtlData_, op.data_);
      return false;
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::checkRtlWrite(unsigned hartId, const McmInstr& instr,
			const MemoryOp& op) const
{
  if (instr.size_ == 0)
    {
      cerr << "Error: Hart-id=" << hartId << " time=" << op.time_ << " tag="
           << instr.tag_;
      if (instr.di_.isSc())
        cerr << " merge buffer " << (op.bypass_ ? "bypass" : "insert")
             << " operation for a non-successful store-conditional instruction\n";
      else
        cerr << " merge buffer insert/bypass for a non-store instruction\n";

      return false;
    }

  assert(not instr.di_.isVector());

  if (op.size_ > instr.size_)
    {
      cerr << "Error: Write size exceeds store instruction size: " << "Hart-id="
	   << hartId << " time=" << time_ << " tag=" << instr.tag_ << " write-size="
	   << unsigned(op.size_) << " store-size=" << unsigned(instr.size_) << '\n';
      return false;
    }

  uint64_t data = instr.storeData_;

  if (op.size_ < instr.size_)
    {
      uint64_t shift = (op.pa_ - instr.physAddr_) * 8;
      data = data >> shift;
      shift = 64 - op.size_*8;
      data = (data << shift) >> shift;
    }

  if (data == op.rtlData_)
    return true;

  const char* tag = instr.di_.isAmo()? " AMO " : " ";

  cerr << "Error: RTL/whisper" << tag << "write mismatch time=" << op.time_
       << " hart-id=" << hartId << " instr-tag="
       << instr.tag_ << " addr=0x" << std::hex << op.pa_
       << " size=" << unsigned(op.size_) << " rtl=0x" << op.rtlData_
       << " whisper=0x" << data << std::dec << '\n';

  return false;
}


template <typename URV>
bool
Mcm<URV>::checkStoreData(Hart<URV>& hart, const McmInstr& store) const
{
  auto hartId = hart.hartId();

  if (store.di_.isVector())
    return checkVecStoreData(hart, store);
  
  // Scalar store
  for (auto opIx : store.memOps_)
    {
      const auto& op = sysMemOps_.at(opIx);
      if (not op.isRead_ and not checkRtlWrite(hartId, store, op))
	return false;
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::checkVecStoreData(Hart<URV>& hart, const McmInstr& store) const
{
  if (not store.di_.isVector())
    return true;

  if (not store.complete_)
    return true;  // Will check again once store is complete.

  auto hartId = hart.hartId();

  // 1. Put RTL byte data in a address to value map.
  std::unordered_map<uint64_t, uint8_t> rtlData;
  for (auto opIx : store.memOps_)
    {
      auto& op = sysMemOps_.at(opIx);
      for (unsigned i = 0; i < op.size_; ++i)
	{
	  uint64_t addr = op.pa_ +i;
	  uint8_t val = op.rtlData_ >> (i*8);
          rtlData[addr] = val;
	}
    }

  // 2. Put reference data in a map.
  auto& vecRefMap = hartData_.at(store.hartIx_).vecRefMap_;
  auto iter = vecRefMap.find(store.tag_);
  assert(iter != vecRefMap.end());
  auto& vecRefs = iter->second;

  std::unordered_map<uint64_t, uint8_t> refData;
  for (auto& ref : vecRefs.refs_)
    {
      for (unsigned i = 0; i < ref.size_; ++i)
	{
	  uint64_t addr = ref.pa_ + i;
	  uint8_t val = ref.data_ >> (i*8);
          refData[addr] = val;
        }
    }

  auto tag = store.tag_;

  // 3. Compare ref data to RTL data.
  for (auto& [addr, rtlVal] : rtlData)
    {
      if (not refData.contains(addr))
        {
          cerr << "Error: hart-id=" << hartId << " tag=" << tag
               << " mismatch on vector store data: addr=0x" << std::hex << addr
               << " rtl=0x" << unsigned(rtlVal) << " whisper=none" << std::dec << '\n';
          return false;
        }
      uint8_t refVal = refData[addr];
      if (rtlVal != refVal)
        {
          cerr << "Error: hart-id=" << hartId << " tag=" << tag
               << " mismatch on vector store data: addr=0x" << std::hex << addr
               << " rtl=0x" << unsigned(rtlVal) << " whisper=0x" << unsigned(refVal)
               << std::dec << '\n';
          return false;
        }
    }

  if (rtlData.size() != refData.size())
    {
      // Check if RTL is missing writes.
      for (auto& [addr, refVal] : rtlData)
        {
          if (rtlData.contains(addr))
            continue;
          cerr << "Error: hart-id=" << hartId << " tag=" << tag
               << " mismatch on vector store data: addr=0x" << std::hex << addr
               << " whisper=0x" << unsigned(refVal) << " rgl=none" << std::dec << '\n';
          return false;
        }
    }

  return true;
}


/// Return a mask where the ith bit is set if addr + i is in the range
/// [cover, cover + coverSize - 1]
unsigned
maskCoveredBytes(uint64_t addr, unsigned size, uint64_t cover, unsigned coverSize)
{
  if (cover <= addr)
    {
      if (cover + coverSize > addr)
	{
	  uint64_t overlap = cover + coverSize - addr;
	  if (overlap > size)
	    overlap = size;
	  assert(overlap > 0 and overlap <= 8);
	  return (1 << overlap) - 1;
	}

      return 0;  // No overlap.
    }

  if (addr + size > cover)
    {
      uint64_t overlap = addr + size - cover;
      if (overlap > coverSize)
	overlap = coverSize;
      assert(overlap > 0 and overlap <= 8);
      unsigned mask = (1 << overlap) - 1;
      mask = mask << (cover - addr);
      return mask;
    }

  return 0;  // No overlap.
}


template <typename URV>
bool
Mcm<URV>::checkStoreComplete(unsigned hartIx, const McmInstr& instr) const
{
  if (instr.isCanceled() or not instr.retired_)
    return false;

  if (instr.di_.extension() == RvExtension::Zicbom)
    {
      // Cbo.flush/clean/inval are not marked store to disable forwarding.
      // For cbo.flush/clean/inval test-bench sends 1 zero-size bypass operation.
      if (instr.memOps_.size() == 1)
        {
          const auto& op = sysMemOps_.at(instr.memOps_.at(0));
          assert(op.size_ == 0 and op.bypass_);
          return op.size_ == 0 and op.bypass_;
        }
      assert(instr.memOps_.size() == 0);
      return false;
    }

  if (not instr.isStore_)
    return false;

  if (instr.di_.instId() == InstId::cbo_zero)
    {
      unsigned count = 0;
      for (auto opIx : instr.memOps_)
	{
	  const auto& op = sysMemOps_.at(opIx);
	  count += op.size_;
	}
      return count == lineSize_;
    }

  if (instr.di_.isVector())
    {
      const auto& vecRefMap = hartData_.at(hartIx).vecRefMap_;
      auto iter = vecRefMap.find(instr.tag_);
      if (iter == vecRefMap.end())
	return false;

      auto& vecRefs = iter->second;

      // Map address to number of times address is covered by an element.
      std::unordered_map<uint64_t, unsigned> byteCover;

      for (const auto& ref : vecRefs.refs_)
	for (unsigned i = 0; i < ref.size_; ++i)
	  byteCover[ref.pa_ + i]++;
	  
      // There must be a write op for each element.
      for (auto opIx : instr.memOps_)
	{
	  const auto& op = sysMemOps_.at(opIx);
	  for (unsigned i = 0; i < op.size_; ++i)
	    {
	      auto& cover = byteCover[op.pa_ + i];
	      if (cover)
		cover--;
	    }
	}

      unsigned missing = 0;
      for (auto& [addr, count] : byteCover)
	missing += count;
      return missing == 0;
    }

  unsigned expectedMask = (1 << instr.size_) - 1;  // Mask of bytes covered by instruction.
  unsigned writeMask = 0;   // Mask of bytes covered by write operations.
  uint64_t addr = instr.physAddr_, addr2 = instr.physAddr2_, size = instr.size_;
  for (auto opIx : instr.memOps_)
    {
      if (opIx >= sysMemOps_.size())
	continue;
      const auto& op = sysMemOps_.at(opIx);
      if (op.isRead_)
	continue;

      unsigned mask = 0;
      if (addr == addr2)
	mask = maskCoveredBytes(addr, size, op.pa_, op.size_);
      else
	{
          unsigned size1 = offsetToNextPage(addr);
          mask = maskCoveredBytes(addr, size1, op.pa_, op.size_);

          unsigned size2 = size - size1;
          unsigned mask2 = maskCoveredBytes(addr2, size2, op.pa_, op.size_);
          mask2 <<= size1;
          mask |= mask2;
	}

      mask &= expectedMask;
      writeMask |= mask;
    }

  return writeMask == expectedMask;
}


template <typename URV>
bool
Mcm<URV>::checkLoadComplete(const McmInstr& instr) const
{
  if (instr.isCanceled() or not instr.isLoad_ or instr.size_ == 0)
    return false;

  unsigned expectedMask = (1 << instr.size_) - 1;  // Mask of bytes covered by instruction.
  unsigned readMask = 0;   // Mask of bytes covered by read operations.
  uint64_t addr = instr.physAddr_, addr2 = instr.physAddr2_, size = instr.size_;
  for (auto opIx : instr.memOps_)
    {
      if (opIx >= sysMemOps_.size())
	continue;
      const auto& op = sysMemOps_.at(opIx);
      if (not op.isRead_)
	continue;

      unsigned mask = 0;
      if (addr == addr2)
	mask = maskCoveredBytes(addr, size, op.pa_, op.size_);
      else
	{
	  unsigned size1 = offsetToNextPage(addr);
          mask = maskCoveredBytes(addr, size1, op.pa_, op.size_);

          unsigned size2 = size - size1;
          unsigned mask2 = maskCoveredBytes(addr2, size2, op.pa_, op.size_);
          mask2 <<= size1;
          mask |= mask2;
	}

      mask &= expectedMask;
      readMask |= mask;
    }

  return readMask == expectedMask;
}


/// Return a mask of the bytes of the given address range that are
/// covered by the given memory operation. Bit i of the returned mask
/// will be set if byte at addr+i is covered by op.
unsigned
getMask(uint64_t addr, unsigned size, const MemoryOp& op)
{
  unsigned mask = 0;

  if (op.pa_ <= addr)
    {
      if (op.pa_ + op.size_ <= addr)
	return mask;  // Op does not overlap address range.
      uint64_t overlap = op.pa_ + op.size_ - addr;
      if (overlap > size)
	overlap = size;
      assert(overlap > 0 and overlap <= 8);
      mask = (1 << overlap) - 1;
    }
  else
    {
      if (addr + size <= op.pa_)
	return mask;  // Op does not overlap address range.
      uint64_t overlap = addr + size - op.pa_;
      if (overlap > op.size_)
	overlap = op.size_;
      mask = (1 << overlap) - 1;
      mask = mask << (op.pa_ - addr);
    }

  return mask;
}


template <typename URV>
unsigned
Mcm<URV>::determineOpMask(const McmInstr& instr, const MemoryOp& op) const
{
  unsigned size = instr.size_;
  uint64_t addr1 = instr.physAddr_, addr2 = instr.physAddr2_;

  if (addr1 == addr2)
    return getMask(addr1, size, op);

  unsigned size1 = offsetToNextPage(addr1);

  if (pageNum(op.pa_) == pageNum(addr1))
    {
      assert(size1 < size);
      return getMask(addr1, size1, op);
    }

  if (pageNum(op.pa_) == pageNum(addr2))
    {
      unsigned size2 = size - size1;
      unsigned mask = getMask(addr2, size2, op);
      mask = mask << size1;
      return mask;
    }

  return 0;  // no overlap.
}


/// If given memory operation overlaps the given address range then
/// set its high end to the end of the address range.
void
trimOp(MemoryOp& op, uint64_t addr, unsigned size)
{
  if (op.pa_ <= addr)
    {
      if (op.pa_ + op.size_ <= addr)
	return;  // Op does not overlap instruction.
      if (op.pa_ + op.size_ > addr + size)
	op.size_ = addr + size - op.pa_;  // Trim wide op.
    }
  else
    {
      if (addr + size <= op.pa_)
	return;  // Op does no overlap instruction.
      if (op.pa_ + op.size_ > addr + size)
	op.size_ = addr + size - op.pa_;  // Trim wide op.
    }

  unsigned n = sizeof(op.data_) - op.size_;  // Unused most sig bytes.
  op.data_ = ((op.data_) << n*8) >> (n*8);
  op.rtlData_ = ((op.rtlData_) << (n*8)) >> (n*8);
}


template <typename URV>
void
Mcm<URV>::trimMemoryOp(const McmInstr& instr, MemoryOp& op)
{
  unsigned size = instr.size_;
  uint64_t addr1 = instr.physAddr_, addr2 = instr.physAddr2_;

  if (addr1 == addr2)
    trimOp(op, addr1, size);
  else
    {
      unsigned size1 = offsetToNextPage(addr1);
      if (pageNum(op.pa_) == pageNum(addr1))
	{
	  assert(size1 < size);
	  trimOp(op, addr1, size1);
	}
      else if (pageNum(op.pa_) == pageNum(addr2))
	{
	  unsigned size2 = size - size1;
	  trimOp(op, addr2, size2);
	}
    }
}


template <typename URV>
void
Mcm<URV>::collectVecRefElems(Hart<URV>& hart, McmInstr& instr, unsigned& activeCount)
{
  auto& vecRefs = hartData_.at(hart.sysHartIndex()).vecRefMap_[instr.tag_];

  const VecLdStInfo& info = hart.getLastVectorMemory();
  const std::vector<VecLdStElem>& elems = info.elems_;

  unsigned elemSize = info.elemSize_;

  activeCount = 0;

  for (auto& elem : elems)
    {
      if (elem.skip_)
	continue;  // Non-active element.

      activeCount++;

      unsigned size1 = elemSize, size2 = 0;
      uint64_t ea1 = elem.pa_, ea2 = elem.pa2_;

      unsigned dataReg = hart.identifyDataRegister(info, elem);
      unsigned ixReg = info.isIndexed_ ? dataReg - elem.field_ : 0;

      if (ea1 != ea2 and pageNum(ea1) != pageNum(ea2))
	{
	  size1 = offsetToNextPage(ea1);
	  size2 = elemSize - size1;
	  assert(size1 > 0 and size1 < elemSize);
	  assert(size2 > 0 and size2 < elemSize);
	  vecRefs.add(elem.ix_, ea1, 0, size1, dataReg, ixReg, elem.field_);
	  vecRefs.add(elem.ix_, ea2, 0, size2, dataReg, ixReg, elem.field_);
	}
      else
	vecRefs.add(elem.ix_, ea1, 0, size1, dataReg, ixReg, elem.field_);
    }
}


struct RefElemCoord
{
  uint64_t addr = 0;
  uint16_t ix = 0;

  bool operator== (const RefElemCoord& other) const
  { return addr == other.addr and ix == other.ix; }
};


// Map a reference address to a reference value and a flag indicating if address is
// covered by a read op.
struct RefElemByte
{
  uint8_t value = 0;
  bool covered = false;
};


template<>
struct std::hash<RefElemCoord>
{
  std::size_t operator()(const RefElemCoord& rec) const noexcept
    {
      std::size_t h1 = std::hash<uint64_t>{}(rec.addr);
      std::size_t h2 = std::hash<uint16_t>{}(rec.ix);
      return h1 ^ (h2 << 1);
    }
};



template <typename URV>
bool
Mcm<URV>::commitVecReadOpsStride0(Hart<URV>& hart, McmInstr& instr)
{
  // Special case: strided with a stride of 0. All we need is for the elements at one
  // active index to be covered.  The reference elements vector begins with the 1st active
  // element. We process vecRefs[0].
  auto& vecRefs = hartData_.at(hart.sysHartIndex()).vecRefMap_[instr.tag_];

  const auto& info = hart.getLastVectorMemory();
  const auto& elems = info.elems_;

  unsigned elemSize = info.elemSize_;
  unsigned vstart = elems.front().ix_;

  assert(info.fields_ == 0);

  bool matched = true;   // True until a mismatch.
  instr.complete_ = true;  // True until byte not covered.

  // Process read ops in reverse order. Trim each op to the reference addresses. Keep ops
  // (marking them as not canceled) where at least one address remains. Mark reference
  // addresses covered by read ops. Set reference (Whisper) values of reference addresses.
  auto& ops = instr.memOps_;

  unsigned mask = (1u << elemSize) - 1;

  const auto& refs = vecRefs.refs_;
  const auto& vecRef = refs.front();

  for (auto iter = ops.rbegin(); iter != ops.rend(); ++iter)
    {
      auto opIx = *iter;
      auto& op = sysMemOps_.at(opIx);
      if (not op.isRead_)
        continue;  // Should not happen.

      if (op.elemIx_ < vstart)
        continue;

      unsigned offset = op.elemIx_ - vstart;  // Location of op.elemIx_ in elems.

      if (offset >= elems.size() or elems.at(offset).skip_)
        continue;  // Skipped element.

      for (unsigned i = 0; i < elemSize; ++i)
        {
          unsigned byteMask = 1 << i;

          uint64_t byteAddr = vecRef.pa_ + i;  // TODO handle page crosser
          if (not op.overlaps(byteAddr))
            continue;

          mask &= ~byteMask;
          op.canceled_ = false;

          if (not isReadDataCheckEnabled(byteAddr))
            continue;

          unsigned offset = byteAddr - op.pa_;
          uint8_t refVal = op.data_ >> (offset*8);
          uint8_t rtlVal = op.rtlData_ >> (offset*8);

          if (refVal != rtlVal)
            {
              if (matched)
                printReadMismatch(hart, op.time_, op.tag_, byteAddr, op.size_, rtlVal, refVal);
              matched = false;
            }
        }
    }

  if (mask and instr.complete_)
    {
      cerr << "Error: hart-id=" << hart.hartId() << " tag=" << instr.tag_
           << " elem-ix=" << unsigned(vecRef.ix_) << " addr=0x" << std::hex << vecRef.pa_ << std::dec 
           << " read ops do not cover all the bytes of vector load instruction\n";
      instr.complete_ = false;
    }

  // Remove ops still marked canceled.
  std::erase_if(ops, [this](MemoryOpIx ix) {
    return ix >= sysMemOps_.size() or sysMemOps_.at(ix).isCanceled();
  });

  return instr.complete_ and matched;
}


template <typename URV>
bool
Mcm<URV>::commitVecReadOpsUnitStride(Hart<URV>& hart, McmInstr& instr)
{
  // Map a reference address to a flag indicating if elem is covered by a read op.
  std::unordered_map<uint64_t, bool> addrMap;

  // Collect reference byte addresses in addrMap.
  auto& vecRefs = hartData_.at(hart.sysHartIndex()).vecRefMap_[instr.tag_];
  for (auto& ref : vecRefs.refs_)
    for (unsigned i = 0; i < ref.size_; ++i)
      addrMap[ref.pa_ + i] = false;

  // Process read ops in reverse order. Trim each op to the reference addresses. Keep ops
  // (marking them as not canceled) where at least one address remains. Mark reference
  // addresses covered by read ops. Set reference (Whisper) values of reference addresses.
  auto& ops = instr.memOps_;

  bool ok = true;

  for (auto iter = ops.rbegin(); iter != ops.rend(); ++iter)
    {
      auto opIx = *iter;
      auto& op = sysMemOps_.at(opIx);
      if (not op.isRead_)
        continue;  // Should not happen.

      uint64_t low = ~uint64_t(0), high = 0; // Range of op addresses overlapping reference.
      bool mismatch = false; // True if mismatch in op
      for (unsigned i = 0; i < op.size_; ++i)
        {
          uint64_t addr = op.pa_ + i;
          auto iter = addrMap.find(addr);
          if (iter == addrMap.end())
            continue;    // No overlap with instruction.

          bool& covered = iter->second;  // Ref elem byte covered
          if (covered)
            continue;  // Address already covered by another read op.

          covered = true;

          if (isReadDataCheckEnabled(addr))
            {
              uint8_t refVal = op.data_ >> (i*8);
              uint8_t rtlVal = op.rtlData_ >> (i*8);

              if (refVal != rtlVal)
                {
                  if (not mismatch)
                    printReadMismatch(hart, op.time_, op.tag_, addr, op.size_, rtlVal, refVal);
                  mismatch = true;
                  ok = false;
                }
            }

          low = std::min(low, addr);
          high = std::max(high, addr);
        }

      if (low <= high)
        {
          unsigned size = high - low + 1;
          trimOp(op, low, size);
          op.canceled_ = false;
        }
    }

  // Remove ops still marked canceled.
  std::erase_if(ops, [this](MemoryOpIx ix) {
    return ix >= sysMemOps_.size() or sysMemOps_.at(ix).isCanceled();
  });

  // Check that all reference addresses are covered by the read operations.
  instr.complete_ = true;
  for (const auto& [addr, covered] : addrMap)
    if (not covered)
      {
	instr.complete_ = false;
	cerr << "Error: hart-id=" << hart.hartId() << " tag=" << instr.tag_
	     << " addr=0x" << std::hex << addr << std::dec 
	     << " read ops do not cover all the bytes of vector load instruction\n";
	return false;
      }

  return ok;


}


template <typename URV>
bool
Mcm<URV>::commitVecReadOps(Hart<URV>& hart, McmInstr& instr)
{
  const VecLdStInfo& info = hart.getLastVectorMemory();

  unsigned elemSize = info.elemSize_;
  if (elemSize == 0)
    {
      cerr << "Error: Mcm::commitVecReadOps: hart-id=" << hart.hartId()
	   << " tag=" << instr.tag_ << " instruction is not a vector load\n";
      return false;
    }

  if ((instr.memOps_.empty() or info.allSkipped()) and instr.size_ == 0)
    instr.size_ = elemSize;

  assert(instr.size_ == elemSize);

  // Collect reference (Whisper) elements and associate them with instruction.
  unsigned activeCount = 0;
  collectVecRefElems(hart, instr, activeCount);

  if (activeCount > 0 and instr.memOps_.empty()) //  and not hart.inDebugMode())
    {
      cerr << "Warning: hart-id=" << hart.hartId() << " time=" << time_ << " tag="
	   << instr.tag_ << " vector load instruction retires without any memory "
	   << "read operation.\n";
      return true;
    }

  // Check if all elements masked off or VL == 0.
  auto& vecRefs = hartData_.at(hart.sysHartIndex()).vecRefMap_[instr.tag_];
  if (vecRefs.refs_.empty())
    {
      instr.memOps_.clear();
      instr.complete_ = true;
      return true;
    }

  if (isUnitStride(info))
    return commitVecReadOpsUnitStride(hart, instr);

  // Special case. Test bench sends a read for up to one element for this case.
  if (info.isStrided_ and info.stride_ == 0 and info.fields_ == 0)
    return commitVecReadOpsStride0(hart, instr);

  // Map a reference address/elem-ix to a flag indicating if elem is covered by a read op.
  std::unordered_map<RefElemCoord, bool> addrMap;

  // Collect reference byte addresses in addrMap.
  for (auto& ref : vecRefs.refs_)
    {
      for (unsigned i = 0; i < ref.size_; ++i)
	{
	  uint64_t pa  = ref.pa_ + i;
          RefElemCoord coord {pa, ref.ix_};
	  addrMap[coord] = false;
	}
    }

  // Process read ops in reverse order. Trim each op to the reference addresses. Keep ops
  // (marking them as not canceled) where at least one address remains. Mark reference
  // addresses covered by read ops. Set reference (Whisper) values of reference addresses.
  auto& ops = instr.memOps_;

  bool ok = true;

  for (auto iter = ops.rbegin(); iter != ops.rend(); ++iter)
    {
      auto opIx = *iter;
      auto& op = sysMemOps_.at(opIx);
      if (not op.isRead_)
        continue;  // Should not happen.

      uint16_t elemIx = op.elemIx_;

      uint64_t low = ~uint64_t(0), high = 0; // Range of op addresses overlapping reference.
      bool mismatch = false; // True if mismatch in op
      for (unsigned i = 0; i < op.size_; ++i)
        {
          uint64_t addr = op.pa_ + i;
          auto iter = addrMap.find(RefElemCoord{addr, elemIx});
          if (iter == addrMap.end())
            continue;    // No overlap with instruction.

          bool& covered = iter->second;  // Ref elem byte covered
          if (covered)
            continue;  // Address already covered by another read op.

          covered = true;

          if (isReadDataCheckEnabled(addr))
            {
              uint8_t refVal = op.data_ >> (i*8);
              uint8_t rtlVal = op.rtlData_ >> (i*8);

              if (refVal != rtlVal)
                {
                  if (not mismatch)
                    printReadMismatch(hart, op.time_, op.tag_, addr, op.size_, rtlVal, refVal);
                  mismatch = true;
                  ok = false;
                }
            }

          low = std::min(low, addr);
          high = std::max(high, addr);
        }

      if (low <= high)
        {
          unsigned size = high - low + 1;
          trimOp(op, low, size);
          op.canceled_ = false;
        }
    }

  // Remove ops still marked canceled.
  std::erase_if(ops, [this](MemoryOpIx ix) {
    return ix >= sysMemOps_.size() or sysMemOps_.at(ix).isCanceled();
  });

  // Check that all reference addresses are covered by the read operations.
  instr.complete_ = true;
  for (const auto& [coord, covered] : addrMap)
    if (not covered)
      {
	instr.complete_ = false;
	cerr << "Error: hart-id=" << hart.hartId() << " tag=" << instr.tag_
	     << " elem-ix=" << coord.ix << " addr=0x" << std::hex << coord.addr << std::dec 
	     << " read ops do not cover all the bytes of vector load instruction\n";
	return false;
      }

  return ok;
}


template <typename URV>
bool
Mcm<URV>::commitReadOps(Hart<URV>& hart, McmInstr& instr)
{
  if (instr.di_.isVector())
    return commitVecReadOps(hart, instr);

  if (instr.memOps_.empty())
    {
      const auto& di = instr.di_;
      if (instr.size_ == 0)
	instr.size_ = di.isAmo() ? di.amoSize() : di.loadSize();

      if (not hart.inDebugMode())
	{
	  cerr << "Warning: hart-id=" << hart.hartId() << " time=" << time_ << " tag="
	       << instr.tag_ << " load/amo instruction retires witout any memory "
	       << "read operation.\n";
	  return true;
	}
    }

  // Mark replayed ops as canceled.
  assert(instr.size_ > 0 and instr.size_ <= 8);
  unsigned expectedMask = (1 << instr.size_) - 1;  // Mask of bytes covered by instruction.
  unsigned readMask = 0;    // Mask of bytes covered by read operations.

  auto& ops = instr.memOps_;
  for (auto& opIx : ops)
    trimMemoryOp(instr, sysMemOps_.at(opIx));

  // Process read ops in reverse order so that later reads take precedence.
  for (auto iter = instr.memOps_.rbegin(); iter  != instr.memOps_.rend(); ++iter)
    {
      auto opIx = *iter;
      auto& op = sysMemOps_.at(opIx);
      if (not op.isRead_)
	continue;

      if (readMask != expectedMask)
	{
	  unsigned mask = determineOpMask(instr, op);
	  mask &= expectedMask;
          if (not mask or ((mask & readMask) == mask))
            continue; // Not matched, or read op already covered by other read ops
	  readMask |= mask;
	  op.canceled_ = false;
	}
    }

  // Remove canceled ops.
  std::erase_if(ops, [this](MemoryOpIx ix) {
    return ix >= sysMemOps_.size() or sysMemOps_.at(ix).isCanceled();
  });

  // Check read operations of instruction comparing RTL values to model (whisper) values.
  bool ok = true;
  for (auto opIx : instr.memOps_)
    {
      auto& op = sysMemOps_.at(opIx);
      if (op.isRead_)
	ok = checkRtlRead(hart, instr, op) and ok;
    }
  return ok;
}


template <typename URV>
bool
Mcm<URV>::vecReadOpOverlapsElemByte(const MemoryOp& op, uint64_t addr, unsigned elemIx,
				    bool unitStride, unsigned elemSize) const
{
  if (op.size_ <= elemSize and (elemIx != op.elemIx_))
    return false;  // Op is for a single element and ix does not match.

  if (op.elemIx_ != elemIx and not unitStride)
    return false;   // For non-unit stride element index must match.

  if (op.elemIx_ > elemIx)
    return false;

  return op.overlaps(addr);
}


template <typename URV>
bool
Mcm<URV>::vecReadOpOverlapsElem(const MemoryOp& op, uint64_t pa1, uint64_t pa2,
				unsigned size, unsigned elemIx, bool unitStride,
				unsigned elemSize) const
{
  unsigned size1 = size;
  if (pa1 != pa2)
    {
      size1 = offsetToNextPage(pa1);
      assert(size1 > 0 and size1 <= 8);
    }

  for (unsigned i = 0; i < size; ++i)
    {
      uint64_t addr = i < size1 ? pa1 + i : pa2 + i - size1;
      if (vecReadOpOverlapsElemByte(op, addr, elemIx, unitStride, elemSize))
	return true;
    }

  return false;
}


template <typename URV>
bool
Mcm<URV>::getCurrentLoadValue(Hart<URV>& hart, uint64_t tag, uint64_t va, uint64_t pa1,
			      uint64_t pa2, unsigned size, bool isVector, uint64_t& value,
			      unsigned elemIx, unsigned /*field*/)
{
  value = 0;
  if (size == 0 or size > 8)
    {
      cerr << "Error: Mcm::getCurrentLoadValue: Invalid size: " << size << '\n';
      return false;
    }

  unsigned hartIx = hart.sysHartIndex();
  McmInstr* instr = findInstr(hartIx, tag);
  if (not instr or instr->isCanceled())
    return false;

  // We expect Mcm::retire to be called after this method is called.
  if (instr->isRetired())
    {
      cerr << "Error: Mcm::getCurrentLoadValue: Instruction already retired\n";
      return false;
    }

  auto& hartData = hartData_.at(hartIx);
  auto& stores = hartData.forwardingStores_;

  if (tag != hartData.currentLoadTag_)
    {
      stores.clear();
      collectForwardingStores(hart, *instr, stores);
      hartData.currentLoadTag_ = tag;
    }

  if (pa2 == pa1 and pageNum(pa1 + size - 1) != pageNum(pa1))
    pa2 = pageAddress(pageNum(pa2) + 1);

  auto& info = hart.getLastVectorMemory();
  unsigned elemSize = info.elemSize_;
  auto& elems = info.elems_;

  bool unitStride = isVector and isUnitStride(info);

  // For strided load with 0 stride, test-bench sometimes sends one read for all active
  // elements and sometimes sends multiple reads.
  if (isVector and not elems.empty())
    if (info.isStrided_ and info.stride_ == 0 and info.fields_ == 0)
      elemIx = effectiveStride0ElemIx(hart, *instr, elemIx);

  // For vector load, we don't check indices if unit stride (no possibility of overlap).
  for (auto opIx : instr->memOps_)
    {
      if (auto& op = sysMemOps_.at(opIx); op.isRead_)
        {
          if (not isVector or unitStride)
            forwardToRead(hart, stores, op);   // Let forwarding override read-op ref data.
          else
            {
              if (info.stride_ == 0 and elemIx != op.elemIx_)
                continue;
              if (vecReadOpOverlapsElem(op, pa1, pa2, size, elemIx, unitStride, elemSize))
                forwardToRead(hart, stores, op);   // Let forwarding override read-op ref data.
            }
        }
    }

  instr->size_ = size;
  instr->virtAddr_ = va;
  instr->physAddr_ = pa1;
  instr->physAddr2_ = pa2;

  value = 0;
  bool covered = true;

  unsigned size1 = size;
  if (pa1 != pa2)
    {
      size1 = offsetToNextPage(pa1);
      assert(size1 > 0 and size1 <= 8);
    }

  for (unsigned byteIx = 0; byteIx < size; ++byteIx)
    {
      uint64_t byteAddr = pa1 + byteIx;
      if (pa1 != pa2 and byteIx >= size1)
	byteAddr = pa2 + byteIx - size1;

      bool byteCovered = false;
      for (auto iter = instr->memOps_.rbegin(); iter  != instr->memOps_.rend(); ++iter)
	{
	  const auto& op = sysMemOps_.at(*iter);
          if (isVector and not unitStride)
            {
              if (info.stride_ == 0 and elemIx != op.elemIx_)
                continue;
              if (not vecReadOpOverlapsElemByte(op, byteAddr, elemIx, unitStride, elemSize))
                continue;  // Vector non-unit-stride ops must match element index.
            }

          uint8_t byte = 0;
          if (not op.getModelReadOpByte(byteAddr, byte))
            continue;

          value |= uint64_t(byte) << (8*byteIx);
          byteCovered = true;
          break;
	}
      covered = covered and byteCovered;
    }

  // Vector cover check done in commitVecReadOps.
  if (not covered and not isVector)
    cerr << "Error: hart-id=" << hart.hartId() << " tag=" << tag << " read ops do not"
	 << " cover all the bytes of load instruction\n";

  // Vector completion check done in commitVecReadOps.
  if (not isVector)
    instr->complete_ = covered;

  return covered;
}


template <typename URV>
unsigned
Mcm<URV>::effectiveStride0ElemIx(const Hart<URV>& hart, const McmInstr& instr, unsigned elemIx) const
{
  const auto& info = hart.getLastVectorMemory();
  const auto& elems = info.elems_;
  unsigned vstart = elems.front().ix_;

  // Find highest read op with highest elem ix that is <= the current index.
  unsigned activeCount = 0;
  unsigned activeIx = 0;
  unsigned high = 0;
  for (auto opIx : instr.memOps_)
    {
      if (auto& op = sysMemOps_.at(opIx); op.isRead_)
        {
          unsigned opElemIx = op.elemIx_;
          if (opElemIx < vstart)
            continue;

          unsigned offset = opElemIx - vstart;
          if (offset >= elems.size() or elems.at(offset).skip_)
            continue;

          activeCount++;
          activeIx = opElemIx;

          if (opElemIx <= elemIx and opElemIx > high)
            high = opElemIx;
        }
    }

  if (activeCount == 1)
    return activeIx;     // Test bench sent 1 read for all elements. Use it.

  return high;
}


template <typename URV>
bool
Mcm<URV>::vecStoreToReadForward(const McmInstr& store, MemoryOp& readOp, uint64_t& mask) const
{
  const auto& vecRefMap = hartData_.at(store.hartIx_).vecRefMap_;
  auto iter = vecRefMap.find(store.tag_);
  if (iter == vecRefMap.end())
    return false;

  auto& vecRefs = iter->second;
  if (vecRefs.isOutOfBounds(readOp))
    return false;

  unsigned count = 0;  // Count of forwarded bytes.
  uint64_t forwardMask = 0;  // Mask of bits forwarded by vector store instruction

  for (unsigned rix = 0; rix < readOp.size_; ++rix)
    {
      uint64_t byteAddr = readOp.pa_ + rix;

      uint64_t byteMask = uint64_t(0xff) << (rix * 8);
      if ((byteMask & mask) == 0)
	continue;  // Byte forwarded by another instruction.

      // Count write ops overlapping byte addr. Identify last overlapping write op.
      unsigned writeCount = 0;
      unsigned lastWopIx = 0;
      for (const auto wopIx : store.memOps_)
	{
	  const auto& wop = sysMemOps_.at(wopIx);
	  if (wop.isRead_ or not wop.overlaps(byteAddr))
	    continue;  // Not a write op (may happen for AMO), or does not overlap byte addr.
	  writeCount++;
	  lastWopIx = wopIx;
	}

      // Count reference elements overlapping byte
      unsigned refCount = 0;
      for (auto& vecRef : vecRefs.refs_)
	if (vecRef.overlaps(byteAddr))
	  refCount++;

      if (refCount == 0)
        continue;

      // We cannot forward if last overlapping write drains before read.
      bool drained = false;
      uint64_t lastWopTime = time_;  // In case no write ops.
      if (refCount == writeCount and writeCount != 0)
	{
	  const auto& lastWop = sysMemOps_.at(lastWopIx);
	  assert(not lastWop.isRead_);
	  lastWopTime = lastWop.time_;
	  drained = lastWopTime < readOp.time_;
	}

      if (drained)
	continue;   // Cannot forward from a drained write.

      if (lastWopTime >= readOp.time_)
        {
          uint64_t offset = lastWopTime - readOp.time_;
          uint16_t off16 = static_cast<uint16_t>(offset);  // TODO: Use gsl
          assert(off16 == offset);  // Check for overflow
          readOp.fwOffset_.at(rix) = std::max(readOp.fwOffset_.at(rix), off16);
        }

      // Process reference model writes in reverse order so that later ones forward first.
      for (auto iter = vecRefs.refs_.rbegin(); iter != vecRefs.refs_.rend(); ++iter)
	{
	  auto& vecRef = *iter;
	  if (not vecRef.overlaps(byteAddr))
	    continue;

	  uint8_t byteVal = vecRef.data_ >> ((byteAddr - vecRef.pa_)*8);
	  uint64_t aligned = uint64_t(byteVal) << 8*rix;

	  readOp.data_ = (readOp.data_ & ~byteMask) | aligned;
	  count++;

	  forwardMask = forwardMask | byteMask;
	  break;
	}
    }

  mask = mask & ~forwardMask;

  return count > 0;
}



template <typename URV>
bool
Mcm<URV>::storeToReadForward(const McmInstr& store, MemoryOp& readOp, uint64_t& mask,
			     uint64_t addr, uint64_t data, unsigned size) const
{
  if (mask == 0)
    return true;  // No bytes left to forward.

  if (store.isCanceled() or not store.isRetired() or not store.isStore_)
    return false;

  uint64_t rol = readOp.pa_, roh = readOp.pa_ + readOp.size_ - 1;
  uint64_t il = addr, ih = il + size - 1;
  if (roh < il or rol > ih)
    return false;  // no overlap

  unsigned count = 0; // Count of forwarded bytes
  for (unsigned rix = 0; rix < readOp.size_; ++rix)
    {
      uint64_t byteAddr = rol + rix;
      if (byteAddr < il or byteAddr > ih)
	continue;  // Read-op byte does not overlap instruction.

      uint64_t byteMask = uint64_t(0xff) << (rix * 8);
      if ((byteMask & mask) == 0)
	continue;  // Byte forwarded by another instruction.

      // Check if read-op byte overlaps drained write-op of instruction
      bool drained = false;
      uint64_t fwdTime = 0;
      for (const auto wopIx : store.memOps_)
	{
	  if (wopIx >= sysMemOps_.size())
	    continue;
	  const auto& wop = sysMemOps_.at(wopIx);
	  if (wop.isRead_ or not wop.overlaps(byteAddr))
	    continue;  // Not a write op (may happen for AMO), or does not overlap byte addr.
	  if (wop.time_ < readOp.time_)
	    {
	      drained = true; // Write op cannot forward.
	      break;
	    }

	  fwdTime = std::max(fwdTime, wop.time_);
	}

      if (drained)
	continue;  // Cannot forward from a drained write.

      if (fwdTime == 0)
	fwdTime = time_;  // Happens if store.memOps_ empty.

      uint64_t offset = fwdTime > readOp.time_ ? fwdTime - readOp.time_ : 0;
      uint16_t off16 = static_cast<uint16_t>(offset);  // TOD: Use gsl
      readOp.fwOffset_.at(rix) = std::max(readOp.fwOffset_.at(rix), off16);

      uint8_t byteVal = data >> (byteAddr - il)*8;
      uint64_t aligned = uint64_t(byteVal) << 8*rix;
	
      readOp.data_ = (readOp.data_ & ~byteMask) | aligned;
      count++;

      mask = mask & ~byteMask;
      if (mask == 0)
	break;
    }

  return count > 0;
}


template <typename URV>
void
Mcm<URV>::collectForwardingStores(Hart<URV>& hart, const McmInstr& instr,
				  std::set<McmInstrIx>& stores) const
{
  auto hartIx = hart.sysHartIndex();

  const auto& instrVec = hartData_.at(hartIx).instrVec_;
  const auto& undrained = hartData_.at(hartIx).undrainedStores_;

  // Collect undrained overlapping stores preceding instr in program order.
  for (auto opIx : instr.memOps_)
    {
      auto& rop = sysMemOps_.at(opIx);  // Read op.
      if (not rop.isRead_)
	continue;

      for (auto iter = undrained.rbegin(); iter != undrained.rend(); ++iter)
	{
	  auto storeTag = *iter;
	  const auto& store = instrVec.at(storeTag);
	  if (store.isCanceled() or store.tag_ >= instr.tag_)
	    continue;
	  if (overlaps(store, rop))
	    stores.insert(store.tag_);
	}
    }

  // Collect overlapping stores preceding instr in program order and with write times
  // after those of instr reads (write times before instr reads imply a drained write that
  // can no longer forward to instr).
  for (auto opIx : instr.memOps_)
    {
      auto& rop = sysMemOps_.at(opIx);  // Read op.
      if (not rop.isRead_)
	continue;

      for (auto iter = sysMemOps_.rbegin(); iter != sysMemOps_.rend(); ++iter)
	{
	  const auto& wop = *iter;
	  if (wop.time_ < rop.time_)
	    break;

	  if (wop.isCanceled()  or  wop.isRead_  or  wop.hartIx_ != rop.hartIx_  or
	      wop.tag_ >= instr.tag_)
	    continue;

	  if (rop.overlaps(wop))
	    stores.insert(wop.tag_);
	}
    }
}


template <typename URV>
bool
Mcm<URV>::forwardToRead(Hart<URV>& hart, const std::set<McmInstrIx>& stores, MemoryOp& readOp) const
{
  auto hartIx = hart.sysHartIndex();

  const auto& instrVec = hartData_.at(hartIx).instrVec_;

  uint64_t mask = (~uint64_t(0)) >> (8 - readOp.size_)*8;

  for (auto iter = stores.rbegin(); iter != stores.rend() and mask != 0; ++iter)
    {
      auto storeTag = *iter;
      const auto& store = instrVec.at(storeTag);

      if (store.di_.isVector())
	{
	  if (not vecStoreToReadForward(store, readOp, mask))
	    continue;
	}
      else if (not storeToReadForward(store, readOp, mask, store.physAddr_, store.storeData_,
				      store.size_))
	{
	  if (store.physAddr_ == store.physAddr2_)
	    continue;
	  unsigned size1 = offsetToNextPage(store.physAddr_);
	  unsigned size2 = store.size_ - size1;
	  assert(size2 > 0 and size2 < 8);
	  uint64_t data2 = store.storeData_ >> size1 * 8;
	  if (not storeToReadForward(store, readOp, mask, store.physAddr2_, data2, size2))
	    continue;
	}
    }

  return true;
}


template <typename URV>
unsigned
Mcm<URV>::effectiveRegIx(const DecodedInst& di, unsigned opIx) const
{
  auto type = di.ithOperandType(opIx);
  switch (type)
    {
    case OperandType::IntReg:
      return di.ithOperand(opIx) + intRegOffset_;

    case OperandType::FpReg:
      return di.ithOperand(opIx) + fpRegOffset_;

    case OperandType::VecReg:
      return di.ithOperand(opIx) + vecRegOffset_;

    case OperandType::CsReg:
      {
	CsrNumber csr{di.ithOperand(opIx)};
	return unsigned(csr) + csRegOffset_;
      }

    case OperandType::Imm:
    case OperandType::None:
      assert(0 && "Error: Assertion failed");
      return 0;
    }
  return 0;
}


template <typename URV>
void
Mcm<URV>::identifyRegisters(const Hart<URV>& hart,
                            const DecodedInst& di,
			    std::vector<unsigned>& sourceRegs,
			    std::vector<unsigned>& destRegs)
{
  sourceRegs.clear();
  destRegs.clear();

  if (not di.isValid())
    return;

  if (di.hasRoundingMode() and RoundingMode(di.roundingMode()) == RoundingMode::Dynamic)
    sourceRegs.push_back(unsigned(CsrNumber::FRM) + csRegOffset_);

  if (di.modifiesFflags())
    destRegs.push_back(unsigned(CsrNumber::FFLAGS) + csRegOffset_);

  for (unsigned i = 0; i < di.operandCount(); ++i)
    {
      OperandMode opMode = di.effectiveIthOperandMode(i);
      bool isDest = opMode == OperandMode::Write or opMode == OperandMode::ReadWrite;
      bool isSource = opMode == OperandMode::Read or opMode == OperandMode::ReadWrite;
      if (not isDest and not isSource)
	continue;

      auto type = di.ithOperandType(i);

      if (type == OperandType::Imm or type == OperandType::None)
	continue;

      size_t regIx = effectiveRegIx(di, i);
      if (regIx == size_t(CsrNumber::FCSR) + csRegOffset_)
	{
	  if (isDest)
	    {
	      destRegs.push_back(size_t(CsrNumber::FFLAGS) + csRegOffset_);
	      destRegs.push_back(size_t(CsrNumber::FRM) + csRegOffset_);
	    }
	  if (isSource)
	    {
	      sourceRegs.push_back(size_t(CsrNumber::FFLAGS) + csRegOffset_);
	      sourceRegs.push_back(size_t(CsrNumber::FRM) + csRegOffset_);
	    }
	}
      else if (type == OperandType::VecReg)
        {
          unsigned touchedRegs = hart.vecOpEmul(i);
	  if (di.vecFieldCount() and (i == 0))
	    touchedRegs *= di.vecFieldCount();

          for (unsigned off = 0; off < touchedRegs; ++off)
            {
              if (isDest)
                destRegs.push_back(regIx + off);
              if (isSource)
                sourceRegs.push_back(regIx + off);
            }
        }
      else
        {
          if (isDest)
	    destRegs.push_back(regIx);
	  if (isSource)
	    sourceRegs.push_back(regIx);
        }
    }
}


template <typename URV>
bool
Mcm<URV>::overlaps(const McmInstr& i1, const McmInstr& i2) const
{
  if (not i1.di_.isVector() and not i2.di_.isVector())
    return i1.overlaps(i2);   // Both scalar.

  if (i1.di_.isVector() and i2.di_.isVector())    // Both vector.
    {
      const auto& vecRefMap1 = hartData_.at(i1.hartIx_).vecRefMap_;
      const auto& vecRefMap2 = hartData_.at(i2.hartIx_).vecRefMap_;
      auto iter1 = vecRefMap1.find(i1.tag_);
      auto iter2 = vecRefMap2.find(i2.tag_);
      if (iter1 == vecRefMap1.end() or iter2 == vecRefMap2.end())
	assert(false);

      auto& vecRefs1 = iter1->second;
      auto& vecRefs2 = iter2->second;

      if (vecRefs1.empty() or vecRefs2.empty())
	return false;

      for (auto& ref1 : vecRefs1.refs_)
	{
	  if (not vecRefs2.isOutOfBounds(ref1))
	    for (auto& ref2 : vecRefs2.refs_)
	      if (rangesOverlap(ref1.pa_, ref1.size_, ref2.pa_, ref2.size_))
		return true;
	}

      return false;
    }

  // One is scalar.
  const McmInstr& scalar = i1.di_.isVector() ? i2 : i1;
  const McmInstr& vec = i1.di_.isVector() ? i1 : i2;

  unsigned size1 = scalar.size_, size2 = 0;
  uint64_t pa1 = scalar.physAddr_, pa2 = scalar.physAddr2_;

  if (pa1 != pa2 and pageNum(pa1) != pageNum(pa2))
    {
      size1 = offsetToNextPage(pa1);
      size2 = scalar.size_ - size1;
      assert(size1 > 0 and size1 < scalar.size_);
      assert(size2 > 0 and size2 < scalar.size_);
    }

  for (unsigned i = 0; i < size1; ++i)
    {
      uint64_t pa = pa1 + i;
      if (vecOverlapsRefPhysAddr(vec, pa))
	return true;
    }

  for (unsigned i = 0; i < size2; ++i)
    {
      uint64_t pa = pa2 + i;
      if (vecOverlapsRefPhysAddr(vec, pa))
	return true;
    }

  return false;
}



template <typename URV>
bool
Mcm<URV>::instrHasRead(const McmInstr& instr) const
{
  return std::ranges::any_of(instr.memOps_,
                             [this](auto opIx) { return opIx < sysMemOps_.size() &&
                                                        sysMemOps_.at(opIx).isRead_; });
}


template <typename URV>
bool
Mcm<URV>::instrHasWrite(const McmInstr& instr) const
{
  return std::ranges::any_of(instr.memOps_,
                             [this](auto opIx) { return opIx < sysMemOps_.size() &&
                                                        not sysMemOps_.at(opIx).isRead_; });
}


template <typename URV>
uint64_t
Mcm<URV>::earliestByteTime(const McmInstr& instr, uint64_t addr) const
{
  uint64_t time = 0;
  bool found = false;

  for (auto opIx : instr.memOps_)
    if (opIx < sysMemOps_.size())
      {
	const auto& op = sysMemOps_.at(opIx);
	if (op.pa_ <= addr and addr < op.pa_ + op.size_)
	  {
	    time = found? std::min(time, op.time_) : op.time_;
	    found = true;
	  }
      }

  return time;
}


template <typename URV>
uint64_t
Mcm<URV>::earliestByteTime(const McmInstr& instr, uint64_t addr, unsigned elemIx) const
{
  uint64_t time = 0;
  bool found = false;

  for (auto opIx : instr.memOps_)
    if (opIx < sysMemOps_.size())
      {
	const auto& op = sysMemOps_.at(opIx);
        if (op.pa_ <= addr and (addr < op.pa_ + op.size_) and op.elemIx_ == elemIx)
	  {
	    time = found? std::min(time, op.time_) : op.time_;
	    found = true;
	  }
      }

  return time;
}


template <typename URV>
uint64_t
Mcm<URV>::latestByteTime(const McmInstr& instr, uint64_t addr) const
{
  uint64_t time = ~uint64_t(0);
  bool found = false;

  for (auto opIx : instr.memOps_)
    if (opIx < sysMemOps_.size())
      {
	const auto& op = sysMemOps_.at(opIx);
	if (op.pa_ <= addr and addr < op.pa_ + op.size_)
	  {
	    time = found? std::max(time, op.time_) : op.time_;
	    found = true;
	  }
      }

  return time;
}


template <typename URV>
uint64_t
Mcm<URV>::latestByteTime(const McmInstr& instr, uint64_t addr, unsigned elemIx) const
{
  uint64_t time = ~uint64_t(0);
  bool found = false;

  for (auto opIx : instr.memOps_)
    if (opIx < sysMemOps_.size())
      {
	const auto& op = sysMemOps_.at(opIx);
	if (op.pa_ <= addr and addr < op.pa_ + op.size_ and op.elemIx_ == elemIx)
	  {
	    time = found? std::max(time, op.time_) : op.time_;
	    found = true;
	  }
      }

  return time;
}


template <typename URV>
bool
Mcm<URV>::vecOverlapsRefPhysAddr(const McmInstr& instr, uint64_t addr) const
{
  assert(instr.di_.isVector());

  auto& vecRefMap = hartData_.at(instr.hartIx_).vecRefMap_;
  auto iter = vecRefMap.find(instr.tag_);
  if (iter == vecRefMap.end())
    return false;

  auto& vecRefs = iter->second;

  if (vecRefs.isOutOfBounds(addr))
    return false;

  for (auto& vecRef : vecRefs.refs_)
    if (vecRef.overlaps(addr))
      return true;

  return false;
}


template <typename URV>
void
Mcm<URV>::printPpo1Error(unsigned hartId, McmInstrIx tag1, McmInstrIx tag2, uint64_t t1,
			 uint64_t t2, uint64_t pa) const
{
  cerr << "Error: PPO rule 1 failed: hart-id=" << hartId << " tag1=" << tag1
       << " tag2=" << tag2 << " time1=";

  if (t1 == ~uint64_t(0))
    cerr << "inf";
  else
    cerr << t1;

  cerr << " time2=" << t2 << std::hex << " pa=0x" << pa << std::dec << '\n';
}


template <typename URV>
bool
Mcm<URV>::ppoRule1(unsigned hartId, const McmInstr& instrA, const McmInstr& instrB) const
{
  if (instrA.isCanceled())
    return true;

  assert(instrA.isRetired());

  if (not instrA.isMemory() or not overlaps(instrA, instrB))
    return true;

  // Check overlapped bytes. We check at the byte level since A may be a vector
  // instruction with overlapping elements. Same for B.
  for (unsigned i = 0; i < instrB.memOps_.size(); ++i)
    {
      auto opIx = instrB.memOps_.at(i);
      auto& bop = sysMemOps_.at(opIx);

      uint64_t tb = bop.time_;

      for (unsigned byteIx = 0; byteIx < bop.size_; ++byteIx)
	{
	  uint64_t addr = bop.pa_ + byteIx;
	  if (not overlapsRefPhysAddr(instrA, addr))
	    continue;

	  uint64_t ta = latestByteTime(instrA, addr);
	  if (ta < tb or (ta == tb and instrA.isStore_))
	    continue;

	  printPpo1Error(hartId, instrA.tag_, instrB.tag_, ta, tb, addr);
	  return false;
	}
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::ppoRule1(unsigned hartId, const McmInstr& instrA, const MemoryOp& opA,
		   const McmInstr& instrB) const
{
  if (instrA.isCanceled() or not instrA.isMemory())
    return true;

  assert(instrA.isRetired());

  // Check memory ops of B overlapping opA.
  for (unsigned i = 0; i < instrB.memOps_.size(); ++i)
    {
      auto opIx = instrB.memOps_.at(i);
      auto& opB = sysMemOps_.at(opIx);

      if (not opA.overlaps(opB))
	continue;

      uint64_t ta = opA.time_;
      uint64_t tb = opB.time_;
      if (ta < tb or (ta == tb and not opB.isRead_))
	continue;

      printPpo1Error(hartId, instrA.tag_, instrB.tag_, ta, tb, opB.pa_);
      return false;
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::ppoRule1(Hart<URV>& hart, const McmInstr& instrB) const
{
  // Rule 1: B is a store, A and B have overlapping addresses.
  assert(instrB.di_.isValid());

  if (not instrB.complete_)
    return true;  // We will try again when B is complete.

  auto hartIx = hart.sysHartIndex();
  const auto& instrVec = hartData_.at(hartIx).instrVec_;

  auto earlyB = earliestOpTime(instrB);

  auto hartId = hart.hartId();

  // Process all the memory operations that may have been reordered with respect to B. If
  // an instruction A, preceding B in program order, is missing a memory operation, we
  // will catch it when we process the un-drained stores below.
  for (auto iter = sysMemOps_.rbegin(); iter != sysMemOps_.rend(); ++iter)
    {
      const auto& op = *iter;
      if (op.isCanceled()  or  op.hartIx_ != hartIx  or  op.tag_ >= instrB.tag_)
	continue;

      if (op.time_ < earlyB)
	break;

      const auto& instrA =  instrVec.at(op.tag_);
      if (instrA.isCanceled()  or  not instrA.isRetired()  or  not instrA.isMemory())
	continue;

      if (not ppoRule1(hartId, instrA, op, instrB))
	return false;
    }

  const auto& undrained = hartData_.at(hartIx).undrainedStores_;

  for (auto& tag : undrained)
    {
      if (tag >= instrB.tag_)
	break;

      const auto& instrA =  instrVec.at(tag);
      if (not ppoRule1(hartId, instrA, instrB))
	return false;
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::ppoRule2(Hart<URV>& hart, const McmInstr& instrB) const
{
  // Rule 2: a and b are loads, x is a byte read by both a and b, there is no store to x
  // between a and b in program order, and a and b return values for x written by
  // different memory operations.

  // Instruction B must be a load/AMO instruction.
  if (not instrB.isLoad_)
    return true;  // NA: B is not a load.

  auto earlyB = effectiveMinTime(hart, instrB);

  unsigned hartIx = hart.sysHartIndex();
  const auto& instrVec = hartData_.at(hartIx).instrVec_;

  for (auto iter = sysMemOps_.rbegin(); iter != sysMemOps_.rend(); ++iter)
    {
      const auto& op = *iter;
      if (op.isCanceled() or op.hartIx_ != hartIx or op.tag_ >= instrB.tag_
          or not op.isRead_)
	continue;

      if (op.time_ < earlyB)
	break;

      const auto& prev =  instrVec.at(op.tag_);   // Instruction preceding B in prog order.
      if (prev.isCanceled()  or  not prev.isRetired()  or  not overlaps(prev, instrB))
	continue;

      auto& instrA = prev;

      if (effectiveMinTime(hart, instrB) >= effectiveMaxTime(instrA))
	continue;  // In order.

      if (instrA.memOps_.empty() or instrB.memOps_.empty())
	{
	  cerr << "Error: PPO Rule 2: Instruction with no memory op: hart-id="
	       << hart.hartId() << " tag1=" << instrA.tag_ << " tag2=" << instrB.tag_ << '\n';
	  return false;
	}
      uint64_t ix0 = instrB.memOps_.front();
      uint64_t ix1 = instrA.memOps_.back();

      for (uint64_t ix = ix0; ix <= ix1; ++ix)
	{
	  const MemoryOp& remoteOp = sysMemOps_.at(ix);
	  if (remoteOp.isCanceled() or remoteOp.hartIx_ == hartIx or remoteOp.isRead_)
	    continue;

	  // Check the bytes of the remote write. If the address of any of them overlaps A
	  // and B and corresponding time is between times of A and B, we fail.
	  for (unsigned byteIx = 0; byteIx < remoteOp.size_; ++byteIx)
	    {
	      uint64_t addr = remoteOp.pa_ + byteIx;

	      if (not overlapsRefPhysAddr(instrA, addr) or not overlapsRefPhysAddr(instrB, addr))
		continue;

	      auto earlyB = effectiveMinByteTime(instrB, addr);
	      auto lateA = effectiveMaxByteTime(instrA, addr);

	      auto rot = remoteOp.time_;
	      if (earlyB <= lateA and earlyB <= rot and rot <= lateA)
		{
		  cerr << "Error: PPO Rule 2 failed: hart-id=" << hart.hartId()
		       << " tag1=" << instrA.tag_ << " tag2=" << instrB.tag_
		       << " store-tag=" << remoteOp.tag_ << " store-hart="
		       << unsigned(remoteOp.hartIx_) << " time1=" << lateA
		       << " time2=" << earlyB << " store-time=" << remoteOp.time_
		       << " addr=0x" << std::hex << addr << std::dec << '\n';
		  return false;
		}
	    }
	}
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::ppoRule3(Hart<URV>& hart, const McmInstr& instrB) const
{
  // Rule 3: A is a write resulting from an AMO/SC instructions, A and
  // B have overlapping addresses, B loads data from A.

  // Instruction B must be a load/AMO instruction.
  const DecodedInst& bdi = instrB.di_;
  if (bdi.isStore())
    return true;  // NA: store instruction.

  if (not bdi.isLoad() and not bdi.isVectorLoad() and not bdi.isAtomic())
    return true;  // NA: must be load/AMO.

  if (not instrB.complete_)
    return true;  // We will try again when B is complete.

  auto earlyB = earliestOpTime(instrB);

  // Addresses of bytes of B written by preceding non-atomic stores in local hart.
  std::unordered_set<uint64_t> locallyWritten;

  unsigned hartIx = hart.sysHartIndex();
  const auto& instrVec = hartData_.at(hartIx).instrVec_;

  for (auto iter = sysMemOps_.rbegin(); iter != sysMemOps_.rend(); ++iter)
    {
      const auto& op = *iter;
      if (op.isCanceled() or op.hartIx_ != hartIx or op.tag_ >= instrB.tag_)
	continue;

      if (op.time_ < earlyB)
	break;

      const auto& instrA =  instrVec.at(op.tag_);
      if (instrA.isCanceled() or not instrA.isRetired() or not instrA.isStore_ or
          not overlaps(instrA, instrB))
	continue;

      // Check if a byte of B is written by A.
      for (auto opIx : instrB.memOps_)
	{
	  const auto& op = sysMemOps_.at(opIx);
	  for (unsigned i = 0; i < op.size_; ++i)
	    {
	      uint64_t addr = op.pa_ + i;
	      if (locallyWritten.contains(addr))
		continue;
              if (not overlaps(instrA, op))
                continue;
	      if (not instrA.di_.isAtomic())
                {
                  locallyWritten.insert(addr);
                  continue;
                }
              
              uint64_t aTime = instrA.complete_ ? latestOpTime(instrA) : ~uint64_t(0);
              if (op.time_ < aTime)
		{
		  cerr << "Error: PPO rule 3 failed: hart-id=" << hart.hartId() << " tag1="
		       << instrA.tag_ << " tag2=" << instrB.tag_ << " time1="
		       << aTime << " time2=" << op.time_
		       << '\n';
		  return false;
		}
	    }
	}
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::finalChecks(Hart<URV>& hart)
{
  unsigned hartIx = hart.sysHartIndex();
  const auto& instrVec = hartData_.at(hartIx).instrVec_;

  const auto& pendingWrites = hartData_.at(hartIx).pendingWrites_;
  if (not pendingWrites.empty())
    cerr << "Warning: Merge buffer is not empty at end of run\n";

  uint64_t toHost = 0;
  bool hasToHost = hart.getToHostAddress(toHost);

  const auto& undrained = hartData_.at(hartIx).undrainedStores_;

  for (auto tag : undrained)
    {
      const auto& instr = instrVec.at(tag);
      if (not hasToHost or toHost != instr.virtAddr_)
	cerr << "Warning: Hart-id=" << hart.hartId() << " tag=" << instr.tag_
	     << " Store instruction is not drained at end of run\n";
    }

  return true;
}


template <typename URV>
uint64_t
Mcm<URV>::effectiveMinTime(Hart<URV>& hart, const McmInstr& instr) const
{
  if (not instr.isLoad_)
    return earliestOpTime(instr);

  // This is valid only if instr is the instruction being retired.
  
  bool isVec = instr.di_.isVector();
  unsigned vl = 0;
  if (isVec)
    {
      const VecLdStInfo& info = hart.getLastVectorMemory();
      vl = info.elemCount_;
    }

  uint64_t inf = ~uint64_t(0);
  uint64_t mint = inf;

  for (auto opIx : instr.memOps_)
    if (opIx < sysMemOps_.size())
      {
	auto& op = sysMemOps_.at(opIx);
	if (isVec and op.elemIx_ >= vl)
	  continue;

        uint64_t opMin = op.time_;
        if (op.isRead_)
          {
            // Take forward time into consideration.
            opMin = op.time_ + op.fwOffset_.at(0);
            for (unsigned i = 1; i < op.size_; ++i)
              opMin = std::min(opMin, (op.time_ + op.fwOffset_.at(i)));
          }

	mint = std::min(mint, opMin);
      }

  if (mint == inf)
    return time_;  // No valid read op.

  return mint;
}


template <typename URV>
uint64_t
Mcm<URV>::effectiveMaxTime(const McmInstr& instr) const
{
  if (not instr.isLoad_)
    return latestOpTime(instr);

  if (not instr.complete_ and instr.memOps_.empty())
    return time_;

  uint64_t maxt = 0;
  for (auto opIx : instr.memOps_)
    if (opIx < sysMemOps_.size())
      {
	auto& op = sysMemOps_.at(opIx);

	uint64_t opMax = op.time_;
        if (op.isRead_)
          {
            // Take forward time into consideration.
            for (unsigned i = 1; i < op.size_; ++i)
              opMax = std::max(opMax, (op.time_ + op.fwOffset_.at(i)));
          }
	maxt = std::max(maxt, opMax);
      }

  return maxt;
}


template <typename URV>
uint64_t
Mcm<URV>::effectiveMinByteTime(const McmInstr& instr, uint64_t addr) const
{
  if (not instr.isLoad_)
    return earliestByteTime(instr, addr);

  if (not instr.complete_ and instr.memOps_.empty())
    return time_;

  uint64_t mint = ~uint64_t(0);
  for (auto opIx : instr.memOps_)
    if (opIx < sysMemOps_.size())
      {
	auto& op = sysMemOps_.at(opIx);
	if (not op.overlaps(addr))
	  continue;

        if (not op.isRead_)
          mint = std::min(mint, op.time_);
        else
          {
            unsigned ix = addr - op.pa_;
            assert(ix < op.size_);
            uint64_t ft = op.time_ + op.fwOffset_.at(ix);
            mint = std::min(mint, ft);
          }
      }

  return mint;
}


template <typename URV>
uint64_t
Mcm<URV>::effectiveMaxByteTime(const McmInstr& instr, uint64_t addr) const
{
  if (not instr.isLoad_)
    return latestByteTime(instr, addr);

  if (not instr.complete_ and instr.memOps_.empty())
    return time_;

  uint64_t maxt = 0;
  for (auto opIx : instr.memOps_)
    if (opIx < sysMemOps_.size())
      {
	auto& op = sysMemOps_.at(opIx);
	if (not op.overlaps(addr))
	  continue;

        if (not op.isRead_)
          maxt = std::max(maxt, op.time_);
        else
          {
            unsigned ix = addr - op.pa_;
            assert(ix < op.size_);
            uint64_t ft = op.time_ + op.fwOffset_.at(ix);
            maxt = std::max(maxt, ft);
          }
      }

  return maxt;
}


template <typename URV>
bool
Mcm<URV>::checkFence(Hart<URV>& hart, const McmInstr& fence) const
{
  assert(fence.isRetired());

  const DecodedInst& bdi = fence.di_;

  // If fence instruction has predecessor write, then check that all preceding stores
  // have drained. This is stronger than what is required by PPO rule 4 but it makes
  // that rule simpler to implement.
  if (not bdi.isFencePredWrite())
    return true;

  if (not bdi.isFenceSuccRead() and not bdi.isFenceSuccWrite() and
      not bdi.isFenceSuccInput() and not bdi.isFenceSuccOutput())
    return true;   // No successor.

  unsigned hartIx = hart.sysHartIndex();
  auto& undrained = hartData_.at(hartIx).undrainedStores_;
  if (undrained.empty())
    return true;

  const auto& instrVec = hartData_.at(hartIx).instrVec_;

  for (auto tag : undrained)
    {
      const auto& instr = instrVec.at(tag);
      if (instr.tag_ > fence.tag_)
	continue;

      for (auto opIx : instr.memOps_)
	{
	  auto& op = sysMemOps_.at(opIx);

          // We may have an early bypass ops (e.g. for an amoadd that has not yet
          // retired). These should not count as they are drained but waiting for their
          // instruction to retire.
	  if (op.canceled_ or op.isRead_ or op.bypass_)
	    continue;

          cerr << "Error: PPO rule 4 failed: Hart-id=" << hart.hartId() << " fence-tag="
               << fence.tag_ << " fence with predecessor-write retired while write is "
               << "pending for tag=" << tag << " at time=" << op.insertTime_ << '\n';

	  return false;
	}
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::ppoRule4(Hart<URV>& hart, const McmInstr& instrB) const
{
  // Rule 4: There is a fence that orders A before B.

  assert(instrB.isRetired());

  // We assume that stores preceding a fence are drained before fence retires if fence
  // has predecessor write. This assumption is checked in checkFence.
  if (not checkFence(hart, instrB))
    return false;

  if (not instrB.isMemory())
    return true;

  auto earlyB = earliestOpTime(instrB);
  if (earlyB > instrB.retireTime_)
    return true;

  unsigned hartIx = hart.sysHartIndex();
  const auto& instrVec = hartData_.at(hartIx).instrVec_;

  // --- FIOM modification start ---
  // Determine if this fence should order I/O operations.
  bool fenceOrdersIo = false;
  // Use the privilege mode before the fence (stored in hart.lastPrivMode()).
  PrivilegeMode fencePriv = instrB.priv_;
  if (fencePriv != PrivilegeMode::Machine) {
    uint64_t menvcfg = hart.peekCsr(CsrNumber::MENVCFG, true);
    if ((menvcfg & 0x1) != 0)
      fenceOrdersIo = true;
    if (instrB.virt_)
      {
        uint64_t henvcfg = hart.peekCsr(CsrNumber::HENVCFG, true);
        if ((henvcfg & 0x1) != 0)
          fenceOrdersIo = true;
      }
    else if (fencePriv == PrivilegeMode::User) {
      uint64_t senvcfg = hart.peekCsr(CsrNumber::SENVCFG, true);
      if ((senvcfg & 0x1) != 0)
        fenceOrdersIo = true;
    }
  }
  // --- FIOM modification end ---


  // Collect all fence instructions that can affect B.
  std::vector<McmInstrIx> fences;
  for (McmInstrIx ix = instrB.tag_; ix > 0; --ix)
    {
      McmInstrIx tag = ix - 1;
      const auto& instr = instrVec.at(tag);
      if (instr.isCanceled())
	continue;
      if (instr.retireTime_ < earlyB)
	break;
      if (instr.di_.isFence())
	fences.push_back(tag);
    }
  if (fences.empty())
    return true;

  // Collect all memory ops out of order with respect to B.
  std::vector<const MemoryOp*> reordered;
  for (auto iter = sysMemOps_.rbegin(); iter != sysMemOps_.rend(); ++iter)
    {
      const auto& op = *iter;
      if (op.canceled_ or op.hartIx_ != hartIx or op.tag_ >= instrB.tag_)
	continue;
      if (op.time_ < earlyB)
	break;
      reordered.push_back(&op);
    }
  if (reordered.empty())
    return true;

  auto& succ = instrB;
  auto succTime = effectiveMinTime(hart, instrB);
  Pma succPma = hart.getPma(succ.physAddr_);

  for (auto fenceTag : fences)
    {
      const auto& fence = instrVec.at(fenceTag);
      bool predRead = fence.di_.isFencePredRead();
      bool predWrite = fence.di_.isFencePredWrite();
      bool succRead = fence.di_.isFenceSuccRead();
      bool succWrite = fence.di_.isFenceSuccWrite();
      bool predIn    = fence.di_.isFencePredInput();
      bool predOut   = fence.di_.isFencePredOutput();
      bool succIn    = fence.di_.isFenceSuccInput();
      bool succOut   = fence.di_.isFenceSuccOutput();

      // If FIOM is set, merge the I/O ordering bits into the normal ordering bits.
      if (fenceOrdersIo)
        {
          predRead  = predRead  || predIn;
          predWrite = predWrite || predOut;
          succRead  = succRead  || succIn;
          succWrite = succWrite || succOut;
        }

      for (auto aOpPtr : reordered)
	{
	  const auto& aOp = *aOpPtr;
	  const auto& pred = instrVec.at(aOp.tag_);
	  if (pred.isCanceled() or not pred.isMemory() or pred.tag_ > fence.tag_)
	    continue;

	  auto predTime = aOp.maxForwardTime();
	  if (predTime < succTime)
	    continue;

	  Pma predPma = hart.getPma(aOp.pa_);

	  if (not (predRead and aOp.isRead_)
	      and not (predWrite and not aOp.isRead_)
	      and not (predIn and aOp.isRead_ and predPma.isIo())
	      and not (predOut and not aOp.isRead_ and predPma.isIo()))
	    continue;

          for (auto bOpIx : succ.memOps_)
            {
              auto bOp = sysMemOps_.at(bOpIx);
              auto bOpTime = bOp.minForwardTime();
              if (bOpTime > predTime)
                continue;

              if (not (succRead and bOp.isRead_)
                  and not (succWrite and not bOp.isRead_)
                  and not (succIn and bOp.isRead_ and succPma.isIo())
                  and not (succOut and not bOp.isRead_ and succPma.isIo()))
                continue;

              if (not pred.complete_ or not pred.retired_)
                {
                  cerr << "Error: PPO rule 4 failed: hart-id=" << hart.hartId()
                       << " tag1=" << pred.tag_ << " fence-tag=" << fence.tag_
                       << " memory instruction before fence is not retired/complete\n";
                  return false;
                }

              // Successor performs before predecessor -- Allow if successor is a load
              // and there is no store from another core to the same cache line.
              bool fail = true;
              if (bOp.isRead_)
                {
                  fail = false;

                  // Check at byte level.
                  for (unsigned i = 0; i < aOp.size_ and not fail; ++i)
                    {
                      uint64_t addr = aOp.pa_ + i;
                      if (not bOp.overlaps(addr))
                        continue;
                      predTime = aOp.forwardTime(addr);  // Predecessor byte time
                      succTime = bOp.forwardTime(addr);
                      if (predTime < succTime)
                        continue;

                      // Look for a store from another hart between predecessor and successor times.
                      auto low = std::lower_bound(sysMemOps_.begin(), sysMemOps_.end(), succTime,
                                                  [](const MemoryOp& op, const uint64_t& t) -> bool
                                                  { return op.time_ < t; });

                      auto high = std::upper_bound(low, sysMemOps_.end(), predTime,
                                                   [](const uint64_t& t, const MemoryOp& op) -> bool
                                                   { return t < op.time_; });

                      for (auto iter = low; iter != high and not fail; ++iter)
                        {
                          auto& op = *iter;
                          if (op.hartIx_ == hartIx or isCbomOp(op) or op.isRead_ or
                              lineNum(op.pa_) != lineNum(addr))
                            continue;
                          fail = op.time_ >= succTime and op.time_ <= predTime;
                        }
                    }
                }

              if (fail)
                {
                  cerr << "Error: PPO rule 4 failed: hart-id=" << hart.hartId()
                       << " tag1=" << pred.tag_ << " tag2=" << succ.tag_
                       << " fence-tag=" << fence.tag_
                       << " time1=" << predTime << " time2=" << succTime
                       << " pa=0x" << std::hex << aOp.pa_ << std::dec << '\n';
                  return false;
                }
            }
        }
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::ppoRule5(Hart<URV>& hart, const McmInstr& instrA, const McmInstr& instrB) const
{
  // Rule 5: A has an acquire annotation.
  if (instrA.isCanceled() or not instrA.isMemory())
    return true;

  assert(instrA.isRetired());

  bool hasAcquire = instrA.di_.isAtomicAcquire();
  if (isTso_)
    hasAcquire = hasAcquire or instrA.di_.isLoad() or instrA.di_.isAmo();

  if (not hasAcquire)
    return true;

  if (instrA.di_.isAmo())
    return instrA.memOps_.size() == 2; // Fail if != 2: Incomplete AMO might finish afrer B

  if (not instrA.complete_)
    return false; // Incomplete store might finish after B

  auto timeA = effectiveMaxTime(instrA);
  auto timeB = effectiveMinTime(hart, instrB);

  if (timeB > timeA)
    return true;

  auto hartIx = hart.sysHartIndex();

  // B performs before A -- Allow if there is no write from another hart, overlapping line
  // of B, at time between times of A and B.
  for (size_t ix = sysMemOps_.size(); ix != 0; ix--)
    {
      const auto& op = sysMemOps_.at(ix-1);
      if (op.isCanceled() or op.time_ > timeA or op.isRead_ or op.hartIx_ == hartIx)
	continue;

      if (op.time_ < timeB)
	break;
      if (isCbomOp(op))
        continue;

      for (auto bopIx : instrB.memOps_)
        {
          const auto bop = sysMemOps_.at(bopIx);
          auto bopTime = bop.maxForwardTime();
          if (bopTime > timeA or op.time_ < bopTime or op.time_ > timeA)
            continue;
          if (lineNum(op.pa_) == lineNum(bop.pa_))
            return false;
        }
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::ppoRule5(Hart<URV>& hart, const McmInstr& instrB) const
{
  // Rule 5: A has an acquire annotation

  if (not instrB.isMemory() or instrB.memOps_.empty())
    return true;

  unsigned hartIx = hart.sysHartIndex();
  const auto& instrVec = hartData_.at(hartIx).instrVec_;
  const auto& undrained = hartData_.at(hartIx).undrainedStores_;

  // If B is a store, make sure that there are no un-drained store, with acquire
  // annotation, preceding B in program order. This is stricter than what the spec
  // mandates.
  if (instrB.isStore_)
    {
      for (auto& tag : undrained)
	{
	  if (tag < instrB.tag_)
	    {
	      const auto& instrA = instrVec.at(tag);
	      bool hasAcquire = instrA.di_.isAtomicAcquire();
	      if (isTso_)
		hasAcquire = hasAcquire or instrA.di_.isLoad() or instrA.di_.isAmo();
	      if (hasAcquire)
		{
		  cerr << "Error: PPO rule 5 failed: hart-id=" << hart.hartId()
		       << " tag1=" << instrA.tag_ << " tag2=" << instrB.tag_ << '\n';
		  return false;
		}
	    }
	  else
	    break;
	}
    }

  auto earlyB = effectiveMinTime(hart, instrB);

  for (auto iter = sysMemOps_.rbegin(); iter != sysMemOps_.rend(); ++iter)
    {
      const auto& op = *iter;
      if (op.isCanceled() or op.hartIx_ != hartIx or op.tag_ >= instrB.tag_)
	continue;
      if (op.time_ < earlyB)
	break;
      const auto& instrA =  instrVec.at(op.tag_);
      if (instrA.isCanceled()  or  not instrA.isRetired()  or  not instrA.isMemory())
	continue;

      if (not ppoRule5(hart, instrA, instrB))
	{
	  cerr << "Error: PPO rule 5 failed: hart-id=" << hart.hartId()
	       << " tag1=" << instrA.tag_ << " tag2=" << instrB.tag_ << '\n';
	  return false;
	}
    }

  for (auto& tag : undrained)
    {
      if (tag >= instrB.tag_)
	break;
      const auto& instrA =  instrVec.at(tag);
      if (not ppoRule5(hart, instrA, instrB))
	{
	  cerr << "Error: PPO rule 5 failed: hart-id=" << hart.hartId()
	       << " tag1=" << instrA.tag_ << " tag2=" << instrB.tag_ << '\n';
	  return false;
	}
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::ppoRule6(Hart<URV>& hart, const McmInstr& instrA, const McmInstr& instrB) const
{
  bool hasRelease = instrB.di_.isAtomicRelease();
  if (isTso_)
    hasRelease = hasRelease or instrB.di_.isStore() or instrB.di_.isAmo();

  if (not instrB.isMemory() or not hasRelease)
    return true;

  if (instrA.isCanceled() or not instrA.isMemory())
    return true;

  assert(instrA.isRetired());

  if (instrA.di_.isAmo())
    return instrA.memOps_.size() == 2; // Fail if incomplete AMO (finishes afrer B).

  if (not instrA.complete_)
    return false;       // Fail if incomplete store (finishes after B).

  if (instrB.memOps_.empty())
    return true;   // Un-drained store.

  auto btime = effectiveMinTime(hart, instrB);
  return effectiveMaxTime(instrA) < btime;  // A finishes before B.
}


template <typename URV>
bool
Mcm<URV>::ppoRule6(Hart<URV>& hart, const McmInstr& instrB) const
{
  // Rule 6: B has a release annotation

  if (not instrB.complete_)
    return true;   // Will redo when B is complete.

  if (instrB.di_.isSc() and instrB.memOps_.empty())
    return true;   // Failed sc instruction has no acquire/release requirements.

  auto hartIx = hart.sysHartIndex();
  const auto& instrVec = hartData_.at(hartIx).instrVec_;

  auto earlyB = earliestOpTime(instrB);

  for (auto iter = sysMemOps_.rbegin(); iter != sysMemOps_.rend(); ++iter)
    {
      const auto& op = *iter;
      if (op.isCanceled()  or  op.hartIx_ != hartIx  or  op.tag_ >= instrB.tag_)
	continue;
      if (op.time_ < earlyB)
	break;
      const auto& instrA =  instrVec.at(op.tag_);
      if (instrA.isCanceled()  or  not instrA.isRetired()  or  not instrA.isMemory())
	continue;

      if (not ppoRule6(hart, instrA, instrB))
	{
	  cerr << "Error: PPO rule 6 failed: hart-id=" << hart.hartId()
	       << " tag1=" << instrA.tag_ << " tag2=" << instrB.tag_ << '\n';
	  return false;
	}
    }

  const auto& undrained = hartData_.at(hartIx).undrainedStores_;

  for (auto& tag : undrained)
    {
      if (tag >= instrB.tag_)
	break;
      const auto& instrA =  instrVec.at(tag);
      if (not ppoRule6(hart, instrA, instrB))
	{
	  cerr << "Error: PPO rule 6 failed: hart-id=" << hart.hartId()
	       << " tag1=" << instrA.tag_ << " tag2=" << instrB.tag_ << '\n';
	  return false;
	}
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::ppoRule7(const McmInstr& instrA, const McmInstr& instrB) const
{
  if (instrA.isCanceled() or not instrA.isMemory())
    return true;

  assert(instrA.isRetired());

  bool bHasRc = instrB.di_.isAtomicRelease() or instrB.di_.isAtomicAcquire();
  if (isTso_)
    bHasRc = bHasRc or instrB.di_.isLoad() or instrB.di_.isStore() or instrB.di_.isAmo();

  bool aHasRc = instrA.di_.isAtomicRelease() or instrA.di_.isAtomicAcquire();
  if (isTso_)
    aHasRc = bHasRc or instrA.di_.isLoad() or instrA.di_.isStore() or instrA.di_.isAmo();

  if (not aHasRc or not bHasRc)
    return true;

  bool incomplete = not instrA.complete_ or (instrA.di_.isAmo() and instrA.memOps_.size() != 2);
  if (incomplete)
    return false;   // Incomplete AMO finishes after B.

  if (instrB.memOps_.empty())
    return true;    // Un-drained store will finish later.

  auto btime = earliestOpTime(instrB);
  auto atime = latestOpTime(instrA);
  if (atime < btime)
    return true;  // A finishes before B

  // B performs before A -- Allow if B is a load and there is no write from another hart,
  // overlapping line of B, at time between the times of A and B.
  if (not instrB.di_.isLoad() or instrB.di_.isVectorLoad())
    return false;

  unsigned hartIx = sysMemOps_.at(instrB.memOps_.at(0)).hartIx_;

  for (size_t ix = sysMemOps_.size(); ix != 0; ix--)
    {
      const auto& op = sysMemOps_.at(ix-1);
      if (op.isCanceled() or op.time_ > atime or op.isRead_ or op.hartIx_ == hartIx)
	continue;

      if (op.time_ < btime)
	break;
      if (isCbomOp(op))
        continue;

      for (auto bopIx : instrB.memOps_)
        {
          const auto bop = sysMemOps_.at(bopIx);
          auto bopTime = bop.maxForwardTime();
          if (bopTime > atime or op.time_ < btime or op.time_ > atime)
            continue;
          if (lineNum(op.pa_) == lineNum(bop.pa_))
            return false;
        }
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::ppoRule7(Hart<URV>& hart, const McmInstr& instrB) const
{
  // Rule 7: A and B have RCsc annotations.

  bool bHasRc = instrB.di_.isAtomicRelease() or instrB.di_.isAtomicAcquire();
  if (isTso_)
    bHasRc = bHasRc or instrB.di_.isLoad() or instrB.di_.isStore() or instrB.di_.isAmo();

  if (not instrB.isMemory() or not bHasRc)
    return true;

  auto hartIx = hart.sysHartIndex();
  const auto& instrVec = hartData_.at(hartIx).instrVec_;

  auto earlyB = earliestOpTime(instrB);

  for (auto iter = sysMemOps_.rbegin(); iter != sysMemOps_.rend(); ++iter)
    {
      const auto& op = *iter;
      if (op.isCanceled()  or  op.hartIx_ != hartIx  or  op.tag_ >= instrB.tag_)
	continue;
      if (op.time_ < earlyB)
	break;
      const auto& instrA =  instrVec.at(op.tag_);
      if (instrA.isCanceled()  or  not instrA.isRetired()  or  not instrA.isMemory())
	continue;

      if (not ppoRule7(instrA, instrB))
	{
	  cerr << "Error: PPO rule 7 failed: hart-id=" << hart.hartId()
	       << " tag1=" << instrA.tag_ << " tag2=" << instrB.tag_ << '\n';
	  return false;
	}
    }

  const auto& undrained = hartData_.at(hartIx).undrainedStores_;

  for (auto tag : undrained)
    {
      if (tag >= instrB.tag_)
	break;
      const auto& instrA =  instrVec.at(tag);
      if (not ppoRule7(instrA, instrB))
	{
	  cerr << "Error: PPO rule 7 failed: hart-id=" << hart.hartId()
	       << " tag1=" << instrA.tag_ << " tag2=" << instrB.tag_ << '\n';
	  return false;
	}
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::ppoRule8(Hart<URV>& hart, const McmInstr& instrB) const
{
  // Rule 8: B is a store-conditional, A is a load-reserve paired with B.
  if (not instrB.isMemory() or not instrB.di_.isSc())
    return true;

  uint64_t addr = 0, value = 0;
  if (not hart.lastStore(addr, value))
    return true;  // Score conditional was not successful.

  auto hartIx = hart.sysHartIndex();
  const auto& instrVec = hartData_.at(hartIx).instrVec_;

  auto earlyB = earliestOpTime(instrB);

  for (auto iter = sysMemOps_.rbegin(); iter != sysMemOps_.rend(); ++iter)
    {
      const auto& op = *iter;
      if (op.isCanceled()  or  op.hartIx_ != hartIx  or  op.tag_ >= instrB.tag_)
	continue;
      if (op.time_ < earlyB)
	break;
      const auto& instrA =  instrVec.at(op.tag_);
      if (instrA.isCanceled()  or  not instrA.isRetired()  or  not instrA.isMemory())
	continue;

      if (not instrA.di_.isLr())
	continue;

      if (not instrA.complete_ or
          (not instrB.memOps_.empty() and earlyB <= latestOpTime(instrA)))
	{
	  cerr << "Error: PPO rule 8 failed: hart-id=" << hart.hartId()
	       << " tag1=" << instrA.tag_ << " tag2=" << instrB.tag_ << '\n';
	  return false;
	}

      return true;
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::ppoRule9(Hart<URV>& hart, const McmInstr& instrB) const
{
  // Rule 9: B has a syntactic address dependency on A

  if (not instrB.isMemory())
    return true;

  uint64_t addrTime = instrB.addrTime_;

  // FIX : Issue error if A and B are both cacheable.

  for (auto opIx : instrB.memOps_)
    {
      if (opIx < sysMemOps_.size() and sysMemOps_.at(opIx).time_ <= addrTime)
	{
	  cerr << "Warning: PPO rule 9 failed: hart-id=" << hart.hartId() << " tag1="
	       << instrB.addrProducer_ << " tag2=" << instrB.tag_
               << " time1=" << addrTime << " time2=" << sysMemOps_.at(opIx).time_ << '\n';
	  return true;  // Temporary
	}
    }

  // Check address dependency of index registers of vector instruction B.

  McmInstrIx ixTag = 0; // Producer of vector index register.
  uint64_t ixTime = 0;  // Producer time of vector index register.
  uint64_t dataTime = 0;
  unsigned ixReg = 0;   // Vector index register.
  if (isVecIndexOutOfOrder(hart, instrB, ixReg, ixTag, ixTime, dataTime))
    {
      cerr << "Warning: PPO rule 9 failed: hart-id=" << hart.hartId() << " tag1="
	   << ixTag << " tag2=" << instrB.tag_ << " time1=" << ixTime
	   << " time2=" << dataTime << " vec-ix-reg=" << ixReg << '\n';
      return true;  // Temporary
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::ppoRule10(Hart<URV>& hart, const McmInstr& instrB) const
{
  // Rule 10: B has a syntactic data dependency on A

  const auto& bdi = instrB.di_;

  if (bdi.isSc() and bdi.op2() == 0)
    return true;  // No dependency on X0

  if (bdi.isStore() and bdi.op0() == 0)
    return true;  // No dependency on X0

  if (bdi.isStore() or bdi.isAmo())
    {
      if ((bdi.isSc() and bdi.op1() == 0) or (bdi.isStore() and bdi.op0() == 0))
	return true;
      uint64_t dataTime = instrB.dataTime_;

      for (auto opIx : instrB.memOps_)
	{
	  if (opIx < sysMemOps_.size() and sysMemOps_.at(opIx).time_ <= dataTime)
	    {
	      cerr << "Error: PPO rule 10 failed: hart-id=" << hart.hartId() << " tag1="
		   << instrB.dataProducer_  << " tag2=" << instrB.tag_ << " time1="
		   << dataTime << " time2=" << sysMemOps_.at(opIx).time_ << '\n';
	      return false;
	    }
	}
    }

  if (bdi.isVectorStore())
    {
      auto hartIx = hart.sysHartIndex();

      std::array<std::pair<unsigned, VecKind>, 32> dataVecs;  // reg-num/kind pairs
      unsigned count = getLdStDataVectors(hart, instrB, dataVecs);

      for (unsigned i = 0; i < count; ++i)
	{
	  auto [dataReg, kind] = dataVecs.at(i);
	  if (kind != VecKind::Active)
            continue;

	  auto atag = vecRegProducer(hartIx, dataReg);
	  if (atag == 0)
	    continue;

	  auto atime = vecRegTime(hartIx, dataReg);  // Time A data reg was produced.
	  auto btime = getVecRegEarlyTime(hart, instrB, dataReg);

	  if (btime <= atime)
	    {
	      cerr << "Error: PPO rule 10 failed: hart-id=" << hart.hartId()
		   << " tag1=" << atag << " tag2=" << instrB.tag_ << " time1="
		   << atime << " time2=" << btime << '\n';
	      return false;
	    }
	}
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::ppoRule11(Hart<URV>& hart, const McmInstr& instrB) const
{
  // Rule 11: B is a store with a control dependency on A

  unsigned hartIx = hart.sysHartIndex();
  const auto& instrVec = hartData_.at(hartIx).instrVec_;

  if (not instrB.isStore_)
    return true;

  auto rule11 = [this, &instrVec] (uint64_t earlyB, auto producerTag) -> bool {
    if (producerTag >= instrVec.size() or producerTag == 0)
      return true;
    const auto& producer = instrVec.at(producerTag);
    if (not producer.di_.isValid())
      return true;

    auto producerTime = producer.retireTime_;
    if (producer.isMemory())
      {
	if (not producer.complete_ and producer.isStore_)
          return false;  // Incomplete store considtional
	producerTime = latestOpTime(producer);
      }

    return earlyB > producerTime;
  };

  auto earlyB = earliestOpTime(instrB);
  auto producerTag = hartData_.at(hartIx).branchProducer_;

  if (hartData_.at(hartIx).branchTime_ and not rule11(earlyB, producerTag))
    {
      cerr << "Error: PPO rule 11 failed (branch): hart-id=" << hart.hartId() << " tag1="
           << producerTag << " tag2=" << instrB.tag_ << '\n';
      return false;
    }

  // VL is control dependency for vector instructions
  const auto& bdi = instrB.di_;
  if (bdi.isVectorStore())
    {
      auto& refMap = hartData_.at(hartIx).vecRefMap_;
      auto iter = refMap.find(instrB.tag_);
      if (iter == refMap.end() or iter->second.empty())
	return true;  // Nothing written by instruction.

      producerTag = hartData_.at(hartIx).vlProducer_;
      if (hartData_.at(hartIx).vlTime_ and not rule11(earlyB, producerTag))
        {
          cerr << "Error: PPO rule 11 failed (vl): hart-id=" << hart.hartId() << " tag1="
               << producerTag << " tag2=" << instrB.tag_ << '\n';
          return false;
        }

      if (bdi.isMasked()) // VM is control dependency for masked vector instructions
        {
          producerTag = vecRegProducer(hartIx, 0);   // V0 is mask register.
          if (not rule11(earlyB, producerTag))
            {
              cerr << "Error: PPO rule 11 failed (vm): hart-id=" << hart.hartId() << " tag1="
                   << producerTag << " tag2=" << instrB.tag_ << '\n';
              return false;
            }
        }
    }

  // what about vstart?

  return true;
}


template <typename URV>
McmInstrIx
Mcm<URV>::getMinReadTagWithLargerTime(unsigned hartIx, const McmInstr& instr) const
{
  assert(not instr.canceled_ and instr.retired_);

  auto eot = earliestOpTime(instr);

  McmInstrIx minTag = instr.tag_;

  for (auto iter = sysMemOps_.rbegin(); iter != sysMemOps_.rend(); ++iter)
    {
      const auto& op = *iter;
      if (op.canceled_ or op.hartIx_ != hartIx or not op.isRead_)
	continue;

      if (op.time_ > eot)
	minTag = std::min(minTag, op.tag_);
      else
	break;
    }

  return minTag;
}


template <typename URV>
bool
Mcm<URV>::overlaps(const McmInstr& instr, const std::unordered_set<uint64_t>& addrSet) const
{
  assert(not instr.isCanceled() and instr.isRetired());
  if (instr.isCanceled() or not instr.isRetired() or not instr.isMemory())
    return false;

  if (not instr.di_.isVector())
    {
      uint64_t pa1 = instr.physAddr_, pa2 = instr.physAddr2_;
      uint64_t size1 = instr.size_, size2 = 0;
      if (pa1 != pa2 and pageNum(pa1) != pageNum(pa2))
	{
	  size1 = offsetToNextPage(pa1);
	  size2 = instr.size_ - size1;
	  assert(size1 > 0 and size1 < instr.size_);
	  assert(size2 > 0 and size2 < instr.size_);
	}

      for (unsigned i = 0; i < size1; ++i)
	if (addrSet.contains(pa1 + i))
	  return true;

      for (unsigned i = 0; i < size2; ++i)
	if (addrSet.contains(pa2 + i))
	  return true;

      return false;
    }

  auto hartIx = instr.hartIx_;

  auto& refMap = hartData_.at(hartIx).vecRefMap_;
  auto iter = refMap.find(instr.tag_);
  if (iter == refMap.end())
    return false;

  auto& vecRefs = refMap.at(instr.tag_);

  for (auto refOp : vecRefs.refs_)
    for (unsigned i = 0; i < refOp.size_; ++i)
      if (addrSet.contains(refOp.pa_ + i))
	return true;

  return false;
}


template <typename URV>
bool
Mcm<URV>::ppoRule12(Hart<URV>& hart, const McmInstr& instrB) const
{
  // Rule 12: B is a load, there is a store M between A and B such that
  // 1. B loads a value written by M
  // 2. M has an address or data dependency on A

  if (not instrB.isLoad_)
    return true;  // NA: B is not a load.

  unsigned hartIx = hart.sysHartIndex();

  auto minTag = getMinReadTagWithLargerTime(hartIx, instrB);
  const auto& instrVec = hartData_.at(hartIx).instrVec_;

  // 1. For each read byte address of B, identify the closest overlapping store M in
  //    program order. Keep the store tag and the B byte load time.
  struct ByteInfo
  {
    McmInstrIx storeTag_ = 0;  // Closest store writing byte.
    uint64_t time_ = 0;        // Time byte was loaded by B.
    unsigned reg_= 0;          // Vector register of B overlapping byte.
  };

  std::unordered_map<uint64_t, ByteInfo> byteMap;

  for (auto ix : instrB.memOps_)
    {
      auto& op = sysMemOps_.at(ix);
      for (unsigned i = 0; i < op.size_; ++i)
	{
	  if (not op.isRead_)
	    continue;
	  uint64_t addr = op.pa_ + i;
	  auto iter = byteMap.find(addr);
	  if (iter != byteMap.end())
	    iter->second.time_ = std::min(iter->second.time_, op.forwardTime(addr));
	  else
	    byteMap[addr] = ByteInfo{0, op.forwardTime(addr)};
	}
    }

  auto& vecRefMap = hartData_.at(hartIx).vecRefMap_;
  if (instrB.di_.isVector())
    {
      auto& bvecRefs = vecRefMap.at(instrB.tag_);
      for (auto& vecRef : bvecRefs.refs_)
	for (unsigned i = 0; i < vecRef.size_; ++i)
	  byteMap[vecRef.pa_ + i].reg_ = vecRef.reg_;
    }

  for (McmInstrIx mTag = instrB.tag_ - 1; mTag >= minTag; --mTag)
    {
      const auto& instrM = instrVec.at(mTag);
      if (instrM.isCanceled() or not instrM.di_.isValid() or not instrM.isStore_)
	continue;

      for (auto& [addr, byteInfo] : byteMap)
	if (byteInfo.storeTag_ == 0 and overlapsRefPhysAddr(instrM, addr))
	  byteInfo.storeTag_ = mTag;
    }

  // 2. Process the bytes of B.
  for (auto& [byteAddr, byteInfo] : byteMap)
    {
      auto mTag = byteInfo.storeTag_;
      if (mTag == 0)
	continue;

      const auto& instrM = instrVec.at(mTag);
      auto& mdi = instrM.di_;   // M decoded-instruction.
      auto byteTime = byteInfo.time_;

      if (not mdi.isVectorStore())
	{       // M is a scalar store or AMO.
	  auto mapt = instrM.addrProducer_;  // M address producer tag.
	  auto mdpt = instrM.dataProducer_;  // M data producer tag.
	  auto& ap = instrVec.at(mapt);  // Address producer.
	  auto& dp = instrVec.at(mdpt);  // Data producer.
	  auto addrTime = instrM.addrTime_;
	  auto dataTime = instrM.dataTime_;

	  if (mapt != 0 and ap.isMemory())
	    if (not ap.complete_ or byteTime <= addrTime)
	      {
		cerr << "Error: PPO rule 12 failed: hart-id=" << hart.hartId() << " tag1="
		     << mapt << " tag2=" << instrB.tag_ << " mtag=" << mTag
		     << " time1=" << addrTime << " time2=" << byteTime << " dep=addr\n";
		return false;
	      }

	  if (mdpt != 0 and dp.isMemory())
	    if (not dp.complete_ or byteTime <= dataTime)
	      {
		cerr << "Error: PPO rule 12 failed: hart-id=" << hart.hartId() << " tag1="
		     << mdpt << " tag2=" << instrB.tag_ << " mtag=" << mTag
		     << " time1=" << dataTime << " time2=" << byteTime << " dep=data\n";
		return false;
	      }
	}
      else    // M is a vector store
	{
	  auto iter = vecRefMap.find(mTag);
	  if (iter == vecRefMap.end())
	    continue;

	  bool isIndexed = instrM.di_.isVectorStoreIndexed();
	  auto& vecRefs = iter->second;

	  // Find last element of M overlapping byte of B. We assume vector store
	  // elements are written in order.
	  auto& refs = vecRefs.refs_;
	  for (auto iter = refs.rbegin(); iter != refs.rend(); ++iter)
	    {
	      auto& vecRef = *iter;
	      if (not vecRef.overlaps(byteAddr))
		continue;
	      unsigned dataVec = vecRef.reg_;  // Identify register of element of M.

	      // Find the producer A of identified vector. M has data dep on A.
	      McmInstrIx aTag = 0;
	      uint64_t aTime = 0;
	      for (auto& vpd : instrM.vecProdTimes_)
		if (vpd.regIx_ == dataVec)
		  {
		    aTag = vpd.tag_;
		    aTime = vpd.time_;
		    break;
		  }

	      if (aTag == 0)
		break;  // No producer of data vec.

	      auto& instrA = instrVec.at(aTag);
	      if (not instrA.isMemory())
		break;

	      // Check B against A.
	      if (not instrA.complete_ or byteTime <= aTime)
		{
		  cerr << "Error: PPO rule 12 failed: hart-id=" << hart.hartId() << " tag1="
		       << aTag << " tag2=" << instrB.tag_ << " mtag=" << mTag
		       << " time1=" << aTime << " time2=" << byteTime << " addr2=0x"
		       << std::hex << byteAddr << std::dec << " dep=data\n";
		  return false;
		}

	      if (not isIndexed)
		break;

	      // Get index register corresponding to dataVec.
	      unsigned ixVec = vecRef.ixReg_;

	      // Find the producer AA of identified index register. M has addr dep on AA.
	      aTag = 0;
	      aTime = 0;

	      for (auto& vpd : instrM.ixProdTimes_)
		if (vpd.regIx_ == ixVec)
		  {
		    aTag = vpd.tag_;
		    aTime = vpd.time_;
		    break;
		  }

	      if (aTag == 0)
		break;

	      auto& instrAA = instrVec.at(aTag);
	      if (not instrAA.isMemory())
		break;

	      // Check B against AA.
	      if (not instrAA.complete_ or byteTime <= aTime)
		{
                  
		  cerr << "Error: PPO rule 12 failed: hart-id=" << hart.hartId() << " tag1="
		       << aTag << " tag2=" << instrB.tag_ << " mtag=" << mTag
		       << " time1=" << aTime << " time2=" << byteTime << " dep=addr\n";
		  return false;
		}
	      break;
	    }
	}
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::ppoRule13(Hart<URV>& hart, const McmInstr& instrB) const
{
  // Rule 13: B is a store, there is a instruction M between A and B such that
  // M has an address dependency on A.

  if (not instrB.isStore_ and not instrB.di_.isAmo())
    return true;  // NA: B is not a store/amo.

  unsigned hartIx = hart.sysHartIndex();

  auto minTag = getMinReadTagWithLargerTime(hartIx, instrB);
  const auto& instrVec = hartData_.at(hartIx).instrVec_;

  auto earlyB = earliestOpTime(instrB);

  // Look for a memory instruction M between B and instruction with minTag.
  for (McmInstrIx mTag = instrB.tag_ - 1; mTag >= minTag; --mTag)
    {
      const auto& instrM = instrVec.at(mTag);
      if (instrM.isCanceled() or not instrM.di_.isValid() or not instrM.isMemory())
	continue;

      auto mapt = instrM.addrProducer_;  // M address producer tag.
      const auto& ap = instrVec.at(mapt);  // Address producer.

      if (ap.isMemory())
	if (not ap.complete_ or isBeforeInMemoryTime(instrB, ap))
	  {
            uint64_t apTime = ap.complete_ ? latestOpTime(ap) : ~uint64_t(0);
	    cerr << "Error: PPO rule 13 failed: hart-id=" << hart.hartId() << " tag1="
		 << mapt << " tag2=" << instrB.tag_ << " mtag=" << mTag
		 << " time1=" << apTime << " time2=" << earlyB << '\n';
	    return false;
	  }

      if (instrM.di_.isVectorLoadIndexed() or instrM.di_.isVectorStoreIndexed())
	{
	  for (auto& ipt : instrM.ixProdTimes_)
	    {
	      if (ipt.time_ < earlyB)
		continue;

	      cerr << "Error: PPO rule 13 failed: hart-id=" << hart.hartId() << " tag1="
		   << ipt.tag_ << " tag2=" << instrB.tag_ << " mtag=" << mTag
		   << " time1=" << ipt.time_ << " time2=" << earlyB << '\n';
	      return false;
	    }
	}
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::ioPpoChecks(Hart<URV>& hart, const McmInstr& instrB) const
{
  // We don't have enough info to do this since PBMT can override the PMA. We will have to
  // change the API to be able to do this. Currenttly this should be enabled only when
  // test has no PBMT.

  // We check that IO memory operations are never reordered. This is stronger
  // that what is required by the spec. We will eventually relax this for
  // non strongly ordered regions.

  auto hartIx = hart.sysHartIndex();

  // Identify earliest IO memory time of instr B.
  uint64_t inf = ~uint64_t(0);
  uint64_t earlyRead = inf, earlyWrite = inf;
  for (auto opIx : instrB.memOps_)
    {
      auto& op = sysMemOps_.at(opIx);
      if (op.isIo_  and  not op.canceled_)
	{
	  if (op.isRead_)
	    earlyRead = std::min(earlyRead, op.time_);
	  else
	    earlyWrite = std::min(earlyWrite, op.time_);
	}
    }

  // Check for IO memory operation reordering.
  // Currently checking read against read, and write against write. We will generalize
  // once we get more accurate IO timing from the test-bench.
  for (auto isRead : { true, false })
    {
      auto earlyB = isRead? earlyRead : earlyWrite;
      if (earlyB == inf)
	continue; // No IO memory ops.

      // Identify IO ops reordered with respect to B.
      for (auto iter = sysMemOps_.rbegin(); iter != sysMemOps_.rend(); ++iter)
	{
	  const auto& op = *iter;
	  if (op.isCanceled() or op.hartIx_ != hartIx or op.tag_ >= instrB.tag_ or not op.isIo_)
	    continue;
	  
	  if (op.isRead_ != isRead)
	    continue;  // Temporary till we get more accurate IO timing.

	  if (op.time_ < earlyB)
	    continue;

	  cerr << "Error: IO PPO rule failed: hart-id=" << hart.hartId() << " tag1="
	       << op.tag_ << " tag2=" << instrB.tag_ << " time1=" << op.time_
	       << " time2=" << earlyB << " type=" << (isRead? "read" : "write")
	       << '\n';
	  return false;
	}
    }

  if (earlyWrite == inf)
    return true;  // No IO write memory ops.

  // Check that all undrained IO ops belong to instructions that follow B in program
  // order.
  const auto& undrained = hartData_.at(hartIx).undrainedStores_;
  const auto& instrVec = hartData_.at(hartIx).instrVec_;

  for (auto tag : undrained)
    {
      if (tag >= instrB.tag_)
	break;

      const auto& instrA = instrVec.at(tag);

      if (not instrA.di_.isVectorStore())
        {
          if (instrA.memOps_.empty())
            {
              cerr << "Error: IO PPO rule failed: hart-id=" << hart.hartId() << " tag1="
                   << instrA.tag_ << " tag2=" << instrB.tag_ << " time1=inf"
                   << " time2=" << earlyWrite << " type=write\n";
              return false;
            }

          uint64_t pa1 = instrA.physAddr_, pa2 = instrA.physAddr2_;
          unsigned size1 = instrA.size_;

          if (pageNum(pa1) != pageNum(pa2))
            size1 = offsetToNextPage(pa1);

          // For each reference byte address of A.
          for (unsigned i = 0; i < instrA.size_; ++i)
            {
              uint64_t byteAddr = i < size1 ? pa1 + i : pa2 + i - size1;
              if (not hart.getPma(byteAddr).isIo())
                continue;   // Not an IO address

              for (auto opIx: instrA.memOps_)
                {
                  auto& aop = sysMemOps_.at(opIx);
                  if (aop.isRead_ or aop.overlaps(byteAddr))
                    continue;

                  // Byte addr not drained in A.
                  cerr << "Error: IO PPO rule failed: hart-id=" << hart.hartId() << " tag1="
                       << instrA.tag_ << " tag2=" << instrB.tag_ << " time1=inf"
                       << " time2=" << earlyWrite << " type=write\n";
                  return false;
                }
            }
        }
      else
        {
          auto& vecRefMap = hartData_.at(hartIx).vecRefMap_;
          auto iter = vecRefMap.find(instrA.tag_);
          if (iter == vecRefMap.end())
            continue;   // No reference data in A.

          auto& vecRefs = iter->second;
          if (vecRefs.empty())
            continue;   // No reference data in A.

          // For each reference byte address of A.
          for (auto& vecRef : vecRefs.refs_)
            {
              for (unsigned i = 0; i < vecRef.size_; ++i)
                {
                  uint64_t byteAddr = vecRef.pa_ + i;
                  if (not hart.getPma(byteAddr).isIo())
                    continue;  // Not an IO address

                  for (auto opIx: instrA.memOps_)
                    {
                      auto aop = sysMemOps_.at(opIx);
                      if (aop.isRead_ or aop.overlaps(byteAddr))
                        continue;

                      // Byte addr not drained in A.
                      cerr << "Error: IO PPO rule failed: hart-id=" << hart.hartId() << " tag1="
                           << instrA.tag_ << " tag2=" << instrB.tag_ << " time1=inf"
                           << " time2=" << earlyWrite << " type=write\n";
                      return false;
                    }
                }
            }
        }
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::checkLoadVsPriorCmo(Hart<URV>& hart, const McmInstr& instrB) const
{
  // If B is a load and A is cbo.flush/clean/inval instruction that overlaps B.

  if (not instrB.isLoad_)
    return true;  // NA: B is not a load.

  auto hartIx = hart.sysHartIndex();
  const auto& instrVec = hartData_.at(hartIx).instrVec_;

  auto earlyB = effectiveMinTime(hart, instrB);

  for (auto ix = instrB.tag_; ix > 0; --ix)
    {
      const auto& instrA = instrVec.at(ix-1);

      if (instrA.isCanceled() or not instrA.isRetired())
	continue;

      if (earlyB > instrA.retireTime_)
	break;

      if (instrA.di_.extension() != RvExtension::Zicbom)
        continue;  // Not cbo.flush/clean/inval

      auto aTime = instrA.retireTime_;
      if (instrA.memOps_.size() == 1)   // CMO instrs have at most 1 bypass op
        {
          const auto& aop = sysMemOps_.at(instrA.memOps_.at(0));
          aTime = std::min(aTime, aop.time_);  // Bypass time may be earlier.
        }

      for (const auto& opIx : instrB.memOps_)
        if (opIx < sysMemOps_.size())
          {
            const auto& bop = sysMemOps_.at(opIx);
            if (overlaps(instrA, bop) and bop.time_ < aTime)
              {
                cerr << "Error: Read op of load instruction happens before retire time of "
                     << "preceding overlapping cbo.clean/flush: hart-id=" << hart.hartId()
                     << " cbo-tag=" << instrA.tag_ << " load-tag=" << instrB.tag_
                     << " cbo-time=" << aTime << " read-time=" << bop.time_
                     << '\n';
                return false;
              }
          }
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::checkSfenceInvalIr(Hart<URV>& hart, const McmInstr& instr) const
{
  // This is very crude. We are using retire time of sinval.vma, we should be using
  // execution time. We are using read/write times of load/store instructions, we should be
  // using translation times. Required times are currently not available. We do not
  // consider instruction address translation.

  unsigned hartIx = hart.sysHartIndex();

  auto invalTag = hartData_.at(hartIx).sinvalVmaTag_;
  if (invalTag == 0)
    return true;   // No sinval.vma was retired

  auto invalTime = hartData_.at(hartIx).sinvalVmaTime_;

  for (size_t ix = sysMemOps_.size(); ix > 0; --ix)
    {
      const auto& op = sysMemOps_.at(ix-1);
      if (op.canceled_ or op.hartIx_ != hartIx)
	continue;
      if (op.tag_ < instr.tag_)
	break;
      if (op.time_ < invalTime)
	{
	  cerr << "Error: Hart-id=" << hart.hartId() << " implicit memory access for "
	       << "translation of instruction at tag=" << op.tag_ << " is out of order "
	       << "with respect to sinval.vma instruction with tag=" << invalTag
	       << " and sfence.inval.ir with tag=" << instr.tag_ << '\n';
	  return false;
	}
    }

  return true;
}


template <typename URV>
bool
Mcm<URV>::checkSfenceWInval(Hart<URV>& hart, const McmInstr& instr) const
{
  // This is very crude: Check that there are no earlier (in program order) stores pending
  // in the store/merge buffer when the sfence.w.inval is retired.

  unsigned hartIx = hart.sysHartIndex();
  const auto& pendingWrites = hartData_.at(hartIx).pendingWrites_;

  for (auto& op : pendingWrites)
    if (op.tag_ < instr.tag_)
      {
        cerr << "Error: Hart-id=" << hart.hartId() << " sfence-tag=" << instr.tag_
             << " store-tag=" << op.tag_ << " sfence.w.inval retires while an older"
             << " store is still the store/merge buffer\n";
        return false;
      }

  return true;
}


template <typename URV>
bool
Mcm<URV>::isVecIndexOutOfOrder(Hart<URV>& hart, const McmInstr& instr, unsigned& ixReg,
			       McmInstrIx& producerTag, uint64_t& producerTime,
			       uint64_t& dataTime) const
{
  const auto& di = instr.di_;

  bool isVecIndexed = di.isVectorLoadIndexed() or di.isVectorStoreIndexed();
  if (not isVecIndexed)
    return false;

  auto hartIx = hart.sysHartIndex();

  const VecLdStInfo& info = hart.getLastVectorMemory();
  const auto& elems = info.elems_;

  unsigned elemSize = info.elemSize_;
  assert(elemSize != 0);

  unsigned ixElemSize = hart.vecLdStIndexElemSize(di);
  assert(ixElemSize != 0);
  unsigned ixElemsPerVec = hart.vecRegSize() / ixElemSize;

  uint64_t maxVal = ~uint64_t(0);

  bool unitStride = isUnitStride(info);

  for (const auto& elem : elems)
    {
      if (elem.skip_)
	continue;

      // Compute the earliest data element time.
      uint64_t dTime = maxVal;
      uint64_t pa1 = elem.pa_, pa2 = elem.pa2_;
      unsigned size = elemSize, size1 = elemSize;
      if (pa1 != pa2)
	{
	  size1 = offsetToNextPage(pa1);
	  assert(size1 > 0 and size1 < 8);
	}

      for (unsigned i = 0; i < size; ++i)
	{
	  uint64_t addr = i < size1 ? pa1 + i : pa2 + i - size1;
	  uint64_t byteTime = unitStride ? earliestByteTime(instr, addr) :
                                           earliestByteTime(instr, addr, elem.ix_);
	  if (byteTime > 0)  // Byte time is zero for undrained writes.
	    dTime = std::min(byteTime, dTime);
	}
      
      // Compare data element time against corresponding index register time.
      unsigned ixVec = info.ixVec_ + elem.ix_ / ixElemsPerVec;
      auto ixTime = vecRegTime(hartIx, ixVec);

      if (ixTime < dTime)
	continue;

      producerTag = vecRegProducer(hartIx, ixVec);
      if (producerTag == 0)
	continue;

      producerTime = ixTime;
      ixReg = ixVec;
      dataTime = dTime;
      return true;
    }

  return false;
}


template <typename URV>
uint64_t
Mcm<URV>::getVecRegEarlyTime(Hart<URV>& hart, const McmInstr& instr, unsigned regNum) const
{
  uint64_t maxVal = ~uint64_t(0);
  uint64_t time = maxVal;   // In case no memory ops or all elements masked.

  if (instr.memOps_.empty())
    return time;

  const VecLdStInfo& info = hart.getLastVectorMemory();
  auto elemSize = info.elemSize_;
  const auto& elems = info.elems_;
  bool unitStride = isUnitStride(info);

  if (elemSize == 0 or info.elems_.empty())
    return time;  // Should not happen.

  unsigned elemsPerVec =  hart.vecRegSize() / elemSize;

  for (auto& elem : elems)
    {
      if (elem.skip_)
	continue;  // Non active.

      unsigned elemReg = info.vec_ + elem.ix_ / elemsPerVec + elem.field_;
      if (elemReg != regNum)
        continue;  // Wrong vector register.

      uint64_t pa1 = elem.pa_, pa2 = elem.pa2_;
      unsigned size = elemSize, size1 = elemSize;
      if (pa1 != pa2)
	{
	  size1 = offsetToNextPage(pa1);
	  assert(size1 > 0 and size1 < 8);
	}

      for (unsigned i = 0; i < size; ++i)
	{
	  uint64_t addr = i < size1 ? pa1 + i : pa2 + i - size1;

          uint64_t byteTime = 0;
          if (not instr.hasOverlap_ or unitStride)
            byteTime = earliestByteTime(instr, addr);
          else
            byteTime = earliestByteTime(instr, addr, elem.ix_);  // FIX: write ops have no index.
	  if (byteTime > 0)  // Byte time is zero for undrained writes.
	    time = std::min(byteTime, time);
	}
    }

  return time;
}


template <typename URV>
void
Mcm<URV>::reportMissingFetch(const Hart<URV>& hart, uint64_t tag, uint64_t pa) const
{
  cerr << "Warning: Hart-id=" << hart.hartId() << " time=" << time_ << " tag=" << tag
       << " pa=0x" << std::hex << pa << std::dec << " opcode missing in instruction cache"
       << " (not brought in with mcm_ifetch)\n";
}



template class WdRiscv::Mcm<uint32_t>;
template class WdRiscv::Mcm<uint64_t>;
