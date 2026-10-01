/* hid: the report descriptor parser's self-test: recorded and made-up
 * descriptors with the layout (or the refusal) hid_rd_parse must give, and
 * reports with what hid_rd_decode must make of them. hid runs the fixtures
 * at every mouse start (a failure keeps that mouse in the boot protocol);
 * utest runs them with the fuzzing too (`drv/hid selftest`).
 *
 * The mice: QEMU's usb-mouse (as its boot log dumps it); a gaming mouse in
 * report protocol, 16 buttons, 16-bit X/Y, a wheel and AC Pan, no report
 * ids; a receiver's interface with a keyboard (id 1), the mouse (id 2,
 * 12-bit X/Y) and consumer control (id 3); the boot mouse of HID 1.11
 * appendix B.2 (3 bytes, no wheel). Not mice: a boot keyboard, a
 * keyboard's media interface, a tablet (absolute X/Y). Broken ones: cut
 * short, nested too deep, unbalanced, a report id of 0, sizes and counts
 * past the limits, fields before the first report id, X and Y in two
 * reports, an X past the longest report. A mouse from another PC is added
 * by pasting its `report descriptor` log lines here with the layout it
 * should get.
 *
 * The fuzzing parses every fixture cut at every length and with seeded
 * random byte changes; a layout that comes out must lie inside its
 * report, and decoding reports of every length with it must not fail. */
#include <jam/driver.h>
#include "report.h"

/* ---- descriptors ---------------------------------------------------------------- */

/* QEMU's usb-mouse (0627:0001), from its boot log (QEMU 10): 5 buttons,
 * X, Y and the wheel in 8 bits each. */
static const uint8_t qemu_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x03, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81,
    0x25, 0x7f, 0x75, 0x08, 0x95, 0x03, 0x81, 0x06, 0xc0, 0xc0,
};

/* A gaming mouse: 16 buttons, X/Y 16 bits (-32767..32767), wheel, AC Pan. */
static const uint8_t gaming16[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,   /* GD Mouse { Pointer { */
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01,   /* buttons 1-16 */
    0x95, 0x10, 0x75, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x16, 0x01, 0x80, 0x26, 0xff, 0x7f, 0x75, 0x10,   /* X, Y: 16 bits, rel */
    0x95, 0x02, 0x09, 0x30, 0x09, 0x31, 0x81, 0x06,
    0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x09, 0x38,   /* wheel */
    0x81, 0x06,
    0x05, 0x0c, 0x0a, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06,         /* AC Pan */
    0xc0, 0xc0,
};

/* A receiver: keyboard id 1, mouse id 2 (12-bit X/Y), consumer id 3. */
static const uint8_t receiver[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7,
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08,
    0x81, 0x01, 0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02,
    0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xff,
    0x00, 0x05, 0x07, 0x19, 0x00, 0x2a, 0xff, 0x00, 0x81, 0x00, 0xc0,
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09,
    0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x95, 0x10, 0x75, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x16, 0x01, 0xf8, 0x26, 0xff, 0x07, 0x75, 0x0c, 0x95, 0x02, 0x09, 0x30,
    0x09, 0x31, 0x81, 0x06, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01, 0x09, 0x38,
    0x81, 0x06, 0x05, 0x0c, 0x0a, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0,
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x03, 0x75, 0x10, 0x95, 0x02, 0x15, 0x01,
    0x26, 0xff, 0x02, 0x19, 0x01, 0x2a, 0xff, 0x02, 0x81, 0x00, 0xc0,
};

/* HID 1.11 appendix B.2's boot mouse: 3 buttons, X, Y; no wheel. */
static const uint8_t boot_mouse[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0,
};

/* A tablet: absolute X/Y (0..32767), relative wheel. Not a mouse here. */
static const uint8_t tablet[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01,
    0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x00, 0x26, 0xff,
    0x7f, 0x75, 0x10, 0x95, 0x02, 0x81, 0x02, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7f, 0x75,
    0x08, 0x95, 0x01, 0x81, 0x06, 0xc0, 0xc0,
};

