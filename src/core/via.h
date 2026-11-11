// Rockwell/Synertek 6522 Versatile Interface Adapter.

#ifndef MACEMU_VIA_H
#define MACEMU_VIA_H

#include "types.h"

namespace macemu {

class Via {
public:
    class Host {
    public:
        virtual ~Host() {}
        virtual u8 viaInputA() = 0;                     // levels on pins configured as inputs
        virtual u8 viaInputB() = 0;
        virtual void viaOutputA(u8 pins) = 0;           // pin levels (inputs read as 1)
        virtual void viaOutputB(u8 pins) = 0;
        virtual void viaIrq(bool asserted) = 0;
        virtual void viaShiftOut(u8 value) = 0;         // SR written while shifting out
        virtual void viaShiftDirection(bool output) = 0;
    };

    enum { IRQ_CA2 = 0, IRQ_CA1 = 1, IRQ_SR = 2, IRQ_CB2 = 3, IRQ_CB1 = 4, IRQ_T2 = 5, IRQ_T1 = 6 };

    // The constructor does not call back into the host; call reset() once
    // the host is fully constructed.
    explicit Via(Host *host);

    void reset();
    u8 read(int reg);
    void write(int reg, u8 v);
    void tick(int clocks);                              // advance timers by VIA (E) clocks
    void raise(int bit);                                // set an interrupt flag
    void shiftIn(u8 v) { sr = v; raise(IRQ_SR); }
    void shiftOutDone() { raise(IRQ_SR); }

    bool shiftingOut() const { return (acr & 0x10) != 0; }
    // Half period of the timer 1 square wave on PB7 in VIA clocks, 0 if off.
    u32 squareWaveHalfPeriod() const { return (acr & 0xC0) == 0xC0 ? (u32)t1Latch + 2 : 0; }
    u8 shiftRegister() const { return sr; }

private:
    Host *host;
    u8 ora, orb, ddra, ddrb, sr, acr, pcr, ifr, ier;
    u16 t1Latch;
    u8 t2LatchLow;
    s32 t1, t2;
    bool t1Armed, t2Armed;

    void clear(u8 bits) { ifr &= (u8)~bits; updateIrq(); }
    void updateIrq() { host->viaIrq((ifr & ier & 0x7F) != 0); }
    void outA() { host->viaOutputA((u8)((ora & ddra) | ~ddra)); }
    void outB() { host->viaOutputB((u8)((orb & ddrb) | ~ddrb)); }
};

} // namespace macemu

#endif
