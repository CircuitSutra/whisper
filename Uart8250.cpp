#include <cassert>
#include <iostream>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
#include "Uart8250.hpp"


using namespace WdRiscv;

FDChannel::FDChannel(int in_fd, int out_fd)
  : in_fd_(in_fd), out_fd_(out_fd)
{
  if (pipe(terminate_pipe_))
    throw std::runtime_error("FDChannel: Failed to get termination pipe\n");

  pollfds_[0].fd = in_fd_;
  pollfds_[0].events = POLLIN;
  pollfds_[1].fd = terminate_pipe_[0];
  pollfds_[1].events = POLLIN;

  if (isatty(in_fd_)) {
    struct termios term;
    tcgetattr(in_fd_, &term);
    cfmakeraw(&term);
    term.c_lflag &= ~ECHO;
    tcsetattr(in_fd_, 0, &term);
  }
}

size_t FDChannel::read(uint8_t *arr, size_t n) {
  while (true) {
    int code = poll(pollfds_, 2, 10);

    if (code > 0)
    {
      // Terminated
      if ((pollfds_[1].revents & POLLIN))
        return 0;

      if ((pollfds_[0].revents & POLLIN) != 0)
      {
        ssize_t count = ::read(in_fd_, arr, n);
        std::cout << "Read " << count << " bytes from in_fd_\n";
        if (count < 0)
          throw std::runtime_error("FDChannel: Failed to read from in_fd_\n");

        if (isatty(in_fd_))
          for (size_t i = 0; i < static_cast<size_t>(count); i++) {
            static uint8_t prev = 0;
            const uint8_t c = arr[i];

            // Force a stop if control-a x is seen.
            if (prev == 1 and c == 'x')
              throw std::runtime_error("Keyboard stop");
            prev = c;
          }

        return count;
      }
    }

    if (code == 0)
      // Timeout
      continue;

    // TODO: handle error return codes from poll.
    return 0;
  }
}

void FDChannel::write(uint8_t byte) {
  int written;
  do {
    written = ::write(out_fd_, &byte, 1);
  } while (written != 1 && written != -1);

  if (written == -1)
    throw std::runtime_error("FDChannel error writing to output\n");
}


void FDChannel::terminate() {
  const uint8_t byte = 0;
  if (::write(terminate_pipe_[1], &byte, 1) != 1)
    std::cerr << "Info: FDChannel::terminate: write failed\n";
}

FDChannel::~FDChannel() {
  for (uint8_t i = 0; i < 2; i++) {
    if (terminate_pipe_[i] != -1)
      close(terminate_pipe_[i]);
  }
}

PTYChannelBase::PTYChannelBase() {
  char name[256];
  if (openpty(&master_, &slave_, name, nullptr, nullptr) < 0)
    throw std::runtime_error("Failed to open a PTY\n");

  std::cerr << "Got PTY " << name << "\n";
}

PTYChannelBase::~PTYChannelBase()
{
  if (master_ != -1)
    close(master_);

  if (slave_ != -1)
    close(slave_);
}


PTYChannel::PTYChannel() : PTYChannelBase(), FDChannel(master_, master_)
{ }

ForkChannel::ForkChannel(std::unique_ptr<UartChannel> readWriteChannel, std::unique_ptr<UartChannel> writeOnlyChannel)
  : readWriteChannel_(std::move(readWriteChannel)), writeOnlyChannel_(std::move(writeOnlyChannel)) {}

size_t ForkChannel::read(uint8_t *buf, size_t size) {
  return readWriteChannel_->read(buf, size);
}

void ForkChannel::write(uint8_t byte) {
  readWriteChannel_->write(byte);
  writeOnlyChannel_->write(byte);
}

void ForkChannel::terminate() {
  readWriteChannel_->terminate();
  writeOnlyChannel_->terminate();
}

Uart8250::Uart8250(uint64_t addr, uint64_t size,
    std::shared_ptr<TT_APLIC::Aplic> aplic, uint32_t iid,
    std::unique_ptr<UartChannel> channel, bool enableInput)
  : IoDevice("uart8250", addr, size, aplic, iid), channel_(std::move(channel))
{
  if (enableInput)
    this->enableInput();
}

