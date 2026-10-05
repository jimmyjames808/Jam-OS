/* utest: keyboard layouts (<keymap.h>, the tables tools/genkeymap.py makes
 * from abi/keymap/): HID usages to evdev codes and back; every key of a
 * 105-key board in the US layout, at both levels, with the modifiers and
 * locks; the inverse that serial text is typed with; the XKB text's first
 * line; and the US table against the hid driver itself (drv/hid as a
 * process against hidmock.c's mock usb-bus and console): every key it
 * gives a character today, in every state of Shift, Caps Lock and Num
 * Lock, must give the same character from the table. */
#define CHECK_PROG "utest"
#define CHECK_CUR  kcur
#include <check.h>
#include <keymap.h>
#include <os.h>
#include "hidmock.h"
#include "utest.h"

static const char *kcur;   /* the keymap test running */

/* Usages of a 105-key board (and of a 104-key one): 0x04 .. 0x65 and the
 * eight modifiers. 0x31 and 0x32 (US \ and the ISO # key) are one key
 * code, so they make 105 codes. */
static bool on_board(uint32_t u)
{
    return (u >= 0x04 && u <= 0x65) || (u >= 0xe0 && u <= 0xe7);
}

bool t_keymap_hid_codes(void)
{
    kcur = "keymap_hid_codes";
    static bool seen[KEYMAP_CODES];
    memset(seen, 0, sizeof(seen));
    unsigned codes = 0;
    for (uint32_t u = 0; u < KEYMAP_HID_USAGES; u++) {
        uint32_t c = keymap_evdev_of_hid(u);
        if (!on_board(u))
            continue;
        if (!c)
            FAIL("usage %#x has no evdev code", u);
        if (keymap_hid_of_evdev(c) != u && !(u == 0x32 && keymap_hid_of_evdev(c) == 0x31))
            FAIL("code %u goes back to usage %#x, not %#x", c, keymap_hid_of_evdev(c), u);
        codes += !seen[c];
        seen[c] = true;
    }
    CHECK_EQ(codes, 105);
    /* Linux's numbers for a few (input-event-codes.h). */
    CHECK_EQ(keymap_evdev_of_hid(0x04), 30);    /* KEY_A */
    CHECK_EQ(keymap_evdev_of_hid(0x29), 1);     /* KEY_ESC */
    CHECK_EQ(keymap_evdev_of_hid(0x28), 28);    /* KEY_ENTER */
    CHECK_EQ(keymap_evdev_of_hid(0x46), 99);    /* KEY_SYSRQ */
    CHECK_EQ(keymap_evdev_of_hid(0x52), 103);   /* KEY_UP */
    CHECK_EQ(keymap_evdev_of_hid(0x64), 86);    /* KEY_102ND */
    CHECK_EQ(keymap_evdev_of_hid(0x65), 127);   /* KEY_COMPOSE */
    CHECK_EQ(keymap_evdev_of_hid(0xe3), 125);   /* KEY_LEFTMETA */
    CHECK_EQ(keymap_evdev_of_hid(0x73), 194);   /* KEY_F24 */
    CHECK_EQ(keymap_evdev_of_hid(0x32), keymap_evdev_of_hid(0x31));
    for (uint32_t u = 0; u < 4; u++)   /* none, ErrorRollOver, POSTFail, ErrorUndefined */
        CHECK_EQ(keymap_evdev_of_hid(u), 0);
    CHECK_EQ(keymap_evdev_of_hid(0xe8), 0);
    CHECK_EQ(keymap_evdev_of_hid(0x10000), 0);
    CHECK_EQ(keymap_hid_of_evdev(0), 0);
    CHECK_EQ(keymap_hid_of_evdev(KEYMAP_CODES), 0);
    CHECK_EQ(keymap_mods_of_hid(0x02), KEYMAP_MOD_SHIFT);
    CHECK_EQ(keymap_mods_of_hid(0x20), KEYMAP_MOD_SHIFT);
    CHECK_EQ(keymap_mods_of_hid(0x11), KEYMAP_MOD_CTRL);
    CHECK_EQ(keymap_mods_of_hid(0x44), KEYMAP_MOD_ALT);
    CHECK_EQ(keymap_mods_of_hid(0x88), KEYMAP_MOD_SUPER);
    CHECK_EQ(keymap_mods_of_hid(0xff),
             KEYMAP_MOD_SHIFT | KEYMAP_MOD_CTRL | KEYMAP_MOD_ALT | KEYMAP_MOD_SUPER);
    return true;
}

