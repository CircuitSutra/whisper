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

#pragma once

#include <array>
#include <string_view>
#include "InstEntry.hpp"
#include "InstId.hpp"

namespace WdRiscv
{

  /// Model a decoded instruction: instruction address, opcode, and
  /// operand fields. All instructions are assumed to have the form
  ///   inst op0, op1, op2, op3
  /// where op0 to op3 are optional. For example, in "add x2, x1, x0"
  /// op0 is x2, op1 is x1 and op2 is x0.
  ///
  /// Load instructions of the form "load rd, offset(rs1)" get mapped
  /// to "load rd, rs1, offset" assigning rd to op0 and offset to op2.
  ///
  /// Store instructions of the form "store rs2, offset(rs1)" get mapped
  /// to "store rs2, rs1, offset" assigning rs2 to op0 and offset to op2.
  ///
  class DecodedInst
  {
  public:

    /// Default contructor: Define an invalid object.
    DecodedInst()
      : addr_(0), physAddr_(0), inst_(0), size_(0), entry_(nullptr), op0_(0),
	op1_(0), op2_(0), op3_(0), valid_(false), masked_(false), vecFields_(0)
    { values_[0] = values_[1] = values_[2] = values_[3] = 0; }

    /// Constructor.
    DecodedInst(uint64_t addr, uint32_t inst, const InstEntry* entry,
		uint32_t op0, uint32_t op1, uint32_t op2, uint32_t op3)
      : addr_(addr), physAddr_(0), inst_(inst), size_(instructionSize(inst)),
	entry_(entry), op0_(op0), op1_(op1), op2_(op2), op3_(op3),
	valid_(entry != nullptr), masked_(false), vecFields_(0)
    { values_[0] = values_[1] = values_[2] = values_[3] = 0; }

    /// Return instruction size in bytes.
    uint32_t instSize() const
    { return size_; }

    /// Return the virtual address of the instruction.
    uint64_t address() const
    { return addr_; }

    /// Return the physical address of the instruction.
    uint64_t physAddress() const
    { return physAddr_; }

    /// Return instruction code.
    uint32_t inst() const
    { return inst_; }

    /// Return the 1st operand (zero if instruction has no operands).
    /// First operand is typically the destination register.
    uint32_t op0() const
    { return op0_; }

    /// Return 2nd operand (zero if instruction has no 2nd operand).
    /// Second operand is typically source register rs1.
    uint32_t op1() const
    { return op1_; }

    /// Return 2nd operand as a signed integer. This is useful
    /// for instructions where the 2nd operand is a signed immediate
    /// value.
    template <typename SI>
    SI op1As() const
    { return int32_t(op1_); }

    /// Return 3rd operand (zero if instruction has no 3rd operand).
    /// Third operand is typically source register rs2 or immediate
    /// value.
    uint32_t op2() const
    { return op2_; }

    /// Return 3rd operand as a signed 32-bit integer. This is useful
    /// for instructions where the 3rd operand is a signed immediate
    /// value.
    template <typename SI>
    SI op2As() const
    { return int32_t(op2_); }

    /// Return 4th operand (zero if instruction has no 4th operand).
    /// Fourth operand is typically source register rs3 for
    /// multiply-add like floating point instructions.
    uint32_t op3() const
    { return op3_; }

    /// Return the operand count associated with this
    /// instruction. Immediate values are counted as operands. For
    /// example, in "addi x3, x4, 10", there are 3 operands: 3, 4, and
    /// 10 with types IntReg, IntReg and Imm respectively.
    unsigned operandCount() const
    { return entry_? entry_->operandCount() : 0; }

    /// Return the ith operands or zero if i is out of bounds. For exmaple, if
    /// decode insruction is "addi x3, x4, 10" then the 0th operand would be 3
    /// and the second operands would be 10.
    uint32_t ithOperand(unsigned i) const;

