#pragma once

#include <cstdint>
#include <functional>
#include "trapEnums.hpp"
#include "VirtMem.hpp"

namespace WdRiscv {

using MemReadCallback  = std::function<bool(uint64_t, bool, uint64_t &)>;
using MemWriteCallback = std::function<bool(uint64_t, bool, uint64_t)>;
using PmpCheckCallback = std::function<bool(uint64_t, PrivilegeMode)>;


bool callMemRead(const VirtMem* vm, uint64_t addr, bool bigEndian, uint64_t& data);
bool callMemWrite(const VirtMem* vm, uint64_t addr, bool bigEndian, uint64_t data);
bool callPmpIsReadable(const VirtMem* vm, uint64_t addr, PrivilegeMode pm);
bool callPmpIsWritable(const VirtMem* vm, uint64_t addr, PrivilegeMode pm);

} // namespace WdRiscv

