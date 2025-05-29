// Copyright 2020 Western Digital Corporation or its affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iomanip>
#include <iostream>
#include <fstream>
#include <sstream>
#include <boost/algorithm/string.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include "Filesystem.hpp"
#include "Hart.hpp"
#include "Core.hpp"
#include "SparseMem.hpp"
#include "System.hpp"
#include "Mcm.hpp"
#include "PerfApi.hpp"
#include "Uart8250.hpp"
#include "Uartsf.hpp"
#include "pci/virtio/Blk.hpp"


using namespace WdRiscv;


inline bool
isPowerOf2(uint64_t x)
{
  return x != 0 and (x & (x-1)) == 0;
}


template <typename URV>
System<URV>::System(unsigned coreCount, unsigned hartsPerCore,
                    unsigned hartIdOffset, size_t memSize,
                    size_t pageSize)
  : hartCount_(coreCount * hartsPerCore), hartsPerCore_(hartsPerCore),
    imsicMgr_((coreCount * hartsPerCore), pageSize), time_(0)
{
  cores_.resize(coreCount);

  memory_ = std::make_unique<Memory>(memSize, pageSize);
  syscall_ = std::make_unique<Syscall<URV>>(sysHarts_, memSize);
  sparseMem_ = nullptr;

  Memory& mem = *memory_;
  mem.setHartCount(hartCount_);

  for (unsigned ix = 0; ix < coreCount; ++ix)
    {
      URV coreHartId = ix * hartIdOffset;
      cores_.at(ix) = std::make_shared<CoreClass>(coreHartId, ix, hartsPerCore, mem, *syscall_, time_);

      // Maintain a vector of all the harts in the system.  Map hart-id to index
      // of hart in system.
      auto core = cores_.at(ix);
      for (unsigned i = 0; i < hartsPerCore; ++i)
        {
          auto hart = core->ithHart(i);
          sysHarts_.push_back(hart);
          URV hartId = coreHartId + i;
          unsigned hartIx = ix*hartsPerCore + i;
          hartIdToIndex_[hartId] = hartIx;
        }
    }

#ifdef MEM_CALLBACKS
  sparseMem_ = std::make_unique<SparseMem>();

  auto readf = [this](uint64_t addr, unsigned size, uint64_t& value) -> bool {
                 return sparseMem_->read(addr, size, value); };

  auto writef = [this](uint64_t addr, unsigned size, uint64_t value) -> bool {
                  return sparseMem_->write(addr, size, value); };

  auto initf = [this](uint64_t addr, const std::span<uint8_t> buffer) -> bool {
                 return sparseMem_->initializePage(addr, buffer); };

  mem.defineReadMemoryCallback(readf);
  mem.defineWriteMemoryCallback(writef);
  mem.defineInitPageCallback(initf);
#endif
}

template <typename URV>
bool
System<URV>::defineUart(const std::string& type, uint64_t addr, uint64_t size,
    uint32_t iid, const std::string& channel_type)
{
  std::shared_ptr<IoDevice> dev;


  auto createChannel = [](std::string_view channel_type) -> std::unique_ptr<UartChannel> {
    auto createChannelImpl = [](std::string_view channel_type, auto &createChannelImpl) -> std::unique_ptr<UartChannel> {
      constexpr std::string_view unixPrefix = "unix:";

      auto pos = channel_type.find(';');
      if (pos != std::string::npos)
      {
        std::string_view readWriteChannelType = channel_type.substr(0, pos);
        std::string_view writeOnlyChannelType = channel_type.substr(pos + 1);

        std::unique_ptr<UartChannel> readWriteChannel = createChannelImpl(readWriteChannelType, createChannelImpl);
        std::unique_ptr<UartChannel> writeOnlyChannel = createChannelImpl(writeOnlyChannelType, createChannelImpl);

        return std::make_unique<ForkChannel>(std::move(readWriteChannel), std::move(writeOnlyChannel));
      }
      else if (channel_type == "stdio")
        return std::make_unique<FDChannel>(fileno(stdin), fileno(stdout));
      else if (channel_type == "pty")
        return std::make_unique<PTYChannel>();
      else if (channel_type.find(unixPrefix, 0) == 0)
      {
        std::string filename(channel_type.substr(unixPrefix.length()));
        if (filename.empty()) {
          std::cerr << "Error: System::defineUart: Missing filename for unix socket channel\n";
          return nullptr;
        }

        int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (server_fd < 0) {
          perror("System::defineUart: Failed to create unix socket");
          return nullptr;
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, filename.c_str(), sizeof(addr.sun_path) - 1);

        // Remove existing socket file if present before binding
        unlink(filename.c_str());

        if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
          perror("System::defineUart: Failed to bind unix socket");
          close(server_fd);
          return nullptr;
        }

        if (listen(server_fd, 1) < 0) {
          perror("System::defineUart: Failed to listen on unix socket");
          close(server_fd);
          return nullptr;
        }

        std::cerr << "Error: System::defineUart: Listening on unix socket: " << filename << "\n";
        std::unique_ptr<SocketChannel> channel;
        try {
          channel = std::make_unique<SocketChannel>(server_fd);
        } catch (const std::runtime_error& e) {
          std::cerr << "Error: System::defineUart: Failed to create SocketChannel: " << e.what() << "\n";
          close(server_fd);
          unlink(filename.c_str());
          return nullptr;
        }

        // SocketChannel constructor called accept(), server fd no longer needed here.
        close(server_fd);
        unlink(filename.c_str());

        return channel;
      }
      else
      {
        std::cerr << "Error: System::defineUart: Invalid channel type: " << channel_type << "\n"
          << "Valid channels: stdio, pty, unix:<server socket path>, or a"
          << "semicolon separated list of those.\n";
        return nullptr;
      }
    };
    return createChannelImpl(channel_type, createChannelImpl);
  };

  if (type == "uartsf")
    dev = std::make_shared<Uartsf>(addr, size);
  else if (type == "uart8250")
  {
    std::unique_ptr<UartChannel> channel = createChannel(channel_type);
    if (!channel)
      return false;
    dev = std::make_shared<Uart8250>(addr, size, aplic_, iid, std::move(channel), false);
  }
  else
  {
    std::cerr << "Error: System::defineUart: Invalid uart type: " << type << "\n";
    return false;
  }

  memory_->registerIoDevice(dev);
  ioDevs_.push_back(std::move(dev));

  return true;
}

