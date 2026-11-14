// The Mac's pointer set directly in Mac OS's low memory (direct mouse).
//
// Mac OS keeps the pointer in globals the cursor's interrupt task reads:
// MTemp and RawMouse (where it is), CrsrPin (the rectangle it is kept in:
// the screen, or the screens), CrsrNew (draw it again) and CrsrCouple (the
// pointer follows the mouse; programs that read the mouse themselves clear
// it).  Moving MTemp and RawMouse and setting CrsrNew moves the pointer at
// once, as Basilisk II does, with no mouse reports to wait for and no
// acceleration on top.

#ifndef MACEMU_POINTER_H
#define MACEMU_POINTER_H

#include "types.h"

namespace macemu {

// False (nothing changed): Mac OS is not up, or the pointer is uncoupled;
// then the ADB mouse moves it, as on a real Mac.
inline bool movePointerDirect(u8 *ram, u32 ramSize, int dx, int dy)
{
    enum { MTemp = 0x828, RawMouse = 0x82C, CrsrPin = 0x834, CrsrNew = 0x8CE, CrsrCouple = 0x8CF };
    if (!ram || ramSize < 0x1000 || ram[CrsrCouple] != 0xFF) return false;
    auto get = [&](u32 a) { return (int)(s16)((ram[a] << 8) | ram[a + 1]); };
    auto put = [&](u32 a, int v) { ram[a] = (u8)((u16)v >> 8); ram[a + 1] = (u8)v; };
    int top = get(CrsrPin), left = get(CrsrPin + 2), bottom = get(CrsrPin + 4), right = get(CrsrPin + 6);
    if (top >= bottom || left >= right || bottom - top > 8192 || right - left > 8192) return false;   // not set up
    int y = get(MTemp) + dy, x = get(MTemp + 2) + dx;
    if (x < left) x = left;
    if (x > right - 1) x = right - 1;
    if (y < top) y = top;
    if (y > bottom - 1) y = bottom - 1;
    put(MTemp, y); put(MTemp + 2, x);
    put(RawMouse, y); put(RawMouse + 2, x);
    ram[CrsrNew] = ram[CrsrCouple];
    return true;
}

} // namespace macemu

#endif
