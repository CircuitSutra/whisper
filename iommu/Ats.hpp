// Copyright 2024 Tenstorrent Corporation.
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

#pragma once

#include <cstdint>

namespace TT_IOMMU
{
  // Forward declarations
  struct AtsInvalCommand;
  struct AtsPrgrCommand;

  // ATS command functions as defined in section 4.1.4
  enum class AtsFunc : uint32_t
  {
    INVAL = 0,  // Send ATS "Invalidation Request" messages
    PRGR  = 1   // Send ATS "Page Request Group Response" messages
    // 2-7 reserved for future standard use
  };

  // ATS specific opcode
  enum class AtsOpcode : uint32_t
  {
    ATS = 4  // IOMMU PCIe ATS commands
  };

  /// Ats command as 2 double words.
  struct AtsCommandData
  {
    uint64_t dw0;
    uint64_t dw1;
  };
  
  // ATS.INVAL command structure
  struct AtsInvalCommand
  {
    AtsOpcode     opcode    : 7;  // Command opcode (bits 0-6)
    AtsFunc       func3     : 3;  // Function (bits 7-9)
    uint64_t      reserved0 : 2;  // Reserved bits (bits 10-11)
    uint64_t      PID       : 20; // PASID (bits 12-31)
    uint64_t      PV        : 1;  // PASID Valid (bit 32)
    uint64_t      DSV       : 1;  // Destination Segment Valid (bit 33)
    uint64_t      reserved1 : 6;  // Reserved bits (bits 34-39)
    uint64_t      RID       : 16; // PCIe Routing ID (bits 40-55)
    uint64_t      DSEG      : 8;  // Destination Segment Number (bits 56-63)
    uint64_t      G         : 1;  
    uint64_t      zero      : 10; 
    uint64_t      s         : 1;  
    uint64_t      address   : 52; 

    // Constructor to initialize opcode and func3
    AtsInvalCommand()
    {
      opcode = AtsOpcode::ATS;
      func3 = AtsFunc::INVAL;
      reserved0 = 0;
      reserved1 = 0;
      zero = 0;
    }
  };

  // ATS.PRGR command structure
  struct AtsPrgrCommand
  {
    AtsOpcode     opcode    : 7;  // Command opcode (bits 0-6)
    AtsFunc       func3     : 3;  // Function (bits 7-9)
    uint64_t      reserved0 : 2;  // Reserved bits (bits 10-11)
    uint64_t      PID       : 20; // PASID (bits 12-31)
    uint64_t      PV        : 1;  // PASID Valid (bit 32)
    uint64_t      DSV       : 1;  // Destination Segment Valid (bit 33)
    uint64_t      reserved1 : 6;  // Reserved bits (bits 34-39)
    uint64_t      RID       : 16; // PCIe Routing ID (bits 40-55)
    uint64_t      DSEG      : 8;  // Destination Segment Number (bits 56-63)
    uint64_t      zero0         : 32;  
    uint64_t      prgi          : 9;  
    uint64_t      zero1         : 3; 
    uint64_t      responsecode  : 4; 
    uint64_t      zero3         : 16; 

    // Constructor to initialize opcode and func3
    AtsPrgrCommand()
    {
      opcode = AtsOpcode::ATS;
      func3 = AtsFunc::PRGR;
      reserved0 = 0;
      reserved1 = 0;
      zero0 = 0;
      zero1 = 0;
      zero3 = 0;
    }
  };
  

  // Union to reinterpret 2 double words as different commands.
  union AtsCommand
  {
    /// Construct from an AtsInvalCommand
    AtsCommand(AtsInvalCommand inval)
      : inval(inval)
    {}

    /// Construct from an AtsPrgrCommand
    AtsCommand(AtsPrgrCommand prgr)
      : prgr(prgr)
    {}

    /// Construct from an AtsCommandData.
    AtsCommand(AtsCommandData data)
      : data(data)
    {}

    /// Construct from 2 double words.
    AtsCommand(uint64_t dw0, uint64_t dw1)
      : data{dw0, dw1}
    {}

    /// True if the opcode corresponds to ATS.
    bool isAts() const
    { return inval.opcode == AtsOpcode::ATS; }

    /// True if ATS invalidate command.
    bool isInval() const
    { return isAts() and inval.func3 == AtsFunc::INVAL; }

    /// True if ATS page request group command.
    bool isPrgr() const
    { return isAts() and prgr.func3 == AtsFunc::PRGR; }

    /// Return the first double word of this command.
    uint64_t dw0() const
    { return data.dw0; }

    /// Return the second double word of this command.
    uint64_t dw1() const
    { return data.dw1; }

    AtsInvalCommand inval;
    AtsPrgrCommand  prgr;
    AtsCommandData  data;
  };

} // namespace TT_IOMMU