    /// Return the ith operands or zero if i is out of bounds. For exmaple, if
    /// decode insruction is "addi x3, x4, 10" then the 0th operand would be 3
    /// and the second operands would be 10.
    int32_t ithOperandAsInt(unsigned i) const;

    /// Return the type of the ith operand or None if i is out of
    /// bounds. Object must be valid.
    OperandType ithOperandType(unsigned i) const
    { return entry_? entry_->ithOperandType(i) : OperandType::None; }

    /// Return the mode of the ith operand or None if i is out of
    /// bounds. Object must be valid.
    OperandMode ithOperandMode(unsigned i) const
    { return entry_? entry_->ithOperandMode(i) : OperandMode::None; }

    /// For csrrs/csrrc the CSR register is read-only if the second integer register is x0
    OperandMode effectiveIthOperandMode(unsigned i) const
    {
      auto mode = this->ithOperandMode(i);
      auto instId = this->instId();
      if (instId == WdRiscv::InstId::csrrs or instId == WdRiscv::InstId::csrrc)
	{
	  if (this->ithOperandType(i) == WdRiscv::OperandType::CsReg and this->op1() == 0)
	    return WdRiscv::OperandMode::Read;
	}
      return mode;
    }

    /// Return true if this object is valid.
    bool isValid() const
    { return valid_; }

    /// Make invalid.
    void invalidate()
    { valid_ = false; }

    /// Return associated instruction table information.
    const InstEntry* instEntry() const
    { return entry_; }

    /// Relevant for floating point instructions with rounding mode. Return true
    /// if instruction has an explcit rounding mode field.
    bool hasRoundingMode() const
    { return entry_ and entry_->hasRoundingMode(); }

    /// Return true if instruction has an explicit rouning mode field that is set
    /// to dynamic.
    bool hasDynamicRoundingMode() const
    { return hasRoundingMode() and roundingMode() == 7; }

    /// Relevant for floating point instructions.
    unsigned roundingMode() const
    { return ((inst_ >> 12) & 7); }

    /// Return true if instruction modifies the FFLAGS CSR.
    bool modifiesFflags() const
    { return entry_ and entry_->modifiesFflags(); }

    /// Immediate values are to be (left) shifted by this size
    unsigned immediateShiftSize() const
    { return entry_? entry_->immediateShiftSize() : 0; }

    /// Return true if instruction is one of mret/sret/dret.
    bool isXRet() const
    { return entry_ and (entry_->instId() == InstId::mret or
                         entry_->instId() == InstId::sret or
                         entry_->instId() == InstId::dret); }

    /// Relevant to atomic instructions: Return true if acquire bit is
    /// set in an atomic instruction.
    bool isAtomicAcquire() const
    { return entry_ and entry_->isAtomic() and ((inst_ >> 26) & 1); }

    /// Relevant to atomic instructions: Return true if release bit is
    /// set in an atomic instrution.
    bool isAtomicRelease() const
    { return entry_ and entry_->isAtomic() and ((inst_ >> 25) & 1); }

    /// Return true if this a fence instruction (not fence.tso).
    bool isFence() const
    { return entry_ and entry_->instId() == InstId::fence; }

    /// Return true if this a pause instruction.
    bool isPause() const
    { return entry_ and entry_->instId() == InstId::pause; }

    /// Return true if this a fence instruction (not fence.tso).
    bool isSfence_vma() const
    { return entry_ and entry_->instId() == InstId::sfence_vma; }

    /// Return true if this a fence instruction (not fence.tso).
    bool isFence_i() const
    { return entry_ and entry_->instId() == InstId::fence_i; }

    /// Return true if this a fence.tso instruction (not fence).
    bool isFenceTso() const
    { return entry_ and entry_->instId() == InstId::fence_tso; }

    /// Predecessor read bit of fence instruction.
    bool isFencePredRead() const
    { return (isFence() or isFenceTso()) and ((inst_ >> 25) & 1); }

