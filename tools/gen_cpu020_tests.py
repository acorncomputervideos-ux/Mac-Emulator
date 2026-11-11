#!/usr/bin/env python3
"""Generate tests/cpu020_cases.h: hand-written 68020/68030/68040 test cases.

Each case is a short assembly snippet (Motorola syntax, register prefix
optional), the initial register state and the expected outcome worked out
from the Motorola M68000 Family Programmer's Reference Manual.  Snippets end
with ILLEGAL, so a normal run finishes in vector 4.

Needs GNU binutils for m68k (Debian/Ubuntu: binutils-m68k-linux-gnu).
The generated header is committed, so building the tests does not.

    tools/gen_cpu020_tests.py > tests/cpu020_cases.h
"""
import os, subprocess, sys, tempfile

AS = os.environ.get("M68K_AS", "m68k-linux-gnu-as")
LD = os.environ.get("M68K_LD", "m68k-linux-gnu-ld")
OBJCOPY = os.environ.get("M68K_OBJCOPY", "m68k-linux-gnu-objcopy")
NM = os.environ.get("M68K_NM", "m68k-linux-gnu-nm")
ORG = 0x1000
SSP = 0x8000

def u32(v): return v & 0xFFFFFFFF

def ccr(x=0, n=0, z=0, v=0, c=0): return (x << 4) | (n << 3) | (z << 2) | (v << 1) | c

# Bit field helpers written independently of the emulator: treat memory as
# a string of bits.
def bits_of(bs): return "".join(format(b, "08b") for b in bs)
def bytes_of(bits): return bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))

CASES = []

def case(name, asm, init=None, expect=None, model=2, sr=0x2700):
    CASES.append(dict(name=name, asm=asm, init=init or {}, expect=expect or {}, model=model, sr=sr))

# ------------------------------------------------------------ addressing modes

case("scaled index", "lea (8,a0,d1.l*4),a2", {"a0": 0x2000, "d1": 3}, {"a2": 0x2014})
case("full format base displacement", "lea (0x12345678,a0,d1.w*2),a2",
     {"a0": 0x1000, "d1": 0xFFFF0003}, {"a2": 0x1234667E})
case("full format, memory indirect absolute", """
    move.l #0x00001357,0x6000
    lea ([0x6000]),a2""", {}, {"a2": 0x00001357})
case("memory indirect pre-indexed", """
    move.l #0x4000,0x3014
    move.l #0xCAFEBABE,0x4020
    move.l ([0x10,a0,d1.l*4],0x20),d2""", {"a0": 0x3000, "d1": 1}, {"d2": 0xCAFEBABE})
case("memory indirect post-indexed", """
    move.l #0x5000,0x3010
    move.l #0x11223344,0x5028
    move.l ([0x10,a0],d1.l*4,0x20),d2""", {"a0": 0x3000, "d1": 2}, {"d2": 0x11223344})
case("PC memory indirect", """
    move.l ([ptr,pc]),d3
    illegal
ptr: .long target
target: .long 0x55667788""", {}, {"d3": 0x55667788})
case("misaligned long access", """
    move.l #0x11223344,0x6001
    move.l 0x6001,d0
    move.w 0x6003,d1""", {}, {"d0": 0x11223344, "d1": 0x3344, "mem8": [(0x6001, 0x11), (0x6004, 0x44)]})

# ------------------------------------------------------------ branches, misc

case("BRA.L", """
    .word 0x60FF
    .long target-.
    moveq #2,d0
    illegal
target:
    moveq #1,d0""", {}, {"d0": 1})
case("BSR.L", """
    .word 0x61FF
    .long sub-.
back: illegal
sub: move.l (sp),d1""", {}, {"d1": "back", "a7": SSP - 4 - 8})
case("EXTB.L", "extb.l d0", {"d0": 0x12345680}, {"d0": 0xFFFFFF80, "ccr": ccr(n=1)})
case("LINK.L", "link.l a6,#-0x12344", {"a6": 0x11111111, "a7": 0x40000},
     {"a6": 0x3FFFC, "a7": 0x3FFFC - 0x12344 - 8, "mem32": [(0x3FFFC, 0x11111111)]})
