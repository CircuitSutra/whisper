# Compiler settings.
CXX      = g++
CXXFLAGS = -std=c++20 -Wall -I. 

# Source files in the current directory.
SRCS    = address_translation.cpp VirtMem.cpp
OBJS    = $(SRCS:.cpp=.o)

# The target static library.
TARGET  = libvirtual_memory.a

.PHONY: all clean

# Default target: build the static library.
all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $(TARGET) $(OBJS)

# Rule for compiling .cpp files into .o files.
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Clean up generated files.
clean:
	rm -f $(OBJS) $(TARGET)