template <typename URV>
System<URV>::~System()
{
  // Final MCM checks
  if (mcm_)
    for (const auto& hartPtr : sysHarts_)
      mcm_->finalChecks(*hartPtr);

  // Write back binary files were marked for update.
  for (auto bf : binaryFiles_)
    {
      auto path = std::get<0>(bf);
      uint64_t addr = std::get<1>(bf);
      uint64_t size = std::get<2>(bf);
      std::cerr << "Error: Updating " << path << " from addr: 0x" << std::hex << addr
		<< std::dec << " size: " << size << '\n';
      FILE* file = fopen(path.c_str(), "w");
      if (not file)
	{
	  std::cerr << "Error: Failed to open " << path << " for update\n";
	  continue;
	}
      for (uint64_t i = 0; i < size; ++i)
	{
	  uint8_t byte = 0;
	  memory_->peek(addr + i, byte, false);
	  fputc(byte, file);
	}
      fclose(file);
    }
}


template <typename URV>
void
System<URV>::checkUnmappedElf(bool flag)
{
  if (memory_)
    memory_->checkUnmappedElf(flag);
}


template <typename URV>
bool
System<URV>::writeAccessedMemory(const std::string& path) const
{
  if (not sparseMem_)
    return false;
  return sparseMem_->writeHexFile(path);
}


template <typename URV>
bool
System<URV>::loadElfFiles(const std::vector<std::string>& files, bool raw, bool verbose)
{
  unsigned registerWidth = sizeof(URV)*8;
  uint64_t end = 0, entry = 0;
  uint64_t gp = 0, tp = 0; // gp: global-pointer, tp: thread-pointer
  unsigned errors = 0;
  ElfSymbol sym;

  for (const auto& file : files)
    {
      if (verbose)
	std::cerr << "Error: Loading ELF file " << file << '\n';
      uint64_t end0 = 0, entry0 = 0;
      if (not memory_->loadElfFile(file, registerWidth, entry0, end0))
	errors++;
      else
	{
	  if (not entry)
	    entry = entry0;

	  if (memory_->findElfSymbol("_end", sym))   // For newlib/linux emulation.
	    end = std::max(end, sym.addr_);
	  else
	    end = std::max(end, end0);

	  if (not gp and memory_->findElfSymbol("__global_pointer$", sym))
	    gp = sym.addr_;

	  if (not tp and memory_->findElfSection(".tdata", sym))
	    tp = sym.addr_;
	}
    }

  for (const auto& hart : sysHarts_)
    {
      if (not toHostSym_.empty() and memory_->findElfSymbol(toHostSym_, sym))
	hart->setToHostAddress(sym.addr_);
      if (not fromHostSym_.empty() and memory_->findElfSymbol(fromHostSym_, sym))
	hart->setFromHostAddress(sym.addr_, true);
      if (not consoleIoSym_.empty() and memory_->findElfSymbol(consoleIoSym_, sym))
	hart->setConsoleIo(URV(sym.addr_));

      if (verbose)
	std::cerr << "Error: Setting program break to 0x" << std::hex << end << std::dec << '\n';
      hart->setTargetProgramBreak(end);

      if (not raw)
	{
	  if (not hart->peekIntReg(RegGp) and gp)
	    {
              if (verbose)
                std::cerr << "Error: Setting register gp to 0x" << std::hex << gp << std::dec << '\n';
	      hart->pokeIntReg(RegGp, URV(gp));
	    }
	  if (not hart->peekIntReg(RegTp) and tp)
	    {
              if (verbose)
                std::cerr << "Error: Setting register tp to 0x" << std::hex << tp << std::dec << '\n';
	      hart->pokeIntReg(RegTp, URV(tp));
	    }
	  if (entry)
	    {
	      if (verbose)
                std::cerr << "Error: Setting PC to 0x" << std::hex << entry << std::dec << '\n';
	      hart->pokePc(URV(entry));
	    }
	}
    }

  return errors == 0;
}


template <typename URV>
bool
System<URV>::loadHexFiles(const std::vector<std::string>& files, bool verbose)
{
  unsigned errors = 0;
  for (const auto& file : files)
    {
      if (verbose)
	std::cerr << "Error: Loading HEX file " << file << '\n';
      if (not memory_->loadHexFile(file))
	errors++;
    }
  return errors == 0;
}


static bool
binaryFileParams(std::string spec, uint64_t defOffset, std::string& filename, uint64_t &offset, bool &update)
{
  using std::cerr;
  // Split filespec around colons. Spec format: <file>,
  // <file>:<offset>, or <file>:<offset>:u
  std::vector<std::string> parts;
  boost::split(parts, spec, boost::is_any_of(":"));

  filename = parts.at(0);
  offset = defOffset;
  update = false;

  if (parts.empty())
    {
      std::cerr << "Error: Empty binary file name\n";
      return false;
    }

  filename = parts.at(0);

  if (parts.size() > 1)
    {
      std::string offsStr = parts.at(1);
      if (offsStr.empty())
	cerr << "Error: Empty binary file offset: " << spec << '\n';
      else
	{
	  char* tail = nullptr;
	  offset = strtoull(offsStr.c_str(), &tail, 0);
	  if (tail and *tail)
	    {
	      cerr << "Error: Invalid binary file offset: " << spec << '\n';
	      return false;
	    }
	}
    }
  else
    cerr << "Error: Binary file " << filename << " does not have an address, will use address 0x"
	 << std::hex << offset << std::dec << '\n';

  if (parts.size() > 2)
    {
      if (parts.at(2) != "u")
	{
	  cerr << "Error: Invalid binary file attribute: " << spec << '\n';
	  return false;
	}
      update = true;
    }

  return true;
}


