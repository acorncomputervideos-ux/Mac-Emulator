#include "dafb.h"

namespace macemu {

enum { MODE_CTRL1 = 0x8, MODE_CTRL2 = 0xC, MODE_SENSE = 0x1C, INTR_MASK = 0x104, INTR_STAT = 0x108,
       INTR_CLEAR = 0x10C, LUT_INDEX = 0x200, LUT = 0x210 };

const Dafb::Mode Dafb::kModes[] = {
    {VGA, 1, 0x100, 0x71e, 640, 480, 0x400, 0x1000},
    {VGA, 2, 0x100, 0x70e, 640, 480, 0x400, 0x1000},
    {VGA, 4, 0x100, 0x706, 640, 480, 0x400, 0x1000},
    {VGA, 8, 0x100, 0x702, 640, 480, 0x400, 0x1000},
    {VGA, 32, 0x100, 0x7ff, 640, 480, 0x1000, 0x1000},
    {VGA, 1, 0xd0, 0x70e, 800, 600, 0x340, 0xe00},
    {VGA, 2, 0xd0, 0x706, 800, 600, 0x340, 0xe00},
    {VGA, 4, 0xd0, 0x702, 800, 600, 0x340, 0xe00},
    {VGA, 8, 0xd0, 0x700, 800, 600, 0x340, 0xe00},
    {VGA, 32, 0x340, 0x100, 800, 600, 0xd00, 0xe00},
    {Apple21, 1, 0x90, 0x506, 1152, 870, 0x240, 0x80},
    {Apple21, 2, 0x90, 0x502, 1152, 870, 0x240, 0x80},
    {Apple21, 4, 0x90, 0x500, 1152, 870, 0x240, 0x80},
    {Apple21, 8, 0x120, 0x5ff, 1152, 870, 0x480, 0x80},
};

Dafb::Dafb(int d) : display(d)
{
    reset();
}

void Dafb::reset()
{
    memFill(regs, 0, sizeof regs);
    memFill(frame, 0, sizeof frame);
    for (int i = 0; i < 256; i++) {
        palette[i * 3] = palette[i * 3 + 1] = palette[i * 3 + 2] = (u8)(255 - i);
        clut[i] = (u32)(255 - i) * 0x010101;
    }
    lutIndex = 0;
    clutVersion = 1;
    mode = 0;                                           // dark until the ROM's driver picks a mode
}

// Monitor sense lines (Apple Technical Note HW26), following QEMU's macfb.
// Reads are active low; writes enable pull-downs.  VGA uses extended sense
// (code 7) with pins 10 and 7 tied together; the 21" display is code 0.
u32 Dafb::sense()
{
    u32 drive = ~regs[MODE_SENSE >> 2] & 7;
    if (display == Apple21) return (~0u & 7) | drive;
    const u32 ext = 5;                                  // VGA
    u32 v = 0;
    if (!(ext & 1) && (drive & 3)) v = drive | 3;       // pins 7-4 tied
    if (!(ext & 2) && (drive & 6)) v = drive | 6;       // pins 10-7 tied
    if (!(ext & 4) && (drive & 5)) v = drive | 5;       // pins 4-10 tied
    return v;
}

void Dafb::selectMode()
{
    for (const Mode &m : kModes) {
        if (m.display != display) continue;
        if ((regs[MODE_CTRL1 >> 2] & 0xFF) == (m.ctrl1 & 0xFF) && (regs[MODE_CTRL2 >> 2] & 0xFF) == (m.ctrl2 & 0xFF)) {
            mode = &m;
            clutVersion++;
            return;
        }
    }
}

u32 Dafb::readReg(u32 off)
{
    off &= 0x3FF;
    switch (off) {
    case MODE_SENSE: return sense();
    case LUT: case LUT + 1: case LUT + 2: case LUT + 3: {
        u8 v = palette[lutIndex];
        lutIndex = (lutIndex + 1) % sizeof palette;
        return v;
    }
    default:
        return off < 0x200 ? regs[off >> 2] : 0;
    }
}

void Dafb::writeReg(u32 off, u32 v, int size)
{
    (void)size;
    off &= 0x3FF;
    switch (off) {
    case MODE_CTRL1: case MODE_CTRL1 + 1: case MODE_CTRL1 + 2: case MODE_CTRL1 + 3:
    case MODE_CTRL2: case MODE_CTRL2 + 1: case MODE_CTRL2 + 2: case MODE_CTRL2 + 3:
        regs[off >> 2] = v;
        if (v) selectMode();
        break;
    case INTR_CLEAR:
        regs[INTR_STAT >> 2] &= ~4u;
        break;
    case LUT_INDEX:
        lutIndex = (v & 0xFF) * 3;
        break;
    case LUT: case LUT + 1: case LUT + 2: case LUT + 3: {
        palette[lutIndex] = (u8)v;
        u32 i = lutIndex / 3;
        clut[i] = ((u32)palette[i * 3] << 16) | ((u32)palette[i * 3 + 1] << 8) | palette[i * 3 + 2];
        clutVersion++;
        lutIndex = (lutIndex + 1) % sizeof palette;
        break;
    }
    default:
        if (off < 0x200) regs[off >> 2] = v;
        break;
    }
}

void Dafb::vbl()
{
    if (regs[INTR_MASK >> 2] & 4) regs[INTR_STAT >> 2] |= 4;
}

bool Dafb::video(VideoFrame &f)
{
    if (!mode) return false;
    f.width = mode->width;
    f.height = mode->height;
    f.depth = mode->depth;
    f.rowBytes = mode->stride;
    f.base = frame + mode->offset;
    f.clut = mode->depth <= 8 ? clut : 0;
    f.clutVersion = clutVersion;
    return true;
}

} // namespace macemu
