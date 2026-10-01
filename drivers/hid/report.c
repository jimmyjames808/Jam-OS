/* hid: the report descriptor parser, as far as a mouse needs it (USB HID
 * 1.11, section 6.2.2). A report descriptor is a list of items, each a
 * prefix byte (tag, type, data size 0/1/2/4) and its data:
 *
 *   - Global items set state that lasts until changed: Usage Page, Logical
 *     Minimum/Maximum, Report Size (bits per field), Report Count (fields),
 *     Report ID; Push and Pop save and restore all of it.
 *   - Local items describe the next main item only: Usage (a 4-byte one
 *     carries its own page), Usage Minimum/Maximum (a range).
 *   - Main items: Collection / End Collection group what is between them;
 *     Input (Output, Feature) adds Report Count fields of Report Size bits
 *     to the report of the current Report ID. An Input is Constant
 *     (padding) or Data, Array (a list of usages that are on, like a
 *     keyboard's keys) or Variable (one value per usage), Absolute or
 *     Relative (a change since the last report: a mouse's movement).
 *
 * What it finds: the first application collection whose usage is Generic
 * Desktop Mouse (or Pointer), and in it the Variable Data inputs of the
 * report that holds its relative X: buttons 1-3 (Button page, one bit
 * each), X, Y, Wheel (Generic Desktop 0x30, 0x31, 0x38, relative) and AC
 * Pan (Consumer 0x238, relative), each with its bit offset in that report,
 * its size and whether it is signed (Logical Minimum below 0). Input
 * fields are packed little-endian from bit 0, each report id counting its
 * own bits; a device that uses report ids at all prefixes every report
 * with its id byte.
 *
 * Not handled, as no mouse needs it: Delimiter (alternative usages; the
 * first is taken), usages whose page comes after them (a Usage takes the
 * Usage Page in force where it stands), Physical/Unit items (read, unused).
 * Fields wider than 32 bits are counted for the offsets but not decoded. */
#include "report.h"

#define TYPE_MAIN   0
#define TYPE_GLOBAL 1
#define TYPE_LOCAL  2

#define MAIN_INPUT          0x8
#define MAIN_OUTPUT         0x9
#define MAIN_COLLECTION     0xa
#define MAIN_FEATURE        0xb
#define MAIN_END_COLLECTION 0xc

#define GLOBAL_USAGE_PAGE   0x0
#define GLOBAL_LOGICAL_MIN  0x1
#define GLOBAL_LOGICAL_MAX  0x2
#define GLOBAL_REPORT_SIZE  0x7
#define GLOBAL_REPORT_ID    0x8
#define GLOBAL_REPORT_COUNT 0x9
#define GLOBAL_PUSH         0xa
#define GLOBAL_POP          0xb

#define LOCAL_USAGE     0x0
#define LOCAL_USAGE_MIN 0x1
#define LOCAL_USAGE_MAX 0x2

#define LONG_ITEM 0xfe

#define INPUT_CONSTANT 0x01
#define INPUT_VARIABLE 0x02
#define INPUT_RELATIVE 0x04

#define COLLECTION_APPLICATION 0x01

/* Usages, page in the high 16 bits. */
#define U_GD_POINTER 0x00010001u
#define U_GD_MOUSE   0x00010002u
#define U_GD_X       0x00010030u
#define U_GD_Y       0x00010031u
#define U_GD_WHEEL   0x00010038u
#define U_AC_PAN     0x000c0238u
#define PAGE_BUTTON  0x0009u

/* The global items (what Push saves). */
struct globals {
    uint32_t page;        /* Usage Page */
    int32_t  lmin;        /* Logical Minimum */
    uint32_t size;        /* Report Size */
    uint32_t count;       /* Report Count */
    uint32_t id;          /* Report ID, 0 before the first */
};

/* A field the mouse might use, with the report it is in. */
struct found {
    struct rd_field f;    /* where it is */
    uint32_t        id;   /* its report id */
};