template <typename URV>
bool
System<URV>::loadBinaryFiles(const std::vector<std::string>& fileSpecs,
			     uint64_t defOffset, bool verbose)
{
  using std::cerr;
  unsigned errors = 0;

  for (const auto& spec : fileSpecs)
    {

      std::string filename;
      uint64_t offset;
      bool update;

      if (!binaryFileParams(spec, defOffset, filename, offset, update))
        {
	  errors++;
	  continue;
        }

      if (verbose)
	cerr << "Error: Loading binary " << filename << " at address 0x" << std::hex
	     << offset << std::dec << '\n';

      if (not memory_->loadBinaryFile(filename, offset))
	{
	  errors++;
	  continue;
	}

      if (update)
	{
	  uint64_t size = Filesystem::file_size(filename);
	  BinaryFile bf = { filename, offset, size };
	  binaryFiles_.push_back(bf);
	}
    }

  return errors == 0;
}


#if LZ4_COMPRESS
template <typename URV>
bool
System<URV>::loadLz4Files(const std::vector<std::string>& fileSpecs,
			  uint64_t defOffset, bool verbose)
{
  using std::cerr;
  unsigned errors = 0;

  for (const auto& spec : fileSpecs)
    {
      std::string filename;
      uint64_t offset;
      bool update;

      if (!binaryFileParams(spec, defOffset, filename, offset, update))
        {
	  errors++;
	  continue;
        }

      if (update)
	{
	  cerr << "Error: Updating not supported on lz4 files, ignoring " << filename << '\n';
	  errors++;
	  continue;
	}

      if (verbose)
	cerr << "Error: Loading lz4 compressed file " << filename << " at address 0x" << std::hex
	     << offset << std::dec << '\n';

      if (not memory_->loadLz4File(filename, offset))
	{
	  errors++;
	  continue;
	}

      if (update)
	{
	  uint64_t size = Filesystem::file_size(filename);
	  BinaryFile bf = { filename, offset, size };
	  binaryFiles_.push_back(bf);
	}
    }

  return errors == 0;
}
#endif


static
bool
saveUsedMemBlocks(const std::string& filename,
		  std::vector<std::pair<uint64_t, uint64_t>>& blocks)
{
  std::ofstream ofs(filename, std::ios::trunc);
  if (not ofs)
    {
      std::cerr << "Error: saveUsedMemBlocks failed - cannot open "
                << filename << " for write\n";
      return false;
    }
  for (auto& it: blocks)
    ofs << it.first << " " << it.second << "\n";
  return true;
}


static
bool
saveTime(const std::string& filename, uint64_t time)
{
  std::ofstream ofs(filename, std::ios::trunc);
  if (not ofs)
    {
      std::cerr << "Error: saveTime failed - cannot open "
                << filename << " for write\n";
      return false;
    }
  ofs << std::dec << time << "\n";
  return true;
}


template <typename URV>
bool
System<URV>::saveSnapshot(const std::string& dir)
{
  Filesystem::path dirPath = dir;
  if (not Filesystem::is_directory(dirPath))
    if (not Filesystem::create_directories(dirPath))
      {
	std::cerr << "Error: Failed to create snapshot directory " << dir << '\n';
	return false;
      }

  uint64_t minSp = ~uint64_t(0);
  for (auto hartPtr : sysHarts_)
    {
      std::string name = "registers";
      if (hartCount_ > 1)
	name += std::to_string(hartPtr->sysHartIndex());
      Filesystem::path regPath = dirPath / name;
      if (not hartPtr->saveSnapshotRegs(regPath.string()))
	return false;

      URV sp = 0;
      if (not hartPtr->peekIntReg(IntRegNumber::RegSp, sp))
	assert(0 && "Error: Assertion failed");
      minSp = std::min(minSp, uint64_t(sp));
    }

  auto& hart0 = *ithHart(0);
  auto& syscall = hart0.getSyscall();

  Filesystem::path usedBlocksPath = dirPath / "usedblocks";
  std::vector<std::pair<uint64_t,uint64_t>> usedBlocks;
  if (sparseMem_)
    sparseMem_->getUsedBlocks(usedBlocks);
  else
    syscall.getUsedMemBlocks(minSp, usedBlocks);

  if (not saveUsedMemBlocks(usedBlocksPath.string(), usedBlocks))
    return false;

  Filesystem::path timePath = dirPath / "time";
  if (not saveTime(timePath.string(), time_))
    return false;

  Filesystem::path memPath = dirPath / "memory";
  if (not memory_->saveSnapshot(memPath.string(), usedBlocks))
    return false;

  Filesystem::path fdPath = dirPath / "fd";
  if (not syscall.saveFileDescriptors(fdPath.string()))
    return false;

  Filesystem::path mmapPath = dirPath / "mmap";
  if (not syscall.saveMmap(mmapPath.string()))
    return false;

  Filesystem::path dtracePath = dirPath / "data-lines";
  if (not memory_->saveDataAddressTrace(dtracePath))
    return false;

  Filesystem::path itracePath = dirPath / "instr-lines";
  if (not memory_->saveInstructionAddressTrace(itracePath))
    return false;

  Filesystem::path branchPath = dirPath / "branch-trace";
  if (not hart0.saveBranchTrace(branchPath))
    return false;

  Filesystem::path imsicPath = dirPath / "imsic";
  if (not imsicMgr_.saveSnapshot(imsicPath))
    return false;

  return true;
}


static
bool
loadUsedMemBlocks(const std::string& filename,
		  std::vector<std::pair<uint64_t, uint64_t>>& blocks)
{
  blocks.clear();
  std::ifstream ifs(filename);
  if (not ifs)
    {
      std::cerr << "Error: loadUsedMemBlocks failed - cannot open "
                << filename << " for read\n";
      return false;
    }

  std::string line;
  while (std::getline(ifs, line))
    {
      std::istringstream iss(line);
      uint64_t addr, length;
      iss >> addr;
      iss >> length;
      blocks.emplace_back(addr, length);
    }

  return true;
}


