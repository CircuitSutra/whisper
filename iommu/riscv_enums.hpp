#pragma once

#include <cstdint>

// Temporary until we consolidate basic types in Whisper.

namespace TT_IOMMU
{
  /// RISCV privilege modes.
  enum class PrivilegeMode : uint32_t
    {
      User = 0, Supervisor = 1, Rserved = 2, Machine = 3
    };
}
