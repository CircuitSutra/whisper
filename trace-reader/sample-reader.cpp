#include "TraceReader.hpp"


// NOLINTBEGIN(cppcoreguidelines-owning-memory, cppcoreguidelines-pro-bounds-pointer-arithmetic)
int
main(int argc, char* argv[])
{
  using namespace WhisperUtil;

  TraceReader* reader = nullptr;

  if (argc < 2)
    return 0;

  // usage: sample-reader <trace-file> [<initial-state-file>]

  std::string traceFile = argv[1];
  std::string initialFile = argc > 2 ? argv[2] : "";

  TraceReader reader{traceFile, initialFile};

  if (not reader)
    {
      std::cerr << "Error: Failed to open " << traceFile << " for input.\n";
      return 1;
    }

  TraceRecord record;

  std::vector<uint64_t> walk;

  //reader->definePageTableMaker(0x100000000, WhisperUtil::PageTableMaker::Sv57, 4*1024*1024);

  while (reader.nextRecord(record))
    {
      reader.printRecord(std::cout, record);
    }

  if (not reader.eof())
    return 1;
  return 0;
}
// NOLINTEND(cppcoreguidelines-owning-memory, cppcoreguidelines-pro-bounds-pointer-arithmetic)
