#include "TraceReader.hpp"


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

  if (not reader->eof())
    return 1;

  return 0;
}