    /// Predecessor write bit of fence instruction.
    bool isFencePredWrite() const
    { return (isFence() or isFenceTso()) and ((inst_ >> 24) & 1); }

    /// Predecessor input (io read) bit of fence instruction.
    bool isFencePredInput() const
    { return (isFence() or isFenceTso()) and ((inst_ >> 27) & 1); }

    /// Predecessor output (io write) bit of fence instruction.
    bool isFencePredOutput() const
    { return (isFence() or isFenceTso()) and ((inst_ >> 26) & 1); }

    /// Successor read bit of fence instruction.
    bool isFenceSuccRead() const
    { return (isFence() or isFenceTso()) and ((inst_ >> 21) & 1); }

    /// Successor write bit of fence instruction.
    bool isFenceSuccWrite() const
    { return (isFence() or isFenceTso()) and ((inst_ >> 20) & 1); }

    /// Successor input (io read) bit of fence instruction.
    bool isFenceSuccInput() const
    { return (isFence() or isFenceTso()) and ((inst_ >> 23) & 1); }

    /// Successor output (io write) bit of fence instruction.
    bool isFenceSuccOutput() const
    { return (isFence() or isFenceTso()) and ((inst_ >> 22) & 1); }

    /// Return true if this is an AMO instruction (atomic but not lr/sc).
    bool isAmo() const
    { return entry_ and entry_->isAmo(); }

    /// Return true if this an atomic instruction (amo or lr/sc).
    bool isAtomic() const
    { return entry_ and entry_->isAtomic(); }

    /// Return true if this is a hypervisor instruction.
    bool isHypervisor() const
    { return entry_ and entry_->isHypervisor(); }

    /// Return true if this a floating point instruction.
    bool isFp() const
    { return entry_ and entry_->isFp(); }

    /// Return true if this a vector instruction. This returns true
    /// for all vector instructions including vector load/store.
    bool isVector() const
    { return entry_ and entry_->isVector(); }

    // Return true if this is a CMO instruction.
    bool isCmo() const
    { return entry_ and entry_->isCmo(); }

    /// Return true if this is a vector load instruction.
    bool isVectorLoad() const
    {
      if (not isVector()) return false;
      unsigned f3 = (inst() >> 12) & 7;
      return (inst() & 0x7f) == 7 and (f3 == 0 or f3 >= 5);
    }

    /// Return true if this is a vector store instruction.
    bool isVectorStore() const
    {
      if (not isVector()) return false;
      unsigned f3 = (inst() >> 12) & 7;
      return (inst() & 0x7f) == 0x27 and (f3 == 0 or f3 >= 5);
    }

    /// Return true if this is a vector strided load instruction.
    bool isVectorLoadStrided() const
    {
      unsigned mop = (inst() >> 26) & 3;
      return isVectorLoad() and (mop == 2);
    }

    /// Return true if this is a vector strided store instruction.
    bool isVectorStoreStrided() const
    {
      unsigned mop = (inst() >> 26) & 3;
      return isVectorStore() and (mop == 2);
    }

    /// Return true if this is a vector indexed load instruction.
    bool isVectorLoadIndexed() const
    {
      unsigned mop = (inst() >> 26) & 3;
      return isVectorLoad() and (mop == 1 or mop == 3);
    }

    /// Return true if this is a vector indexed store instruction.
    bool isVectorStoreIndexed() const
    {
      unsigned mop = (inst() >> 26) & 3;
      return isVectorStore() and (mop == 1 or mop == 3);
    }

    /// Return true if this an mop (maybe-operation) instruction.
    bool isMop() const
    {
      auto id = instId();
      return id == InstId::mop_rr or id == InstId::mop_r or id == InstId::c_mop;
    }