/* Where the walk is. */
struct parse {
    struct globals g;                    /* the global items in force */
    struct globals stack[RD_MAX_PUSH];   /* Push's saved copies */
    uint32_t nstack;                     /* how many */
    uint32_t usages[RD_MAX_USAGES];      /* the Usage items since the last main item */
    uint32_t nusages;                    /* how many kept */
    uint32_t umin, umax;                 /* Usage Minimum/Maximum, with their pages */
    bool     have_min, have_max;         /* ... given since the last main item */
    uint32_t depth;                      /* open collections */
    uint32_t mouse_depth;                /* depth of the mouse collection's content, 0: outside */
    bool     mouse_seen;                 /* the mouse collection was found (only the first counts) */
    bool     uses_ids;                   /* a Report ID item appeared */
    uint16_t bits[256];                  /* input bits so far, per report id (saturates) */
    struct found button[3], x, y, wheel, pan;   /* the mouse's fields, first of each */
    uint8_t  nbuttons_by_id[256];        /* one-bit Button-page inputs, per report id */
};

/* The item's data as an unsigned and a sign-extended value. */
static uint32_t data_u(const uint8_t *p, uint32_t size)
{
    uint32_t v = 0;
    for (uint32_t k = 0; k < size; k++)
        v |= (uint32_t)p[k] << (8 * k);
    return v;
}

static int32_t data_s(const uint8_t *p, uint32_t size)
{
    uint32_t v = data_u(p, size);
    if (size == 1)
        return (int8_t)v;
    if (size == 2)
        return (int16_t)v;
    return (int32_t)v;
}

/* A usage as a 32-bit page:id; a 4-byte item names its own page. */
static uint32_t full_usage(const struct parse *s, uint32_t v, uint32_t size)
{
    return size == 4 ? v : (s->g.page & 0xffff) << 16 | (v & 0xffff);
}

static void clear_locals(struct parse *s)
{
    s->nusages = 0;
    s->have_min = s->have_max = false;
}

/* The usage of field i of the main item being parsed (0: none). */
static uint32_t usage_of(const struct parse *s, uint32_t i)
{
    if (s->nusages)
        return s->usages[i < s->nusages ? i : s->nusages - 1];
    if (s->have_min) {
        /* page:id + i; fields past Usage Maximum get the maximum (a wrap
         * past 0xffffffff too: it is only a usage value, never an index) */
        uint32_t u = s->umin + i;
        if (s->have_max && (u > s->umax || u < s->umin))
            u = s->umax;
        return u;
    }
    return 0;
}

/* Keep f as `which` unless an earlier field already is. */
static void take(struct found *which, const struct parse *s, uint32_t bit, bool is_signed)
{
    if (which->f.size)
        return;
    which->f = (struct rd_field){ (uint16_t)bit, (uint8_t)s->g.size, is_signed };
    which->id = s->g.id;
}

/* One field of a Variable Data input in the mouse collection. */
static void mouse_field(struct parse *s, uint32_t usage, uint32_t bit, bool relative)
{
    bool is_signed = s->g.lmin < 0;
    if (usage >> 16 == PAGE_BUTTON && s->g.size == 1) {
        uint32_t b = usage & 0xffff;
        if (s->nbuttons_by_id[s->g.id] < 255)
            s->nbuttons_by_id[s->g.id]++;
        if (b >= 1 && b <= 3)
            take(&s->button[b - 1], s, bit, false);
        return;
    }
    if (!relative)
        return;   /* absolute X/Y: a tablet or touch screen, not a mouse */
    if (usage == U_GD_X)
        take(&s->x, s, bit, is_signed);
    else if (usage == U_GD_Y)
        take(&s->y, s, bit, is_signed);
    else if (usage == U_GD_WHEEL)
        take(&s->wheel, s, bit, is_signed);
    else if (usage == U_AC_PAN)
        take(&s->pan, s, bit, is_signed);
}

/* An Input item: its fields go to the current report id's bits. */
static enum rd_status input(struct parse *s, uint32_t flags)
{
    if (s->uses_ids && !s->g.id)
        return RD_BAD_REPORT_ID;   /* fields before the first Report ID */
    uint32_t at = s->bits[s->g.id];
    bool mouse = s->mouse_depth && (flags & (INPUT_CONSTANT | INPUT_VARIABLE)) == INPUT_VARIABLE;
    bool decodable = s->g.size >= 1 && s->g.size <= RD_MAX_FIELD_BITS;
    for (uint32_t i = 0; mouse && decodable && i < s->g.count; i++) {
        uint32_t bit = at + i * s->g.size;   /* < 4096 * 33: no wrap */
        if (bit + s->g.size > RD_MAX_REPORT_BITS)
            break;
        mouse_field(s, usage_of(s, i), bit, flags & INPUT_RELATIVE);
    }
    uint32_t end = at + s->g.size * s->g.count;   /* <= 8184 + 256 * 4096: no wrap */
    s->bits[s->g.id] = (uint16_t)(end < RD_MAX_REPORT_BITS ? end : RD_MAX_REPORT_BITS);
    return RD_OK;
}

