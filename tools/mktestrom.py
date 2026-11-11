#!/usr/bin/env python3
"""Build a tiny self-test ROM (no Apple code).

    tools/mktestrom.py selftest.rom           128K Mac Plus ROM
    tools/mktestrom.py --macii colour.rom     256K Mac II ROM

The Plus ROM switches the overlay off, paints a checkerboard and counts
vertical-blank interrupts.  The Mac II ROM opens the emulator's NuBus video
card through its driver interface, selects 8-bit colour, loads a 256-colour
table and draws a colour pattern.  Use them to check the video, timing and
build pipeline without a real Macintosh ROM.
"""
import struct, sys

def words(*w):
    return b"".join(struct.pack(">H", x) for x in w)

# Mac II program (tools/selftest_colour.s in the commit history; assembled
# with m68k-linux-gnu-as and linked at $40800000).
MACII = bytes.fromhex(
    "0001000040800010000000000000000046fc270021fcf9000000202a45f9f90ffff0257c"
    "00002000000824bc0000000131fc0002301a21fc00003100301c31fc0083310042783106"
    "257c00003000000424bc0000000241f84000700030c03200024100e0e14930c13200e749"
    "024100e0e14930c13200ed49024100c0e14930c152400c40010066d431fc0003301a21fc"
    "00003200301c21fc0000400032004278320431fc00ff320624bc0000000241f9f9000000"
    "740076003003e4483202e249d04110c052430c43028066ec52420c4201e066e260fe")

if len(sys.argv) > 1 and sys.argv[1] == "--macii":
    rom = bytearray(b"\xff" * 0x40000)
    rom[:len(MACII)] = MACII
    open(sys.argv[2] if len(sys.argv) > 2 else "colour.rom", "wb").write(rom)
    sys.exit(0)

rom = bytearray(b"\xff" * 0x20000)
rom[0:8] = words(0x0000, 0x2000, 0x0040, 0x0010)        # SSP, PC
code = words(
    0x13FC, 0x0060, 0x00EF, 0xE3FE,     # move.b #$60,vBufA      overlay off
    0x13FC, 0x00FF, 0x00EF, 0xE7FE,     # move.b #$FF,vDirA
    0x31FC, 0x1234, 0x0100,             # move.w #$1234,$100.w
    0x41F9, 0x003F, 0xA700,             # lea    $3FA700,a0      screen base (4 MB)
    0x7400 | 170,                       # moveq  #170,d2         row pairs - 1
    # rowpair:
    0x223C, 0xF0F0, 0xF0F0,             # move.l #$F0F0F0F0,d1
    0x7001,                             # moveq  #1,d0           two passes: pattern, inverse
    # pass:
    0x7600 | 63,                        # moveq  #63,d3          4 rows x 16 longs - 1
    0x20C1,                             # move.l d1,(a0)+
    0x51CB, 0xFFFC,                     # dbra   d3,*-2
    0x4681,                             # not.l  d1
    0x51C8, 0xFFF4,                     # dbra   d0,pass
    0x51CA, 0xFFE8,                     # dbra   d2,rowpair
    0x21FC, 0x0040, 0x0080, 0x0064,     # move.l #$400080,$64.w  level 1 autovector
    0x13FC, 0x0082, 0x00EF, 0xFDFE,     # move.b #$82,vIER       enable VBL (CA1)
    0x46FC, 0x2000,                     # move   #$2000,sr
    0x60FE,                             # bra.s  *
)
rom[0x10:0x10 + len(code)] = code
isr = words(
    0x5278, 0x0102,                     # addq.w #1,$102.w
    0x4A39, 0x00EF, 0xE3FE,             # tst.b  vBufA           acknowledge
    0x4E73,                             # rte
)
rom[0x80:0x80 + len(isr)] = isr
open(sys.argv[1] if len(sys.argv) > 1 else "selftest.rom", "wb").write(rom)