case("RTD", """
    pea back
    move.l #0x12345678,-(sp)
    move.l #0x9ABCDEF0,-(sp)
    move.l 8(sp),-(sp)
    rtd #8
    moveq #9,d0
back: illegal""", {}, {"a7": SSP - 4 - 8, "d0": 0})
case("TST.L An (68020)", "tst.l a0", {"a0": 0x80000000}, {"ccr": ccr(n=1)})
case("CMPI PC-relative (68020)", """
    cmpi.w #0x1234,(cval,pc)
    illegal
cval: .word 0x1234""", {}, {"ccr": ccr(z=1)})
case("MOVE from CCR", "move.w ccr,d0", {"d0": 0xFFFFFFFF}, {"d0": 0xFFFF001F}, sr=0x271F)

# ------------------------------------------------------------ multiply / divide

case("MULU.L 32-bit overflow", "mulu.l d0,d1", {"d0": 0x10000, "d1": 0x10001},
     {"d1": 0x00010000, "ccr": ccr(v=1)})
case("MULS.L 64-bit", "muls.l d0,d2:d1", {"d0": u32(-3), "d1": 0x40000000},
     {"d2": 0xFFFFFFFF, "d1": 0x40000000, "ccr": ccr(n=1)})
case("MULU.L 64-bit", "mulu.l d0,d2:d1", {"d0": 0xFFFFFFFF, "d1": 0xFFFFFFFF},
     {"d2": 0xFFFFFFFE, "d1": 1, "ccr": ccr(n=1)})
case("MULS.L 32-bit no overflow", "muls.l d0,d1", {"d0": u32(-7), "d1": 1000},
     {"d1": u32(-7000), "ccr": ccr(n=1)})
case("DIVU.L 32-bit", "divu.l d0,d1", {"d0": 7, "d1": 100}, {"d1": 14, "ccr": ccr()})
case("DIVUL.L remainder", "divul.l d0,d2:d1", {"d0": 7, "d1": 100}, {"d1": 14, "d2": 2})
q, r = -1000000000, -7
dvd = u32(-100000000007 >> 32), u32(-100000000007)
case("DIVS.L 64-bit", "divs.l d0,d2:d1", {"d0": 100, "d2": dvd[0], "d1": dvd[1]},
     {"d1": u32(q), "d2": u32(r), "ccr": ccr(n=1)})
case("DIVU.L 64-bit overflow", "divu.l d0,d2:d1", {"d0": 1, "d2": 1, "d1": 0},
     {"d2": 1, "d1": 0, "vbits": ("ccr", 0x02, 0x02)})
case("DIVS.L by zero", """
    divs.l d0,d1
after: illegal""", {"d0": 0, "d1": 5},
     {"vector": 5, "a7": SSP - 12, "mem16": [(SSP - 12 + 6, 0x2014)],
      "mem32": [(SSP - 12 + 2, "after"), (SSP - 12 + 8, ORG)]})

# ------------------------------------------------------------ bounds checks

case("CHK.L out of range (negative)", "chk.l d0,d1", {"d0": 5, "d1": 0x80000000},
     {"vector": 6, "mem16": [(SSP - 12 + 6, 0x2018)], "vbits": ("ccr", 0x08, 0x08)})
case("CHK.L in range", "chk.l d0,d1", {"d0": 5, "d1": 3}, {"vector": 4})
BOUNDS = """
    move.w #10,0x6000
    move.w #20,0x6002
    move.w #0xFFFE,0x6004
    move.w #5,0x6006
    lea 0x6000,a0
"""
case("CMP2.W in range", BOUNDS + "cmp2.w (a0),d1", {"d1": 0xFFFF000F}, {"vbits": ("ccr", 0x05, 0x00)})
case("CMP2.W equal to lower bound", BOUNDS + "cmp2.w (a0),d1", {"d1": 10}, {"vbits": ("ccr", 0x05, 0x04)})
case("CMP2.W above", BOUNDS + "cmp2.w (a0),d1", {"d1": 25}, {"vbits": ("ccr", 0x05, 0x01)})
case("CMP2.W An with sign-extended bounds", BOUNDS + "cmp2.w (4,a0),a1", {"a1": 0xFFFFFFFF},
     {"vbits": ("ccr", 0x05, 0x00)})
