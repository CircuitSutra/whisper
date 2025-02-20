#pragma once

#include <cstdint>
#include <iostream>
#include "aplic/Aplic.hpp"

namespace WdRiscv
{

  class IoDevice
  {
  public:

    /// Define a memory mapped io device at the given address using
    /// size bytes of the address space.
    IoDevice(uint64_t addr, uint64_t size)
      : addr_(addr), size_(size), aplic_(nullptr), eiid_(0)
    { }

    IoDevice(uint64_t addr, uint64_t size, std::shared_ptr<TT_APLIC::Aplic> aplic, uint32_t eiid)
      : addr_(addr), size_(size), aplic_(eiid != 0 ? aplic : nullptr), eiid_(eiid)
    { }

    virtual ~IoDevice() = default;

    /// Read a word from the device. Return 0 if address is outside
    /// the device range.
    virtual uint32_t read(uint64_t addr) = 0;

    /// Write a word to the device. No-op if address is outside the
    /// device range.
    virtual void write(uint64_t addr, uint32_t value) = 0;

    /// Return true if this device has a pending interrupt.
    bool isInterruptPending() const
    { 
      if (!aplic_)
        return false;

      return aplic_->getSourceState(eiid_);
    }

    /// Mark this device as having/not-having a pending interrupt.
    void setInterruptPending(bool flag)
    { 
      if (aplic_)
        aplic_->setSourceState(eiid_, flag);
    }

    /// Return true if given address falls within the memory mapped
    /// address range of this device.
    bool isAddressInRange(uint64_t addr) const
    { return addr >= addr_ and addr - addr_ < size_; }

    uint64_t address() const
    { return addr_; }

    uint64_t size() const
    { return size_; }

  private:
    uint64_t addr_ = 0;
    uint64_t size_ = 0;

    std::shared_ptr<TT_APLIC::Aplic> aplic_;
    uint32_t eiid_;
  };

}
