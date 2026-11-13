# Host-side build: unit tests and the headless runner.
# The Raspberry Pi kernel is built with ./build.sh.

CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra
BUILD    := build

CORE     := src/core/macii.cpp src/core/m68k.cpp src/core/fpu.cpp src/core/fmath.cpp src/core/via.cpp src/core/iwm.cpp src/core/scsi.cpp src/core/scsicmd.cpp src/core/rtc.cpp src/core/adb.cpp src/core/asc.cpp src/core/nubusvideo.cpp src/core/mmu040.cpp src/core/dafb.cpp src/core/esp.cpp src/core/quadra.cpp src/core/scc.cpp src/core/sonic.cpp src/core/mac.cpp src/core/jit.cpp src/core/state.cpp src/core/sharedfs.cpp src/core/macnames.cpp
FRONTEND := src/frontend/config.cpp src/frontend/keymap.cpp src/frontend/video.cpp src/frontend/osd.cpp src/frontend/natrouter.cpp
HEADERS  := $(wildcard src/core/*.h src/frontend/*.h src/host/*.h)

# BearSSL (third_party/bearssl, MIT licence): TLS for the HTTPS helper.
CC       ?= gcc
BEARSSL_SRC := $(wildcard third_party/bearssl/src/*.c third_party/bearssl/src/*/*.c)
BEARSSL_OBJ := $(patsubst third_party/bearssl/src/%.c,$(BUILD)/bearssl/%.o,$(BEARSSL_SRC))
BEARSSL_INC := -I third_party/bearssl/inc
PROXY    := src/frontend/httpsproxy.cpp $(BUILD)/libbearssl.a

# SingleStepTests 68000 vectors (git clone https://github.com/SingleStepTests/680x0 sst)
SST      ?= sst/68000/v1
SST_PER_INSTRUCTION ?= 1000

.PHONY: all test cputest clean

all: $(BUILD)/hwtest $(BUILD)/fetest $(BUILD)/cpu020test $(BUILD)/videotest $(BUILD)/maciitest $(BUILD)/mmutest $(BUILD)/webtest $(BUILD)/hfstest $(BUILD)/nattest $(BUILD)/proxytest $(BUILD)/sharedfstest $(BUILD)/cputest $(BUILD)/macemu-host

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/bearssl/%.o: third_party/bearssl/src/%.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -I third_party/bearssl/inc -I third_party/bearssl/src -c $< -o $@

$(BUILD)/libbearssl.a: $(BEARSSL_OBJ)
	ar rcs $@ $^

$(BUILD)/hwtest: tests/hwtest.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/hwtest.cpp $(CORE)

$(BUILD)/fetest: tests/fetest.cpp $(CORE) $(FRONTEND) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/fetest.cpp $(CORE) $(FRONTEND)

$(BUILD)/cpu020test: tests/cpu020test.cpp tests/cpu020_cases.h $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/cpu020test.cpp src/core/m68k.cpp src/core/jit.cpp src/core/fpu.cpp src/core/fmath.cpp

$(BUILD)/videotest: tests/videotest.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/videotest.cpp src/core/m68k.cpp src/core/jit.cpp src/core/nubusvideo.cpp

$(BUILD)/maciitest: tests/maciitest.cpp $(CORE) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/maciitest.cpp $(CORE)

$(BUILD)/webtest: tests/webtest.cpp src/circle/webserver.cpp src/circle/webserver.h src/core/macnames.cpp src/core/macnames.h $(wildcard tests/webshim/*/*.h tests/webshim/*/*/*.h) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -I tests/webshim -o $@ tests/webtest.cpp src/circle/webserver.cpp src/frontend/hfs.cpp src/core/macnames.cpp -lpthread

$(BUILD)/nattest: tests/nattest.cpp src/frontend/natrouter.cpp src/frontend/natrouter.h src/host/natposix.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/nattest.cpp src/frontend/natrouter.cpp -lpthread

$(BUILD)/proxytest: tests/proxytest.cpp src/frontend/httpsproxy.h src/frontend/natrouter.h src/host/natposix.h $(PROXY) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(BEARSSL_INC) -fsanitize=address,undefined -o $@ tests/proxytest.cpp $(PROXY) -lpthread

$(BUILD)/hfstest: tests/hfstest.cpp src/frontend/hfs.cpp src/frontend/hfs.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/hfstest.cpp src/frontend/hfs.cpp

$(BUILD)/sharedfstest: tests/sharedfstest.cpp src/core/sharedfs.cpp src/core/macnames.cpp src/core/m68k.cpp src/core/jit.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/sharedfstest.cpp src/core/sharedfs.cpp src/core/macnames.cpp src/core/m68k.cpp src/core/jit.cpp

$(BUILD)/mmutest: tests/mmutest.cpp src/core/m68k.cpp src/core/mmu040.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -fsanitize=address,undefined -o $@ tests/mmutest.cpp src/core/m68k.cpp src/core/jit.cpp src/core/mmu040.cpp

# Regenerate the video driver (needs binutils-m68k-linux-gnu).
src/core/videodrv.h: src/core/videodrv.s tools/build_videodrv.py
	python3 tools/build_videodrv.py

# Regenerate the 68020+ cases (needs binutils-m68k-linux-gnu).
tests/cpu020_cases.h: tools/gen_cpu020_tests.py
	python3 tools/gen_cpu020_tests.py > $@.tmp && mv $@.tmp $@

$(BUILD)/cputest: tests/cputest.cpp src/core/m68k.cpp src/core/m68k.h | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests/cputest.cpp src/core/m68k.cpp src/core/jit.cpp

$(BUILD)/macemu-host: src/host/main.cpp $(CORE) $(FRONTEND) $(PROXY) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(BEARSSL_INC) -o $@ src/host/main.cpp $(CORE) $(FRONTEND) $(PROXY)

test: $(BUILD)/hwtest $(BUILD)/fetest $(BUILD)/cpu020test $(BUILD)/videotest $(BUILD)/maciitest $(BUILD)/mmutest $(BUILD)/webtest $(BUILD)/hfstest $(BUILD)/nattest $(BUILD)/proxytest $(BUILD)/sharedfstest
	$(BUILD)/hwtest
	$(BUILD)/fetest
	$(BUILD)/cpu020test
	$(BUILD)/videotest
	$(BUILD)/maciitest
	$(BUILD)/mmutest
	$(BUILD)/webtest
	$(BUILD)/hfstest $(HFS_IMAGE)
	$(BUILD)/nattest
	$(BUILD)/proxytest
	$(BUILD)/sharedfstest

# The JIT against the interpreter, on AArch64 (here through qemu-aarch64):
#   make jittest   (needs aarch64-linux-gnu-g++ and qemu-user)
.PHONY: jittest
jittest:
	mkdir -p build-arm64
	aarch64-linux-gnu-g++ -O2 -g -Wall -Wextra -static -o build-arm64/jittest tests/jittest.cpp src/core/m68k.cpp src/core/jit.cpp
	qemu-aarch64 build-arm64/jittest 3

$(BUILD)/vectors.txt: tools/sst2txt.py | $(BUILD)
	python3 tools/sst2txt.py $(SST) $(SST_PER_INSTRUCTION) > $@

cputest: $(BUILD)/cputest $(BUILD)/vectors.txt
	$(BUILD)/cputest $(BUILD)/vectors.txt

clean:
	rm -rf $(BUILD)
