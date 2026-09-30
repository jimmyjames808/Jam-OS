/* hda: the dump. One line for the controller, two for each codec, one
 * per widget (two for a pin: the second decodes its configuration
 * default, capabilities and current state), and a last line naming the
 * front-panel headphone jack if the configuration defaults show one.
 * Every number the codec gave is printed in hex next to the words decoded
 * from it (spec 7.3: the parameters, and the Configuration Default
 * verb), so the dump can be read without the spec and checked against
 * it.
 *
 * The lines go to the kernel log, which the console shows and logd saves
 * to /data/logs. The kernel lets a process print 100 lines at once and
 * then 50 a second, dropping the rest, so a long dump paces itself past
 * the first LOG_BURST lines. */
#include "hda.h"

#define LINE       190   /* stays under the kernel's 200-byte line split with its prefix */
#define LOG_BURST  80    /* lines logged at once; after that one per LOG_GAP */
#define LOG_GAP    (21 * NS_PER_MS)

/* A line being built. */
struct sb {
    char   s[LINE];
    size_t n;
};

static void add(struct sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add(struct sb *b, const char *fmt, ...)
{
    if (b->n >= sizeof(b->s) - 1)
        return;
    va_list ap;
    va_start(ap, fmt);
    int m = drv_vsnprintf(b->s + b->n, sizeof(b->s) - b->n, fmt, ap);
    va_end(ap);
    if (m > 0)
        b->n += (size_t)m;
    if (b->n > sizeof(b->s) - 1)
        b->n = sizeof(b->s) - 1;
}

void out_line(struct out *o, const char *fmt, ...)
{
    char line[LINE];
    va_list ap;
    va_start(ap, fmt);
    int m = drv_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t n = m < 0 ? 0 : (size_t)m >= sizeof(line) ? sizeof(line) - 1 : (size_t)m;
    if (o->log) {
        if (o->lines >= LOG_BURST)
            drv_sleep_until(drv_clock_ns() + LOG_GAP);
        drv_log("%s", line);
    }
    if (o->buf && o->len + n + 1 <= o->cap) {
        for (size_t i = 0; i < n; i++)
            o->buf[o->len++] = line[i];
        o->buf[o->len++] = '\n';
    }
    o->lines++;
}

/* ---- words ----------------------------------------------------------------------- */

static const char *const wtypes[16] = {
    "dac", "adc", "mixer", "selector", "pin", "power", "knob", "beep",
    "type8", "type9", "type10", "type11", "type12", "type13", "type14", "vendor",
};
static const char *const conn_kinds[4] = { "jack", "none", "fixed", "jack+fixed" };
static const char *const gross[4] = { "ext", "int", "sep", "other" };
static const char *const geo[16] = {
    "", "rear", "front", "left", "right", "top", "bottom", "sp7",
    "sp8", "sp9", "g10", "g11", "g12", "g13", "g14", "g15",
};
static const char *const devices[16] = {
    "line-out", "speaker", "hp-out", "cd", "spdif-out", "dig-out", "modem-line", "modem-hs",
    "line-in", "aux", "mic", "telephony", "spdif-in", "dig-in", "dev14", "other",
};
static const char *const jacks[16] = {
    "jack?", "1/8in", "1/4in", "atapi", "rca", "optical", "digital", "analog",
    "din", "xlr", "rj11", "combo", "type12", "type13", "type14", "other",
};
static const char *const colours[16] = {
    "colour?", "black", "grey", "blue", "green", "red", "orange", "yellow",
    "purple", "pink", "c10", "c11", "c12", "c13", "white", "other",
};
static const char *const vrefs[8] = { "hiz", "50", "gnd", "r3", "80", "100", "r6", "r7" };
static const char *const rates[12] = {
    "8", "11", "16", "22", "32", "44.1", "48", "88.2", "96", "176", "192", "384",
};
static const unsigned sizes[5] = { 8, 16, 20, 24, 32 };

/* The 6-bit location (spec 7.3, Configuration Default): a few special values,
 * else gross location and side. */
static void add_location(struct sb *b, unsigned loc)
{
    static const struct { uint8_t v; const char *name; } special[] = {
        { 0x07, "rear-panel" }, { 0x08, "drive-bay" }, { 0x17, "riser" },
        { 0x18, "hdmi" }, { 0x19, "atapi" }, { 0x37, "lid-in" }, { 0x38, "lid-out" },
    };
    for (unsigned i = 0; i < sizeof(special) / sizeof(special[0]); i++)
        if (special[i].v == loc) {
            add(b, " %s", special[i].name);
            return;
        }
    add(b, " %s%s%s", gross[loc >> 4 & 3], (loc & 0xf) ? "-" : "", geo[loc & 0xf]);
}

static void add_pcm(struct sb *b, uint32_t pcm, uint32_t formats)
{
    add(b, " pcm");
    const char *sep = " ";
    for (unsigned i = 0; i < 5; i++)
        if (pcm & (1u << (16 + i))) {
            add(b, "%s%u", sep, sizes[i]);
            sep = ",";
        }
    add(b, "b");
    sep = " ";
    for (unsigned i = 0; i < 12; i++)
        if (pcm & (1u << i)) {
            add(b, "%s%s", sep, rates[i]);
            sep = ",";
        }
    add(b, "k%s%s", formats & 2 ? " float" : "", formats & 4 ? " ac3" : "");
}

/* Amplifier Capabilities (spec 7.3, parameters): steps 0..n of (s+1)/4 dB, 0 dB
 * at step `offset`, and whether it mutes. */
static void add_ampcaps(struct sb *b, const char *which, uint32_t caps)
{
    unsigned steps = caps >> 8 & 0x7f, size = ((caps >> 16 & 0x7f) + 1) * 25;
    add(b, " %s 0-%u x%u.%02udB 0dB@%u%s", which, steps, size / 100, size % 100,
        caps & 0x7f, caps & (1u << 31) ? " +mute" : "");
}

/* An amp's current value: "m" for muted, then the gain step; one value
 * when both sides agree. */
static void add_amp(struct sb *b, struct amp_now a)
{
    add(b, "%s%u", a.l & 0x80 ? "m" : "", a.l & 0x7f);
    if (a.l != a.r)
        add(b, "/%s%u", a.r & 0x80 ? "m" : "", a.r & 0x7f);
}

static void add_power(struct sb *b, uint8_t p)
{
    add(b, " pwr D%u/D%u", p >> 4 & 0xf, p & 0xf);   /* actual / set */
}

/* ---- lines ------------------------------------------------------------------------ */

void hda_dump_ctrl(struct out *o, const struct hda *h, const char *where)
{
    uint16_t g = h->gcap;
    out_line(o, "controller %04x:%04x%s: gcap %#06x: %u in, %u out, %u bidi streams, %s; "
             "codecs %#06x; commands through %s (%u/%u entries)", h->vendor, h->device, where, g,
             g >> 8 & 0xf, g >> 12 & 0xf, g >> 3 & 0x1f, g & 1 ? "64-bit" : "32-bit only",
             h->codec_mask, h->rings ? "CORB/RIRB" : h->immediate_ok ? "the immediate interface"
                                                                     : "nothing",
             h->corb_entries, h->rirb_entries);
    out_line(o, "controller %04x:%04x: pci config 40-4f %08x %08x %08x %08x", h->vendor, h->device,
             h->cfg40[0], h->cfg40[1], h->cfg40[2], h->cfg40[3]);
}

static void dump_header(struct out *o, const struct codec *c)
{
    out_line(o, "codec %u: %04x:%04x rev %#x subsys %04x:%04x, %u function group(s)%s; "
             "audio group at node %02x%s", c->cad, c->vendor >> 16, c->vendor & 0xffff,
             c->revision, c->subsys >> 16, c->subsys & 0xffff, c->nfgs,
             c->fg_types & (1u << 2) ? " (a modem group too)" : "", c->afg,
             c->afg_unsol ? " (unsolicited-capable)" : "");
    if (!c->afg)
        return;
    struct sb b = { .n = 0 };
    add(&b, "codec %u afg %02x: widgets %02x-%02x", c->cad, c->afg, c->first,
        c->first + c->count - 1);
    add_power(&b, c->afg_power);
    add(&b, " (supports %#x) gpio %#x caps %#x", c->afg_states, c->afg_gpio, c->afg_caps);
    add_pcm(&b, c->afg_pcm, c->afg_formats);
    if (c->afg_amp_out)
        add_ampcaps(&b, "out-amp", c->afg_amp_out);
    if (c->afg_amp_in)
        add_ampcaps(&b, "in-amp", c->afg_amp_in);
    out_line(o, "%s", b.s);
}

static void add_conns(struct sb *b, const struct widget *w)
{
    add(b, " conn");
    unsigned n = w->nconn < MAX_CONN ? w->nconn : MAX_CONN;
    for (unsigned i = 0; i < n; i++)
        add(b, " %02x%s", w->conn[i], w->has_sel && w->conn_sel == i ? "*" : "");
    if (w->nconn > MAX_CONN)
        add(b, " +%u", w->nconn - MAX_CONN);
}

static void dump_widget(struct out *o, const struct codec *c, const struct widget *w)
{
    struct sb b = { .n = 0 };
    uint32_t caps = w->caps;
    enum wtype t = WCAP_TYPE(caps);
    add(&b, "c%u %02x %-8s caps %08x %uch%s%s", c->cad, w->nid, wtypes[t], caps, WCAP_CHANS(caps),
        caps & WCAP_DIGITAL ? " digital" : "", caps & WCAP_UNSOL ? " unsol" : "");
    if (w->has_power)
        add_power(&b, w->power);
    if (caps & WCAP_OUT_AMP) {
        add_ampcaps(&b, "out-amp", w->amp_out);
        add(&b, " now ");
        add_amp(&b, w->out);
    }
    if (caps & WCAP_IN_AMP) {
        add_ampcaps(&b, "in-amp", w->amp_in);
        add(&b, " now");
        for (unsigned i = 0; i < w->nin; i++) {
            add(&b, " ");
            add_amp(&b, w->in[i]);
        }
    }
    if (caps & WCAP_CONN_LIST)
        add_conns(&b, w);
    if (t == W_OUT || t == W_IN) {
        add(&b, " stream %u ch %u fmt %#06x", w->stream >> 4, w->stream & 0xf, w->format);
        if (caps & WCAP_FMT_OVR)
            add_pcm(&b, w->pcm, w->formats);
    }
    if (w->errors)
        add(&b, " (%u verbs failed)", w->errors);
    out_line(o, "%s", b.s);
}

static void add_pincaps(struct sb *b, uint32_t p)
{
    add(b, " | caps %08x%s%s%s%s%s%s%s%s%s", p, p & PINCAP_OUT ? " out" : "",
        p & PINCAP_IN ? " in" : "", p & PINCAP_HP ? " hp" : "",
        p & PINCAP_PRESENCE ? " presence" : "", p & PINCAP_TRIGGER ? " trigger" : "",
        p & PINCAP_EAPD ? " eapd" : "", p & PINCAP_BALANCED ? " balanced" : "",
        p & PINCAP_HDMI ? " hdmi" : "", p & PINCAP_DP ? " dp" : "");
    if (PINCAP_VREF(p)) {
        add(b, " vref");
        for (unsigned i = 0; i < 8; i++)
            if (PINCAP_VREF(p) & (1u << i))
                add(b, "%s%s", i == (unsigned)__builtin_ctz(PINCAP_VREF(p)) ? "=" : ",", vrefs[i]);
    }
}

/* A pin's second line: what the board says it is, what it can do, how it
 * is set now. */
static void dump_pin(struct out *o, const struct codec *c, const struct widget *w)
{
    struct sb b = { .n = 0 };
    uint32_t cfg = w->config;
    add(&b, "c%u %02x   cfg %08x: %s", c->cad, w->nid, cfg, conn_kinds[cfg >> 30]);
    add_location(&b, cfg >> 24 & 0x3f);
    add(&b, " %s %s %s as%u sq%u%s", colours[cfg >> 12 & 0xf], devices[cfg >> 20 & 0xf],
        jacks[cfg >> 16 & 0xf], cfg >> 4 & 0xf, cfg & 0xf,
        cfg & CFG_NO_PRESENCE ? " no-detect" : "");
    add_pincaps(&b, w->pincaps);
    uint8_t ctl = w->pin_ctl;
    add(&b, " | ctl %02x%s%s%s vref=%s", ctl, ctl & 0x40 ? " out" : "", ctl & 0x80 ? " hp" : "",
        ctl & 0x20 ? " in" : "", vrefs[ctl & 7]);
    if (w->pincaps & PINCAP_EAPD)
        add(&b, " eapd=%s", w->eapd & 2 ? "on" : "off");
    if (w->has_sense)
        add(&b, " | %s", w->sense & (1u << 31) ? "plugged" : "empty");
    if (w->caps & WCAP_UNSOL)
        add(&b, " | unsol %s tag %u", w->unsol & 0x80 ? "on" : "off", w->unsol & 0x3f);
    out_line(o, "%s", b.s);
}

/* The front-panel headphone jacks the configuration defaults name. */
static void dump_front_hp(struct out *o, const struct codec *c)
{
    struct sb b = { .n = 0 };
    add(&b, "codec %u: front-panel headphone jack:", c->cad);
    unsigned found = 0;
    for (unsigned i = 0; i < c->nw; i++) {
        const struct widget *w = &c->w[i];
        uint32_t cfg = w->config;
        if (WCAP_TYPE(w->caps) != W_PIN || cfg >> 30 == 1 || (cfg >> 20 & 0xf) != 2 ||
            (cfg >> 24 & 0xf) != 2)
            continue;
        add(&b, " node %02x%s", w->nid, w->has_sense ? (w->sense >> 31 ? " (plugged)" : " (empty)")
                                                    : "");
        found++;
    }
    if (!found)
        add(&b, " none in the configuration defaults");
    out_line(o, "%s", b.s);
}

void hda_dump_codec(struct out *o, const struct codec *c)
{
    dump_header(o, c);
    for (unsigned i = 0; i < c->nw; i++) {
        dump_widget(o, c, &c->w[i]);
        if (WCAP_TYPE(c->w[i].caps) == W_PIN)
            dump_pin(o, c, &c->w[i]);
    }
    if (c->afg)
        dump_front_hp(o, c);
}
