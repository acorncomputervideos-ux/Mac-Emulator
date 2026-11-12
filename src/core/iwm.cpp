#include "iwm.h"

namespace macemu {

namespace gcr {

const u8 kEncode[64] = {
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6, 0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
};

static u8 gDecode[256];
static bool gDecodeReady = false;

static int decodeNibble(u8 n)
{
    if (!gDecodeReady) {
        for (int i = 0; i < 256; i++) gDecode[i] = 0xFF;
        for (int i = 0; i < 64; i++) gDecode[kEncode[i]] = (u8)i;
        gDecodeReady = true;
    }
    return gDecode[n] == 0xFF ? -1 : gDecode[n];
}

int sectorsPerTrack(int track) { return 12 - (track >> 4); }

u32 trackStartBlock(int track, int sides)
{
    u32 n = 0;
    for (int t = 0; t < track; t++) n += (u32)sectorsPerTrack(t);
    return n * (u32)sides;
}

void encodeSector(const u8 *in, u8 *out)
{
    u8 b1[175], b2[175], b3[175];
    u32 c1 = 0, c2 = 0, c3 = 0;
    int i = 0, j = 0;
    for (;;) {
        c1 = (c1 & 0xFF) << 1;
        if (c1 & 0x100) c1++;
        u8 v = in[i++];
        c3 += v;
        if (c1 & 0x100) { c3++; c1 &= 0xFF; }
        b1[j] = (u8)(v ^ c1);

        v = in[i++];
        c2 += v;
        if (c3 > 0xFF) { c2++; c3 &= 0xFF; }
        b2[j] = (u8)(v ^ c3);

        if (i == 524) break;

        v = in[i++];
        c1 += v;
        if (c2 > 0xFF) { c1++; c2 &= 0xFF; }
        b3[j] = (u8)(v ^ c2);
        j++;
    }
    b3[174] = 0;
    u32 c4 = ((c1 & 0xC0) >> 6) | ((c2 & 0xC0) >> 4) | ((c3 & 0xC0) >> 2);

    int n = 0;
    for (i = 0; i < 175; i++) {
        out[n++] = (u8)(((b1[i] & 0xC0) >> 2) | ((b2[i] & 0xC0) >> 4) | ((b3[i] & 0xC0) >> 6));
        out[n++] = b1[i] & 0x3F;
        out[n++] = b2[i] & 0x3F;
        if (i != 174) out[n++] = b3[i] & 0x3F;
    }
    out[n++] = (u8)(c4 & 0x3F);
    out[n++] = (u8)(c3 & 0x3F);
    out[n++] = (u8)(c2 & 0x3F);
    out[n++] = (u8)(c1 & 0x3F);
}

bool decodeSector(const u8 *nib, u8 *out)
{
    u8 b1[175], b2[175], b3[175];
    int n = 0;
    for (int i = 0; i < 175; i++) {
        u8 w4 = nib[n++], w1 = nib[n++], w2 = nib[n++];
        u8 w3 = i != 174 ? nib[n++] : 0;
        b1[i] = (u8)((w1 & 0x3F) | ((w4 << 2) & 0xC0));
        b2[i] = (u8)((w2 & 0x3F) | ((w4 << 4) & 0xC0));
        b3[i] = (u8)((w3 & 0x3F) | ((w4 << 6) & 0xC0));
    }
    u32 c1 = 0, c2 = 0, c3 = 0;
    int i = 0, j = 0;
    for (;;) {
        c1 = (c1 & 0xFF) << 1;
        if (c1 & 0x100) c1++;
        u8 v = (u8)(b1[j] ^ c1);
        c3 += v;
        if (c1 & 0x100) { c3++; c1 &= 0xFF; }
        out[i++] = v;

        v = (u8)(b2[j] ^ c3);
        c2 += v;
        if (c3 > 0xFF) { c2++; c3 &= 0xFF; }
        out[i++] = v;

        if (i == 524) break;

        v = (u8)(b3[j] ^ c2);
        c1 += v;
        if (c2 > 0xFF) { c1++; c2 &= 0xFF; }
        out[i++] = v;
        j++;
    }
    u32 c4 = ((c1 & 0xC0) >> 6) | ((c2 & 0xC0) >> 4) | ((c3 & 0xC0) >> 2);
    return nib[n] == (c4 & 0x3F) && nib[n + 1] == (c3 & 0x3F) &&
           nib[n + 2] == (c2 & 0x3F) && nib[n + 3] == (c1 & 0x3F);
}

} // namespace gcr

enum { kGapBytes = 14, kSyncBytes = 6, kSectorBytes = 740 };

Iwm::Iwm()
{
    for (int i = 0; i < kDrives; i++) {
        Drive &d = drv[i];
        d.present = true;
        d.image = 0;
        d.dataOffset = 0;
        d.sides = 2;
        d.writeProtect = false;
        d.motor = false;
        d.stepOut = false;
        d.track = 0;
        d.switched = false;
        d.hd = d.mfm = false;
        d.nibLen = 0;
        d.nibValid[0] = d.nibValid[1] = false;
        d.nibTrack = -1;
        d.pos = 0;
        d.writing = false;
        d.writeSide = d.writeStart = d.writeCount = 0;
        ejectedByGuest[i] = false;
    }
    sel = false;
    activityCounter = 0;
    reset();
}

void Iwm::reset()
{
    for (int i = 0; i < kDrives; i++) {
        if (drv[i].writing) endWrite(drv[i]);
        drv[i].motor = false;
    }
    ca0 = ca1 = ca2 = lstrb = enable = select = q6 = q7 = false;
    mode = 0;
    underrunCountdown = 0;
    ism = false;
    modeSeq = 0;
    ismReset();
}

void Iwm::ismReset()
{
    ismMode = ismSetup = ismPhases = ismError = 0;
    memFill(ismParam, 0, sizeof ismParam);
    paramIdx = 0;
    fifoCount = 0;
    crc = 0xFFFF;
    mfmPos = 0;
    hunting = true;
    lastSector = -1;
    lastWasMark = false;
    wlen = 0;
}

// ---------------------------------------------------------------- ISM (MFM)

static u16 crcByte(u16 crc, u8 v)
{
    crc ^= (u16)(v << 8);
    for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (u16)((crc << 1) ^ 0x1021) : (u16)(crc << 1);
    return crc;
}

// An IBM-format HD track: 18 sectors of 512 bytes.  Generated on the fly
// from the image; positions are byte offsets in this layout.
namespace {
enum {
    kGap4a = 80, kSync = 12, kGap1 = 50, kGap2 = 22, kGap3 = 84,
    kIdField = kSync + 3 + 1 + 4 + 2,                   // sync, A1 x3, FE, C H R N, CRC
    kDataField = kSync + 3 + 1 + 512 + 2,                // sync, A1 x3, FB, data, CRC
    kSectorLen = kIdField + kGap2 + kDataField + kGap3,
    kPreamble = kGap4a + kSync + 4 + kGap1,             // gap, sync, C2 x3 FC, gap
    kMfmSectors = 18,
    kMfmTrackLen = kPreamble + kMfmSectors * kSectorLen + 200,
};
}

Iwm::Drive *Iwm::ismDrive()
{
    if (ismMode & 0x02) return &drv[0];
    if (ismMode & 0x04) return &drv[1];
    return 0;
}

bool Iwm::ismReading() const
{
    return (ismMode & 0x08) && !(ismMode & 0x10);
}

// The byte at mfmPos (then advance): marks are A1 (and C2 for the index
// mark) written with a missing clock, which the ISM reports as marks.
bool Iwm::mfmByte(u8 &v, bool &mark)
{
    Drive *pd = ismDrive();
    if (!pd || !pd->image || !pd->hd) return false;
    Drive &d = *pd;
    int side = (ismMode & 0x20) ? 1 : 0;
    int p = mfmPos;
    mfmPos = (mfmPos + 1) % kMfmTrackLen;
    mark = false;
    if (p < kPreamble) {
        if (p < kGap4a) { v = 0x4E; return true; }
        p -= kGap4a;
        if (p < kSync) { v = 0; return true; }
        p -= kSync;
        if (p < 3) { v = 0xC2; mark = true; return true; }
        if (p == 3) { v = 0xFC; return true; }
        v = 0x4E;
        return true;
    }
    p -= kPreamble;
    int sector = p / kSectorLen;
    if (sector >= kMfmSectors) { v = 0x4E; return true; }
    int q = p % kSectorLen;
    static u8 id[4], data[512];
    static u16 idCrc, dataCrc;
    static int cachedFor = -1;
    int key = ((d.track * 2 + side) * kMfmSectors + sector) * 2 + (int)(pd - drv);
    if (q == 0 || cachedFor != key) {
        id[0] = (u8)d.track; id[1] = (u8)side; id[2] = (u8)(sector + 1); id[3] = 2;
        u16 c = 0xFFFF;
        for (int i = 0; i < 3; i++) c = crcByte(c, 0xA1);
        c = crcByte(c, 0xFE);
        for (int i = 0; i < 4; i++) c = crcByte(c, id[i]);
        idCrc = c;
        u32 block = (u32)((d.track * 2 + side) * kMfmSectors + sector);
        if (!d.image->read(d.dataOffset + block * 512, data, 512)) memFill(data, 0, 512);
        c = 0xFFFF;
        for (int i = 0; i < 3; i++) c = crcByte(c, 0xA1);
        c = crcByte(c, 0xFB);
        for (int i = 0; i < 512; i++) c = crcByte(c, data[i]);
        dataCrc = c;
        cachedFor = key;
    }
    if (q < kSync) { v = 0; return true; }
    q -= kSync;
    if (q < 3) { v = 0xA1; mark = true; return true; }
    q -= 3;
    if (q == 0) { v = 0xFE; return true; }
    q -= 1;
    if (q < 4) { v = id[q]; if (q == 2) lastSector = sector; return true; }
    q -= 4;
    if (q < 2) { v = (u8)(q == 0 ? idCrc >> 8 : idCrc); return true; }
    q -= 2;
    if (q < kGap2) { v = 0x4E; return true; }
    q -= kGap2;
    if (q < kSync) { v = 0; return true; }
    q -= kSync;
    if (q < 3) { v = 0xA1; mark = true; return true; }
    q -= 3;
    if (q == 0) { v = 0xFB; return true; }
    q -= 1;
    if (q < 512) { v = data[q]; if (q == 0) activityCounter++; return true; }
    q -= 512;
    if (q < 2) { v = (u8)(q == 0 ? dataCrc >> 8 : dataCrc); return true; }
    v = 0x4E;
    return true;
}

void Iwm::fillFifo()
{
    while (fifoCount < 2 && ismReading()) {
        u8 v;
        bool mark;
        if (!mfmByte(v, mark)) return;
        if (hunting) {                                  // nothing until the first mark
            if (!mark) continue;
            hunting = false;
        }
        fifo[fifoCount].v = v;
        fifo[fifoCount].mark = mark;
        fifoCount++;
    }
}

// Write mode ended: find the fields that were written.  An ID field (FE)
// names the sector of the data field (FB) that follows; a data field on its
// own goes to the sector of the last ID field read (a normal sector write).
void Iwm::mfmEndWrite()
{
    Drive *pd = ismDrive();
    if (pd && pd->image && pd->hd && !pd->writeProtect) {
        Drive &d = *pd;
        int side = (ismMode & 0x20) ? 1 : 0;
        int sector = lastSector;
        for (int i = 0; i + 3 < wlen; i++) {
            if (wbuf[i] != 0xA1 || wbuf[i + 1] != 0xA1 || wbuf[i + 2] != 0xA1) continue;
            u8 kind = wbuf[i + 3];
            if (kind == 0xFE && i + 8 <= wlen) { sector = wbuf[i + 6] - 1; i += 7; continue; }
            if (kind == 0xFB && i + 4 + 512 <= wlen && sector >= 0 && sector < kMfmSectors) {
                u32 block = (u32)((d.track * 2 + side) * kMfmSectors + sector);
                d.image->write(d.dataOffset + block * 512, wbuf + i + 4, 512);
                activityCounter++;
                i += 3 + 512;
            }
        }
    }
    wlen = 0;
}

void Iwm::ismPhase(u8 v)
{
    ismPhases = v;
    bool strobe = (v & 0x08) != 0;
    ca0 = (v & 0x01) != 0;
    ca1 = (v & 0x02) != 0;
    ca2 = (v & 0x04) != 0;
    select = (ismMode & 0x04) != 0;
    if (strobe && !lstrb && (ismMode & 0x06)) command();   // only an enabled drive sees the lines
    lstrb = strobe;
}

u8 Iwm::ismRead(int reg)
{
    switch (reg) {
    case 0: case 1: {                                   // data, mark
        fillFifo();
        if (!fifoCount) return 0;
        u8 v = fifo[0].v;
        // CRC: restart at the first of a run of marks, then include every byte.
        if (fifo[0].mark && !lastWasMark) crc = 0xFFFF;
        crc = crcByte(crc, v);
        lastWasMark = fifo[0].mark;
        fifo[0] = fifo[1];
        fifoCount--;
        return v;
    }
    case 2: { u8 e = ismError; ismError = 0; return e; }
    case 3: return ismParam[paramIdx++ & 15];
    case 4: return ismPhases;
    case 5: return ismSetup;
    case 6: return ismMode;
    default: {                                          // handshake
        fillFifo();
        u8 h = 0;
        if (ismMode & 0x10) h |= 0xC0;                  // writing: room in the FIFO
        else {
            if (fifoCount >= 1) h |= 0x80;
            if (fifoCount >= 2) h |= 0x40;
            if (fifoCount >= 1 && fifo[0].mark) h |= 0x01;
        }
        if (ismError) h |= 0x20;
        // bit 1: CRC error.  The fields generated here always check out.
        if (sense(0)) h |= 0x08;
        return h;
    }
    }
}

void Iwm::ismWrite(int reg, u8 v)
{
    switch (reg) {
    case 0: case 1:                                     // data, mark
        if (wlen < (int)sizeof wbuf) wbuf[wlen++] = v;
        break;
    case 2: break;                                      // CRC: the chip appends it
    case 3: ismParam[paramIdx++ & 15] = v; break;
    case 4: ismPhase(v); break;
    case 5: ismSetup = v; break;
    case 6: {                                           // mode: clear bits
        paramIdx = 0;
        u8 old = ismMode;
        ismMode &= (u8)~v;
        if ((old & 0x10) && !(ismMode & 0x10)) mfmEndWrite();
        if ((old & 0x08) && !(ismMode & 0x08)) { if (old & 0x10) mfmEndWrite(); hunting = true; fifoCount = 0; }
        if (v & 0x40) {                                 // back to IWM mode
            ism = false;
            modeSeq = 0;
        }
        enable = (ismMode & 0x80) != 0;
        break;
    }
    default: {                                          // mode: set bits
        paramIdx = 0;
        u8 old = ismMode;
        ismMode |= v;
        if (v & 0x01) { fifoCount = 0; ismMode &= (u8)~0x01; hunting = true; }   // clear FIFO
        if (!(old & 0x08) && (ismMode & 0x08)) {        // action: start reading or writing
            hunting = true;
            fifoCount = 0;
            wlen = 0;
        }
        enable = (ismMode & 0x80) != 0;
        select = (ismMode & 0x04) != 0;
        break;
    }
    }
}

bool Iwm::insert(int drive, Storage *image, bool forceReadOnly)
{
    Drive &d = drv[drive];
    if (d.image || !image) return false;
    u32 size = image->size();
    u32 offset = 0, dataSize = size;
    u8 hdr[84];
    if (size > 84 && image->read(0, hdr, 84) && hdr[0] <= 63 && be16(hdr + 0x52) == 0x0100) {
        u32 ds = be32(hdr + 0x40);                      // DiskCopy 4.2
        if (ds + 84 <= size) { offset = 84; dataSize = ds; }
    }
    d.hd = false;
    if (dataSize == 409600) d.sides = 1;
    else if (dataSize == 819200) d.sides = 2;
    else if (dataSize == 1474560 && superDrive) { d.sides = 2; d.hd = true; }
    else return false;
    d.image = image;
    d.dataOffset = offset;
    d.writeProtect = forceReadOnly || image->readOnly();
    d.mfm = superDrive;                                 // an FDHD drive starts in MFM mode
    d.nibValid[0] = d.nibValid[1] = false;
    d.nibTrack = -1;
    d.switched = true;
    d.writing = false;
    return true;
}

Storage *Iwm::eject(int drive)
{
    Drive &d = drv[drive];
    if (d.writing) endWrite(d);
    Storage *s = d.image;
    if (s) s->flush();
    d.image = 0;
    d.motor = false;
    d.nibValid[0] = d.nibValid[1] = false;
    return s;
}

void Iwm::flush()
{
    for (int i = 0; i < kDrives; i++) {
        if (drv[i].writing) endWrite(drv[i]);
        if (drv[i].image) drv[i].image->flush();
    }
}

// Build the nibble stream for one side of the current track.
void Iwm::loadTrack(Drive &d, int side)
{
    if (d.nibTrack != d.track) {
        d.nibValid[0] = d.nibValid[1] = false;
        d.nibTrack = d.track;
    }
    if (d.nibValid[side]) return;

    int spt = gcr::sectorsPerTrack(d.track);
    d.nibLen = spt * kSectorBytes;
    u8 *p = d.nib[side];
    bool exists = d.image && side < d.sides;
    u32 base = gcr::trackStartBlock(d.track, d.sides) + (u32)(side * spt);
    u8 format = d.sides == 2 ? 0x22 : 0x02;

    for (int s = 0; s < spt; s++) {
        if (!exists) {                                  // unformatted side: no marks at all
            for (int i = 0; i < kSectorBytes; i++) *p++ = 0xFF;
            continue;
        }
        u8 sec[524];
        memFill(sec, 0, 12);
        if (!d.image->read(d.dataOffset + (base + (u32)s) * 512, sec + 12, 512)) memFill(sec + 12, 0, 512);
        activityCounter++;

        for (int i = 0; i < kGapBytes; i++) *p++ = 0xFF;
        u8 trk = (u8)(d.track & 0x3F), sd = (u8)((side << 5) | (d.track >> 6));
        *p++ = 0xD5; *p++ = 0xAA; *p++ = 0x96;
        *p++ = gcr::kEncode[trk];
        *p++ = gcr::kEncode[s];
        *p++ = gcr::kEncode[sd];
        *p++ = gcr::kEncode[format];
        *p++ = gcr::kEncode[(trk ^ s ^ sd ^ format) & 0x3F];
        *p++ = 0xDE; *p++ = 0xAA;
        for (int i = 0; i < kSyncBytes; i++) *p++ = 0xFF;
        *p++ = 0xD5; *p++ = 0xAA; *p++ = 0xAD;
        *p++ = gcr::kEncode[s];
        u8 nib[703];
        gcr::encodeSector(sec, nib);
        for (int i = 0; i < 703; i++) *p++ = gcr::kEncode[nib[i]];
        *p++ = 0xDE; *p++ = 0xAA; *p++ = 0xFF;
    }
    d.nibValid[side] = true;
    if (d.pos >= d.nibLen) d.pos = 0;
}

// A write burst has finished: pull every complete data field out of the
// region that was written and store it in the image.  The data field carries
// its own sector number, so this handles both single-sector rewrites and
// whole-track formatting.
void Iwm::endWrite(Drive &d)
{
    d.writing = false;
    if (!d.image || d.writeProtect || d.writeSide >= d.sides) return;
    int len = d.nibLen;
    if (len <= 0) return;
    int count = d.writeCount > len ? len : d.writeCount;
    const u8 *t = d.nib[d.writeSide];
    int spt = gcr::sectorsPerTrack(d.track);
    u32 base = gcr::trackStartBlock(d.track, d.sides) + (u32)(d.writeSide * spt);

    for (int i = 0; i + 3 + 1 + 703 <= count; i++) {
        int at = (d.writeStart + i) % len;
        if (t[at] != 0xD5 || t[(at + 1) % len] != 0xAA || t[(at + 2) % len] != 0xAD) continue;
        int sector = gcr::decodeNibble(t[(at + 3) % len]);
        if (sector < 0 || sector >= spt) continue;
        u8 nib[703];
        bool ok = true;
        for (int k = 0; k < 703; k++) {
            int v = gcr::decodeNibble(t[(at + 4 + k) % len]);
            if (v < 0) { ok = false; break; }
            nib[k] = (u8)v;
        }
        u8 sec[524];
        if (!ok || !gcr::decodeSector(nib, sec)) continue;
        d.image->write(d.dataOffset + (base + (u32)sector) * 512, sec + 12, 512);
        activityCounter++;
        i += 3 + 703;
    }
    d.nibValid[d.writeSide] = false;                    // re-encode cleanly on next access
}

u8 Iwm::readData()
{
    Drive &d = cur();
    if (!enable || !d.present || !d.motor || !d.image) return 0;
    if (d.hd) {                                         // MFM media: no GCR to be found
        static u32 noise = 0x12345678;
        noise = noise * 1103515245u + 12345u;
        return (u8)(0x80 | (noise >> 24)) & 0xEF;       // never a D5 AA 96 header
    }
    if (d.writing) endWrite(d);
    int side = sel ? 1 : 0;
    loadTrack(d, side);
    u8 v = d.nib[side][d.pos];
    if (++d.pos >= d.nibLen) d.pos = 0;
    return v;
}

void Iwm::writeData(u8 v)
{
    Drive &d = cur();
    underrunCountdown = 2;
    if (!enable || !d.present || !d.motor || !d.image) return;
    int side = sel ? 1 : 0;
    if (d.writing && d.writeSide != side) endWrite(d);
    loadTrack(d, side);
    if (!d.writing) {
        d.writing = true;
        d.writeSide = side;
        d.writeStart = d.pos;
        d.writeCount = 0;
    }
    d.nib[side][d.pos] = v;
    if (++d.pos >= d.nibLen) d.pos = 0;
    d.writeCount++;
}

// Drive status selected by CA2, CA1, CA0 and SEL.
bool Iwm::sense(u64 cpuCycles)
{
    bool r = senseImpl(cpuCycles);
    if (trace) trace(traceCtx, 48 + ((ca2 ? 8 : 0) | (ca1 ? 4 : 0) | (ca0 ? 2 : 0) | (sel ? 1 : 0)), false, r);
    return r;
}

bool Iwm::senseImpl(u64 cpuCycles)
{
    Drive &d = cur();
    if (!d.present) return true;
    int reg = (ca2 ? 8 : 0) | (ca1 ? 4 : 0) | (ca0 ? 2 : 0) | (sel ? 1 : 0);
    switch (reg) {
    case 0x0: return d.stepOut;                         // DIRTN  0 = toward higher tracks
    case 0x1: return d.image == 0;                      // CSTIN  0 = disk in place
    case 0x2: return true;                              // STEP   1 = step complete
    case 0x3: return !d.writeProtect;                   // WRTPRT 0 = protected
    case 0x4: return !d.motor;                          // MOTORON 0 = running
    case 0x5: return d.track != 0;                      // TK0    0 = at track 0
    case 0x6: return false;                             // SWITCHED
    case 0x7: {                                         // TACH   60 pulses per revolution
        if (!d.motor || !d.image) return false;
        static const u32 rpm[5] = {394, 429, 472, 525, 590};
        u64 edges = cpuCycles * (2 * rpm[d.track >> 4]) / 7833600ull;
        return (edges & 1) != 0;
    }
    case 0x8: case 0x9: return true;                    // RDDATA (instantaneous)
    case 0xA: return superDrive;                        // SUPERDRIVE 1 = FDHD
    case 0xB: return !d.mfm;                            // FDHD: 0 = in MFM mode
    case 0xC: return true;                              // SIDES  1 = double sided drive
    case 0xD: return false;                             // READY  0 = ready
    case 0xE: return false;                             // INSTALLED 0 = drive connected
    case 0xF: return superDrive ? !(d.image && d.hd) : true;   // FDHD: 0 = HD disk; else REVISED
    default: return true;
    }
}

// LSTRB pulsed: CA1, CA0 and SEL choose the command, CA2 is its argument.
void Iwm::command()
{
    Drive &d = cur();
    if (!d.present) return;
    int reg = (ca1 ? 4 : 0) | (ca0 ? 2 : 0) | (sel ? 1 : 0);
    if (trace) trace(traceCtx, 32 + reg, true, ca2);    // drive command, argument CA2
    switch (reg) {
    case 0: d.stepOut = ca2; break;
    case 1: if (ca2) d.switched = false; break;
    case 2:
        if (!ca2) {
            if (d.writing) endWrite(d);
            if (d.stepOut) { if (d.track > 0) d.track--; }
            else if (d.track < 79) d.track++;
        }
        break;
    case 3:                                             // FDHD: CA2 0 = MFM mode, 1 = GCR
        if (superDrive) d.mfm = !ca2;
        break;
    case 4:
        if (ca2 && d.writing) endWrite(d);
        d.motor = !ca2;
        break;
    case 6:
        if (ca2 && d.image) {
            int idx = select ? 1 : 0;
            if (d.writing) endWrite(d);
            d.image->flush();
            d.motor = false;
            ejectedByGuest[idx] = true;                 // frontend releases the storage
        }
        break;
    default: break;
    }
}

void Iwm::setLine(int line, u64 cpuCycles)
{
    (void)cpuCycles;
    bool on = (line & 1) != 0;
    switch (line >> 1) {
    case 0: ca0 = on; break;
    case 1: ca1 = on; break;
    case 2: ca2 = on; break;
    case 3:
        if (on && !lstrb) command();
        lstrb = on;
        break;
    case 4:
        if (!on && enable) for (int i = 0; i < kDrives; i++) if (drv[i].writing) endWrite(drv[i]);
        enable = on;
        break;
    case 5:
        if (on != select && cur().writing) endWrite(cur());
        select = on;
        break;
    case 6: q6 = on; break;
    default:
        if (!on && q7 && cur().writing) endWrite(cur());
        q7 = on;
        break;
    }
}

u8 Iwm::read(int line, u64 cpuCycles)
{
    u8 r = readImpl(line, cpuCycles);
    if (trace) trace(traceCtx, line, false, r);
    return r;
}

u8 Iwm::readImpl(int line, u64 cpuCycles)
{
    if (ism) return ismRead(line & 7);
    setLine(line, cpuCycles);
    if (!q7 && !q6) return enable ? readData() : 0xFF;
    if (!q7 && q6) return (u8)((sense(cpuCycles) ? 0x80 : 0) | (enable ? 0x20 : 0) | (mode & 0x1F));
    if (q7 && !q6) {                                    // handshake: ready, underrun
        u8 v = 0xBF;
        if (underrunCountdown > 0) { underrunCountdown--; v |= 0x40; }
        return v;
    }
    return 0xFF;
}

void Iwm::write(int line, u8 v, u64 cpuCycles)
{
    if (trace) trace(traceCtx, line, true, v);
    if (ism) { ismWrite(line & 7, v); return; }
    setLine(line, cpuCycles);
    if (q7 && q6) {
        if (enable) writeData(v);
        else {
            mode = v & 0x1F;
            // SWIM: bit 6 written 1, 0, 1, 1 switches to the ISM.
            if (superDrive) {
                bool b = (v & 0x40) != 0;
                static const bool seq[4] = {true, false, true, true};
                modeSeq = b == seq[modeSeq] ? modeSeq + 1 : (b ? 1 : 0);
                if (modeSeq == 4) { ism = true; modeSeq = 0; ismReset(); ismMode = 0x40; }
            }
        }
    }
}

} // namespace macemu
