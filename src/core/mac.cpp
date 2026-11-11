#include "mac.h"

namespace macemu {

enum {
    kVBlankLine = 342,
    kKbdDelayLines = 20,                                // ~0.9 ms between keyboard protocol steps
    kKbdInquiryLines = 5564,                            // 0.25 s: keyboard answers "no key" (0x7B)
};

Machine::Machine() : cpu(this), via(this)
{
    rom = 0; ram = 0;
    romMask = ramSize = ramMask = 0;
    hasScsi = false;
    overlay = true;
    portA = portB = 0xFF;
    viaIrqLine = false;
    nmiFrames = 0;
    line = 0;
    lineStart = nextLine = 0;
    viaFrac = 0;
    frameCount = 0;
    rtcSeconds = 0;
    memFill(pram, 0, sizeof pram);
    pramDirty = false;
    rtcEnabledLow = false; rtcClk = false; rtcWriteProtect = false;
    rtcShift = rtcCmd = rtcExtAddr = 0;
    rtcBits = rtcState = 0;
    rtcOutput = false; rtcDataOut = true;
    kbdState = KbdIdle; kbdTimer = kbdInquiryTimer = 0;
    kbdCommand = 0; kbdInstant = 0x7B; kbdResult = 0; kbdHaveResult = false;
    keyHead = keyTail = 0;
    mouseDx = mouseDy = 0; mouseDown = false;
    memFill(sccPointer, 0, sizeof sccPointer);
    memFill(sccWr, 0, sizeof sccWr);
    memFill(soundBuf, 0, sizeof soundBuf);
    squarePhase = 0; squareLevel = false;
}

static bool isPow2(u32 v) { return v && !(v & (v - 1)); }

bool Machine::init(const u8 *romData, u32 romSize, u8 *ramData, u32 ramBytes)
{
    if (!romData || !ramData) return false;
    if (!isPow2(romSize) || romSize < 0x10000 || romSize > 0x40000) return false;
    if (!isPow2(ramBytes) || ramBytes < 0x20000 || ramBytes > 0x400000) return false;
    rom = romData; romMask = romSize - 1;
    ram = ramData; ramSize = ramBytes; ramMask = ramBytes - 1;
    hasScsi = romSize >= 0x20000;                       // 64K ROMs predate SCSI
    reset();
    return true;
}

void Machine::resetLine()
{
    via.reset();
    iwm.reset();
    scsi.reset();
    memFill(sccPointer, 0, sizeof sccPointer);
    memFill(sccWr, 0, sizeof sccWr);
    kbdState = KbdIdle; kbdTimer = kbdInquiryTimer = 0;
    kbdHaveResult = false;
}

void Machine::reset()
{
    overlay = true;
    resetLine();
    keyHead = keyTail = 0;
    kbdInstant = 0x7B;
    mouseDx = mouseDy = 0;
    cpu.hardReset();
    updateIrq();
}

void Machine::updateIrq()
{
    int level = viaIrqLine ? 1 : 0;
    if (nmiFrames > 0) level |= 4;
    cpu.setIRQ(level);
}

const u8 *Machine::screen() const
{
    u32 base = ramSize - 0x5900;
    if (!(portA & 0x40)) base -= 0x8000;                // alternate screen buffer
    return ram + (base & ramMask);
}

// ---------------------------------------------------------------- memory

u8 Machine::read8(u32 addr)
{
    addr &= 0xFFFFFF;
    if (addr < 0x400000) return overlay ? rom[addr & romMask] : ram[addr & ramMask];
    if (addr < 0x500000) return rom[addr & romMask];
    if (overlay && addr >= 0x600000 && addr < 0x800000) return ram[addr & ramMask];
    return deviceRead(addr);
}

u16 Machine::read16(u32 addr)
{
    addr &= 0xFFFFFE;
    const u8 *p;
    if (addr < 0x400000) p = overlay ? rom + (addr & romMask) : ram + (addr & ramMask);
    else if (addr < 0x500000) p = rom + (addr & romMask);
    else if (overlay && addr >= 0x600000 && addr < 0x800000) p = ram + (addr & ramMask);
    else {
        // Peripheral chips sit on one half of the data bus only.
        if (addr >= 0xA00000 && addr < 0xE00000) return (u16)(0xFF00 | deviceRead(addr + 1));
        return (u16)((deviceRead(addr) << 8) | 0xFF);
    }
    return (u16)((p[0] << 8) | p[1]);
}

void Machine::write8(u32 addr, u8 v)
{
    addr &= 0xFFFFFF;
    if (addr < 0x400000) { if (!overlay) ram[addr & ramMask] = v; return; }
    if (addr < 0x500000) return;
    if (overlay && addr >= 0x600000 && addr < 0x800000) { ram[addr & ramMask] = v; return; }
    deviceWrite(addr, v);
}

void Machine::write16(u32 addr, u16 v)
{
    addr &= 0xFFFFFE;
    u8 *p;
    if (addr < 0x400000) { if (overlay) return; p = ram + (addr & ramMask); }
    else if (addr < 0x500000) return;
    else if (overlay && addr >= 0x600000 && addr < 0x800000) p = ram + (addr & ramMask);
    else {
        if (addr >= 0xE00000) deviceWrite(addr, (u8)(v >> 8));
        else deviceWrite(addr + 1, (u8)v);
        return;
    }
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

u8 Machine::deviceRead(u32 addr)
{
    switch (addr >> 20) {
    case 0x5:
        if (hasScsi && addr >= 0x580000) return scsi.read((int)(addr >> 4) & 7, (addr & 0x200) != 0);
        return 0xFF;
    case 0x8: case 0x9:
        if (addr & 1) return 0xFF;
        return sccRead((int)(addr >> 1) & 1, (addr & 4) != 0);
    case 0xC: case 0xD:
        return iwm.read((int)(addr >> 9) & 15, cpu.totalCycles);
    case 0xE:
        return via.read((int)(addr >> 9) & 15);
    default:
        return 0xFF;
    }
}

void Machine::deviceWrite(u32 addr, u8 v)
{
    switch (addr >> 20) {
    case 0x5:
        if (hasScsi && addr >= 0x580000) scsi.write((int)(addr >> 4) & 7, v, (addr & 0x200) != 0);
        return;
    case 0xA: case 0xB:
        sccWrite((int)(addr >> 1) & 1, (addr & 4) != 0, v);
        return;
    case 0xC: case 0xD:
        iwm.write((int)(addr >> 9) & 15, v, cpu.totalCycles);
        return;
    case 0xE:
        via.write((int)(addr >> 9) & 15, v);
        return;
    default:
        return;
    }
}

// ---------------------------------------------------------------- VIA wiring

u8 Machine::viaInputA()
{
    return 0xFF;                                        // bit 7: SCC wait/request, idle high
}

u8 Machine::viaInputB()
{
    u8 v = 0xFF;
    if (mouseDown) v &= (u8)~0x08;
    if (rtcOutput && !rtcDataOut) v &= (u8)~0x01;
    if (cpu.totalCycles - lineStart >= 288) v &= (u8)~0x40;   // horizontal blanking
    return v;
}

void Machine::viaOutputA(u8 pins)
{
    portA = pins;
    overlay = (pins & 0x10) != 0;
    iwm.setSel((pins & 0x20) != 0);
}

void Machine::viaOutputB(u8 pins)
{
    portB = pins;
    rtcLines(pins);
}

// ---------------------------------------------------------------- real-time clock

void Machine::rtcLines(u8 pins)
{
    bool enabled = !(pins & 0x04);
    bool clk = (pins & 0x02) != 0;
    if (!enabled) {
        rtcBits = 0; rtcState = 0; rtcOutput = false; rtcDataOut = true;
        rtcClk = clk;
        return;
    }
    if (clk && !rtcClk) {                               // rising edge: sample input
        if (!rtcOutput) {
            rtcShift = (u8)((rtcShift << 1) | (pins & 1));
            if (++rtcBits == 8) { rtcBits = 0; rtcByte(rtcShift); }
        }                                               // output: last bit stays until disabled
    } else if (!clk && rtcClk) {                        // falling edge: present output
        if (rtcOutput && rtcBits < 8) {
            rtcDataOut = (rtcShift & 0x80) != 0;
            rtcShift = (u8)(rtcShift << 1);
            rtcBits++;
        }
    }
    rtcClk = clk;
}

void Machine::rtcByte(u8 v)
{
    switch (rtcState) {
    case 0:
        if ((v & 0x78) == 0x38) { rtcCmd = v; rtcState = 2; }          // extended command
        else if (v & 0x80) { rtcShift = rtcRead(v); rtcOutput = true; rtcBits = 0; }
        else { rtcCmd = v; rtcState = 1; }
        break;
    case 1:
        rtcWrite(rtcCmd, v);
        rtcState = 0;
        break;
    case 2:
        rtcExtAddr = (u8)(((rtcCmd & 7) << 5) | ((v >> 2) & 0x1F));
        if (rtcCmd & 0x80) { rtcShift = pram[rtcExtAddr]; rtcOutput = true; rtcBits = 0; }
        else rtcState = 3;
        break;
    default:
        if (!rtcWriteProtect && pram[rtcExtAddr] != v) { pram[rtcExtAddr] = v; pramDirty = true; }
        rtcState = 0;
        break;
    }
}

u8 Machine::rtcRead(u8 cmd)
{
    int idx = (cmd >> 2) & 0x1F;
    if (idx < 8) return (u8)(rtcSeconds >> (8 * (idx & 3)));
    if (idx < 12) return pram[0x10 + (idx & 3)];
    if (idx >= 16) return pram[idx & 15];
    return 0;
}

void Machine::rtcWrite(u8 cmd, u8 v)
{
    int idx = (cmd >> 2) & 0x1F;
    if (idx == 13) { rtcWriteProtect = (v & 0x80) != 0; return; }
    if (idx == 12 || rtcWriteProtect) return;           // test register
    if (idx < 8) {
        int sh = 8 * (idx & 3);
        rtcSeconds = (rtcSeconds & ~(0xFFu << sh)) | ((u32)v << sh);
        return;
    }
    int a = idx < 12 ? 0x10 + (idx & 3) : idx >= 16 ? (idx & 15) : -1;
    if (a >= 0 && pram[a] != v) { pram[a] = v; pramDirty = true; }
}

// ---------------------------------------------------------------- keyboard

void Machine::keyEvent(int macKey, bool down)
{
    if (macKey < 0 || macKey > 0x7F) return;
    int next = (keyHead + 1) & 63;
    if (next == keyTail) return;                        // queue full: drop
    keyQueue[keyHead] = (u8)(macKey | (down ? 0 : 0x80));
    keyHead = next;
}

void Machine::kbdResultReady(u8 v)
{
    if (kbdState != KbdIdle) { kbdHaveResult = true; kbdResult = v; }
    else via.shiftIn(v);
}

bool Machine::kbdTryInquiry()
{
    if (keyHead == keyTail) return false;
    u8 ev = keyQueue[keyTail];
    keyTail = (keyTail + 1) & 63;
    int key = ev & 0x7F;
    u8 code = (u8)((((key << 1) | 1) & 0x7F) | (ev & 0x80));
    if (key >= 0x40) {                                  // keypad: 0x79 prefix, code via Instant
        kbdInstant = code;
        kbdResultReady(0x79);
    } else {
        kbdResultReady(code);
    }
    return true;
}

// The Mac wrote a command byte to the shift register.
void Machine::viaShiftOut(u8 value)
{
    if (kbdState != KbdIdle) return;
    kbdState = KbdReceiving;
    kbdCommand = value;
    kbdTimer = kKbdDelayLines;
    kbdInquiryTimer = 0;
}

// The Mac turned the shift register around to listen for the answer.
void Machine::viaShiftDirection(bool output)
{
    if (!output && kbdState == KbdReceived) {
        kbdState = KbdEnding;
        kbdTimer = kKbdDelayLines;
    }
}

void Machine::kbdTick()
{
    if (kbdTimer > 0 && --kbdTimer == 0) {
        if (kbdState == KbdReceiving) {
            kbdState = KbdReceived;
            via.shiftOutDone();
            switch (kbdCommand) {
            case 0x10:                                  // Inquiry
                if (!kbdTryInquiry()) kbdInquiryTimer = kKbdInquiryLines;
                break;
            case 0x14:                                  // Instant
                kbdResultReady(kbdInstant);
                kbdInstant = 0x7B;
                break;
            case 0x16:                                  // Model number (also resets)
                kbdInstant = 0x7B;
                kbdResultReady(0x0B);
                break;
            case 0x36:                                  // Self test
                kbdResultReady(0x7D);
                break;
            default:
                kbdResultReady(0x7B);
                break;
            }
        } else if (kbdState == KbdEnding) {
            kbdState = KbdIdle;
            if (kbdHaveResult) { kbdHaveResult = false; via.shiftIn(kbdResult); }
        }
    }
    if (kbdInquiryTimer > 0) {
        if (kbdTryInquiry()) kbdInquiryTimer = 0;
        else if (--kbdInquiryTimer == 0) kbdResultReady(0x7B);
    }
}

// ---------------------------------------------------------------- mouse

void Machine::mouseMove(int dx, int dy)
{
    mouseDx += dx;
    mouseDy += dy;
}

// Movement is delivered the way the ROM's own mouse interrupt handler would
// leave it: add the delta to MTemp and flag the cursor task (CrsrNew).  The
// OS then applies its usual clamping and cursor drawing.  This is only safe
// once the system has enabled SCC interrupts, i.e. after the RAM test.
void Machine::mouseUpdate()
{
    if (!mouseDx && !mouseDy) return;
    if (!((sccWr[0][9] | sccWr[1][9]) & 0x08) || overlay) return;
    u8 *mtemp = ram + 0x828;
    putBe16(mtemp, (u16)(be16(mtemp) + mouseDy));
    putBe16(mtemp + 2, (u16)(be16(mtemp + 2) + mouseDx));
    ram[0x8CE] = ram[0x8CF];                            // CrsrNew = CrsrCouple
    mouseDx = mouseDy = 0;
}

// ---------------------------------------------------------------- SCC

u8 Machine::sccRead(int ch, bool data)
{
    if (data) return 0;
    int reg = sccPointer[ch];
    sccPointer[ch] = 0;
    switch (reg & 7) {
    case 0: return 0x44;                                // Tx buffer empty, Tx underrun
    case 1: return 0x07;                                // all sent
    case 2: return sccWr[ch][2];
    default: return 0;
    }
}

void Machine::sccWrite(int ch, bool data, u8 v)
{
    if (data) return;
    int reg = sccPointer[ch];
    if (reg == 0) {
        sccPointer[ch] = v & 7;
        if ((v & 0x38) == 0x08) sccPointer[ch] |= 8;    // point high
        return;
    }
    sccPointer[ch] = 0;
    sccWr[ch][reg] = v;
    if (reg == 9) {
        if ((v & 0xC0) == 0xC0) { memFill(sccWr, 0, sizeof sccWr); sccWr[ch][9] = v & 0x3F; }
        sccWr[0][9] = sccWr[1][9] = sccWr[ch][9];       // WR9 is shared by both channels
    }
}

// ---------------------------------------------------------------- sound

void Machine::soundSample(int n)
{
    bool enabled;
    u32 half = via.squareWaveHalfPeriod() * 10;         // in CPU cycles
    if (half) {                                         // timer 1 chops the output via PB7
        squarePhase += kCyclesPerLine;
        while (squarePhase >= half) { squarePhase -= half; squareLevel = !squareLevel; }
        enabled = !squareLevel;
    } else {
        enabled = !(portB & 0x80);
    }
    u32 base = ramSize - ((portA & 0x08) ? 0x300 : 0x5F00);
    int sample = ram[(base + 2 * (u32)n) & ramMask];
    int volume = portA & 7;
    soundBuf[n] = enabled ? (s16)((sample - 128) * (volume + 1) * 31) : 0;
}

// ---------------------------------------------------------------- run loop

void Machine::runFrame()
{
    for (line = 0; line < kLinesPerFrame; line++) {
        lineStart = nextLine;
        nextLine += kCyclesPerLine;
        while (cpu.totalCycles < nextLine) {
            viaFrac += cpu.step();
            if (viaFrac >= 10) {
                via.tick(viaFrac / 10);
                viaFrac %= 10;
            }
        }
        soundSample(line);
        kbdTick();
        if (line == kVBlankLine) {
            via.raise(Via::IRQ_CA1);
            mouseUpdate();
        }
    }
    frameCount++;
    if (frameCount % 60 == 0) {
        rtcSeconds++;
        via.raise(Via::IRQ_CA2);
    }
    if (nmiFrames > 0) { nmiFrames--; updateIrq(); }
}

} // namespace macemu
