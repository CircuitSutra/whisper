#pragma once

#include <vector>
#include <iostream>
#include <cstring>
#include <functional>


class MemoryModel {
public:
    explicit MemoryModel(size_t size) : memory(size, 0) {
        std::cout << "[MEM] Created memory model of size " << size << " bytes" << std::endl;
    }
    using ReadHandlerFunc = std::function<bool(uint64_t, unsigned, uint64_t&)>;
    
    void setReadHandler(ReadHandlerFunc handler) {
        readHandler_ = handler;
    }
    bool read(uint64_t addr, unsigned size, uint64_t& data) const {
        // Check if a read handler is installed
        if (readHandler_) {
            bool result = readHandler_(addr, size, data);
            if (!result) {
                std::cout << "[MEM] Read handler failed for addr 0x" 
                          << std::hex << addr << std::dec << std::endl;
                return false;
            }
        }
        
        // Normal read logic follows
        if (addr + size > memory.size()) {
            std::cout << "[MEM] Read error: address 0x" << std::hex << addr 
                      << " + size " << std::dec << size 
                      << " exceeds memory size " << memory.size() << std::endl;
            return false;
        }
        data = 0;
        std::memcpy(&data, &memory[addr], size);
        std::cout << "[MEM] Read " << size << " bytes from addr 0x" << std::hex << addr 
                  << " -> 0x" << data << std::dec << std::endl;
        return true;
    }
    
    bool write(uint64_t addr, unsigned size, uint64_t data) {
        if (addr + size > memory.size()) {
            std::cout << "[MEM] Write error: address 0x" << std::hex << addr 
                      << " + size " << std::dec << size 
                      << " exceeds memory size " << memory.size() << std::endl;
            return false;
        }
        std::cout << "[MEM] Writing 0x" << std::hex << data << " (" << size 
                  << " bytes) to addr 0x" << addr << std::dec << std::endl;
        std::memcpy(&memory[addr], &data, size);
        return true;
    }

    uint64_t size() const {
      return memory.size();
    }

private:
    std::vector<uint8_t> memory;
    ReadHandlerFunc readHandler_ = nullptr;
};
