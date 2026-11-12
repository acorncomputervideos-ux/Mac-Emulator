#include "sonic.h"

namespace macemu {

Sonic::Sonic()
{
    link = 0;
    mem = 0;
    memSize = 0;
    framesSent = framesReceived = 0;
    reset();
}

void Sonic::reset()
{
    memFill(regs, 0, sizeof regs);
    memFill(cam, 0, sizeof cam);
    regs[SR] = 0x0004;                                  // silicon revision
    regs[CR] = CR_RST | CR_STP | CR_RXDIS;
    regs[TCR] = TCR_NCRS | TCR_PTX;
    regs[EOBC] = 0x02F8;
    regs[RCR] = RCR_CRS;                                // the cable is plugged in
    rbaFull = false;
    stationKnown = false;
}

// ---------------------------------------------------------------- memory

bool Sonic::readMem(u32 a, u8 *out, u32 len)
{
    if (!mem || a >= memSize || len > memSize - a) { memFill(out, 0, len); return false; }
    memCopy(out, mem + a, len);
    return true;
}

void Sonic::writeMem(u32 a, const u8 *in, u32 len)
{
    if (!mem || a >= memSize || len > memSize - a) return;
    memCopy(mem + a, in, len);
}

// Descriptor fields: 16 bits each, or in 32-bit mode the low half of a long.
u16 Sonic::field(u32 base, int index)
{
    u8 b[4];
    if (wide()) { readMem(base + (u32)index * 4, b, 4); return be16(b + 2); }
    readMem(base + (u32)index * 2, b, 2);
    return be16(b);
}

void Sonic::setField(u32 base, int index, u16 v)
{
    u8 b[4] = {0, 0, (u8)(v >> 8), (u8)v};
    if (wide()) writeMem(base + (u32)index * 4, b, 4);
    else writeMem(base + (u32)index * 2, b + 2, 2);
}

// ---------------------------------------------------------------- registers

u16 Sonic::read(int reg)
{
    reg &= 63;
    if (reg == CAP0 || reg == CAP1 || reg == CAP2) {    // the CAM, only while in reset
        if (!(regs[CR] & CR_RST)) return 0;
        return cam[regs[CEP] & 15][CAP0 - reg];
    }
    return regs[reg];
}

void Sonic::write(int reg, u16 v)
{
    reg &= 63;
    switch (reg) {
    case CR: command(v); break;
    case CAP0: case CAP1: case CAP2: case SR: case MDT:
        break;                                          // read only
    case DCR:
        if (regs[CR] & CR_RST) regs[DCR] = (u16)(v & 0xBFFF);
        break;
    case DCR2:
        if (regs[CR] & CR_RST) regs[DCR2] = (u16)(v & 0xF017);
        break;
    case TCR: regs[TCR] = (u16)(v & 0xF000); break;     // the low bits are status
    case RCR: regs[RCR] = (u16)((v & 0xFE00) | (regs[RCR] & 0x01FF)); break;
    case IMR: regs[IMR] = (u16)(v & 0x7FFF); break;
    case ISR: {                                         // write 1 to clear
        u16 clear = (u16)(v & regs[ISR]);
        regs[ISR] &= (u16)~clear;
        if (clear & ISR_RBE) readResource();
        break;
    }
    case RSA: case REA: case RRP: case RWP:             // aligned pointers
        regs[reg] = (u16)(v & (wide() ? 0xFFFC : 0xFFFE));
        break;
    case CRCT: case FAET: case MPT:                     // tally counters
        regs[reg] = (u16)~v;
        break;
    default:
        regs[reg] = v;
    }
}

void Sonic::command(u16 v)
{
    if ((regs[CR] & CR_RST) && !(v & CR_RST)) {         // leaving reset does only that
        regs[CR] &= (u16)~CR_RST;
        return;
    }
    regs[CR] |= (u16)(v & 0x03BF);
    if (v & CR_TXP) transmit();
    if (v & CR_RXDIS) regs[CR] &= (u16)~CR_RXEN;
    if (v & CR_RXEN) { regs[CR] &= (u16)~CR_RXDIS; if (regs[CR] & CR_RXEN) poll(); }
    if (v & CR_STP) regs[CR] &= (u16)~CR_ST;
    if (v & CR_ST) regs[CR] &= (u16)~CR_STP;
    if (v & CR_RST) {
        regs[CR] &= (u16)~(CR_LCAM | CR_RRRA | CR_TXP | CR_HTX);
        regs[CR] |= CR_RST | CR_RXDIS;
        regs[CR] &= (u16)~CR_RXEN;
    }
    if (v & CR_RRRA) { readResource(); regs[CR] &= (u16)~CR_RRRA; }
    if (v & CR_LCAM) loadCam();
}

u8 Sonic::prom(u32 offset)
{
    // Bytes 0-5: the address with each byte's bits reversed; byte 7 makes
    // the XOR of all eight 0xFF.
    offset &= 0xFFF;
    if (offset >= 8 || offset == 6) return 0;
    if (!stationKnown) {
        static const u8 apple[6] = {0x08, 0x00, 0x07, 0x12, 0x34, 0x56};
        memCopy(station, apple, 6);
        if (link) link->etherAddress(station);
        stationKnown = true;
    }
    u8 p[8] = {0};
    for (int i = 0; i < 6; i++) {
        u8 b = station[i], r = 0;
        for (int k = 0; k < 8; k++) if (b & (1 << k)) r |= (u8)(0x80 >> k);
        p[i] = r;
    }
    u8 sum = 0;
    for (int i = 0; i < 6; i++) sum ^= p[i];
    p[7] = (u8)(0xFF - sum);
    return p[offset];
}

void Sonic::tick(u32 clocks)
{
    if (regs[CR] & CR_STP) return;
    if (!(regs[CR] & CR_ST)) return;
    u32 wt = addr(WT1, WT0);
    if (clocks <= wt) { wt -= clocks; }
    else {
        wt = 0xFFFFFFFFu - (clocks - wt - 1);
        regs[ISR] |= ISR_TC;
    }
    regs[WT1] = (u16)(wt >> 16);
    regs[WT0] = (u16)wt;
}

// ---------------------------------------------------------------- transmit

void Sonic::transmit()
{
    for (int guard = 0; guard < 256; guard++) {
        regs[TTDA] = regs[CTDA];
        u32 d = ((u32)regs[UTDA] << 16) | (regs[TTDA] & 0xFFFE);
        regs[TCR] = (u16)(field(d, 1) & 0xF000);
        regs[TPS] = field(d, 2);
        regs[TFC] = field(d, 3);
        if (regs[TCR] & TCR_PINT) regs[ISR] |= ISR_PINT;

        u32 len = 0;
        for (int i = 0; i < regs[TFC] && i < 64; i++) {
            regs[TSA0] = field(d, 4 + 3 * i);
            regs[TSA1] = field(d, 5 + 3 * i);
            regs[TFS] = field(d, 6 + 3 * i);
            u32 n = regs[TFS];
            if (n > sizeof frame - len) n = sizeof frame - len;
            readMem(addr(TSA1, TSA0), frame + len, n);
            len += n;
        }
        if (regs[TCR] & TCR_CRCI) len = len >= 4 ? len - 4 : 0;   // it carries its own FCS

        if (len >= 14) {
            if (regs[RCR] & RCR_LB) {                   // loopback: straight back in
                regs[TCR] |= TCR_CRSL;
                if (receive(frame, len)) regs[RCR] |= RCR_LBK;
            } else if (link) {
                link->etherSend(frame, len);
            }
            framesSent++;
        }
        regs[TCR] |= TCR_PTX;
        setField(d, 0, (u16)(regs[TCR] & 0x0FFF));

        if (regs[CR] & CR_HTX) break;
        regs[CTDA] = field(d, 4 + 3 * regs[TFC]);
        if (regs[CTDA] & EOL) break;
    }
    regs[CR] &= (u16)~CR_TXP;
    regs[ISR] |= ISR_TXDN;
}

// ---------------------------------------------------------------- receive

void Sonic::loadCam()
{
    u32 size = 4 * fieldBytes();
    for (int guard = 0; (regs[CDC] & 0x1F) && guard < 32; guard++) {
        u32 p = addr(URRA, CDP);
        int i = field(p, 0) & 15;
        cam[i][0] = field(p, 1);
        cam[i][1] = field(p, 2);
        cam[i][2] = field(p, 3);
        regs[CDC]--;
        regs[CDP] = (u16)(regs[CDP] + size);
    }
    regs[CE] = field(addr(URRA, CDP), 0);
    regs[CR] &= (u16)~CR_LCAM;
    regs[ISR] |= ISR_LCD;
}

// The next receive buffer from the resource area.
void Sonic::readResource()
{
    u32 p = addr(URRA, RRP);
    regs[CRBA0] = field(p, 0);
    regs[CRBA1] = field(p, 1);
    regs[RBWC0] = field(p, 2);
    regs[RBWC1] = field(p, 3);
    regs[RRP] = (u16)(regs[RRP] + 4 * fieldBytes());
    if (regs[RRP] == regs[REA]) regs[RRP] = regs[RSA];
    if (regs[RRP] == regs[RWP]) regs[ISR] |= ISR_RBE;  // that was the last one
    rbaFull = false;
}

int Sonic::filter(const u8 *f) const
{
    bool group = (f[0] & 1) != 0;
    if ((regs[RCR] & RCR_PRO) && !group) return 0;
    if ((regs[RCR] & RCR_AMC) && group) return RCR_MC;
    bool bcast = true;
    for (int i = 0; i < 6; i++) if (f[i] != 0xFF) bcast = false;
    if (bcast && (regs[RCR] & RCR_BRD)) return RCR_BC;
    for (int i = 0; i < 16; i++) {
        if (!(regs[CE] & (1 << i))) continue;
        if (f[0] == (u8)cam[i][0] && f[1] == (u8)(cam[i][0] >> 8) &&
            f[2] == (u8)cam[i][1] && f[3] == (u8)(cam[i][1] >> 8) &&
            f[4] == (u8)cam[i][2] && f[5] == (u8)(cam[i][2] >> 8))
            return group ? RCR_MC : 0;
    }
    return -1;
}

static u32 crc32(const u8 *p, u32 n)
{
    u32 c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

bool Sonic::receive(const u8 *in, u32 len)
{
    if (!(regs[CR] & CR_RXEN) || len < 14 || len > 1514) return false;
    regs[RCR] &= (u16)~0x01DF;                          // status bits, all but carrier sense
    if (rbaFull) return false;

    u8 buf[1518];
    memCopy(buf, in, len);
    if (len < 60) { memFill(buf + len, 0, 60 - len); len = 60; }   // runts arrive padded
    u32 fcs = crc32(buf, len);
    buf[len] = (u8)fcs; buf[len + 1] = (u8)(fcs >> 8);
    buf[len + 2] = (u8)(fcs >> 16); buf[len + 3] = (u8)(fcs >> 24);
    u32 rxLen = len + 4;
    u32 padded = wide() ? (rxLen + 3) & ~3u : (rxLen + 1) & ~1u;

    bool accepted = false;
    if (padded > addr(RBWC1, RBWC0) * 2) {             // does not fit the buffer
        regs[ISR] |= ISR_RBAE;
        regs[RCR] |= RCR_LPKT;
    } else {
        int type = filter(buf);
        if (type < 0) return false;

        if (regs[LLFA] & EOL) {                         // no descriptor last time
            u32 d = ((u32)regs[URDA] << 16) | (regs[CRDA] & 0xFFFE);
            regs[LLFA] = field(d, 5);
            if (regs[LLFA] & EOL) return false;         // still none
            setField(d, 6, 0);
            regs[CRDA] = regs[LLFA];
            regs[ISR] |= ISR_PKTRX;
        }

        regs[TRBA0] = regs[CRBA0];
        regs[TRBA1] = regs[CRBA1];
        u32 a = addr(CRBA1, CRBA0);
        writeMem(a, buf, rxLen);
        static const u8 fill[3] = {0xFF, 0xFF, 0xFF};
        if (padded > rxLen) writeMem(a + rxLen, fill, padded - rxLen);
        a += padded;
        regs[CRBA1] = (u16)(a >> 16);
        regs[CRBA0] = (u16)a;
        u32 words = addr(RBWC1, RBWC0) - padded / 2;
        regs[RBWC1] = (u16)(words >> 16);
        regs[RBWC0] = (u16)words;

        if (words < regs[EOBC]) regs[RCR] |= RCR_LPKT;  // the buffer is used up
        regs[RCR] |= (u16)(type | RCR_PRX);

        u32 d = ((u32)regs[URDA] << 16) | (regs[CRDA] & 0xFFFE);
        setField(d, 0, regs[RCR]);
        setField(d, 1, (u16)rxLen);
        setField(d, 2, regs[TRBA0]);
        setField(d, 3, regs[TRBA1]);
        setField(d, 4, regs[RSC]);
        regs[LLFA] = field(d, 5);
        if (regs[LLFA] & EOL) {
            regs[ISR] |= ISR_RDE;                       // out of descriptors
        } else {
            setField(d, 6, 0);
            regs[CRDA] = regs[LLFA];
            regs[ISR] |= ISR_PKTRX;
        }
        regs[RSC] = (u16)((regs[RSC] & 0xFF00) | ((regs[RSC] + 1) & 0xFF));
        framesReceived++;
        accepted = true;
    }

    if (regs[RCR] & RCR_LPKT) {
        if (regs[RRP] == regs[RWP]) rbaFull = true;
        else readResource();
    }
    return accepted;
}

void Sonic::poll()
{
    if (!link) return;
    for (int i = 0; i < 16 && (regs[CR] & CR_RXEN) && !rbaFull; i++) {
        u32 n = link->etherReceive(frame);
        if (!n) break;
        receive(frame, n);
    }
}

} // namespace macemu
