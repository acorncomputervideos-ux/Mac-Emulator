#!/bin/bash
# The JIT against the interpreter over a whole Mac session: boots a Quadra
# ROM and a Mac OS disk twice under qemu-aarch64 (the JIT is ARM code), once
# interpreted and once with the JIT, with a fixed clock, and compares the
# cycle counts, every screenshot and the disk afterwards.  They must be
# identical: the JIT runs the Mac cycle for cycle as the interpreter does.
#
#   tools/jit-system-check.sh ROM DISK [FRAMES] [SHARED-FOLDER]
#
# Extra input (mouse, typing) goes through the host runner's environment
# variables (MOUSE_SCRIPT, TYPE2, ...), the same for both runs.
# Needs aarch64-linux-gnu-g++ and qemu-aarch64 (the ARM runner is built here).
set -e
ROM=$1; DISK=$2; FRAMES=${3:-4000}; SHARE=$4
[ -f "$ROM" ] && [ -f "$DISK" ] || { echo "usage: $0 ROM DISK [FRAMES] [SHARED-FOLDER]"; exit 2; }
TOP=$(cd "$(dirname "$0")/.." && pwd)
cd "$TOP"
make -s -j"$(nproc)" BUILD=build-arm64 CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc \
     CXXFLAGS="-O2 -static -pthread" build-arm64/macemu-host
WORK=$(mktemp -d)
for v in 0 1; do
    cp "$DISK" "$WORK/disk$v"
    SH=()
    if [ -n "$SHARE" ]; then cp -a "$SHARE" "$WORK/share$v"; SH=(--share "$WORK/share$v"); fi
    MACEMU_TIME=1763000000 JIT=$v JIT_STATS=1 qemu-aarch64 build-arm64/macemu-host "$ROM" --ram 8192 \
        --hd0 "$WORK/disk$v" "${SH[@]}" --frames "$FRAMES" --shot-every 500 --out "$WORK/r$v" \
        > "$WORK/out$v" 2>&1 &
done
wait
ok=1
c0=$(grep "^ran " "$WORK/out0"); c1=$(grep "^ran " "$WORK/out1")
echo "interpreter: $c0"; echo "JIT:         $c1"
[ "$c0" = "$c1" ] || { echo "FAIL: the cycle counts differ"; ok=0; }
cmp -s "$WORK/disk0" "$WORK/disk1" || { echo "FAIL: the disks differ"; ok=0; }
for f in "$WORK"/r0-*.ppm; do
    cmp -s "$f" "${f/r0-/r1-}" || { echo "FAIL: $(basename "$f") differs"; ok=0; }
done
grep "^JIT:" "$WORK/out1" || true
[ $ok = 1 ] && echo "identical" && rm -rf "$WORK" && exit 0
echo "(runs kept in $WORK)"; exit 1
