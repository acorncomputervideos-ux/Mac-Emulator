// The emulator's own NuBus colour video card.
//
// Slot space layout (offsets from $Fs000000, or $s00000 in 24-bit mode):
//   $000000-$0FFFEF  frame buffer (1, 2, 4 or 8 bits per pixel)
//   $0FFFF0-$0FFFFF  register window used by the driver
//   $FFF000-$FFFFFF  declaration ROM with the driver (videodrv.s)
//
// The driver hands Control and Status calls to the card, which reads and
// writes their parameter blocks through GuestMemory.

#ifndef MACEMU_NUBUSVIDEO_H
#define MACEMU_NUBUSVIDEO_H

#include "types.h"
#include "emulator.h"

namespace macemu {

// Byte access to the Mac's memory with the CPU's current addressing mode.
class GuestMemory {
public:
    virtual ~GuestMemory() {}
    virtual u8 peek(u32 addr) = 0;
    virtual void poke(u32 addr, u8 v) = 0;
    u16 peek16(u32 a) { return (u16)((peek(a) << 8) | peek(a + 1)); }
    u32 peek32(u32 a) { return ((u32)peek16(a) << 16) | peek16(a + 2); }
    void poke16(u32 a, u16 v) { poke(a, (u8)(v >> 8)); poke(a + 1, (u8)v); }
    void poke32(u32 a, u32 v) { poke16(a, (u16)(v >> 16)); poke16(a + 2, (u16)v); }
};

class NubusVideo {
public:
    enum {
        kVramSize = 0x100000, kRegs = 0xFFFF0, kRomSize = 0x1000,
        kFirstMode = 0x80, kModes = 4,
    };

    // width * height must fit in 1 MB at 8 bits per pixel.
    NubusVideo(int slot, int width, int height, GuestMemory &mem);

    u8 read(u32 offset);                // offset within the 16 MB slot space
    void write(u32 offset, u8 v);
    u8 *vram() { return frame; }        // for fast frame buffer access

    void vbl();                         // 60.15 Hz
    bool irq() const { return irqPending; }
    bool video(VideoFrame &f);
    const u8 *declRom() const { return rom; }
    int depth() const { return 1 << (mode - kFirstMode); }
    u32 rowBytes() const { return (u32)width * (u32)depth() / 8; }

    int slot, width, height;

private:
    GuestMemory &mem;
    u8 frame[kVramSize];
    u8 rom[kRomSize];
    u32 pb, dce, result, cmdLatch;
    bool opened, irqEnabled, irqPending, gray;
    int mode, defaultMode;
    u32 clut[256];
    u32 clutVersion;

    void buildRom();
    void command(u32 cmd);
    s16 control(u16 code, u32 param);
    s16 status(u16 code, u32 param);
    void setMode(int m);
    void grayPage();
    void defaultColors();
    u32 baseAddress();
};

} // namespace macemu

#endif