static
bool
loadTime(const std::string& filename, uint64_t& time)
{
  std::ifstream ifs(filename);
  if (not ifs)
    {
      std::cerr << "Error: loadTime failed - cannot open "
                << filename << " for read\n";
      return false;
    }

  std::string line;
  std::getline(ifs, line);
  uint64_t val = strtoull(line.c_str(), nullptr, 0);
  time = val;
  return true;
}


template <typename URV>
bool
System<URV>::configImsic(uint64_t mbase, uint64_t mstride,
			 uint64_t sbase, uint64_t sstride,
			 unsigned guests, const std::vector<unsigned>& idsVec,
                         const std::vector<unsigned>& tmVec, // Threshold masks
                         bool maplic, bool saplic, bool gaplic, bool trace)
{
  using std::cerr;

  size_t ps = pageSize();

  if ((mbase % ps) != 0)
    {
      cerr << "Error: IMISC mbase (0x" << std::hex << mbase << ") is not"
	   << " a multiple of page size (0x" << ps << ")\n" << std::dec;
      return false;
    }

  if (mstride == 0)
    {
      cerr << "Error: IMSIC mstride must not be zero.\n";
      return false;
    }

  if ((mstride % ps) != 0)
    {
      cerr << "Error: IMISC mstride (0x" << std::hex << mstride << ") is not"
	   << " a multiple of page size (0x" << ps << ")\n" << std::dec;
      return false;
    }

  if (sstride)
    {
      if ((sbase % ps) != 0)
	{
	  cerr << "Error: IMISC sbase (0x" << std::hex << sbase << ") is not"
	       << " a multiple of page size (0x" << ps << ")\n" << std::dec;
	  return false;
	}

      if ((sstride % ps) != 0)
	{
	  cerr << "Error: IMISC sstride (0x" << std::hex << sstride << ") is not"
	       << " a multiple of page size (0x" << ps << ")\n" << std::dec;
	  return false;
	}
    }

  if (guests and sstride < (guests + 1)*ps)
    {
      cerr << "Error: IMISC supervisor stride (0x" << std::hex << sstride << ") is"
	   << " too small for configured guests (" << std::dec << guests << ").\n";
      return false;
    }

  if (mstride and sstride)
    {
      unsigned hc = hartCount();
      uint64_t mend = mbase + hc*mstride, send = sbase + hc*sstride;
      if ((sbase > mbase and sbase < mend) or
	  (send > mbase and send < mend))
	{
	  cerr << "Error: IMSIC machine file address range overlaps that of supervisor.\n";
	  return false;
	}
    }

  if (idsVec.size() != 3)
    {
      cerr << "Error: IMSIC interrupt-ids array size (" << idsVec.size() << ") is "
	   << "invalid -- Expecting 3.\n";
      return false;
    }

  for (auto ids : idsVec)
    {
      if ((ids % 64) != 0)
	{
	  cerr << "Error: IMSIC interrupt id limit (" << ids << ") is not a multiple of 64.\n";
	  return false;
	}

      if (ids > 2048)
	{
	  cerr << "Error: IMSIC interrupt id limit (" << ids << ") is larger than 2048.\n";
	  return false;
	}
    }

  if (idsVec.size() != tmVec.size())
    {
      cerr << "Error: IMSIC interrupt ids count (" << idsVec.size() << ") is different "
	   << " thant the threshold-mask count (" << tmVec.size() << ")\n";
      return false;
    }

  for (size_t i = 0; i < idsVec.size(); ++i)
    {
      if (tmVec.at(i) < idsVec.at(i) - 1)
	{
	  cerr << "Error: Threshold mask (" << tmVec.at(0) << ") cannot be less than the "
	       << "max interrupt id (" << (idsVec.at(i) - 1) << ").\n";
	  return false;
	}
    }

  bool ok = imsicMgr_.configureMachine(mbase, mstride, idsVec.at(0), tmVec.at(0), maplic);
  ok = imsicMgr_.configureSupervisor(sbase, sstride, idsVec.at(1), tmVec.at(1), saplic) and ok;
  ok = imsicMgr_.configureGuests(guests, idsVec.at(2), tmVec.at(2), gaplic) and ok;
  if (not ok)
    {
      cerr << "Error: Failed to configure IMSIC.\n";
      return false;
    }

  uint64_t mend = mbase + mstride * hartCount();
  uint64_t send = sbase + sstride * hartCount();

  auto readFunc = [this](uint64_t addr, unsigned size, uint64_t& data) -> bool {
    return this->imsicMgr_.read(addr, size, data);
  };

  auto writeFunc = [this](uint64_t addr, unsigned size, uint64_t data) -> bool {
    return  this->imsicMgr_.write(addr, size, data);
  };

  for (unsigned i = 0; i < hartCount(); ++i)
    {
      auto hart = ithHart(i);
      auto imsic = imsicMgr_.ithImsic(i);
      hart->attachImsic(imsic, mbase, mend, sbase, send, readFunc, writeFunc, trace);
    }

  return true;
}


template <typename URV>
bool
System<URV>::configAplic(unsigned num_sources, std::span<const TT_APLIC::DomainParams> domain_params)
{
  aplic_ = std::make_shared<TT_APLIC::Aplic>(hartCount_, num_sources, domain_params);

  TT_APLIC::DirectDeliveryCallback aplicCallback = [this] (unsigned hartIx, TT_APLIC::Privilege privilege, bool interState) -> bool {
    bool is_machine = privilege == TT_APLIC::Machine;
    std::cerr << "Error: Delivering interrupt hart=" << hartIx << " privilege="
              << (is_machine ? "machine" : "supervisor")
              << " interrupt-state=" << (interState? "on" : "off") << '\n';
    // if an IMSIC is present, then interrupt should only be delivery if its eidelivery is 0x40000000
    auto& hart = *ithHart(hartIx);
    auto imsic = hart.imsic();
    if (imsic)
      {
        unsigned eidelivery = is_machine ? imsic->machineDelivery() : imsic->supervisorDelivery();
        if (eidelivery != 0x40000000)
          {
            std::cerr << "Error: Cannot deliver interrupt; for direct delivery mode, IMSIC's eidelivery must be 0x40000000\n";
            return false;
          }
      }
    auto mip = hart.peekCsr(CsrNumber::MIP);
    mip = hart.overrideWithMvip(mip);
    int xeip = is_machine ? 11 : 9;
    if (interState)
      mip |= 1 << xeip;
    else
      mip &= ~(1 << xeip);
    return hart.pokeCsr(CsrNumber::MIP, mip);
  };
  aplic_->setDirectCallback(aplicCallback);

  TT_APLIC::MsiDeliveryCallback imsicFunc = [this] (uint64_t addr, uint32_t data) -> bool {
    return imsicMgr_.write(addr, 4, data);
  };
  aplic_->setMsiCallback(imsicFunc);

  for (auto& hart : sysHarts_)
    hart->attachAplic(aplic_);

  return true;
}


