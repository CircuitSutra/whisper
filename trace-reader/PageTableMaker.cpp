#include "PageTableMaker.hpp"


// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)


using namespace WhisperUtil;


PageTableMaker::PageTableMaker(uint64_t rootPageAddr, Mode mode, uint64_t arenaSize)
  : rootPageAddr_(rootPageAddr), mode_(mode), arenaSize_(arenaSize)
{
  if (arenaSize_ < pageSize_)
    arenaSize_ = pageSize_;
  arena_ = new uint8_t[arenaSize];
  memset(arena_, 0, pageSize_);
  arenaTail_ = arena_ + pageSize_;
}


PageTableMaker::~PageTableMaker()
{
  delete [] arena_;
  arena_ = arenaTail_ = nullptr;
  arenaSize_ = 0;
}


bool
PagetTableMakser::allocatePage(uint64_t& pageAddr);
{
  if ((pageAddr % pageSize_) != 0)
    return false;
  uint64_t offset = arenaTail_ - arena_;
  if (offset + pageSize_ > arenaSize_)
    return false;
  memset(arenaTail_, 0, pageSize_);
  pageAddr = rootPageAddr_ + offset;
  arenaTail_ += pageSize_;
  return true;
}


bool
PageTableMaker::makeWalk(uint64_t vaddr, uint64_t paddr, std::vector<uint64_t>& walk)
{
  using namespace WdRiscv;

  walk.clear();
  if (mode_ == Sv32)
    return genericMakeWalk<Va32, Pte32>(vaddr, paddr, walk);
  if (mode_ == Sv39)
    return genericMakeWalk<Va39, Pte39>(vaddr, paddr, walk);
  if (mode_ == Sv48)
    return genericMakeWalk<Va48, Pte48>(vaddr, paddr, walk);
  if (mode_ == Sv57)
    return genericMakeWalk<Va57, Pte57>(vaddr, paddr, walk);
  return true;
}


template <typename VaType, typename PteType>
bool
PageTableMaker::genericMakeWalk(uint64_t virtAddr, uint64_t physAddr,
				std::vector<uint64_t>& walk)
{
  unsigned pteSize = PteType::size();
  uint64_t nodeAddr = rootPageAddr_;  // Current page in walk.
  unsigned pageShift = 12;

  walk.clear();

  VaType va(virtAddr);
  unsigned levels = PteType::levels();
  for (unsigned i = 0; i < levels; ++i)
    {
      walk.push_back(nodeAddr);

      uint64_t vpn = va.vpn(levels - 1 - i);
      uint64_t pteAddr = nodeAddr + vpn*pteSize;

      PteType pte{0};
      if (not readPte(pteAddr, pte))
	return false;
      if (not pte.valid())
	{
	  uint64_t nextNode = 0;
	  if (not allocatePage(nextNode))
	    return false;
	  pte.bits_.valid_ = true;
	  if (i + 1 < levels)
	    {
	      pte.bits_.read_ = pte.bits_.write_ = pte.bits_.exec_ = false;
	      pte.setPpn(nextNode >> pageShift);
	    }
	  else
	    {
	      pte.bits_.read_ = pte.bits_.write_ = pte.bits_.exec_ = true;
	      pte.setPpn(physAddr >> pageShift);
	    }
	  if (not writePte(pteAddr, pte))
	    return false;
	}
      if (pte.leaf())
	break;
      nodeAddr = pte.ppn() << pageShift;
    }

  return true;
}


// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