Uart8250::~Uart8250()
{
  terminate_ = true;
  channel_->terminate();
  if (inThread_.joinable())
    inThread_.join();
}

void Uart8250::enableInput() {
  auto func = [this]() { this->monitorInput(); };
  inThread_ = std::thread(func);
}

void Uart8250::interruptUpdate() {
  uint32_t initialIir = iir_;
  iir_ &= ~0xf;
  if ((ier_ & 0x01) && (lsr_ & 0x01)) {
    iir_ |= 0x04; // Receive interrupt pending
  } else if ((ier_ & 0x02) && (lsr_ & 0x20)) {
    iir_ |= 0x02; // Transmitter Holding Register Empty Interrupt
  } else {
    iir_ |= 0x01; // No interrupt pending
  }

  if ((initialIir & 1) != (iir_ & 1)) {
    setInterruptPending(!(iir_ & 1));
  }
}

uint32_t Uart8250::read(uint64_t addr) {
  uint64_t offset = (addr - address()) / 4;
  bool dlab = lcr_ & 0x80;

  if (dlab == 0) {
    switch (offset) {
      case 0: {
        std::unique_lock<std::mutex> lock(mutex_);
        uint32_t res = 0;
        if (!rx_fifo.empty()) {
          res = rx_fifo.front();
          rx_fifo.pop();
        }
        if (rx_fifo.empty())
          lsr_ &= ~1; // Clear least sig bit of line status.
        lock.unlock();
        cv_.notify_all();
        return res;
      }
      case 1: return ier_;
      case 2: {
	uint32_t iir = iir_;
	interruptUpdate();
	return iir;
      }
      case 3: return lcr_;
      case 4: return mcr_;
      case 5: return lsr_;
      case 6: return msr_;
      case 7: return scr_;
    }
  } else {
    switch (offset) {
      case 0: return dll_;
      case 1: return dlm_;
    }
  }

  assert(0);
  return 0;
}

void Uart8250::write(uint64_t addr, uint32_t value) {
  uint64_t offset = (addr - address()) / 4;
  bool dlab = lcr_ & 0x80;

  if (dlab == 0) {
    switch (offset) {
      case 0: {
        uint8_t byte = value;
        if (byte) {
          channel_->write(byte);
        }
        interruptUpdate();
      }
      break;
      case 1: {
        ier_ = value;
        interruptUpdate();
      } break;
      case 2: fcr_ = value; break;
      case 3: lcr_ = value; break;
      case 4: mcr_ = value; break;
      case 5:
      case 6: break;
      case 7: scr_ = value; break;
      default:
        /* std::cerr << "Uart writing addr 0x" << std::hex << addr << std::dec << '\n'; */
        assert(0);
    }
  } else {
    switch (offset) {
      case 0: dll_ = value; break;
      case 1: dlm_ = value; break;
      case 3: lcr_ = value; break;
      case 5: psd_ = value; break;
      default:
        /* std::cerr << "Uart writing addr 0x" << std::hex << addr << std::dec << '\n'; */
        assert(0);
    }
  }
}



void Uart8250::monitorInput() {
  while (true) {
    if (terminate_)
      return;

    std::array<uint8_t, FIFO_SIZE> arr;
    size_t count = channel_->read(arr.data(), FIFO_SIZE);
    if (count == 0)
      // EOF
      return;

    std::unique_lock<std::mutex> lock(mutex_);

    size_t i = 0;
    do {
      if (terminate_)
        return;

      for (; i < count && rx_fifo.size() < FIFO_SIZE; i++) {
        rx_fifo.push(arr[i]);
      }

      lsr_ |= 1;  // Set receiver data ready
      interruptUpdate();

      if (rx_fifo.size() >= FIFO_SIZE)
        // Block until rx_fifo has space
        cv_.wait(lock);
    } while (i != count);
  }
}