    /// Return the element size in bytes of a vector load/store instruction. Return zero
    /// for a non vector load/store instruction. For indexed or segment-indexed
    /// instructions, this returns the index element size.
    unsigned vecLoadOrStoreElemSize() const
    {
      if (not isVectorLoad() and not isVectorStore()) return 0;
      unsigned f3 = (inst() >> 12) & 7;
      if (f3 == 0) return 1;
      if (f3 == 5) return 2;
      if (f3 == 6) return 4;
      if (f3 == 7) return 8;
      return 0;
    }

    /// Return the element size in bytes of a vector load instruction. Return zero for a
    /// non-vector-load instruction. For load-indexed and load-segment-indexed, this
    /// returns the index element size.
    unsigned vecLoadElemSize() const
    {
      if (not isVectorLoad())
	return 0;
      return vecLoadOrStoreElemSize();
    }

    /// Return the element size in bytes of a vector store instruction. Return zero for a
    /// non-vector-store instruction. For store-indexed and store-segment-indexed, this
    /// returns the index element size.
    unsigned vecStoreElemSize() const
    {
      if (not isVectorStore())
	return 0;
      return vecLoadOrStoreElemSize();
    }

    /// Return true if this a CSR instruction.
    bool isCsr() const
    { return entry_ and entry_->isCsr(); }

    /// Return true if this a multiply instruction.
    bool isMultiply() const
    { return entry_ and entry_->isMultiply(); }

    /// Return true if this a divide instruction.
    bool isDivide() const
    { return entry_ and entry_->isDivide(); }

    /// Return true if this is a load instruction. This includes
    /// floating point load, load-reserve, and hypervisor load, but
    /// not AMOs.
    bool isLoad() const
    { return entry_ and entry_->isLoad(); }

    /// Return true if this is a load instruction. This includes
    /// floating point load, load-reserve, and hypervisor load, but
    /// not AMOs. If successful, set isUnsigned to true if the load
    /// is unsigned.
    bool isLoad(bool& isUnsigned) const
    { return entry_ and entry_->isLoad(isUnsigned); }

    /// Return true if this instruction is viewed as a load by the performance
    /// counters. By default LR is not a perf-load instuctions. Also by default FP loads
    /// are not perf-loads.
    bool isPerfLoad() const
    { return entry_ and entry_->isPerfLoad(); }

    /// Return true if this instruction is viewed as a store by the performance
    /// counters. By default SC is not a perf-store instuctions. Also by default FP stores
    /// are not perf-stores.
    bool isPerfStore() const
    { return entry_ and entry_->isPerfStore(); }

    /// Return true if this is a store instruction. This includes
    /// floating point store and store-conditional but not AMOs.
    bool isStore() const
    { return entry_ and entry_->isStore(); }

    /// Return true if this is an lr (load reserve) instruction.
    bool isLr() const
    { return entry_ and entry_->isLr(); }

    /// Return true if this is an sc (score conditional) instruction.
    bool isSc() const
    { return entry_ and entry_->isSc(); }

    /// Return true if this is an cbo.zero (cache bloc zoer) instruction.
    bool isCbo_zero() const
    { return entry_ and entry_->instId() == InstId::cbo_zero; }

    /// Return the data size in bytes of a load instruction. Return
    /// zero for a non-load instruction.
    unsigned loadSize() const
    { return entry_ ? entry_->loadSize() : 0; }

    /// Return the data size in bytes of a store instruction. Return zero for a non-store
    /// instruction.
    unsigned storeSize() const
    { return entry_ ? entry_->storeSize() : 0; }

    /// Return the data size in bytes of an amo instruction (excluding lr/sc). Return zero
    /// for a non-amo instruction.
    unsigned amoSize() const
    { return entry_ ? entry_->amoSize() : 0; }

    /// Return true if this is a branch instruction.
    bool isBranch() const
    { return entry_ and entry_->isBranch(); }

    /// Return true if this is a conditional branch instruction.
    bool isConditionalBranch() const
    { return entry_ and entry_->isConditionalBranch(); }

    /// Return true if this is a branch instruction where the target
    /// address is in a register.
    bool isBranchToRegister() const
    { return entry_ and entry_->isBranchToRegister(); }

