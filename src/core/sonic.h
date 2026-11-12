// National Semiconductor DP83932 SONIC: the Quadra's built-in Ethernet.
//
// The chip is a bus master: the Mac's driver leaves descriptor lists in
// RAM (transmit descriptors, a receive resource area of buffers, receive
// descriptors, and CAM entries for the address filter) and the SONIC reads
// and writes them itself.  Registers are 16 bits wide; on the Quadra they
// are 4 bytes apart and descriptors are in 32-bit mode, each 16-bit field
// in the low half of a big-endian long.
//
// Frames go to and from the frontend through an EtherLink, without the
// 4-byte frame check sequence.

#ifndef MACEMU_SONIC_H
#define MACEMU_SONIC_H

#include "types.h"

namespace macemu {

class EtherLink {
public:
    virtual ~EtherLink() {}
    // The station address to put in the address PROM.  Asked when the Mac
    // reads the PROM, so the frontend may choose it late (once it has an
    // IP address, say).
    virtual void etherAddress(u8 mac[6]) = 0;
    // A frame the Mac sent: destination, source, type/length, data.
    virtual void etherSend(const u8 *frame, u32 len) = 0;
    // A frame for the Mac, if one is waiting: copies it into `frame`
    // (room for 1514 bytes) and returns its length, or 0.
    virtual u32 etherReceive(u8 *frame) = 0;
};

class Sonic {
public:
    Sonic();
    void reset();

    // Main memory for the chip's DMA (physical address 0 upwards).
    void attachMemory(u8 *ram, u32 size) { mem = ram; memSize = size; }
    void attachLink(EtherLink *l) { link = l; }

    u16 read(int reg);
    void write(int reg, u16 v);
    u8 prom(u32 offset);                // the station address PROM
    bool irq() const { return (regs[IMR] & regs[ISR]) != 0; }

    // Passing time: the general purpose timer, and frames from the link.
    void tick(u32 busClocks);
    void poll();                        // take waiting frames while there is room
    bool receiving() const { return (regs[CR] & CR_RXEN) != 0; }

    // Hand a frame (without FCS) to the receiver.  False if it was dropped:
    // not for this station, no room, or the receiver is off.
    bool receive(const u8 *frame, u32 len);

    u32 framesSent, framesReceived;     // statistics

    enum {
        CR = 0x00, DCR = 0x01, RCR = 0x02, TCR = 0x03, IMR = 0x04, ISR = 0x05,
        UTDA = 0x06, CTDA = 0x07, TPS = 0x08, TFC = 0x09, TSA0 = 0x0A, TSA1 = 0x0B,
        TFS = 0x0C, URDA = 0x0D, CRDA = 0x0E, CRBA0 = 0x0F, CRBA1 = 0x10,
        RBWC0 = 0x11, RBWC1 = 0x12, EOBC = 0x13, URRA = 0x14, RSA = 0x15, REA = 0x16,
        RRP = 0x17, RWP = 0x18, TRBA0 = 0x19, TRBA1 = 0x1A, LLFA = 0x1F, TTDA = 0x20,
        CEP = 0x21, CAP2 = 0x22, CAP1 = 0x23, CAP0 = 0x24, CE = 0x25, CDP = 0x26,
        CDC = 0x27, SR = 0x28, WT0 = 0x29, WT1 = 0x2A, RSC = 0x2B, CRCT = 0x2C,
        FAET = 0x2D, MPT = 0x2E, MDT = 0x2F, DCR2 = 0x3F,
    };
    enum {
        CR_HTX = 0x0001, CR_TXP = 0x0002, CR_RXDIS = 0x0004, CR_RXEN = 0x0008,
        CR_STP = 0x0010, CR_ST = 0x0020, CR_RST = 0x0080, CR_RRRA = 0x0100,
        CR_LCAM = 0x0200,
        DCR_DW = 0x0020,
        RCR_PRX = 0x0001, RCR_LBK = 0x0002, RCR_CRS = 0x0020, RCR_LPKT = 0x0040,
        RCR_BC = 0x0080, RCR_MC = 0x0100, RCR_LB = 0x0600, RCR_AMC = 0x0800,
        RCR_PRO = 0x1000, RCR_BRD = 0x2000,
        TCR_PTX = 0x0001, TCR_CRSL = 0x0080, TCR_NCRS = 0x0100, TCR_CRCI = 0x2000,
        TCR_PINT = 0x8000,
        ISR_RBAE = 0x0010, ISR_RBE = 0x0020, ISR_RDE = 0x0040, ISR_TC = 0x0080,
        ISR_TXDN = 0x0200, ISR_PKTRX = 0x0400, ISR_PINT = 0x0800, ISR_LCD = 0x1000,
        EOL = 0x0001,
    };

private:
    EtherLink *link;
    u8 *mem;
    u32 memSize;
    u16 regs[64];
    u16 cam[16][3];
    u8 station[6];                      // the address, asked once after each reset
    bool stationKnown;
    bool rbaFull;                       // no receive buffer left until the driver adds one
    u8 frame[1520];

    bool wide() const { return (regs[DCR] & DCR_DW) != 0; }
    u32 fieldBytes() const { return wide() ? 4 : 2; }
    u32 addr(int hi, int lo) const { return ((u32)regs[hi] << 16) | regs[lo]; }
    u16 field(u32 base, int index);
    void setField(u32 base, int index, u16 v);
    bool readMem(u32 a, u8 *out, u32 len);
    void writeMem(u32 a, const u8 *in, u32 len);

    void command(u16 v);
    void transmit();
    void loadCam();
    void readResource();
    int filter(const u8 *f) const;      // the RCR packet type bits, or -1
};

} // namespace macemu

#endif
