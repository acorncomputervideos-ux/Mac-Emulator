#include "via.h"

namespace macemu {

Via::Via(Host *host) : host(host)
{
    ora = orb = ddra = ddrb = sr = acr = pcr = ifr = ier = 0;
    t1Latch = 0xFFFF; t2LatchLow = 0xFF;
    t1 = t2 = 0xFFFF;
    t1Armed = t2Armed = false;
}

void Via::reset()
{
    ora = orb = ddra = ddrb = sr = acr = pcr = ifr = ier = 0;
    t1Latch = 0xFFFF; t2LatchLow = 0xFF;
    t1 = t2 = 0xFFFF;
    t1Armed = t2Armed = false;
    outA();
    outB();
    updateIrq();
}

void Via::raise(int bit)
{
    ifr |= (u8)(1 << bit);
    updateIrq();
}

void Via::tick(int clocks)
{
    t1 -= clocks;
    if (t1 < 0) {
        if (acr & 0x40) {                               // free running
            raise(IRQ_T1);
            s32 period = (s32)t1Latch + 2;
            do t1 += period; while (t1 < 0);
        } else {
            if (t1Armed) { t1Armed = false; raise(IRQ_T1); }
            t1 &= 0xFFFF;
        }
    }
    t2 -= clocks;
    if (t2 < 0) {
        if (t2Armed) { t2Armed = false; raise(IRQ_T2); }
        t2 &= 0xFFFF;
    }
}

u8 Via::read(int reg)
{
    switch (reg & 15) {
    case 0:
        clear((1 << IRQ_CB1) | (1 << IRQ_CB2));
        return (u8)((orb & ddrb) | (host->viaInputB() & ~ddrb));
    case 1:
        clear((1 << IRQ_CA1) | (1 << IRQ_CA2));
        return (u8)((ora & ddra) | (host->viaInputA() & ~ddra));
    case 15:
        return (u8)((ora & ddra) | (host->viaInputA() & ~ddra));
    case 2: return ddrb;
    case 3: return ddra;
    case 4: clear(1 << IRQ_T1); return (u8)t1;
    case 5: return (u8)(t1 >> 8);
    case 6: return (u8)t1Latch;
    case 7: return (u8)(t1Latch >> 8);
    case 8: clear(1 << IRQ_T2); return (u8)t2;
    case 9: return (u8)(t2 >> 8);
    case 10: clear(1 << IRQ_SR); return sr;
    case 11: return acr;
    case 12: return pcr;
    case 13: return (u8)(ifr | ((ifr & ier & 0x7F) ? 0x80 : 0));
    default: return (u8)(ier | 0x80);
    }
}

void Via::write(int reg, u8 v)
{
    switch (reg & 15) {
    case 0: orb = v; clear((1 << IRQ_CB1) | (1 << IRQ_CB2)); outB(); break;
    case 1: ora = v; clear((1 << IRQ_CA1) | (1 << IRQ_CA2)); outA(); break;
    case 15: ora = v; outA(); break;
    case 2: ddrb = v; outB(); break;
    case 3: ddra = v; outA(); break;
    case 4: case 6: t1Latch = (u16)((t1Latch & 0xFF00) | v); break;
    case 5:
        t1Latch = (u16)((t1Latch & 0x00FF) | (v << 8));
        t1 = t1Latch + 1;
        t1Armed = true;
        clear(1 << IRQ_T1);
        break;
    case 7:
        t1Latch = (u16)((t1Latch & 0x00FF) | (v << 8));
        clear(1 << IRQ_T1);
        break;
    case 8: t2LatchLow = v; break;
    case 9:
        t2 = ((v << 8) | t2LatchLow) + 1;
        t2Armed = true;
        clear(1 << IRQ_T2);
        break;
    case 10:
        sr = v;
        clear(1 << IRQ_SR);
        if (acr & 0x10) host->viaShiftOut(v);
        break;
    case 11: {
        bool wasOut = (acr & 0x10) != 0;
        acr = v;
        bool isOut = (acr & 0x10) != 0;
        if (wasOut != isOut) host->viaShiftDirection(isOut);
        break;
    }
    case 12: pcr = v; break;
    case 13: clear(v & 0x7F); break;
    default:
        if (v & 0x80) ier |= (v & 0x7F); else ier &= (u8)~v;
        updateIrq();
        break;
    }
}

} // namespace macemu
