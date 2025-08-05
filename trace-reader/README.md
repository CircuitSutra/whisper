# whisper-trace-reader

The Whisper trace reader is a C++ library that reads a trace file produced by whisper and
provides an API to access the trace records in that file.

## Trace file format

The Whisper trace is a single text file with one record per line. The first non-empty line
is a header record describing the fields of the remaining records in the file. The
remaining lines correspond to the retired instructions of a run in program order. All
non-empty lines consists of comma separated values. All integer values use C-style
representation (integers encoded in hexadecimal notation are prefixed with "0x").


## Header record

The header record consists of a list of strings, separated by commas, defining the fields
of the remaining records in the file. For example, if the first string in the header line
is "pc", then the first field of every subsequent record will contain an integer
representing the program counter of the corresponding instruction. Below is a description
of the fields.

### pc

The program counter associated with the record. The field will contain either one integer
denoting the virtual address of the instruction or a pair of integers, separated by a
colon, and denoting respectively the virtual and physical addresses of the instruction.

### inst

An integer value denoting the opcode of the instruction.

### modified regs

A list of semicolon separated sub-fields denoting the registers modified by the instruction
and their values. Each sub-field consist of a register name (a string) followed by
the equal sign, followed by an integer value.

### memory

A list of semicolon separated sub-fields denoting the memory locations read/written by
the instruction. Each sub-field consists of an address or a colon separated pair of
addresses optionally followed by the equal sign and a value. If the memory transaction is a
read, then the equal sign and the value will be omitted. The address denotes the physical
memory address of the transaction. The colon separated pair of addresses denote the virtual
memory address and the corresponding physical address.

### inst info

The inst info value is a single character designating the type of the instruction as
follows
* f: floating point
* v: vector
* a: atomic
* l: load
* s: store

### privilege

The privilege mode of the machine when the instruction is fetched. The value is a
string interpreted as follows:
* m: machine privilege
* s: supervisor privilege
* u: user privilege
* vs: virtual supervisor privilege
* vu: virtual user privilege

### trap

Whether or not the instruction encountered a trap. The value is either an empty string
indicating that the instruction did not encounter a trap or an integer value containing
the cause of the trap (value of MCAUSE/SCAUSE CSR).

### disassembly

The disassembly of the instruction. The value is a string.

### hartid

The hart id of the hart that executed the instruction. The value is an integer.

### iptw

The instruction page table walk. The value is either the empty string or a semicolon separated
list of values representing the page table walk.

### dptw

The data page table walk. The value is either the empty string or a semicolon separated
list of values representing the page table walk.

### pmp

The physical memory protection.