case("CMP2.W An below", BOUNDS + "cmp2.w (4,a0),a1", {"a1": 0xFFFFFFF0}, {"vbits": ("ccr", 0x05, 0x01)})
case("CHK2.B out of range", """
    move.b #1,0x6000
    move.b #9,0x6001
    chk2.b 0x6000,d1""", {"d1": 0x0C}, {"vector": 6, "mem16": [(SSP - 12 + 6, 0x2018)]})

# ------------------------------------------------------------ CAS / CAS2

case("CAS.L equal", """
    move.l #0x1234,0x6000
    cas.l d0,d1,0x6000""", {"d0": 0x1234, "d1": 0x5678},
     {"mem32": [(0x6000, 0x5678)], "vbits": ("ccr", 0x04, 0x04)})
case("CAS.L not equal", """
    move.l #0x1234,0x6000
    cas.l d0,d1,0x6000""", {"d0": 0x1111, "d1": 0x5678},
     {"mem32": [(0x6000, 0x1234)], "d0": 0x1234, "ccr": ccr()})
case("CAS.W leaves upper word", """
    move.w #0xAAAA,0x6000
    cas.w d0,d1,0x6000""", {"d0": 0x77770001, "d1": 0},
     {"d0": 0x7777AAAA, "ccr": ccr(n=1)})
CAS2_SETUP = """
    move.l #100,0x6000
    move.l #200,0x6010
    lea 0x6000,a0
    lea 0x6010,a1
"""
case("CAS2.L success", CAS2_SETUP + "cas2.l d0:d1,d2:d3,(a0):(a1)",
     {"d0": 100, "d1": 200, "d2": 111, "d3": 222},
     {"mem32": [(0x6000, 111), (0x6010, 222)], "vbits": ("ccr", 0x04, 0x04)})
case("CAS2.L second compare fails", CAS2_SETUP + "cas2.l d0:d1,d2:d3,(a0):(a1)",
     {"d0": 100, "d1": 201, "d2": 111, "d3": 222},
     {"mem32": [(0x6000, 100), (0x6010, 200)], "d0": 100, "d1": 200, "vbits": ("ccr", 0x04, 0x00)})

# ------------------------------------------------------------ bit fields

case("BFEXTU register", "bfextu d0{#4:#8},d1", {"d0": 0x12345678}, {"d1": 0x23, "ccr": ccr()})
case("BFEXTS register negative", "bfexts d0{#0:#4},d1", {"d0": 0xF2345678}, {"d1": 0xFFFFFFFF, "ccr": ccr(n=1)})
case("BFEXTU register wraps", "bfextu d0{#28:#8},d1", {"d0": 0x12345678}, {"d1": 0x81})
case("BFEXTU width 32 from register", "bfextu d0{d2:d3},d1", {"d0": 0x12345678, "d2": 8, "d3": 0},
     {"d1": 0x34567812})
case("BFINS register", "bfins d1,d0{#8:#8}", {"d0": 0x12345678, "d1": 0xAB},
     {"d0": 0x12AB5678, "ccr": ccr(n=1)})
case("BFCLR register", "bfclr d0{#0:#4}", {"d0": 0xF2345678}, {"d0": 0x02345678, "ccr": ccr(n=1)})
case("BFSET register zero", "bfset d0{#24:#8}", {"d0": 0x12345600}, {"d0": 0x123456FF, "ccr": ccr(z=1)})
case("BFFFO register", "bfffo d0{#0:#32},d1", {"d0": 0x00010000}, {"d1": 15})
case("BFFFO none set", "bfffo d0{#4:#8},d1", {"d0": 0}, {"d1": 12, "ccr": ccr(z=1)})

MEM = bytes([0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC])
MEMSET = """
    move.l #0x12345678,0x6000
    move.w #0x9ABC,0x6004
"""
def bf(offset, width, base=0x6000, data=MEM, start=0x6000):
    b = bits_of(data)
    pos = (base - start) * 8 + offset
    return b, pos, b[pos:pos + width]

b, pos, f = bf(4, 32)
case("BFEXTU memory, five bytes", MEMSET + "bfextu 0x6000{#4:#32},d1", {}, {"d1": int(f, 2)})
b, pos, f = bf(-8, 8, base=0x6001)
case("BFEXTU memory, negative offset", MEMSET + "lea 0x6001,a0\n bfextu (a0){d2:#8},d1",
     {"d2": u32(-8)}, {"d1": int(f, 2)})