/* What usage u types on the US layout with mods, or 0xffffffff for no key. */
static uint32_t us_cp(uint32_t u, uint32_t mods)
{
    struct keymap_sym s;
    return keymap_decode(&keymap_us, keymap_evdev_of_hid(u), mods, &s) ? s.cp : 0xffffffffu;
}

static uint32_t us_sym(uint32_t u, uint32_t mods)
{
    struct keymap_sym s;
    return keymap_decode(&keymap_us, keymap_evdev_of_hid(u), mods, &s) ? s.sym : 0xffffffffu;
}

#define SH   KEYMAP_MOD_SHIFT
#define CAPS KEYMAP_MOD_CAPS
#define NUM  KEYMAP_MOD_NUM

bool t_keymap_us_keys(void)
{
    kcur = "keymap_us_keys";
    for (uint32_t u = 0; u < KEYMAP_HID_USAGES; u++)
        if (on_board(u) && us_sym(u, 0) == 0xffffffffu)
            FAIL("usage %#x is no key of the US layout", u);
    /* Letters: Shift or Caps Lock, not both. Ctrl, Alt, Super change nothing. */
    CHECK_EQ(us_cp(0x04, 0), 'a');
    CHECK_EQ(us_cp(0x04, SH), 'A');
    CHECK_EQ(us_cp(0x04, CAPS), 'A');
    CHECK_EQ(us_cp(0x04, SH | CAPS), 'a');
    CHECK_EQ(us_cp(0x06, KEYMAP_MOD_CTRL), 'c');
    CHECK_EQ(us_cp(0x1d, KEYMAP_MOD_ALT | KEYMAP_MOD_SUPER | NUM), 'z');
    /* The digit row and symbols: Shift only. */
    CHECK_EQ(us_cp(0x1e, 0), '1');
    CHECK_EQ(us_cp(0x1e, SH), '!');
    CHECK_EQ(us_cp(0x1e, CAPS), '1');
    CHECK_EQ(us_cp(0x35, SH), '~');
    CHECK_EQ(us_cp(0x34, SH), '"');
    CHECK_EQ(us_cp(0x64, SH), '|');
    /* The keypad: Num Lock alone picks the digit. */
    CHECK_EQ(us_cp(0x5f, NUM), '7');
    CHECK_EQ(us_cp(0x5f, NUM | SH), '7');
    CHECK_EQ(us_cp(0x5f, 0), 0);
    CHECK_EQ(us_sym(0x5f, 0), 0xff95);   /* KP_Home */
    CHECK_EQ(us_sym(0x5f, NUM), 0xffb7);   /* KP_7 */
    CHECK_EQ(us_cp(0x63, NUM), '.');
    CHECK_EQ(us_cp(0x55, 0), '*');
    CHECK_EQ(us_cp(0x58, 0), '\n');
    /* The control keys. */
    CHECK_EQ(us_cp(0x28, 0), '\n');
    CHECK_EQ(us_cp(0x2b, SH), '\t');
    CHECK_EQ(us_sym(0x2b, SH), 0xfe20);   /* ISO_Left_Tab */
    CHECK_EQ(us_cp(0x2a, 0), '\b');
    CHECK_EQ(us_cp(0x29, 0), 0x1b);
    CHECK_EQ(us_cp(0x2c, SH), ' ');
    CHECK_EQ(us_cp(0x4c, 0), 0);          /* Delete types nothing */
    CHECK_EQ(us_sym(0x3a, 0), 0xffbe);    /* F1 */
    CHECK_EQ(us_sym(0xe1, 0), 0xffe1);    /* Shift_L */
    /* Repeat: everything but the modifiers and the lock keys. */
    for (uint32_t u = 0x04; u < KEYMAP_HID_USAGES; u++) {
        uint32_t c = keymap_evdev_of_hid(u);
        if (!c)
            continue;
        bool want = u < 0xe0 && u != 0x39 && u != 0x47 && u != 0x53;
        if (keymap_us.keys[c].repeats != want)
            FAIL("usage %#x repeats %d, want %d", u, keymap_us.keys[c].repeats, want);
    }
    struct keymap_sym s = { 7, 7 };
    CHECK(!keymap_decode(&keymap_us, 0, 0, &s));
    CHECK(!keymap_decode(&keymap_us, 84, 0, &s));   /* no key has 84 */
    CHECK(!keymap_decode(&keymap_us, KEYMAP_CODES, 0, &s));
    CHECK(s.sym == 7 && s.cp == 7);   /* untouched */
    return true;
}

