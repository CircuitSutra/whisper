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
  struct IofenceCCommand;

  // ATS command functions as defined in section 4.1.4
  enum class AtsFunc : uint32_t
  {
    INVAL = 0,  // Send ATS "Invalidation Request" messages
    PRGR  = 1   // Send ATS "Page Request Group Response" messages
    // 2-7 reserved for future standard use
  };

  // IOFENCE command functions
  enum class IofenceFunc : uint32_t
  {
    C = 0  // Command queue fence
    // 1-15 reserved for future standard use
  };

  // Command opcodes
  enum class CommandOpcode : uint32_t
  {
    // 0-1 reserved
    IOFENCE = 2,  // IOMMU fence commands
    // 3 reserved  
    ATS = 4       // IOMMU PCIe ATS commands
  };

  // For backward compatibility
  using AtsOpcode = CommandOpcode;

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
    uint64_t      destId        : 16; 

    // Constructor to initialize opcode and func3
    AtsPrgrCommand()
    {
      opcode = AtsOpcode::ATS;
      func3 = AtsFunc::PRGR;
      reserved0 = 0;
      reserved1 = 0;
      zero0 = 0;
      zero1 = 0;
      destId = 0;
    }
  };
  
  // IOFENCE.C command structure based on specification
  struct IofenceCCommand
  {
    CommandOpcode   opcode      : 7;  // Command opcode (bits 0-6) = IOFENCE (0x2)
    IofenceFunc     func3       : 3;  // Function (bits 7-9) = C (0x0)
    uint64_t        AV          : 1;  // Address Valid (bit 10)
    uint64_t        WSI         : 1;  // Wire-Signaled-Interrupt (bit 11)
    uint64_t        PR          : 1;  // Previous Reads (bit 12)
    uint64_t        PW          : 1;  // Previous Writes (bit 13)
    uint64_t        reserved0   : 18; // Reserved bits (bits 14-31)
    uint64_t        DATA        : 32; // Data to write (bits 32-63)
    uint64_t        ADDR        : 62; // Address[63:2] for 4-byte aligned writes (bits 64-125)
    uint64_t        reserved1   : 2;

    // Constructor to initialize opcode and func3
    IofenceCCommand()
    {
      opcode = CommandOpcode::IOFENCE;
      func3 = IofenceFunc::C;
      AV = 0;
      WSI = 0;
      PR = 0;
      PW = 0;
      reserved0 = 0;
      DATA = 0;
      ADDR = 0;
    }
  };

  // Union to reinterpret 2 double words as different commands.
  union Command
  {
    /// Construct from an AtsInvalCommand
    Command(AtsInvalCommand inval)
      : inval(inval)
    {}

    /// Construct from an AtsPrgrCommand
    Command(AtsPrgrCommand prgr)
      : prgr(prgr)
    {}

    /// Construct from an IofenceCCommand
    Command(IofenceCCommand iofence)
      : iofence(iofence)
    {}

    /// Construct from an AtsCommandData.
    Command(AtsCommandData data)
      : data(data)
    {}

    /// Construct from 2 double words.
    Command(uint64_t dw0, uint64_t dw1)
      : data{dw0, dw1}
    {}

    /// True if the opcode corresponds to ATS.
    bool isAts() const
    { return (CommandOpcode)inval.opcode == CommandOpcode::ATS; }

    /// True if the opcode corresponds to IOFENCE.
    bool isIofence() const
    { return (CommandOpcode)iofence.opcode == CommandOpcode::IOFENCE; }

    /// True if ATS invalidate command.
    bool isInval() const
    { return isAts() and inval.func3 == AtsFunc::INVAL; }

    /// True if ATS page request group command.
    bool isPrgr() const
    { return isAts() and prgr.func3 == AtsFunc::PRGR; }

    /// True if IOFENCE.C command.
    bool isIofenceC() const
    { return isIofence() and iofence.func3 == IofenceFunc::C; }

    /// Return the first double word of this command.
    uint64_t dw0() const
    { return data.dw0; }

    /// Return the second double word of this command.
    uint64_t dw1() const
    { return data.dw1; }

    AtsInvalCommand   inval;
    AtsPrgrCommand    prgr;
    IofenceCCommand   iofence;
    AtsCommandData    data;
  };

  // For backward compatibility
  using AtsCommand = Command;

} // namespace TT_IOMMU


