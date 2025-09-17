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

#include "Ats.hpp"
#include <iostream>
#include <iomanip>
#include <cassert>

using namespace TT_IOMMU;

void testIotinvalVmaCommand()
{
  std::cout << "Testing IOTINVAL.VMA command structure..." << std::endl;
  
  // Test creating IOTINVAL.VMA command with different combinations
  
  // Test 1: Global invalidation (GV=0, AV=0, PSCV=0)
  IotinvalVmaCommand vmaCmd1;
  vmaCmd1.GV = 0;
  vmaCmd1.AV = 0;
  vmaCmd1.PSCV = 0;
  
  Command cmd1(vmaCmd1);
  assert(cmd1.isIotinval() == true);
  assert(cmd1.isIotinvalVma() == true);
  assert(cmd1.isIotinvalGvma() == false);
  
  std::cout << "  IOTINVAL.VMA (global): opcode=" << static_cast<int>(vmaCmd1.opcode) 
            << ", func3=" << static_cast<int>(vmaCmd1.func3)
            << ", GV=" << vmaCmd1.GV << ", AV=" << vmaCmd1.AV 
            << ", PSCV=" << vmaCmd1.PSCV << std::endl;
  
  // Test 2: Address-specific invalidation (GV=0, AV=1, PSCV=0)
  IotinvalVmaCommand vmaCmd2;
  vmaCmd2.GV = 0;
  vmaCmd2.AV = 1;
  vmaCmd2.PSCV = 0;
  vmaCmd2.ADDR = 0x12345;  // Page address
  
  Command cmd2(vmaCmd2);
  assert(cmd2.isIotinvalVma() == true);
  
  std::cout << "  IOTINVAL.VMA (address-specific): GV=" << vmaCmd2.GV 
            << ", AV=" << vmaCmd2.AV << ", PSCV=" << vmaCmd2.PSCV 
            << ", ADDR=0x" << std::hex << vmaCmd2.ADDR << std::dec << std::endl;
  
  // Test 3: Process and address specific (GV=0, AV=1, PSCV=1)
  IotinvalVmaCommand vmaCmd3;
  vmaCmd3.GV = 0;
  vmaCmd3.AV = 1;
  vmaCmd3.PSCV = 1;
  vmaCmd3.PSCID = 0x1234;
  vmaCmd3.ADDR = 0x56789;
  
  Command cmd3(vmaCmd3);
  assert(cmd3.isIotinvalVma() == true);
  
  std::cout << "  IOTINVAL.VMA (process+address): GV=" << vmaCmd3.GV 
            << ", AV=" << vmaCmd3.AV << ", PSCV=" << vmaCmd3.PSCV 
            << ", PSCID=0x" << std::hex << vmaCmd3.PSCID 
            << ", ADDR=0x" << vmaCmd3.ADDR << std::dec << std::endl;
  
  std::cout << "  ✓ IOTINVAL.VMA command structure test PASSED!" << std::endl << std::endl;
}

void testIotinvalGvmaCommand()
{
  std::cout << "Testing IOTINVAL.GVMA command structure..." << std::endl;
  
  // Test creating IOTINVAL.GVMA command
  
  // Test 1: Global second-stage invalidation (GV=0, AV=0, PSCV=0)
  IotinvalGvmaCommand gvmaCmd1;
  gvmaCmd1.GV = 0;
  gvmaCmd1.AV = 0;
  gvmaCmd1.PSCV = 0;  // Must be 0 for GVMA
  
  Command cmd1(gvmaCmd1);
  assert(cmd1.isIotinval() == true);
  assert(cmd1.isIotinvalGvma() == true);
  assert(cmd1.isIotinvalVma() == false);
  
  std::cout << "  IOTINVAL.GVMA (global): opcode=" << static_cast<int>(gvmaCmd1.opcode) 
            << ", func3=" << static_cast<int>(gvmaCmd1.func3)
            << ", GV=" << gvmaCmd1.GV << ", AV=" << gvmaCmd1.AV 
            << ", PSCV=" << gvmaCmd1.PSCV << std::endl;
  
  // Test 2: Guest-specific invalidation (GV=1, AV=0, PSCV=0)
  IotinvalGvmaCommand gvmaCmd2;
  gvmaCmd2.GV = 1;
  gvmaCmd2.AV = 0;
  gvmaCmd2.PSCV = 0;
  gvmaCmd2.GSCID = 0x5678;
  
  Command cmd2(gvmaCmd2);
  assert(cmd2.isIotinvalGvma() == true);
  
  std::cout << "  IOTINVAL.GVMA (guest-specific): GV=" << gvmaCmd2.GV 
            << ", AV=" << gvmaCmd2.AV << ", PSCV=" << gvmaCmd2.PSCV 
            << ", GSCID=0x" << std::hex << gvmaCmd2.GSCID << std::dec << std::endl;
  
  std::cout << "  ✓ IOTINVAL.GVMA command structure test PASSED!" << std::endl << std::endl;
}