/* The boot keyboard of HID 1.11 appendix B.1. */
static const uint8_t keyboard[] = {
    0x05, 0x01, 0x09, 0x06, 0xa1, 0x01, 0x05, 0x07, 0x19, 0xe0, 0x29, 0xe7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01,
    0x75, 0x03, 0x91, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
    0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xc0,
};

/* A keyboard's second interface: system control id 2, consumer id 3,
 * vendor id 6 (a long item in it too). */
static const uint8_t media[] = {
    0x05, 0x01, 0x09, 0x80, 0xa1, 0x01, 0x85, 0x02, 0x19, 0x81, 0x29, 0x83, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x95, 0x05, 0x81, 0x01, 0xc0,
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x03, 0x19, 0x00, 0x2a, 0x3c, 0x02, 0x15,
    0x00, 0x26, 0x3c, 0x02, 0x95, 0x01, 0x75, 0x10, 0x81, 0x00, 0xc0,
    0x06, 0x00, 0xff, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x06, 0xfe, 0x02, 0x10, 0xaa, 0xbb,
    0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x07, 0x81, 0x02, 0xc0,
};

/* Broken ones. */
static const uint8_t end_without_start[] = { 0x05, 0x01, 0xc0 };
static const uint8_t pop_without_push[] = { 0x05, 0x01, 0xb4 };
static const uint8_t push_too_deep[] = { 0xa4, 0xa4, 0xa4, 0xa4, 0xa4 };
static const uint8_t id_zero[] = { 0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x00, 0xc0 };
static const uint8_t size_too_big[] = { 0x76, 0x01, 0x01 };            /* Report Size 257 */
static const uint8_t count_too_big[] = { 0x96, 0x01, 0x10 };           /* Report Count 4097 */
static const uint8_t long_item_cut[] = { 0xfe, 0x05, 0x00, 0xaa, 0xbb };
static const uint8_t item_cut[] = { 0x05, 0x01, 0x16, 0x01 };          /* 2 data bytes: 1 */
/* The boot mouse's fields, then a Report ID: its reports carry no id. */
static const uint8_t fields_before_id[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0,
    0x05, 0x0c, 0x09, 0x01, 0xa1, 0x01, 0x85, 0x03, 0x75, 0x10, 0x95, 0x01, 0x81, 0x00, 0xc0,
};
/* X in report 1, Y in report 2. */
static const uint8_t xy_split[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x15, 0x81, 0x25, 0x7f, 0x75, 0x08, 0x95, 0x01,
    0x85, 0x01, 0x09, 0x30, 0x81, 0x06, 0x85, 0x02, 0x09, 0x31, 0x81, 0x06, 0xc0,
};
/* 1024 bytes of padding first: X and Y lie past the longest report. */
static const uint8_t xy_too_far[] = {
    0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x75, 0x08, 0x96, 0x00, 0x04, 0x81, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7f, 0x95, 0x02, 0x81, 0x06, 0xc0,
};

/* Built at run time: nesting and item counts past the limits. */
static uint8_t made[RD_MAX_BYTES + 1];

/* ---- the fixtures ----------------------------------------------------------------- */

#define F(b, s, sg) { b, s, sg }
#define NONE        { 0, 0, false }
#define BTNS        { F(0, 1, false), F(1, 1, false), F(2, 1, false) }

