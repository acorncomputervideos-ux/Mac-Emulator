// Hardware unit tests: GCR codec, IWM/floppy, SCSI, RTC, keyboard and a
// whole-machine smoke test driven by a small hand-assembled test ROM.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../src/core/mac.h"
#include "../src/core/keys.h"
#include "../src/core/esp.h"

using namespace macemu;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

struct MemStorage : Storage {
    std::vector<u8> data;
    bool ro = false;
    int flushes = 0;
    explicit MemStorage(u32 n) : data(n) {}
    u32 size() override { return (u32)data.size(); }
    bool read(u32 off, void *buf, u32 len) override { if (off + len > data.size()) return false; memcpy(buf, &data[off], len); return true; }
    bool write(u32 off, const void *buf, u32 len) override { if (ro || off + len > data.size()) return false; memcpy(&data[off], buf, len); return true; }
    bool readOnly() override { return ro; }
    void flush() override { flushes++; }
};

// ------------------------------------------------------------ GCR

static void testGcr()
{
    srand(1);
    for (int iter = 0; iter < 200; iter++) {
        u8 in[524], out[524], nib[703];
        for (auto &b : in) b = (u8)rand();
        gcr::encodeSector(in, nib);
        for (int i = 0; i < 703; i++) CHECK(nib[i] < 64);
        CHECK(gcr::decodeSector(nib, out));
        CHECK(!memcmp(in, out, 524));
        nib[rand() % 690] ^= 1;
        CHECK(!gcr::decodeSector(nib, out));
    }
    CHECK(gcr::sectorsPerTrack(0) == 12 && gcr::sectorsPerTrack(79) == 8);
    CHECK(gcr::trackStartBlock(80, 2) == 1600 && gcr::trackStartBlock(80, 1) == 800);
}

// ------------------------------------------------------------ IWM

// Talks to the IWM the way the ROM's .Sony driver does.
struct FloppyClient {
    Iwm &iwm;
    u64 t = 0;
    explicit FloppyClient(Iwm &i) : iwm(i) {}
    enum { CA0 = 0, CA1 = 2, CA2 = 4, LSTRB = 6, ENABLE = 8, SELECT = 10, Q6 = 12, Q7 = 14 };
    u8 rd(int line) { t += 100; return iwm.read(line, t); }
    void wr(int line, u8 v) { t += 100; iwm.write(line, v, t); }
    void lines(int reg) {                               // reg = CA2 CA1 CA0 SEL
        rd(CA2 + ((reg >> 3) & 1)); rd(CA1 + ((reg >> 2) & 1)); rd(CA0 + ((reg >> 1) & 1));
        iwm.setSel(reg & 1);
    }
    bool status(int reg) { lines(reg); rd(Q7); return (rd(Q6 + 1) & 0x80) != 0; }
    void command(int reg) { lines(reg); rd(LSTRB + 1); rd(LSTRB); }
    u8 nibble() { return rd(Q6); }                      // Q7 is already low in read mode
    static int dec(u8 n) { for (int i = 0; i < 64; i++) if (gcr::kEncode[i] == n) return i; return -1; }

