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

#include <algorithm>
#include "PerfRegs.hpp"


using namespace WdRiscv;


PerfRegs::PerfRegs(unsigned numCounters)
{
  // 29 counters: MHPMCOUNTER3 to MHPMCOUNTER31
  counters_.resize(29);

  config(numCounters);
}


void
PerfRegs::config(unsigned numCounters)
{
  assert(numCounters < counters_.size());

  eventOfCounter_.resize(numCounters);
  enableMask_.resize(numCounters);
}


bool
PerfRegs::applyPerfEventAssign()
{
  if (not hasPending_)
    return false;

  hasPending_ = false;

  if (pendingCounter_ >= eventOfCounter_.size())
    return false;

  eventOfCounter_.at(pendingCounter_) = pendingEvent_;
  enableMask_.at(pendingCounter_) = pendingMask_;

  activeCounter_ = false;

  for (auto& event : eventOfCounter_)
    if (event != EventNumber::None)
      {
	activeCounter_ = true;
	break;
      }

  return true;
}


void
PerfRegs::reset()
{
  eventOfCounter_.assign(eventOfCounter_.size(), EventNumber::None);
}


// Map a performance event name (string) to the corresponding internal id (enum).
const std::unordered_map<std::string_view, EventNumber>
PerfRegs::eventNameToId_ = {
  { "None", EventNumber::None },
  { "CpuCycles", EventNumber::CpuCycles },
  { "InstCommited", EventNumber::InstCommited },
  { "Inst16Commited", EventNumber::Inst16Commited },
  { "Inst32Commited", EventNumber::Inst32Commited },
  { "InstAligned", EventNumber::InstAligned },
  { "Mult", EventNumber::Mult },
  { "Div", EventNumber::Div },
  { "Load", EventNumber::Load },
  { "Store", EventNumber::Store },
  { "MisalignLoad", EventNumber::MisalignLoad },
  { "MisalignStore", EventNumber::MisalignStore },
  { "Alu", EventNumber::Alu },
  { "CsrRead", EventNumber::CsrRead },
  { "CsrReadWrite", EventNumber::CsrReadWrite },
  { "CsrWrite", EventNumber::CsrWrite },
  { "Ebreak", EventNumber::Ebreak },
  { "Ecall", EventNumber::Ecall },
  { "Fence", EventNumber::Fence },
  { "Fencei", EventNumber::Fencei },
  { "Mret", EventNumber::Mret },
  { "Branch", EventNumber::Branch },
  { "BranchTaken", EventNumber::BranchTaken },
  { "Exception", EventNumber::Exception },
  { "TimerInterrupt", EventNumber::TimerInterrupt },
  { "ExternalInterrupt", EventNumber::ExternalInterrupt },
  { "BusFetch", EventNumber::BusFetch },
  { "BusTransactions", EventNumber::BusTransactions },
  { "BusMisalign", EventNumber::BusMisalign },
  { "IbusError", EventNumber::IbusError },
  { "DbusError", EventNumber::DbusError },
  { "Atomic", EventNumber::Atomic },
  { "Lr", EventNumber::Lr },
  { "Sc", EventNumber::Sc },
  { "Bitmanip", EventNumber::Bitmanip },
  { "BusLoad", EventNumber::BusLoad },
  { "BusStore", EventNumber::BusStore },
  { "MultDiv", EventNumber::MultDiv },
  { "FpHalf", EventNumber::FpHalf },
  { "FpSingle", EventNumber::FpSingle },
  { "FpDouble", EventNumber::FpDouble },
  { "Vector", EventNumber::Vector },
  { "Csr", EventNumber::Csr }
};
