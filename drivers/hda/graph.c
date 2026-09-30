/* hda: reading a codec's graph (spec chapter 7). A codec is a tree of
 * nodes: the root (node 0) names the codec and lists its function groups;
 * the audio function group (AFG) lists its widgets, and holds the
 * defaults (PCM rates, amplifier steps) widgets use unless they override
 * them. Widgets are converters (DAC, ADC), mixers, selectors, pin
 * complexes (a jack or a fixed speaker/mic), a beep generator and a few
 * others; each widget's connection list names the widgets whose output it
 * takes, so the graph runs from a DAC through mixers and selectors to a
 * pin.
 *
 * Everything here is read with GET verbs (hda_get enforces it). A widget
 * whose verbs fail is counted in its `errors` and the walk goes on; only a
 * codec whose root does not answer fails. */
#include "hda.h"

/* One GET into *out, counting a failure against w. */
static uint32_t get(struct hda *h, struct codec *c, struct widget *w, uint32_t verb,
                    uint32_t payload)
{
    uint32_t v = 0;
    if (hda_get(h, c->cad, w->nid, verb, payload, &v) != OK) {
        w->errors++;
        return 0;
    }
    return v;
}

static uint32_t param(struct hda *h, struct codec *c, struct widget *w, uint32_t p)
{
    return get(h, c, w, V_GET_PARAM, p);
}

static void add_conn(struct widget *w, unsigned *n, uint16_t nid)
{
    if (*n < MAX_CONN)
        w->conn[*n] = nid;
    (*n)++;
}

/* One list entry e (range: its range bit); *prev: the entry before it. */
static void conn_entry(struct widget *w, unsigned *n, uint16_t *prev, uint16_t e, uint32_t range)
{
    uint16_t nid = (uint16_t)(e & (range - 1));
    if ((e & range) && *prev && nid > *prev && nid - *prev < 128)
        for (uint16_t x = (uint16_t)(*prev + 1); x <= nid; x++)
            add_conn(w, n, x);
    else
        add_conn(w, n, nid);
    *prev = nid;
}

/* The connection list (spec 7.3, Get Connection List Entry): asked at index
 * i answers the entries from i rounded down, four 8-bit ones (short form)
 * or two 16-bit ones (long form). An entry with its top bit set is the
 * end of a range: every node from the entry before it up to this one. */
static void read_conn(struct hda *h, struct codec *c, struct widget *w)
{
    uint32_t len = param(h, c, w, P_CONN_LEN);
    unsigned total = len & 0x7fu, per = len & 0x80u ? 2 : 4, bits = 32 / per;
    uint32_t mask = (1u << bits) - 1, range = 1u << (bits - 1);
    w->conn_long = len & 0x80u;
    unsigned n = 0;
    uint16_t prev = 0;
    for (unsigned i = 0; i < total; i += per) {
        uint32_t v = get(h, c, w, V_GET_CONN_LIST, i);
        for (unsigned k = 0; k < per && i + k < total; k++) {
            conn_entry(w, &n, &prev, (uint16_t)((v >> (k * bits)) & mask), range);
        }
    }
    w->nconn = (uint8_t)(n > 255 ? 255 : n);
}

/* bit 7 mute, 6:0 gain, for one amp (output or input `index`, `left`). */
static uint8_t amp(struct hda *h, struct codec *c, struct widget *w, bool output, bool left,
                   unsigned index)
{
    uint32_t p = (output ? AMP_GET_OUT : 0) | (left ? AMP_GET_LEFT : 0) | (index & 0xfu);
    return (uint8_t)get(h, c, w, V4_GET_AMP, p);
}

static void read_amps(struct hda *h, struct codec *c, struct widget *w, enum wtype t)
{
    bool ovr = w->caps & WCAP_AMP_OVR;
    if (w->caps & WCAP_OUT_AMP) {
        w->amp_out = ovr ? param(h, c, w, P_AMP_OUT) : c->afg_amp_out;
        w->out = (struct amp_now){ amp(h, c, w, true, true, 0), amp(h, c, w, true, false, 0) };
    }
    if (!(w->caps & WCAP_IN_AMP))
        return;
    w->amp_in = ovr ? param(h, c, w, P_AMP_IN) : c->afg_amp_in;
    /* A pin's or a converter's input amp has one index; a mixer's or a
     * selector's has one per connection. */
    unsigned n = t == W_PIN || t == W_IN || !w->nconn ? 1 : w->nconn;
    if (n > MAX_IN_AMPS)
        n = MAX_IN_AMPS;
    for (unsigned i = 0; i < n; i++)
        w->in[i] = (struct amp_now){ amp(h, c, w, false, true, i), amp(h, c, w, false, false, i) };
    w->nin = (uint8_t)n;
}

