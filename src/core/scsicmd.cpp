#include "scsicmd.h"

namespace macemu {

static ScsiResult status(u8 st = 0, u8 key = 0, u8 asc = 0)
{
    ScsiResult r = {ScsiStatus, 0, 0, 0, st, key, asc};
    return r;
}

static ScsiResult reply(u32 len, u32 alloc)
{
    ScsiResult r = {ScsiDataIn, len < alloc ? len : alloc, 0, 0, 0, 0, 0};
    if (!r.len) r.action = ScsiStatus;
    return r;
}

static void lbaToMsf(u32 lba, u8 *p)
{
    lba += 150;                                          // 2 s of lead-in
    p[0] = 0;
    p[1] = (u8)(lba / (60 * 75));
    p[2] = (u8)((lba / 75) % 60);
    p[3] = (u8)(lba % 75);
}

// CD-ROM only commands; returns true if handled.
static bool cdCommand(ScsiDevice &d, const u8 *cdb, u8 *buf, ScsiResult &r)
{
    u32 blocks2k = d.storage ? d.storage->size() / 2048 : 0;
    switch (cdb[0]) {
    case 0x43: {                                         // READ TOC: one data track
        bool msf = (cdb[1] & 2) != 0;
        u32 alloc = be16(cdb + 7);
        memFill(buf, 0, 20);
        buf[2] = 1; buf[3] = 1;                          // first and last track
        u8 *t = buf + 4;
        int n = 0;
        if (cdb[6] <= 1) {                               // track 1
            t[1] = 0x14; t[2] = 1;                       // data track, ADR 1
            if (msf) lbaToMsf(0, t + 4);
            t += 8; n++;
        }
        t[1] = 0x14; t[2] = 0xAA;                        // lead-out
        if (msf) lbaToMsf(blocks2k, t + 4); else putBe32(t + 4, blocks2k);
        n++;
        u32 len = 4 + 8 * (u32)n;
        buf[0] = (u8)((len - 2) >> 8); buf[1] = (u8)(len - 2);
        r = reply(len, alloc);
        return true;
    }
    case 0x42: {                                         // READ SUB-CHANNEL: no audio playing
        u32 alloc = be16(cdb + 7);
        memFill(buf, 0, 16);
        buf[1] = 0x15;                                   // audio status: none
        if (cdb[2] & 0x40) { buf[3] = 12; buf[4] = cdb[3]; buf[5] = 0x14; buf[6] = 1; buf[7] = 1; r = reply(16, alloc); }
        else r = reply(4, alloc);
        return true;
    }
    case 0x1B:                                           // START STOP UNIT: eject?
        if ((cdb[4] & 3) == 2) {
            if (d.locked) { r = status(2, 5, 0x53); return true; }   // medium removal prevented
            d.present = false;
            d.ejected = true;
        }
        r = status();
        return true;
    case 0x1E:                                           // PREVENT ALLOW MEDIUM REMOVAL
        d.locked = (cdb[4] & 1) != 0;
        r = status();
        return true;
    case 0x45: case 0x47: case 0x48: case 0x4B: case 0x4E:   // audio play / pause / stop
        r = status(2, 5, 0x64);                          // illegal mode for this track (data)
        return true;
    default:
        return false;
    }
}

ScsiResult scsiExecute(ScsiDevice &d, const u8 *cdb, u8 *buf, u8 senseKey, u8 senseAsc)
{
    u8 op = cdb[0];

    if (op == 0x03) {                                    // REQUEST SENSE
        memFill(buf, 0, 18);
        buf[0] = 0x70; buf[2] = senseKey; buf[7] = 10; buf[12] = senseAsc;
        return reply(18, cdb[4] ? cdb[4] : 4);
    }

    if (d.cdrom) {
        if (op == 0x12) {                                // INQUIRY: an AppleCD 300 (Sony CDU-8003)
            static const char id[] = "SONY    CD-ROM CDU-8003A1.9a";
            memFill(buf, 0, 36);
            buf[0] = 5;                                  // CD-ROM
            buf[1] = 0x80;                               // removable
            buf[2] = 2; buf[3] = 2; buf[4] = 31;
            memCopy(buf + 8, id, 28);
            return reply(36, cdb[4]);
        }
        if (d.unitAttention && d.present) {              // a new CD: report it once
            d.unitAttention = false;
            return status(2, 6, 0x28);
        }
        ScsiResult r;
        if (op == 0x1B || op == 0x1E) { cdCommand(d, cdb, buf, r); return r; }
        if (!d.present || !d.storage) return status(2, 2, 0x3A);   // no medium
        if (cdCommand(d, cdb, buf, r)) return r;
    }

    Storage *s = d.storage;
    u32 blocks = d.blocks();
    if (op == 0x5A && !d.cdrom) return status(2, 5, 0x20);   // hard disks: MODE SENSE(6) only
    switch (op) {
    case 0x00: case 0x01: case 0x04: case 0x07: case 0x0B: case 0x1B: case 0x1E:
    case 0x2B: case 0x2F: case 0x35:
        if (op == 0x35) s->flush();
        return status();

    case 0x12: {                                         // INQUIRY
        static const char id[] = "MACEMU  CIRCLE HARD DISK1.0 ";
        memFill(buf, 0, 36);
        buf[2] = 1; buf[3] = 1; buf[4] = 31;
        memCopy(buf + 8, id, 28);
        return reply(36, cdb[4]);
    }

    case 0x15: {                                         // MODE SELECT(6)
        ScsiResult r = {ScsiDataOut, cdb[4], 0, 0, 0, 0, 0};
        if (!r.len) r.action = ScsiStatus;
        return r;
    }

    case 0x1A: case 0x5A: {                              // MODE SENSE(6) / (10)
        bool ten = op == 0x5A;
        int page = cdb[2] & 0x3F;
        u32 h = ten ? 8 : 4;                             // header length
        memFill(buf, 0, 96);
        u32 n = h;
        if (d.cdrom) buf[ten ? 3 : 2] = 0x80;            // write protected
        else buf[2] = s->readOnly() ? 0x80 : 0;
        if (ten) buf[7] = 8; else buf[3] = 8;
        buf[n + 1] = (u8)(blocks >> 16); buf[n + 2] = (u8)(blocks >> 8); buf[n + 3] = (u8)blocks;
        buf[n + 5] = (u8)(d.blockBytes >> 16); buf[n + 6] = (u8)(d.blockBytes >> 8); buf[n + 7] = (u8)d.blockBytes;
        n += 8;
        if (!d.cdrom) {
            if (page == 0x03 || page == 0x3F) {          // format device
                buf[n] = 0x03; buf[n + 1] = 22;
                buf[n + 11] = 32;                        // sectors per track
                buf[n + 12] = 0x02;                      // bytes per sector
                n += 24;
            }
            if (page == 0x04 || page == 0x3F) {          // rigid disk geometry
                u32 cyl = blocks / (32 * 4);
                buf[n] = 0x04; buf[n + 1] = 22;
                buf[n + 2] = (u8)(cyl >> 16); buf[n + 3] = (u8)(cyl >> 8); buf[n + 4] = (u8)cyl;
                buf[n + 5] = 4;                          // heads
                n += 24;
            }
        } else {
            if (page == 0x01 || page == 0x3F) { buf[n] = 0x01; buf[n + 1] = 6; n += 8; }      // error recovery
            if (page == 0x0D || page == 0x3F) { buf[n] = 0x0D; buf[n + 1] = 6; buf[n + 5] = 60; buf[n + 7] = 75; n += 8; }
            if (page == 0x0E || page == 0x3F) {          // audio control
                buf[n] = 0x0E; buf[n + 1] = 14; buf[n + 2] = 4;
                buf[n + 8] = 1; buf[n + 9] = 0xFF; buf[n + 10] = 2; buf[n + 11] = 0xFF;
                n += 16;
            }
        }
        if (ten) { buf[0] = (u8)((n - 2) >> 8); buf[1] = (u8)(n - 2); return reply(n, be16(cdb + 7)); }
        buf[0] = (u8)(n - 1);
        return reply(n, cdb[4]);
    }

    case 0x25:                                           // READ CAPACITY
        putBe32(buf, blocks - 1);
        putBe32(buf + 4, d.blockBytes);
        return reply(8, 8);

    case 0x08: case 0x0A: case 0x28: case 0x2A: {        // READ / WRITE
        u32 lba, count;
        if (op < 0x20) {
            lba = ((u32)(cdb[1] & 0x1F) << 16) | ((u32)cdb[2] << 8) | cdb[3];
            count = cdb[4] ? cdb[4] : 256;
        } else {
            lba = be32(cdb + 2);
            count = be16(cdb + 7);
        }
        if (lba > blocks || count > blocks - lba) return status(2, 5, 0x21);
        if (!count) return status();
        bool isWrite = op == 0x0A || op == 0x2A;
        if (isWrite && (d.cdrom || s->readOnly())) return status(2, 7, 0x27);
        ScsiResult r = {isWrite ? ScsiWriteBlocks : ScsiReadBlocks, 0, lba, count, 0, 0, 0};
        return r;
    }

    default:
        return status(2, 5, 0x20);                       // illegal request
    }
}

void scsiDataOutDone(ScsiDevice &d, const u8 *cdb, const u8 *buf, u32 len)
{
    // MODE SELECT: a CD drive accepts a block length of 512 or 2048.
    if (!d.cdrom || cdb[0] != 0x15 || len < 12 || buf[3] < 8) return;
    u32 bl = ((u32)buf[9] << 16) | ((u32)buf[10] << 8) | buf[11];
    if (bl == 512 || bl == 2048) d.blockBytes = bl;
}

} // namespace macemu
