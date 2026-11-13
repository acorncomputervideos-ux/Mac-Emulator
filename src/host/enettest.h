// The Mac's built-in Ethernet through Mac OS's own drivers (ENET_TEST=1 with
// --share DIR, which provides the moment and the means to call Mac OS;
// ENET_AT=frame waits until then, e.g. after a restart):
//  - finds the Ethernet hardware with the Slot Manager and opens its ".ENET"
//    driver with OpenSlot, as MacTCP and AppleTalk do;
//  - sends an ARP request for the emulator's router (10.0.2.2) with EWrite;
//  - opens MacTCP (".IPP") and asks for its address every second or so: set
//    to Ethernet and "Server", it gets 10.0.2.15 from the router.
// Checked with Mac OS 7.5.3 and MacTCP 2.0.6.
#ifndef MACEMU_ENETTEST_H
#define MACEMU_ENETTEST_H

#include <cstdio>
#include <string>
#include "../core/sharedfs.h"

namespace macemu {

class EnetTest {
public:
    EnetTest(SharedFolder &s, M68K &c) : sf(s), cpu(c) {}
    SharedFolder &sf;
    M68K &cpu;
    int frame = 0;
    bool done = false;
    bool armed = true;                  // (ENET_AT=frame: not before then)
    s16 ippRef = 0;
    u32 ippPb = 0;
    int asks = 0;


