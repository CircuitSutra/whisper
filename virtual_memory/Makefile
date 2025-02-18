CXX      = g++
CXXFLAGS = -std=c++20 -Wall -I. 

SRCS    = address_translation.cpp VirtMem.cpp
OBJS    = $(SRCS:.cpp=.o)

TARGET  = libvirtual_memory.a

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(AR) rcs $(TARGET) $(OBJS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

