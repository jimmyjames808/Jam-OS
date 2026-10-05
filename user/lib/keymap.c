/* libos: keyboard layouts (<keymap.h>): the lookups over the tables
 * tools/genkeymap.py makes (keymap_keys.c, keymap_<layout>.c). Pure
 * functions of read-only data. */
#include <keymap.h>
#include <os.h>

/* Every layout there is; a new one is a layout file, its generated table
 * and a line here. */
static const struct keymap *const layouts[] = { &keymap_us };

#define FIRST_LINE "// jamos-keymap "
#define LAYOUT_NAME  15   /* a layout name's characters (abi/keymap/us.txt) */

uint32_t keymap_evdev_of_hid(uint32_t usage)
{
    return usage < KEYMAP_HID_USAGES ? keymap_evdev_by_hid[usage] : 0;
}

uint32_t keymap_hid_of_evdev(uint32_t code)
{
    return code < KEYMAP_CODES ? keymap_hid_by_evdev[code] : 0;
}

uint32_t keymap_mods_of_hid(uint8_t hid_mods)
{
    /* hid's byte: bits 0-3 left Ctrl, Shift, Alt, GUI; 4-7 the right ones. */
    uint32_t both = (uint32_t)(hid_mods | hid_mods >> 4) & 0xf, m = 0;
    if (both & 1)
        m |= KEYMAP_MOD_CTRL;
    if (both & 2)
        m |= KEYMAP_MOD_SHIFT;
    if (both & 4)
        m |= KEYMAP_MOD_ALT;
    if (both & 8)
        m |= KEYMAP_MOD_SUPER;
    return m;
}

const struct keymap *keymap_by_name(const char *name)
{
    for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++)
        if (!strcmp(layouts[i]->name, name))
            return layouts[i];
    return NULL;
}

static bool name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

const struct keymap *keymap_of_xkb(const char *text, size_t len)
{
    size_t pre = sizeof(FIRST_LINE) - 1;
    if (!text || len < pre || memcmp(text, FIRST_LINE, pre) != 0)
        return NULL;
    char name[LAYOUT_NAME + 1];
    size_t n = 0;
    for (size_t i = pre; i < len && name_char(text[i]); i++) {
        if (n == LAYOUT_NAME)
            return NULL;
        name[n++] = text[i];
    }
    /* The name ends at the ':' (never at the end of the text). */
    if (!n || pre + n >= len || text[pre + n] != ':')
        return NULL;
    name[n] = 0;
    return keymap_by_name(name);
}

unsigned keymap_level(uint32_t type, uint32_t mods)
{
    bool shift = (mods & KEYMAP_MOD_SHIFT) != 0;
    switch (type) {
    case KEYMAP_TWO_LEVEL:
        return shift;
    case KEYMAP_ALPHABETIC:
        return shift != ((mods & KEYMAP_MOD_CAPS) != 0);
    case KEYMAP_KEYPAD:
        return (mods & KEYMAP_MOD_NUM) != 0;
    default:
        return 0;
    }
}

bool keymap_decode(const struct keymap *km, uint32_t code, uint32_t mods,
                   struct keymap_sym *out)
{
    if (code >= KEYMAP_CODES || km->keys[code].type == KEYMAP_NONE)
        return false;
    const struct keymap_key *k = &km->keys[code];
    unsigned l = keymap_level(k->type, mods);
    out->sym = k->sym[l];
    out->cp = k->cp[l];
    return true;
}

bool keymap_typing(const struct keymap *km, uint32_t cp, uint32_t *code, uint32_t *mods)
{
    if (!cp)
        return false;
    for (uint32_t c = 1; c < KEYMAP_CODES; c++) {
        const struct keymap_key *k = &km->keys[c];
        if (k->type == KEYMAP_NONE || k->type == KEYMAP_KEYPAD)
            continue;
        for (unsigned l = 0; l < 2; l++) {
            if (k->cp[l] != cp || (l == 1 && k->type == KEYMAP_ONE_LEVEL))
                continue;
            *code = c;
            *mods = l ? KEYMAP_MOD_SHIFT : 0;
            return true;
        }
    }
    return false;
}