    // Find the address field for `sector`, leaving the head just past it.
    bool seekSector(int side, int track, int sector) {
        iwm.setSel(side);
        rd(Q7);
        for (int tries = 0; tries < 40000; tries++) {
            if (nibble() != 0xD5) continue;
            if (nibble() != 0xAA) continue;
            if (nibble() != 0x96) continue;
            int trk = dec(nibble()), sec = dec(nibble()), sd = dec(nibble()), fmt = dec(nibble()), sum = dec(nibble());
            if ((trk ^ sec ^ sd ^ fmt) != sum) return false;
            if (trk != (track & 0x3F) || (sd >> 5) != side) return false;
            if (nibble() != 0xDE || nibble() != 0xAA) return false;
            if (sec == sector) return true;
        }
        return false;
    }
    bool readSector(int side, int track, int sector, u8 *data512) {
        if (!seekSector(side, track, sector)) return false;
        for (int i = 0; i < 20; i++) if (nibble() == 0xD5) break;
        if (nibble() != 0xAA || nibble() != 0xAD) return false;
        if (dec(nibble()) != sector) return false;
        u8 nib[703], sec[524];
        for (auto &n : nib) { int v = dec(nibble()); if (v < 0) return false; n = (u8)v; }
        if (!gcr::decodeSector(nib, sec)) return false;
        memcpy(data512, sec + 12, 512);
        return nibble() == 0xDE && nibble() == 0xAA;
    }
    void writeSector(int side, int track, int sector, const u8 *data512) {
        if (!seekSector(side, track, sector)) return;
        u8 sec[524] = {0}, nib[703];
        memcpy(sec + 12, data512, 512);
        gcr::encodeSector(sec, nib);
        rd(Q6 + 1);
        wr(Q7 + 1, 0xFF);                               // enter write mode with the first sync byte
        for (int i = 0; i < 5; i++) wr(Q6 + 1, 0xFF);
        wr(Q6 + 1, 0xD5); wr(Q6 + 1, 0xAA); wr(Q6 + 1, 0xAD);
        wr(Q6 + 1, gcr::kEncode[sector]);
        for (u8 n : nib) wr(Q6 + 1, gcr::kEncode[n]);
        wr(Q6 + 1, 0xDE); wr(Q6 + 1, 0xAA); wr(Q6 + 1, 0xFF);
        rd(Q7);                                         // leave write mode
        rd(Q6);
    }
};

static void testFloppy()
{
    for (int sides = 1; sides <= 2; sides++) {
        Iwm iwm;
        MemStorage disk(sides == 2 ? 819200 : 409600);
        for (size_t i = 0; i < disk.data.size(); i++) disk.data[i] = (u8)(i * 7 + (i >> 9) * 13);
        FloppyClient c(iwm);

        // Mode register handshake as done by the ROM at startup.
        c.rd(FloppyClient::ENABLE);
        c.rd(FloppyClient::Q6 + 1);
        c.wr(FloppyClient::Q7 + 1, 0x1F);
        c.rd(FloppyClient::Q7);
        CHECK((c.rd(FloppyClient::Q6 + 1) & 0x1F) == 0x1F);
        c.rd(FloppyClient::Q6);

        c.rd(FloppyClient::SELECT);                     // internal drive
        c.rd(FloppyClient::ENABLE + 1);
        CHECK(!c.status(0xE));                          // drive installed
        CHECK(c.status(0xC));                           // double sided mechanism
        CHECK(c.status(0x1));                           // no disk
        CHECK(iwm.insert(0, &disk));
        CHECK(!c.status(0x1));                          // disk in place
        CHECK(c.status(0x3));                           // not write protected
        c.command(0x4);                                 // motor on (CA1, CA2=0)
        CHECK(!c.status(0x4));
        CHECK(!c.status(0x5));                          // at track 0

        // Tachometer toggles while spinning.
        int toggles = 0; bool last = c.status(0x7);
        for (int i = 0; i < 2000; i++) { c.t += 5000; bool s = c.status(0x7); if (s != last) toggles++; last = s; }
        CHECK(toggles > 100);

        // Step to track 37 and read every sector on every side.
        c.command(0x0);                                 // direction: inward
        for (int i = 0; i < 37; i++) c.command(0x2);
        CHECK(c.status(0x5));
        int spt = gcr::sectorsPerTrack(37);
        for (int side = 0; side < sides; side++) {
            for (int s = 0; s < spt; s++) {
                u8 buf[512];
                CHECK(c.readSector(side, 37, s, buf));
                u32 lba = gcr::trackStartBlock(37, sides) + (u32)(side * spt + s);
                CHECK(!memcmp(buf, &disk.data[lba * 512], 512));
            }
        }

        // Rewrite one sector and read it back.
        u8 pattern[512], back[512];
        for (int i = 0; i < 512; i++) pattern[i] = (u8)(0xC3 ^ i);
        int side = sides - 1;
        c.writeSector(side, 37, 5, pattern);
        u32 lba = gcr::trackStartBlock(37, sides) + (u32)(side * spt + 5);
        CHECK(!memcmp(pattern, &disk.data[lba * 512], 512));
        CHECK(c.readSector(side, 37, 5, back) && !memcmp(pattern, back, 512));
        // Neighbours untouched.
        CHECK(c.readSector(side, 37, 6, back) && !memcmp(back, &disk.data[(lba + 1) * 512], 512));

        // Back to track 0, then eject.
        c.command(0x8);                                 // direction: outward (CA2=1)
        for (int i = 0; i < 80; i++) c.command(0x2);
        CHECK(!c.status(0x5));
        c.command(0xE);                                 // eject (CA2 CA1 CA0)
        CHECK(iwm.ejectedByGuest[0]);
        CHECK(iwm.eject(0) == &disk);
        CHECK(disk.flushes > 0);
        CHECK(c.status(0x1));
    }

    // DiskCopy 4.2 container and bad sizes.
    Iwm iwm;
    MemStorage dc(84 + 819200 + 100), bad(123456), hd(1474560);
    dc.data[0] = 5; dc.data[0x41] = 0x0C; dc.data[0x42] = 0x80; dc.data[0x52] = 1; dc.data[0x53] = 0;
    dc.data[84] = 0x4C; dc.data[85] = 0x4B;
    CHECK(iwm.insert(0, &dc));
    CHECK(!iwm.insert(1, &bad));
    CHECK(!iwm.insert(1, &hd));
    FloppyClient c(iwm);
    c.rd(FloppyClient::ENABLE + 1);
    c.command(0x4);
    u8 buf[512];
    CHECK(c.readSector(0, 0, 0, buf) && buf[0] == 0x4C && buf[1] == 0x4B);
}

