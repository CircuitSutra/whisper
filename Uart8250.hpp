#pragma once

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <poll.h>
#include "IoDevice.hpp"

// Forward declaring termios becausing including termios.h leaks macro
// VSTART that conflicts with VSTART in CsrNumber::VSTART.
struct termios;

namespace WdRiscv
{

  /// A UartChannel represents the other end of the uart
  class UartChannel {
  public:
    virtual ~UartChannel() = default;

    /// Block until bytes are available
    /// Return 0 on EOF
    /// Return number of bytes read and populate buf on success
    virtual size_t read(uint8_t *buf, size_t size) = 0;

    /// Send the given byte
    virtual void write(uint8_t byte) = 0;

    /// Signal to the channel we're terminating. Should unblock any blocked
    /// reads.
    virtual void terminate() {}
  };

  class FDChannel : public UartChannel {
  public:
    FDChannel(int in_fd, int out_fd);
    ~FDChannel() override;

    size_t read(uint8_t *buf, size_t size) override;
    void write(uint8_t byte) override;
    void terminate() override;

  private:
    void restoreTermios();
    
    int in_fd_, out_fd_;
    int terminate_pipe_[2] = {-1, -1};
    struct pollfd pollfds_[2];
    bool is_tty_;
    std::unique_ptr<termios> original_termios_;
    uint8_t prev_ = 0;  // Previous character for control sequence detection
  };


  // This base class is necessary so we can create the PTY before passing the
  // fd to FDChannel's constructor
  class PTYChannelBase {
  public:
    // Do not allow copying because that may cause the PTY fds to be used
    // after close
    PTYChannelBase(const PTYChannelBase&) = delete;
    PTYChannelBase& operator=(const PTYChannelBase&) = delete;

  protected:
    PTYChannelBase();
    ~PTYChannelBase();

    int master_ = -1;
    int slave_ = -1;
  };

  class PTYChannel : private PTYChannelBase, public FDChannel {
    public:
      PTYChannel();
  };

  // Base class to handle accepting a connection from a server socket
  class SocketChannelBase {
  public:
    // Do not allow copying because that may cause the socket fd to be used
    // after close
    SocketChannelBase(const SocketChannelBase&) = delete;
    SocketChannelBase& operator=(const SocketChannelBase&) = delete;

  protected:
    SocketChannelBase(int server_fd);
    ~SocketChannelBase();

    int conn_fd_ = -1;
  };

  // UartChannel implementation using a socket connection
  class SocketChannel : private SocketChannelBase, public FDChannel {
    public:
      SocketChannel(int server_fd);
  };

  class ForkChannel : public UartChannel {
    public:
      ForkChannel(std::unique_ptr<UartChannel> readWriteChannel, std::unique_ptr<UartChannel> writeOnlyChannel);
      size_t read(uint8_t *buf, size_t size) override;
      void write(uint8_t byte) override;
      void terminate() override;

    private:
      std::unique_ptr<UartChannel> readWriteChannel_;
      std::unique_ptr<UartChannel> writeOnlyChannel_;
  };

  class Uart8250 : public IoDevice
  {
  public:

    Uart8250(uint64_t addr, uint64_t size, std::shared_ptr<TT_APLIC::Aplic> aplic, uint32_t iid, std::unique_ptr<UartChannel> channel, bool enableInput = true, unsigned regShift = 2);

    ~Uart8250() override;

    void enable() override;

    void disable() override;

    uint32_t read(uint64_t addr) override;

    void write(uint64_t addr, uint32_t value) override;

    bool saveSnapshot(const std::string& filename) const override;

    bool loadSnapshot(const std::string& filename) override;

  private:
    static const constexpr size_t FIFO_SIZE = 1024;

    std::unique_ptr<UartChannel> channel_;
    unsigned regShift_ = 2;  // Register shift value (default 2 for 4-byte spacing: 1 << 2 = 4)

    /// Update the interrupt status based on the current state of the Uart
    void interruptUpdate();

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

    std::thread inThread_;
    std::atomic<bool> terminate_ = false;
    std::mutex mutex_;   // Synchronize access to rx_fifo with inThread_.
    std::condition_variable cv_; // Wake inThread_ when there is space in the rx_fifo

    std::queue<uint8_t> rx_fifo;
  };
}
