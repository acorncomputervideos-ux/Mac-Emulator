#include "asc.h"

namespace macemu {

enum {
    R_VERSION = 0x00, R_MODE = 0x01, R_CONTROL = 0x02, R_FIFOMODE = 0x03, R_FIFOIRQ = 0x04,
    R_WAVECTRL = 0x05, R_VOLUME = 0x06, R_CLOCK = 0x07, R_WAVETABLE = 0x10,
};

Asc::Asc(int t) : type(t)
{
    onIrq = 0;
    irqCtx = 0;
    reset();
}

void Asc::resetFifo(Fifo &f)
{
    f.rd = f.wr = f.count = 0;
    f.status = 0;
}

void Asc::reset()
{
    memFill(regs, 0, sizeof regs);
    for (int i = 0; i < 2; i++) {
        memFill(fifo[i].data, 0x80, sizeof fifo[i].data);
        memFill(fifo[i].ext, 0, sizeof fifo[i].ext);
        resetFifo(fifo[i]);
    }
    irqLine = false;
    emptyCycle = 0;
}

void Asc::raiseIrq()
{
    bool was = irqLine;
    irqLine = true;
    if (!was && onIrq) onIrq(irqCtx);
}

u8 Asc::read(u32 off)
{
    off &= 0x1FFF;
    if (off < 0x800) return fifo[off >> 10].data[off & 0x3FF];
    if (off >= 0xF00 && off < 0xF40) return fifo[(off >> 5) & 1].ext[off & 0x1F];
    u32 r = off - 0x800;
    if (r >= sizeof regs) return 0;
    switch (r) {
    case R_VERSION:
        return type == Enhanced ? 0xB0 : 0x00;
    case R_FIFOIRQ: {
        u8 v = (u8)((fifo[0].status & 3) | ((fifo[1].status & 3) << 2));
        fifo[0].status = fifo[1].status = 0;
        irqLine = false;
        return v;
    }
    default:
        return regs[r];
    }
}

void Asc::write(u32 off, u8 v)
{
    off &= 0x1FFF;
    if (off < 0x800) {
        Fifo &f = fifo[off >> 10];
        if (regs[R_MODE] == 1) {                        // FIFO mode: append
            if (f.count >= 0x400) return;
            f.data[f.wr] = v;
            f.wr = (f.wr + 1) & 0x3FF;
            f.count++;
            if (f.count <= 0x1FF) f.status |= 1; else f.status &= (u8)~1;
            if (f.count == 0x400) { f.status |= 2; raiseIrq(); }
        } else {
            f.data[off & 0x3FF] = v;                    // wavetable data
        }
        return;
    }
    if (off >= 0xF00 && off < 0xF40) { fifo[(off >> 5) & 1].ext[off & 0x1F] = v; return; }
    u32 r = off - 0x800;
    if (r >= sizeof regs) return;
    switch (r) {
    case R_VERSION:
    case R_FIFOIRQ:
        return;
    case R_MODE:
        v &= 3;
        if (v != regs[R_MODE]) {
            resetFifo(fifo[0]);
            resetFifo(fifo[1]);
            irqLine = false;
            emptyCycle = 0;
        }
        break;
    case R_FIFOMODE:
        if (v & 0x80) {
            resetFifo(fifo[0]);
            resetFifo(fifo[1]);
            irqLine = false;
        }
        break;
    default:
        break;
    }
    regs[r] = v;
}

u8 Asc::pop(Fifo &f)
{
    if (!f.count) return 0x80;
    u8 v = f.data[f.rd];
    f.rd = (f.rd + 1) & 0x3FF;
    f.count--;
    if (f.count <= 0x1FF) f.status |= 1; else f.status &= (u8)~1;
    if (f.count == 0x1FF && halfIrqEnabled(f)) raiseIrq();
    if (f.count == 0) { f.status |= 2; raiseIrq(); }
    return v;
}

s16 Asc::sample()
{
    int mixed;
    switch (regs[R_MODE] & 3) {
    case 1: {                                           // FIFO
        bool hadData = fifo[0].count || fifo[1].count;
        int left = pop(fifo[0]);                        // both FIFOs always play
        int right = pop(fifo[1]);
        mixed = (left + right) / 2 - 128;
        // Mac OS keeps the FIFOs running when idle and waits for an
        // interrupt each time an empty FIFO would have drained.
        if (!hadData) {
            if (++emptyCycle >= 0x400) {
                emptyCycle = 0;
                fifo[0].status |= 3;
                fifo[1].status |= 3;
                raiseIrq();
            }
        } else {
            emptyCycle = 0;
        }
        break;
    }
    case 2: {                                           // four wavetable voices
        int sum = 0;
        for (int voice = 0; voice < 4; voice++) {
            u8 *p = regs + R_WAVETABLE + voice * 8;
            u32 phase = be32(p) + be32(p + 4);
            putBe32(p, phase);
            u32 offset = (phase >> 15) & 0x1FF;
            sum += fifo[voice >> 1].data[((voice & 1) << 9) + offset];
        }
        mixed = sum / 4 - 128;
        break;
    }
    default:
        return 0;
    }
    // Volume: three bits in 7..5 (the level sent to the Sony sound chip).
    int volume = (regs[R_VOLUME] >> 5) & 7;
    return (s16)(mixed * 32 * (volume + 1));
}

} // namespace macemu
