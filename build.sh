#!/bin/sh
# Build the Mac Emulator kernel for a Raspberry Pi model.
#
#   ./build.sh [MODEL]      MODEL: pi3 (default), pi4, pi5, pi3-32, pi2, pi1, pizero2
#   ./build.sh firmware     download the Pi boot firmware into ./sdcard
#   ./build.sh all          firmware plus every 64-bit model
#
# Downloads Circle into ./circle on first use, builds it with the SD card
# and FAT file system add-ons, then builds the kernel image into ./sdcard.
#
# Needs a cross compiler: aarch64-none-elf- (Arm GNU toolchain) or
# aarch64-linux-gnu- for 64-bit models, arm-none-eabi- for 32-bit ones.
# Override with PREFIX=... if yours has another name.
set -e

MODEL=${1:-pi3}

if [ "$MODEL" = all ]; then
    "$0" firmware && "$0" pi3 && "$0" pi4 && "$0" pi5
    exit $?
fi
CIRCLE_TAG=${CIRCLE_TAG:-Step51.1.1}
TOP=$(cd "$(dirname "$0")" && pwd)
CIRCLE="$TOP/circle"

case "$MODEL" in
    firmware)   RASPPI=4; AARCH=64; IMG= ;;
    pi1|pizero) RASPPI=1; AARCH=32; IMG=kernel.img ;;
    pi2)        RASPPI=2; AARCH=32; IMG=kernel7.img ;;
    pi3-32)     RASPPI=3; AARCH=32; IMG=kernel8-32.img ;;
    pi3|pizero2) RASPPI=3; AARCH=64; IMG=kernel8.img ;;
    pi4)        RASPPI=4; AARCH=64; IMG=kernel8-rpi4.img ;;
    pi5)        RASPPI=5; AARCH=64; IMG=kernel_2712.img ;;
    *) echo "unknown model $MODEL"; exit 2 ;;
esac

if [ -z "$PREFIX" ]; then
    if [ "$AARCH" = 64 ]; then
        for p in aarch64-none-elf- aarch64-linux-gnu-; do
            if command -v ${p}gcc >/dev/null 2>&1; then PREFIX=$p; break; fi
        done
    else
        PREFIX=arm-none-eabi-
    fi
fi
command -v ${PREFIX}gcc >/dev/null 2>&1 || { echo "cross compiler ${PREFIX}gcc not found"; exit 1; }

if [ ! -d "$CIRCLE" ]; then
    git clone --depth 1 --branch "$CIRCLE_TAG" https://github.com/rsta2/circle.git "$CIRCLE"
fi
# The Wi-Fi driver's WPA supplicant is a submodule of Circle.
if [ ! -f "$CIRCLE/addon/wlan/hostap/wpa_supplicant/Makefile.circle" ]; then
    git -C "$CIRCLE" submodule update --init --depth 1 addon/wlan/hostap
fi

STAMP="$CIRCLE/.macemu-$MODEL"
if [ ! -f "$STAMP" ]; then
    cd "$CIRCLE"
    rm -f .macemu-*
    ./configure -r $RASPPI -p "$PREFIX" -f
    # Linux-targeted compilers default to outline atomics, which need libc.
    case "$PREFIX" in *linux*) echo 'CFLAGS += -mno-outline-atomics' >> Config.mk ;; esac
    ./makeall clean >/dev/null
    make -C addon/fatfs clean >/dev/null
    make -C addon/SDCard clean >/dev/null
    make -C addon/wlan clean >/dev/null
    make -C addon/wlan/hostap/wpa_supplicant -f Makefile.circle clean >/dev/null
    ./makeall -j"$(nproc)"
    make -C addon/fatfs -j"$(nproc)"
    make -C addon/SDCard -j"$(nproc)"
    make -C addon/wlan -j"$(nproc)"
    make -C addon/wlan/hostap/wpa_supplicant -f Makefile.circle -j"$(nproc)"
    touch "$STAMP"
fi

if [ "$MODEL" = firmware ]; then
    make -C "$CIRCLE/boot" firmware armstub64
    mkdir -p "$TOP/sdcard"
    cd "$CIRCLE/boot"
    cp bootcode.bin start.elf fixup.dat start4.elf fixup4.dat LICENCE.broadcom armstub8-rpi4.bin \
       bcm2710-rpi-zero-2-w.dtb bcm2711-rpi-4-b.dtb bcm2711-rpi-400.dtb bcm2711-rpi-cm4.dtb \
       bcm2712-rpi-5-b.dtb bcm2712d0-rpi-5-b.dtb bcm2712-rpi-500.dtb "$TOP/sdcard/"
    mkdir -p "$TOP/sdcard/overlays"
    cp bcm2712d0.dtbo "$TOP/sdcard/overlays/"
    "$TOP/tools/fetch_wifi_firmware.sh" "$TOP/sdcard/firmware"
    echo "Firmware copied to sdcard/"
    exit 0
fi

cd "$TOP/src/circle"
rm -f ../core/*.o ../frontend/*.o *.o
make CIRCLEHOME="$CIRCLE" -j"$(nproc)"
mkdir -p "$TOP/sdcard"
cp "$IMG" "$TOP/sdcard/"
echo "Built sdcard/$IMG"
