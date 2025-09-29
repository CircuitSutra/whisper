RISC-V IOMMU Command Queue Test Cases
BASIC FUNCTIONALITY TESTS

Test Case 1: Queue Initialization
    Allocate 4KB aligned memory buffer for 16 entries (256 bytes), write cqb with log(size)-1=3
    Allocate 4KB aligned memory buffer for 64 entries (1KB), write cqb with log(size)-1=5
    Allocate 4KB aligned memory buffer for 256 entries (4KB), write cqb with log(size)-1=7
    Set cqt = 0
    Set cqcsr.cqen = 1
    Poll cqcsr.cqon until it reads 1

Test Case 2: Queue Size Validation
    Test 2 entries (32 bytes): log(size)-1=0
    Test 4 entries (64 bytes): log(size)-1=1
    Test 8 entries (128 bytes): log(size)-1=2
    Test 512 entries (8KB): log(size)-1=8, verify 8KB alignment required
    Test 1024 entries (16KB): log(size)-1=9, verify 16KB alignment required
    Test invalid sizes (non-power-of-2) are rejected

COMMAND PROCESSING TESTS
Test Case 3: Basic Command Submission
    Initialize queue with 16 entries (LOG2SZ-1=3)
    Submit simple IOFENCE.C command
    Update cqt = 1
    Monitor cqh advancement from 0 to 1

Test Case 4: Queue Full/Empty Detection
    Use 16-entry queue: full when cqt=15 and cqh=0
    Empty when cqh == cqt
    Test wraparound: cqt goes from 15 to 0

Test Case 5: Command Ordering
    Submit sequence of IOTINVAL commands with different parameters
    Submit IOFENCE.C command
    Monitor completion order

COMMAND TYPE TESTS
Test Case 6: IOTINVAL.VMA Commands
    GV=0, AV=0, PSCV=0 (invalidate all host address spaces)
    GV=0, AV=1, PSCV=1 (specific IOVA and PSCID)
    GV=1, AV=0, PSCV=0 (all VM address spaces for GSCID)
    GV=1, AV=1, PSCV=1 (specific VM, IOVA, and PSCID)

Test Case 7: IOTINVAL.GVMA Commands
    GV=0, AV=ignored (all VM address spaces)
    GV=1, AV=0 (specific GSCID, all addresses)
    GV=1, AV=1 (specific GSCID and GPA)
    Verify PSCV=1 with GVMA is rejected as illegal

Test Case 8: IOFENCE.C Commands
    Submit multiple IOTINVAL commands
    Submit IOFENCE.C with PR=1, PW=1
    Verify all previous commands complete before IOFENCE.C
    Test IOFENCE.C with AV=1 (memory write operation)
    Test WSI interrupt generation if supported

Test Case 9: ATS Commands (if capabilities.ATS = 1)
    ATS.INVAL with various RID, PASID combinations
    ATS.PRGR with different payload configurations
    Test timeout handling for unresponsive devices
    Test DSV=1 with segment numbers

ERROR HANDLING TESTS
Test Case 10: Illegal Command Detection
    Submit command with reserved opcode
    Submit command with reserved bits set
    Submit valid opcode with unsupported func3
    Verify cqcsr.cmd_ill bit set and queue stops processing
    Clear cmd_ill and verify processing resumes

Test Case 11: Command Timeout Handling
    Submit ATS.INVAL to non-existent/unresponsive device
    Submit IOFENCE.C command
    Wait for timeout period
    Verify cqcsr.cmd_to bit set when IOFENCE.C processes

Test Case 12: Memory Access Fault Handling
    Configure queue in protected/invalid memory region
    Attempt command processing
    Verify cqcsr.cqmf bit set and queue processing stops
    Clear cqmf and verify recovery

CONFIGURATION TESTS
Test Case 13: Endianness Handling
    Configure fctl.BE = 0 (little-endian)
    Submit commands with multi-byte operands
    Change fctl.BE = 1 (big-endian) if supported
    Repeat command submission

Test Case 14: Queue Disable and Reconfiguration
    Start with 16-entry queue (LOG2SZ-1=3)
    Set cqcsr.cqen = 0 while queue active
    Wait for cqcsr.cqon = 0
    Reconfigure to 64-entry queue (LOG2SZ-1=5) at different address
    Re-enable queue

