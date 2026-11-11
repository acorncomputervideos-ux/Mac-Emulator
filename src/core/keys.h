// Macintosh virtual key codes as understood by Machine::keyEvent().
//
// Codes below 0x40 are main-block keys.  Codes 0x40 and up live on the
// numeric keypad of the Macintosh Plus keyboard, which is also where its
// four arrow keys are wired.

#ifndef MACEMU_KEYS_H
#define MACEMU_KEYS_H

namespace macemu {

enum MacKey {
    MK_A = 0x00, MK_S = 0x01, MK_D = 0x02, MK_F = 0x03, MK_H = 0x04, MK_G = 0x05, MK_Z = 0x06, MK_X = 0x07,
    MK_C = 0x08, MK_V = 0x09, MK_B = 0x0B, MK_Q = 0x0C, MK_W = 0x0D, MK_E = 0x0E, MK_R = 0x0F,
    MK_Y = 0x10, MK_T = 0x11, MK_1 = 0x12, MK_2 = 0x13, MK_3 = 0x14, MK_4 = 0x15, MK_6 = 0x16, MK_5 = 0x17,
    MK_Equal = 0x18, MK_9 = 0x19, MK_7 = 0x1A, MK_Minus = 0x1B, MK_8 = 0x1C, MK_0 = 0x1D,
    MK_RightBracket = 0x1E, MK_O = 0x1F, MK_U = 0x20, MK_LeftBracket = 0x21, MK_I = 0x22, MK_P = 0x23,
    MK_Return = 0x24, MK_L = 0x25, MK_J = 0x26, MK_Quote = 0x27, MK_K = 0x28, MK_Semicolon = 0x29,
    MK_Backslash = 0x2A, MK_Comma = 0x2B, MK_Slash = 0x2C, MK_N = 0x2D, MK_M = 0x2E, MK_Period = 0x2F,
    MK_Tab = 0x30, MK_Space = 0x31, MK_Grave = 0x32, MK_Backspace = 0x33, MK_Enter = 0x34,
    MK_Command = 0x37, MK_Shift = 0x38, MK_CapsLock = 0x39, MK_Option = 0x3A,

    MK_KpDecimal = 0x41, MK_Right = 0x42, MK_Left = 0x46, MK_KpClear = 0x47, MK_Down = 0x48,
    MK_KpEnter = 0x4C, MK_Up = 0x4D, MK_KpMinus = 0x4E,
    MK_Kp0 = 0x52, MK_Kp1 = 0x53, MK_Kp2 = 0x54, MK_Kp3 = 0x55, MK_Kp4 = 0x56, MK_Kp5 = 0x57,
    MK_Kp6 = 0x58, MK_Kp7 = 0x59, MK_Kp8 = 0x5B, MK_Kp9 = 0x5C,

    MK_None = 0xFF
};

} // namespace macemu

#endif
