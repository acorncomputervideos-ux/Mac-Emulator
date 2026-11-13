// Zilog Z8530 SCC (serial ports, LocalTalk), modelled far enough for the
// Mac OS serial and AppleTalk drivers: write and read registers, transmit
// buffer empty and external/status interrupts (including the transmit
// underrun/EOM latch that ends an SDLC frame), and the RR2/RR3 interrupt
// status.  Nothing is connected to the ports: transmitted bytes vanish and
// nothing is ever received, as with an unplugged cable.

#ifndef MACEMU_SCC_H
#define MACEMU_SCC_H

#include "types.h"

namespace macemu {

class StateIO;

class Scc {
public:
    void state(StateIO &s);             // save or load (state.cpp)
    Scc();
    void reset();
    // ch: 0 = channel B, 1 = channel A (the Mac's modem port).
    u8 read(int ch, bool data);
    void write(int ch, bool data, u8 v);
    // Advance time; a byte takes `byteCycles` CPU cycles to send.
    void tick(u32 cycles);
    bool irq() const;
    bool sending() const { return ch[0].shifting || ch[1].shifting; }
    bool masterIrqEnabled() const { return (ch[1].wr[9] & 0x08) != 0; }   // WR9 MIE

    u32 byteCycles;                     // set by the machine (CPU clock / 28800)

private:
    struct Channel {
        u8 wr[16];
        u8 ptr;
        bool txEmpty;                   // transmit buffer empty
        u32 txBusy;                     // cycles until the byte in the shifter is gone
        bool shifting;                  // a byte is being sent
        bool eom;                       // RR0 bit 6: transmit underrun / EOM latch
        bool hunt;                      // RR0 bit 4: sync/hunt
        u8 rr0Latched;                  // RR0 status as last reported (for ext/status changes)
        bool txIP, extIP;
    };
    Channel ch[2];

    void resetChannel(Channel &c);
    u8 rr0(const Channel &c) const;
    void statusChanged(Channel &c);
    void command(Channel &c, u8 v);
    int pendingStatus() const;          // RR2 status code of the highest pending interrupt
};

} // namespace macemu

#endif
