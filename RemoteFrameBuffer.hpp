#pragma once

#include <cstdint>
#include <thread>
#include <atomic>
#include "IoDevice.hpp"

namespace WdRiscv
{
  
  class RemoteFrameBuffer : public IoDevice
  {
    public:
      RemoteFrameBuffer(uint64_t addr, uint64_t width, uint64_t height, uint64_t bytes_per_pixel);
      ~RemoteFrameBuffer() override;

      uint32_t read(uint64_t addr) override;
      void write(uint64_t addr, uint32_t value) override;

    private:
      
      void vncServerLoop();

      uint64_t width_;
      uint64_t height_;
      uint64_t bytes_per_pixel_;

      uint32_t* frame_buffer_;

      std::atomic<bool> terminate_ = false;
      std::atomic<bool> frame_buffer_updated_ = true;
      std::thread displayThread_;
  };
}