static enum rd_status collection(struct parse *s, uint32_t type)
{
    if (s->depth == RD_MAX_DEPTH)
        return RD_TOO_DEEP;
    uint32_t usage = usage_of(s, 0);
    s->depth++;
    if (s->depth == 1 && type == COLLECTION_APPLICATION && !s->mouse_seen &&
        (usage == U_GD_MOUSE || usage == U_GD_POINTER)) {
        s->mouse_seen = true;
        s->mouse_depth = 1;
    }
    return RD_OK;
}

static enum rd_status main_item(struct parse *s, uint32_t tag, uint32_t v)
{
    enum rd_status st = RD_OK;
    if (tag == MAIN_INPUT) {
        st = input(s, v);
    } else if (tag == MAIN_COLLECTION) {
        st = collection(s, v & 0xff);
    } else if (tag == MAIN_END_COLLECTION) {
        if (!s->depth)
            return RD_UNBALANCED;
        if (--s->depth < s->mouse_depth)
            s->mouse_depth = 0;
    } else if ((tag == MAIN_OUTPUT || tag == MAIN_FEATURE) && s->uses_ids && !s->g.id) {
        st = RD_BAD_REPORT_ID;
    }
    clear_locals(s);   /* local items last until the next main item */
    return st;
}

static enum rd_status global_item(struct parse *s, uint32_t tag, const uint8_t *p,
                                  uint32_t size)
{
    uint32_t v = data_u(p, size);
    switch (tag) {
    case GLOBAL_USAGE_PAGE:  s->g.page = v; break;
    case GLOBAL_LOGICAL_MIN: s->g.lmin = data_s(p, size); break;
    case GLOBAL_LOGICAL_MAX: break;   /* the minimum alone says signed or not */
    case GLOBAL_REPORT_SIZE:
        if (v > RD_MAX_REPORT_SIZE)
            return RD_BAD_SIZE;
        s->g.size = v;
        break;
    case GLOBAL_REPORT_COUNT:
        if (v > RD_MAX_REPORT_COUNT)
            return RD_BAD_SIZE;
        s->g.count = v;
        break;
    case GLOBAL_REPORT_ID:
        if (v == 0 || v > 255)
            return RD_BAD_REPORT_ID;
        s->g.id = v;
        s->uses_ids = true;
        break;
    case GLOBAL_PUSH:
        if (s->nstack == RD_MAX_PUSH)
            return RD_TOO_DEEP;
        s->stack[s->nstack++] = s->g;
        break;
    case GLOBAL_POP:
        if (!s->nstack)
            return RD_UNBALANCED;
        s->g = s->stack[--s->nstack];
        break;
    }
    return RD_OK;
}

static void local_item(struct parse *s, uint32_t tag, uint32_t v, uint32_t size)
{
    if (tag == LOCAL_USAGE && s->nusages < RD_MAX_USAGES) {
        s->usages[s->nusages++] = full_usage(s, v, size);
    } else if (tag == LOCAL_USAGE_MIN) {
        s->umin = full_usage(s, v, size);
        s->have_min = true;
    } else if (tag == LOCAL_USAGE_MAX) {
        s->umax = full_usage(s, v, size);
        s->have_max = true;
    }
}

/* Every item of d; RD_OK if the walk got to the end. */
static enum rd_status walk(struct parse *s, const uint8_t *d, uint32_t n)
{
    uint32_t items = 0;
    for (uint32_t i = 0; i < n;) {
        if (++items > RD_MAX_ITEMS)
            return RD_TOO_MANY_ITEMS;
        uint8_t b = d[i];
        if (b == LONG_ITEM) {   /* 0xfe, data size, long tag, data: skipped */
            if (n - i < 3 || n - i - 3 < d[i + 1])
                return RD_TRUNCATED;
            i += 3u + d[i + 1];
            continue;
        }
        uint32_t size = (b & 3) == 3 ? 4 : b & 3, tag = b >> 4, type = (b >> 2) & 3;
        if (n - i - 1 < size)
            return RD_TRUNCATED;
        const uint8_t *p = d + i + 1;
        enum rd_status st = RD_OK;
        if (type == TYPE_MAIN)
            st = main_item(s, tag, data_u(p, size));
        else if (type == TYPE_GLOBAL)
            st = global_item(s, tag, p, size);
        else if (type == TYPE_LOCAL)
            local_item(s, tag, data_u(p, size), size);
        if (st != RD_OK)
            return st;
        i += 1 + size;
    }
    return s->depth ? RD_UNBALANCED : RD_OK;
}

