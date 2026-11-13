// HFS reading and floppy building (src/frontend/hfs.cpp).
//   build/hfstest [IMAGE]   IMAGE: an HFS disk image to walk completely
//   HFSTEST_FLOPPY=out.dsk  also write the test floppy, to boot in the emulator
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../src/frontend/hfs.h"
#include "../src/frontend/util.h"

using namespace macemu;

static int failures = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Mem : Storage {
    std::vector<u8> d;
    u32 size() override { return (u32)d.size(); }
    bool read(u32 o, void *b, u32 n) override { if (o + n > d.size()) return false; memcpy(b, &d[o], n); return true; }
    bool write(u32 o, const void *b, u32 n) override { if (o + n > d.size()) return false; memcpy(&d[o], b, n); return true; }
    bool readOnly() override { return false; }
};

static unsigned files, folders;
static unsigned long long bytes;
static void walk(HfsVolume &v, u32 folder, int depth)
{
    static HfsEntry list[4][512];
    HfsEntry *e = list[depth < 3 ? depth : 3];
    int n = v.list(folder, e, 512);
    for (int i = 0; i < n; i++) {
        if (e[i].folder) { folders++; if (depth < 8) walk(v, e[i].id, depth + 1); continue; }
        files++;
        std::vector<u8> buf(e[i].dataLen + e[i].rsrcLen + 1);
        bool ok = v.readFork(e[i], false, 0, buf.data(), e[i].dataLen) &&
                  v.readFork(e[i], true, 0, buf.data() + e[i].dataLen, e[i].rsrcLen);
        if (!ok) printf("  cannot read %s\n", e[i].name);
        CHECK(ok);
        bytes += e[i].dataLen + e[i].rsrcLen;
    }
}

static void testImage(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("(no image %s: skipped)\n", path); return; }
    Mem m;
    fseek(f, 0, SEEK_END); m.d.resize((size_t)ftell(f)); fseek(f, 0, SEEK_SET);
    if (fread(m.d.data(), 1, m.d.size(), f) != m.d.size()) {}
    fclose(f);
    HfsVolume v;
    CHECK(v.open(&m));
    files = folders = 0; bytes = 0;
    walk(v, 2, 0);
    // The volume header counts files and folders (not the root).
    u32 base = 0;
    for (u32 i = 1; i < 64 && m.d[i * 512] == 'P'; i++) {
        if (!memcmp(&m.d[i * 512 + 48], "Apple_HFS", 9)) { base = be32(&m.d[i * 512 + 8]) * 512; break; }
    }
    u32 nFiles = be32(&m.d[base + 1024 + 84]), nDirs = be32(&m.d[base + 1024 + 88]);
    printf("%s: \"%s\", %u folders, %u files, %llu bytes (header: %u, %u)\n", path, v.name(), folders, files, bytes, nDirs, nFiles);
    CHECK(files == nFiles && folders == nDirs && files > 50);
}

static void testFloppy()
{
    // A MacBinary file with both forks, plain files, odd and clashing names.
    std::vector<u8> data(3000), rsrc(1234), text(70000);
    for (size_t i = 0; i < data.size(); i++) data[i] = (u8)(i * 13);
    for (size_t i = 0; i < rsrc.size(); i++) rsrc[i] = (u8)(i * 7 + 1);
    for (size_t i = 0; i < text.size(); i++) text[i] = (u8)("Hello, Mac!\r"[i % 12]);
    HfsEntry fake = {};
    strCopy(fake.name, "Test App", sizeof fake.name);
    fake.type = 0x4150504C; fake.creator = 0x54455354;  // APPL TEST
    fake.dataLen = (u32)data.size(); fake.rsrcLen = (u32)rsrc.size();
    std::vector<u8> mb(128 + macBinaryPad((u32)data.size()) + rsrc.size());
    macBinaryHeader(fake, mb.data());
    memcpy(&mb[128], data.data(), data.size());
    memcpy(&mb[128 + macBinaryPad((u32)data.size())], rsrc.data(), rsrc.size());

    std::vector<HfsNewFile> list;
    HfsNewFile app;
    CHECK(macBinaryParse(mb.data(), (u32)mb.size(), app));
    CHECK(app.type == 0x4150504C && app.dataLen == 3000 && app.rsrcLen == 1234 && !strcmp(app.name, "Test App"));
    list.push_back(app);
    const char *names[] = {"read me.txt", "Read Me.txt", "zeta", "Alpha", "b%c*d?.txt", "", "MIDDLE"};
    for (const char *n : names) {
        HfsNewFile t = {};
        strCopy(t.name, n, sizeof t.name);
        t.type = 0x54455854; t.creator = 0x74747874;     // TEXT ttxt
        t.data = text.data(); t.dataLen = 9000;
        list.push_back(t);
    }
    for (int i = 0; i < 30; i++) {                      // enough for several catalog nodes
        HfsNewFile t = {};
        snprintf(t.name, sizeof t.name, "Note %02d", 29 - i);
        t.type = 0x54455854; t.creator = 0x74747874;
        t.data = text.data(); t.dataLen = (u32)(100 + i);
        list.push_back(t);
    }

    Mem m;
    m.d.resize(kHfsFloppyBytes);
    CHECK(hfsMakeFloppy(m.d.data(), "Transfer", list.data(), (int)list.size(), 0xE0000000u));
    if (const char *out = getenv("HFSTEST_FLOPPY")) {
        FILE *f = fopen(out, "wb");
        fwrite(m.d.data(), 1, m.d.size(), f);
        fclose(f);
    }

    HfsVolume v;
    CHECK(v.open(&m));
    CHECK(!strcmp(v.name(), "Transfer"));
    HfsEntry e[64];
    int n = v.list(2, e, 64);
    CHECK(n == (int)list.size());
    bool sorted = true, unique = true, found = false;
    for (int i = 1; i < n; i++) {
        sorted &= hfsCompareNames(e[i - 1].name, e[i].name) < 0;
        unique &= hfsCompareNames(e[i - 1].name, e[i].name) != 0;
    }
    CHECK(sorted && unique);
    for (int i = 0; i < n; i++) {
        if (strcmp(e[i].name, "Test App")) continue;
        found = true;
        std::vector<u8> d(3000), r(1234);
        CHECK(v.readFork(e[i], false, 0, d.data(), 3000) && d == data);
        CHECK(v.readFork(e[i], true, 0, r.data(), 1234) && r == rsrc);
        CHECK(e[i].type == 0x4150504C && e[i].creator == 0x54455354);
        u8 hdr[128];
        macBinaryHeader(e[i], hdr);                     // and back to MacBinary
        HfsNewFile again;
        std::vector<u8> mb2(hdr, hdr + 128);
        mb2.insert(mb2.end(), d.begin(), d.end());
        mb2.resize(128 + macBinaryPad(3000));
        mb2.insert(mb2.end(), r.begin(), r.end());
        CHECK(macBinaryParse(mb2.data(), (u32)mb2.size(), again) && again.rsrcLen == 1234);
    }
    CHECK(found);
    for (int i = 0; i < n; i++) {
        bool clean = true;
        for (const char *p = e[i].name; *p; p++) clean &= *p != '%' && *p != '*' && *p != '?';
        CHECK(clean && e[i].name[0]);
    }

    // Too much for 800K: refused, not truncated.
    std::vector<u8> big(900000);
    HfsNewFile huge = {};
    strCopy(huge.name, "Huge", sizeof huge.name);
    huge.data = big.data(); huge.dataLen = (u32)big.size();
    CHECK(!hfsMakeFloppy(m.d.data(), "X", &huge, 1, 0));
}

