// The shared folder through the real Mac OS File Manager (SHARE_TEST=1 with
// --share DIR): once Mac OS is up, File Manager calls on the volume, and
// what they did checked on both sides.  DIR starts with a file "ReadMe.txt"
// and an empty folder "Folder" in it (Mac OS 7.5 or later on the disk).
#ifndef MACEMU_SHARETEST_H
#define MACEMU_SHARETEST_H

#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <ctime>
#include <sys/stat.h>
#include "../core/sharedfs.h"

namespace macemu {

struct ShareTest {
    SharedFolder &sf;
    M68K &cpu;
    std::string dir;
    int frame = 0, done = 0, checks = 0, failures = 0;
    u32 mem = 0;                                        // 16 KB in the Mac: pb, names, buffers

    ShareTest(SharedFolder &s, M68K &c, const std::string &d) : sf(s), cpu(c), dir(d) {}

    enum { PB = 0, NAME = 0x100, NAME2 = 0x200, SPEC = 0x300, BUF = 0x1000 };

    void check(bool ok, const char *what) {
        checks++;
        if (!ok) { failures++; printf("SHARE FAIL: %s\n", what); }
    }
    u32 r32(u32 a) { return cpu.rd(a, 2); }
    u16 r16(u32 a) { return (u16)cpu.rd(a, 1); }
    u8 r8(u32 a) { return (u8)cpu.rd(a, 0); }
    void w32(u32 a, u32 v) { cpu.wr(a, 2, v); }
    void w16(u32 a, u16 v) { cpu.wr(a, 1, v); }
    void w8(u32 a, u8 v) { cpu.wr(a, 0, v); }
    void pstr(u32 a, const char *s) { u32 n = (u32)strlen(s); w8(a, (u8)n); for (u32 i = 0; i < n; i++) w8(a + 1 + i, (u8)s[i]); }
    std::string getp(u32 a) { std::string s; for (u32 i = 0; i < r8(a); i++) s += (char)r8(a + 1 + i); return s; }

    void pb(const char *name, u32 dirID = 2) {
        for (u32 i = 0; i < 128; i++) w8(mem + PB + i, 0);
        if (name) { pstr(mem + NAME, name); w32(mem + PB + 18, mem + NAME); }
        w16(mem + PB + 22, (u16)sf.volumeRefNum());
        w32(mem + PB + 48, dirID);
    }
    s16 call(u16 trap, u32 selector = 0) {
        SharedFolder::Regs r = {};
        r.a[0] = mem + PB;
        r.d[0] = selector;
        sf.runTrap(trap, r);
        return (s16)(r.d[0] & 0xFFFF);
    }
    std::string hostFile(const std::string &p) {
        std::string s;
        FILE *f = fopen((dir + "/" + p).c_str(), "rb");
        if (!f) return "<missing>";
        int c;
        while ((c = fgetc(f)) != EOF) s += (char)c;
        fclose(f);
        return s;
    }

    static void idle(void *ctx) { ((ShareTest *)ctx)->tick(); }
    void tick() {
        if (done || ++frame < 30) return;               // let the Finder settle first
        done = 1;
        SharedFolder::Regs r = {};
        r.d[0] = 0x4000;
        sf.runTrap(0xA71E, r);                          // NewPtrSysClear
        mem = r.a[0];
        if (!mem) { check(false, "memory for the test"); return; }
        run();
        printf("shared folder test: %d checks, %d failures\n", checks, failures);
    }

