OFLAGS = -O3
CXXFLAGS += -I. -I../ -I$(BOOST_ROOT)/include -std=c++20 $(OFLAGS) -fPIC -Wall -Wextra -Werror

BOOST_LIB_DIR := $(BOOST_ROOT)/lib

# Needed to link against boost libraries which were compiled using older ABI
ifeq ($(BOOST_ROOT), /tools_vendor/FOSS/boost/1.82)
  CXXFLAGS += -D_GLIBCXX_USE_CXX11_ABI=0
endif

SRCS := TraceReader.cpp PageTableMaker.cpp
OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)

default: sample-reader dj-sample

TraceReader.a: $(OBJS)
	$(AR) cr $@ $^

sample-reader: sample-reader.o TraceReader.a
	$(CXX) -o $@ $^ -L$(BOOST_LIB_DIR) -l:libboost_iostreams.a -lz

dj-sample: dj-sample.o TraceReader.a
	$(CXX) -o $@ $^ -L$(BOOST_LIB_DIR) -l:libboost_iostreams.a -lz

-include $(DEPS)

clean:
	rm -f TraceReader.a $(OBJS) $(DEPS) *.o sample-reader
