#include "adb.h"

namespace macemu {

// ---------------------------------------------------------------- devices

int AdbDevice::talk(int reg, u8 *out)
{
    if (reg != 3) return 0;
    out[0] = (u8)(0x60 | address);                      // exceptional event, SRQ enable
    out[1] = handlerId;
    return 2;
}

void AdbDevice::listen(int reg, const u8 *data, int len)
{
    if (reg != 3 || len < 2) return;
    switch (data[1]) {
    case 0x00:                                          // change address and enables
    case 0xFD:                                          // change address if activator pressed
    case 0xFE:                                          // change address if no collision
        address = data[0] & 0x0F;
        break;
    case 0xFF:                                          // self test
        break;
    default:
        address = data[0] & 0x0F;
        handlerId = data[1];
        break;
    }
}

void AdbKeyboard::reset()
{
    AdbDevice::reset();
    head = tail = 0;
    modifiers = 0xFFFF;
    leds = 0;
}

// Bits of register 2 (active low): delete, caps lock, reset, control,
// shift, option, command in the high byte.
static u16 modifierBit(int code)
{
    switch (code) {
    case 0x33: return 0x4000;                           // delete
    case 0x39: return 0x2000;                           // caps lock
    case 0x36: return 0x0800;                           // control
    case 0x38: return 0x0400;                           // shift
    case 0x3A: return 0x0200;                           // option
    case 0x37: return 0x0100;                           // command
    default: return 0;
    }
}

void AdbKeyboard::key(int code, bool down)
{
    if (code < 0 || code > 0x7F) return;
    u16 bit = modifierBit(code);
    if (bit) { if (down) modifiers &= (u16)~bit; else modifiers |= bit; }
    int next = (head + 1) & 63;
    if (next == tail) return;
    queue[head] = (u8)(code | (down ? 0 : 0x80));
    head = next;
}

int AdbKeyboard::talk(int reg, u8 *out)
{
    switch (reg) {
    case 0: {
        if (head == tail) return 0;
        out[0] = queue[tail];
        tail = (tail + 1) & 63;
        if (head != tail) { out[1] = queue[tail]; tail = (tail + 1) & 63; }
        else out[1] = 0xFF;
        return 2;
    }
    case 2:
        out[0] = (u8)(modifiers >> 8);
        out[1] = (u8)(modifiers | 0xF8) & (u8)~(leds & 7);
        return 2;
    default:
        return AdbDevice::talk(reg, out);
    }
}

void AdbKeyboard::listen(int reg, const u8 *data, int len)
{
    if (reg == 2 && len >= 2) { leds = (u8)(~data[1] & 7); return; }
    AdbDevice::listen(reg, data, len);
}

void AdbMouse::reset()
{
    AdbDevice::reset();
    dx = dy = 0;
    button = reportedButton = false;
}

int AdbMouse::talk(int reg, u8 *out)
{
    if (reg != 0) return AdbDevice::talk(reg, out);
    if (!hasData()) return 0;
    int x = dx < -63 ? -63 : dx > 63 ? 63 : dx;
    int y = dy < -63 ? -63 : dy > 63 ? 63 : dy;
    dx -= x;
    dy -= y;
    reportedButton = button;
    out[0] = (u8)((y & 0x7F) | (button ? 0 : 0x80));
    out[1] = (u8)((x & 0x7F) | 0x80);
    return 2;
}

// ---------------------------------------------------------------- transceiver

AdbTransceiver::AdbTransceiver(Via &v) : via(v)
{
    count = 0;
    reset();
}

void AdbTransceiver::reset()
{
    state = StateIdle;
    intHigh = true;
    pollBlocked = false;
    pollReply = false;
    timedOut = false;
    outLen = inLen = inIndex = 0;
    pollCommand = 0x2C;                                 // talk R0 of the keyboard
    pollMask = 0;
    pending = 0;
    pollIndex = 0;
    for (int i = 0; i < count; i++) devices[i]->reset();
}

AdbDevice *AdbTransceiver::find(int addr)
{
    for (int i = 0; i < count; i++) if (devices[i]->address == addr) return devices[i];
    return 0;
}

int AdbTransceiver::commandLength(u8 cmd)
{
    if ((cmd & 0x0C) == 0x08) return 3;                 // listen: command plus two data bytes
    return 1;                                           // talk, flush, reset
}

int AdbTransceiver::request(const u8 *cmd, int len, u8 *reply)
{
    timedOut = false;
    if ((cmd[0] & 0x0F) == 0x00) {                      // SendReset
        for (int i = 0; i < count; i++) devices[i]->reset();
        timedOut = true;
        return 0;
    }
    pending = 0;
    for (int i = 0; i < count; i++) if (devices[i]->hasData()) pending |= (u16)(1u << devices[i]->address);
    AdbDevice *d = find(cmd[0] >> 4);
    if (!d) { timedOut = true; return 0; }
    int reg = cmd[0] & 3;
    switch (cmd[0] & 0x0C) {
    case 0x00:                                          // flush
        d->flush();
        return 0;
    case 0x08:                                          // listen
        d->listen(reg, cmd + 1, len - 1);
        return 0;
    case 0x0C: {                                        // talk
        int n = d->talk(reg, reply);
        if (!n) timedOut = true;
        return n;
    }
    default:
        timedOut = true;
        return 0;
    }
}

void AdbTransceiver::send(int newState, u8 sr)
{
    switch (newState) {
    case StateNew:
        pollBlocked = true;
        if (pollReply) {
            intHigh = false;                            // the waiting data is an autopoll reply
        } else {
            intHigh = true;
            outLen = 0;
            out[outLen++] = sr;
        }
        via.shiftOutDone();
        break;
    case StateEven:
    case StateOdd:
        intHigh = true;
        if (outLen < (int)sizeof out) out[outLen++] = sr;
        via.shiftOutDone();
        break;
    default:
        intHigh = true;
        pollBlocked = false;
        return;
    }

    if (outLen && outLen == commandLength(out[0])) {
        inLen = request(out, outLen, in);
        inIndex = 0;
        if (timedOut) { in[0] = in[1] = 0xFF; inLen = 2; }
        if ((out[0] & 0x0C) == 0x0C) {                  // talk: becomes the autopoll command
            pollCommand = out[0];
            pollMask = (u16)(1u << (pollCommand >> 4));
        }
        outLen = 0;
        pollReply = false;
    }
}

void AdbTransceiver::receive(int newState)
{
    u8 v;
    switch (newState) {
    case StateNew:
        intHigh = true;
        return;
    case StateIdle:
        intHigh = true;
        pollBlocked = false;
        return;
    default:
        break;
    }
    if (inIndex == 0) {                                 // first byte: /INT flags a timeout
        v = in[0];
        intHigh = !timedOut;
        inIndex++;
    } else if (inIndex == 1) {                          // second byte: /INT flags another device's SRQ
        v = in[1];
        intHigh = !(pending & ~pollMask);
        inIndex++;
    } else if (inIndex < inLen) {
        v = in[inIndex++];
        intHigh = true;
    } else if (inIndex == inLen) {                      // end of data
        v = timedOut ? 0xFF : 0x00;
        intHigh = false;
        inIndex++;
    } else {
        v = 0xFF;
        intHigh = false;
        timedOut = false;
        pollReply = false;
        pollBlocked = false;
    }
    via.shiftIn(v);
}

void AdbTransceiver::portB(u8 pins)
{
    int newState = (pins >> 4) & 3;
    if (newState == state) return;
    state = newState;
    if (via.shiftingOut()) send(state, via.shiftRegister());
    else receive(state);
}

void AdbTransceiver::tick()
{
    if (pollBlocked || state != StateIdle || via.shiftingOut()) return;
    pollBlocked = true;
    u8 sr;
    if (inLen > 0 && inIndex == 0) {                    // an answer the Mac has not read yet
        sr = out[0];
    } else {
        inIndex = 0;
        inLen = 0;
        timedOut = false;
        sr = pollCommand;
        // Poll the devices with data, starting with the autopoll target.
        AdbDevice *hit = 0;
        AdbDevice *target = find(pollCommand >> 4);
        if (target && target->hasData()) hit = target;
        for (int i = 0; i < count && !hit; i++) {
            AdbDevice *d = devices[(pollIndex + i) % count];
            if (d->hasData()) { hit = d; pollIndex = (pollIndex + i + 1) % count; }
        }
        if (hit) {
            u8 cmd = (u8)((hit->address << 4) | 0x0C);
            inLen = hit->talk(0, in);
            sr = cmd;
            pollReply = true;
        } else {
            in[0] = in[1] = 0xFF;
            inLen = 2;
            timedOut = true;
        }
    }
    intHigh = false;
    via.shiftIn(sr);
}

} // namespace macemu
