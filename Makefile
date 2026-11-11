# Host-side build: unit tests and the headless runner.
# The Raspberry Pi kernel is built with ./build.sh.

CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra
BUILD    := build

CORE     := src/core/macii.cpp src/core/m68k.cpp src/core/fpu.cpp src/core/fmath.cpp src/core/via.cpp src/core/iwm.cpp src/core/scsi.cpp src/core/rtc.cpp src/core/adb.cpp src/core/asc.cpp src/core/nubusvideo.cpp src/core/mmu040.cpp src/core/mac.cpp
FRONTEND := src/frontend/config.cpp src/frontend/keymap.cpp src/frontend/video.cpp src/frontend/osd.cpp
HEADERS  := $(wildcard src/core/*.h src/frontend/*.h)

# SingleStepTests 68000 vectors (git clone https://github.com/SingleStepTests/680x0 sst)
SST      ?= sst/68000/v1
SST_PER_INSTRUCTION ?= 1000

.PHONY: all test cputest clean

all: $(BUILD)/hwtest $(BUILD)/fetest $(BUILD)/cpu020test $(BUILD)/videotest $(BUILD)/maciitest $(BUILD)/mmutest $(BUILD)/cputest $(BUILD)/macemu-host

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/hwtest: tests/hwtest.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/hwtest.cpp $(CORE)

$(BUILD)/fetest: tests/fetest.cpp $(CORE) $(FRONTEND) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/fetest.cpp $(CORE) $(FRONTEND)

$(BUILD)/cpu020test: tests/cpu020test.cpp tests/cpu020_cases.h $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/cpu020test.cpp src/core/m68k.cpp src/core/fpu.cpp src/core/fmath.cpp

$(BUILD)/videotest: tests/videotest.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/videotest.cpp src/core/m68k.cpp src/core/nubusvideo.cpp

$(BUILD)/maciitest: tests/maciitest.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/maciitest.cpp $(CORE)

$(BUILD)/mmutest: tests/mmutest.cpp src/core/m68k.cpp src/core/mmu040.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/mmutest.cpp src/core/m68k.cpp src/core/mmu040.cpp

# Regenerate the video driver (needs binutils-m68k-linux-gnu).
src/core/videodrv.h: src/core/videodrv.s tools/build_videodrv.py
	python3 tools/build_videodrv.py

# Regenerate the 68020+ cases (needs binutils-m68k-linux-gnu).
tests/cpu020_cases.h: tools/gen_cpu020_tests.py
	python3 tools/gen_cpu020_tests.py > $@.tmp && mv $@.tmp $@

$(BUILD)/cputest: tests/cputest.cpp src/core/m68k.cpp src/core/m68k.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests/cputest.cpp src/core/m68k.cpp

$(BUILD)/macemu-host: src/host/main.cpp $(CORE) $(FRONTEND) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ src/host/main.cpp $(CORE) $(FRONTEND)

test: $(BUILD)/hwtest $(BUILD)/fetest $(BUILD)/cpu020test $(BUILD)/videotest $(BUILD)/maciitest $(BUILD)/mmutest $(BUILD)/mmutest
	$(BUILD)/hwtest
	$(BUILD)/fetest
	$(BUILD)/cpu020test
	$(BUILD)/videotest
	$(BUILD)/maciitest
	$(BUILD)/mmutest

$(BUILD)/vectors.txt: tools/sst2txt.py | $(BUILD)
	python3 tools/sst2txt.py $(SST) $(SST_PER_INSTRUCTION) > $@

cputest: $(BUILD)/cputest $(BUILD)/vectors.txt
	$(BUILD)/cputest $(BUILD)/vectors.txt

clean:
	rm -rf $(BUILD)
