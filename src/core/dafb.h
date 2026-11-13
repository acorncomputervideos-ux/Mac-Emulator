// DAFB: the built-in video of the Quadra 700/800/900/950 and the Centris
// and Quadra 610/650.  4 MB of VRAM at $F9000000, registers at $F9800000.
// The ROM's own driver programs it; modes are recognised from the two mode
// control values it writes (as in QEMU's macfb model).

#ifndef MACEMU_DAFB_H
#define MACEMU_DAFB_H

#include "types.h"
#include "emulator.h"

namespace macemu {

class StateIO;

class Dafb {
public:
    void state(StateIO &s);             // save or load (state.cpp)
    enum { kVramSize = 0x400000 };
    enum Display { VGA = 0, Apple21 = 1 };

    explicit Dafb(int display = VGA);
    void reset();
    u32 readReg(u32 off);
    void writeReg(u32 off, u32 v, int size);
    u8 *vram() { return frame; }
    void vbl();
    bool irq() const { return (regs[0x108 >> 2] & regs[0x104 >> 2] & 4) != 0;  }
    bool video(VideoFrame &f);

private:
    struct Mode { u8 display, depth; u32 ctrl1, ctrl2; u16 width, height; u32 stride, offset; };
    static const Mode kModes[];
    int display;
    const Mode *mode;
    u32 regs[0x80];
    u8 palette[256 * 3];
    u32 lutIndex;
    u32 clut[256];
    u32 clutVersion;
    u8 frame[kVramSize];

    u32 sense();
    void selectMode();
};

} // namespace macemu

#endif
