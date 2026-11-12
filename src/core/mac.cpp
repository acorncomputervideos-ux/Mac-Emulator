#include "mac.h"

namespace macemu {

enum {
    kVBlankLine = 342,
    kKbdDelayLines = 20,                                // ~0.9 ms between keyboard protocol steps
    kKbdInquiryLines = 5564,                            // 0.25 s: keyboard answers "no key" (0x7B)
};

Machine::Machine() : cpu(this), via(this), adb(via)
{
    se = false;
    stepHook = 0;
    adb.quietAfterReset = true;
    adb.attach(&adbKeyboard);
    adb.attach(&adbMouse);
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
    kbdState = KbdIdle; kbdTimer = kbdInquiryTimer = 0;
    kbdCommand = 0; kbdInstant = 0x7B; kbdResult = 0; kbdHaveResult = false;
    keyHead = keyTail = 0;
    mouseDx = mouseDy = 0; mouseDown = false;
    sccIrqLine = false;
    scc.byteCycles = kCpuHz / 28800;                    // 230.4 kbit/s LocalTalk
    memFill(soundBuf, 0, sizeof soundBuf);
    squarePhase = 0; squareLevel = false;
}

static bool isPow2(u32 v) { return v && !(v & (v - 1)); }

bool Machine::init(const u8 *romData, u32 romSize, u8 *ramData, u32 ramBytes)
{
    if (!romData || !ramData) return false;
    if (!isPow2(romSize) || romSize < 0x10000 || romSize > 0x80000) return false;
    if (!isPow2(ramBytes) || ramBytes < 0x20000 || ramBytes > 0x400000) return false;
    se = romSize >= 0x40000 && be16(romData + 8) == 0x0276;
    if (romSize > 0x20000 && !se) return false;
    rom = romData; romMask = romSize - 1;
    ram = ramData; ramSize = ramBytes; ramMask = ramBytes - 1;
    hasScsi = romSize >= 0x20000;                       // 64K ROMs predate SCSI
    iwm.superDrive = romSize >= 0x80000;               // the Classic's SuperDrive
    reset();
    return true;
}

void Machine::resetLine()
{
    via.reset();
    iwm.reset();
    iwm.superDrive = rom && romMask >= 0x7FFFF;
    scsi.reset();
    adb.reset();
    scc.reset();
    sccIrqLine = false;
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
    // The 68000's interrupt lines: VIA level 1, SCC level 2, interrupt switch 4.
    int level = viaIrqLine ? 1 : 0;
    if (sccIrqLine) level |= 2;
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
    if (se && overlay && addr >= 0x400000 && addr < 0x500000) overlay = false;   // the SE's ROM switch
    if (addr < 0x400000) return overlay ? rom[addr & romMask] : ram[addr & ramMask];
    if (addr < 0x500000) return rom[addr & romMask];
    if (overlay && addr >= 0x600000 && addr < 0x800000) return ram[addr & ramMask];
    return deviceRead(addr);
}

u16 Machine::read16(u32 addr)
{
    addr &= 0xFFFFFE;
    if (se && overlay && addr >= 0x400000 && addr < 0x500000) overlay = false;
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
        {
            u8 v = scc.read((int)(addr >> 1) & 1, (addr & 4) != 0);
            sccCheck();
            return v;
        }
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
        scc.write((int)(addr >> 1) & 1, (addr & 4) != 0, v);
        sccCheck();
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
    if (se) { if (!adb.intLine()) v &= (u8)~0x08; }    // /ADBINT
    else if (mouseDown) v &= (u8)~0x08;
    if (!rtc.dataLine()) v &= (u8)~0x01;
    if (cpu.totalCycles - lineStart >= 288) v &= (u8)~0x40;   // horizontal blanking
    return v;
}

void Machine::viaOutputA(u8 pins)
{
    portA = pins;
    if (!se) overlay = (pins & 0x10) != 0;             // the SE uses PA4 for its second drive
    iwm.setSel((pins & 0x20) != 0);
}

void Machine::viaOutputB(u8 pins)
{
    portB = pins;
    rtc.lines(pins);
    if (se) adb.portB(pins);
}

// ---------------------------------------------------------------- keyboard

bool Machine::video(VideoFrame &f)
{
    f.width = kScreenWidth;
    f.height = kScreenHeight;
    f.depth = 1;
    f.rowBytes = kScreenWidth / 8;
    f.base = screen();
    f.clut = 0;
    f.clutVersion = 0;
    return true;
}

// ADB codes to Mac Plus keyboard codes.  The Plus keyboard has its arrow
// keys on the keypad and no Escape, Control or function keys.
static int plusKeyCode(int adb)
{
    switch (adb) {
    case 0x3B: return 0x46;                             // left
    case 0x3C: return 0x42;                             // right
    case 0x3D: return 0x48;                             // down
    case 0x3E: return 0x4D;                             // up
    case 0x35: case 0x36: return -1;                    // escape, control
    default: return adb < 0x60 ? adb : -1;
    }
}

void Machine::keyEvent(int adbKey, bool down)
{
    if (se) { adbKeyboard.key(adbKey, down); return; }
    int macKey = plusKeyCode(adbKey);
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
    if (se) return;                                     // ADB follows port B
    if (kbdState != KbdIdle) return;
    kbdState = KbdReceiving;
    kbdCommand = value;
    kbdTimer = kKbdDelayLines;
    kbdInquiryTimer = 0;
}

// The Mac turned the shift register around to listen for the answer.
void Machine::viaShiftDirection(bool output)
{
    if (se) return;
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
    if (se) { adbMouse.move(dx, dy); return; }
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
    if (!scc.masterIrqEnabled() || overlay) return;
    u8 *mtemp = ram + 0x828;
    putBe16(mtemp, (u16)(be16(mtemp) + mouseDy));
    putBe16(mtemp + 2, (u16)(be16(mtemp + 2) + mouseDx));
    ram[0x8CE] = ram[0x8CF];                            // CrsrNew = CrsrCouple
    mouseDx = mouseDy = 0;
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
            if (stepHook) stepHook();
            int c = cpu.step();
            viaFrac += c;
            if (scc.sending()) { scc.tick((u32)c); sccCheck(); }
            if (viaFrac >= 10) {
                via.tick(viaFrac / 10);
                viaFrac %= 10;
            }
        }
        soundSample(line);
        if (se) { if (line == 0 || line == 185) adb.tick(); }   // autopoll every 8 ms
        else kbdTick();
        if (line == kVBlankLine) {
            via.raise(Via::IRQ_CA1);
            if (!se) mouseUpdate();
        }
    }
    frameCount++;
    if (frameCount % 60 == 0) {
        rtc.tickSecond();
        via.raise(Via::IRQ_CA2);
    }
    if (nmiFrames > 0) { nmiFrames--; updateIrq(); }
}

} // namespace macemu
