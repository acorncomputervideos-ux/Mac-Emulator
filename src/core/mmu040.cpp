#include "mmu040.h"

namespace macemu {

enum {
    D_RESIDENT = 2,                     // upper-level descriptors: UDT bit 1
    D_WP = 1u << 2, D_USED = 1u << 3, D_MODIFIED = 1u << 4, D_SUPER = 1u << 7, D_GLOBAL = 1u << 10,
    // MMUSR
    SR_RESIDENT = 1u << 0, SR_TRANSPARENT = 1u << 1, SR_WP = 1u << 2, SR_MODIFIED = 1u << 4,
    SR_SUPER = 1u << 7, SR_GLOBAL = 1u << 10, SR_BUSERR = 1u << 11,
};

Mmu040::Mmu040(Bus &b) : bus(b)
{
    reset();
}

void Mmu040::reset()
{
    tc = urp = srp = mmusr = 0;
    itt[0] = itt[1] = dtt[0] = dtt[1] = 0;
    flush();
}

void Mmu040::flush()
{
    generation++;
    for (int i = 0; i < kCacheSize; i++) cache[i].tag = 0;
}

// TTx: base (31-24), mask (23-16), E (15), S field (14-13), W (2).
bool Mmu040::transparent(u32 addr, bool supervisor, bool code, bool write, bool &wp) const
{
    const u32 *regs = code ? itt : dtt;
    (void)write;
    for (int i = 0; i < 2; i++) {
        u32 r = regs[i];
        if (!(r & 0x8000)) continue;
        u32 sfield = (r >> 13) & 3;
        if (sfield == 0 && supervisor) continue;        // user only
        if (sfield == 1 && !supervisor) continue;       // supervisor only
        u32 mask = (r >> 16) & 0xFF;
        if (((addr >> 24) & ~mask & 0xFF) != ((r >> 24) & ~mask & 0xFF)) continue;
        wp = (r & 0x4) != 0;
        return true;
    }
    return false;
}

bool Mmu040::walk(u32 la, bool supervisor, bool write, bool update,
                  u32 &frame, bool &wp, bool &superOnly, bool &modified, u32 &status)
{
    wp = superOnly = modified = false;
    status = 0;
    u32 root = (supervisor ? srp : urp) & 0xFFFFFE00;

    u32 rootAddr = root + ((la >> 25) & 0x7F) * 4;
    u32 rootDesc = rd32(rootAddr);
    if (!(rootDesc & D_RESIDENT)) return false;
    if (rootDesc & D_WP) wp = true;
    if (update && !(rootDesc & D_USED)) wr32(rootAddr, rootDesc | D_USED);

    u32 ptrAddr = (rootDesc & 0xFFFFFE00) + ((la >> 18) & 0x7F) * 4;
    u32 ptrDesc = rd32(ptrAddr);
    if (!(ptrDesc & D_RESIDENT)) return false;
    if (ptrDesc & D_WP) wp = true;
    if (update && !(ptrDesc & D_USED)) wr32(ptrAddr, ptrDesc | D_USED);

    bool big = pageShift() == 13;
    u32 pageTable = ptrDesc & (big ? 0xFFFFFF80 : 0xFFFFFF00);
    u32 index = big ? (la >> 13) & 0x1F : (la >> 12) & 0x3F;
    u32 pageAddr = pageTable + index * 4;
    u32 pageDesc = rd32(pageAddr);
    if ((pageDesc & 3) == 2) {                          // indirect descriptor
        pageAddr = pageDesc & 0xFFFFFFFC;
        pageDesc = rd32(pageAddr);
    }
    if ((pageDesc & 3) == 0 || (pageDesc & 3) == 2) return false;

    if (pageDesc & D_WP) wp = true;
    superOnly = (pageDesc & D_SUPER) != 0;
    bool allowedWrite = !wp && !(superOnly && !supervisor);
    if (update) {
        u32 nd = pageDesc | D_USED;
        if (write && allowedWrite) nd |= D_MODIFIED;
        if (nd != pageDesc) wr32(pageAddr, nd);
        pageDesc = nd;
    }
    modified = (pageDesc & D_MODIFIED) != 0;
    frame = pageDesc & (big ? 0xFFFFE000 : 0xFFFFF000);
    status = (pageDesc & (D_GLOBAL | D_SUPER)) | (modified ? SR_MODIFIED : 0) | (wp ? SR_WP : 0) | SR_RESIDENT;
    return true;
}

u32 Mmu040::translate(u32 la, bool write, bool supervisor, bool code, bool &fault)
{
    bool wp = false;
    if (transparent(la, supervisor, code, write, wp)) {
        if (write && wp) fault = true;
        return la;
    }
    u32 shift = pageShift();
    u32 page = la >> shift;
    u32 offset = la & ((1u << shift) - 1);
    u32 tag = (page << 3) | 1 | (supervisor ? 2 : 0) | (code ? 4 : 0);
    Entry &e = cache[(page ^ (page >> 8) ^ (supervisor ? 0x55 : 0) ^ (code ? 0xAA : 0)) & (kCacheSize - 1)];
    if (e.tag == tag && !(write && !e.modified)) {
        if ((write && e.writeProtect) || (e.supervisorOnly && !supervisor)) { fault = true; return la; }
        return e.frame | offset;
    }
    u32 frame, status;
    bool superOnly, modified;
    if (!walk(la, supervisor, write, true, frame, wp, superOnly, modified, status)) {
        fault = true;
        return la;
    }
    if ((write && wp) || (superOnly && !supervisor)) { fault = true; return la; }
    e.tag = tag;
    e.frame = frame;
    e.writeProtect = wp;
    e.supervisorOnly = superOnly;
    e.modified = modified;
    return frame | offset;
}

bool Mmu040::control(int reg, u32 &v, bool write)
{
    u32 *r;
    u32 mask = 0xFFFFFFFF;
    switch (reg) {
    case 0x003: r = &tc; mask = 0xC000; break;
    case 0x004: r = &itt[0]; mask = 0xFFFFE364; break;
    case 0x005: r = &itt[1]; mask = 0xFFFFE364; break;
    case 0x006: r = &dtt[0]; mask = 0xFFFFE364; break;
    case 0x007: r = &dtt[1]; mask = 0xFFFFE364; break;
    case 0x805: r = &mmusr; break;
    case 0x806: r = &urp; mask = 0xFFFFFE00; break;
    case 0x807: r = &srp; mask = 0xFFFFFE00; break;
    default: return false;
    }
    if (write) {
        // Only a change can alter translations.  (On a real 68040 these
        // writes do not flush the ATC at all; Mac OS writes the same TT
        // values again and again when it switches addressing modes.)
        if (*r != (v & mask)) {
            *r = v & mask;
            flush();                                    // also bumps the generation for the CPU's TLB
        }
    } else {
        v = *r;
    }
    return true;
}

// $F500-$F51F PFLUSH variants, $F548/$F568 PTESTW/PTESTR (An).
bool Mmu040::execute(M68K &cpu, u16 op)
{
    int reg = op & 7;
    if ((op & 0xFFE0) == 0xF500) {
        flush();                                        // every PFLUSH form: drop the whole cache
        return true;
    }
    if ((op & 0xFFD8) == 0xF548) {
        bool write = (op & 0x20) == 0;
        u32 la = cpu.addressRegs()[reg];
        bool supervisor = (cpu.dfc & 4) != 0;
        bool wp = false;
        if (transparent(la, supervisor, false, write, wp)) {
            mmusr = (la & 0xFFFFF000) | SR_TRANSPARENT | SR_RESIDENT;
            return true;
        }
        u32 frame, status;
        bool superOnly, modified;
        if (walk(la, supervisor, write, true, frame, wp, superOnly, modified, status)) mmusr = frame | status;
        else mmusr = SR_BUSERR;
        return true;
    }
    return false;
}

} // namespace macemu
