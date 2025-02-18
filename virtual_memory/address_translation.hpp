#pragma once

#include <cstdint>
#include "trapEnums.hpp"
#include "ad_VirtMem.hpp"         

namespace WdRiscv {

typedef bool (*MemReadCallback)(uint64_t addr, bool bigEndian, uint32_t &data);
typedef bool (*MemWriteCallback)(uint64_t addr, bool bigEndian, uint32_t data);
typedef bool (*PmpCheckCallback)(uint64_t addr, PrivilegeMode pm);

void setMemReadCallback(MemReadCallback cb);
void setMemWriteCallback(MemWriteCallback cb);
void setPmpIsReadableCallback(PmpCheckCallback cb);
void setPmpIsWritableCallback(PmpCheckCallback cb);

bool callMemRead(uint64_t addr, bool bigEndian, uint32_t data);
bool callMemWrite(uint64_t addr, bool bigEndian, uint32_t data);
bool callPmpIsReadable(uint64_t addr, PrivilegeMode pm);
bool callPmpIsWritable(uint64_t addr, PrivilegeMode pm);

} // namespace WdRiscv

