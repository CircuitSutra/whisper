#include "TraceReader.hpp"


using namespace WhisperUtil;

int
main(int argc, char* argv[])
{
  if (argc < 2)
    return 1;

  TraceReader reader(argv[1]);

  TraceRecord record;
  while (reader.nextRecord(record))
    {
      if (record.isVector())
        {
          bool ta = reader.tailAgnostic();
          bool ma = reader.maskAgnostic();
          unsigned vstart = reader.vstartValue();
          unsigned vl = reader.vlValue();
          unsigned gx8 = reader.groupMultiplierX8();
          unsigned sewib = reader.vecElemWidthInBytes();
          std::string name = record.instructionName();
          std::cout << name << " ta=" << ta << " ma=" << ma << " vstart=" << vstart
                    << " vl=" << vl << " sweib=" << sewib << " groupx8=" << gx8 << '\n';
        }
    }

  return 0;
}