Test Case 15: Busy Bit Behavior
    Write to cqcsr and verify busy bit set
    Poll until busy bit clears
    Verify subsequent writes work correctly

INTERRUPT TESTS
Test Case 16: Command Queue Interrupts
    Enable command queue interrupts (cqcsr.cie = 1)
    Trigger cmd_ill error and verify ipsr.cip set
    Trigger cmd_to error and verify ipsr.cip set
    Trigger cqmf error and verify ipsr.cip set
    Test fence_w_ip interrupt (WSI only)
    Verify MSI/WSI generated according to configuration

Test Case 17: Interrupt Clearing
    Generate interrupt condition
    Clear error bits in cqcsr
    Verify ipsr.cip clears appropriately

REGISTER ACCESS TESTS
Test Case 18: Register Width Validation
    Test 32-bit access to 32-bit registers (cqh, cqt, cqcsr)
    Test 32-bit and 64-bit access to 64-bit register (cqb)
    Verify unaligned accesses behave as specified

Test Case 19: Reserved Field Behavior
    Write 1s to reserved fields
    Verify reserved fields read as 0 or preserve values appropriately
    Test WARL field behavior

EDGE CASE TESTS
Test Case 20: Boundary Conditions
    Test with 2-entry queue (log(size)-1=0) - minimum size
    Test with 1024-entry queue (log(size)-1=9) - large size
    Test queue indices at wrap-around points (15->0 for 16-entry queue)
    Test simultaneous head/tail updates

COMMAND FORMAT TESTS
Test Case 21: IOTINVAL Command Format
    opcode=1, func3=0 for IOTINVAL.VMA
    opcode=1, func3=1 for IOTINVAL.GVMA
    Test reserved func3 values (2-7) are rejected
    Verify 16-byte command structure

Test Case 22: IOFENCE Command Format
    opcode=2, func3=0 for IOFENCE.C
    Test PR bit (bit 10) functionality
    Test PW bit (bit 11) functionality
    Test AV bit (bit 12) and associated ADDR/DATA fields
    Test WSI bit (bit 13) if wire-signaled interrupts supported

Test Case 23: ATS Command Format
    opcode=4, func3=0 for ATS.INVAL
    opcode=4, func3=1 for ATS.PRGR
    Test RID field (bits 31:16)
    Test PV bit and PID field
    Test DSV bit and DSEG field
    Test PAYLOAD field formatting

SPECIFIC PARAMETER TESTS
Test Case 24: Address and ID Range Testing
    Test IOVA addresses: 0x0, 0x1000, 0xFFFFFFFF000 (page boundaries)
    Test device IDs: 0, 255, 65535, 16777215 (various widths)
    Test process IDs: 0, 255, 1048575 (20-bit max)
    Test GSCID values: 0, 1, 65535 (16-bit range)
    Test PSCID values: 0, 1, 1048575 (20-bit range)

Test Case 25: Command Operand Validation
    IOTINVAL.VMA: Test all 8 combinations of GV/AV/PSCV bits
    IOTINVAL.GVMA: Test all 4 combinations of GV/AV bits (PSCV must be 0)
    IOFENCE.C: Test all combinations of PR/PW/AV/WSI bits

Test Case 26: Multi-Command Sequences
    Submit 10 IOTINVAL.VMA commands with different parameters
    Follow with IOFENCE.C and verify ordering
    Mix different command types in single batch
    Test maximum queue utilization (fill to capacity-1)

ENDIANNESS SPECIFIC TESTS
Test Case 27: Little-Endian Command Processing (fctl.BE=0)
    Submit IOTINVAL.VMA with ADDR=0x123456789ABC0000
    Verify ADDR field interpreted as little-endian
    Submit IOFENCE.C with DATA=0xDEADBEEF
    Verify DATA field written as little-endian

Test Case 28: Big-Endian Command Processing (fctl.BE=1)
    Submit IOTINVAL.VMA with ADDR=0x123456789ABC0000
    Verify ADDR field interpreted as big-endian
    Submit IOFENCE.C with DATA=0xDEADBEEF
    Verify DATA field written as big-endian

Test Case 29: Endianness Transition
    Start with fctl.BE=0, submit commands
    Change to fctl.BE=1 (if supported)
    Submit identical commands
    Verify different byte order processing
