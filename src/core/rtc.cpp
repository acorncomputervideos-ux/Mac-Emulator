#include "rtc.h"

namespace macemu {

Rtc::Rtc()
{
    seconds = 0;
    memFill(pram, 0, sizeof pram);
    pramDirty = false;
    clk = false; writeProtect = false;
    shiftReg = cmd_ = extAddr = 0;
    bits = state_ = 0;
    output = false; dataOut = true;
}

void Rtc::lines(u8 pins)
{
    bool enabled = !(pins & 0x04);
    bool c = (pins & 0x02) != 0;
    if (!enabled) {
        bits = 0; state_ = 0; output = false; dataOut = true;
        clk = c;
        return;
    }
    if (c && !clk) {                                    // rising edge: sample input
        if (!output) {
            shiftReg = (u8)((shiftReg << 1) | (pins & 1));
            if (++bits == 8) { bits = 0; byte(shiftReg); }
        }                                               // output: last bit stays until disabled
    } else if (!c && clk) {                             // falling edge: present output
        if (output && bits < 8) {
            dataOut = (shiftReg & 0x80) != 0;
            shiftReg = (u8)(shiftReg << 1);
            bits++;
        }
    }
    clk = c;
}

void Rtc::byte(u8 v)
{
    switch (state_) {
    case 0:
        if ((v & 0x78) == 0x38) { cmd_ = v; state_ = 2; }          // extended command
        else if (v & 0x80) { shiftReg = readReg(v); output = true; bits = 0; }
        else { cmd_ = v; state_ = 1; }
        break;
    case 1:
        writeReg(cmd_, v);
        state_ = 0;
        break;
    case 2:
        extAddr = (u8)(((cmd_ & 7) << 5) | ((v >> 2) & 0x1F));
        if (cmd_ & 0x80) { shiftReg = pram[extAddr]; output = true; bits = 0; }
        else state_ = 3;
        break;
    default:
        if (!writeProtect && pram[extAddr] != v) { pram[extAddr] = v; pramDirty = true; }
        state_ = 0;
        break;
    }
}

u8 Rtc::readReg(u8 cmd)
{
    int idx = (cmd >> 2) & 0x1F;
    if (idx < 8) return (u8)(seconds >> (8 * (idx & 3)));
    if (idx < 12) return pram[0x10 + (idx & 3)];
    if (idx >= 16) return pram[idx & 15];
    return 0;
}

void Rtc::writeReg(u8 cmd, u8 v)
{
    int idx = (cmd >> 2) & 0x1F;
    if (idx == 13) { writeProtect = (v & 0x80) != 0; return; }
    if (idx == 12 || writeProtect) return;           // test register
    if (idx < 8) {
        int sh = 8 * (idx & 3);
        seconds = (seconds & ~(0xFFu << sh)) | ((u32)v << sh);
        return;
    }
    int a = idx < 12 ? 0x10 + (idx & 3) : idx >= 16 ? (idx & 15) : -1;
    if (a >= 0 && pram[a] != v) { pram[a] = v; pramDirty = true; }
}

} // namespace macemu
