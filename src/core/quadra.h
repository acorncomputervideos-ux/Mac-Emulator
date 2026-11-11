// Macintosh Quadra 800 (and with the same ROM the Quadra 610/650 and
// Centris 610/650): 68040 with MMU and FPU at 33 MHz, VIA1/VIA2 with ADB
// and the RTC, the GLUE interrupt router, djMEMC and IOSB, NCR 53C96 SCSI,
// SWIM floppy (IWM mode), Enhanced ASC, DAFB built-in video and NuBus
// slots.  Follows QEMU's q800 machine, which boots Mac OS with this ROM.
//
// Physical map:
//   $00000000  RAM (reads beyond the installed RAM return 0)
//   $40800000  1 MB ROM (also at $40000000)
//   $50000000  I/O, repeated every 256 KB up to $53FFFFFF
//   $5FFFFFFC  machine ID
//   $F9000000  DAFB VRAM, $F9800000 DAFB registers

#ifndef MACEMU_QUADRA_H
#define MACEMU_QUADRA_H

#include "types.h"
#include "m68k.h"
#include "fpu.h"
#include "mmu040.h"
#include "via.h"
#include "iwm.h"
#include "scsi.h"
#include "esp.h"
#include "rtc.h"
#include "adb.h"
#include "asc.h"
#include "dafb.h"
#include "scc.h"
#include "emulator.h"

namespace macemu {

class Quadra : public Bus, public Emulator {
public:
    enum {
        kCpuHz = 33333333,
        kCyclesPerFrame = 554170,                       // 60.15 Hz
        kSamplesPerFrame = 370,
    };

    Quadra();

    // rom: the 1 MB ROM shared by the Quadra 610/650/800 (checksum F1ACAD13).
    bool init(const u8 *rom, u32 romSize, u8 *ram, u32 ramSize);

    // Emulator
    const char *modelName() const override { return "Macintosh Quadra 800"; }
    void reset() override;
    void runFrame() override;
    bool video(VideoFrame &f) override { return dafb.video(f); }
    const s16 *sound(u32 &count, u32 &rate) override { count = kSamplesPerFrame; rate = 22257; return soundBuf; }
    void keyEvent(int macKey, bool down) override { keyboard.key(macKey, down); }
    void mouseMove(int dx, int dy) override { mouse.move(dx, dy); }
    void mouseButton(bool down) override { mouse.setButton(down); }
    void interruptButton() override { nmiFrames = 6; updateIrq(); }
    Iwm &floppy() override { return iwm; }
    void attachDisk(int id, Storage *d) override { esp.attach(id, d); }
    void flushDisks() override { iwm.flush(); esp.flush(); }
    Rtc &clockChip() override { return rtc; }
    u64 cycles() const override { return cpu.totalCycles; }
    u32 diskActivity() const override { return iwm.activityCounter + esp.activityCounter; }
    bool poweredOff() const override { return powerOff; }
    u32 debugPC() const override { return cpu.pc; }

    // Bus (physical addresses; the MMU translates before these)
    u8 read8(u32 addr) override { return (u8)access(addr, 1, false, 0); }
    u16 read16(u32 addr) override { return (u16)access(addr, 2, false, 0); }
    u32 read32(u32 addr) override { return access(addr, 4, false, 0); }
    void write8(u32 addr, u8 v) override { access(addr, 1, true, v); }
    void write16(u32 addr, u16 v) override { access(addr, 2, true, v); }
    void write32(u32 addr, u32 v) override { access(addr, 4, true, v); }
    void resetLine() override;
    u8 *hostPage(u32 addr, bool &writable) override;

    // Optional log of accesses that hit no device (for bring-up).
    void (*ioLog)(void *ctx, const char *what, u32 addr, u32 value);
    void (*stepHook)();                 // optional: called before every instruction
    bool logAllIo;                      // with ioLog: report every I/O access, not just bus errors
    void *ioLogCtx;

    M68K cpu;
    Fpu fpu;
    Mmu040 mmu;
    Via via1, via2;
    Iwm iwm;
    Esp esp;
    Rtc rtc;
    Asc asc;
    AdbTransceiver adb;
    AdbKeyboard keyboard;
    AdbMouse mouse;
    Dafb dafb;
    Scc scc;

private:
    struct Via1Host : Via::Host {
        Quadra &m;
        explicit Via1Host(Quadra &q) : m(q) {}
        u8 viaInputA() override { return (u8)((0xFF & ~0x56) | 0x12); }   // CPU ID: Quadra 800
        u8 viaInputB() override;
        void viaOutputA(u8 pins) override { m.iwm.setSel((pins & 0x20) != 0); }
        void viaOutputB(u8 pins) override;
        void viaIrq(bool on) override { m.via1Irq = on; m.updateIrq(); }
        void viaShiftOut(u8) override {}
        void viaShiftDirection(bool) override {}
    };
    struct Via2Host : Via::Host {
        Quadra &m;
        explicit Via2Host(Quadra &q) : m(q) {}
        u8 viaInputA() override { return m.dafb.irq() ? 0x3F : 0x7F; }    // bit 6: built-in video VBL
        u8 viaInputB() override { return 0x00; }
        void viaOutputA(u8) override {}
        void viaOutputB(u8 pins) override { if (!(pins & 0x04) && m.via2PowerDriven) m.powerOff = true; }
        void viaIrq(bool on) override { m.via2Irq = on; m.updateIrq(); }
        void viaShiftOut(u8) override {}
        void viaShiftDirection(bool) override {}
    };

    Via1Host via1Host;
    Via2Host via2Host;
    const u8 *rom;
    u32 romMask;
    u8 *ram;
    u32 ramSize;
    bool powerOff, via2PowerDriven, auxMode;
    bool via1Irq, via2Irq, sccIrq;
    bool videoIrqSeen, espIrqSeen, ascIrqSeen;
    int nmiFrames;
    u32 viaFrac;
    u64 frameStart;
    u32 frameCount;
    s16 soundBuf[kSamplesPerFrame];
    u32 djmemc[16], iosb[8];

    u32 access(u32 addr, int size, bool write, u32 value);
    u32 io(u32 off, int size, bool write, u32 value);
    u32 io2(u32 off, int size, bool write, u32 value);
    u32 io2(u32 off, bool write, u32 value);
    u32 busError(u32 addr, bool write, u32 value);
    void updateIrq();
};

} // namespace macemu

#endif
