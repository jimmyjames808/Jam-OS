/* hid: the report descriptor parser (report.c) and its self-test
 * (fixtures.c). It knows nothing of USB or of the driver: bytes in, a
 * mouse report layout out, so the self-test can feed it any descriptor.
 *
 * The descriptor is the device's own data and is not trusted: every
 * length, count and nesting depth it states is checked against the limits
 * below, and a report is only ever read inside the bytes that arrived. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RD_MAX_BYTES       1024   /* a longer descriptor is not parsed (the driver's buffer) */
#define RD_MAX_ITEMS       768    /* items parsed at most (real ones average ~2 bytes) */
#define RD_MAX_DEPTH       16     /* Collection nesting */
#define RD_MAX_PUSH        4      /* Push nesting */
#define RD_MAX_USAGES      32     /* usages kept per main item; later ones are dropped */
#define RD_MAX_REPORT_SIZE 256    /* Report Size, bits per field */
#define RD_MAX_REPORT_COUNT 4096  /* Report Count, fields per main item */
#define RD_MAX_REPORT_BITS 8184   /* one input report (1023 bytes after its id byte) */
#define RD_MAX_FIELD_BITS  32     /* the widest field the mouse layer decodes */

/* What the parser made of a descriptor. Every value but RD_OK means the
 * layout is unusable: the driver keeps the boot protocol. */
enum rd_status {
    RD_OK = 0,
    RD_EMPTY,             /* no bytes */
    RD_TOO_LONG,          /* over RD_MAX_BYTES */
    RD_TRUNCATED,         /* an item's data runs past the end */
    RD_TOO_MANY_ITEMS,    /* over RD_MAX_ITEMS */
    RD_TOO_DEEP,          /* Collection nesting over RD_MAX_DEPTH, or Push over RD_MAX_PUSH */
    RD_UNBALANCED,        /* End Collection without a Collection, Pop without a Push, or a
                           * Collection left open at the end */
    RD_BAD_REPORT_ID,     /* Report ID 0 or over 255, or fields before the first id */
    RD_BAD_SIZE,          /* Report Size or Report Count over its limit */
    RD_NO_MOUSE,          /* no Generic Desktop Mouse (or Pointer) application collection */
    RD_NO_XY,             /* the mouse has no relative X and Y in one report */
};

/* One field of the mouse's input report. */
struct rd_field {
    uint16_t bit;         /* offset in the report, after the report id byte */
    uint8_t  size;        /* bits, 1..RD_MAX_FIELD_BITS; 0: the mouse has no such field */
    bool     is_signed;   /* its Logical Minimum is negative: sign-extend it */
};

/* The mouse's input report, as its descriptor lays it out. */
struct rd_mouse {
    uint8_t        report_id;   /* the mouse's report id, 0: the device uses none */
    uint16_t       bits;        /* the report's length in bits, without the id byte */
    uint8_t        nbuttons;    /* Button-page fields of one bit (buttons 1..n) */
    struct rd_field button[3];  /* buttons 1 (left), 2 (right), 3 (middle) */
    struct rd_field x, y;       /* Generic Desktop X, Y: relative */
    struct rd_field wheel;      /* Generic Desktop Wheel, relative; size 0: none */
    struct rd_field pan;        /* Consumer AC Pan (the horizontal wheel); size 0: none */
};

/* One report decoded. */
struct rd_move {
    int16_t dx, dy;       /* counts moved, clamped to int16 */
    int8_t  wheel, pan;   /* notches (+: away from the user / right), clamped to +-127 */
    uint8_t buttons;      /* bit 0 left, 1 right, 2 middle */
};

/* What hid_rd_decode made of a report. */
enum rd_decoded {
    RD_MOVE,              /* *out holds it */
    RD_OTHER_ID,          /* another report id: not the mouse's (or no id byte at all) */
    RD_SHORT,             /* X and Y are not both inside the bytes that arrived */
};

/* Parse descriptor d (n bytes) and find its mouse: its first Generic
 * Desktop Mouse (or Pointer) application collection, the report id of its
 * relative X and the fields of that report. *out is written only on
 * RD_OK. Reads nothing outside d[0..n). */
enum rd_status hid_rd_parse(const uint8_t *d, uint32_t n, struct rd_mouse *out);
/* The status as a few words, for the log. */
const char    *hid_rd_status_str(enum rd_status st);
/* Report r (n bytes, as it arrived) with layout m. A field that doesn't
 * fit in the bytes that came (the wheel of a short report) reads as 0.
 * Reads nothing outside r[0..n). */
enum rd_decoded hid_rd_decode(const struct rd_mouse *m, const uint8_t *r, uint32_t n,
                              struct rd_move *out);

/* fixtures.c: the parser and the decoder against recorded descriptors
 * (mice, keyboards, broken ones). Each failure goes to the log; true if
 * all passed. With `fuzz`, every fixture is also parsed cut at every
 * length and with thousands of seeded byte changes, and each layout that
 * comes out is checked to lie inside its report (slower: tests only). */
bool hid_rd_selftest(bool fuzz);