b, pos, f = bf(12, 16)
nb = b[:pos] + "".join("1" if c == "0" else "0" for c in f) + b[pos + 16:]
after = bytes_of(nb)
case("BFCHG memory", MEMSET + "bfchg 0x6000{#12:#16}", {},
     {"mem8": [(0x6000 + i, after[i]) for i in range(6)], "ccr": ccr()})
b, pos, f = bf(30, 9)
ins = format(0x1F5 & 0x1FF, "09b")
nb = b[:pos] + ins + b[pos + 9:]
after = bytes_of(nb)
case("BFINS memory across five bytes", MEMSET + "bfins d0,0x6000{#30:#9}", {"d0": 0x1F5},
     {"mem8": [(0x6000 + i, after[i]) for i in range(6)], "ccr": ccr(n=1)})
case("BFTST memory", MEMSET + "bftst 0x6000{#0:#3}", {}, {"ccr": ccr(z=1)})

# ------------------------------------------------------------ PACK / UNPK

case("PACK register", "pack d0,d1,#0", {"d0": 0x0305, "d1": 0xFFFFFFFF}, {"d1": 0xFFFFFF35})
case("PACK register with adjustment", "pack d0,d1,#0x0101", {"d0": 0x0305}, {"d1": 0x46})
case("PACK memory", """
    move.w #0x3132,0x6000
    lea 0x6002,a0
    lea 0x6011,a1
    pack -(a0),-(a1),#0xFFD0""", {}, {"mem8": [(0x6010, 0x12)], "a0": 0x6000, "a1": 0x6010})
case("UNPK register", "unpk d0,d1,#0x3030", {"d0": 0x47, "d1": 0xFFFF0000}, {"d1": 0xFFFF3437})
case("UNPK memory", """
    move.b #0x59,0x6000
    lea 0x6001,a0
    lea 0x6012,a1
    unpk -(a0),-(a1),#0x3030""", {}, {"mem8": [(0x6010, 0x35), (0x6011, 0x39)], "a1": 0x6010})

# ------------------------------------------------------------ traps and exceptions

case("TRAPEQ taken", "trapeq", {}, {"vector": 7, "mem16": [(SSP - 12 + 6, 0x201C)]}, sr=0x2704)
case("TRAPNE.W not taken", "trapne.w #1234\n moveq #3,d0", {}, {"vector": 4, "d0": 3}, sr=0x2704)
case("TRAP format 0", "trap #3", {}, {"vector": 35, "a7": SSP - 8, "mem16": [(SSP - 8 + 6, 0x008C)]})
case("RTE format 2", """
    move.l #ORG_ADDR,-(sp)
    move.w #0x2018,-(sp)
    pea back
    move.w #0x2700,-(sp)
    rte
    moveq #1,d0
back: illegal""".replace("ORG_ADDR", str(ORG)), {}, {"a7": SSP - 8, "d0": 0})
case("RTE bad format", """
    move.w #0x5018,-(sp)
    pea 0
    move.w #0x2700,-(sp)
    rte""", {}, {"vector": 14})
case("MOVE from SR is privileged", "move.w sr,d0", {}, {"vector": 8}, sr=0x0000)
case("MOVEC VBR", """
    move.l #0x800+32*4,0x400+32*4
    movec d0,vbr
    movec vbr,d1
    trap #0""", {"d0": 0x400}, {"d1": 0x400, "vector": 32})
case("MOVEC CACR 68020", "movec d0,cacr\n movec cacr,d1", {"d0": 0xFFFFFFFF}, {"d1": 3})
case("MOVEC CACR 68040", "movec d0,cacr\n movec cacr,d1", {"d0": 0xFFFFFFFF}, {"d1": 0x80008000}, model=4)
case("MOVEC USP", "movec d0,usp\n move.l usp,a1", {"d0": 0x5555}, {"a1": 0x5555})
case("MOVEC is privileged", "movec vbr,d0", {}, {"vector": 8}, sr=0x0000)
case("MOVES.L", "moves.l d0,(a0)", {"d0": 0xDEADBEEF, "a0": 0x6000}, {"mem32": [(0x6000, 0xDEADBEEF)]})
case("Bus error frame 68020", "move.l 0x90000004,d0", {},
     {"vector": 2, "a7": SSP - 32, "mem16": [(SSP - 32 + 6, 0xA008)], "mem32": [(SSP - 32 + 16, 0x90000004)]})