// ------------------------------------------------------------ SCSI

struct ScsiClient {
    Scsi &s;
    explicit ScsiClient(Scsi &sc) : s(sc) {}
    bool select(int id) {
        s.write(0, 0x80, false);                        // our ID
        s.write(2, 0x01, false);                        // arbitrate
        if (!(s.read(1, false) & 0x40)) return false;   // AIP
        if (s.read(1, false) & 0x20) return false;      // lost
        s.write(1, 0x0C, false);                        // SEL + BSY
        s.write(2, 0x00, false);
        s.write(0, (u8)(0x80 | (1 << id)), false);
        s.write(1, 0x05, false);                        // SEL + data bus, BSY released
        if (!(s.read(4, false) & 0x40)) { s.write(1, 0, false); return false; }
        s.write(1, 0x00, false);                        // release SEL
        return true;
    }
    bool waitReq() { return (s.read(4, false) & 0x20) != 0; }
    int phase() { return (s.read(4, false) >> 2) & 7; }
    bool putByte(u8 v) {
        if (!waitReq() || !(s.read(5, false) & 0x08)) return false;
        s.write(0, v, false);
        s.write(1, 0x01, false);
        s.write(1, 0x11, false);                        // ACK
        if (s.read(4, false) & 0x20) return false;      // REQ must drop
        s.write(1, 0x00, false);
        return true;
    }
    int getByte() {
        if (!waitReq() || !(s.read(5, false) & 0x08)) return -1;
        u8 v = s.read(0, false);
        s.write(1, 0x10, false);
        s.write(1, 0x00, false);
        return v;
    }
    bool command(const u8 *cdb, int n) {
        s.write(3, 2, false);                           // expect COMMAND phase
        for (int i = 0; i < n; i++) if (!putByte(cdb[i])) return false;
        return true;
    }
    int dmaRead(u8 *buf, int max) {
        s.write(3, 1, false);                           // DATA IN
        s.write(2, 0x02, false);
        s.write(7, 0, false);
        int n = 0;
        while (n < max && (s.read(5, false) & 0x40)) buf[n++] = s.read(6, true);
        s.write(2, 0, false);
        return n;
    }
    int dmaWrite(const u8 *buf, int max) {
        s.write(3, 0, false);                           // DATA OUT
        s.write(2, 0x02, false);
        s.write(5, 0, false);
        int n = 0;
        while (n < max && (s.read(5, false) & 0x40)) s.write(0, buf[n++], true);
        s.write(2, 0, false);
        return n;
    }
    int complete() {                                    // returns status, -1 on protocol error
        s.write(3, 3, false);
        int st = getByte();
        s.write(3, 7, false);
        int msg = getByte();
        if (msg != 0 || (s.read(4, false) & 0x40)) return -1;
        return st;
    }
};

