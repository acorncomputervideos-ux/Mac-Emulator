// Apple Desktop Bus: the VIA-driven ADB transceiver of the Mac II family
// and Quadra 700/800/900, with an extended keyboard and a mouse.
//
// The Mac selects a transaction state with VIA1 PB4/PB5 (ST0/ST1) and moves
// bytes through the VIA shift register; the transceiver answers by loading
// the shift register, raising its interrupt and driving /ADBINT (VIA1 PB3)
// to flag timeouts, service requests and the end of data.  Transfers are
// instantaneous; idle autopolling runs from tick().

#ifndef MACEMU_ADB_H
#define MACEMU_ADB_H

#include "types.h"
#include "via.h"

namespace macemu {

class AdbDevice {
public:
    AdbDevice(u8 addr, u8 handler) : address(addr), defaultAddress(addr), handlerId(handler), defaultHandler(handler) {}
    virtual ~AdbDevice() {}
    virtual void reset() { address = defaultAddress; handlerId = defaultHandler; }
    // Talk to register 0..3: fill out[], return length (0 = no response).
    virtual int talk(int reg, u8 *out);
    virtual void listen(int reg, const u8 *data, int len);
    virtual bool hasData() const = 0;
    virtual void flush() {}

    u8 address, defaultAddress;
    u8 handlerId, defaultHandler;
};

class AdbKeyboard : public AdbDevice {
public:
    AdbKeyboard() : AdbDevice(2, 2) { reset(); }
    void reset() override;
    int talk(int reg, u8 *out) override;
    void listen(int reg, const u8 *data, int len) override;
    bool hasData() const override { return head != tail; }
    void flush() override;
    void key(int code, bool down);

    u8 leds;                    // register 2 low bits written by the Mac

private:
    u8 queue[64];
    int head, tail;
    u16 modifiers;              // register 2, active low
    u8 held[16] = {};           // keys down now: reported again after a reset or flush
    void queueHeld();
};

class AdbMouse : public AdbDevice {
public:
    AdbMouse() : AdbDevice(3, 1) { reset(); }
    void reset() override;
    int talk(int reg, u8 *out) override;
    bool hasData() const override { return dx || dy || button != reportedButton; }
    void flush() override { dx = dy = 0; reportedButton = button; }
    void move(int x, int y) { dx += x; dy += y; }
    void setButton(bool down) { button = down; }

private:
    int dx, dy;
    bool button, reportedButton;
};

class AdbTransceiver {
public:
    enum { kMaxDevices = 4 };

    explicit AdbTransceiver(Via &via);
    void attach(AdbDevice *d) { if (count < kMaxDevices) devices[count++] = d; }
    void reset();

    // VIA1 port B changed (bits 4-5 are the transaction state).
    void portB(u8 pins);
    // Level of /ADBINT for VIA1 PB3 (true = high = inactive).
    bool intLine() const { return intHigh; }
    // Call every few milliseconds of emulated time for autopolling.
    void tick();
    // No autopolling after a reset until the Mac has sent a command (the
    // SE and Classic enable the shift register interrupt before their ADB
    // driver is installed).
    bool quietAfterReset = false;

private:
    enum { StateNew = 0, StateEven = 1, StateOdd = 2, StateIdle = 3 };

    Via &via;
    AdbDevice *devices[kMaxDevices];
    int count;
    int state;
    bool intHigh;
    bool pollBlocked;
    bool pollReply;             // the data waiting came from an autopoll
    bool timedOut;
    u8 out[8];
    int outLen;
    u8 in[8];
    int inLen, inIndex;
    u8 pollCommand;
    u16 pollMask;
    u16 pending;
    int pollIndex;

    AdbDevice *find(int addr);
    int request(const u8 *cmd, int len, u8 *reply);
    void send(int newState, u8 sr);
    void receive(int newState);
    static int commandLength(u8 cmd);
};

} // namespace macemu

#endif
