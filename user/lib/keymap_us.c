/* The English (US) layout: the C table and the XKB keymap (keymap_us.c).
 * Made by tools/genkeymap.py from abi/keymap/us.txt and abi/keymap/keys.txt:
 * don't edit it; change what it is made from and run `make keymap` (`make
 * check` fails while it is stale). <keymap.h> says how the tables are used. */
#include <keymap.h>

/* By evdev code: type, repeats, reserved, keysyms, characters. */
static const struct keymap_key keys[KEYMAP_CODES] = {
    /* ESC: Escape */
    [1] = { KEYMAP_ONE_LEVEL, true, 0, { 0xff1b, 0 }, { 0x1b, 0 } },
    /* AE01: 1 exclam */
    [2] = { KEYMAP_TWO_LEVEL, true, 0, { 0x31, 0x21 }, { 0x31, 0x21 } },
    /* AE02: 2 at */
    [3] = { KEYMAP_TWO_LEVEL, true, 0, { 0x32, 0x40 }, { 0x32, 0x40 } },
    /* AE03: 3 numbersign */
    [4] = { KEYMAP_TWO_LEVEL, true, 0, { 0x33, 0x23 }, { 0x33, 0x23 } },
    /* AE04: 4 dollar */
    [5] = { KEYMAP_TWO_LEVEL, true, 0, { 0x34, 0x24 }, { 0x34, 0x24 } },
    /* AE05: 5 percent */
    [6] = { KEYMAP_TWO_LEVEL, true, 0, { 0x35, 0x25 }, { 0x35, 0x25 } },
    /* AE06: 6 asciicircum */
    [7] = { KEYMAP_TWO_LEVEL, true, 0, { 0x36, 0x5e }, { 0x36, 0x5e } },
    /* AE07: 7 ampersand */
    [8] = { KEYMAP_TWO_LEVEL, true, 0, { 0x37, 0x26 }, { 0x37, 0x26 } },
    /* AE08: 8 asterisk */
    [9] = { KEYMAP_TWO_LEVEL, true, 0, { 0x38, 0x2a }, { 0x38, 0x2a } },
    /* AE09: 9 parenleft */
    [10] = { KEYMAP_TWO_LEVEL, true, 0, { 0x39, 0x28 }, { 0x39, 0x28 } },
    /* AE10: 0 parenright */
    [11] = { KEYMAP_TWO_LEVEL, true, 0, { 0x30, 0x29 }, { 0x30, 0x29 } },
    /* AE11: minus underscore */
    [12] = { KEYMAP_TWO_LEVEL, true, 0, { 0x2d, 0x5f }, { 0x2d, 0x5f } },
    /* AE12: equal plus */
    [13] = { KEYMAP_TWO_LEVEL, true, 0, { 0x3d, 0x2b }, { 0x3d, 0x2b } },
    /* BKSP: BackSpace */
    [14] = { KEYMAP_ONE_LEVEL, true, 0, { 0xff08, 0 }, { 0x8, 0 } },
    /* TAB: Tab ISO_Left_Tab */
    [15] = { KEYMAP_TWO_LEVEL, true, 0, { 0xff09, 0xfe20 }, { 0x9, 0x9 } },
    /* AD01: q Q */
    [16] = { KEYMAP_ALPHABETIC, true, 0, { 0x71, 0x51 }, { 0x71, 0x51 } },
    /* AD02: w W */
    [17] = { KEYMAP_ALPHABETIC, true, 0, { 0x77, 0x57 }, { 0x77, 0x57 } },
    /* AD03: e E */
    [18] = { KEYMAP_ALPHABETIC, true, 0, { 0x65, 0x45 }, { 0x65, 0x45 } },
    /* AD04: r R */
    [19] = { KEYMAP_ALPHABETIC, true, 0, { 0x72, 0x52 }, { 0x72, 0x52 } },
    /* AD05: t T */
    [20] = { KEYMAP_ALPHABETIC, true, 0, { 0x74, 0x54 }, { 0x74, 0x54 } },
    /* AD06: y Y */
    [21] = { KEYMAP_ALPHABETIC, true, 0, { 0x79, 0x59 }, { 0x79, 0x59 } },
    /* AD07: u U */
    [22] = { KEYMAP_ALPHABETIC, true, 0, { 0x75, 0x55 }, { 0x75, 0x55 } },
    /* AD08: i I */
    [23] = { KEYMAP_ALPHABETIC, true, 0, { 0x69, 0x49 }, { 0x69, 0x49 } },
    /* AD09: o O */
    [24] = { KEYMAP_ALPHABETIC, true, 0, { 0x6f, 0x4f }, { 0x6f, 0x4f } },
    /* AD10: p P */
    [25] = { KEYMAP_ALPHABETIC, true, 0, { 0x70, 0x50 }, { 0x70, 0x50 } },
    /* AD11: bracketleft braceleft */
    [26] = { KEYMAP_TWO_LEVEL, true, 0, { 0x5b, 0x7b }, { 0x5b, 0x7b } },
    /* AD12: bracketright braceright */
    [27] = { KEYMAP_TWO_LEVEL, true, 0, { 0x5d, 0x7d }, { 0x5d, 0x7d } },
    /* RTRN: Return */
    [28] = { KEYMAP_ONE_LEVEL, true, 0, { 0xff0d, 0 }, { 0xa, 0 } },
    /* LCTL: Control_L */
    [29] = { KEYMAP_ONE_LEVEL, false, 0, { 0xffe3, 0 }, { 0, 0 } },
    /* AC01: a A */
    [30] = { KEYMAP_ALPHABETIC, true, 0, { 0x61, 0x41 }, { 0x61, 0x41 } },
    /* AC02: s S */
    [31] = { KEYMAP_ALPHABETIC, true, 0, { 0x73, 0x53 }, { 0x73, 0x53 } },
    /* AC03: d D */
    [32] = { KEYMAP_ALPHABETIC, true, 0, { 0x64, 0x44 }, { 0x64, 0x44 } },
    /* AC04: f F */
    [33] = { KEYMAP_ALPHABETIC, true, 0, { 0x66, 0x46 }, { 0x66, 0x46 } },
    /* AC05: g G */
    [34] = { KEYMAP_ALPHABETIC, true, 0, { 0x67, 0x47 }, { 0x67, 0x47 } },
    /* AC06: h H */
    [35] = { KEYMAP_ALPHABETIC, true, 0, { 0x68, 0x48 }, { 0x68, 0x48 } },
    /* AC07: j J */
    [36] = { KEYMAP_ALPHABETIC, true, 0, { 0x6a, 0x4a }, { 0x6a, 0x4a } },
    /* AC08: k K */
    [37] = { KEYMAP_ALPHABETIC, true, 0, { 0x6b, 0x4b }, { 0x6b, 0x4b } },
    /* AC09: l L */
    [38] = { KEYMAP_ALPHABETIC, true, 0, { 0x6c, 0x4c }, { 0x6c, 0x4c } },
    /* AC10: semicolon colon */
    [39] = { KEYMAP_TWO_LEVEL, true, 0, { 0x3b, 0x3a }, { 0x3b, 0x3a } },
    /* AC11: apostrophe quotedbl */
    [40] = { KEYMAP_TWO_LEVEL, true, 0, { 0x27, 0x22 }, { 0x27, 0x22 } },
    /* TLDE: grave asciitilde */
    [41] = { KEYMAP_TWO_LEVEL, true, 0, { 0x60, 0x7e }, { 0x60, 0x7e } },
    /* LFSH: Shift_L */
    [42] = { KEYMAP_ONE_LEVEL, false, 0, { 0xffe1, 0 }, { 0, 0 } },
    /* BKSL: backslash bar */
    [43] = { KEYMAP_TWO_LEVEL, true, 0, { 0x5c, 0x7c }, { 0x5c, 0x7c } },
    /* AB01: z Z */
    [44] = { KEYMAP_ALPHABETIC, true, 0, { 0x7a, 0x5a }, { 0x7a, 0x5a } },
    /* AB02: x X */
    [45] = { KEYMAP_ALPHABETIC, true, 0, { 0x78, 0x58 }, { 0x78, 0x58 } },
    /* AB03: c C */
    [46] = { KEYMAP_ALPHABETIC, true, 0, { 0x63, 0x43 }, { 0x63, 0x43 } },
    /* AB04: v V */
    [47] = { KEYMAP_ALPHABETIC, true, 0, { 0x76, 0x56 }, { 0x76, 0x56 } },
    /* AB05: b B */
    [48] = { KEYMAP_ALPHABETIC, true, 0, { 0x62, 0x42 }, { 0x62, 0x42 } },
    /* AB06: n N */
    [49] = { KEYMAP_ALPHABETIC, true, 0, { 0x6e, 0x4e }, { 0x6e, 0x4e } },
    /* AB07: m M */
    [50] = { KEYMAP_ALPHABETIC, true, 0, { 0x6d, 0x4d }, { 0x6d, 0x4d } },
    /* AB08: comma less */
    [51] = { KEYMAP_TWO_LEVEL, true, 0, { 0x2c, 0x3c }, { 0x2c, 0x3c } },
    /* AB09: period greater */
    [52] = { KEYMAP_TWO_LEVEL, true, 0, { 0x2e, 0x3e }, { 0x2e, 0x3e } },
    /* AB10: slash question */
    [53] = { KEYMAP_TWO_LEVEL, true, 0, { 0x2f, 0x3f }, { 0x2f, 0x3f } },
    /* RTSH: Shift_R */
    [54] = { KEYMAP_ONE_LEVEL, false, 0, { 0xffe2, 0 }, { 0, 0 } },
    /* KPMU: KP_Multiply */
    [55] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffaa, 0 }, { 0x2a, 0 } },
    /* LALT: Alt_L */
    [56] = { KEYMAP_ONE_LEVEL, false, 0, { 0xffe9, 0 }, { 0, 0 } },
    /* SPCE: space */
    [57] = { KEYMAP_ONE_LEVEL, true, 0, { 0x20, 0 }, { 0x20, 0 } },
    /* CAPS: Caps_Lock */
    [58] = { KEYMAP_ONE_LEVEL, false, 0, { 0xffe5, 0 }, { 0, 0 } },
    /* FK01: F1 */
    [59] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffbe, 0 }, { 0, 0 } },
    /* FK02: F2 */
    [60] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffbf, 0 }, { 0, 0 } },
    /* FK03: F3 */
    [61] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffc0, 0 }, { 0, 0 } },
    /* FK04: F4 */
    [62] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffc1, 0 }, { 0, 0 } },
    /* FK05: F5 */
    [63] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffc2, 0 }, { 0, 0 } },
    /* FK06: F6 */
    [64] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffc3, 0 }, { 0, 0 } },
    /* FK07: F7 */
    [65] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffc4, 0 }, { 0, 0 } },
    /* FK08: F8 */
    [66] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffc5, 0 }, { 0, 0 } },
    /* FK09: F9 */
    [67] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffc6, 0 }, { 0, 0 } },
    /* FK10: F10 */
    [68] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffc7, 0 }, { 0, 0 } },
    /* NMLK: Num_Lock */
    [69] = { KEYMAP_ONE_LEVEL, false, 0, { 0xff7f, 0 }, { 0, 0 } },
    /* SCLK: Scroll_Lock */
    [70] = { KEYMAP_ONE_LEVEL, false, 0, { 0xff14, 0 }, { 0, 0 } },
    /* KP7: KP_Home KP_7 */
    [71] = { KEYMAP_KEYPAD, true, 0, { 0xff95, 0xffb7 }, { 0, 0x37 } },
    /* KP8: KP_Up KP_8 */
    [72] = { KEYMAP_KEYPAD, true, 0, { 0xff97, 0xffb8 }, { 0, 0x38 } },
    /* KP9: KP_Prior KP_9 */
    [73] = { KEYMAP_KEYPAD, true, 0, { 0xff9a, 0xffb9 }, { 0, 0x39 } },
    /* KPSU: KP_Subtract */
    [74] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffad, 0 }, { 0x2d, 0 } },
    /* KP4: KP_Left KP_4 */
    [75] = { KEYMAP_KEYPAD, true, 0, { 0xff96, 0xffb4 }, { 0, 0x34 } },
    /* KP5: KP_Begin KP_5 */
    [76] = { KEYMAP_KEYPAD, true, 0, { 0xff9d, 0xffb5 }, { 0, 0x35 } },
    /* KP6: KP_Right KP_6 */
    [77] = { KEYMAP_KEYPAD, true, 0, { 0xff98, 0xffb6 }, { 0, 0x36 } },
    /* KPAD: KP_Add */
    [78] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffab, 0 }, { 0x2b, 0 } },
    /* KP1: KP_End KP_1 */
    [79] = { KEYMAP_KEYPAD, true, 0, { 0xff9c, 0xffb1 }, { 0, 0x31 } },
    /* KP2: KP_Down KP_2 */
    [80] = { KEYMAP_KEYPAD, true, 0, { 0xff99, 0xffb2 }, { 0, 0x32 } },
    /* KP3: KP_Next KP_3 */
    [81] = { KEYMAP_KEYPAD, true, 0, { 0xff9b, 0xffb3 }, { 0, 0x33 } },
    /* KP0: KP_Insert KP_0 */
    [82] = { KEYMAP_KEYPAD, true, 0, { 0xff9e, 0xffb0 }, { 0, 0x30 } },
    /* KPDL: KP_Delete KP_Decimal */
    [83] = { KEYMAP_KEYPAD, true, 0, { 0xff9f, 0xffae }, { 0, 0x2e } },
    /* LSGT: backslash bar */
    [86] = { KEYMAP_TWO_LEVEL, true, 0, { 0x5c, 0x7c }, { 0x5c, 0x7c } },
    /* FK11: F11 */
    [87] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffc8, 0 }, { 0, 0 } },
    /* FK12: F12 */
    [88] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffc9, 0 }, { 0, 0 } },
    /* KPEN: KP_Enter */
    [96] = { KEYMAP_ONE_LEVEL, true, 0, { 0xff8d, 0 }, { 0xa, 0 } },
    /* RCTL: Control_R */
    [97] = { KEYMAP_ONE_LEVEL, false, 0, { 0xffe4, 0 }, { 0, 0 } },
    /* KPDV: KP_Divide */
    [98] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffaf, 0 }, { 0x2f, 0 } },
    /* PRSC: Print */
    [99] = { KEYMAP_ONE_LEVEL, true, 0, { 0xff61, 0 }, { 0, 0 } },
    /* RALT: Alt_R */
    [100] = { KEYMAP_ONE_LEVEL, false, 0, { 0xffea, 0 }, { 0, 0 } },
    /* HOME: Home */
    [102] = { KEYMAP_ONE_LEVEL, true, 0, { 0xff50, 0 }, { 0, 0 } },
    /* UP: Up */
    [103] = { KEYMAP_ONE_LEVEL, true, 0, { 0xff52, 0 }, { 0, 0 } },
    /* PGUP: Prior */
    [104] = { KEYMAP_ONE_LEVEL, true, 0, { 0xff55, 0 }, { 0, 0 } },
    /* LEFT: Left */
    [105] = { KEYMAP_ONE_LEVEL, true, 0, { 0xff51, 0 }, { 0, 0 } },
    /* RGHT: Right */
    [106] = { KEYMAP_ONE_LEVEL, true, 0, { 0xff53, 0 }, { 0, 0 } },
    /* END: End */
    [107] = { KEYMAP_ONE_LEVEL, true, 0, { 0xff57, 0 }, { 0, 0 } },
    /* DOWN: Down */
    [108] = { KEYMAP_ONE_LEVEL, true, 0, { 0xff54, 0 }, { 0, 0 } },
    /* PGDN: Next */
    [109] = { KEYMAP_ONE_LEVEL, true, 0, { 0xff56, 0 }, { 0, 0 } },
    /* INS: Insert */
    [110] = { KEYMAP_ONE_LEVEL, true, 0, { 0xff63, 0 }, { 0, 0 } },
    /* DELE: Delete */
    [111] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffff, 0 }, { 0, 0 } },
    /* MUTE: XF86AudioMute */
    [113] = { KEYMAP_ONE_LEVEL, true, 0, { 0x1008ff12, 0 }, { 0, 0 } },
    /* VOL-: XF86AudioLowerVolume */
    [114] = { KEYMAP_ONE_LEVEL, true, 0, { 0x1008ff11, 0 }, { 0, 0 } },
    /* VOL+: XF86AudioRaiseVolume */
    [115] = { KEYMAP_ONE_LEVEL, true, 0, { 0x1008ff13, 0 }, { 0, 0 } },
    /* POWR: XF86PowerOff */
    [116] = { KEYMAP_ONE_LEVEL, true, 0, { 0x1008ff2a, 0 }, { 0, 0 } },
    /* KPEQ: KP_Equal */
    [117] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffbd, 0 }, { 0x3d, 0 } },
    /* PAUS: Pause */
    [119] = { KEYMAP_ONE_LEVEL, true, 0, { 0xff13, 0 }, { 0, 0 } },
    /* LWIN: Super_L */
    [125] = { KEYMAP_ONE_LEVEL, false, 0, { 0xffeb, 0 }, { 0, 0 } },
    /* RWIN: Super_R */
    [126] = { KEYMAP_ONE_LEVEL, false, 0, { 0xffec, 0 }, { 0, 0 } },
    /* COMP: Menu */
    [127] = { KEYMAP_ONE_LEVEL, true, 0, { 0xff67, 0 }, { 0, 0 } },
    /* FK13: F13 */
    [183] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffca, 0 }, { 0, 0 } },
    /* FK14: F14 */
    [184] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffcb, 0 }, { 0, 0 } },
    /* FK15: F15 */
    [185] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffcc, 0 }, { 0, 0 } },
    /* FK16: F16 */
    [186] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffcd, 0 }, { 0, 0 } },
    /* FK17: F17 */
    [187] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffce, 0 }, { 0, 0 } },
    /* FK18: F18 */
    [188] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffcf, 0 }, { 0, 0 } },
    /* FK19: F19 */
    [189] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffd0, 0 }, { 0, 0 } },
    /* FK20: F20 */
    [190] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffd1, 0 }, { 0, 0 } },
    /* FK21: F21 */
    [191] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffd2, 0 }, { 0, 0 } },
    /* FK22: F22 */
    [192] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffd3, 0 }, { 0, 0 } },
    /* FK23: F23 */
    [193] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffd4, 0 }, { 0, 0 } },
    /* FK24: F24 */
    [194] = { KEYMAP_ONE_LEVEL, true, 0, { 0xffd5, 0 }, { 0, 0 } },
};

