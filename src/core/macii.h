// Macintosh II: 68020 + 68881 at 15.67 MHz, two VIAs, ADB, the Apple Sound
// Chip, NCR 5380 SCSI, IWM floppy and NuBus slots $9-$E (the emulator's
// video card sits in slot $9).  Needs the 256K Macintosh II ROM.
//
// 32-bit physical map:
//   $00000000  RAM                 $40000000  ROM (mirrored)
//   $50000000  I/O (mirrored)      $Fs000000  NuBus standard slot space
// In 24-bit mode (VIA2 PB3 low) the HMMU maps $000000-$7FFFFF to RAM,
// $800000 to ROM, $s00000 to slot $s and $F00000 to I/O.

#ifndef MACEMU_MACII_H
#define MACEMU_MACII_H

#include "types.h"
#include "m68k.h"
#include "fpu.h"
#include "via.h"
#include "iwm.h"
#include "scsi.h"
#include "rtc.h"
#include "adb.h"
#include "asc.h"
#include "nubusvideo.h"
#include "emulator.h"

namespace macemu {

class MacII : public Bus, public Emulator, public GuestMemory {
public:
    enum {
        kCpuHz = 15667200,
        kCyclesPerFrame = 260468,                       // 60.15 Hz
        kSamplesPerFrame = 370,                         // 22257 Hz
        kVideoSlot = 9,
    };

    MacII(int videoWidth = 640, int videoHeight = 480);

    // rom: 256K Mac II ROM.  ram: ramSize bytes, a power of two from 1 MB.
    bool init(const u8 *rom, u32 romSize, u8 *ram, u32 ramSize);

    // Emulator
    const char *modelName() const override { return "Macintosh II"; }
    void reset() override;
    void runFrame() override;
    bool video(VideoFrame &f) override { return card.video(f); }
    const s16 *sound(u32 &count, u32 &rate) override { count = kSamplesPerFrame; rate = 22257; return soundBuf; }
    void keyEvent(int macKey, bool down) override { keyboard.key(macKey, down); }
    void mouseMove(int dx, int dy) override { mouse.move(dx, dy); }
    void mouseButton(bool down) override { mouse.setButton(down); }
    void interruptButton() override { nmiFrames = 6; updateIrq(); }
    Iwm &floppy() override { return iwm; }
    void attachDisk(int id, Storage *d) override { scsi.attach(id, d); }
    void flushDisks() override { iwm.flush(); scsi.flush(); }
    Rtc &clockChip() override { return rtc; }
    u64 cycles() const override { return cpu.totalCycles; }
    u32 diskActivity() const override { return iwm.activityCounter + scsi.activityCounter; }

    // Bus
    u8 read8(u32 addr) override;
    u16 read16(u32 addr) override;
    u32 read32(u32 addr) override;
    void write8(u32 addr, u8 v) override;
    void write16(u32 addr, u16 v) override;
    void write32(u32 addr, u32 v) override;
    void resetLine() override;
    u8 *hostPage(u32 addr, bool &writable) override;

    // GuestMemory (logical addresses in the current addressing mode)
    u8 peek(u32 addr) override;
    void poke(u32 addr, u8 v) override;

    bool poweredOff() const override { return powerOff; }
    u32 debugPC() const override { return cpu.pc; }
    bool addressing32() const { return mode32; }

    M68K cpu;
    Fpu fpu;
    Via via1, via2;
    Iwm iwm;
    Scsi scsi;
    Rtc rtc;
    Asc asc;
    AdbTransceiver adb;
    AdbKeyboard keyboard;
    AdbMouse mouse;
    NubusVideo card;

private:
    struct Via1Host : Via::Host {
        MacII &m;
        explicit Via1Host(MacII &mm) : m(mm) {}
        u8 viaInputA() override { return 0xFF; }
        u8 viaInputB() override;
        void viaOutputA(u8 pins) override { m.iwm.setSel((pins & 0x20) != 0); }
        void viaOutputB(u8 pins) override { m.rtc.lines(pins); m.adb.portB(pins); }
        void viaIrq(bool on) override { m.via1Irq = on; m.updateIrq(); }
        void viaShiftOut(u8) override {}
        void viaShiftDirection(bool) override {}
    };
    struct Via2Host : Via::Host {
        MacII &m;
        explicit Via2Host(MacII &mm) : m(mm) {}
        u8 viaInputA() override { return m.card.irq() ? (u8)~(1 << (kVideoSlot - 9)) : 0xFF; }
        u8 viaInputB() override { return 0xFF; }
        void viaOutputA(u8) override {}
        void viaOutputB(u8 pins) override;
        void viaIrq(bool on) override { m.via2Irq = on; m.updateIrq(); }
        void viaShiftOut(u8) override {}
        void viaShiftDirection(bool) override {}
    };

    Via1Host via1Host;
    Via2Host via2Host;
    const u8 *rom;
    u32 romMask;
    u8 *ram;
    u32 ramSize, ramMask;
    bool overlay, mode32, powerOff;
    bool via1Irq, via2Irq, slotIrqSeen;
    int nmiFrames;
    int viaFrac;
    u64 frameStart;
    u32 frameCount;
    s16 soundBuf[kSamplesPerFrame];
    u8 sccPointer[2], sccWr[2][16];

    u32 translate(u32 addr) const;
    u8 phys8(u32 addr, bool sideEffects = true);
    void physWrite8(u32 addr, u8 v);
    u8 io8(u32 off);
    void ioWrite8(u32 off, u8 v);
    u8 sccRead(int ch, bool data);
    void sccWrite(int ch, bool data, u8 v);
    void updateIrq();
    static void ascIrq(void *ctx);
};

} // namespace macemu

#endif