static void read_pin(struct hda *h, struct codec *c, struct widget *w)
{
    w->pincaps = param(h, c, w, P_PIN_CAPS);
    w->config = get(h, c, w, V_GET_CONFIG, 0);
    w->pin_ctl = (uint8_t)get(h, c, w, V_GET_PIN_CTL, 0);
    if (w->pincaps & PINCAP_EAPD)
        w->eapd = (uint8_t)get(h, c, w, V_GET_EAPD, 0);
    /* Presence is read only where no trigger is needed: a trigger is a
     * SET verb (it starts an impedance measurement), and this driver
     * sends none. */
    if ((w->pincaps & PINCAP_PRESENCE) && !(w->pincaps & PINCAP_TRIGGER)) {
        w->sense = get(h, c, w, V_GET_PIN_SENSE, 0);
        w->has_sense = true;
    }
}

static void read_widget(struct hda *h, struct codec *c, struct widget *w)
{
    w->caps = param(h, c, w, P_WIDGET_CAPS);
    enum wtype t = WCAP_TYPE(w->caps);
    if (w->caps & WCAP_CONN_LIST)
        read_conn(h, c, w);
    if ((w->caps & WCAP_CONN_LIST) && t != W_MIXER && w->nconn > 1) {
        w->conn_sel = (uint8_t)get(h, c, w, V_GET_CONN_SEL, 0);
        w->has_sel = true;
    }
    if (t == W_OUT || t == W_IN) {
        bool ovr = w->caps & WCAP_FMT_OVR;
        w->pcm = ovr ? param(h, c, w, P_PCM) : c->afg_pcm;
        w->formats = ovr ? param(h, c, w, P_FORMATS) : c->afg_formats;
        w->stream = (uint8_t)get(h, c, w, V_GET_STREAM, 0);
        w->format = (uint16_t)get(h, c, w, V4_GET_FORMAT, 0);
    }
    if (t == W_PIN)
        read_pin(h, c, w);
    read_amps(h, c, w, t);
    if (w->caps & WCAP_POWER) {
        w->power = (uint8_t)get(h, c, w, V_GET_POWER, 0);
        w->has_power = true;
    }
    if (w->caps & WCAP_UNSOL)
        w->unsol = (uint8_t)get(h, c, w, V_GET_UNSOL, 0);
}

/* The AFG's own parameters and its widget range. */
static void read_afg(struct hda *h, struct codec *c)
{
    struct widget fg = { .nid = c->afg };
    c->subsys = get(h, c, &fg, V_GET_SUBSYS, 0);
    c->afg_caps = param(h, c, &fg, P_AFG_CAPS);
    c->afg_pcm = param(h, c, &fg, P_PCM);
    c->afg_formats = param(h, c, &fg, P_FORMATS);
    c->afg_amp_in = param(h, c, &fg, P_AMP_IN);
    c->afg_amp_out = param(h, c, &fg, P_AMP_OUT);
    c->afg_states = param(h, c, &fg, P_POWER_STATES);
    c->afg_gpio = param(h, c, &fg, P_GPIO);
    c->afg_power = (uint8_t)get(h, c, &fg, V_GET_POWER, 0);
    uint32_t nodes = param(h, c, &fg, P_NODES);
    c->first = (uint8_t)(nodes >> 16);
    c->count = (uint8_t)nodes;
}

status_t hda_read_codec(struct hda *h, unsigned cad, struct codec *c)
{
    *c = (struct codec){ .cad = (uint8_t)cad };
    status_t st = hda_param(h, cad, 0, P_VENDOR, &c->vendor);
    if (st != OK)
        return st;
    struct widget root = { .nid = 0 };
    c->revision = param(h, c, &root, P_REVISION);
    uint32_t nodes = param(h, c, &root, P_NODES);
    unsigned first = (nodes >> 16) & 0xffu, n = nodes & 0xffu;
    c->nfgs = (uint8_t)n;
    for (unsigned i = 0; i < n && first + i <= 0x7f; i++) {
        struct widget fg = { .nid = (uint8_t)(first + i) };
        uint32_t type = param(h, c, &fg, P_FG_TYPE);
        c->fg_types |= 1u << (type & 0x1fu);
        if ((type & 0xffu) == FG_AUDIO && !c->afg) {
            c->afg = fg.nid;
            c->afg_unsol = (type >> 8) & 1u;
        }
    }
    if (!c->afg)
        return OK;
    read_afg(h, c);
    for (unsigned i = 0; i < c->count && c->nw < MAX_WIDGETS && c->first + i <= 0x7f; i++) {
        struct widget *w = &c->w[c->nw++];
        w->nid = (uint8_t)(c->first + i);
        read_widget(h, c, w);
    }
    return OK;
}
