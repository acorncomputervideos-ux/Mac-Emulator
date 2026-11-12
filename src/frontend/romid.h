// Which Macintosh a ROM belongs to, from its checksum (the first long), and
// which of the emulated models runs it.

#ifndef MACEMU_ROMID_H
#define MACEMU_ROMID_H

#include "../core/types.h"

namespace macemu {

enum RomFamily {
    RomUnknown,
    RomCompact,         // 128K, 512K, Plus: the Plus model
    RomSE,              // SE, Classic: the Plus model with ADB
    RomMacII,           // Mac II
    RomQuadra,          // Quadra 610/650/800, Centris 610/650
    RomNotYet,          // a known Mac that is not emulated
};

struct RomInfo {
    u32 checksum;
    RomFamily family;
    const char *name;
};

inline const RomInfo &romIdentify(const u8 *rom, u32 size)
{
    static const RomInfo known[] = {
        {0x28BA61CE, RomCompact, "Macintosh 128K/512K"},
        {0x28BA4E50, RomCompact, "Macintosh 512K"},
        {0x4D1EEEE1, RomCompact, "Macintosh Plus (v1)"},
        {0x4D1EEAE1, RomCompact, "Macintosh Plus (v2)"},
        {0x4D1F8172, RomCompact, "Macintosh Plus (v3)"},
        {0xB2E362A8, RomSE, "Macintosh SE"},
        {0xB306E171, RomSE, "Macintosh SE FDHD"},
        {0xA49F9914, RomSE, "Macintosh Classic"},
        {0x9779D2C4, RomMacII, "Macintosh II"},
        {0x97851DB6, RomMacII, "Macintosh II (FDHD)"},
        {0xF1ACAD13, RomQuadra, "Quadra 610/650/800"},
        {0xF1A6F343, RomQuadra, "Centris 610/650"},
        {0x97221136, RomNotYet, "Macintosh IIx/IIcx/SE/30"},
        {0x368CADFE, RomNotYet, "Macintosh IIci"},
        {0x36B7FB6C, RomNotYet, "Macintosh IIsi"},
        {0x4147DD77, RomNotYet, "Macintosh IIfx"},
        {0x350EACF0, RomNotYet, "Macintosh LC"},
        {0x35C28F5F, RomNotYet, "Macintosh LC II / Classic II"},
        {0xECD99DC0, RomNotYet, "Macintosh Color Classic"},
        {0xECBBC41C, RomNotYet, "Macintosh LC III"},
        {0x4957EB49, RomNotYet, "Macintosh IIvx / Performa 600"},
        {0xFF7439EE, RomNotYet, "LC 475 / Quadra 605 / Performa 570"},
        {0x06684214, RomNotYet, "LC 580 / Quadra 630 / Performa 580"},
        {0x420DBFF3, RomNotYet, "Quadra 700/900"},
        {0x3DC27823, RomNotYet, "Quadra 950"},
    };
    static const RomInfo unknownCompact = {0, RomCompact, "unknown compact Mac"};
    static const RomInfo unknown = {0, RomUnknown, "unknown Mac"};
    u32 sum = size >= 4 ? ((u32)rom[0] << 24 | (u32)rom[1] << 16 | (u32)rom[2] << 8 | rom[3]) : 0;
    for (const RomInfo &r : known) if (r.checksum == sum) return r;
    return size <= 0x20000 ? unknownCompact : unknown;
}

} // namespace macemu

#endif
