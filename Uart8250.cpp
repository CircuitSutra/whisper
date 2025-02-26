#include <cassert>
#include <iostream>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include "Uart8250.hpp"


using namespace WdRiscv;

FDChannel::FDChannel(int in_fd, int out_fd)
  : in_fd_(in_fd), out_fd_(out_fd)
{
  inPollfd.fd = in_fd_;
  inPollfd.events = POLLIN;
}

bool FDChannel::read(uint8_t& byte) {
  int code = poll(&inPollfd, 1, -1);

  if (code == 0)
    return false;

  if (code == 1)
  {
    if ((inPollfd.revents & POLLIN) != 0)
    {
      uint8_t c;
      if (::read(in_fd_, &c, sizeof(c)) != 1)
	std::cerr << "FDChannel: unexpected fail on read\n";
      if (isatty(in_fd_))
      {
	static uint8_t prev = 0;

	// Force a stop if control-a x is seen.
	if (prev == 1 and c == 'x')
	  throw std::runtime_error("Keyboard stop");
	prev = c;
      }

      byte = c;
      return true;
    }
  }

  // TODO: handle error return codes from poll.
  return false;
}

void FDChannel::write(uint8_t byte) {
  int written;
  do {
    written = ::write(out_fd_, &byte, 1);
  } while (written != 1 && written != -1);

  if (written == -1) {
    std::cerr << "FDChannel error writing to output\n";
    assert(0);
  }
}

bool FDChannel::isTTY() {
  return isatty(in_fd_);
}

StdIOChannel::StdIOChannel()
  : FDChannel(fileno(stdin), fileno(stdout))
{
  struct termios term;
  tcgetattr(fileno(stdin), &term);
  cfmakeraw(&term);
  term.c_lflag &= ~ECHO;
  tcsetattr(fileno(stdin), 0, &term);
}

Uart8250::Uart8250(uint64_t addr, uint64_t size,
    std::shared_ptr<TT_APLIC::Aplic> aplic, uint32_t eiid,
    std::unique_ptr<UartChannel> channel)
  : IoDevice(addr, size, aplic, eiid), channel_(std::move(channel))
{
  auto func = [this]() { this->monitorInput(); };
  stdinThread_ = std::thread(func);
}

Uart8250::~Uart8250()
{
  terminate_ = true;
  stdinThread_.join();
}

uint32_t
Uart8250::read(uint64_t addr)
{
  uint64_t offset = (addr - address()) / 4;
  bool dlab = lcr_ & 0x80;

  if (dlab == 0)
    {
      switch (offset)
	{
	case 0:
	  {
	    std::lock_guard<std::mutex> lock(mutex_);
	    uint32_t res = 0;
	    if (!rx_fifo.empty()) {
	      res = rx_fifo.front();
	      rx_fifo.pop();
	    }
	    if (rx_fifo.empty()) {
	      lsr_ &= ~1;  // Clear least sig bit
	      iir_ |= 1;   // Set least sig bit indicating no interrupt.
	      setInterruptPending(false);
	    }
	    return res;
	  }

	case 1: return ier_;
	case 2: return iir_;
	case 3: return lcr_;
	case 4: return mcr_;
	case 5: return lsr_;
	case 6: return msr_;
	case 7: return scr_;
	}
    }
  else
    {
      switch (offset)
	{
	case 0: return dll_;
	case 1: return dlm_;
	}
    }

  assert(0);
  return 0;
}


void
Uart8250::write(uint64_t addr, uint32_t value)
{
  uint64_t offset = (addr - address()) / 4;
  bool dlab = lcr_ & 0x80;

  if (dlab == 0)
    {
      switch (offset)
	{
	case 0:
	    {
	      uint8_t byte = value;
	      if (byte)
		{
		  channel_->write(byte);
		}
	    }
	  break;

	case 1: ier_ = value; break;
	case 2: fcr_ = value; break;
	case 3: lcr_ = value; break;
	case 4: mcr_ = value; break;
	case 5:
	case 6: break;
	case 7: scr_ = value; break;
	default:
	  std::cerr << "Uart writing addr 0x" << std::hex << addr << std::dec << '\n';
	  assert(0);
	}
    }
  else
    {
      switch (offset)
	{
	case 0: dll_ = value; break;
	case 1: dlm_ = value; break;
	case 3: lcr_ = value; break;
	case 5: psd_ = value; break;
	default:
	  std::cerr << "Uart writing addr 0x" << std::hex << addr << std::dec << '\n';
	  assert(0);
	}
    }
}


void
Uart8250::monitorInput()
{
  if (!channel_->isTTY())
    {
      uint8_t c = 0;
      while (channel_->read(c))
      {
	rx_fifo.push(c);

	while (rx_fifo.size() > 1024)
	  ;  // Avoid makeing queue too large.
      }
      return;
    }

  while (true)
  {
    if (terminate_)
      return;

    uint8_t c;
    if (!channel_->read(c))
      continue;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      rx_fifo.push(c);

      lsr_ |= 1;  // Set least sig bit of line status.
      iir_ &= ~1;  // Clear bit 0 indicating interrupt is pending.
      setInterruptPending(true);
    }
  }
}
