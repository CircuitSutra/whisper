#include <cassert>
#include <iostream>
#include <unistd.h>
#include <poll.h>
#include "Uart8250.hpp"


using namespace WdRiscv;


Uart8250::Uart8250(uint64_t addr, uint64_t size, std::shared_ptr<TT_APLIC::Aplic> aplic, uint32_t eiid)
  : IoDevice(addr, size, aplic, eiid)
{
  auto func = [this]() { this->monitorStdin(); };
  stdinThread_ = std::thread(func);

  int fd = fileno(stdin);        // stdin file descriptor
  if (isatty(fd))
    {
      struct termios term;
      tcgetattr(fd, &term);
      termAttr_ = term;  // Save terminal state to restore in destructor.

      cfmakeraw(&term);          // Make terminal raw.
      term.c_lflag &= ~ECHO;     // Turn off echo.
      tcsetattr(fd, 0, &term);
    }
}


Uart8250::~Uart8250()
{
  terminate_ = true;
  stdinThread_.join();

  int fd = fileno(stdin);        // stdin file descriptor

  if (isatty(fd))
    tcsetattr(fd, 0, &termAttr_);  // Restore terminal state.
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
	      int c = static_cast<int>(value & 0xff);
	      if (c)
		{
		  putchar(c);
		  fflush(stdout);
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
Uart8250::monitorStdin()
{
  int fd = fileno(stdin);        // stdin file descriptor
  if (not isatty(fd))
    {
      char c = 0;
      while (::read(fd, &c, sizeof(c) == 1))
        {
          rx_fifo.push(c);

          while (rx_fifo.size() > 1024)
            ;  // Avoid makeing queue too large.
        }
      return;
    }

  struct pollfd inPollfd;

  inPollfd.fd = fd;
  inPollfd.events = POLLIN;

  while (true)
    {
      if (terminate_)
	return;

      int code = poll(&inPollfd, 1, -1);
      if (code == 0)
	continue;   // Timed out.

      if (code == 1)
	{
	  if ((inPollfd.revents & POLLIN) != 0)
	    {
	      std::lock_guard<std::mutex> lock(mutex_);
	      char c;
              auto rc = ::read(fd, &c, sizeof(c));
              if (rc == 1)
                {
                  if (isatty(fd))
                    {
                      static char prev = 0;

                      // Force a stop if control-a x is seen.
                      if (prev == 1 and c == 'x')
                        throw std::runtime_error("Keyboard stop");
                      prev = c;
                    }
                  rx_fifo.push(c);
                  lsr_ |= 1;  // Set least sig bit of line status.
                  iir_ &= ~1;  // Clear bit 0 indicating interrupt is pending.
                  setInterruptPending(true);
                }
              else if (rc == 0)
                continue;
              else
		std::cerr << "Uart8250::monitorStdin: unexpected fail on read\n";
	    }
	}

      // TODO: handle error return codes from poll.
    }
}
