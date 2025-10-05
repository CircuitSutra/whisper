#pragma once


#include <vector>
#include <cstring>
#include "virtual_memory/Pte.hpp"


namespace WhisperUtil {

  /// Class for generating a page table walk and populating the page
  /// table given a virtual address and a corresponding physical
  /// address.
  class PageTableMaker
  {
  public:

    enum Mode { Bare = 0, Sv32 = 1, Sv39 = 8, Sv48 = 9, Sv57 = 10, Sv64 = 11 };

    PageTableMaker(uint64_t rootPageAddr, Mode mode = Sv57,
		   uint64_t arenaSize = uint64_t(4)*1024*1024);

    ~PageTableMaker()
    {
      delete [] arena_;
      arena_ = arenaTail_ = nullptr;
      arenaSize_ = 0;
    }

    bool makeWalk(uint64_t virtAddr, uint64_t physAddr,
		  std::vector<uint64_t>& walk);

  protected:

    template <typename VaType, typename PteType>
    bool genericMakeWalk(uint64_t virtAddr, uint64_t physAddr,
			 std::vector<uint64_t>& walk);

    bool allocatePage(uint64_t& pageAddr);

    bool readPte(uint64_t addr, WdRiscv::Pte32& pte) const
    { return readMem(addr, pte.data_); }

    bool readPte(uint64_t addr, WdRiscv::Pte39& pte) const
    { return readMem(addr, pte.data_); }

    bool readPte(uint64_t addr, WdRiscv::Pte48& pte) const
    { return readMem(addr, pte.data_); }

    bool readPte(uint64_t addr, WdRiscv::Pte57& pte) const
    { return readMem(addr, pte.data_); }

    bool writePte(uint64_t addr, WdRiscv::Pte32& pte)
    { return writeMem(addr, pte.data_); }

    bool writePte(uint64_t addr, WdRiscv::Pte39& pte)
    { return writeMem(addr, pte.data_); }

    bool writePte(uint64_t addr, WdRiscv::Pte48& pte)
    { return writeMem(addr, pte.data_); }

    bool writePte(uint64_t addr, WdRiscv::Pte57& pte)
    { return writeMem(addr, pte.data_); }

    bool readMem(uint64_t addr, uint32_t& word) const
    {
      uint64_t offset = addr - rootPageAddr_;
      if (addr < rootPageAddr_ or offset + sizeof(word) - 1 >= arenaSize_)
	return false;
      const auto* ptr = reinterpret_cast<const uint32_t*>(arena_ + offset);
      word = *ptr;
      return true;
    }

    bool readMem(uint64_t addr, uint64_t& doubleWord) const
    {
      uint64_t offset = addr - rootPageAddr_;
      if (addr < rootPageAddr_ or offset + sizeof(doubleWord) - 1 >= arenaSize_)
	return false;
      const auto* ptr = reinterpret_cast<const uint64_t*>(arena_ + offset);
      doubleWord = *ptr;
      return true;
    }

    bool writeMem(uint64_t addr, uint32_t word)
    {
      uint64_t offset = addr - rootPageAddr_;
      if (addr < rootPageAddr_ or offset + sizeof(word) - 1 >= arenaSize_)
	return false;
      auto* ptr = reinterpret_cast<uint32_t*>(arena_ + offset);
      *ptr = word;
      return true;
    }

    bool writeMem(uint64_t addr, uint64_t doubleWord)
    {
      uint64_t offset = addr - rootPageAddr_;
      if (addr < rootPageAddr_ or offset + sizeof(doubleWord) - 1 >= arenaSize_)
	return false;
      auto* ptr = reinterpret_cast<uint64_t*>(arena_ + offset);
      *ptr = doubleWord;
      return true;
    }

  private:

    uint64_t rootPageAddr_ = 0;
    Mode mode_ = Mode::Bare;
    uint64_t arenaSize_ = 0;
    uint8_t* arena_ = nullptr;
    uint8_t* arenaTail_ = nullptr;
    unsigned pageSize_ = 4096;
  };

}

