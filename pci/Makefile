OFLAGS   := -O3
CXX_STD  ?= c++20
CXXFLAGS := $(OFLAGS) -std=$(CXX_STD) -fPIC -pedantic -Wall -Wextra -Werror -I.
DEPFLAGS := -MMD -MP

# Needed to link against boost libraries which were compiled using older ABI
ifeq ($(BOOST_ROOT), /tools_vendor/FOSS/boost/1.82)
  CXXFLAGS += -D_GLIBCXX_USE_CXX11_ABI=0
endif

SRCS := Pci.cpp PciDev.cpp virtio/Virtio.cpp virtio/Blk.cpp
OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)

%.o: %.cpp
	$(CXX) -c $(CXXFLAGS) $(DEPFLAGS) -o $@ $<

libpci.a: $(OBJS)
	$(AR) rcs $@ $^

-include $(DEPS)

clean:
	rm -f libpci.a $(OBJS) $(DEPS)
