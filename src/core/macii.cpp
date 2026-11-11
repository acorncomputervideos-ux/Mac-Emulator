#include "macii.h"

namespace macemu {

MacII::MacII(int videoWidth, int videoHeight)
    : cpu(this), fpu(Fpu::MC68881), via1(&via1Host), via2(&via2Host), asc(Asc::Original),
      adb(via1), card(kVideoSlot, videoWidth, videoHeight, *this),
      via1Host(*this), via2Host(*this)
{
    rom = 0; ram = 0;
    romMask = ramSize = ramMask = 0;
    overlay = true; mode32 = true; powerOff = false;
    via1Irq = via2Irq = slotIrqSeen = false;
    nmiFrames = 0;
    viaFrac = 0;
    frameStart = 0;
    frameCount = 0;
    memFill(soundBuf, 0, sizeof soundBuf);
    memFill(sccPointer, 0, sizeof sccPointer);
    memFill(sccWr, 0, sizeof sccWr);
    cpu.setModel(M68K::MC68020);
    cpu.attachFpu(&fpu);
    adb.attach(&keyboard);
    adb.attach(&mouse);
    asc.onIrq = ascIrq;
    asc.irqCtx = this;
    // 1 = 32-bit mode at power-on: VIA2 port B pins float high until set as outputs.
}

static bool isPow2(u32 v) { return v && !(v & (v - 1)); }

bool MacII::init(const u8 *romData, u32 romSize, u8 *ramData, u32 ramBytes)
{
    if (!romData || !ramData || !isPow2(romSize) || romSize < 0x40000 || romSize > 0x100000) return false;
    if (!isPow2(ramBytes) || ramBytes < 0x100000 || ramBytes > 0x10000000) return false;
    rom = romData; romMask = romSize - 1;
    ram = ramData; ramSize = ramBytes; ramMask = ramBytes - 1;
    reset();
    return true;
}

void MacII::resetLine()
{
    via1.reset();
    via2.reset();
    iwm.reset();
    scsi.reset();
    asc.reset();
    adb.reset();
    memFill(sccPointer, 0, sizeof sccPointer);
    memFill(sccWr, 0, sizeof sccWr);
}

void MacII::reset()
{
    overlay = true;
    mapGeneration++;
    powerOff = false;
    resetLine();
    cpu.hardReset();
    updateIrq();
}

void MacII::updateIrq()
{
    int level = 0;
    if (via1Irq) level = 1;
    if (via2Irq) level = 2;
    if (nmiFrames > 0) level = 7;
    cpu.setIRQ(level);
}

void MacII::ascIrq(void *ctx)
{
    static_cast<MacII *>(ctx)->via2.raise(Via::IRQ_CB1);
}

u8 MacII::Via1Host::viaInputB()
{
    u8 v = 0xFF;
    if (!m.rtc.dataLine()) v &= (u8)~0x01;
    if (!m.adb.intLine()) v &= (u8)~0x08;
    return v;
}

void MacII::Via2Host::viaOutputB(u8 pins)
{
    bool m32 = (pins & 0x08) != 0;                      // PB3: 32-bit addressing
    if (m32 != m.mode32) { m.mode32 = m32; m.mapGeneration++; }
    if (!(pins & 0x04)) m.powerOff = true;              // PB2 low: soft power off
}

// ---------------------------------------------------------------- memory

u32 MacII::translate(u32 a) const
{
    if (mode32) return a;
    a &= 0xFFFFFF;
    if (a < 0x800000) return a;
    if (a < 0x900000) return 0x40000000 | a;
    if (a < 0xF00000) return 0xF0000000 | ((a & 0xF00000) << 4) | (a & 0xFFFFF);
    return 0x50000000 | a;
}

u8 MacII::io8(u32 off)
{
    off &= 0x1FFFF;
    if (off < 0x2000) return via1.read((int)(off >> 9) & 15);
    if (off < 0x4000) return via2.read((int)(off >> 9) & 15);
    if (off < 0x6000) return sccRead((int)(off >> 1) & 1, (off & 4) != 0);
    if (off < 0x8000) return scsi.read(6, true);        // pseudo-DMA
    if (off >= 0x10000 && off < 0x12000) return scsi.read((int)(off >> 4) & 7, false);
    if (off >= 0x12000 && off < 0x14000) return scsi.read(6, true);
    if (off >= 0x14000 && off < 0x16000) return asc.read(off - 0x14000);
    if (off >= 0x16000 && off < 0x18000) return iwm.read((int)(off >> 9) & 15, cpu.totalCycles / 2);
    return 0xFF;
}

void MacII::ioWrite8(u32 off, u8 v)
{
    off &= 0x1FFFF;
    if (off < 0x2000) via1.write((int)(off >> 9) & 15, v);
    else if (off < 0x4000) via2.write((int)(off >> 9) & 15, v);
    else if (off < 0x6000) sccWrite((int)(off >> 1) & 1, (off & 4) != 0, v);
    else if (off < 0x8000) scsi.write(0, v, true);
    else if (off >= 0x10000 && off < 0x12000) scsi.write((int)(off >> 4) & 7, v, false);
    else if (off >= 0x12000 && off < 0x14000) scsi.write(0, v, true);
    else if (off >= 0x14000 && off < 0x16000) asc.write(off - 0x14000, v);
    else if (off >= 0x16000 && off < 0x18000) iwm.write((int)(off >> 9) & 15, v, cpu.totalCycles / 2);
}

u8 *MacII::hostPage(u32 addr, bool &writable)
{
    u32 a = translate(addr);
    writable = false;
    if (a < 0x40000000) {
        if (overlay) return (u8 *)rom + (a & romMask & ~0xFFFu);
        writable = true;
        return ram + (a & ramMask & ~0xFFFu);
    }
    if ((a >> 28) == 4) return overlay ? 0 : (u8 *)rom + (a & romMask & ~0xFFFu);   // first access ends the overlay
    if ((a >> 24) == 0xF0 + kVideoSlot && (a & 0xFFFFFF) < (NubusVideo::kRegs & ~0xFFFu)) {
        writable = true;
        return card.vram() + (a & 0xFFF000);
    }
    return 0;
}

u8 MacII::phys8(u32 a, bool sideEffects)
{
    switch (a >> 28) {
    case 0: case 1: case 2: case 3:
        if (overlay) return rom[a & romMask];
        return ram[a & ramMask];
    case 4:
        if (sideEffects && overlay) { overlay = false; mapGeneration++; }
        return rom[a & romMask];
    case 5:
        return sideEffects ? io8(a) : 0xFF;
    case 0xF: {
        int slot = (int)(a >> 24) & 15;
        if (slot == kVideoSlot) return card.read(a & 0xFFFFFF);
        break;
    }
    default:
        break;
    }
    if (sideEffects) signalBusError(a, false);
    return 0xFF;
}

void MacII::physWrite8(u32 a, u8 v)
{
    switch (a >> 28) {
    case 0: case 1: case 2: case 3:
        ram[a & ramMask] = v;
        return;
    case 4:
        if (overlay) { overlay = false; mapGeneration++; }
        return;
    case 5:
        ioWrite8(a, v);
        return;
    case 0xF:
        if (((a >> 24) & 15) == kVideoSlot) { card.write(a & 0xFFFFFF, v); return; }
        break;
    default:
        break;
    }
    signalBusError(a, true);
}

u8 MacII::read8(u32 addr)
{
    return phys8(translate(addr));
}

// The I/O chips are 8-bit ports: the 68020's dynamic bus sizing turns
// word and long accesses into consecutive byte accesses.
u16 MacII::read16(u32 addr)
{
    u32 a = translate(addr);
    if (a < 0x40000000 && !overlay) {
        const u8 *p = ram + (a & ramMask & ~1u);
        return (u16)((p[0] << 8) | p[1]);
    }
    if ((a >> 24) == 0xF0 + kVideoSlot && (a & 0xFFFFFF) < NubusVideo::kRegs) {
        const u8 *p = card.vram() + (a & 0xFFFFFE);
        return (u16)((p[0] << 8) | p[1]);
    }
    u8 hi = phys8(a);
    return (u16)((hi << 8) | phys8(a + 1));
}

u32 MacII::read32(u32 addr)
{
    u32 a = translate(addr);
    if (a < 0x40000000 && !overlay && (a & ramMask) <= ramMask - 3) {
        return be32(ram + (a & ramMask));
    }
    if ((a >> 24) == 0xF0 + kVideoSlot && (a & 0xFFFFFF) + 3 < NubusVideo::kRegs) {
        return be32(card.vram() + (a & 0xFFFFFF));
    }
    u32 h = read16(addr);
    return (h << 16) | read16(addr + 2);
}

void MacII::write8(u32 addr, u8 v)
{
    physWrite8(translate(addr), v);
}

void MacII::write16(u32 addr, u16 v)
{
    u32 a = translate(addr);
    if (a < 0x40000000) {
        u8 *p = ram + (a & ramMask & ~1u);
        p[0] = (u8)(v >> 8);
        p[1] = (u8)v;
        return;
    }
    physWrite8(a, (u8)(v >> 8));
    physWrite8(a + 1, (u8)v);
}

void MacII::write32(u32 addr, u32 v)
{
    u32 a = translate(addr);
    if (a < 0x40000000 && (a & ramMask) <= ramMask - 3) {
        putBe32(ram + (a & ramMask), v);
        return;
    }
    if ((a >> 24) == 0xF0 + kVideoSlot && (a & 0xFFFFFF) + 3 < NubusVideo::kRegs) {
        putBe32(card.vram() + (a & 0xFFFFFF), v);
        return;
    }
    write16(addr, (u16)(v >> 16));
    write16(addr + 2, (u16)v);
}

u8 MacII::peek(u32 addr)
{
    u32 a = translate(addr);
    if ((a >> 28) == 5) return 0xFF;                    // never touch I/O from the card
    return phys8(a, false);
}

void MacII::poke(u32 addr, u8 v)
{
    u32 a = translate(addr);
    if (a < 0x40000000) ram[a & ramMask] = v;
}

// ---------------------------------------------------------------- SCC (enough for the ROM)

u8 MacII::sccRead(int ch, bool data)
{
    if (data) return 0;
    int reg = sccPointer[ch];
    sccPointer[ch] = 0;
    switch (reg & 7) {
    case 0: return 0x44;
    case 1: return 0x07;
    case 2: return sccWr[ch][2];
    default: return 0;
    }
}

void MacII::sccWrite(int ch, bool data, u8 v)
{
    if (data) return;
    int reg = sccPointer[ch];
    if (reg == 0) {
        sccPointer[ch] = v & 7;
        if ((v & 0x38) == 0x08) sccPointer[ch] |= 8;
        return;
    }
    sccPointer[ch] = 0;
    sccWr[ch][reg] = v;
}

// ---------------------------------------------------------------- run loop

void MacII::runFrame()
{
    u64 end = frameStart + kCyclesPerFrame;
    u64 nextSample = frameStart;
    int sample = 0;
    bool midPoll = false;                               // ADB autopolls about every 8 ms
    while (cpu.totalCycles < end) {
        if (!midPoll && cpu.totalCycles >= frameStart + kCyclesPerFrame / 2) { midPoll = true; adb.tick(); }
        viaFrac += cpu.step();
        if (viaFrac >= 20) {                            // VIA clock: CPU / 20
            via1.tick(viaFrac / 20);
            via2.tick(viaFrac / 20);
            viaFrac %= 20;
        }
        if (cpu.totalCycles >= nextSample && sample < kSamplesPerFrame) {
            soundBuf[sample++] = asc.sample();
            nextSample += kCyclesPerFrame / kSamplesPerFrame;
        }
        // Slot interrupts reach VIA2 CA1 on the falling edge of /NMRQ.
        bool slotIrq = card.irq();
        if (slotIrq && !slotIrqSeen) via2.raise(Via::IRQ_CA1);
        slotIrqSeen = slotIrq;
    }
    while (sample < kSamplesPerFrame) soundBuf[sample++] = asc.sample();
    frameStart = end;

    via1.raise(Via::IRQ_CA1);                           // 60.15 Hz tick
    card.vbl();
    adb.tick();
    if (++frameCount % 60 == 0) {
        rtc.tickSecond();
        via1.raise(Via::IRQ_CA2);
    }
    if (nmiFrames > 0) { nmiFrames--; updateIrq(); }
}

} // namespace macemu