template <typename URV>
bool
System<URV>::configPci(uint64_t configBase, uint64_t mmioBase, uint64_t mmioSize, unsigned buses, unsigned slots)
{
  if (mmioBase - configBase < (1ULL << 28))
    {
      std::cerr << "Error: PCI config space typically needs 28bits to fully cover entire region" << std::endl;
      return false;
    }

  pci_ = std::make_shared<Pci>(configBase, (1ULL << 28), mmioBase, mmioSize, buses, slots);

  auto readf = [this](uint64_t addr, size_t size, uint64_t& data) -> bool {
    bool ok = false;
    if (size == 1)
      {
        uint8_t tmp = 0;
        ok = this->memory_->peek(addr, tmp, false);
        data = tmp;
      }
    if (size == 2)
      {
        uint16_t tmp = 0;
        ok = this->memory_->peek(addr, tmp, false);
        data = tmp;
      }
    if (size == 4)
      {
        uint32_t tmp = 0;
        ok = this->memory_->peek(addr, tmp, false);
        data = tmp;
      }
    if (size == 8)
      return this->memory_->peek(addr, data, false);
    return ok;
  };

  auto writef = [this](uint64_t addr, size_t size, uint64_t data) -> bool {
    bool ok = false;
    if (size == 1)
      ok = this->memory_->poke(addr, uint8_t(data), false);
    if (size == 2)
      ok = this->memory_->poke(addr, uint16_t(data), false);
    if (size == 4)
      ok = this->memory_->poke(addr, uint32_t(data), false);
    if (size == 8)
      ok = this->memory_->poke(addr, data, false);
    return ok;
  };

  auto msif = [this](uint64_t addr, unsigned size, uint64_t data) -> bool {
    return this->imsicMgr_.write(addr, size, data);
  };

  pci_->define_read_mem(readf);
  pci_->define_write_mem(writef);
  pci_->define_msi(msif);

  for (auto& hart : sysHarts_)
    hart->attachPci(pci_);
  return true;
}


template <typename URV>
bool
System<URV>::addPciDevices(const std::vector<std::string>& devs)
{
  if (not pci_)
    {
      std::cerr << "Error: Please specify a PCI region in the json" << std::endl;
      return false;
    }

  for (const auto& devStr : devs)
    {
      std::vector<std::string> tokens;
      boost::split(tokens, devStr, boost::is_any_of(":"), boost::token_compress_on);

      if (tokens.size() < 3)
        {
          std::cerr << "Error: PCI device string should have at least 3 fields" << std::endl;
          return false;
        }

      std::string name = tokens.at(0);
      unsigned bus = std::stoi(tokens.at(1));
      unsigned slot = std::stoi(tokens.at(2));

      if (name == "virtio-blk")
        {
          if (not (tokens.size() == 4))
            {
              std::cerr << "Error: virtio-blk requires backing input file" << std::endl;
              return false;
            }

          std::shared_ptr<Blk> dev = std::make_shared<Blk>(false);
          if (not dev->open_file(tokens.at(3)))
            return false;

          if (not pci_->register_device(dev, bus, slot))
            return false;
        }
      else
        return false;
    }
  return true;
}


template <typename URV>
bool
System<URV>::enableMcm(unsigned mbLineSize, bool mbLineCheckAll,
		       const std::vector<unsigned>& enabledPpos)
{
  if (mbLineSize != 0)
    if (not isPowerOf2(mbLineSize) or mbLineSize > 512)
      {
	std::cerr << "Error: Invalid merge buffer line size: "
		  << mbLineSize << '\n';
	return false;
      }

  mcm_ = std::make_shared<Mcm<URV>>(this->hartCount(), pageSize(), mbLineSize);
  mbSize_ = mbLineSize;
  mcm_->setCheckWholeMbLine(mbLineCheckAll);

  mcm_->enablePpo(false);

  for (auto ppoIx : enabledPpos)
    if (ppoIx < Mcm<URV>::PpoRule::Limit)
      {
	typedef typename Mcm<URV>::PpoRule Rule;
	Rule rule = Rule(ppoIx);
	mcm_->enablePpo(rule, true);
      }

  for (auto& hart :  sysHarts_)
    hart->setMcm(mcm_);

  return true;
}


template <typename URV>
bool
System<URV>::enableMcm(unsigned mbLineSize, bool mbLineCheckAll, bool enablePpos)
{
  if (mbLineSize != 0)
    if (not isPowerOf2(mbLineSize) or mbLineSize > 512)
      {
	std::cerr << "Error: Invalid merge buffer line size: "
		  << mbLineSize << '\n';
	return false;
      }

  mcm_ = std::make_shared<Mcm<URV>>(this->hartCount(), pageSize(), mbLineSize);
  mbSize_ = mbLineSize;
  mcm_->setCheckWholeMbLine(mbLineCheckAll);

  typedef typename Mcm<URV>::PpoRule Rule;

  for (unsigned ix = 0; ix < Rule::Io; ++ix)   // Temporary: Disable IO rule.
    {
      Rule rule = Rule(ix);
      mcm_->enablePpo(rule, enablePpos);
    }

  for (auto& hart :  sysHarts_)
    hart->setMcm(mcm_);

  return true;
}