static const struct fixture {
    const char    *name;
    const uint8_t *d;       /* the descriptor (NULL: made at run time by make()) */
    uint32_t       n;       /* its length (or how much of it is used) */
    enum rd_status want;    /* what hid_rd_parse must say */
    struct rd_mouse m;      /* and with RD_OK, the layout */
} fixtures[] = {
#define D(a) a, sizeof(a)
    { "qemu usb-mouse", D(qemu_mouse), RD_OK,
      { 0, 32, 5, BTNS, F(8, 8, true), F(16, 8, true), F(24, 8, true), NONE } },
    { "gaming, 16-bit x/y", D(gaming16), RD_OK,
      { 0, 64, 16, BTNS, F(16, 16, true), F(32, 16, true), F(48, 8, true), F(56, 8, true) } },
    { "receiver, mouse id 2", D(receiver), RD_OK,
      { 2, 56, 16, BTNS, F(16, 12, true), F(28, 12, true), F(40, 8, true), F(48, 8, true) } },
    { "boot mouse", D(boot_mouse), RD_OK,
      { 0, 24, 3, BTNS, F(8, 8, true), F(16, 8, true), NONE, NONE } },
    { "tablet", D(tablet), RD_NO_XY, { 0 } },
    { "keyboard", D(keyboard), RD_NO_MOUSE, { 0 } },
    { "media keys", D(media), RD_NO_MOUSE, { 0 } },
    { "empty", gaming16, 0, RD_EMPTY, { 0 } },
    { "last End Collection cut", gaming16, sizeof(gaming16) - 1, RD_UNBALANCED, { 0 } },
    { "cut inside an item", gaming16, 30, RD_TRUNCATED, { 0 } },
    { "item data cut", D(item_cut), RD_TRUNCATED, { 0 } },
    { "long item cut", D(long_item_cut), RD_TRUNCATED, { 0 } },
    { "End Collection first", D(end_without_start), RD_UNBALANCED, { 0 } },
    { "Pop without Push", D(pop_without_push), RD_UNBALANCED, { 0 } },
    { "Push too deep", D(push_too_deep), RD_TOO_DEEP, { 0 } },
    { "report id 0", D(id_zero), RD_BAD_REPORT_ID, { 0 } },
    { "Report Size 257", D(size_too_big), RD_BAD_SIZE, { 0 } },
    { "Report Count 4097", D(count_too_big), RD_BAD_SIZE, { 0 } },
    { "fields before the first id", D(fields_before_id), RD_BAD_REPORT_ID, { 0 } },
    { "x and y in two reports", D(xy_split), RD_NO_XY, { 0 } },
    { "x past the longest report", D(xy_too_far), RD_NO_XY, { 0 } },
    { "nested too deep", NULL, 2 * (RD_MAX_DEPTH + 1), RD_TOO_DEEP, { 0 } },
    { "too many items", NULL, RD_MAX_ITEMS + 1, RD_TOO_MANY_ITEMS, { 0 } },
    { "too long", NULL, RD_MAX_BYTES + 1, RD_TOO_LONG, { 0 } },
#undef D
};

/* The descriptor of a run-time fixture into made[]. */
static const uint8_t *make(const struct fixture *f)
{
    for (uint32_t i = 0; i < f->n; i++)
        made[i] = f->want == RD_TOO_DEEP ? (i % 2 ? 0x00 : 0xa1)   /* Collection (Physical) */
                                         : 0x00;                    /* a size-0 main item */
    return made;
}

static bool same_field(const struct rd_field *a, const struct rd_field *b)
{
    return a->bit == b->bit && a->size == b->size && a->is_signed == b->is_signed;
}

static bool same_mouse(const struct rd_mouse *a, const struct rd_mouse *b)
{
    bool ok = a->report_id == b->report_id && a->bits == b->bits && a->nbuttons == b->nbuttons &&
              same_field(&a->x, &b->x) && same_field(&a->y, &b->y) &&
              same_field(&a->wheel, &b->wheel) && same_field(&a->pan, &b->pan);
    for (unsigned k = 0; k < 3; k++)
        ok &= same_field(&a->button[k], &b->button[k]);
    return ok;
}

static bool check(const struct fixture *f)
{
    struct rd_mouse m = { 0 };
    enum rd_status st = hid_rd_parse(f->d ? f->d : make(f), f->n, &m);
    if (st == f->want && (st != RD_OK || same_mouse(&m, &f->m)))
        return true;
    drv_log("hid: report parser self-test: %s: got \"%s\", want \"%s\"%s", f->name,
            hid_rd_status_str(st), hid_rd_status_str(f->want),
            st == RD_OK && f->want == RD_OK ? " (another layout)" : "");
    if (st == RD_OK)
        drv_log("hid: ... got id %u, %u bits, %u buttons, x %u/%u, y %u/%u, wheel %u/%u, pan "
                "%u/%u", m.report_id, m.bits, m.nbuttons, m.x.bit, m.x.size, m.y.bit, m.y.size,
                m.wheel.bit, m.wheel.size, m.pan.bit, m.pan.size);
    return false;
}

/* ---- decoding --------------------------------------------------------------------- */

