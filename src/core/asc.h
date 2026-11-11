// Apple Sound Chip (344S0063) of the Mac II family, and the Enhanced ASC
// of the Quadras: two 1 KB FIFOs (left/right) or four wavetable voices,
// played at 22257 Hz.

#ifndef MACEMU_ASC_H
#define MACEMU_ASC_H

#include "types.h"

namespace macemu {

class Asc {
public:
    enum Type { Original = 0, Enhanced = 1 };

    explicit Asc(int type);
    void reset();
    u8 read(u32 offset);                // offset within the 0x2000-byte chip window
    void write(u32 offset, u8 v);

    // Produce one output sample (22257 Hz).  Returns a signed 16-bit value.
    s16 sample();
    // The interrupt output (to VIA2 CB1 on the Mac II).
    bool irq() const { return irqLine; }
    // Called whenever irq() goes from low to high.
    void (*onIrq)(void *ctx);
    void *irqCtx;

private:
    struct Fifo {
        u8 data[0x400];
        u32 rd, wr, count;
        u8 status;                      // bit 0 half empty, bit 1 full/empty
        u8 ext[0x20];                   // Enhanced ASC per-channel registers
    };

    int type;
    u8 regs[0x40];
    Fifo fifo[2];
    bool irqLine;
    u32 emptyCycle;

    void raiseIrq();
    void resetFifo(Fifo &f);
    u8 pop(Fifo &f);
    bool halfIrqEnabled(const Fifo &f) const { return type == Original || (f.ext[9] & 1); }
};

} // namespace macemu

#endif
