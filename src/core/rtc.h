// Real-time clock and 256 bytes of parameter RAM (the 343-0042 "RTC" chip
// used from the Mac Plus to the Quadras), driven serially through three VIA
// port B lines: data (bit 0), clock (bit 1) and /enable (bit 2).

#ifndef MACEMU_RTC_H
#define MACEMU_RTC_H

#include "types.h"

namespace macemu {

class StateIO;

class Rtc {
public:
    void state(StateIO &s);             // save or load (state.cpp)
    Rtc();
    void lines(u8 pins);                // new VIA port B levels
    bool dataLine() const { return !output || dataOut; }   // level the RTC drives on bit 0
    bool driving() const { return output; }
    void tickSecond() { seconds++; }

    u32 seconds;
    u8 pram[256];
    bool pramDirty;

private:
    bool clk, writeProtect;
    u8 shiftReg, cmd_, extAddr;
    int bits, state_;
    bool output, dataOut;
    void byte(u8 v);
    u8 readReg(u8 cmd);
    void writeReg(u8 cmd, u8 v);
};

} // namespace macemu

#endif