    /// Return true if this is a compressed instruction.
    bool isCompressed() const
    { return entry_ and entry_->isCompressed(); }

    /// Return the RISCV extension of this instruction.
    RvExtension extension() const
    { return entry_? entry_->extension() : RvExtension::None; }

    /// Return the RISCV format of this instruction.
    RvFormat format() const
    { return entry_? entry_->format() : RvFormat::None; }

    /// Return the instruction id of this instruction.
    InstId instId() const
    { return entry_? entry_->instId() : InstId::illegal; }

    /// Return the instruction name.
    std::string name() const;

    /// Associated a value with the ith operand. This has no effect if
    /// i is out of bounds or if the ith operand is an immediate. Note
    /// that the association is only in this object and that no
    /// register value is changed by this method.
    void setIthOperandValue(unsigned i, uint64_t value);

    /// Return value associated with ith operand.
    uint64_t ithOperandValue(unsigned i) const
    { return i < 4? values_[i] : 0; }

    /// Return true if this is a vector instruction with masking enabled,
    bool isMasked() const
    { return masked_; }

    /// Return number of fields in vector ld/st instruction. Return zero
    /// if this is not a vector ld/st.
    unsigned vecFieldCount() const
    { return vecFields_; }

    /// Reset this object to the given values.
    void reset(uint64_t addr, uint64_t physAddr, uint32_t inst,
	       const InstEntry* entry,
	       uint32_t op0, uint32_t op1, uint32_t op2, uint32_t op3)
    {
      addr_ = addr;
      physAddr_ = physAddr;
      inst_ = inst;
      entry_ = entry;
      op0_ = op0; op1_ = op1; op2_ = op2; op3_ = op3;
      size_ = instructionSize(inst);
      valid_ = entry != nullptr;
    }

    /// Mark as a masked instruction. Only relevant to vector instructions.
    void setMasked(bool flag)
    { masked_ = flag; }

    /// Set the field count. Only relevant to vector load/store instruction.
    void setVecFieldCount(uint32_t count)
    { vecFields_ = count; }

    /// Reset address to given value.
    void resetAddr(uint64_t addr)
    { addr_ = addr; }

  protected:

    void setAddr(uint64_t addr)
    { addr_ = addr; }

    void setInst(uint32_t inst)
    { inst_ = inst; size_ = instructionSize(inst); }

    void setEntry(const InstEntry* e)
    { entry_ = e; if (not e) valid_ = false; }

    void setOp0(uint32_t op0)
    { op0_ = op0; }

    void setOp1(uint32_t op1)
    { op1_ = op1; }

    void setOp2(uint32_t op2)
    { op2_ = op2; }

    void setOp3(uint32_t op3)
    { op3_ = op3; }

  private:

    uint64_t addr_;
    uint64_t physAddr_;
    uint32_t inst_;
    uint32_t size_;
    const InstEntry* entry_;
    uint32_t op0_;    // 1st operand (typically a register number)
    uint32_t op1_;    // 2nd operand (register number or immediate value)
    uint32_t op2_;    // 3rd operand (register number or immediate value)
    uint32_t op3_;    // 4th operand (typically a register number)

    std::array<uint64_t, 4> values_;  // Values of operands.
    bool valid_;
    bool masked_;     // For vector instructions.
    uint8_t vecFields_;   // For vector ld/st instructions.
  };


  /// Return 2nd operand as a signed 64-bit integer. This is useful
  /// for instructions where the 2nd operand is a signed immediate
  /// value.
  template <>
  inline
  int64_t
  DecodedInst::op1As<int64_t>() const
  { return int32_t(op1_); }


  /// Return 3rd operand as a signed 64-bit integer. This is useful
  /// for instructions where the 3rd operand is a signed immediate
  /// value.
  template <>
  inline
  int64_t
  DecodedInst::op2As<int64_t>() const
  { return int32_t(op2_); }

}
