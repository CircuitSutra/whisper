OFLAGS = -O3
CXXFLAGS += -I. -I../ -std=c++20 $(OFLAGS) -fPIC -Wall -Wextra -Werror

# Needed to link against boost libraries which were compiled using older ABI
ifeq ($(BOOST_ROOT), /tools_vendor/FOSS/boost/1.82)
  CXXFLAGS += -D_GLIBCXX_USE_CXX11_ABI=0
endif

SRCS := TraceReader.cpp PageTableMaker.cpp
OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)

default: sample-reader

TraceReader.a: $(OBJS)
	$(AR) cr $@ $^

sample-reader: sample-reader.o TraceReader.a
	$(CXX) -o $@ $^ -lboost_iostreams -lz

-include $(DEPS)

clean:
	rm -f TraceReader.a $(OBJS) $(DEPS) *.o sample-reader