static void testScsi()
{
    Scsi scsi;
    MemStorage disk(2048 * 512);
    for (size_t i = 0; i < disk.data.size(); i++) disk.data[i] = (u8)(i ^ (i >> 9));
    scsi.attach(3, &disk);
    ScsiClient c(scsi);
    u8 buf[4096];

    CHECK(!c.select(1));                                // nobody home
    CHECK(c.select(3));
    CHECK(c.phase() == 2);                              // COMMAND
    const u8 inq[6] = {0x12, 0, 0, 0, 36, 0};
    CHECK(c.command(inq, 6));
    CHECK(c.dmaRead(buf, 100) == 36);
    CHECK(buf[0] == 0 && !memcmp(buf + 8, "MACEMU", 6));
    CHECK(c.complete() == 0);

    CHECK(c.select(3));
    const u8 cap[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(c.command(cap, 10));
    CHECK(c.dmaRead(buf, 100) == 8);
    CHECK(be32(buf) == 2047 && be32(buf + 4) == 512);
    CHECK(c.complete() == 0);

    CHECK(c.select(3));
    const u8 rd6[6] = {0x08, 0, 0, 5, 3, 0};
    CHECK(c.command(rd6, 6));
    CHECK(c.dmaRead(buf, 4096) == 1536);
    CHECK(!memcmp(buf, &disk.data[5 * 512], 1536));
    CHECK(c.complete() == 0);

    // Programmed-I/O read of the same data.
    CHECK(c.select(3));
    const u8 rd10[10] = {0x28, 0, 0, 0, 0, 9, 0, 0, 1, 0};
    CHECK(c.command(rd10, 10));
    scsi.write(3, 1, false);
    bool same = true;
    for (int i = 0; i < 512; i++) if (c.getByte() != disk.data[9 * 512 + i]) same = false;
    CHECK(same);
    CHECK(c.complete() == 0);

    CHECK(c.select(3));
    const u8 wr6[6] = {0x0A, 0, 0, 100, 2, 0};
    u8 pat[1024];
    for (int i = 0; i < 1024; i++) pat[i] = (u8)(i * 3);
    CHECK(c.command(wr6, 6));
    CHECK(c.dmaWrite(pat, 1024) == 1024);
    CHECK(c.complete() == 0);
    CHECK(!memcmp(pat, &disk.data[100 * 512], 1024));

    // Out of range read -> CHECK CONDITION, then REQUEST SENSE reports it.
    CHECK(c.select(3));
    const u8 bad[6] = {0x08, 0x1F, 0xFF, 0xFF, 1, 0};
    CHECK(c.command(bad, 6));
    CHECK(c.complete() == 2);
    CHECK(c.select(3));
    const u8 sense[6] = {0x03, 0, 0, 0, 18, 0};
    CHECK(c.command(sense, 6));
    CHECK(c.dmaRead(buf, 100) == 18);
    CHECK(buf[2] == 5);
    CHECK(c.complete() == 0);

    // Read-only media refuses writes.
    disk.ro = true;
    CHECK(c.select(3));
    CHECK(c.command(wr6, 6));
    CHECK(c.complete() == 2);
}

// ------------------------------------------------------------ whole machine

static void put16(std::vector<u8> &r, u32 at, std::initializer_list<u16> words)
{
    for (u16 w : words) { r[at++] = (u8)(w >> 8); r[at++] = (u8)w; }
}

// A tiny "ROM": leaves overlay mode, stores a marker, paints the screen,
// installs a VBL handler and spins.
static std::vector<u8> makeTestRom()
{
    std::vector<u8> r(0x20000, 0xFF);
    put16(r, 0, {0x0000, 0x2000, 0x0040, 0x0010});      // SSP, PC
    put16(r, 0x10, {
        0x13FC, 0x0060, 0x00EF, 0xE3FE,                 // move.b #$60,vBufA      (overlay off)
        0x13FC, 0x00FF, 0x00EF, 0xE7FE,                 // move.b #$FF,vDirA
        0x31FC, 0x1234, 0x0100,                         // move.w #$1234,$100.w
        0x41F9, 0x003F, 0xA700,                         // lea    $3FA700,a0      (screen)
        0x303C, 0x2ABF,                                 // move.w #10943,d0
        0x30FC, 0xAAAA,                                 // move.w #$AAAA,(a0)+
        0x51C8, 0xFFFA,                                 // dbra   d0,*-4
        0x21FC, 0x0040, 0x0080, 0x0064,                 // move.l #$400080,$64.w  (level 1 autovector)
        0x13FC, 0x0082, 0x00EF, 0xFDFE,                 // move.b #$82,vIER       (enable CA1 = VBL)
        0x46FC, 0x2000,                                 // move   #$2000,sr
        0x60FE,                                         // bra.s  *
    });
    put16(r, 0x80, {
        0x5278, 0x0102,                                 // addq.w #1,$102.w
        0x4A39, 0x00EF, 0xE3FE,                         // tst.b  vBufA           (acknowledge CA1)
        0x4E73,                                         // rte
    });
    return r;
}

struct Rig {
    std::vector<u8> rom = makeTestRom();
    std::vector<u8> ram = std::vector<u8>(0x400000);
    Machine m;
    Rig() { if (!m.init(rom.data(), (u32)rom.size(), ram.data(), (u32)ram.size())) abort(); }
    enum { vBufB = 0xEFE1FE, vDirB = 0xEFE5FE, vSR = 0xEFF5FE, vACR = 0xEFF7FE, vIFR = 0xEFFBFE };
};

// 53C96: an unaligned buffer makes the Mac's driver put the first word in
// the FIFO by hand and DMA the rest.  The FIFO bytes must reach the disk
// first (they used to be left behind, shifting and wrecking the block).
static void testEspUnalignedWrite()
{
    MemStorage disk(64 * 512);
    Esp esp;
    esp.attach(0, &disk);
    u8 data[1024];
    for (int i = 0; i < 1024; i++) data[i] = (u8)(i * 7 + 3);

    esp.write(4, 0);                                    // target ID 0
    const u8 cdb[10] = {0x2A, 0, 0, 0, 0, 5, 0, 0, 2, 0};   // WRITE(10) LBA 5, 2 blocks
    for (u8 b : cdb) esp.write(2, b);
    esp.write(3, 0x41);                                 // select, CDB from the FIFO
    esp.read(5);
    esp.write(2, data[0]);                              // first word by hand
    esp.write(2, data[1]);
    esp.write(0, (u8)(1022 & 0xFF));                    // TC = the rest
    esp.write(1, (u8)(1022 >> 8));
    esp.write(14, 0);
    esp.write(3, 0x90);                                 // DMA transfer information
    for (int i = 2; i < 1024; i++) esp.dmaWrite(data[i]);
    CHECK(memcmp(&disk.data[5 * 512], data, 1024) == 0);
    CHECK(disk.data[7 * 512] == 0 && disk.data[5 * 512 - 1] == 0);
}

// Run one command on the 53C96 the way the Mac's driver does: select with
// the CDB in the FIFO, pseudo-DMA the data in, then status and message.
static std::vector<u8> espCommand(Esp &e, int id, const std::vector<u8> &cdb, u32 want, u8 &status)
{
    e.write(4, (u8)id);
    for (u8 b : cdb) e.write(2, b);
    e.write(3, 0x41);
    e.read(5);
    std::vector<u8> out;
    if (want) {
        e.write(0, (u8)want); e.write(1, (u8)(want >> 8)); e.write(14, (u8)(want >> 16));
        e.write(3, 0x90);
        for (u32 i = 0; i < want; i++) out.push_back(e.dmaRead());
        e.read(5);
    }
    e.write(3, 0x11);                                   // initiator command complete
    status = e.read(2);
    e.read(2);
    e.read(5);
    e.write(3, 0x12);                                   // message accepted
    e.read(5);
    return out;
}

// A CD-ROM image on SCSI ID 3: identity, disc change, 2048- and 512-byte
// blocks, table of contents, write protection, lock and eject.
static void testCdrom()
{
    MemStorage cd(300 * 2048);
    for (u32 i = 0; i < cd.data.size(); i++) cd.data[i] = (u8)(i / 2048 + i % 251);
    Esp esp;
    esp.device(3).insertCd(&cd);
    u8 st;

    auto inq = espCommand(esp, 3, {0x12, 0, 0, 0, 36, 0}, 36, st);
    CHECK(st == 0 && inq[0] == 5 && (inq[1] & 0x80) && memcmp(&inq[8], "SONY    ", 8) == 0);

    espCommand(esp, 3, {0x00, 0, 0, 0, 0, 0}, 0, st);
    CHECK(st == 2);                                     // unit attention: a new disc
    auto sense = espCommand(esp, 3, {0x03, 0, 0, 0, 18, 0}, 18, st);
    CHECK(sense[2] == 6 && sense[12] == 0x28);
    espCommand(esp, 3, {0x00, 0, 0, 0, 0, 0}, 0, st);
    CHECK(st == 0);

    auto cap = espCommand(esp, 3, {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 8, st);
    CHECK(be32(&cap[0]) == 299 && be32(&cap[4]) == 2048);

    auto blk = espCommand(esp, 3, {0x28, 0, 0, 0, 0, 7, 0, 0, 2, 0}, 4096, st);   // 2 blocks at 7
    CHECK(st == 0 && memcmp(blk.data(), &cd.data[7 * 2048], 4096) == 0);

    auto toc = espCommand(esp, 3, {0x43, 0, 0, 0, 0, 0, 0, 0, 20, 0}, 20, st);
    CHECK(toc[2] == 1 && toc[3] == 1 && toc[6] == 1 && toc[14] == 0xAA && be32(&toc[16]) == 300);

    espCommand(esp, 3, {0x2A, 0, 0, 0, 0, 1, 0, 0, 1, 0}, 0, st);
    CHECK(st == 2);                                     // read-only

    // MODE SELECT to 512-byte blocks, as some drivers do.
    esp.write(4, 3);
    for (u8 b : std::vector<u8>{0x15, 0, 0, 0, 12, 0}) esp.write(2, b);
    esp.write(3, 0x41); esp.read(5);
    const u8 params[12] = {0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 2, 0};
    esp.write(0, 12); esp.write(1, 0); esp.write(14, 0); esp.write(3, 0x90);
    for (u8 b : params) esp.dmaWrite(b);
    esp.read(5); esp.write(3, 0x11); esp.read(2); esp.read(2); esp.read(5); esp.write(3, 0x12); esp.read(5);
    cap = espCommand(esp, 3, {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 8, st);
    CHECK(be32(&cap[0]) == 1199 && be32(&cap[4]) == 512);
    blk = espCommand(esp, 3, {0x28, 0, 0, 0, 0, 5, 0, 0, 1, 0}, 512, st);
    CHECK(memcmp(blk.data(), &cd.data[5 * 512], 512) == 0);

    // Locked: no eject.  Unlocked: eject, then "no medium".
    espCommand(esp, 3, {0x1E, 0, 0, 0, 1, 0}, 0, st);
    espCommand(esp, 3, {0x1B, 0, 0, 0, 2, 0}, 0, st);
    CHECK(st == 2 && esp.device(3).present);
    espCommand(esp, 3, {0x1E, 0, 0, 0, 0, 0}, 0, st);
    espCommand(esp, 3, {0x1B, 0, 0, 0, 2, 0}, 0, st);
    CHECK(st == 0 && !esp.device(3).present && esp.device(3).ejected);
    espCommand(esp, 3, {0x00, 0, 0, 0, 0, 0}, 0, st);
    sense = espCommand(esp, 3, {0x03, 0, 0, 0, 18, 0}, 18, st);
    CHECK(sense[2] == 2 && sense[12] == 0x3A);
}

static void testMachine()
{
    Rig r;
    CHECK(r.m.cpu.pc == 0x400010 && r.m.cpu.a[7] == 0x2000);
    for (int i = 0; i < 10; i++) r.m.runFrame();
    CHECK(r.ram[0x100] == 0x12 && r.ram[0x101] == 0x34);
    const u8 *scr = r.m.screen();
    CHECK(scr == r.ram.data() + 0x3FA700);
    bool painted = true;
    for (int i = 0; i < Machine::kScreenBytes; i++) if (scr[i] != 0xAA) painted = false;
    CHECK(painted);
    int vbls = (r.ram[0x102] << 8) | r.ram[0x103];
    CHECK(vbls >= 8 && vbls <= 10);
    CHECK(r.m.cpu.totalCycles >= 10ull * 370 * 352 && r.m.cpu.totalCycles < 10ull * 370 * 352 + 200);

    // Invalid configurations are refused.
    Machine m2;
    CHECK(!m2.init(r.rom.data(), 0x12345, r.ram.data(), 0x400000));
    CHECK(!m2.init(r.rom.data(), 0x20000, r.ram.data(), 0x300000));
    CHECK(m2.init(r.rom.data(), 0x20000, r.ram.data(), 0x80000));
}

// Bit-bang the RTC through VIA port B like the ROM's clock routines.
struct RtcClient {
    Machine &m;
    explicit RtcClient(Machine &mm) : m(mm) {}
    void port(u8 v) { m.write8(Rig::vBufB, v); }
    void sendByte(u8 b) {
        m.write8(Rig::vDirB, 0x87);                     // data line is an output
        for (int i = 7; i >= 0; i--) {
            u8 bit = (b >> i) & 1;
            port(bit);                                  // clock low, data
            port((u8)(bit | 2));                        // clock high
        }
    }
    u8 recvByte() {
        m.write8(Rig::vDirB, 0x86);                     // data line is an input
        u8 v = 0;
        for (int i = 0; i < 8; i++) {
            port(0);
            port(2);
            v = (u8)((v << 1) | (m.read8(Rig::vBufB) & 1));
        }
        return v;
    }
    void begin() { m.write8(Rig::vDirB, 0x87); port(0x06); port(0x02); port(0x00); }
    void end() { port(0x04); }
    u8 read(u8 cmd) { begin(); sendByte(cmd); u8 v = recvByte(); end(); return v; }
    void write(u8 cmd, u8 v) { begin(); sendByte(cmd); sendByte(v); end(); }
};

static void testRtc()
{
    Rig r;
    r.m.runFrame();
    r.m.setClock(0xA1B2C3D4);
    RtcClient c(r.m);
    CHECK(c.read(0x81) == 0xD4);
    CHECK(c.read(0x85) == 0xC3);
    CHECK(c.read(0x89) == 0xB2);
    CHECK(c.read(0x8D) == 0xA1);
    c.write(0x35, 0x00);                                // write enable
    c.write(0x01, 0x11);                                // seconds low byte
    CHECK(r.m.clock() == 0xA1B2C311);
    c.write(0x41 | (3 << 2), 0x5A);                     // PRAM[3]
    CHECK(r.m.rtc.pram[3] == 0x5A && r.m.rtc.pramDirty);
    CHECK(c.read(0xC1 | (3 << 2)) == 0x5A);
    c.write(0x21 | (2 << 2), 0x77);                     // PRAM[0x12]
    CHECK(r.m.rtc.pram[0x12] == 0x77);
    CHECK(c.read(0xA1 | (2 << 2)) == 0x77);
    // Extended PRAM, address 0xB6.
    c.begin(); c.sendByte((u8)(0x38 | (0xB6 >> 5))); c.sendByte((u8)((0xB6 & 0x1F) << 2)); c.sendByte(0x99); c.end();
    CHECK(r.m.rtc.pram[0xB6] == 0x99);
    c.begin(); c.sendByte((u8)(0xB8 | (0xB6 >> 5))); c.sendByte((u8)((0xB6 & 0x1F) << 2)); u8 v = c.recvByte(); c.end();
    CHECK(v == 0x99);
    c.write(0x35, 0x80);                                // write protect
    c.write(0x41 | (3 << 2), 0x00);
    CHECK(r.m.rtc.pram[3] == 0x5A);
    // The clock ticks once per 60 frames.
    u32 before = r.m.clock();
    for (int i = 0; i < 120; i++) r.m.runFrame();
    CHECK(r.m.clock() == before + 2);
}

static int kbdCommand(Rig &r, u8 cmd, int maxFrames = 40)
{
    r.m.write8(Rig::vACR, 0x1C);                        // shift out under external clock
    r.m.write8(Rig::vSR, cmd);
    for (int i = 0; i < maxFrames && !(r.m.read8(Rig::vIFR) & 0x04); i++) r.m.runFrame();
    if (!(r.m.read8(Rig::vIFR) & 0x04)) return -1;
    r.m.write8(Rig::vACR, 0x0C);                        // shift in
    r.m.read8(Rig::vSR);                                // clears the flag
    for (int i = 0; i < maxFrames && !(r.m.read8(Rig::vIFR) & 0x04); i++) r.m.runFrame();
    if (!(r.m.read8(Rig::vIFR) & 0x04)) return -1;
    return r.m.read8(Rig::vSR);
}

static void testKeyboard()
{
    Rig r;
    r.m.runFrame();
    CHECK(kbdCommand(r, 0x16) == 0x0B);                 // model
    CHECK(kbdCommand(r, 0x36) == 0x7D);                 // self test
    CHECK(kbdCommand(r, 0x10) == 0x7B);                 // inquiry times out with "null"
    r.m.keyEvent(MK_A, true);
    r.m.keyEvent(MK_A, false);
    r.m.keyEvent(MK_Return, true);
    r.m.keyEvent(MK_Left, true);
    CHECK(kbdCommand(r, 0x10) == 0x01);
    CHECK(kbdCommand(r, 0x10) == 0x81);
    CHECK(kbdCommand(r, 0x10) == 0x49);
    CHECK(kbdCommand(r, 0x10) == 0x79);                 // keypad prefix
    CHECK(kbdCommand(r, 0x14) == 0x0D);                 // instant: left arrow
    // A key pressed while an inquiry is pending answers it right away.
    r.m.write8(Rig::vACR, 0x1C);
    r.m.write8(Rig::vSR, 0x10);
    r.m.runFrame();
    r.m.write8(Rig::vACR, 0x0C);
    r.m.read8(Rig::vSR);
    r.m.runFrame();
    CHECK(!(r.m.read8(Rig::vIFR) & 0x04));
    r.m.keyEvent(MK_Space, true);
    r.m.runFrame();
    CHECK((r.m.read8(Rig::vIFR) & 0x04) && r.m.read8(Rig::vSR) == 0x63);
}

static void testMouseAndSound()
{
    Rig r;
    for (int i = 0; i < 3; i++) r.m.runFrame();
    CHECK(r.m.read8(Rig::vBufB) & 0x08);
    r.m.mouseButton(true);
    CHECK(!(r.m.read8(Rig::vBufB) & 0x08));
    // Mouse deltas are held back until the OS enables SCC interrupts.
    r.ram[0x828] = 0; r.ram[0x829] = 100; r.ram[0x82A] = 0; r.ram[0x82B] = 200; r.ram[0x8CF] = 0xFF; r.ram[0x8CE] = 0;
    r.m.mouseMove(5, -3);
    r.m.runFrame();
    CHECK(r.ram[0x829] == 100 && r.ram[0x8CE] == 0);
    r.m.write8(0xBFFFF9, 9); r.m.write8(0xBFFFF9, 0x08);   // WR9: master interrupt enable
    r.m.runFrame();
    CHECK(r.ram[0x829] == 97 && r.ram[0x82B] == 205 && r.ram[0x8CE] == 0xFF);

    // Sound: buffer bytes at full volume, PB7 low = enabled.
    for (int i = 0; i < 370; i++) r.ram[0x3FFD00 + 2 * i] = (u8)(i & 1 ? 0xFF : 0x00);
    r.m.write8(0xEFE3FE, 0x6F);                         // volume 7, main buffers
    r.m.write8(Rig::vDirB, 0x80); r.m.write8(Rig::vBufB, 0x00);
    r.m.runFrame();
    CHECK(r.m.soundBuffer()[0] < -20000 && r.m.soundBuffer()[1] > 20000);
    r.m.write8(Rig::vBufB, 0x80);                       // sound off
    r.m.runFrame();
    CHECK(r.m.soundBuffer()[0] == 0 && r.m.soundBuffer()[1] == 0);
}

int main()
{
    testCdrom();
    testEspUnalignedWrite();
    testGcr();
    testFloppy();
    testScsi();
    testMachine();
    testRtc();
    testKeyboard();
    testMouseAndSound();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
