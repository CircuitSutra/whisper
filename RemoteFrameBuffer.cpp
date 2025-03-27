#include <iostream>
#include <rfb/rfb.h>
#include <sys/mman.h>
#include <cassert>
#include <thread>
#include <chrono>

#include "RemoteFrameBuffer.hpp"

using namespace WdRiscv;

// Don't change these
#define RFB_BITS_PER_SAMPLE 8
#define RFB_SAMPLES_PER_PIXEL 3

// Time between RFB updates
#define RFB_FRAME_TIME_US  100000 

RemoteFrameBuffer::RemoteFrameBuffer(uint64_t addr, uint64_t width, uint64_t height, uint64_t bytes_per_pixel)
  : IoDevice(addr, width*height*bytes_per_pixel), width_(width), height_(height), bytes_per_pixel_(bytes_per_pixel) {

  // TODO: either hard code it to be 4 or support 1,2,4 bpp
  assert(bytes_per_pixel == 4 && "bytes per pixel must be 4");

  // each value in the frame buffer represents a pixel
  frame_buffer_ = (uint32_t *) malloc(size());
  if (frame_buffer_ == nullptr) {
    std::cerr << "Failed to allocate frame buffer of size " << size() << "bytes.\n";
    throw std::runtime_error("Out of memory");
  }
  

  auto func = [this]() { this->vncServerLoop(); };
  displayThread_ = std::thread(func);
}

RemoteFrameBuffer::~RemoteFrameBuffer()
{
  if (frame_buffer_)
    {
      munmap(frame_buffer_, size());
      frame_buffer_ = nullptr;
    }

  terminate_ = true;
  displayThread_.join();
}

void
RemoteFrameBuffer::vncServerLoop()
{
  // idk what these are for
  int rfbArgc = 1;
  char programName[] = "whisper";
  char *rfbArgv[] = {programName, nullptr};

  rfbScreenInfoPtr rfbScreen = rfbGetScreen(&rfbArgc, rfbArgv, width_, height_, RFB_BITS_PER_SAMPLE, RFB_SAMPLES_PER_PIXEL, bytes_per_pixel_);
  if (!rfbScreen)
    return;
  rfbScreen->desktopName = "Whisper VNC";
  rfbScreen->frameBuffer = (char*) frame_buffer_;
  rfbScreen->alwaysShared = TRUE;
  rfbScreen->port = 5999;

  rfbInitServer(rfbScreen);

  while(rfbIsActive(rfbScreen) && !terminate_) 
  {
    rfbProcessEvents(rfbScreen, RFB_FRAME_TIME_US);
    if (frame_buffer_updated_) {
        rfbMarkRectAsModified(rfbScreen, 0, 0, width_, height_);
        frame_buffer_updated_ = false;
    }
  }

  rfbScreenCleanup(rfbScreen);

}

uint32_t
RemoteFrameBuffer::read(uint64_t addr)
{
  uint64_t offset = addr - address();
  
  if (offset >= size()) 
    return 0;

  return *(frame_buffer_ + (offset) / 4);
}

void
RemoteFrameBuffer::write(uint64_t addr, uint32_t value)
{
  uint64_t offset = addr - address();

  assert(offset < size() && "RemoteFrameBuffer: Writing outside of buffer range");
  *(frame_buffer_ + (offset) / 4) = value;
  frame_buffer_updated_ = true;
}
