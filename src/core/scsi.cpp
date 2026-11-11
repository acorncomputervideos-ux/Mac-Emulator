#include "scsi.h"

namespace macemu {

enum {
    ICR_RST = 0x80, ICR_AIP = 0x40, ICR_LA = 0x20, ICR_ACK = 0x10,
    ICR_BSY = 0x08, ICR_SEL = 0x04, ICR_ATN = 0x02, ICR_DBUS = 0x01,
    MODE_DMA = 0x02, MODE_ARB = 0x01,
    BUS_MSG = 4, BUS_CD = 2, BUS_IO = 1,
};

Scsi::Scsi()
{
    for (int i = 0; i < kTargets; i++) disks[i] = 0;
    activityCounter = 0;
    senseKey = senseCode = 0;
    reset();
}

void Scsi::reset()
{
    odr = icr = mode = tcr = 0;
    aip = irq = dmaSend = dmaRecv = false;
    phase = Free;
    req = false;
    atnAtSelect = false;
    target = -1;
    cmdLen = cmdPos = 0;
    bufLen = bufPos = 0;
    lba = blocksLeft = 0;
    blockIo = false;
    status = 0;
}

void Scsi::flush()
{
    for (int i = 0; i < kTargets; i++) if (disks[i]) disks[i]->flush();
}

u8 Scsi::busPhaseBits() const
{
    switch (phase) {
    case Command: return BUS_CD;
    case DataIn: return BUS_IO;
    case Status: return BUS_CD | BUS_IO;
    case MsgIn: return BUS_MSG | BUS_CD | BUS_IO;
    case MsgOut: return BUS_MSG | BUS_CD;
    default: return 0;
    }
}

u8 Scsi::currentIn() const
{
    switch (phase) {
    case DataIn: return bufPos < bufLen ? buf[bufPos] : 0;
    case Status: return status;
    case MsgIn: return 0;                               // COMMAND COMPLETE
    default: return 0;
    }
}

void Scsi::setPhase(Phase p)
{
    phase = p;
    req = p != Free && p != Selected;
    if (p == Free) {
        target = -1;
        dmaSend = dmaRecv = false;
        if (mode & 0x04) irq = true;                    // monitor busy: loss of BSY
    }
    if (p == Command) { cmdPos = 0; cmdLen = 6; }
    // A phase change in the middle of a DMA transfer interrupts the chip.
    if ((mode & MODE_DMA) && p != Free && !phaseMatch()) irq = true;
}

void Scsi::finish(u8 st, u8 key, u8 code)
{
    status = st;
    senseKey = key;
    senseCode = code;
    blockIo = false;
    setPhase(Status);
}

// Send the first `len` bytes of buf, limited by the allocation length.
void Scsi::reply(u32 len, u32 alloc)
{
    bufLen = len < alloc ? len : alloc;
    bufPos = 0;
    status = 0;
    senseKey = senseCode = 0;
    if (bufLen) setPhase(DataIn); else setPhase(Status);
}

bool Scsi::loadBlock()
{
    activityCounter++;
    bufPos = 0;
    bufLen = 512;
    return disks[target]->read(lba * 512, buf, 512);
}

void Scsi::storeBlock()
{
    activityCounter++;
    disks[target]->write(lba * 512, buf, 512);
}

void Scsi::execute()
{
    Storage *d = disks[target];
    u32 blocks = d->size() / 512;
    u8 op = cmd[0];

    switch (op) {
    case 0x00:                                          // TEST UNIT READY
    case 0x01:                                          // REZERO UNIT
    case 0x04:                                          // FORMAT UNIT
    case 0x07:                                          // REASSIGN BLOCKS
    case 0x0B:                                          // SEEK(6)
    case 0x1B:                                          // START STOP UNIT
    case 0x1E:                                          // PREVENT ALLOW MEDIUM REMOVAL
    case 0x2B:                                          // SEEK(10)
    case 0x2F:                                          // VERIFY(10)
    case 0x35:                                          // SYNCHRONIZE CACHE
        if (op == 0x35) d->flush();
        finish(0);
        return;

    case 0x03: {                                        // REQUEST SENSE
        u32 alloc = cmd[4] ? cmd[4] : 4;
        memFill(buf, 0, 18);
        buf[0] = 0x70;
        buf[2] = senseKey;
        buf[7] = 10;
        buf[12] = senseCode;
        reply(18, alloc);
        return;
    }

    case 0x12: {                                        // INQUIRY
        static const char id[] = "MACEMU  CIRCLE HARD DISK1.0 ";
        memFill(buf, 0, 36);
        buf[2] = 1;
        buf[3] = 1;
        buf[4] = 31;
        memCopy(buf + 8, id, 28);
        reply(36, cmd[4]);
        return;
    }

    case 0x15:                                          // MODE SELECT(6)
        bufLen = cmd[4];
        bufPos = 0;
        blockIo = false;
        if (bufLen) setPhase(DataOut); else finish(0);
        return;

    case 0x1A: {                                        // MODE SENSE(6)
        int page = cmd[2] & 0x3F;
        memFill(buf, 0, 64);
        u32 n = 4;
        buf[2] = d->readOnly() ? 0x80 : 0;
        buf[3] = 8;
        buf[n + 1] = (u8)(blocks >> 16); buf[n + 2] = (u8)(blocks >> 8); buf[n + 3] = (u8)blocks;
        buf[n + 6] = 0x02;                              // 512 bytes per block
        n += 8;
        if (page == 0x03 || page == 0x3F) {             // format device
            buf[n] = 0x03; buf[n + 1] = 22;
            buf[n + 11] = 32;                           // sectors per track
            buf[n + 12] = 0x02;                         // bytes per sector
            n += 24;
        }
        if (page == 0x04 || page == 0x3F) {             // rigid disk geometry
            u32 cyl = blocks / (32 * 4);
            buf[n] = 0x04; buf[n + 1] = 22;
            buf[n + 2] = (u8)(cyl >> 16); buf[n + 3] = (u8)(cyl >> 8); buf[n + 4] = (u8)cyl;
            buf[n + 5] = 4;                             // heads
            n += 24;
        }
        buf[0] = (u8)(n - 1);
        reply(n, cmd[4]);
        return;
    }

    case 0x25:                                          // READ CAPACITY
        putBe32(buf, blocks - 1);
        putBe32(buf + 4, 512);
        reply(8, 8);
        return;

    case 0x08: case 0x0A: case 0x28: case 0x2A: {       // READ / WRITE
        bool six = op < 0x20;
        u32 count;
        if (six) {
            lba = ((u32)(cmd[1] & 0x1F) << 16) | ((u32)cmd[2] << 8) | cmd[3];
            count = cmd[4] ? cmd[4] : 256;
        } else {
            lba = be32(cmd + 2);
            count = be16(cmd + 7);
        }
        if (lba > blocks || count > blocks - lba) { finish(2, 5, 0x21); return; }
        if (count == 0) { finish(0); return; }
        bool isWrite = op == 0x0A || op == 0x2A;
        if (isWrite && d->readOnly()) { finish(2, 7, 0x27); return; }
        blocksLeft = count;
        blockIo = true;
        status = 0;
        senseKey = senseCode = 0;
        if (isWrite) { bufPos = 0; bufLen = 512; setPhase(DataOut); }
        else if (loadBlock()) setPhase(DataIn);
        else finish(2, 3, 0x11);
        return;
    }

    default:
        finish(2, 5, 0x20);                             // illegal request
        return;
    }
}

// The initiator has put a byte on the bus and acknowledged it.
void Scsi::byteOut(u8 v)
{
    switch (phase) {
    case MsgOut:
        setPhase(Command);
        return;
    case Command:
        if (cmdPos == 0) {
            int group = v >> 5;
            cmdLen = group == 0 ? 6 : group == 5 ? 12 : 10;
        }
        cmd[cmdPos++] = v;
        if (cmdPos >= cmdLen) execute();
        return;
    case DataOut:
        if (bufPos < 512) buf[bufPos] = v;
        bufPos++;
        if (bufPos < bufLen) return;
        if (!blockIo) { finish(0); return; }
        storeBlock();
        lba++;
        bufPos = 0;
        if (--blocksLeft == 0) finish(0);
        return;
    default:
        return;
    }
}

// The initiator has read the current byte and acknowledged it.
void Scsi::byteInTaken()
{
    switch (phase) {
    case DataIn:
        if (++bufPos < bufLen) return;
        if (blockIo && --blocksLeft > 0) {
            lba++;
            if (!loadBlock()) finish(2, 3, 0x11);
            return;
        }
        blockIo = false;
        setPhase(Status);
        return;
    case Status:
        setPhase(MsgIn);
        return;
    case MsgIn:
        setPhase(Free);
        return;
    default:
        return;
    }
}

u8 Scsi::read(int reg, bool dack)
{
    bool dmaActive = (mode & MODE_DMA) && dmaRecv;
    switch (reg & 7) {
    case 0:
    case 6: {
        u8 v;
        if (phase == Free || phase == Selected) v = (icr & ICR_DBUS) || aip ? odr : 0;
        else if (busPhaseBits() & BUS_IO) v = currentIn();
        else v = odr;
        if ((dack || ((reg & 7) == 6 && dmaActive)) && dmaActive && req && phaseMatch() && (busPhaseBits() & BUS_IO))
            byteInTaken();
        return v;
    }
    case 1:
        return (u8)((icr & 0x9F) | (aip ? ICR_AIP : 0));
    case 2:
        return mode;
    case 3:
        return tcr;
    case 4: {
        u8 v = 0;
        if (icr & ICR_RST) v |= 0x80;
        if ((phase != Free) || (icr & ICR_BSY)) v |= 0x40;
        if (req) v |= 0x20;
        u8 p = busPhaseBits();
        if (p & BUS_MSG) v |= 0x10;
        if (p & BUS_CD) v |= 0x08;
        if (p & BUS_IO) v |= 0x04;
        if (icr & ICR_SEL) v |= 0x02;
        return v;
    }
    case 5: {
        u8 v = 0;
        if ((mode & MODE_DMA) && (dmaSend || dmaRecv) && req && phaseMatch()) v |= 0x40;
        if (irq) v |= 0x10;
        if (phaseMatch()) v |= 0x08;
        if (icr & ICR_ATN) v |= 0x02;
        if (icr & ICR_ACK) v |= 0x01;
        return v;
    }
    default:                                            // reset parity / interrupts
        irq = false;
        return 0;
    }
}

void Scsi::write(int reg, u8 v, bool dack)
{
    switch (reg & 7) {
    case 0:
        odr = v;
        if ((mode & MODE_DMA) && dmaSend && req && phaseMatch() && !(busPhaseBits() & BUS_IO) &&
            phase != Free && phase != Selected)
            byteOut(v);
        (void)dack;
        return;

    case 1: {
        u8 old = icr;
        icr = v & 0x9F;
        if (v & ICR_RST) {                              // bus reset
            u8 keepIcr = icr;
            reset();
            icr = keepIcr;
            irq = true;
            return;
        }
        // Selection: SEL with the target's ID bit on the data bus, BSY released.
        if (phase == Free && (icr & ICR_SEL) && !(icr & ICR_BSY) && (icr & ICR_DBUS)) {
            for (int id = 0; id < kTargets; id++) {
                if (disks[id] && (odr & (1 << id))) {
                    target = id;
                    phase = Selected;
                    req = false;
                    atnAtSelect = (icr & ICR_ATN) != 0;
                    aip = false;
                    break;
                }
            }
        }
        if (phase == Selected && !(icr & ICR_SEL)) {
            if (icr & ICR_ATN) atnAtSelect = true;
            setPhase(atnAtSelect ? MsgOut : Command);
        }
        // Programmed handshake.
        if (!(old & ICR_ACK) && (icr & ICR_ACK) && req) {
            req = false;                                // target sees ACK, drops REQ
        } else if ((old & ICR_ACK) && !(icr & ICR_ACK) && !req && phase != Free && phase != Selected) {
            req = true;                                 // provisional; phase handlers may change it
            if (busPhaseBits() & BUS_IO) byteInTaken();
            else byteOut(odr);
        }
        return;
    }

    case 2:
        if ((v & MODE_ARB) && !(mode & MODE_ARB)) aip = phase == Free;
        if (!(v & MODE_ARB)) aip = false;
        if (!(v & MODE_DMA)) dmaSend = dmaRecv = false;
        mode = v;
        return;

    case 3:
        tcr = v;
        return;

    case 4:
        return;

    case 5:                                             // start DMA send
        if (mode & MODE_DMA) dmaSend = true;
        return;

    case 6:
        return;

    default:                                            // start DMA initiator receive
        if (mode & MODE_DMA) dmaRecv = true;
        return;
    }
}

} // namespace macemu
