CXX := clang++
CXXFLAGS := -stdlib=libc++ -std=c++17 -W -Wall -O3 -ffast-math -I../dsp -I../code

PYTHON := python3
PYBIND_INCLUDES := $(shell $(PYTHON) -m pybind11 --includes)
PYEXT := $(shell $(PYTHON)-config --extension-suffix)
PYLDFLAGS := $(shell $(PYTHON)-config --embed --ldflags)

all: encode modem_tx$(PYEXT) modem_rx$(PYEXT)

encode: encode_cli.cc encode_engine.cc encode_engine.hh common.hh
	$(CXX) $(CXXFLAGS) encode_cli.cc encode_engine.cc -o encode

modem_tx$(PYEXT): pybind_modem_tx.cc encode_engine.cc encode_engine.hh common.hh
	$(CXX) $(CXXFLAGS) -shared -fPIC $(PYBIND_INCLUDES) \
		pybind_modem_tx.cc \
		encode_engine.cc \
		-o modem_tx$(PYEXT) $(PYLDFLAGS)

modem_rx$(PYEXT): pybind_modem_rx.cc decode_engine.cc decode_engine.hh common.hh
	$(CXX) $(CXXFLAGS) -shared -fPIC $(PYBIND_INCLUDES) \
		pybind_modem_rx.cc \
		decode_engine.cc \
		-o modem_rx$(PYEXT) $(PYLDFLAGS)

freezer: freezer.cc
	$(CXX) $(CXXFLAGS) $< -o $@

polar_tables.hh: freezer
	./$< > $@

.PHONY: clean

clean:
	rm -f encode modem_tx*.so modem_tx*.dylib modem_rx*.so modem_rx*.dylib