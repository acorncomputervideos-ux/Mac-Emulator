// USB HID keyboard usage codes -> Macintosh Plus key codes.

#ifndef MACEMU_KEYMAP_H
#define MACEMU_KEYMAP_H

#include "../core/types.h"

namespace macemu {

// USB HID usage IDs the frontends treat specially.
enum {
    HID_CAPSLOCK = 0x39,
    HID_F1 = 0x3A, HID_F9 = 0x42, HID_F10 = 0x43, HID_F11 = 0x44, HID_F12 = 0x45,
    HID_ENTER = 0x28, HID_ESC = 0x29, HID_SPACE = 0x2C, HID_KP_ENTER = 0x58,
    HID_RIGHT = 0x4F, HID_LEFT = 0x50, HID_DOWN = 0x51, HID_UP = 0x52,
    HID_PAGEUP = 0x4B, HID_PAGEDOWN = 0x4E,
};

// HID modifier byte bits.
enum {
    HIDMOD_LCTRL = 0x01, HIDMOD_LSHIFT = 0x02, HIDMOD_LALT = 0x04, HIDMOD_LGUI = 0x08,
    HIDMOD_RCTRL = 0x10, HIDMOD_RSHIFT = 0x20, HIDMOD_RALT = 0x40, HIDMOD_RGUI = 0x80,
};

// Returns a MacKey, or MK_None (0xFF) if the key has no Macintosh equivalent.
int usbToMacKey(u8 usage);

// Mac modifier key for one bit of the HID modifier byte.
int usbModifierToMacKey(u8 modifierBit, bool altIsOption);

} // namespace macemu

#endif
