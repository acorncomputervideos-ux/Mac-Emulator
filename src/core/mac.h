// The Macintosh Plus (and 128K/512K/512Ke) machine: memory map, VIA wiring,
// real-time clock, keyboard, mouse, video, sound and the run loop.
//
// With an SE or Classic ROM (version 0x0276) it is that machine instead:
// the same board with an ADB keyboard and mouse on the VIA (PB3-PB5 and
// the shift register), the ROM overlay cleared by the first access to the
// ROM, and on the Classic a SuperDrive and 512K of ROM with System 6 on a
// ROM disk (hold Command-Option-X-O at start-up).

#ifndef MACEMU_MAC_H
#define MACEMU_MAC_H

#include "types.h"
#include "m68k.h"
#include "via.h"
#include "iwm.h"
#include "scsi.h"
#include "rtc.h"
#include "adb.h"
#include "scc.h"
#include "emulator.h"

namespace macemu {

class Machine : public Bus, public Via::Host, public Emulator {
public:
    enum {
        kScreenWidth = 512, kScreenHeight = 342, kScreenBytes = 512 * 342 / 8,
        kLinesPerFrame = 370, kCyclesPerLine = 352,
        kSoundSamplesPerFrame = 370,                    // one per scan line: 22254.5 Hz
        kCpuHz = 7833600,
    };

    Machine();

    // `rom` is a 64K or 128K ROM image (256K SE, 512K Classic); `ram` is
    // ramSize bytes (128K, 512K, 1M, 2M or 4M).  Both buffers stay owned by
    // the caller.
    bool init(const u8 *rom, u32 romSize, u8 *ram, u32 ramSize);
    void reset() override;
    void runFrame() override;                           // emulate 1/60.15 s

    // Video: 512x342, 1 bit per pixel, MSB first, 1 = black.
    const u8 *screen() const;

    // Sound: signed 16-bit mono samples produced by the last runFrame().
    const s16 *soundBuffer() const { return soundBuf; }

    // Emulator
    const char *modelName() const override {
        return se ? (romMask >= 0x7FFFF ? "Macintosh Classic" : "Macintosh SE")
                  : rom && romMask >= 0x1FFFF ? "Macintosh Plus" : "Macintosh 512K";
    }
    bool video(VideoFrame &f) override;
    const s16 *sound(u32 &count, u32 &rate) override { count = kSoundSamplesPerFrame; rate = 22255; return soundBuf; }
    void keyEvent(int macKey, bool down) override;      // ADB key codes (keys.h)
    void mouseMove(int dx, int dy) override;
    void mouseButton(bool down) override { if (se) adbMouse.setButton(down); else mouseDown = down; }
    void interruptButton() override { nmiFrames = 6; }
    Iwm &floppy() override { return iwm; }
    void attachDisk(int id, Storage *d) override { scsi.attach(id, d); }
    ScsiDevice &scsiDevice(int id) override { return scsi.device(id); }
    void flushDisks() override { iwm.flush(); scsi.flush(); }
    Rtc &clockChip() override { return rtc; }
    u64 cycles() const override { return cpu.totalCycles; }
    u32 diskActivity() const override { return iwm.activityCounter + scsi.activityCounter; }
    u32 debugPC() const override { return cpu.pc; }

    // Real-time clock and parameter RAM.
    void setClock(u32 secondsSince1904) { rtc.seconds = secondsSince1904; }
    u32 clock() const { return rtc.seconds; }
    Rtc rtc;

    M68K cpu;
    Via via;
    Iwm iwm;
    Scsi scsi;
    bool hasScsi;
    bool se;                            // SE or Classic: ADB
    void (*stepHook)();                 // optional: called before every instruction
    u32 frameCount;
    AdbTransceiver adb;
    AdbKeyboard adbKeyboard;
    AdbMouse adbMouse;

    // Bus
    u8 read8(u32 addr) override;
    u16 read16(u32 addr) override;
    void write8(u32 addr, u8 v) override;
    void write16(u32 addr, u16 v) override;
    void resetLine() override;

    // Via::Host
    u8 viaInputA() override;
    u8 viaInputB() override;
    void viaOutputA(u8 pins) override;
    void viaOutputB(u8 pins) override;
    void viaIrq(bool asserted) override { viaIrqLine = asserted; updateIrq(); }
    void viaShiftOut(u8 value) override;
    void viaShiftDirection(bool output) override;

private:
    const u8 *rom;
    u32 romMask;
    u8 *ram;
    u32 ramSize, ramMask;
    bool overlay;
    u8 portA, portB;
    bool viaIrqLine;
    int nmiFrames;
    int line;
    u64 lineStart, nextLine;
    int viaFrac;


    // Keyboard
    enum { KbdIdle, KbdReceiving, KbdReceived, KbdEnding };
    int kbdState, kbdTimer, kbdInquiryTimer;
    u8 kbdCommand, kbdInstant, kbdResult;
    bool kbdHaveResult;
    u8 keyQueue[64];
    int keyHead, keyTail;
    void kbdTick();
    void kbdResultReady(u8 v);
    bool kbdTryInquiry();

    // Mouse
    int mouseDx, mouseDy;
    bool mouseDown;
    void mouseUpdate();

    // SCC: serial ports with nothing attached (LocalTalk finds no network)
    Scc scc;
    bool sccIrqLine;
    void sccCheck() { bool i = scc.irq(); if (i != sccIrqLine) { sccIrqLine = i; updateIrq(); } }

    // Sound
    s16 soundBuf[kSoundSamplesPerFrame];
    u32 squarePhase;
    bool squareLevel;
    void soundSample(int n);

    void updateIrq();
    // The Plus decodes its 128K ROM at $400000-$43FFFF only.  Its ROM tells
    // a Plus (SCSI) from a 512Ke by comparing $420000 with $440000.
    bool romGap(u32 addr) const { return hasScsi && !se && romMask == 0x1FFFF && addr >= 0x440000; }
    u8 deviceRead(u32 addr);
    void deviceWrite(u32 addr, u8 v);
};

} // namespace macemu

#endif