template <typename URV>
void
System<URV>::endMcm()
{
  if (mcm_)
    {
      auto& path = memory_->dataLineTracePath();
      if (not path.empty())
        {
          bool skipClean = true;
          bool includeValues = true;
          memory_->saveDataAddressTrace(path, skipClean, includeValues);

          std::string emptyPath;
          memory_->enableDataLineTrace(emptyPath);  // Disable
        }
    }

  for (auto& hart :  sysHarts_)
    hart->setMcm(nullptr);
  mcm_ = nullptr;
}


template <typename URV>
bool
System<URV>::enablePerfApi(std::vector<FILE*>& traceFiles)
{
  if constexpr (sizeof(URV) == 4)
    {
      std::cerr << "Error: Performance model API is not supported for RV32\n";
      return false;
    }
  else
    {
      perfApi_ = std::make_shared<TT_PERF::PerfApi>(*this);
      for (auto& hart : sysHarts_)
	hart->setPerfApi(perfApi_);
      perfApi_->enableTraceLog(traceFiles);
    }

  return true;
}


template <typename URV>
void
System<URV>::enableTso(bool flag)
{
  if (mcm_)
    mcm_->enableTso(flag);
}


template <typename URV>
bool
System<URV>::mcmRead(Hart<URV>& hart, uint64_t time, uint64_t tag, uint64_t addr,
		     unsigned size, uint64_t data, unsigned elemIx, unsigned field)
{
  if (not mcm_)
    return false;
  return mcm_->readOp(hart, time, tag, addr, size, data, elemIx, field);
}


template <typename URV>
bool
System<URV>::mcmMbWrite(Hart<URV>& hart, uint64_t time, uint64_t addr,
			const std::vector<uint8_t>& data,
			const std::vector<bool>& mask, bool skipCheck)
{
  if (not mcm_)
    return false;
  return mcm_->mergeBufferWrite(hart, time, addr, data, mask, skipCheck);
}


template <typename URV>
bool
System<URV>::mcmMbInsert(Hart<URV>& hart, uint64_t time, uint64_t tag, uint64_t addr,
                         unsigned size, uint64_t data, unsigned elem, unsigned field)
{
  if (not mcm_)
    return false;
  return mcm_->mergeBufferInsert(hart, time, tag, addr, size, data, elem, field);
}


template <typename URV>
bool
System<URV>::mcmBypass(Hart<URV>& hart, uint64_t time, uint64_t tag, uint64_t addr,
                       unsigned size, uint64_t data, unsigned elem, unsigned field)
{
  if (not mcm_)
    return false;
  return mcm_->bypassOp(hart, time, tag, addr, size, data, elem, field);
}


template <typename URV>
bool
System<URV>::mcmIFetch(Hart<URV>& hart, uint64_t /*time*/, uint64_t addr)
{
  if (not mcm_)
    return false;
  return hart.mcmIFetch(addr);
}


template <typename URV>
bool
System<URV>::mcmIEvict(Hart<URV>& hart, uint64_t /*time*/, uint64_t addr)
{
  if (not mcm_)
    return false;
  return hart.mcmIEvict(addr);
}


template <typename URV>
bool
System<URV>::mcmRetire(Hart<URV>& hart, uint64_t time, uint64_t tag,
		       const DecodedInst& di, bool trapped)
{
  if (not mcm_)
    return false;
  return mcm_->retire(hart, time, tag, di, trapped);
}


template <typename URV>
bool
System<URV>::mcmSkipReadDataCheck(uint64_t addr, unsigned size, bool enable)
{
  if (not mcm_)
    return false;
  mcm_->skipReadDataCheck(addr, size, enable);
  return true;
}


template <typename URV>
void
System<URV>::perfApiCommandLog(FILE* log)
{
  if (not perfApi_)
    return;
  perfApi_->enableCommandLog(log);
}


template <typename URV>
void
System<URV>::perfApiTraceLog(std::vector<FILE*>& files)
{
  if (not perfApi_)
    return;
  perfApi_->enableTraceLog(files);
}


template <typename URV>
bool
System<URV>::perfApiFetch(unsigned hart, uint64_t time, uint64_t tag, uint64_t vpc)
{
  if (not perfApi_)
    return false;

  bool trap; ExceptionCause cause; uint64_t trapPc;
  return perfApi_->fetch(hart, time, tag, vpc, trap, cause, trapPc);
}


template <typename URV>
bool
System<URV>::perfApiDecode(unsigned hart, uint64_t time, uint64_t tag)
{
  if (not perfApi_)
    return false;
  return perfApi_->decode(hart, time, tag);
}


template <typename URV>
bool
System<URV>::perfApiExecute(unsigned hart, uint64_t time, uint64_t tag)
{
  if (not perfApi_)
    return false;
  return perfApi_->execute(hart, time, tag);
}


template <typename URV>
bool
System<URV>::perfApiRetire(unsigned hart, uint64_t time, uint64_t tag)
{
  if (not perfApi_)
    return false;
  return perfApi_->retire(hart, time, tag);
}


template <typename URV>
bool
System<URV>::perfApiDrainStore(unsigned hart, uint64_t time, uint64_t tag)
{
  if (not perfApi_)
    return false;
  return perfApi_->drainStore(hart, time, tag);
}


template <typename URV>
bool
System<URV>::perfApiPredictBranch(unsigned hart, uint64_t /*time*/, uint64_t tag,
				  bool taken, uint64_t target)
{
  if (not perfApi_)
    return false;
  return perfApi_->predictBranch(hart, tag, taken, target);
}


template <typename URV>
bool
System<URV>::perfApiFlush(unsigned hart, uint64_t time, uint64_t tag)
{
  if (not perfApi_)
    return false;
  return perfApi_->flush(hart, time, tag);
}


template <typename URV>
bool
System<URV>::perfApiShouldFlush(unsigned hart, uint64_t time, uint64_t tag, bool& flush,
				uint64_t& addr)
{
  flush = false;
  if (not perfApi_)
    return false;
  return perfApi_->shouldFlush(hart, time, tag, flush, addr);
}


