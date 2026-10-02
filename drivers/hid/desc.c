/* hid: the descriptors a HID interface's set-up walks (hid.c reads them
 * into h->buf): the configuration descriptor, for this interface's HID
 * descriptor (the report descriptor's length, HID 1.11 6.2.1) and its
 * interrupt IN endpoint; and a one-line summary of the report descriptor
 * for the log, its top-level application collections with their first
 * report id. The device writes these bytes: every walk stays inside the
 * n bytes that arrived, every item's length checked first. (The mouse's
 * report layout is report.c's.) */
#include "hid.h"

/* The report descriptor's length in a HID descriptor (len bytes at d,
 * HID 1.11 6.2.1): its first REPORT entry with a length, 0 if none. */
static uint16_t report_len(const uint8_t *d, uint8_t len)
{
    for (uint32_t k = 0; k < d[5] && 6 + 3 * k + 3 <= len; k++) {
        uint16_t n = (uint16_t)(d[7 + 3 * k] | d[8 + 3 * k] << 8);
        if (d[6 + 3 * k] == DESC_REPORT && n)
            return n;
    }
    return 0;
}

/* This interface's HID descriptor and interrupt IN endpoint, from the
 * configuration descriptor in h->buf (n bytes). */
void hid_parse_config(struct hid *h, uint32_t n)
{
    const uint8_t *d = h->buf;
    bool ours = false;
    for (uint32_t i = 0; i + 2 <= n;) {
        uint8_t len = d[i], type = d[i + 1];
        if (len < 2 || i + len > n)
            break;
        if (type == DESC_INTERFACE && len >= 9) {
            ours = d[i + 2] == h->iface && d[i + 3] == h->alt;
        } else if (ours && type == DESC_HID && len >= 9) {
            if (!h->report_desc_len)
                h->report_desc_len = report_len(d + i, len);
        } else if (ours && type == DESC_ENDPOINT && len >= 7) {
            uint8_t addr = d[i + 2], attr = d[i + 3];
            if ((addr & 0x80) && (attr & 3) == 3 && !h->ep_in) {
                h->ep_in = addr;
                h->max_packet = (uint16_t)((d[i + 4] | d[i + 5] << 8) & 0x7ff);
            }
        }
        i += len;
    }
}

static const char *collection_name(uint32_t page, uint32_t usage)
{
    if (page == 0x01) {
        switch (usage) {
        case 0x01: return "pointer";
        case 0x02: return "mouse";
        case 0x04: return "joystick";
        case 0x05: return "gamepad";
        case 0x06: return "keyboard";
        case 0x07: return "keypad";
        case 0x80: return "system control";
        }
    }
    if (page == 0x0c)
        return "consumer control";
    if (page >= 0xff00)
        return "vendor";
    return "other";
}

/* The summary being built: out holds len characters, cap bytes with the
 * NUL. */
struct text {
    char    *s;          /* the buffer */
    uint32_t len, cap;   /* characters in it; its size with the NUL */
};

/* Append to t; past the cap the text is cut, as snprintf cuts it. */
static void text_add(struct text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void text_add(struct text *t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = drv_vsnprintf(t->s + t->len, t->cap - t->len, fmt, ap);
    va_end(ap);
    if (r > 0)
        t->len = t->len + (uint32_t)r < t->cap ? t->len + (uint32_t)r : t->cap - 1;
}

/* A top-level application collection: its name into t (with its page and
 * usage when it has no name of its own). *count: collections so far. */
static void add_collection(struct text *t, uint32_t page, uint32_t usage, uint32_t *count)
{
    const char *nm = collection_name(page, usage);
    text_add(t, "%s%s", (*count)++ ? ", " : "", nm);
    if (nm[0] == 'v' || nm[0] == 'o')
        text_add(t, " (page 0x%x usage 0x%x)", page, usage);
}

/* A one-line summary of the report descriptor in h->buf (n bytes): its
 * top-level application collections with their first report id, e.g.
 * "keyboard id 1, consumer control id 3, vendor (page 0xff00 usage 0x1) id 6". */
void hid_summarise_report(const struct hid *h, uint32_t n, char *out, uint32_t cap)
{
    const uint8_t *d = h->buf;
    struct text t = { out, 0, cap };
    uint32_t page = 0, usage = 0, depth = 0, count = 0;
    bool want_id = false;
    out[0] = 0;
    for (uint32_t i = 0; i < n;) {
        uint8_t b = d[i];
        if (b == 0xfe) {                          /* long item */
            i += 3u + (i + 1 < n ? d[i + 1] : 0);
            continue;
        }
        uint32_t size = (b & 3) == 3 ? 4 : (b & 3), v = 0;
        if (i + 1 + size > n)
            break;
        for (uint32_t k = 0; k < size; k++)
            v |= (uint32_t)d[i + 1 + k] << (8 * k);
        switch (b & 0xfc) {
        case 0x04: page = v; break;               /* Usage Page */
        case 0x08:                                /* Usage (a 4-byte one names its page) */
            usage = size == 4 ? v & 0xffff : v;
            if (size == 4)
                page = v >> 16;
            break;
        case 0xa0:                                /* Collection */
            if (depth++ == 0 && v == 1) {         /* a top-level application one */
                add_collection(&t, page, usage, &count);
                want_id = true;
            }
            break;
        case 0xc0:                                /* End Collection */
            if (depth && !--depth)
                want_id = false;
            break;
        case 0x84:                                /* Report ID */
            if (want_id) {
                text_add(&t, " id %u", v);
                want_id = false;
            }
            break;
        }
        i += 1 + size;
    }
    if (!count)
        text_add(&t, "no collections");
}
