#include "address_translation.hpp"

namespace WdRiscv {

namespace {
    MemReadCallback    g_memReadCallback    = nullptr;
    MemWriteCallback   g_memWriteCallback   = nullptr;
    PmpCheckCallback   g_pmpReadableCallback = nullptr;
    PmpCheckCallback   g_pmpWritableCallback = nullptr;
}

void setMemReadCallback(MemReadCallback cb) {
    g_memReadCallback = cb;
}

void setMemWriteCallback(MemWriteCallback cb) {
    g_memWriteCallback = cb;
}

void setPmpIsReadableCallback(PmpCheckCallback cb) {
    g_pmpReadableCallback = cb;
}

void setPmpIsWritableCallback(PmpCheckCallback cb) {
    g_pmpWritableCallback = cb;
}


bool callMemRead(uint64_t addr, bool bigEndian, uint32_t &data) {
    if (g_memReadCallback)
        return g_memReadCallback(addr, bigEndian, data);
    return false;
}

bool callMemWrite(uint64_t addr, bool bigEndian, uint32_t data) {
    if (g_memWriteCallback)
        return g_memWriteCallback(addr, bigEndian, data);
    return false;
}

bool callPmpIsReadable(uint64_t addr, PrivilegeMode pm) {
    if (g_pmpReadableCallback)
        return g_pmpReadableCallback(addr, pm);
    return true;
}

bool callPmpIsWritable(uint64_t addr, PrivilegeMode pm) {
    if (g_pmpWritableCallback)
        return g_pmpWritableCallback(addr, pm);
    return true;
}

} // namespace WdRiscv
