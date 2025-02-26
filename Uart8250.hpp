#pragma once

#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <poll.h>
#include "IoDevice.hpp"


namespace WdRiscv
{

  /// A UartChannel represents the other end of the uart
  class UartChannel {
  public:
    virtual ~UartChannel() = default;

    /// Block until a byte is available
    /// Return false on failure
    /// Return true on success and the read byte is placed in byte parameter
    virtual bool read(uint8_t& byte) = 0;

    /// Send the given byte
    virtual void write(uint8_t byte) = 0;

    // Does this channel correspond to a TTY?
    virtual bool isTTY() = 0;
  };

  class FDChannel : public UartChannel {
  public:
    FDChannel(int in_fd, int out_fd);

    bool read(uint8_t& byte) override;
    void write(uint8_t byte) override;
    bool isTTY() override;

  private:
    int in_fd_, out_fd_;
    struct pollfd inPollfd;
  };


  class StdIOChannel : public FDChannel {
  public:
    StdIOChannel();
  };

  class Uart8250 : public IoDevice
  {
  public:

    Uart8250(uint64_t addr, uint64_t size, std::shared_ptr<TT_APLIC::Aplic> aplic, uint32_t eiid, std::unique_ptr<UartChannel> channel);

    ~Uart8250() override;

    uint32_t read(uint64_t addr) override;

    void write(uint64_t addr, uint32_t value) override;

  private:
    std::unique_ptr<UartChannel> channel_;

    /// This runs in its own thread. It monitors the standard input and
    /// marks interrupt pending when input is possible placing the input
    /// character in the rx_fifo for the Uart to consme.
    void monitorInput();

    uint8_t ier_ = 0;     // Interrupt enable
    uint8_t iir_ = 1;     // Interrupt id
    uint8_t lcr_ = 0;     // Line control
    uint8_t mcr_ = 0;     // Modem control
    uint8_t lsr_ = 0x60;  // Line satus
    uint8_t msr_ = 0;     // Modem status
    uint8_t scr_ = 0;     // Scratch
    uint8_t fcr_ = 0;     // Fifo control
    uint8_t dll_ = 0x1;   // Divisor latch lsb
    uint8_t dlm_ = 0x1;   // Divisor latch msb
    uint8_t psd_ = 0;     // Pre-scaler division

    std::thread stdinThread_;
    std::atomic<bool> terminate_ = false;
    std::mutex mutex_;   // Synchronize access to byte_ with stdinThread_.
    std::queue<uint8_t> rx_fifo;
  };
}