template <typename URV>
bool
System<URV>::produceTestSignatureFile(std::string_view outPath) const
{
  // Find the begin_signature and end_signature section addresses
  // from the elf file
  WdRiscv::ElfSymbol beginSignature, endSignature;
  for (auto&& [symbolName, pSymbol] : { std::pair{ "begin_signature", &beginSignature },
                                        std::pair{ "end_signature",   &endSignature } })
    {
      if (not findElfSymbol(symbolName, *pSymbol))
        {
          std::cerr << "Error: Failed to find symbol " << symbolName << " in memory.\n";
          return false;
        }
    }

  if (beginSignature.addr_ > endSignature.addr_)
    {
      std::cerr << "Error: Ending address for signature file is before starting address.\n";
      return false;
    }

  // Get all of the data between the two sections and store it in a vector to ensure
  // it can all be read correctly.
  std::vector<uint32_t> data;
  data.reserve((endSignature.addr_ - beginSignature.addr_) / 4);
  for (std::size_t addr = beginSignature.addr_; addr < endSignature.addr_; addr += 4)
    {
      uint32_t value;
      if (not memory_->peek(addr, value, true))
        {
          std::cerr << "Error: Unable to read data at address 0x" << std::hex << addr << ".\n";
          return false;
        }

      data.push_back(value);
    }

  // If all data has been read correctly, write it as 32-bit hex values with one
  // per line.
  std::ofstream outFile(outPath.data());
  outFile << std::hex << std::setfill('0');
  for (uint32_t value : data)
    {
      outFile << std::setw(8) << value << '\n';
    }

  return true;
}


template <typename URV>
bool
System<URV>::getSparseMemUsedBlocks(std::vector<std::pair<uint64_t, uint64_t>>& usedBlocks) const
{
  if (sparseMem_)
    {
      sparseMem_->getUsedBlocks(usedBlocks);
      return true;
    }
  return false;
}


extern void forceUserStop(int);


template <typename URV>
bool
System<URV>::batchRun(std::vector<FILE*>& traceFiles, bool waitAll, uint64_t stepWinLo, uint64_t stepWinHi)
{
  auto forceSnapshot = [this]() -> void {
      uint64_t tag = ++snapIx_;
      std::string pathStr = snapDir_ + std::to_string(tag);
      Filesystem::path path = pathStr;
      if (not Filesystem::is_directory(path) and
	  not Filesystem::create_directories(path))
        {
          std::cerr << "Error: Failed to create snapshot directory " << pathStr << '\n';
          std::cerr << "Error: Continuing...\n";
        }

      if (not saveSnapshot(pathStr))
        {
          std::cerr << "Error: Failed to save a snapshot\n";
          std::cerr << "Error: Continuining...\n";
        }
  };

  if (hartCount() == 0)
    return true;

  while (true)
    {
      struct {
        bool prog = false;
        bool stop = false;
      } snap;

      std::atomic<bool> result = true;

      if (hartCount() == 1)
        {
          auto& hart = *ithHart(0);
          try
            {
              result = hart.run(traceFiles.at(0));
#if FAST_SLOPPY
              hart.reportOpenedFiles(std::cout);
#endif
            }
          catch (const CoreException& ce)
            {
              if (ce.type() == CoreException::Type::Snapshot or
                  ce.type() == CoreException::Type::SnapshotAndStop)
                snap.prog = true;
              snap.stop = ce.type() != CoreException::Type::Snapshot;
            }
        }
      else if (not stepWinLo and not stepWinHi)
        {
          // Run each hart in its own thread.
          std::vector<std::thread> threadVec;
          std::atomic<unsigned> finished = 0;  // Count of finished threads.

          auto threadFunc = [&result, &finished, &snap] (Hart<URV>* hart, FILE* traceFile) {
                              try
                                {
                                  bool r = hart->run(traceFile);
                                  result = result and r;
                                }
                              catch (const CoreException& ce)
                                {
                                  if (ce.type() == CoreException::Type::Snapshot or
                                      ce.type() == CoreException::Type::SnapshotAndStop)
                                    snap.prog = true;
                                  snap.stop = ce.type() != CoreException::Type::Snapshot;
                                }
                              finished++;
                            };

          for (unsigned i = 0; i < hartCount(); ++i)
            {
              Hart<URV>* hart = ithHart(i).get();
              threadVec.emplace_back(std::thread(threadFunc, hart, traceFiles.at(i)));
            }

          if (not waitAll)
            {
              // First thread to finish terminates run.
              while (finished == 0)
                sleep(1);
              forceUserStop(0);
            }

          for (auto& t : threadVec)
            {
              if (snap.prog)
                forceUserStop(0);
              t.join();
            }
        }
      else
        {
          // Run all harts in one thread round-robin.
          const uint64_t stepWindow = stepWinHi - stepWinLo + 1;
          unsigned finished = 0;
          std::vector<bool> stopped(sysHarts_.size(), false);

          for (auto hptr : sysHarts_)
            finished += hptr->hasTargetProgramFinished();

          while ((waitAll and finished != hartCount()) or
                 (not waitAll and finished == 0))
            {
              for (auto hptr : sysHarts_)
                {
                  unsigned ix = hptr->sysHartIndex();
                  if (stopped.at(ix))
                    continue;

                  // step N times
                  unsigned steps = (rand() % stepWindow) + stepWinLo;
                  try
                    {
                      bool stop;
                      result = hptr->runSteps(steps, stop, traceFiles.at(ix)) and result;
                      stopped.at(ix) = stop;
                    }
                  catch (const CoreException& ce)
                    {
                      if (ce.type() == CoreException::Type::Snapshot or
                          ce.type() == CoreException::Type::SnapshotAndStop)
                        snap.prog = true;
                      snap.stop = ce.type() != CoreException::Type::Snapshot;
                    }
                  if (stopped.at(ix))
                    finished++;
                }

              if (snap.prog)
                break;
            }
        }

      if (snap.prog)
        {
          forceSnapshot();
          if (snap.stop)
            return result;
        }
      else
        return result;
    }
}