/* The XKB keymap (format xkb_v1), sent as wl_keyboard.keymap. */
static const char xkb[] =
    "// jamos-keymap us: English (US), from abi/keymap/us.txt by tools/genkeymap.py\n"
    "xkb_keymap {\n"
    "xkb_keycodes \"jamos-us\" {\n"
    "    minimum = 8;\n"
    "    maximum = 255;\n"
    "    <ESC> = 9;\n"
    "    <AE01> = 10;\n"
    "    <AE02> = 11;\n"
    "    <AE03> = 12;\n"
    "    <AE04> = 13;\n"
    "    <AE05> = 14;\n"
    "    <AE06> = 15;\n"
    "    <AE07> = 16;\n"
    "    <AE08> = 17;\n"
    "    <AE09> = 18;\n"
    "    <AE10> = 19;\n"
    "    <AE11> = 20;\n"
    "    <AE12> = 21;\n"
    "    <BKSP> = 22;\n"
    "    <TAB> = 23;\n"
    "    <AD01> = 24;\n"
    "    <AD02> = 25;\n"
    "    <AD03> = 26;\n"
    "    <AD04> = 27;\n"
    "    <AD05> = 28;\n"
    "    <AD06> = 29;\n"
    "    <AD07> = 30;\n"
    "    <AD08> = 31;\n"
    "    <AD09> = 32;\n"
    "    <AD10> = 33;\n"
    "    <AD11> = 34;\n"
    "    <AD12> = 35;\n"
    "    <RTRN> = 36;\n"
    "    <LCTL> = 37;\n"
    "    <AC01> = 38;\n"
    "    <AC02> = 39;\n"
    "    <AC03> = 40;\n"
    "    <AC04> = 41;\n"
    "    <AC05> = 42;\n"
    "    <AC06> = 43;\n"
    "    <AC07> = 44;\n"
    "    <AC08> = 45;\n"
    "    <AC09> = 46;\n"
    "    <AC10> = 47;\n"
    "    <AC11> = 48;\n"
    "    <TLDE> = 49;\n"
    "    <LFSH> = 50;\n"
    "    <BKSL> = 51;\n"
    "    <AB01> = 52;\n"
    "    <AB02> = 53;\n"
    "    <AB03> = 54;\n"
    "    <AB04> = 55;\n"
    "    <AB05> = 56;\n"
    "    <AB06> = 57;\n"
    "    <AB07> = 58;\n"
    "    <AB08> = 59;\n"
    "    <AB09> = 60;\n"
    "    <AB10> = 61;\n"
    "    <RTSH> = 62;\n"
    "    <KPMU> = 63;\n"
    "    <LALT> = 64;\n"
    "    <SPCE> = 65;\n"
    "    <CAPS> = 66;\n"
    "    <FK01> = 67;\n"
    "    <FK02> = 68;\n"
    "    <FK03> = 69;\n"
    "    <FK04> = 70;\n"
    "    <FK05> = 71;\n"
    "    <FK06> = 72;\n"
    "    <FK07> = 73;\n"
    "    <FK08> = 74;\n"
    "    <FK09> = 75;\n"
    "    <FK10> = 76;\n"
    "    <NMLK> = 77;\n"
    "    <SCLK> = 78;\n"
    "    <KP7> = 79;\n"
    "    <KP8> = 80;\n"
    "    <KP9> = 81;\n"
    "    <KPSU> = 82;\n"
    "    <KP4> = 83;\n"
    "    <KP5> = 84;\n"
    "    <KP6> = 85;\n"
    "    <KPAD> = 86;\n"
    "    <KP1> = 87;\n"
    "    <KP2> = 88;\n"
    "    <KP3> = 89;\n"
    "    <KP0> = 90;\n"
    "    <KPDL> = 91;\n"
    "    <LSGT> = 94;\n"
    "    <FK11> = 95;\n"
    "    <FK12> = 96;\n"
    "    <KPEN> = 104;\n"
    "    <RCTL> = 105;\n"
    "    <KPDV> = 106;\n"
    "    <PRSC> = 107;\n"
    "    <RALT> = 108;\n"
    "    <HOME> = 110;\n"
    "    <UP> = 111;\n"
    "    <PGUP> = 112;\n"
    "    <LEFT> = 113;\n"
    "    <RGHT> = 114;\n"
    "    <END> = 115;\n"
    "    <DOWN> = 116;\n"
    "    <PGDN> = 117;\n"
    "    <INS> = 118;\n"
    "    <DELE> = 119;\n"
    "    <MUTE> = 121;\n"
    "    <VOL-> = 122;\n"
    "    <VOL+> = 123;\n"
    "    <POWR> = 124;\n"
    "    <KPEQ> = 125;\n"
    "    <PAUS> = 127;\n"
    "    <LWIN> = 133;\n"
    "    <RWIN> = 134;\n"
    "    <COMP> = 135;\n"
    "    <FK13> = 191;\n"
    "    <FK14> = 192;\n"
    "    <FK15> = 193;\n"
    "    <FK16> = 194;\n"
    "    <FK17> = 195;\n"
    "    <FK18> = 196;\n"
    "    <FK19> = 197;\n"
    "    <FK20> = 198;\n"
    "    <FK21> = 199;\n"
    "    <FK22> = 200;\n"
    "    <FK23> = 201;\n"
    "    <FK24> = 202;\n"
    "    indicator 1 = \"Caps Lock\";\n"
    "    indicator 2 = \"Num Lock\";\n"
    "    indicator 3 = \"Scroll Lock\";\n"
    "};\n"
    "xkb_types \"jamos-us\" {\n"
    "    virtual_modifiers NumLock;\n"
    "    type \"ONE_LEVEL\" {\n"
    "        modifiers = none;\n"
    "        level_name[Level1] = \"Any\";\n"
    "    };\n"
    "    type \"TWO_LEVEL\" {\n"
    "        modifiers = Shift;\n"
    "        map[Shift] = Level2;\n"
    "        level_name[Level1] = \"Base\";\n"
    "        level_name[Level2] = \"Shift\";\n"
    "    };\n"
    "    // Shift or Caps Lock, not both, picks level 2\n"
    "    type \"ALPHABETIC\" {\n"
    "        modifiers = Shift+Lock;\n"
    "        map[Shift] = Level2;\n"
    "        map[Lock] = Level2;\n"
    "        level_name[Level1] = \"Base\";\n"
    "        level_name[Level2] = \"Caps\";\n"
    "    };\n"
    "    // Num Lock alone picks level 2: Shift changes nothing here\n"
    "    type \"KEYPAD\" {\n"
    "        modifiers = NumLock;\n"
    "        map[NumLock] = Level2;\n"
    "        level_name[Level1] = \"Base\";\n"
    "        level_name[Level2] = \"Number\";\n"
    "    };\n"
    "};\n"
    "xkb_compatibility \"jamos-us\" {\n"
    "    virtual_modifiers NumLock,Alt,Super;\n"
    "    interpret.useModMapMods = AnyLevel;\n"
    "    interpret.repeat = False;\n"
    "    interpret.locking = False;\n"
    "    interpret Shift_L+AnyOfOrNone(all) {\n"
    "        action = SetMods(modifiers=Shift,clearLocks);\n"
    "    };\n"
    "    interpret Shift_R+AnyOfOrNone(all) {\n"
    "        action = SetMods(modifiers=Shift,clearLocks);\n"
    "    };\n"
    "    interpret Caps_Lock+AnyOfOrNone(all) {\n"
    "        action = LockMods(modifiers=Lock);\n"
    "    };\n"
    "    interpret Control_L+AnyOfOrNone(all) {\n"
    "        action = SetMods(modifiers=Control,clearLocks);\n"
    "    };\n"
    "    interpret Control_R+AnyOfOrNone(all) {\n"
    "        action = SetMods(modifiers=Control,clearLocks);\n"
    "    };\n"
    "    interpret Alt_L+AnyOfOrNone(all) {\n"
    "        virtualModifier = Alt;\n"
    "        action = SetMods(modifiers=Alt,clearLocks);\n"
    "    };\n"
    "    interpret Alt_R+AnyOfOrNone(all) {\n"
    "        virtualModifier = Alt;\n"
    "        action = SetMods(modifiers=Alt,clearLocks);\n"
    "    };\n"
    "    interpret Num_Lock+AnyOfOrNone(all) {\n"
    "        virtualModifier = NumLock;\n"
    "        action = LockMods(modifiers=NumLock);\n"
    "    };\n"
    "    interpret Super_L+AnyOfOrNone(all) {\n"
    "        virtualModifier = Super;\n"
    "        action = SetMods(modifiers=Super,clearLocks);\n"
    "    };\n"
    "    interpret Super_R+AnyOfOrNone(all) {\n"
    "        virtualModifier = Super;\n"
    "        action = SetMods(modifiers=Super,clearLocks);\n"
    "    };\n"
    "    indicator \"Caps Lock\" {\n"
    "        whichModState = Locked;\n"
    "        modifiers = Lock;\n"
    "    };\n"
    "    indicator \"Num Lock\" {\n"
    "        whichModState = Locked;\n"
    "        modifiers = NumLock;\n"
    "    };\n"
    "};\n"
    "xkb_symbols \"jamos-us\" {\n"
    "    name[Group1] = \"English (US)\";\n"
    "    key <ESC> { type = \"ONE_LEVEL\", [ Escape ] };\n"
    "    key <FK01> { type = \"ONE_LEVEL\", [ F1 ] };\n"
    "    key <FK02> { type = \"ONE_LEVEL\", [ F2 ] };\n"
    "    key <FK03> { type = \"ONE_LEVEL\", [ F3 ] };\n"
    "    key <FK04> { type = \"ONE_LEVEL\", [ F4 ] };\n"
    "    key <FK05> { type = \"ONE_LEVEL\", [ F5 ] };\n"
    "    key <FK06> { type = \"ONE_LEVEL\", [ F6 ] };\n"
    "    key <FK07> { type = \"ONE_LEVEL\", [ F7 ] };\n"
    "    key <FK08> { type = \"ONE_LEVEL\", [ F8 ] };\n"
    "    key <FK09> { type = \"ONE_LEVEL\", [ F9 ] };\n"
    "    key <FK10> { type = \"ONE_LEVEL\", [ F10 ] };\n"
    "    key <FK11> { type = \"ONE_LEVEL\", [ F11 ] };\n"
    "    key <FK12> { type = \"ONE_LEVEL\", [ F12 ] };\n"
    "    key <PRSC> { type = \"ONE_LEVEL\", [ Print ] };\n"
    "    key <SCLK> { type = \"ONE_LEVEL\", repeat = No, [ Scroll_Lock ] };\n"
    "    key <PAUS> { type = \"ONE_LEVEL\", [ Pause ] };\n"
    "    key <TLDE> { type = \"TWO_LEVEL\", [ grave, asciitilde ] };\n"
    "    key <AE01> { type = \"TWO_LEVEL\", [ 1, exclam ] };\n"
    "    key <AE02> { type = \"TWO_LEVEL\", [ 2, at ] };\n"
    "    key <AE03> { type = \"TWO_LEVEL\", [ 3, numbersign ] };\n"
    "    key <AE04> { type = \"TWO_LEVEL\", [ 4, dollar ] };\n"
    "    key <AE05> { type = \"TWO_LEVEL\", [ 5, percent ] };\n"
    "    key <AE06> { type = \"TWO_LEVEL\", [ 6, asciicircum ] };\n"
    "    key <AE07> { type = \"TWO_LEVEL\", [ 7, ampersand ] };\n"
    "    key <AE08> { type = \"TWO_LEVEL\", [ 8, asterisk ] };\n"
    "    key <AE09> { type = \"TWO_LEVEL\", [ 9, parenleft ] };\n"
    "    key <AE10> { type = \"TWO_LEVEL\", [ 0, parenright ] };\n"
    "    key <AE11> { type = \"TWO_LEVEL\", [ minus, underscore ] };\n"
    "    key <AE12> { type = \"TWO_LEVEL\", [ equal, plus ] };\n"
    "    key <BKSP> { type = \"ONE_LEVEL\", [ BackSpace ] };\n"
    "    key <TAB> { type = \"TWO_LEVEL\", [ Tab, ISO_Left_Tab ] };\n"
    "    key <AD01> { type = \"ALPHABETIC\", [ q, Q ] };\n"
    "    key <AD02> { type = \"ALPHABETIC\", [ w, W ] };\n"
    "    key <AD03> { type = \"ALPHABETIC\", [ e, E ] };\n"
    "    key <AD04> { type = \"ALPHABETIC\", [ r, R ] };\n"
    "    key <AD05> { type = \"ALPHABETIC\", [ t, T ] };\n"
    "    key <AD06> { type = \"ALPHABETIC\", [ y, Y ] };\n"
    "    key <AD07> { type = \"ALPHABETIC\", [ u, U ] };\n"
    "    key <AD08> { type = \"ALPHABETIC\", [ i, I ] };\n"
    "    key <AD09> { type = \"ALPHABETIC\", [ o, O ] };\n"
    "    key <AD10> { type = \"ALPHABETIC\", [ p, P ] };\n"
    "    key <AD11> { type = \"TWO_LEVEL\", [ bracketleft, braceleft ] };\n"
    "    key <AD12> { type = \"TWO_LEVEL\", [ bracketright, braceright ] };\n"
    "    key <BKSL> { type = \"TWO_LEVEL\", [ backslash, bar ] };\n"
    "    key <CAPS> { type = \"ONE_LEVEL\", repeat = No, [ Caps_Lock ] };\n"
    "    key <AC01> { type = \"ALPHABETIC\", [ a, A ] };\n"
    "    key <AC02> { type = \"ALPHABETIC\", [ s, S ] };\n"
    "    key <AC03> { type = \"ALPHABETIC\", [ d, D ] };\n"
    "    key <AC04> { type = \"ALPHABETIC\", [ f, F ] };\n"
    "    key <AC05> { type = \"ALPHABETIC\", [ g, G ] };\n"
    "    key <AC06> { type = \"ALPHABETIC\", [ h, H ] };\n"
    "    key <AC07> { type = \"ALPHABETIC\", [ j, J ] };\n"
    "    key <AC08> { type = \"ALPHABETIC\", [ k, K ] };\n"
    "    key <AC09> { type = \"ALPHABETIC\", [ l, L ] };\n"
    "    key <AC10> { type = \"TWO_LEVEL\", [ semicolon, colon ] };\n"
    "    key <AC11> { type = \"TWO_LEVEL\", [ apostrophe, quotedbl ] };\n"
    "    key <RTRN> { type = \"ONE_LEVEL\", [ Return ] };\n"
    "    key <LFSH> { type = \"ONE_LEVEL\", repeat = No, [ Shift_L ] };\n"
    "    key <LSGT> { type = \"TWO_LEVEL\", [ backslash, bar ] };\n"
    "    key <AB01> { type = \"ALPHABETIC\", [ z, Z ] };\n"
    "    key <AB02> { type = \"ALPHABETIC\", [ x, X ] };\n"
    "    key <AB03> { type = \"ALPHABETIC\", [ c, C ] };\n"
    "    key <AB04> { type = \"ALPHABETIC\", [ v, V ] };\n"
    "    key <AB05> { type = \"ALPHABETIC\", [ b, B ] };\n"
    "    key <AB06> { type = \"ALPHABETIC\", [ n, N ] };\n"
    "    key <AB07> { type = \"ALPHABETIC\", [ m, M ] };\n"
    "    key <AB08> { type = \"TWO_LEVEL\", [ comma, less ] };\n"
    "    key <AB09> { type = \"TWO_LEVEL\", [ period, greater ] };\n"
    "    key <AB10> { type = \"TWO_LEVEL\", [ slash, question ] };\n"
    "    key <RTSH> { type = \"ONE_LEVEL\", repeat = No, [ Shift_R ] };\n"
    "    key <LCTL> { type = \"ONE_LEVEL\", repeat = No, [ Control_L ] };\n"
    "    key <LWIN> { type = \"ONE_LEVEL\", repeat = No, [ Super_L ] };\n"
    "    key <LALT> { type = \"ONE_LEVEL\", repeat = No, [ Alt_L ] };\n"
    "    key <SPCE> { type = \"ONE_LEVEL\", [ space ] };\n"
    "    key <RALT> { type = \"ONE_LEVEL\", repeat = No, [ Alt_R ] };\n"
    "    key <RWIN> { type = \"ONE_LEVEL\", repeat = No, [ Super_R ] };\n"
    "    key <COMP> { type = \"ONE_LEVEL\", [ Menu ] };\n"
    "    key <RCTL> { type = \"ONE_LEVEL\", repeat = No, [ Control_R ] };\n"
    "    key <INS> { type = \"ONE_LEVEL\", [ Insert ] };\n"
    "    key <HOME> { type = \"ONE_LEVEL\", [ Home ] };\n"
    "    key <PGUP> { type = \"ONE_LEVEL\", [ Prior ] };\n"
    "    key <DELE> { type = \"ONE_LEVEL\", [ Delete ] };\n"
    "    key <END> { type = \"ONE_LEVEL\", [ End ] };\n"
    "    key <PGDN> { type = \"ONE_LEVEL\", [ Next ] };\n"
    "    key <UP> { type = \"ONE_LEVEL\", [ Up ] };\n"
    "    key <LEFT> { type = \"ONE_LEVEL\", [ Left ] };\n"
    "    key <DOWN> { type = \"ONE_LEVEL\", [ Down ] };\n"
    "    key <RGHT> { type = \"ONE_LEVEL\", [ Right ] };\n"
    "    key <NMLK> { type = \"ONE_LEVEL\", repeat = No, [ Num_Lock ] };\n"
    "    key <KPDV> { type = \"ONE_LEVEL\", [ KP_Divide ] };\n"
    "    key <KPMU> { type = \"ONE_LEVEL\", [ KP_Multiply ] };\n"
    "    key <KPSU> { type = \"ONE_LEVEL\", [ KP_Subtract ] };\n"
    "    key <KP7> { type = \"KEYPAD\", [ KP_Home, KP_7 ] };\n"
    "    key <KP8> { type = \"KEYPAD\", [ KP_Up, KP_8 ] };\n"
    "    key <KP9> { type = \"KEYPAD\", [ KP_Prior, KP_9 ] };\n"
    "    key <KPAD> { type = \"ONE_LEVEL\", [ KP_Add ] };\n"
    "    key <KP4> { type = \"KEYPAD\", [ KP_Left, KP_4 ] };\n"
    "    key <KP5> { type = \"KEYPAD\", [ KP_Begin, KP_5 ] };\n"
    "    key <KP6> { type = \"KEYPAD\", [ KP_Right, KP_6 ] };\n"
    "    key <KP1> { type = \"KEYPAD\", [ KP_End, KP_1 ] };\n"
    "    key <KP2> { type = \"KEYPAD\", [ KP_Down, KP_2 ] };\n"
    "    key <KP3> { type = \"KEYPAD\", [ KP_Next, KP_3 ] };\n"
    "    key <KPEN> { type = \"ONE_LEVEL\", [ KP_Enter ] };\n"
    "    key <KP0> { type = \"KEYPAD\", [ KP_Insert, KP_0 ] };\n"
    "    key <KPDL> { type = \"KEYPAD\", [ KP_Delete, KP_Decimal ] };\n"
    "    key <KPEQ> { type = \"ONE_LEVEL\", [ KP_Equal ] };\n"
    "    key <POWR> { type = \"ONE_LEVEL\", [ XF86PowerOff ] };\n"
    "    key <FK13> { type = \"ONE_LEVEL\", [ F13 ] };\n"
    "    key <FK14> { type = \"ONE_LEVEL\", [ F14 ] };\n"
    "    key <FK15> { type = \"ONE_LEVEL\", [ F15 ] };\n"
    "    key <FK16> { type = \"ONE_LEVEL\", [ F16 ] };\n"
    "    key <FK17> { type = \"ONE_LEVEL\", [ F17 ] };\n"
    "    key <FK18> { type = \"ONE_LEVEL\", [ F18 ] };\n"
    "    key <FK19> { type = \"ONE_LEVEL\", [ F19 ] };\n"
    "    key <FK20> { type = \"ONE_LEVEL\", [ F20 ] };\n"
    "    key <FK21> { type = \"ONE_LEVEL\", [ F21 ] };\n"
    "    key <FK22> { type = \"ONE_LEVEL\", [ F22 ] };\n"
    "    key <FK23> { type = \"ONE_LEVEL\", [ F23 ] };\n"
    "    key <FK24> { type = \"ONE_LEVEL\", [ F24 ] };\n"
    "    key <MUTE> { type = \"ONE_LEVEL\", [ XF86AudioMute ] };\n"
    "    key <VOL+> { type = \"ONE_LEVEL\", [ XF86AudioRaiseVolume ] };\n"
    "    key <VOL-> { type = \"ONE_LEVEL\", [ XF86AudioLowerVolume ] };\n"
    "    modifier_map Shift { <LFSH>, <RTSH> };\n"
    "    modifier_map Lock { <CAPS> };\n"
    "    modifier_map Control { <LCTL>, <RCTL> };\n"
    "    modifier_map Mod1 { <LALT>, <RALT> };\n"
    "    modifier_map Mod2 { <NMLK> };\n"
    "    modifier_map Mod4 { <LWIN>, <RWIN> };\n"
    "};\n"
    "};\n";

const struct keymap keymap_us = {
    "us", "English (US)", keys, xkb, sizeof(xkb),
};