static const struct decode_case {
    unsigned        fixture;   /* index into fixtures[] (its layout) */
    uint8_t         r[10];     /* the report */
    uint32_t        n;         /* its length */
    enum rd_decoded want;      /* what hid_rd_decode must say */
    struct rd_move  move;      /* and with RD_MOVE, the event */
} decodes[] = {
    /* qemu: left, x +5, y -3, wheel +1 (away from the user). */
    { 0, { 0x01, 0x05, 0xfd, 0x01 }, 4, RD_MOVE, { 5, -3, 1, 0, 1 } },
    /* gaming: left + middle, x 300, y -1000, wheel -1, pan +1. */
    { 1, { 0x05, 0x00, 0x2c, 0x01, 0x18, 0xfc, 0xff, 0x01 }, 8, RD_MOVE, { 300, -1000, -1, 1, 5 } },
    /* without the pan byte: the pan reads 0. */
    { 1, { 0x02, 0x00, 0x00, 0x80, 0xff, 0x7f, 0x02 }, 7, RD_MOVE, { -32768, 32767, 2, 0, 2 } },
    /* Y not all there: short. */
    { 1, { 0x00, 0x00, 0x01, 0x00, 0x01 }, 5, RD_SHORT, { 0 } },
    /* receiver: a keyboard report (id 1) is not the mouse's. */
    { 2, { 0x01, 0x00, 0x00, 0x04, 0, 0, 0, 0, 0 }, 9, RD_OTHER_ID, { 0 } },
    { 2, { 0 }, 0, RD_OTHER_ID, { 0 } },
    /* id 2: right, x -5, y 300 (12 bits each, sharing a byte), wheel +1. */
    { 2, { 0x02, 0x02, 0x00, 0xfb, 0xcf, 0x12, 0x01, 0x00 }, 8, RD_MOVE, { -5, 300, 1, 0, 2 } },
    /* x -2047, y +2047. */
    { 2, { 0x02, 0x00, 0x00, 0x01, 0xf8, 0x7f, 0x00, 0x00 }, 8, RD_MOVE, { -2047, 2047, 0, 0, 0 } },
    /* id byte only: short. */
    { 2, { 0x02 }, 1, RD_SHORT, { 0 } },
    /* boot mouse: all three buttons, x -128, y 127, no wheel. */
    { 3, { 0x07, 0x80, 0x7f }, 3, RD_MOVE, { -128, 127, 0, 0, 7 } },
    /* extra bytes after the report are ignored. */
    { 3, { 0x00, 0x01, 0x01, 0x55, 0x55 }, 5, RD_MOVE, { 1, 1, 0, 0, 0 } },
};

/* A wide field: 32 bits at bit 7, signed (spans five bytes), and a 16-bit
 * wheel, both clamped. */
static bool check_wide(void)
{
    const struct rd_mouse m = {
        .bits = 72, .x = F(7, 32, true), .y = F(40, 8, true), .wheel = F(48, 16, true),
    };
    const uint8_t r[9] = { 0x00, 0x00, 0x00, 0x00, 0x40, 0x01, 0x2c, 0x01, 0 };
    struct rd_move mv;
    if (hid_rd_decode(&m, r, sizeof(r), &mv) == RD_MOVE && mv.dx == -32768 && mv.dy == 1 &&
        mv.wheel == 127)
        return true;
    drv_log("hid: report parser self-test: wide fields decode wrong (%d %d %d)", mv.dx, mv.dy,
            mv.wheel);
    return false;
}

static bool check_decode(const struct decode_case *c)
{
    struct rd_move mv = { 0 };
    enum rd_decoded got = hid_rd_decode(&fixtures[c->fixture].m, c->r, c->n, &mv);
    if (got == c->want && (got != RD_MOVE || (mv.dx == c->move.dx && mv.dy == c->move.dy &&
                                              mv.wheel == c->move.wheel &&
                                              mv.pan == c->move.pan &&
                                              mv.buttons == c->move.buttons)))
        return true;
    drv_log("hid: report parser self-test: %s, %u-byte report: got %u (%d %d wheel %d pan %d "
            "buttons %#x), want %u", fixtures[c->fixture].name, c->n, got, mv.dx, mv.dy,
            mv.wheel, mv.pan, mv.buttons, c->want);
    return false;
}

