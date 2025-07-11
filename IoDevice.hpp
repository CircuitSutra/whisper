#pragma once

#include <cstdint>
#include <iostream>
#include <string_view>
#include "aplic/Aplic.hpp"

namespace WdRiscv
{

  class IoDevice
  {
  public:

    /// Define a memory mapped io device at the given address using
    /// size bytes of the address space.
    IoDevice(const std::string& type, uint64_t addr, uint64_t size)
      : type_(type), addr_(addr), size_(size), aplic_(nullptr), iid_(0)
    { }

    IoDevice(const std::string& type, uint64_t addr, uint64_t size, std::shared_ptr<TT_APLIC::Aplic> aplic, uint32_t iid)
      : type_(type), addr_(addr), size_(size), aplic_(iid != 0 ? aplic : nullptr), iid_(iid)
    { }

    virtual ~IoDevice() = default;

    /// Read a word from the device. Return 0 if address is outside
    /// the device range.
    virtual uint32_t read(uint64_t addr) = 0;

    /// Write a word to the device. No-op if address is outside the
    /// device range.
    virtual void write(uint64_t addr, uint32_t value) = 0;

    virtual void enable() = 0;

    virtual void disable() = 0;

    virtual bool saveSnapshot(const std::string& filename) const = 0;

    virtual bool loadSnapshot(const std::string& filename) = 0;

    /// Return true if this device has a pending interrupt.
    bool isInterruptPending() const
    {
      if (!aplic_)
        return false;

      return aplic_->getSourceState(iid_);
    }

    /// Mark this device as having/not-having a pending interrupt.
    void setInterruptPending(bool flag)
    {
      if (aplic_)
        aplic_->setSourceState(iid_, flag);
    }

    /// Return true if given address falls within the memory mapped
    /// address range of this device.
    bool isAddressInRange(uint64_t addr) const
    { return addr >= addr_ and addr - addr_ < size_; }

    uint64_t address() const
    { return addr_; }

    uint64_t size() const
    { return size_; }

    std::string_view type() const
    { return type_; }

  private:
    const std::string type_;
    const uint64_t addr_ = 0;
    const uint64_t size_ = 0;

    std::shared_ptr<TT_APLIC::Aplic> aplic_;
    const uint32_t iid_;
  };

}
