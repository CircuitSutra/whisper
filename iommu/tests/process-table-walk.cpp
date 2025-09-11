#include <iostream>
#include <cstring>
#include "Iommu.hpp"


using namespace TT_IOMMU;


// -----------------------------------------------------------------------------
// Simple flat memory model
// -----------------------------------------------------------------------------
class MemoryModel
{
public:
  MemoryModel(size_t size)
    : memory(size, 0)
  { }
    
  bool read(uint64_t addr, unsigned size, uint64_t& data) const
  {
    if (addr + size > memory.size())
      {
        assert(0);
        return false;
      }
    data = 0;
    std::memcpy(&data, &memory[addr], size);
    return true;
  }
    
  bool write(uint64_t addr, unsigned size, uint64_t data)
  {
    if (addr + size > memory.size())
      {
        assert(0);
        return false;
      }
    std::memcpy(&memory[addr], &data, size);
    return true;
  }
    
  uint64_t size() const
  { return memory.size(); }


private:
  std::vector<uint8_t> memory;
};


// -----------------------------------------------------------------------------
// Configure DDTP for index calculation
// -----------------------------------------------------------------------------
static void
configureDdtp(Iommu& iommu, uint64_t rootPpn, Ddtp::Mode mode)
{
  Ddtp ddtp;
  ddtp.bits_.mode_ = mode;
  ddtp.bits_.ppn_ = rootPpn;

  iommu.writeCsr(CsrNumber::Ddtp, ddtp.value_);
  
  uint64_t readBack = iommu.readCsr(CsrNumber::Ddtp);
  assert(readBack = ddtp.value_);
}


static void
installMemCbs(Iommu& iommu, MemoryModel& mem)
{
  std::function<bool(uint64_t,unsigned,uint64_t&)> rcb =
    [&mem](uint64_t a, unsigned s, uint64_t& d) { return mem.read(a, s, d); };

  std::function<bool(uint64_t,unsigned,uint64_t)> wcb =
    [&mem](uint64_t a, unsigned s, uint64_t d) { return mem.write(a, s, d); };

  iommu.setMemReadCb(rcb);
  iommu.setMemWriteCb(wcb);
}


/// Setup a device table at the given address for the given mode (level1, level2, or
/// level3) and all the subsequent lower modes (levels) until the leaf level
/// (level1). Setup an entry in each created table corresponding to the indices in the
/// given device id, so that a traversal using the given id will find a leaf entry in the
/// created table. Fill that leaf entry with a copy of the given leaf. Assume all
/// addresses at or above addr are available. Return address past new table (pointer to
/// new top of unused memory). Set leafAddr to the address of the leaf entry of the
/// created table.
static uint64_t
setupDeviceTable(Iommu& iommu, uint64_t addr, unsigned devId, Ddtp::Mode mode,
                 const DeviceContext& leaf, uint64_t& leafAddr)
{
  bool extended = iommu.isDcExtended();

  // Make addr a multiple of page size.
  uint64_t pageSize = iommu.pageSize();
  uint64_t remainder = addr % pageSize;
  if (remainder)
    addr += pageSize - remainder;
  assert((addr % pageSize) == 0);

  uint64_t rootPpn = addr / pageSize;

  configureDdtp(iommu, rootPpn, mode);

  unsigned levels = 0;
  if (mode == Ddtp::Mode::Level3)
    levels = 3;
  else if (mode == Ddtp::Mode::Level2)
    levels = 2;
  else if (mode == Ddtp::Mode::Level1)
    levels = 1;

  while (levels >= 2)
    {
      // Create level3/2 table (1 page) at addr.
      unsigned ddi = Devid{devId}.ithDdi(levels-1, extended); // ddi2 or ddi1
      unsigned entrySize = sizeof(Ddte);
      uint64_t entryAddr = addr + ddi * entrySize;
      assert(entryAddr + entrySize <= addr + pageSize);

      // Fill entry at ddi.  Make it point to the subsequent (levels - 1) table.
      uint64_t pageAddr = addr + pageSize;
      Ddte ddte;
      ddte.bits_.v_ = 1;
      ddte.bits_.ppn_ = pageAddr / pageSize;

      // Write entry to memory.
      iommu.writeDevDirTableEntry(entryAddr, ddte.value_);

      addr += pageSize;  // Free memory is now past created page.
      --levels;  // Next level to be created.
    }

  if (levels == 1)
    {
      // For 1 level, we use ddi[0] as index into the root page.
      unsigned ddi0 = Devid{devId}.ithDdi(0, extended);
      unsigned leafSize = iommu.devDirTableLeafSize(extended);
      leafAddr = addr + ddi0 * leafSize;
      assert(leafAddr + leafSize <= addr + pageSize);

      // Put copy of leaf in memory.
      iommu.writeDeviceContext(leafAddr, leaf);

      addr += pageSize;  // Free memory is now past created page.
    }

  return addr;
}


