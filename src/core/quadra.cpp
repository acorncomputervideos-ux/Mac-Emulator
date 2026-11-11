#include "quadra.h"

namespace macemu {

Quadra::Quadra()
    : cpu(this), fpu(Fpu::MC68040FPU), mmu(*this), via1(&via1Host), via2(&via2Host),
      asc(Asc::Enhanced), adb(via1), dafb(Dafb::VGA), via1Host(*this), via2Host(*this)
{
    ioLog = 0;
    stepHook = 0;
    logAllIo = false;
    ioLogCtx = 0;
    rom = 0; ram = 0;
    romMask = ramSize = 0;
    powerOff = via2PowerDriven = auxMode = false;
    via1Irq = via2Irq = sccIrq = false;
    videoIrqSeen = espIrqSeen = ascIrqSeen = false;
    nmiFrames = 0;
    viaFrac = 0;
    frameStart = 0;
    frameCount = 0;
    memFill(soundBuf, 0, sizeof soundBuf);
    memFill(djmemc, 0, sizeof djmemc);
    memFill(iosb, 0, sizeof iosb);
    scc.byteCycles = kCpuHz / 28800;                   // 230.4 kbit/s LocalTalk
    cpu.setModel(M68K::MC68040);
    cpu.attachFpu(&fpu);
    cpu.attachMmu(&mmu, &mmu);
    adb.attach(&keyboard);
    adb.attach(&mouse);
}

bool Quadra::init(const u8 *romData, u32 romSize, u8 *ramData, u32 ramBytes)
{
    if (!romData || !ramData || romSize != 0x100000) return false;
    if (ramBytes < 0x400000 || ramBytes > 0x10000000 || (ramBytes & 0xFFFFF)) return false;
    rom = romData; romMask = romSize - 1;
    ram = ramData; ramSize = ramBytes;
    reset();
    return true;
}

void Quadra::resetLine()
{
    via1.reset();
    via2.reset();
    iwm.reset();
    esp.reset();
    asc.reset();
    adb.reset();
    scc.reset();
    memFill(djmemc, 0, sizeof djmemc);
    memFill(iosb, 0, sizeof iosb);
}

void Quadra::reset()
{
    powerOff = false;
    via2PowerDriven = false;
    auxMode = false;
    resetLine();
    dafb.reset();
    mmu.reset();
    // As QEMU does: the reset vectors come from the ROM header, with the
    // initial PC relative to the ROM base.
    putBe32(ram, be32(rom));
    putBe32(ram + 4, 0x40800000 + be32(rom + 4));
    cpu.hardReset();
    updateIrq();
}

// GLUE: in classic mode (VIA1 PB6 set) VIA1 is level 1 and VIA2 level 2;
// in A/UX mode (the reset state) VIA1 is level 6.  SCC is level 4, the
// interrupt switch level 7.
void Quadra::updateIrq()
{
    int level = 0;
    if (via2Irq) level = 2;
    if (sccIrq) level = level > 4 ? level : 4;
    if (via1Irq) level = auxMode ? (level > 1 ? level : 1) : 6 > level ? 6 : level;
    if (nmiFrames > 0) level = 7;
    cpu.setIRQ(level);
}

u8 Quadra::Via1Host::viaInputB()
{
    u8 v = 0xFF;
    if (!m.rtc.dataLine()) v &= (u8)~0x01;
    if (!m.adb.intLine()) v &= (u8)~0x08;
    return v;
}

void Quadra::Via1Host::viaOutputB(u8 pins)
{
    m.rtc.lines(pins);
    m.adb.portB(pins);
    bool aux = (pins & 0x40) != 0;
    if (aux != m.auxMode) { m.auxMode = aux; m.updateIrq(); }
}

// ---------------------------------------------------------------- memory

u8 *Quadra::hostPage(u32 a, bool &writable)
{
    writable = true;
    if (a < 0x40000000) return a + 0x1000 <= ramSize ? ram + a : 0;
    if ((a >> 24) == 0xF9 && (a & 0xFFFFFF) < Dafb::kVramSize) return dafb.vram() + (a & 0xFFFFFF);
    u32 page = a & 0xFFF00000;
    if (page == 0x40000000 || page == 0x40800000) { writable = false; return (u8 *)rom + (a & romMask); }
    return 0;
}

u32 Quadra::busError(u32 addr, bool write, u32 value)
{
    if (ioLog) ioLog(ioLogCtx, write ? "bus error (write)" : "bus error (read)", addr, value);
    signalBusError(addr, write);
    return 0xFFFFFFFF;
}

static inline u32 readBytes(const u8 *p, int size)
{
    return size == 1 ? p[0] : size == 2 ? (u32)be16(p) : be32(p);
}

static inline void writeBytes(u8 *p, int size, u32 v)
{
    if (size == 1) p[0] = (u8)v;
    else if (size == 2) putBe16(p, (u16)v);
    else putBe32(p, v);
}

u32 Quadra::access(u32 a, int size, bool write, u32 v)
{
    switch (a >> 28) {
    case 0: case 1: case 2: case 3:                     // RAM; nothing installed reads as 0
        if (a + (u32)size <= ramSize) {
            if (write) { writeBytes(ram + a, size, v); return 0; }
            return readBytes(ram + a, size);
        }
        return 0;
    case 4: {
        u32 page = a & 0x0FF00000;
        if (page != 0x00800000 && page != 0x00000000) return busError(a, write, v);
        if (write) return 0;
        return readBytes(rom + (a & romMask), size);
    }
    case 5:
        if (a >= 0x5FFFFFFC) return write ? 0 : (size == 4 ? 0xA55A2BAD : 0);
        if (a < 0x54000000) return io(a & 0x3FFFF, size, write, v);
        return busError(a, write, v);
    case 0xF:
        if ((a >> 24) == 0xF9) {
            u32 off = a & 0xFFFFFF;
            if (off < Dafb::kVramSize) {
                if (write) { writeBytes(dafb.vram() + off, size, v); return 0; }
                return readBytes(dafb.vram() + off, size);
            }
            if (off >= 0x800000 && off < 0x800400) {
                if (write) { dafb.writeReg(off, v, size); return 0; }
                return dafb.readReg(off);
            }
        }
        return busError(a, write, v);
    default:
        return busError(a, write, v);
    }
}

// The I/O slice.  Byte-wide chips see consecutive byte accesses when the
// CPU reads or writes words or longs (dynamic bus sizing).
u32 Quadra::io(u32 off, int size, bool write, u32 v)
{
    if (logAllIo && ioLog && size == 1) {
        u32 r = io2(off, write, v);
        ioLog(ioLogCtx, write ? "io write" : "io read ", 0x50000000 | off, write ? v : r);
        return r;
    }
    return io2(off, size, write, v);
}

u32 Quadra::io2(u32 off, bool write, u32 v)
{
    return io2(off, 1, write, v);
}

u32 Quadra::io2(u32 off, int size, bool write, u32 v)
{
    if (size > 1 && !((off >= 0x0E000 && off < 0x10000) || (off >= 0x18000 && off < 0x1A000))) {
        u32 r = 0;
        for (int i = 0; i < size; i++) {
            u32 b = io(off + (u32)i, 1, write, write ? (v >> (8 * (size - 1 - i))) & 0xFF : 0);
            r = (r << 8) | (b & 0xFF);
        }
        return r;
    }
    if (off < 0x2000) {
        if (write) { via1.write((int)(off >> 9) & 15, (u8)v); return 0; }
        return via1.read((int)(off >> 9) & 15);
    }
    if (off < 0x4000) {
        int reg = (int)(off >> 9) & 15;
        if (write) {
            if (reg == 2) via2PowerDriven = (v & 0x04) != 0;   // DDRB: PB2 is an output
            via2.write(reg, (u8)v);
            return 0;
        }
        u8 r = via2.read(reg);
        if (reg == 13) r = (u8)((r & ~0x01) | (esp.drq() ? 0x01 : 0));   // SCSI DRQ is live (CA2)
        return r;
    }
    if (off >= 0x08000 && off < 0x09000) {              // Ethernet address PROM
        static const u8 prom[8] = {0x10, 0x00, 0xE0, 0x00, 0x00, 0x00, 0x00, 0x00};
        u8 b = off < 0x8008 ? prom[off - 0x8000] : 0;
        if ((off & 0xFFF) == 7) {
            u8 sum = 0;
            for (int i = 0; i < 6; i++) sum ^= prom[i];
            b = (u8)(0xFF - sum);
        }
        return write ? 0 : b;
    }
    if (off >= 0x0A000 && off < 0x0A100) return 0;      // SONIC Ethernet: idle
    if ((off >= 0x0C000 && off < 0x0C008) || (off >= 0x0C020 && off < 0x0C040)) {
        int ch = (int)(off >> 1) & 1, data = (int)(off >> 2) & 1;
        if (write) {
            scc.write(ch, data != 0, (u8)v);
            bool si = scc.irq();
            if (si != sccIrq) { sccIrq = si; updateIrq(); }
            return 0;
        }
        return scc.read(ch, data != 0);
    }
    if (off >= 0x0E000 && off < 0x10000) {              // djMEMC
        u32 r = (off & 0x3F) >> 2;
        if (write) { djmemc[r & 15] = v; return 0; }
        return size == 4 ? djmemc[r & 15] : (djmemc[r & 15] >> (8 * (4 - size - (int)(off & 3)))) & (size == 1 ? 0xFF : 0xFFFF);
    }
    if (off >= 0x10000 && off < 0x10100) {
        if (write) { esp.write((int)(off >> 4) & 15, (u8)v); return 0; }
        return esp.read((int)(off >> 4) & 15);
    }
    if (off >= 0x10100 && off < 0x10200) {
        if (write) { esp.dmaWrite((u8)v); return 0; }
        return esp.dmaRead();
    }
    if (off >= 0x14000 && off < 0x16000) {
        if (write) { asc.write(off - 0x14000, (u8)v); return 0; }
        return asc.read(off - 0x14000);
    }
    if (off >= 0x18000 && off < 0x1A000) {              // IOSB
        u32 r = (off >> 8) & 7;
        if (write) { iosb[r] = v; return 0; }
        return iosb[r];
    }
    if (off >= 0x1E000 && off < 0x20000) {              // SWIM in IWM mode
        if (write) { iwm.write((int)(off >> 9) & 15, (u8)v, cpu.totalCycles / 4); return 0; }
        return iwm.read((int)(off >> 9) & 15, cpu.totalCycles / 4);
    }
    return busError(0x50000000 | off, write, v);
}

// ---------------------------------------------------------------- run loop

void Quadra::runFrame()
{
    u64 end = frameStart + kCyclesPerFrame;
    u64 nextSample = frameStart;
    int sample = 0;
    bool midPoll = false;                               // ADB autopolls about every 8 ms
    while (cpu.totalCycles < end) {
        // Run a short batch of instructions, then service the devices: a
        // couple of microseconds of latency, far less overhead.
        u32 c = 0;
        for (int i = 0; i < 16 && cpu.totalCycles < end; i++) {
            if (stepHook) { cpu.instPC = cpu.pc; stepHook(); }
            c += (u32)cpu.step();
        }
        // VIA E clock: 783360 Hz from a 33.33 MHz CPU clock, kept as a fraction.
        viaFrac += c * 783360u;
        if (viaFrac >= (u32)kCpuHz) {
            u32 ticks = viaFrac / (u32)kCpuHz;
            via1.tick((int)ticks);
            via2.tick((int)ticks);
            viaFrac -= ticks * (u32)kCpuHz;
        }
        if (scc.sending()) {
            scc.tick(c);
            bool si = scc.irq();
            if (si != sccIrq) { sccIrq = si; updateIrq(); }
        }
        if (!midPoll && cpu.totalCycles >= frameStart + kCyclesPerFrame / 2) { midPoll = true; adb.tick(); }
        while (cpu.totalCycles >= nextSample && sample < kSamplesPerFrame) {
            soundBuf[sample++] = asc.sample();
            nextSample += kCyclesPerFrame / kSamplesPerFrame;
        }
        bool e = esp.irq();                             // SCSI interrupt: VIA2 CB2
        if (e && !espIrqSeen) via2.raise(Via::IRQ_CB2);
        espIrqSeen = e;
        bool sIrq = asc.irq();                          // sound: VIA2 CB1 in classic mode
        if (sIrq && !ascIrqSeen) via2.raise(Via::IRQ_CB1);
        ascIrqSeen = sIrq;
        bool v = dafb.irq();                            // built-in video: VIA2 CA1
        if (v && !videoIrqSeen) via2.raise(Via::IRQ_CA1);
        videoIrqSeen = v;
    }
    while (sample < kSamplesPerFrame) soundBuf[sample++] = asc.sample();
    frameStart = end;

    via1.raise(Via::IRQ_CA1);                           // 60.15 Hz
    dafb.vbl();
    adb.tick();
    if (++frameCount % 60 == 0) {
        rtc.tickSecond();
        via1.raise(Via::IRQ_CA2);
    }
    if (nmiFrames > 0) { nmiFrames--; updateIrq(); }
}

} // namespace macemu