// A big volume built in pieces (the web page's transfer CD).
static void testBigVolume()
{
    std::vector<std::vector<u8>> forks;
    std::vector<HfsNewFile> list;
    const u32 sizes[][2] = {{40u << 20, 300000}, {0, 5000}, {12345, 0}, {3u << 20, 1u << 20}};
    for (int i = 0; i < 4; i++) {
        HfsNewFile f = {};
        snprintf(f.name, sizeof f.name, "Big file %d", 4 - i);
        f.type = 0x4150504C; f.creator = 0x54455354 + (u32)i; f.flags = 0x2000;
        f.dataLen = sizes[i][0]; f.rsrcLen = sizes[i][1];
        list.push_back(f);
        for (int k = 0; k < 2; k++) {
            std::vector<u8> b(sizes[i][k]);
            for (size_t j = 0; j < b.size(); j++) b[j] = (u8)(j * 7 + i * 13 + k * 101 + (j >> 12));
            forks.push_back(b);
        }
    }
    std::vector<u8> meta(256 * 1024);
    std::vector<u32> pos(2 * list.size());
    u32 metaBytes = 0, volBytes = 0;
    CHECK(hfsPlanVolume(meta.data(), (u32)meta.size(), "Transfer 7", list.data(), (int)list.size(), 0xE0000000u,
                        pos.data(), metaBytes, volBytes));
    CHECK(volBytes % 2048 == 0 && metaBytes <= meta.size());
    CHECK(volBytes >= (44u << 20) && volBytes < (46u << 20));          // just big enough
    Mem m;
    m.d.assign(volBytes, 0);
    memcpy(m.d.data(), meta.data(), metaBytes);
    for (size_t i = 0; i < forks.size(); i++) {
        CHECK(pos[i] >= metaBytes && pos[i] + forks[i].size() <= volBytes - 1024);
        if (!forks[i].empty()) memcpy(&m.d[pos[i]], forks[i].data(), forks[i].size());
    }
    memcpy(&m.d[volBytes - 1024], &meta[1024], 512);

    HfsVolume v;
    CHECK(v.open(&m));
    CHECK(!strcmp(v.name(), "Transfer 7"));
    HfsEntry e[8];
    int n = v.list(2, e, 8);
    CHECK(n == 4);
    for (int i = 0; i < n; i++) {
        int k = 4 - (e[i].name[9] - '0');               // "Big file 4" is list[0]
        CHECK(e[i].dataLen == sizes[k][0] && e[i].rsrcLen == sizes[k][1] && e[i].creator == 0x54455354 + (u32)k);
        std::vector<u8> d(sizes[k][0]), r(sizes[k][1]);
        CHECK(v.readFork(e[i], false, 0, d.data(), (u32)d.size()) && d == forks[2 * k]);
        CHECK(v.readFork(e[i], true, 0, r.data(), (u32)r.size()) && r == forks[2 * k + 1]);
    }
    if (const char *out = getenv("HFSTEST_BIG")) {
        FILE *f = fopen(out, "wb");
        fwrite(m.d.data(), 1, m.d.size(), f);
        fclose(f);
    }
}

int main(int argc, char **argv)
{
    testFloppy();
    testBigVolume();
    testImage(argc > 1 ? argv[1] : "build/hfstest-image.hda");
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