case("Bus error frame 68040", "move.l 0x90000004,d0", {},
     {"vector": 2, "a7": SSP - 60, "mem16": [(SSP - 60 + 6, 0x7008)], "mem32": [(SSP - 60 + 20, 0x90000004)]},
     model=4)
case("Line F without FPU", ".word 0xF200,0x0000", {}, {"vector": 11})
case("MOVE16 (Ax)+,(Ay)+", """
    move.l #0x01020304,0x6000
    move.l #0x05060708,0x6004
    move.l #0x090A0B0C,0x6008
    move.l #0x0D0E0F10,0x600C
    lea 0x6004,a0
    lea 0x6100,a1
    move16 (a0)+,(a1)+""", {}, {"mem32": [(0x6100, 0x01020304), (0x610C, 0x0D0E0F10)],
                                 "a0": 0x6014, "a1": 0x6110}, model=4)
case("MOVE16 is a line F trap on 68020", ".word 0xF620,0x9000", {}, {"vector": 11})
case("M bit selects the master stack", """
    movec d0,msp
    move.w #0x3700,sr
    move.l sp,d1
    trap #1""", {"d0": 0x9000}, {"d1": 0x9000, "vector": 33, "a7": 0x9000 - 8})

CASES = [c for c in CASES if c]

def assemble(asm, workdir):
    src = os.path.join(workdir, "t.s")
    with open(src, "w") as f:
        f.write("    .text\n    .globl _start\n_start:\n" + asm + "\n    illegal\n")
    obj = os.path.join(workdir, "t.o")
    elf = os.path.join(workdir, "t.elf")
    binf = os.path.join(workdir, "t.bin")
    subprocess.run([AS, "-m68040", "--register-prefix-optional", "-o", obj, src], check=True)
    subprocess.run([LD, "-Ttext=0x%x" % ORG, "-o", elf, obj], check=True)
    subprocess.run([OBJCOPY, "-O", "binary", "-j", ".text", elf, binf], check=True)
    syms = {}
    for line in subprocess.run([NM, elf], check=True, capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    return open(binf, "rb").read(), syms

def value(v, syms):
    return syms[v] if isinstance(v, str) else u32(v)

REGS = ["d%d" % i for i in range(8)] + ["a%d" % i for i in range(8)]

def main():
    out = ["// Generated by tools/gen_cpu020_tests.py - do not edit.", ""]
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for n, c in enumerate(CASES):
            code, syms = assemble(c["asm"], tmp)
            out.append("static const unsigned char code%d[] = {%s};" % (n, ",".join(str(b) for b in code)))
            init = []
            for k, v in c["init"].items():
                init.append("{%d,0x%08X}" % (REGS.index(k), value(v, syms)))
            exp = []
            e = c["expect"]
            for k, v in e.items():
                if k in REGS:
                    exp.append("{X_REG,%d,0,0x%08X,0xFFFFFFFF}" % (REGS.index(k), value(v, syms)))
                elif k == "ccr":
                    exp.append("{X_CCR,0,0,0x%02X,0x1F}" % v)
                elif k == "vbits":
                    exp.append("{X_CCR,0,0,0x%02X,0x%02X}" % (v[2], v[1]))
                elif k == "vector":
                    exp.append("{X_VECTOR,0,0,%d,0}" % v)
                elif k in ("mem8", "mem16", "mem32"):
                    size = {"mem8": 1, "mem16": 2, "mem32": 4}[k]
                    for addr, val in v:
                        exp.append("{X_MEM,%d,0x%X,0x%08X,0}" % (size, addr, value(val, syms)))
            if "vector" not in e:
                exp.append("{X_VECTOR,0,0,4,0}")
            out.append("static const Init init%d[] = {%s{-1,0}};" % (n, "".join(i + "," for i in init)))
            out.append("static const Expect exp%d[] = {%s{X_END,0,0,0,0}};" % (n, "".join(x + "," for x in exp)))
            rows.append('    {"%s", %d, 0x%04X, code%d, %d, init%d, exp%d},' % (
                c["name"], c["model"], c["sr"], n, len(code), n, n))
    out.append("")
    out.append("static const Case kCases[] = {")
    out += rows
    out.append("};")
    print("\n".join(out))

if __name__ == "__main__":
    main()
