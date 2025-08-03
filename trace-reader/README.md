# whisper-trace-reader

Trace reader for Whisper CSV traces.

## Trace format

The Whisper trace is a single text file with one record per line. The first non-empty line
is a header describing the remaining records in the file. All non-empty lines consists of
comma separated values. All integer values use C-style representation.


## Header record

The header record consists of a list of strings, separated by commas, defining the fields
of the remaining records in the file. Below is a description of the fields.

### pc

The program counter associated with the record. The field will contain either one integer
denoting the virtual address of the instruction or a pair of integers, separated by a
colon, and denoting respectively the virtual and physical addresses of the instruction.

### inst
An integer value denoting the opcode of the instruction.

### modified regs

A list of semicolon separated sub-fields denoting the registers modified by the instruction
and their values. Each sub-field consist of a register name (a string) followed by
an equal sign, followed by an integer value.

### memory

A list of semicolon separated sub-fields denoting the memory locations accessed/written by
the instruction. Each sub-fields consists of an address or a colon separated pair of
addresses optionally followed by an equal sign and a value. If the memory transaction is a
read, then the equal sign and the value will be omitted. The address denotes the physical
memory address of the transaction. The colon separated pair of addresses denote the virtual
memory address and the corresponding physical address.

### inst info

### privilege

The privilege mode of the machine when the instruction is fetched.

### trap

Whether or not the instruction encountered a trap.

### disassembly

The disassembly of the instruction.

### hartid

The hart id of the hart that executed the instruction.

### iptw

The instruction page table walk.

### dptw

The data page table walk.

### pmp

The physical memory protection.
