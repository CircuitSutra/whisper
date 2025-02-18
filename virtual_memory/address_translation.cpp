#include "address_translation.hpp"

namespace WdRiscv {

    bool callMemRead(const VirtMem* vm, uint64_t addr, bool bigEndian, uint64_t &data) {
        const auto &cb = vm->getMemReadCallback();
        return cb ? cb(addr, bigEndian, data) : false;
    }

    bool callMemWrite(const VirtMem* vm, uint64_t addr, bool bigEndian, uint64_t data) {
        const auto &cb = vm->getMemWriteCallback();
        return cb ? cb(addr, bigEndian, data) : false;
    }

    bool callPmpIsReadable(const VirtMem* vm, uint64_t addr, PrivilegeMode pm) {
        const auto &cb = vm->getPmpReadableCallback();
        return cb ? cb(addr, pm) : true;
    }

    bool callPmpIsWritable(const VirtMem* vm, uint64_t addr, PrivilegeMode pm) {
        const auto &cb = vm->getPmpWritableCallback();
        return cb ? cb(addr, pm) : true;
    }

} // namespace WdRiscv
