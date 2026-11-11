# Host-side build: unit tests and the headless runner.
# The Raspberry Pi kernel is built with ./build.sh.

CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra
BUILD    := build

CORE     := src/core/m68k.cpp src/core/via.cpp src/core/iwm.cpp src/core/scsi.cpp src/core/mac.cpp
FRONTEND := src/frontend/config.cpp src/frontend/keymap.cpp src/frontend/video.cpp src/frontend/osd.cpp
HEADERS  := $(wildcard src/core/*.h src/frontend/*.h)

# SingleStepTests 68000 vectors (git clone https://github.com/SingleStepTests/680x0 sst)
SST      ?= sst/68000/v1
SST_PER_INSTRUCTION ?= 1000

.PHONY: all test cputest clean

all: $(BUILD)/hwtest $(BUILD)/fetest $(BUILD)/cputest $(BUILD)/macemu-host

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/hwtest: tests/hwtest.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/hwtest.cpp $(CORE)

$(BUILD)/fetest: tests/fetest.cpp $(CORE) $(FRONTEND) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/fetest.cpp $(CORE) $(FRONTEND)

$(BUILD)/cputest: tests/cputest.cpp src/core/m68k.cpp src/core/m68k.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests/cputest.cpp src/core/m68k.cpp

$(BUILD)/macemu-host: src/host/main.cpp $(CORE) $(FRONTEND) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ src/host/main.cpp $(CORE) $(FRONTEND)

test: $(BUILD)/hwtest $(BUILD)/fetest
	$(BUILD)/hwtest
	$(BUILD)/fetest

$(BUILD)/vectors.txt: tools/sst2txt.py | $(BUILD)
	python3 tools/sst2txt.py $(SST) $(SST_PER_INSTRUCTION) > $@

cputest: $(BUILD)/cputest $(BUILD)/vectors.txt
	$(BUILD)/cputest $(BUILD)/vectors.txt

clean:
	rm -rf $(BUILD)