bool t_keymap_typing(void)
{
    kcur = "keymap_typing";
    static const uint32_t controls[] = { '\n', '\t', '\b', 0x1b };
    for (uint32_t cp = 0x20; cp < 0x7f + 4; cp++) {
        uint32_t want = cp < 0x7f ? cp : controls[cp - 0x7f], code, mods;
        if (!keymap_typing(&keymap_us, want, &code, &mods))
            FAIL("nothing types %#x", want);
        struct keymap_sym s;
        CHECK(keymap_decode(&keymap_us, code, mods, &s));
        if (s.cp != want)
            FAIL("%#x: code %u mods %#x types %#x", want, code, mods, s.cp);
        CHECK(mods == 0 || mods == KEYMAP_MOD_SHIFT);
        CHECK(keymap_us.keys[code].type != KEYMAP_KEYPAD);
    }
    uint32_t code = 99, mods = 99;
    CHECK(keymap_typing(&keymap_us, 'Q', &code, &mods));
    CHECK(code == 16 && mods == KEYMAP_MOD_SHIFT);   /* KEY_Q */
    CHECK(keymap_typing(&keymap_us, '7', &code, &mods));
    CHECK(code == 8 && mods == 0);                   /* the digit row's, not the keypad's */
    CHECK(keymap_typing(&keymap_us, '\n', &code, &mods));
    CHECK(code == 28 && mods == 0);                  /* Return, not the keypad's Enter */
    code = mods = 99;
    CHECK(!keymap_typing(&keymap_us, 0, &code, &mods));
    CHECK(!keymap_typing(&keymap_us, 0x7f, &code, &mods));
    CHECK(!keymap_typing(&keymap_us, 0xe9, &code, &mods));   /* e acute: not on US */
    CHECK(code == 99 && mods == 99);
    return true;
}

