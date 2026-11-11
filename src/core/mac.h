// The Macintosh Plus (and 128K/512K/512Ke) machine: memory map, VIA wiring,
// real-time clock, keyboard, mouse, video, sound and the run loop.

#ifndef MACEMU_MAC_H
#define MACEMU_MAC_H

#include "types.h"
#include "m68k.h"
#include "via.h"
#include "iwm.h"
#include "scsi.h"

namespace macemu {

class Machine : public Bus, public Via::Host {
public:
    enum {
        kScreenWidth = 512, kScreenHeight = 342, kScreenBytes = 512 * 342 / 8,
        kLinesPerFrame = 370, kCyclesPerLine = 352,
        kSoundSamplesPerFrame = 370,                    // one per scan line: 22254.5 Hz
        kCpuHz = 7833600,
    };

    Machine();

    // `rom` is a 64K or 128K ROM image; `ram` is ramSize bytes (128K, 512K,
    // 1M, 2M or 4M).  Both buffers stay owned by the caller.
    bool init(const u8 *rom, u32 romSize, u8 *ram, u32 ramSize);
    void reset();
    void runFrame();                                    // emulate 1/60.15 s

    // Video: 512x342, 1 bit per pixel, MSB first, 1 = black.
    const u8 *screen() const;

    // Sound: signed 16-bit mono samples produced by the last runFrame().
    const s16 *sound() const { return soundBuf; }

    // Input.  Key codes are Macintosh virtual key codes (see keys.h).
    void keyEvent(int macKey, bool down);
    void mouseMove(int dx, int dy);
    void mouseButton(bool down) { mouseDown = down; }
    void interruptButton() { nmiFrames = 6; }

    // Real-time clock and parameter RAM.
    void setClock(u32 secondsSince1904) { rtcSeconds = secondsSince1904; }
    u32 clock() const { return rtcSeconds; }
    u8 pram[256];
    bool pramDirty;

    M68K cpu;
    Via via;
    Iwm iwm;
    Scsi scsi;
    bool hasScsi;
    u32 frameCount;

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

    // RTC
    u32 rtcSeconds;
    bool rtcEnabledLow, rtcClk, rtcWriteProtect;
    u8 rtcShift, rtcCmd, rtcExtAddr;
    int rtcBits, rtcState;
    bool rtcOutput, rtcDataOut;
    void rtcLines(u8 pins);
    void rtcByte(u8 v);
    u8 rtcRead(u8 cmd);
    void rtcWrite(u8 cmd, u8 v);

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

    // SCC (just enough for the ROM and for mouse gating)
    u8 sccPointer[2];
    u8 sccWr[2][16];
    u8 sccRead(int ch, bool data);
    void sccWrite(int ch, bool data, u8 v);

    // Sound
    s16 soundBuf[kSoundSamplesPerFrame];
    u32 squarePhase;
    bool squareLevel;
    void soundSample(int n);

    void updateIrq();
    u8 deviceRead(u32 addr);
    void deviceWrite(u32 addr, u8 v);
};

} // namespace macemu

#endif