/// Setup a process table at the page number and with the number of levels specified by
/// the given device context. Setup the entries corresponding to the given process id to
/// that a later process table traversal will lead to a leaf entry. Set that leaf entry to
/// a copy of the given leaf. Return the address past the memory used to set up the
/// process table. This will be address of remainin free memory. We assume that the device
/// table page number points to the top free page.
uint64_t
setupProcessTable(Iommu& iommu, const DeviceContext& dc, unsigned processId,
                  const ProcessContext& leaf, uint64_t& leafAddr)
{
  Procid procid(processId);

  uint64_t pageSize = iommu.pageSize();
  uint64_t addr = dc.pdtpPpn() * pageSize;  // Table root.
  unsigned levels = dc.processTableLevels();

  if (levels == 0)
    return addr;

  // Setup level3 or level2 table or both depending on levels.
  while (levels >= 2)
    {
      // Create level3/2 table (1 page) at addr.
      unsigned pdi = procid.ithPdi(levels-1);  // pdi[2] or pdi[1] for level3 or level2
      unsigned entrySize = sizeof(Pdte);
      uint64_t entryAddr = addr + pdi * entrySize;
      assert(entryAddr + entrySize <= addr + pageSize);

      // Fill entry at pdi.  Make it point to the subsequent (levels-1) table.
      uint64_t nextPageAddr = addr + pageSize;
      Pdte pdte;
      pdte.bits_.v_ = 1;
      pdte.bits_.ppn_ = nextPageAddr / pageSize;

      iommu.writeProcDirTableEntry(dc, entryAddr, pdte.value_);

      --levels;
      addr += pageSize;
    }

  if (levels == 1)
    {
      unsigned pdi0 = procid.ithPdi(0);
      leafAddr = addr + pdi0*sizeof(leaf);
      assert(leafAddr + sizeof(leaf) <= addr + pageSize);

      if (not iommu.writeProcessContext(dc, leafAddr, leaf))
        assert(0);

      addr += pageSize;
    }

  return addr;
}


bool
procTableWalk(uint64_t capabilities, Ddtp::Mode ddtpMode, PdtpMode pdtpMode, bool be)
{
  MemoryModel mem(4 * 1024 * 1024);

  Iommu iommu(0x1000, 0x800, mem.size());
  iommu.configureCapabilities(capabilities);
  iommu.reset();
  installMemCbs(iommu, mem);

  constexpr uint32_t devId = 0x2A;
  uint64_t rootAddr = 0x100 * iommu.pageSize();  // Root addr for device table.
  uint64_t dcAddr = 0;  // Address of leaf entry corresponding to devId.

  DeviceContext leaf;   // Invalid leaf
  uint64_t addr = setupDeviceTable(iommu, rootAddr, devId, ddtpMode, leaf, dcAddr);

  // Make leaf of device tree point to where we will place the process table.
  TransControl tc;
  tc.bits_.v_ = 1;
  tc.bits_.pdtv_ = 1;   // FSC field of leaf holds process directory tree addr
  tc.bits_.sbe_ = be;   // Set endianness for process table

  uint64_t iohgatp = 0;

  Pdtp pdtp;
  pdtp.bits_.ppn_ = addr / iommu.pageSize();
  pdtp.bits_.mode_ = pdtpMode;

  // Write device context leaf to memory.
  auto dc = DeviceContext(tc.value_, iohgatp, 0 /*transAtrrib*/, pdtp.value_);
  if (not iommu.writeDeviceContext(dcAddr, dc))
    assert(0);

  uint32_t procId = 97;
  ProcessContext leafPc;  // Dummy
  uint64_t leafPcAddr = 0;
  addr = setupProcessTable(iommu, dc, procId, leafPc, leafPcAddr);

  // Write a specific process table leaf to memory.
  unsigned cause = 0;
  auto expected = ProcessContext{0x1, 0xdef};  // Arbitrary but valid
  if (not iommu.writeProcessContext(dc, leafPcAddr, expected))
    return false;
  
  bool ok = iommu.loadProcessContext(dc, procId, leafPc, cause);
  assert(expected == leafPc);
  ok = ok and expected == leafPc;

  return ok;
}


int
main()
{
  bool ok = true;

  Capabilities capStruct;

  capStruct.bits_.pd8_ = 1;
  capStruct.bits_.pd17_ = 1;
  capStruct.bits_.pd20_ = 1;
  capStruct.bits_.sv39_ = 1;
  capStruct.bits_.sv39x4_ = 1;
  capStruct.bits_.amoHwad_ = 1;

  uint64_t caps = capStruct.value_;

  // Perform process table walk in all combinations of table levels of the process
  // directory table and the device directory table.
  bool procBe = false;  // Process table is little endian.
  for (Ddtp::Mode dmode : { Ddtp::Mode::Level1, Ddtp::Mode::Level2, Ddtp::Mode::Level3 } )
    for (PdtpMode pmode : { PdtpMode::Pd8, PdtpMode::Pd17, PdtpMode::Pd20 } )
      ok = procTableWalk(caps, dmode, pmode, procBe);

  // Repeat with big endian process table.
  capStruct.bits_.end_ = 1;    // Allow bott big and little endian access.
  caps = capStruct.value_;

  procBe = true;  // Process table is big endian.
  for (Ddtp::Mode dmode : { Ddtp::Mode::Level1, Ddtp::Mode::Level2, Ddtp::Mode::Level3 } )
    for (PdtpMode pmode : { PdtpMode::Pd8, PdtpMode::Pd17, PdtpMode::Pd20 } )
      ok = procTableWalk(caps, dmode, pmode, procBe);

  if (ok)
    std::cerr << "Test passed\n";
  else
    std::cerr << "Test failed\nn";

  return ok? 0 : 1;
}