bool t_keymap_xkb_text(void)
{
    kcur = "keymap_xkb_text";
    const char *x = keymap_us.xkb;
    size_t n = strlen(x);
    CHECK_EQ(keymap_us.xkb_size, n + 1);
    CHECK(!strncmp(x, "// jamos-keymap us: ", 20));
    CHECK(strstr(x, "\nxkb_keymap {\n") && strstr(x, "xkb_keycodes ") && strstr(x, "xkb_types ") &&
          strstr(x, "xkb_compatibility ") && strstr(x, "xkb_symbols "));
    CHECK(strstr(x, "    <AC01> = 38;\n"));   /* KEY_A + 8 */
    CHECK(strstr(x, "    key <AC01> { type = \"ALPHABETIC\", [ a, A ] };\n"));
    CHECK(keymap_of_xkb(x, n) == &keymap_us);
    CHECK(keymap_by_name("us") == &keymap_us);
    CHECK(!keymap_by_name("") && !keymap_by_name("uk") && !keymap_by_name("US"));
    static const char *const bad[] = {
        "// jamos-keymap uk: English (UK)\n", "// jamos-keymap us\n", "// jamos-keymap us",
        "// jamos-keymap : x\n", "// jamos-keymap US: x\n", "xkb_keymap {\n", "",
        "// jamos-keymap abcdefghijklmnopq: x\n", "//jamos-keymap us: x\n",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (keymap_of_xkb(bad[i], strlen(bad[i])))
            FAIL("\"%s\" was taken for a keymap of ours", bad[i]);
    CHECK(!keymap_of_xkb(x, 18));   /* cut off before the ':' */
    CHECK(!keymap_of_xkb(NULL, 0));
    return true;
}

/* ---- against the hid driver ---------------------------------------------------- */

#define LSHIFT 0x02
#define U_CAPS 0x39
#define U_SCRL 0x47
#define U_NUM  0x53
#define U_LAST 0x67   /* the keypad's =: the last usage hid gives a character */

static bool report(struct mock *m, uint8_t mods, uint8_t key)
{
    const uint8_t r[8] = { mods, 0, key, 0, 0, 0, 0, 0 };
    CHECK_ST(jam_channel_write(m->reports, r, sizeof(r), NULL, 0), OK);
    return true;
}

/* A lock key pressed and let go; its two events wait to be read. */
static bool toggle(struct mock *m, uint8_t key)
{
    unsigned want = m->nev + 2;
    if (!report(m, 0, key) || !report(m, 0, 0))
        return false;
    if (!mock_pump(m, now() + 10 * NS_PER_S, mock_have_events, want))
        FAIL("the lock key %#x: %u events, want %u", key, m->nev, want);
    return true;
}

/* mock_pump's condition: at least n events that are not REPEATs (a slow
 * run may add some; they are skipped). */
static bool have_events(struct mock *m, unsigned n)
{
    unsigned k = 0;
    for (unsigned i = 0; i < m->nev; i++)
        k += m->ev[i].kind != EV_KEY || m->ev[i].state != INPUT_KEY_REPEAT;
    return k >= n;
}

static bool tested(uint32_t u)
{
    return u != U_CAPS && u != U_SCRL && u != U_NUM;
}

/* hid's key for a keypad key with Num Lock off is the key printed on it:
 * our keysym KP_Home .. KP_End is Home .. End's + 0x45; KP_Insert,
 * KP_Delete are Insert's and Delete's. */
static uint32_t nav_of(uint32_t kp_sym)
{
    if (kp_sym >= 0xff95 && kp_sym <= 0xff9c)
        return kp_sym - 0x45;
    return kp_sym == 0xff9e ? 0xff63 : kp_sym == 0xff9f ? 0xffff : 0;
}

/* One state: every key typed with these modifiers and locks; each DOWN
 * hid sends must carry the table's character. */
static bool pass(struct mock *m, bool shift, uint32_t locks)
{
    m->nev = 0;
    unsigned want = shift ? 2 : 0;   /* Shift's own down and up */
    for (uint32_t u = 0x04; u <= U_LAST; u++) {
        if (!tested(u))
            continue;
        if (!report(m, shift ? LSHIFT : 0, (uint8_t)u) || !report(m, shift ? LSHIFT : 0, 0))
            return false;
        want += 2;
    }
    if (!report(m, 0, 0))
        return false;
    if (!mock_pump(m, now() + 20 * NS_PER_S, have_events, want))
        FAIL("%u events, want %u", m->nev, want);
    uint32_t mods = (shift ? KEYMAP_MOD_SHIFT : 0) | locks;
    uint32_t u = 0x04;
    for (unsigned i = 0; i < m->nev; i++) {
        const struct ev *e = &m->ev[i];
        if (e->kind != EV_KEY || e->state != INPUT_KEY_DOWN || e->usage >= 0xe0)
            continue;
        while (!tested(u))
            u++;
        uint32_t cp = us_cp(u, mods), sym = us_sym(u, mods);
        if (e->cp != cp)
            FAIL("usage %#x mods %#x: hid types %#x, the table %#x", u, mods, e->cp, cp);
        if (e->usage != u && nav_of(sym) != us_sym(e->usage, 0))
            FAIL("usage %#x: hid sends it as %#x, the table says keysym %#x", u, e->usage, sym);
        u++;
    }
    if (u != U_LAST + 1)
        FAIL("hid's DOWNs ended at usage %#x", u);
    return true;
}

bool t_keymap_matches_hid(void)
{
    kcur = "keymap_matches_hid";
    static struct mock m;
    if (!mock_start(&m, &dev_kbd, 0, true, true))
        return false;
    /* hid starts with Num Lock on and Caps Lock off. */
    static const struct { uint8_t key; uint32_t locks; } states[] = {
        { 0, NUM }, { U_CAPS, NUM | CAPS }, { U_NUM, CAPS }, { U_CAPS, 0 },
    };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        if (states[i].key && !toggle(&m, states[i].key))
            return false;
        if (!pass(&m, false, states[i].locks) || !pass(&m, true, states[i].locks))
            return false;
    }
    mock_unplug(&m, true, true);
    return mock_finish(&m, 0);
}
