#include "esp.h"

namespace macemu {

enum {
    R_TCLO = 0, R_TCMID = 1, R_FIFO = 2, R_CMD = 3, R_STAT = 4, R_INTR = 5, R_SEQ = 6, R_FLAGS = 7,
    R_CFG1 = 8, R_TCHI = 14,
    STAT_TC = 0x10, STAT_INT = 0x80,
    INTR_FC = 0x08, INTR_BS = 0x10, INTR_DC = 0x20, INTR_IL = 0x40, INTR_RST = 0x80,
    CHIP_ID = 0x04,                                     // FAS100A family, as the Quadras report
};

Esp::Esp()
{
    for (int i = 0; i < kTargets; i++) disks[i] = 0;
    activityCounter = 0;
    reset();
}

void Esp::reset()
{
    memFill(rregs, 0, sizeof rregs);
    memFill(wregs, 0, sizeof wregs);
    rregs[R_CFG1] = 7;
    fifoCount = 0;
    tchiWritten = false;
    irqLine = false;
    dma = false;
    connected = false;
    target = -1;
    phase = BusFree;
    lastCmd = 0;
    dmaDone = false;
    fifoFromTarget = false;
    cdbLen = 0;
    bufLen = bufPos = 0;
    lba = blocksLeft = 0;
    reading = writing = false;
    status = 0;
    senseKey = senseCode = 0;
}

void Esp::flush()
{
    for (int i = 0; i < kTargets; i++) if (disks[i]) disks[i]->flush();
}

void Esp::setTc(u32 v)
{
    bool was = tc() != 0;
    rregs[R_TCLO] = (u8)v;
    rregs[R_TCMID] = (u8)(v >> 8);
    rregs[R_TCHI] = (u8)(v >> 16);
    if (was && v == 0) rregs[R_STAT] |= STAT_TC;
}

u8 Esp::pop()
{
    if (!fifoCount) return 0;
    if (fifoCount == 1) fifoFromTarget = false;
    u8 v = fifo[0];
    for (int i = 1; i < fifoCount; i++) fifo[i - 1] = fifo[i];
    fifoCount--;
    return v;
}

void Esp::raise(u8 intr)
{
    rregs[R_INTR] |= intr;
    rregs[R_STAT] |= STAT_INT;
    irqLine = true;
}

bool Esp::drq() const
{
    if (!dma || !connected) return false;
    if (fifoFromTarget && fifoCount > 0) return true;  // even after the target moved on to status
    switch (phase) {
    case DataIn: return false;
    case DataOut: case Command: return tc() != 0;
    default: return false;
    }
}

// ---------------------------------------------------------------- target side

void Esp::finish(u8 st, u8 key, u8 code)
{
    status = st;
    senseKey = key;
    senseCode = code;
    reading = writing = false;
    bufLen = bufPos = 0;
    phase = Status;
}

void Esp::reply(u32 len, u32 alloc)
{
    bufLen = len < alloc ? len : alloc;
    bufPos = 0;
    status = 0;
    senseKey = senseCode = 0;
    phase = bufLen ? DataIn : Status;
}

bool Esp::loadBlock()
{
    activityCounter++;
    bufPos = 0;
    bufLen = 512;
    return disks[target]->read(lba * 512, buf, 512);
}

void Esp::execute()
{
    Storage *d = disks[target];
    u32 blocks = d->size() / 512;
    u8 op = cdb[0];
    switch (op) {
    case 0x00: case 0x01: case 0x04: case 0x07: case 0x0B: case 0x1B: case 0x1E:
    case 0x2B: case 0x2F: case 0x35:
        if (op == 0x35) d->flush();
        finish(0);
        return;
    case 0x03: {                                        // REQUEST SENSE
        memFill(buf, 0, 18);
        buf[0] = 0x70; buf[2] = senseKey; buf[7] = 10; buf[12] = senseCode;
        reply(18, cdb[4] ? cdb[4] : 4);
        return;
    }
    case 0x12: {                                        // INQUIRY
        static const char id[] = "MACEMU  CIRCLE HARD DISK1.0 ";
        memFill(buf, 0, 36);
        buf[2] = 1; buf[3] = 1; buf[4] = 31;
        memCopy(buf + 8, id, 28);
        reply(36, cdb[4]);
        return;
    }
    case 0x15:                                          // MODE SELECT: accept and ignore
        bufLen = cdb[4];
        bufPos = 0;
        phase = bufLen ? DataOut : Status;
        status = 0;
        return;
    case 0x1A: {                                        // MODE SENSE
        int page = cdb[2] & 0x3F;
        memFill(buf, 0, 64);
        u32 n = 4;
        buf[2] = d->readOnly() ? 0x80 : 0;
        buf[3] = 8;
        buf[n + 1] = (u8)(blocks >> 16); buf[n + 2] = (u8)(blocks >> 8); buf[n + 3] = (u8)blocks;
        buf[n + 6] = 0x02;
        n += 8;
        if (page == 0x03 || page == 0x3F) { buf[n] = 3; buf[n + 1] = 22; buf[n + 11] = 32; buf[n + 12] = 2; n += 24; }
        if (page == 0x04 || page == 0x3F) {
            u32 cyl = blocks / 128;
            buf[n] = 4; buf[n + 1] = 22;
            buf[n + 2] = (u8)(cyl >> 16); buf[n + 3] = (u8)(cyl >> 8); buf[n + 4] = (u8)cyl; buf[n + 5] = 4;
            n += 24;
        }
        buf[0] = (u8)(n - 1);
        reply(n, cdb[4]);
        return;
    }
    case 0x25:                                          // READ CAPACITY
        putBe32(buf, blocks - 1);
        putBe32(buf + 4, 512);
        reply(8, 8);
        return;
    case 0x08: case 0x0A: case 0x28: case 0x2A: {
        u32 count;
        if (op < 0x20) {
            lba = ((u32)(cdb[1] & 0x1F) << 16) | ((u32)cdb[2] << 8) | cdb[3];
            count = cdb[4] ? cdb[4] : 256;
        } else {
            lba = be32(cdb + 2);
            count = be16(cdb + 7);
        }
        if (lba > blocks || count > blocks - lba) { finish(2, 5, 0x21); return; }
        if (!count) { finish(0); return; }
        bool isWrite = op == 0x0A || op == 0x2A;
        if (isWrite && d->readOnly()) { finish(2, 7, 0x27); return; }
        blocksLeft = count;
        status = 0;
        senseKey = senseCode = 0;
        if (isWrite) { writing = true; bufPos = 0; bufLen = 512; phase = DataOut; }
        else if (loadBlock()) { reading = true; phase = DataIn; }
        else finish(2, 3, 0x11);
        return;
    }
    default:
        finish(2, 5, 0x20);
        return;
    }
}

bool Esp::targetByteIn(u8 &v)
{
    if (phase != DataIn || bufPos >= bufLen) return false;
    v = buf[bufPos++];
    if (bufPos >= bufLen) {
        if (reading && --blocksLeft > 0) {
            lba++;
            if (!loadBlock()) finish(2, 3, 0x11);
        } else {
            reading = false;
            phase = Status;
        }
    }
    return true;
}

void Esp::targetByteOut(u8 v)
{
    if (phase == Command) {
        if (cdbLen < 16) cdb[cdbLen++] = v;
        int need = (cdb[0] >> 5) == 0 ? 6 : (cdb[0] >> 5) == 5 ? 12 : 10;
        if (cdbLen >= need) execute();
        return;
    }
    if (phase != DataOut) return;
    if (bufPos < 512) buf[bufPos] = v;
    if (++bufPos < bufLen) return;
    if (!writing) { finish(0); return; }           // MODE SELECT data
    activityCounter++;
    disks[target]->write(lba * 512, buf, 512);
    lba++;
    bufPos = 0;
    if (--blocksLeft == 0) finish(0);
}

// ---------------------------------------------------------------- initiator side

// In the command phase the chip hands FIFO bytes to the target as they
// arrive.  When the CDB is complete the target starts the command and the
// chip interrupts: bus service, plus function complete after a selection.
void Esp::commandBytes()
{
    if (!connected || phase != Command) return;
    while (fifoCount && phase == Command) targetByteOut(pop());
    if (phase == Command) return;
    rregs[R_SEQ] = 4;
    u8 c = lastCmd & 0x7F;
    raise(c == 0x41 || c == 0x42 ? (INTR_BS | INTR_FC) : INTR_BS);
}

void Esp::select(bool atn, bool stop)
{
    target = wregs[R_STAT] & 7;                         // bus ID register
    rregs[R_SEQ] = 0;
    if (!disks[target]) {
        connected = false;
        phase = BusFree;
        fifoCount = 0;
        rregs[R_STAT] = 0;
        raise(INTR_DC);                                 // selection timeout
        return;
    }
    connected = true;
    cdbLen = 0;
    if (atn && fifoCount) pop();                        // identify message
    if (stop) {                                         // stop after the message byte
        phase = Command;
        rregs[R_SEQ] = 1;
        raise(INTR_BS | INTR_FC);
        return;
    }
    phase = Command;
    commandBytes();                                     // CDB already in the FIFO, or later via (pseudo-)DMA
}

void Esp::fillFifo()
{
    u8 v;
    while (phase == DataIn && fifoCount < 16 && tc() > 0 && targetByteIn(v)) {
        fifoFromTarget = true;
        push(v);
        setTc(tc() - 1);
    }
}

void Esp::transferInfo()
{
    if (!connected) return;
    if (dma) {
        // Pseudo-DMA: the chip moves data between the target and its FIFO,
        // the CPU empties or fills the FIFO; we interrupt when it is done.
        dmaDone = false;
        if (phase == DataOut) {
            // Bytes the driver put in the FIFO by hand (the odd first word of
            // an unaligned buffer) go to the target before the DMA data.
            while (fifoCount && phase == DataOut) targetByteOut(pop());
        }
        if (phase == DataIn) {
            fillFifo();
            if (fifoCount < 2 && (tc() == 0 || phase != DataIn)) { dmaDone = true; raise(INTR_BS); }
        }
        if (phase == Command) commandBytes();
        if (phase == Status || phase == MsgIn) raise(INTR_BS);
        return;
    }
    switch (phase) {
    case DataIn: {                                      // one byte per TI, as on the chip
        u8 v;
        if (!fifoCount && targetByteIn(v)) { push(v); fifoFromTarget = false; }
        raise(INTR_BS);
        break;
    }
    case Command:
        commandBytes();
        break;
    case DataOut:
        while (fifoCount && phase == DataOut) targetByteOut(pop());
        raise(INTR_BS);
        break;
    case Status:
        push(status);
        phase = MsgIn;
        raise(INTR_BS);
        break;
    case MsgIn:
        push(0);
        raise(INTR_BS);
        break;
    default:
        raise(INTR_BS);
        break;
    }
}

void Esp::command(u8 cmd)
{
    if (cmd & 0x80) {
        dma = true;
        u32 start = wregs[R_TCLO] | (wregs[R_TCMID] << 8) | (wregs[R_TCHI] << 16);
        setTc(start ? start : 0x10000);
    } else {
        dma = false;
    }
    switch (cmd & 0x7F) {
    case 0x00: break;                                   // NOP
    case 0x01: fifoCount = 0; fifoFromTarget = false; break;   // flush FIFO
    case 0x02: {                                        // reset chip
        Storage *keep[kTargets];
        for (int i = 0; i < kTargets; i++) keep[i] = disks[i];
        u32 act = activityCounter;
        reset();
        for (int i = 0; i < kTargets; i++) disks[i] = keep[i];
        activityCounter = act;
        break;
    }
    case 0x03:                                          // reset SCSI bus
        connected = false;
        phase = BusFree;
        if (!(rregs[R_CFG1] & 0x40)) raise(INTR_RST);
        break;
    case 0x10: transferInfo(); break;                   // transfer information
    case 0x11:                                          // initiator command complete steps
        if (!connected) break;
        fifoCount = 0;
        push(status);
        push(0);                                        // COMMAND COMPLETE
        phase = MsgIn;
        raise(INTR_FC);
        break;
    case 0x12:                                          // message accepted
        connected = false;
        phase = BusFree;
        rregs[R_SEQ] = 0;
        raise(INTR_DC);
        break;
    case 0x18:                                          // transfer pad
        raise(INTR_BS);
        break;
    case 0x1A: case 0x1B: break;                        // set / reset ATN
    case 0x41: select(false, false); break;
    case 0x42: select(true, false); break;
    case 0x43: select(true, true); break;
    case 0x44: rregs[R_INTR] = 0; break;                // enable selection/reselection
    case 0x45: rregs[R_INTR] = 0; raise(0); break;      // disable selection/reselection
    default:
        raise(INTR_IL);
        break;
    }
}

void Esp::describe(char *o) const
{
    static const char hex[] = "0123456789ABCDEF";
    const u8 v[] = {(u8)phase, lastCmd, cdb[0], (u8)fifoCount, rregs[R_TCMID], rregs[R_TCLO], (u8)connected, (u8)dma, rregs[R_INTR], rregs[R_STAT]};
    const char *names[] = {"phase", "cmd", "cdb", "fifo", "tcm", "tcl", "conn", "dma", "intr", "stat"};
    int n = 0;
    for (int i = 0; i < 10; i++) {
        for (const char *p = names[i]; *p; p++) o[n++] = *p;
        o[n++] = '=';
        o[n++] = hex[v[i] >> 4];
        o[n++] = hex[v[i] & 15];
        o[n++] = ' ';
    }
    o[n] = 0;
}

u8 Esp::read(int reg)
{
    reg &= 15;
    switch (reg) {
    case R_FIFO: return pop();
    case R_STAT: {
        u8 v = rregs[R_STAT] & (STAT_TC | STAT_INT);
        if (connected) v |= (u8)(phase & 7);
        return v;
    }
    case R_INTR: {
        u8 v = rregs[R_INTR];
        rregs[R_INTR] = 0;
        rregs[R_STAT] &= STAT_TC;
        irqLine = false;
        return v;
    }
    case R_FLAGS: return (u8)((fifoCount & 0x1F) | (rregs[R_SEQ] << 5));
    case R_TCHI: return tchiWritten ? rregs[R_TCHI] : (u8)CHIP_ID;
    default: return rregs[reg];
    }
}

void Esp::write(int reg, u8 v)
{
    reg &= 15;
    switch (reg) {
    case R_TCHI:
        tchiWritten = true;
        /* fall through */
    case R_TCLO: case R_TCMID:
        rregs[R_STAT] &= (u8)~STAT_TC;
        break;
    case R_FIFO:
        push(v);
        commandBytes();
        break;
    case R_CMD:
        rregs[R_CMD] = v;
        lastCmd = v;
        command(v);
        break;
    case R_CFG1: case 0x0B: case 0x0C: case 0x0D: case 0x0F:
        rregs[reg] = v;
        break;
    default:
        break;
    }
    wregs[reg] = v;
}

u8 Esp::dmaRead()
{
    if (!connected) return 0;
    u8 v = pop();
    fillFifo();
    // Done once TC is exhausted (or the target stops sending) and at most one
    // byte is left: pseudo-DMA moves words, the odd byte is read from the FIFO.
    if (dma && !dmaDone && fifoCount < 2 && (tc() == 0 || phase != DataIn)) {
        dmaDone = true;
        raise(INTR_BS);
    }
    return v;
}

void Esp::dmaWrite(u8 v)
{
    if (!connected || (phase != DataOut && phase != Command)) return;
    if (dma) setTc(tc() ? tc() - 1 : 0);
    if (phase == Command) {
        push(v);
        commandBytes();
        return;
    }
    targetByteOut(v);
    if (dma && (tc() == 0 || phase != DataOut)) raise(INTR_BS);
}

} // namespace macemu
