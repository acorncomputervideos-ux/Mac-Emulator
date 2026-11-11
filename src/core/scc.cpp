#include "scc.h"

namespace macemu {

enum {
    RR0_RX = 0x01, RR0_TXEMPTY = 0x04, RR0_DCD = 0x08, RR0_HUNT = 0x10, RR0_CTS = 0x20, RR0_EOM = 0x40,
    WR1_EXTIE = 0x01, WR1_TXIE = 0x02,
    WR9_VIS = 0x01, WR9_NV = 0x02, WR9_MIE = 0x08, WR9_SHSL = 0x10,
};

Scc::Scc()
{
    byteCycles = 1000;
    reset();
}

void Scc::resetChannel(Channel &c)
{
    u8 wr9 = c.wr[9];
    memFill(c.wr, 0, sizeof c.wr);
    c.wr[9] = wr9;
    c.ptr = 0;
    c.txEmpty = true;
    c.txBusy = 0;
    c.shifting = false;
    c.eom = true;
    c.hunt = true;
    c.txIP = c.extIP = false;
    c.rr0Latched = rr0(c);
}

void Scc::reset()
{
    for (int i = 0; i < 2; i++) { ch[i].wr[9] = 0; resetChannel(ch[i]); }
}

u8 Scc::rr0(const Channel &c) const
{
    u8 v = RR0_CTS;                                     // handshake input idle
    if (c.txEmpty) v |= RR0_TXEMPTY;
    if (c.hunt) v |= RR0_HUNT;
    if (c.eom) v |= RR0_EOM;
    return v;
}

// External/status interrupt when an enabled RR0 condition changes.
void Scc::statusChanged(Channel &c)
{
    u8 now = rr0(c);
    u8 changed = (u8)(now ^ c.rr0Latched);
    u8 enabled = c.wr[15] & (RR0_DCD | RR0_HUNT | RR0_CTS | RR0_EOM | 0x80);
    if ((changed & enabled) && (c.wr[1] & WR1_EXTIE) && !c.extIP) c.extIP = true;
    if (!c.extIP) c.rr0Latched = now;
}

void Scc::command(Channel &c, u8 v)
{
    switch ((v >> 3) & 7) {
    case 2:                                             // reset external/status interrupts
        c.extIP = false;
        c.rr0Latched = rr0(c);
        break;
    case 5:                                             // reset Tx interrupt pending
        c.txIP = false;
        break;
    default:
        break;
    }
    if ((v >> 6) == 3) c.eom = false;                   // reset Tx underrun/EOM latch
    if ((v >> 6) == 0 && ((v >> 3) & 7) == 0) {}        // null
}

u8 Scc::read(int n, bool data)
{
    Channel &c = ch[n & 1];
    if (data) return 0;                                 // nothing is ever received
    int reg = c.ptr;
    c.ptr = 0;
    switch (reg) {
    case 0: case 4:
        return rr0(c);
    case 1: case 5:
        return c.txEmpty && !c.shifting ? 0x07 : 0x06; // all sent
    case 2: case 6: {
        u8 vec = ch[1].wr[2];                           // the vector lives in channel A's WR2
        if (n & 1) return vec;                          // channel A: unmodified
        int status = pendingStatus();
        if (ch[1].wr[9] & WR9_SHSL)
            return (u8)((vec & 0x8F) | (((status & 1) << 6) | ((status & 2) << 4) | ((status & 4) << 2)));
        return (u8)((vec & 0xF1) | (status << 1));
    }
    case 3: case 7: {
        if (!(n & 1)) return 0;
        u8 v = 0;
        if (ch[0].extIP) v |= 0x01;
        if (ch[0].txIP) v |= 0x02;
        if (ch[1].extIP) v |= 0x08;
        if (ch[1].txIP) v |= 0x10;
        return v;
    }
    case 10: case 14: return 0;
    case 12: return c.wr[12];
    case 13: case 9: return c.wr[13];
    case 15: case 11: return c.wr[15];
    default: return 0;
    }
}

void Scc::write(int n, bool data, u8 v)
{
    Channel &c = ch[n & 1];
    if (data) {                                         // transmit buffer
        if (!(c.wr[5] & 0x08)) return;                  // transmitter disabled
        c.txEmpty = false;
        c.txIP = false;
        if (!c.shifting) {                              // straight into the shifter
            c.shifting = true;
            c.txBusy = byteCycles;
            c.txEmpty = true;
            if (c.wr[1] & WR1_TXIE) c.txIP = true;
        }
        statusChanged(c);
        return;
    }
    int reg = c.ptr;
    c.ptr = 0;
    if (reg == 0) {
        c.ptr = v & 7;
        if (((v >> 3) & 7) == 1) c.ptr |= 8;            // point high
        command(c, v);
        statusChanged(c);
        return;
    }
    if (reg == 9) {                                     // shared by both channels
        switch (v >> 6) {
        case 1: resetChannel(ch[0]); break;
        case 2: resetChannel(ch[1]); break;
        case 3: reset(); break;
        default: break;
        }
        ch[0].wr[9] = ch[1].wr[9] = v & 0x3F;
        return;
    }
    if (reg == 2) { ch[0].wr[2] = ch[1].wr[2] = v; return; }
    if (reg == 8) { c.ptr = 0; write(n, true, v); return; }
    c.wr[reg] = v;
    if (reg == 3 && (v & 0x10)) c.hunt = true;          // enter hunt mode
    if (reg == 5 && !(v & 0x08)) {                      // transmitter off
        c.shifting = false;
        c.txEmpty = true;
    }
    if (reg == 1 && !(v & WR1_TXIE)) c.txIP = false;
    statusChanged(c);
}

void Scc::tick(u32 cycles)
{
    for (int i = 0; i < 2; i++) {
        Channel &c = ch[i];
        if (!c.shifting) continue;
        if (c.txBusy > cycles) { c.txBusy -= cycles; continue; }
        if (!c.txEmpty) {                               // next byte from the buffer
            c.txEmpty = true;
            c.txBusy = byteCycles;
            if (c.wr[1] & WR1_TXIE) c.txIP = true;
        } else {                                        // underrun: the frame ends
            c.shifting = false;
            if (!c.eom) c.eom = true;
        }
        statusChanged(c);
    }
}

// Status codes, V3..V1: 0 B Tx empty, 1 B ext/status, 2 B Rx, 3 B special
// (also "none"), 4 A Tx empty, 5 A ext/status, 6 A Rx, 7 A special.
int Scc::pendingStatus() const
{
    if (ch[1].txIP) return 4;
    if (ch[1].extIP) return 5;
    if (ch[0].txIP) return 0;
    if (ch[0].extIP) return 1;
    return 3;
}

bool Scc::irq() const
{
    if (!(ch[1].wr[9] & WR9_MIE)) return false;
    for (int i = 0; i < 2; i++) {
        const Channel &c = ch[i];
        if ((c.txIP && (c.wr[1] & WR1_TXIE)) || (c.extIP && (c.wr[1] & WR1_EXTIE))) return true;
    }
    return false;
}

} // namespace macemu