/* f if it is in report `id`, else "none". */
static struct rd_field in_report(const struct found *f, uint32_t id)
{
    return f->f.size && f->id == id ? f->f : (struct rd_field){ 0 };
}

enum rd_status hid_rd_parse(const uint8_t *d, uint32_t n, struct rd_mouse *out)
{
    if (!n)
        return RD_EMPTY;
    if (n > RD_MAX_BYTES)
        return RD_TOO_LONG;
    static struct parse s;   /* ~1.3 KiB, off the stack: not reentrant (hid has one thread) */
    s = (struct parse){ 0 };
    enum rd_status st = walk(&s, d, n);
    if (st != RD_OK)
        return st;
    if (!s.mouse_seen)
        return RD_NO_MOUSE;
    if (!s.x.f.size || !s.y.f.size || s.x.id != s.y.id)
        return RD_NO_XY;
    uint32_t id = s.x.id;
    if (s.uses_ids && !id)
        return RD_BAD_REPORT_ID;   /* the device prefixes reports with ids, but not this one */
    struct rd_mouse m = {
        .report_id = (uint8_t)id, .bits = s.bits[id], .nbuttons = s.nbuttons_by_id[id],
        .x = s.x.f, .y = s.y.f, .wheel = in_report(&s.wheel, id), .pan = in_report(&s.pan, id),
    };
    for (unsigned k = 0; k < 3; k++)
        m.button[k] = in_report(&s.button[k], id);
    *out = m;
    return RD_OK;
}

const char *hid_rd_status_str(enum rd_status st)
{
    static const char *const names[] = {
        [RD_OK] = "ok", [RD_EMPTY] = "empty", [RD_TOO_LONG] = "too long",
        [RD_TRUNCATED] = "truncated", [RD_TOO_MANY_ITEMS] = "too many items",
        [RD_TOO_DEEP] = "nested too deep", [RD_UNBALANCED] = "unbalanced",
        [RD_BAD_REPORT_ID] = "bad report id", [RD_BAD_SIZE] = "report size or count too big",
        [RD_NO_MOUSE] = "no mouse collection", [RD_NO_XY] = "no relative x and y",
    };
    return (unsigned)st < sizeof(names) / sizeof(names[0]) && names[st] ? names[st] : "?";
}

/* ---- decoding a report ---------------------------------------------------------- */

/* Field f of payload p (n bytes): 0 if absent or not all inside p. */
static int32_t field(const struct rd_field *f, const uint8_t *p, uint32_t n)
{
    if (!f->size || (uint64_t)f->bit + f->size > 8ull * n)
        return 0;
    uint32_t first = f->bit / 8, last = (f->bit + f->size - 1) / 8;   /* at most 5 bytes */
    uint64_t v = 0;
    for (uint32_t k = first; k <= last; k++)
        v |= (uint64_t)p[k] << (8 * (k - first));
    v = (v >> (f->bit % 8)) & ((1ull << f->size) - 1);   /* size <= 32 */
    if (f->is_signed && (v >> (f->size - 1)) & 1)
        v |= ~0ull << f->size;   /* sign-extend */
    return (int32_t)(int64_t)v;
}

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

enum rd_decoded hid_rd_decode(const struct rd_mouse *m, const uint8_t *r, uint32_t n,
                              struct rd_move *out)
{
    if (m->report_id) {
        if (!n || r[0] != m->report_id)
            return RD_OTHER_ID;
        r++;
        n--;
    }
    uint64_t have = 8ull * n;
    if ((uint64_t)m->x.bit + m->x.size > have || (uint64_t)m->y.bit + m->y.size > have)
        return RD_SHORT;
    uint8_t buttons = 0;
    for (unsigned k = 0; k < 3; k++)
        buttons |= (uint8_t)((field(&m->button[k], r, n) & 1) << k);
    *out = (struct rd_move){
        .dx = (int16_t)clamp(field(&m->x, r, n), -32768, 32767),
        .dy = (int16_t)clamp(field(&m->y, r, n), -32768, 32767),
        .wheel = (int8_t)clamp(field(&m->wheel, r, n), -127, 127),
        .pan = (int8_t)clamp(field(&m->pan, r, n), -127, 127),
        .buttons = buttons,
    };
    return RD_MOVE;
}