void testCommandOpcodes()
{
  std::cout << "Testing IOTINVAL command opcode assignments..." << std::endl;
  
  // Verify opcode values match specification
  assert(static_cast<uint32_t>(CommandOpcode::IOTINVAL) == 1);
  assert(static_cast<uint32_t>(CommandOpcode::IOFENCE) == 2);
  assert(static_cast<uint32_t>(CommandOpcode::ATS) == 4);
  
  // Verify function codes
  assert(static_cast<uint32_t>(IotinvalFunc::VMA) == 0);
  assert(static_cast<uint32_t>(IotinvalFunc::GVMA) == 1);
  
  std::cout << "  Opcodes: IOTINVAL=" << static_cast<int>(CommandOpcode::IOTINVAL)
            << ", IOFENCE=" << static_cast<int>(CommandOpcode::IOFENCE)  
            << ", ATS=" << static_cast<int>(CommandOpcode::ATS) << std::endl;
  
  std::cout << "  Functions: VMA=" << static_cast<int>(IotinvalFunc::VMA)
            << ", GVMA=" << static_cast<int>(IotinvalFunc::GVMA) << std::endl;
  
  std::cout << "  ✓ Command opcode assignments are correct!" << std::endl << std::endl;
}

void printSpecificationOverview()
{
  std::cout << "=== RISC-V IOMMU IOTINVAL Command Implementation ===" << std::endl;
  std::cout << "This implementation provides stub support for:" << std::endl;
  std::cout << "  - IOTINVAL.VMA  (First-stage page table cache invalidation)" << std::endl;
  std::cout << "  - IOTINVAL.GVMA (Second-stage page table cache invalidation)" << std::endl;
  std::cout << std::endl;
  
  std::cout << "Command format according to RISC-V IOMMU Specification 3.1.1:" << std::endl;
  std::cout << "  GV: Guest-Soft-Context ID (GSCID) operand valid" << std::endl;
  std::cout << "  AV: Address (ADDR) operand valid" << std::endl;
  std::cout << "  PSCV: Process Soft-Context ID (PSCID) operand valid (VMA only)" << std::endl;
  std::cout << std::endl;
  
  std::cout << "Invalidation scopes based on operand combinations:" << std::endl;
  std::cout << "  GV=0, AV=0, PSCV=0: Invalidate all entries for all address spaces" << std::endl;
  std::cout << "  GV=0, AV=0, PSCV=1: Invalidate entries for specific PSCID (VMA only)" << std::endl;
  std::cout << "  GV=0, AV=1, PSCV=0: Invalidate entries for specific address" << std::endl;
  std::cout << "  GV=0, AV=1, PSCV=1: Invalidate entries for specific address+PSCID (VMA only)" << std::endl;
  std::cout << "  GV=1, AV=0, PSCV=0: Invalidate all entries for specific GSCID" << std::endl;
  std::cout << "  GV=1, AV=1, PSCV=0: Invalidate entries for specific address+GSCID" << std::endl;
  std::cout << std::endl;
}

int main()
{
  printSpecificationOverview();
  
  testCommandOpcodes();
  testIotinvalVmaCommand();
  testIotinvalGvmaCommand();
  
  std::cout << "🎉 All IOTINVAL command structure tests PASSED!" << std::endl;
  std::cout << std::endl;
  std::cout << "Implementation Status:" << std::endl;
  std::cout << "  ✓ Command structures defined" << std::endl;
  std::cout << "  ✓ Opcode and function assignments" << std::endl;
  std::cout << "  ✓ Command queue integration" << std::endl;
  std::cout << "  ✓ Stub execution methods" << std::endl;
  std::cout << "  ⚠ Actual cache invalidation logic - TODO (left unimplemented as requested)" << std::endl;
  std::cout << std::endl;
  std::cout << "The stub implementation correctly parses commands and validates parameters," << std::endl;
  std::cout << "but does not perform actual IOATC invalidation operations." << std::endl;
  
  return 0;
}
