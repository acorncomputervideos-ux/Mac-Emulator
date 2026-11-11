#include "keymap.h"
#include "../core/keys.h"

namespace macemu {

static const u8 kLetters[26] = {
    MK_A, MK_B, MK_C, MK_D, MK_E, MK_F, MK_G, MK_H, MK_I, MK_J, MK_K, MK_L, MK_M,
    MK_N, MK_O, MK_P, MK_Q, MK_R, MK_S, MK_T, MK_U, MK_V, MK_W, MK_X, MK_Y, MK_Z,
};

static const u8 kDigits[10] = {MK_1, MK_2, MK_3, MK_4, MK_5, MK_6, MK_7, MK_8, MK_9, MK_0};

static const u8 kKeypadDigits[10] = {MK_Kp1, MK_Kp2, MK_Kp3, MK_Kp4, MK_Kp5, MK_Kp6, MK_Kp7, MK_Kp8, MK_Kp9, MK_Kp0};

int usbToMacKey(u8 u)
{
    if (u >= 0x04 && u <= 0x1D) return kLetters[u - 0x04];
    if (u >= 0x1E && u <= 0x27) return kDigits[u - 0x1E];
    if (u >= 0x59 && u <= 0x62) return kKeypadDigits[u - 0x59];
    switch (u) {
    case 0x28: return MK_Return;
    case 0x2A: return MK_Backspace;
    case 0x2B: return MK_Tab;
    case 0x2C: return MK_Space;
    case 0x2D: return MK_Minus;
    case 0x2E: return MK_Equal;
    case 0x2F: return MK_LeftBracket;
    case 0x30: return MK_RightBracket;
    case 0x31: case 0x32: return MK_Backslash;
    case 0x33: return MK_Semicolon;
    case 0x34: return MK_Quote;
    case 0x35: case 0x64: return MK_Grave;
    case 0x36: return MK_Comma;
    case 0x37: return MK_Period;
    case 0x38: return MK_Slash;
    case 0x39: return MK_CapsLock;
    case 0x4C: return MK_Backspace;                     // forward delete
    case 0x4F: return MK_Right;
    case 0x50: return MK_Left;
    case 0x51: return MK_Down;
    case 0x52: return MK_Up;
    case 0x53: return MK_KpClear;                       // Num Lock sits where Clear does
    case 0x54: return MK_Slash;
    case 0x56: return MK_KpMinus;
    case 0x58: return MK_KpEnter;
    case 0x63: return MK_KpDecimal;
    default: return MK_None;
    }
}

int usbModifierToMacKey(u8 bit, bool altIsOption)
{
    switch (bit) {
    case HIDMOD_LSHIFT: case HIDMOD_RSHIFT: return MK_Shift;
    case HIDMOD_LCTRL: case HIDMOD_RCTRL: return MK_Command;
    case HIDMOD_LALT: return altIsOption ? MK_Option : MK_Command;
    case HIDMOD_LGUI: case HIDMOD_RGUI: return altIsOption ? MK_Command : MK_Option;
    case HIDMOD_RALT: return MK_Option;
    default: return MK_None;
    }
}

} // namespace macemu
