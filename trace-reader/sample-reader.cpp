#include "TraceReader.hpp"


// NOLINTBEGIN(cppcoreguidelines-owning-memory, cppcoreguidelines-pro-bounds-pointer-arithmetic)
int
main(int argc, char* argv[])
{
  using namespace WhisperUtil;

  TraceReader* reader = nullptr;

  if (argc > 2)
    {
      reader = new TraceReader(argv[1], argv[2]);
    }
  else if (argc > 1)
    {
      reader = new TraceReader(argv[1]);
    }
  else
    {
      return 0;
    }

  if (not *reader)
    {
      std::cerr << "Error: Failed to open " << argv[1] << " for input.\n";
      return 1;
    }

  TraceRecord record;

  std::vector<uint64_t> walk;

  //reader->definePageTableMaker(0x100000000, WhisperUtil::PageTableMaker::Sv57, 4*1024*1024);

  while (reader->nextRecord(record))
    {
      reader->printRecord(std::cout, record);
    }

  bool ok = reader->eof();
  delete reader;

  if (not ok)
    return 1;
  return 0;
}
// NOLINTEND(cppcoreguidelines-owning-memory, cppcoreguidelines-pro-bounds-pointer-arithmetic)