/* ---- fuzzing ---------------------------------------------------------------------- */

#define FUZZ_ROUNDS 4000   /* changed copies per fixture */

static uint32_t rng_next(uint32_t *s)
{
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

/* A layout that came out of the parser must lie inside its report. */
static bool inside(const struct rd_field *f, uint32_t bits)
{
    return !f->size || (f->size <= RD_MAX_FIELD_BITS && f->bit + f->size <= bits);
}

static bool sane(const struct rd_mouse *m)
{
    bool ok = m->bits <= RD_MAX_REPORT_BITS && m->x.size && m->y.size && inside(&m->x, m->bits) &&
              inside(&m->y, m->bits) && inside(&m->wheel, m->bits) && inside(&m->pan, m->bits);
    for (unsigned k = 0; k < 3; k++)
        ok &= inside(&m->button[k], m->bits) && m->button[k].size <= 1;
    return ok;
}

/* Parse d (n bytes); with a layout, decode reports of every length 0..16
 * of rng bytes. False if a layout is not sane. *parsed counts RD_OKs. */
static bool fuzz_one(const uint8_t *d, uint32_t n, uint32_t *rng, uint32_t *parsed)
{
    struct rd_mouse m;
    if (hid_rd_parse(d, n, &m) != RD_OK)
        return true;
    (*parsed)++;
    if (!sane(&m))
        return false;
    uint8_t r[16];
    for (uint32_t len = 0; len <= sizeof(r); len++) {
        for (uint32_t k = 0; k < len; k++)
            r[k] = (uint8_t)rng_next(rng);
        if (len)
            r[0] = m.report_id ? m.report_id : r[0];
        struct rd_move mv;
        (void)hid_rd_decode(&m, r, len, &mv);   /* any answer: it must only not read past r */
    }
    return true;
}

static bool fuzz(const struct fixture *f, uint32_t *parsed)
{
    static uint8_t copy[RD_MAX_BYTES];
    uint32_t rng = 0x5eed0000u ^ f->n;
    for (uint32_t cut = 0; cut <= f->n; cut++)
        if (!fuzz_one(f->d, cut, &rng, parsed))
            return false;
    for (uint32_t round = 0; round < FUZZ_ROUNDS; round++) {
        for (uint32_t i = 0; i < f->n; i++)
            copy[i] = f->d[i];
        for (uint32_t k = 0, changes = 1 + rng_next(&rng) % 3; k < changes; k++)
            copy[rng_next(&rng) % f->n] = (uint8_t)rng_next(&rng);
        if (!fuzz_one(copy, f->n - rng_next(&rng) % 4 % f->n, &rng, parsed)) {
            drv_log("hid: report parser self-test: %s: round %u gave a layout outside its "
                    "report", f->name, round);
            return false;
        }
    }
    return true;
}

bool hid_rd_selftest(bool with_fuzz)
{
    unsigned nf = sizeof(fixtures) / sizeof(fixtures[0]), nd = sizeof(decodes) / sizeof(decodes[0]);
    unsigned passed = 0;
    for (unsigned i = 0; i < nf; i++)
        passed += check(&fixtures[i]);
    for (unsigned i = 0; i < nd; i++)
        passed += check_decode(&decodes[i]);
    passed += check_wide();
    bool ok = passed == nf + nd + 1;
    if (!with_fuzz) {
        if (!ok)
            drv_log("hid: report parser self-test: %u of %u passed", passed, nf + nd + 1);
        return ok;
    }
    uint32_t parsed = 0, fuzzed = 0;
    for (unsigned i = 0; i < nf; i++) {
        if (!fixtures[i].d || fixtures[i].n < 2)
            continue;
        ok &= fuzz(&fixtures[i], &parsed);
        fuzzed++;
    }
    drv_log("hid: report parser self-test: %u of %u checks passed; %u fixtures fuzzed (%u "
            "layouts parsed, all inside their reports): %s", passed, nf + nd + 1, fuzzed, parsed,
            ok ? "PASSED" : "FAILED");
    return ok;
}