    static void idle(void *ctx) { ((EnetTest *)ctx)->tick(); }
    void tick() {
        if (done && ippRef && asks < 6 && ++frame % 60 == 0) { askAddress(); return; }
        if (done || !armed || ++frame < 30) return;
        done = true;
        SharedFolder::Regs r = {};
        r.d[0] = 0x1000;
        sf.runTrap(0xA71E, r);                          // NewPtrSysClear
        u32 mem = r.a[0];
        if (!mem) { puts("ENET: no memory"); return; }
        u32 pb = mem, name = mem + 0x100, frm = mem + 0x200, wds = mem + 0x300;
        static const char nm[] = "\x05.ENET";
        for (int i = 0; i < 6; i++) cpu.wr(name + i, 0, (u8)nm[i]);

        // Where is the Ethernet hardware?  The Slot Manager's sNextTypeSRsrc:
        // category catNetwork (4), type typeEtherNet (1), any driver.
        u32 sp = mem + 0x500;
        for (int i = 0; i < 64; i++) cpu.wr(sp + i, 0, 0);
        cpu.wr(sp + 40, 1, 4); cpu.wr(sp + 42, 1, 1); cpu.wr(sp + 48, 0, 3);
        int slot = -1, id = -1;
        for (int n = 0; n < 8; n++) {
            r = SharedFolder::Regs();
            r.d[0] = 0x15;                              // sNextTypeSRsrc
            r.a[0] = sp;
            sf.runTrap(0xA06E, r);                      // _SlotManager
            s16 e = (s16)(r.d[0] & 0xFFFF);
            if (e) { printf("ENET: sNextTypeSRsrc -> %d (no more)\n", e); break; }
            printf("ENET: Ethernet sResource: slot %X id %02X, drvrSW %04X drvrHW %04X\n", cpu.rd(sp + 49, 0),
                   cpu.rd(sp + 50, 0), cpu.rd(sp + 44, 1), cpu.rd(sp + 46, 1));
            if (slot < 0) { slot = (int)cpu.rd(sp + 49, 0); id = (int)cpu.rd(sp + 50, 0); }
        }
        // ... and the classic way to open its driver: OpenSlot (_Open with the slot).
        cpu.wr(pb + 18, 2, name);                       // ioNamePtr
        if (slot >= 0) { cpu.wr(pb + 34, 0, (u8)slot); cpu.wr(pb + 35, 0, (u8)id); }
        r = SharedFolder::Regs();
        r.a[0] = pb;
        sf.runTrap(slot >= 0 ? 0xA200 : 0xA000, r);    // OpenSlot / _Open
        s16 err = (s16)(r.d[0] & 0xFFFF);
        s16 ref = (s16)cpu.rd(pb + 24, 1);
        printf("ENET: open .ENET -> %d, refNum %d\n", err, ref);
        if (err) return;
        // ARP: who has 10.0.2.2? tell 10.0.2.15
        static const u8 arp[42] = {
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0x08, 0x06,
            0, 1, 8, 0, 6, 4, 0, 1, 0, 0, 0, 0, 0, 0, 10, 0, 2, 15,
            0, 0, 0, 0, 0, 0, 10, 0, 2, 2};
        for (int i = 0; i < 42; i++) cpu.wr(frm + i, 0, arp[i]);
        cpu.wr(wds, 1, 42); cpu.wr(wds + 2, 2, frm); cpu.wr(wds + 6, 1, 0);
        for (int i = 0; i < 0x40; i++) cpu.wr(pb + 0x40 + i, 0, 0);
        u32 cpb = pb + 0x40;
        cpu.wr(cpb + 24, 1, (u16)ref);                  // ioRefNum
        cpu.wr(cpb + 26, 1, 244);                       // csCode: EWrite
        cpu.wr(cpb + 30, 2, wds);                       // ePointer: the write-data structure
        r = SharedFolder::Regs();
        r.a[0] = cpb;
        sf.runTrap(0xA004, r);                          // _Control
        printf("ENET: EWrite -> %d\n", (s16)(r.d[0] & 0xFFFF));
        // Our Ethernet address, as the driver reports it (EGetInfo, csCode 252).
        for (int i = 0; i < 0x40; i++) cpu.wr(cpb + i, 0, 0);
        cpu.wr(cpb + 24, 1, (u16)ref); cpu.wr(cpb + 26, 1, 252);
        cpu.wr(cpb + 30, 2, mem + 0x400); cpu.wr(cpb + 34, 1, 18);
        r = SharedFolder::Regs();
        r.a[0] = cpb;
        sf.runTrap(0xA004, r);
        printf("ENET: EGetInfo -> %d, address %02X:%02X:%02X:%02X:%02X:%02X\n", (s16)(r.d[0] & 0xFFFF),
               cpu.rd(mem + 0x400, 0), cpu.rd(mem + 0x401, 0), cpu.rd(mem + 0x402, 0),
               cpu.rd(mem + 0x403, 0), cpu.rd(mem + 0x404, 0), cpu.rd(mem + 0x405, 0));

        // MacTCP: open its driver (it takes its address then, from the
        // router with "Server") and ask for the address (ipctlGetAddr, 15).
        static const char ipp[] = "\x04.IPP";
        for (int i = 0; i < 5; i++) cpu.wr(name + i, 0, (u8)ipp[i]);
        for (int i = 0; i < 0x40; i++) cpu.wr(pb + i, 0, 0);
        cpu.wr(pb + 18, 2, name);
        r = SharedFolder::Regs();
        r.a[0] = pb;
        sf.runTrap(0xA000, r);                          // _Open ".IPP"
        err = (s16)(r.d[0] & 0xFFFF);
        ref = (s16)cpu.rd(pb + 24, 1);
        printf("IPP: open .IPP -> %d, refNum %d\n", err, ref);
        if (err) return;
        ippRef = ref;
        ippPb = cpb;
        askAddress();
    }
    void askAddress() {
        u32 cpb = ippPb;
        asks++;
        for (int i = 0; i < 0x40; i++) cpu.wr(cpb + i, 0, 0);
        cpu.wr(cpb + 24, 1, (u16)ippRef); cpu.wr(cpb + 26, 1, 15);
        SharedFolder::Regs r = {};
        r.a[0] = cpb;
        sf.runTrap(0xA004, r);
        u32 ip = cpu.rd(cpb + 28, 2), mask = cpu.rd(cpb + 32, 2);
        printf("IPP: ipctlGetAddr -> %d, address %u.%u.%u.%u mask %08X\n", (s16)(r.d[0] & 0xFFFF),
               ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, mask);
    }
};

} // namespace macemu

#endif