/// Run producing a snapshot after each snapPeriod instructions. Each
/// snapshot goes into its own directory names <dir><n> where <dir> is
/// the string in snapDir and <n> is a sequential integer starting at
/// 0. Return true on success and false on failure.
template <typename URV>
bool
System<URV>::snapshotRun(std::vector<FILE*>& traceFiles, const std::vector<uint64_t>& periods)
{
  if (hartCount() == 0)
    return true;

  Hart<URV>& hart0 = *ithHart(0);

  uint64_t globalLimit = hart0.getInstructionCountLimit();

  for (size_t ix = 0; true; ++ix)
    {
      uint64_t nextLimit = globalLimit;
      if (not periods.empty())
	{
	  if (periods.size() == 1)
	    nextLimit = hart0.getInstructionCount() + periods.at(0);
	  else
	    nextLimit = ix < periods.size() ? periods.at(ix) : globalLimit;
	}

      nextLimit = std::min(nextLimit, globalLimit);

      uint64_t tag = 0;
      if (periods.size() > 1)
	tag = ix < periods.size() ? periods.at(ix) : nextLimit;
      else
        tag = ++snapIx_;
      std::string pathStr = snapDir_ + std::to_string(tag);
      Filesystem::path path = pathStr;
      if (not Filesystem::is_directory(path) and
	  not Filesystem::create_directories(path))
	{
	  std::cerr << "Error: Failed to create snapshot directory " << pathStr << '\n';
	  return false;
	}

      for (auto hartPtr : sysHarts_)
	hartPtr->setInstructionCountLimit(nextLimit);

      batchRun(traceFiles, true /*waitAll*/, 0 /*stepWindowLo*/, 0 /*stepWindowHi*/);

      bool done = false;
      for (auto& hartPtr : sysHarts_)
	if (hartPtr->hasTargetProgramFinished() or nextLimit >= globalLimit)
	  {
	    done = true;
	    Filesystem::remove_all(path);
	    break;
	  }
      if (done)
	break;

      if (not saveSnapshot(pathStr))
	{
	  std::cerr << "Error: Failed to save a snapshot\n";
	  return false;
	}
    }

  // Incremental branch traces are in snapshot directories. Turn off
  // branch tracing to prevent top-level branch tracing file from
  // being generated since the data will be for the last snapshot and
  // not for the whole run. Same is done for instruction and data line
  // tracing.
  for (auto hartPtr : sysHarts_)
    {
      hartPtr->traceBranches(std::string(), 0);
      std::string emptyPath;
      memory_->enableDataLineTrace(emptyPath);
      memory_->enableInstructionLineTrace(emptyPath);
    }

  return true;
}


template <typename URV>
bool
System<URV>::loadSnapshot(const std::string& snapDir, bool restoreTrace)
{
  using std::cerr;

  if (not Filesystem::is_directory(snapDir))
    {
      cerr << "Error: Path is not a snapshot directory: " << snapDir << '\n';
      return false;
    }

  if (hartCount_ == 0)
    {
      cerr << "Error: System::loadSnapshot: System with no harts\n";
      return false;
    }

  Filesystem::path dirPath = snapDir;

  // Restore the register values.
  for (auto hartPtr : sysHarts_)
    {
      unsigned ix = hartPtr->sysHartIndex();

      std::string name = "registers" + std::to_string(ix);

      Filesystem::path regPath = dirPath / name;
      bool missing = not Filesystem::is_regular_file(regPath);
      if (missing and ix == 0 and hartCount_ == 1)
	{
	  // Support legacy snapshots where hart index was not appended to filename.
	  regPath = dirPath / "registers";
	  missing = not Filesystem::is_regular_file(regPath);
	}
      if (missing)
	{
	  cerr << "Error: Snapshot file does not exists: " << regPath << '\n';
	  return false;
	}

      if (not hartPtr->loadSnapshotRegs(regPath.string()))
	return false;
    }


  Filesystem::path usedBlocksPath = dirPath / "usedblocks";
  std::vector<std::pair<uint64_t,uint64_t>> usedBlocks;
  if (not loadUsedMemBlocks(usedBlocksPath.string(), usedBlocks))
    return false;

  auto& hart0 = *ithHart(0);

  Filesystem::path timePath = dirPath / "time";
  if (not loadTime(timePath.string(), time_))
    {
      std::cerr << "Error: Using instruction count for time\n";
      time_ = hart0.getInstructionCount();  // Legacy snapshots.
    }

  auto& syscall = hart0.getSyscall();
  Filesystem::path mmapPath = dirPath / "mmap";
  if (not syscall.loadMmap(mmapPath.string()))
    return false;

  if (restoreTrace)
    {
      Filesystem::path dtracePath = dirPath / "data-lines";
      if (not memory_->loadDataAddressTrace(dtracePath))
        return false;

      Filesystem::path itracePath = dirPath / "instr-lines";
      if (not memory_->loadInstructionAddressTrace(itracePath))
        return false;

      Filesystem::path branchPath = dirPath / "branch-trace";
      if (not hart0.loadBranchTrace(branchPath))
        return false;
    }

  Filesystem::path memPath = dirPath / "memory";
  if (not memory_->loadSnapshot(memPath.string(), usedBlocks))
    return false;

  // Rearm CLINT time compare.
  for (auto hartPtr : sysHarts_)
    {
      uint64_t mtimeCmpBase = 0;
      if (hartPtr->hasAclintTimeCompare(mtimeCmpBase))
	{
          uint64_t timeCmpAddr = mtimeCmpBase + hartPtr->sysHartIndex() * 8;
	  uint64_t timeCmp = 0;
	  memory_->peek(timeCmpAddr, timeCmp, false);
	  hartPtr->setAclintAlarm(timeCmp);
	}
    }

  Filesystem::path fdPath = dirPath / "fd";
  if (not syscall.loadFileDescriptors(fdPath.string()))
    return false;

  Filesystem::path imsicPath = dirPath / "imsic";
  if (not imsicMgr_.loadSnapshot(imsicPath))
    return false;

  return true;
}


template class WdRiscv::System<uint32_t>;
template class WdRiscv::System<uint64_t>;