    void run() {
        // The volume
        pb(0);
        w32(mem + PB + 18, mem + NAME);
        check(call(0xA207) == 0 && getp(mem + NAME) == "Shared", "HGetVInfo names the volume");
        {
            // Files changing on the host move the modification date on, never the creation date.
            u32 created = r32(mem + PB + 30), modified = r32(mem + PB + 34);
            FILE *f = fopen((dir + "/new on host.txt").c_str(), "w");
            if (f) { fputs("x", f); fclose(f); }
            struct timespec later[2] = {{time(0) + 3600, 0}, {time(0) + 3600, 0}};
            utimensat(AT_FDCWD, (dir + "/new on host.txt").c_str(), later, 0);
            sf.forgetListings();
            pb(0);
            w32(mem + PB + 18, mem + NAME);
            call(0xA207);
            (void) modified;                            // (moves on at the next poll, for the Finder)
            check(r32(mem + PB + 30) == created, "the volume's creation date stays");
            remove((dir + "/new on host.txt").c_str());
            sf.forgetListings();
        }

        // A file: create, type, data
        pb("Test File");
        check(call(0xA208) == 0, "HCreate");
        pb("Test File");
        check(call(0xA208) == -48, "HCreate again: dupFNErr");
        pb("Test File");
        check(call(0xA20C) == 0, "HGetFInfo");
        w32(mem + PB + 32, 0x54455854); w32(mem + PB + 36, 0x74747874); w16(mem + PB + 40, 0x0100);
        w32(mem + PB + 48, 2);                          // (HGetFInfo gave the file's ID in ioDirID)
        { s16 e = call(0xA20D); if (e) printf("  HSetFInfo -> %d\n", e); check(e == 0, "HSetFInfo"); }
        pb("Test File");
        check(call(0xA20C) == 0 && r32(mem + PB + 32) == 0x54455854 && r32(mem + PB + 36) == 0x74747874, "type and creator kept");

        pb("Test File");
        w8(mem + PB + 27, 3);                           // fsRdWrPerm
        check(call(0xA200) == 0, "HOpen");
        s16 ref = (s16)r16(mem + PB + 24);
        for (u32 i = 0; i < 5000; i++) w8(mem + BUF + i, (u8)(i * 7 + 3));
        w16(mem + PB + 24, (u16)ref);
        w32(mem + PB + 32, mem + BUF); w32(mem + PB + 36, 5000); w16(mem + PB + 44, 1); w32(mem + PB + 46, 0);
        check(call(0xA003) == 0 && r32(mem + PB + 40) == 5000, "Write 5000 bytes");
        check(call(0xA011) == 0 && r32(mem + PB + 28) == 5000, "GetEOF");
        for (u32 i = 0; i < 100; i++) w8(mem + BUF + 0x2000 + i, 0);
        w32(mem + PB + 32, mem + BUF + 0x2000); w32(mem + PB + 36, 50); w16(mem + PB + 44, 1); w32(mem + PB + 46, 100);
        bool same = call(0xA002) == 0 && r32(mem + PB + 40) == 50;
        for (u32 i = 0; i < 50 && same; i++) same = r8(mem + BUF + 0x2000 + i) == (u8)((100 + i) * 7 + 3);
        check(same, "Read 50 bytes from 100");
        w32(mem + PB + 36, 100); w16(mem + PB + 44, 1); w32(mem + PB + 46, 4950);
        check(call(0xA002) == -39 && r32(mem + PB + 40) == 50, "Read past the end: eofErr, 50 bytes");
        check(call(0xA001) == 0, "Close");

        // The resource fork
        pb("Test File");
        w8(mem + PB + 27, 3);
        check(call(0xA20A) == 0, "HOpenRF");
        ref = (s16)r16(mem + PB + 24);
        for (u32 i = 0; i < 700; i++) w8(mem + BUF + i, (u8)(255 - i));
        w32(mem + PB + 32, mem + BUF); w32(mem + PB + 36, 700); w16(mem + PB + 44, 1); w32(mem + PB + 46, 0);
        check(call(0xA003) == 0, "Write the resource fork");
        check(call(0xA001) == 0, "Close it");

        pb("Test File");
        check(call(0xA260, 9) == 0 && r32(mem + PB + 54) == 5000 && r32(mem + PB + 64) == 700
              && r32(mem + PB + 32) == 0x54455854 && !(r8(mem + PB + 30) & 0x10), "GetCatInfo: sizes, type, a file");
        u32 fileID = r32(mem + PB + 48);

        // ... and on the host
        std::string data = hostFile("Test File");
        bool okData = data.size() == 5000;
        for (u32 i = 0; i < 5000 && okData; i++) okData = (u8)data[i] == (u8)(i * 7 + 3);
        check(okData, "the data fork is the host file");
        std::string ad = hostFile("._Test File");
        check(ad.size() == 82 + 700 && ad.substr(50, 8) == "TEXTttxt", "AppleDouble: Finder info");
        bool okRsrc = ad.size() == 782;
        for (u32 i = 0; i < 700 && okRsrc; i++) okRsrc = (u8)ad[82 + i] == (u8)(255 - i);
        check(okRsrc, "AppleDouble: the resource fork");

        // A folder; the file moved into it; renamed (with Mac Roman)
        pb("Sub");
        check(call(0xA260, 6) == 0, "DirCreate");
        u32 subID = r32(mem + PB + 48);
        check(subID >= 16 && access((dir + "/Sub").c_str(), F_OK) == 0, "the folder on the host");
        pb("Test File");
        w32(mem + PB + 28, 0); w32(mem + PB + 36, subID);   // ioNewName none, ioNewDirID
        check(call(0xA260, 5) == 0, "CatMove into the folder");
        check(hostFile("Sub/Test File").size() == 5000 && hostFile("Sub/._Test File").size() == 782, "moved on the host, with its AppleDouble");
        pb(0, subID);
        w16(mem + PB + 28, 1);
        w32(mem + PB + 18, mem + NAME);
        check(call(0xA260, 9) == 0 && getp(mem + NAME) == "Test File" && r32(mem + PB + 48) == fileID, "listed in the folder, same ID");

        pb("Test File", subID);
        pstr(mem + NAME2, "Caf\x8E \xC4");              // "Café ƒ" in Mac Roman
        w32(mem + PB + 28, mem + NAME2);
        check(call(0xA20B) == 0, "HRename (Mac Roman)");
        check(hostFile("Sub/Caf\xC3\xA9 \xC6\x92").size() == 5000, "renamed on the host, in UTF-8");
        pb("Caf\x8E \xC4", subID);
        w8(mem + PB + 27, 1);
        check(call(0xA200) == 0, "HOpen by the new name");
        ref = (s16)r16(mem + PB + 24);
        w32(mem + PB + 32, mem + BUF + 0x2000); w32(mem + PB + 36, 10); w16(mem + PB + 44, 1); w32(mem + PB + 46, 0);
        check(call(0xA002) == 0 && r8(mem + BUF + 0x2000 + 9) == (u8)(9 * 7 + 3), "Read it");
        check(call(0xA001) == 0, "Close");

        // Names the card cannot take
        pb("A/B?");
        check(call(0xA208) == 0 && access((dir + "/A%2FB%3F").c_str(), F_OK) == 0, "'/' and '?' escaped on the host");
        pb("A/B?");
        check(call(0xA20C) == 0, "found again by its Mac name");
        pb("A/B?");
        check(call(0xA209) == 0, "HDelete it");

        // Made on the host
        FILE *f = fopen((dir + "/fromhost.txt").c_str(), "wb");
        fputs("Made on the host\n", f);
        fclose(f);
        sf.forgetListings();
        pb("fromhost.txt");
        check(call(0xA20C) == 0 && r32(mem + PB + 32) == 0x54455854, "a host file: found, TEXT from its name");
        pb("fromhost.txt");
        w8(mem + PB + 27, 1);
        { s16 e = call(0xA200); if (e) printf("  HOpen fromhost -> %d\n", e); check(e == 0, "HOpen it"); }
        w32(mem + PB + 32, mem + BUF); w32(mem + PB + 36, 100); w16(mem + PB + 44, 1); w32(mem + PB + 46, 0);
        check(call(0xA002) == -39 && r32(mem + PB + 40) == 17 && r8(mem + BUF) == 'M', "Read it (17 bytes, then eofErr)");
        call(0xA001);

        // Every item of the root, by index
        std::string all;
        for (int i = 1; i < 50; i++) {
            pb(0);
            w16(mem + PB + 28, (u16)i);
            w32(mem + PB + 18, mem + NAME);
            if (call(0xA260, 9) != 0) break;
            all += getp(mem + NAME) + "|";
        }
        check(all.find("ReadMe.txt|") != std::string::npos && all.find("Folder|") != std::string::npos
              && all.find("Sub|") != std::string::npos && all.find("fromhost.txt|") != std::string::npos
              && all.find("._") == std::string::npos, "listing the root by index (no AppleDouble files)");

        // FSSpecs
        pb("ReadMe.txt");
        w32(mem + PB + 28, mem + SPEC);
        check(call(0xA260, 0x1B) == 0 && r32(mem + SPEC + 2) == 2 && getp(mem + SPEC + 6) == "ReadMe.txt", "MakeFSSpec");
        pb("Not There");
        w32(mem + PB + 28, mem + SPEC);
        check(call(0xA260, 0x1B) == -43 && getp(mem + SPEC + 6) == "Not There", "MakeFSSpec for a new file: fnfErr");

        // Deleting
        pb(0, 2);
        pstr(mem + NAME, "Sub");
        w32(mem + PB + 18, mem + NAME);
        check(call(0xA209) == -47, "HDelete a folder with something in it: fBsyErr");
        pb("Caf\x8E \xC4", subID);
        check(call(0xA209) == 0 && access((dir + "/Sub/._Caf\xC3\xA9 \xC6\x92").c_str(), F_OK) != 0, "HDelete the file and its AppleDouble");
        pb("Sub");
        check(call(0xA209) == 0 && access((dir + "/Sub").c_str(), F_OK) != 0, "HDelete the empty folder");
        pb("fromhost.txt");
        check(call(0xA209) == 0, "HDelete the host's file");
    }
};

} // namespace macemu

#endif
