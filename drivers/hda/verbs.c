/* hda: the verbs the driver may send to a codec (spec 7.3), and the
 * sequences made of them. Every command to a codec goes through this
 * file: hda_get takes GET verbs, which read and change nothing, and
 * hda_set takes the SET verbs on `sets` below, each with the payload bits
 * that verb defines. ctrl.c's hda_command is the raw send, and nothing
 * but this file calls it, so `sets` is the whole list of what the driver
 * can ever change in a codec. Not on it, on purpose: the configuration
 * default (the board's description of its jacks), the function group
 * reset, GPIOs, the subsystem id, the beep generator, digital converter
 * controls and the vendor coefficient verbs.
 *
 * The sequences: at start, the path path.c found programmed silent
 * (power up, the pin's output off, every amp on the path muted, the
 * path's inputs selected); while a stream runs, the same path opened
 * (hda_output_open: its amps unmuted at the gain, the pin's output and
 * EAPD on) and closed again as soon as it stops. The DAC's stream tag and
 * format are stream.c's (through hda_set too). */
#include "hda.h"

/* A GET verb: 12-bit verbs 0xf00-0xfff, or the 4-bit GET verbs 0xa
 * (converter format) and 0xb (amplifier gain/mute). Everything else sets
 * something in the codec (spec 7.3). */
static bool is_get(uint32_t verb, uint32_t payload)
{
    if (verb >= 0xf00 && verb <= 0xfff)
        return payload <= 0xff;
    return (verb == V4_GET_FORMAT || verb == V4_GET_AMP) && payload <= 0xffff;
}

/* The command word (spec 7.3): codec address, node, then a 12-bit verb
 * with an 8-bit payload or a 4-bit verb with a 16-bit payload. */
static uint32_t command(unsigned cad, unsigned nid, uint32_t verb, uint32_t payload)
{
    uint32_t cmd = (uint32_t)cad << 28 | (uint32_t)nid << 20;
    return cmd | (verb >= 0x100 ? verb << 8 | payload : verb << 16 | payload);
}

status_t hda_get(struct hda *h, unsigned cad, unsigned nid, uint32_t verb, uint32_t payload,
                 uint32_t *out)
{
    if (cad >= HDA_MAX_CODECS || nid > 0x7f || !is_get(verb, payload))
        return ERR_INVALID_ARGS;
    return hda_command(h, cad, command(cad, nid, verb, payload), out);
}

status_t hda_param(struct hda *h, unsigned cad, unsigned nid, uint32_t param, uint32_t *out)
{
    return hda_get(h, cad, nid, V_GET_PARAM, param, out);
}

/* ---- SET verbs ---------------------------------------------------------------- */

#define POWER_WAIT  (100 * NS_PER_MS)   /* a node to reach D0 after SET_POWER_STATE */

/* The allow-list: each SET verb and the payload bits it may carry. */
static const struct {
    uint16_t verb;
    uint16_t bits;
} sets[] = {
    { V_SET_CONN_SEL,  0x00ff },   /* the connection list index */
    { V_SET_POWER,     0x0003 },   /* D0-D3; never D3cold */
    { V_SET_STREAM,    0x00ff },   /* stream 7:4, channel 3:0 */
    { V_SET_PIN_CTL,   0x00e7 },   /* hp, out, in, vref; 4:3 are digital pins' only */
    { V_SET_UNSOL,     0x00bf },   /* enable, tag */
    { V_SET_PIN_SENSE, 0x0001 },   /* right channel */
    { V_SET_EAPD,      0x0007 },   /* L/R swap, EAPD, BTL */
    { V4_SET_FORMAT,   0x7f7f },   /* PCM only (bit 15 0), 7 reserved */
    { V4_SET_AMP,      0xffff },   /* checked below */
};

status_t hda_set(struct hda *h, unsigned cad, unsigned nid, uint32_t verb, uint32_t payload)
{
    unsigned i = 0, n = sizeof(sets) / sizeof(sets[0]);
    while (i < n && sets[i].verb != verb)
        i++;
    if (i == n) {
        drv_log("refused: verb %#x to node %02x is not on the allow-list", verb, nid);
        return ERR_NOT_SUPPORTED;
    }
    if (cad >= HDA_MAX_CODECS || nid > 0x7f || (payload & ~(uint32_t)sets[i].bits))
        return ERR_INVALID_ARGS;
    /* An amp SET that names no amp (output or input) or no side does nothing. */
    if (verb == V4_SET_AMP && (!(payload & (AMP_SET_OUT | AMP_SET_IN)) ||
                               !(payload & (AMP_SET_LEFT | AMP_SET_RIGHT))))
        return ERR_INVALID_ARGS;
    uint32_t ignored;
    return hda_command(h, cad, command(cad, nid, verb, payload), &ignored);
}

status_t hda_power_up(struct hda *h, unsigned cad, unsigned nid)
{
    status_t st = hda_set(h, cad, nid, V_SET_POWER, PS_D0);
    if (st != OK)
        return st;
    uint64_t deadline = drv_clock_ns() + POWER_WAIT;
    uint32_t v = 0;
    for (;;) {
        if ((st = hda_get(h, cad, nid, V_GET_POWER, 0, &v)) != OK)
            return st;
        if ((v >> 4 & 0xfu) == PS_D0)
            return OK;
        if (drv_clock_ns() > deadline) {
            drv_log("node %02x: power stays D%u (asked D0)", nid, v >> 4 & 0xfu);
            return ERR_TIMED_OUT;
        }
        drv_sleep_until(drv_clock_ns() + NS_PER_MS);
    }
}

/* ---- the path, muted ------------------------------------------------------------ */

/* Every amp of w that sound to the pin passes muted, at gain step 0 (an
 * amp without a mute, like the ALC897 DACs', is then at its quietest):
 * the output amp, and on a mixer or selector the input amp of each
 * connection. A pin's input amp is on its way in (the mic boost), not out,
 * and is left alone. */
static status_t mute_all(struct hda *h, unsigned cad, const struct widget *w)
{
    const uint32_t both = AMP_SET_LEFT | AMP_SET_RIGHT | AMP_MUTE;
    status_t st = OK;
    if (w->caps & WCAP_OUT_AMP)
        st = hda_set(h, cad, w->nid, V4_SET_AMP, AMP_SET_OUT | both);
    unsigned t = WCAP_TYPE(w->caps), n = w->nconn < MAX_CONN ? w->nconn : MAX_CONN;
    if (!(w->caps & WCAP_IN_AMP) || (t != W_MIXER && t != W_SELECTOR))
        return st;
    if (n > 16)
        n = 16;   /* the index is 4 bits */
    for (unsigned i = 0; i < n && st == OK; i++)
        st = hda_set(h, cad, w->nid, V4_SET_AMP, AMP_SET_IN | AMP_SET_INDEX(i) | both);
    return st;
}

/* Power, then pin output off, then mutes, then selects: nothing on the
 * path is ever connected while unmuted. */
status_t hda_path_program_muted(struct hda *h, const struct codec *c, const struct path *p)
{
    unsigned cad = c->cad;
    const struct widget *pin = p->n ? hda_widget(c, p->nid[p->n - 1]) : NULL;
    if (!pin)
        return ERR_NOT_FOUND;
    status_t st = hda_power_up(h, cad, c->afg);
    for (unsigned i = 0; i < p->n && st == OK; i++) {
        const struct widget *w = hda_widget(c, p->nid[i]);
        if (w->caps & WCAP_POWER)
            st = hda_power_up(h, cad, w->nid);
    }
    if (st == OK)
        st = hda_set(h, cad, pin->nid, V_SET_PIN_CTL,
                     pin->pin_ctl & ~(uint32_t)(PINCTL_OUT | PINCTL_HP) & 0xe7u);
    for (unsigned i = 0; i < p->n && st == OK; i++)
        st = mute_all(h, cad, hda_widget(c, p->nid[i]));
    for (unsigned i = 1; i < p->n && st == OK; i++) {
        const struct widget *w = hda_widget(c, p->nid[i]);
        if (WCAP_TYPE(w->caps) != W_MIXER && w->nconn > 1)
            st = hda_set(h, cad, w->nid, V_SET_CONN_SEL, p->in[i]);
    }
    return st;
}

void hda_path_read_back(struct hda *h, struct codec *c, const struct path *p)
{
    uint32_t v = 0;
    if (hda_get(h, c->cad, c->afg, V_GET_POWER, 0, &v) == OK)
        c->afg_power = (uint8_t)v;
    for (unsigned i = 0; i < c->nw; i++)
        for (unsigned k = 0; k < p->n; k++)
            if (c->w[i].nid == p->nid[k])
                hda_read_widget(h, c, &c->w[i]);
}

/* ---- the output: opened while a stream plays ---------------------------------------
 * Steps 4-6 of the path in docs/A1-PLAN.md, and their reverse. On the
 * PC's ALC897: mixer 0c's input 0 (from DAC 02) unmuted, DAC 02's output
 * amp to the gain (it has no mute: step 0 is its quietest, -65.25 dB),
 * pin 1b's output amp unmuted (it has only a mute), pin 1b's control to
 * output + headphone amp (0xc0), EAPD on. Nothing but the path's nodes is
 * touched, and the other inputs of its mixers stay muted. */

/* An amp's 0 dB step, or its top one if it can't reach 0 dB. */
static unsigned unity(uint32_t amp)
{
    unsigned off = AMPCAP_OFFSET(amp), steps = AMPCAP_STEPS(amp);
    return off < steps ? off : steps;
}

/* The step of the volume amp nearest `cb` centibels, in [0, unity]. */
static unsigned step_for(uint32_t amp, int32_t cb)
{
    int32_t size = (int32_t)AMPCAP_STEP_MDB(amp), mdb = cb * 100;
    int32_t d = mdb >= 0 ? (mdb + size / 2) / size : -((-mdb + size / 2) / size);
    int32_t step = (int32_t)AMPCAP_OFFSET(amp) + d, top = (int32_t)unity(amp);
    return step < 0 ? 0 : step > top ? (unsigned)top : (unsigned)step;
}

/* Step `step` of amp in centibels, rounded half away from zero. */
static int32_t cb_of(uint32_t amp, unsigned step)
{
    int32_t mdb = ((int32_t)step - (int32_t)AMPCAP_OFFSET(amp)) * (int32_t)AMPCAP_STEP_MDB(amp);
    return mdb >= 0 ? (mdb + 50) / 100 : -((-mdb + 50) / 100);
}

void hda_output_init(struct output *o, const struct codec *c, const struct path *p)
{
    *o = (struct output){ 0 };
    if (!c || !p || p->rule == PATH_NONE || !p->n)
        return;
    o->c = c;
    o->p = p;
    for (unsigned i = 0; i < p->n && !o->vol; i++) {
        const struct widget *w = hda_widget(c, p->nid[i]);
        if (w && (w->caps & WCAP_OUT_AMP) && AMPCAP_STEPS(w->amp_out)) {
            o->vol = w->nid;
            o->vol_amp = w->amp_out;
        }
    }
    if (o->vol)
        o->step = (uint8_t)step_for(o->vol_amp, GAIN_DEFAULT_CB);
}

/* The pin at the path's end, as read back after the muted set-up. */
static const struct widget *out_pin(const struct output *o)
{
    return hda_widget(o->c, o->p->nid[o->p->n - 1]);
}

static const uint32_t LR = AMP_SET_LEFT | AMP_SET_RIGHT;

/* One SET of the opening; on failure the step is named in the log. */
static status_t open_set(struct hda *h, const struct output *o, unsigned nid, uint32_t verb,
                         uint32_t payload, const char *what)
{
    status_t st = hda_set(h, o->c->cad, nid, verb, payload);
    if (st != OK)
        drv_log("output: %s of node %02x failed (%s)", what, nid, status_str(st));
    return st;
}

status_t hda_output_open(struct hda *h, struct output *o)
{
    if (!o->c)
        return ERR_NOT_FOUND;
    const struct path *p = o->p;
    unsigned cad = o->c->cad;
    const struct widget *pin = out_pin(o);
    status_t st = OK;
    o->open = true;   /* from here on hda_output_close undoes what was done */
    o->failed = false;
    /* Step 4: the path's input on each mixer and selector after the DAC. */
    for (unsigned i = 1; i < p->n && st == OK; i++) {
        const struct widget *w = hda_widget(o->c, p->nid[i]);
        unsigned t = WCAP_TYPE(w->caps);
        if ((t == W_MIXER || t == W_SELECTOR) && (w->caps & WCAP_IN_AMP) && p->in[i] < 16)
            st = open_set(h, o, w->nid, V4_SET_AMP,
                          AMP_SET_IN | AMP_SET_INDEX(p->in[i]) | LR | unity(w->amp_in),
                          "the input amp");
    }
    /* Step 5: the output amps, the volume amp at the gain, the rest at 0 dB. */
    for (unsigned i = 0; i < p->n && st == OK; i++) {
        const struct widget *w = hda_widget(o->c, p->nid[i]);
        if (w->caps & WCAP_OUT_AMP)
            st = open_set(h, o, w->nid, V4_SET_AMP,
                          AMP_SET_OUT | LR | (w->nid == o->vol ? o->step : unity(w->amp_out)),
                          "the output amp");
    }
    /* Step 6: the pin's output (and headphone amp), then EAPD. */
    uint32_t ctl = PINCTL_OUT | (pin->pincaps & PINCAP_HP ? PINCTL_HP : 0);
    if (st == OK)
        st = open_set(h, o, pin->nid, V_SET_PIN_CTL, ctl, "the pin control");
    if (st == OK && (pin->pincaps & PINCAP_EAPD))
        st = open_set(h, o, pin->nid, V_SET_EAPD, (pin->eapd | 0x2u) & 0x7u, "EAPD");
    if (st != OK) {
        o->failed = true;
        (void)hda_output_close(h, o);
        drv_log("output: not unmuted: the path is muted again");
        return st;
    }
    /* What the codec took, for the log (the PC's is the one that matters). */
    uint32_t rc = 0, re = 0, ra = 0;
    (void)hda_get(h, cad, pin->nid, V_GET_PIN_CTL, 0, &rc);
    if (pin->pincaps & PINCAP_EAPD)
        (void)hda_get(h, cad, pin->nid, V_GET_EAPD, 0, &re);
    if (o->vol)
        (void)hda_get(h, cad, o->vol, V4_GET_AMP, AMP_GET_OUT | AMP_GET_LEFT, &ra);
    char at[48] = "at 0 dB (no amp on the path has gain steps)";
    if (o->vol) {
        char db[16];
        hda_db_str(db, sizeof(db), cb_of(o->vol_amp, o->step));
        drv_snprintf(at, sizeof(at), "at %s dB (node %02x step %u)", db, o->vol, o->step);
    }
    drv_log("output: unmuted %s; read back: pin %02x ctl %02x eapd %02x, volume amp %02x", at,
            pin->nid, rc & 0xffu, re & 0xffu, ra & 0xffu);
    return OK;
}

status_t hda_output_close(struct hda *h, struct output *o)
{
    if (!o->open)
        return OK;
    const struct path *p = o->p;
    const struct widget *pin = out_pin(o);
    status_t first = OK, st;
#define TRY(x) do { if ((st = (x)) != OK && first == OK) first = st; } while (0)
    if (pin->pincaps & PINCAP_EAPD)
        TRY(hda_set(h, o->c->cad, pin->nid, V_SET_EAPD, pin->eapd & 0x5u));
    TRY(hda_set(h, o->c->cad, pin->nid, V_SET_PIN_CTL,
                pin->pin_ctl & ~(uint32_t)(PINCTL_OUT | PINCTL_HP) & 0xe7u));
    for (unsigned i = p->n; i-- > 0;) {
        const struct widget *w = hda_widget(o->c, p->nid[i]);
        if (w->caps & WCAP_OUT_AMP)
            TRY(hda_set(h, o->c->cad, w->nid, V4_SET_AMP, AMP_SET_OUT | LR | AMP_MUTE));
    }
    for (unsigned i = p->n; i-- > 1;) {
        const struct widget *w = hda_widget(o->c, p->nid[i]);
        unsigned t = WCAP_TYPE(w->caps);
        if ((t == W_MIXER || t == W_SELECTOR) && (w->caps & WCAP_IN_AMP) && p->in[i] < 16)
            TRY(hda_set(h, o->c->cad, w->nid, V4_SET_AMP,
                        AMP_SET_IN | AMP_SET_INDEX(p->in[i]) | LR | AMP_MUTE));
    }
#undef TRY
    o->open = false;
    if (first != OK)
        drv_log("output: muting again: a verb failed (%s)", status_str(first));
    else if (!o->failed)
        drv_log("output: muted again");
    return first;
}

status_t hda_output_set_gain(struct hda *h, struct output *o, int32_t cb)
{
    if (!o->c)
        return ERR_NOT_FOUND;
    if (!o->vol)
        return ERR_NOT_SUPPORTED;
    o->step = (uint8_t)step_for(o->vol_amp, cb);
    if (!o->open)
        return OK;
    return hda_set(h, o->c->cad, o->vol, V4_SET_AMP, AMP_SET_OUT | LR | o->step);
}

status_t hda_output_gain(const struct output *o, int32_t *cb, int32_t *min, int32_t *max)
{
    if (!o->c)
        return ERR_NOT_FOUND;
    if (!o->vol)
        return ERR_NOT_SUPPORTED;
    *cb = cb_of(o->vol_amp, o->step);
    *min = cb_of(o->vol_amp, 0);
    *max = cb_of(o->vol_amp, unity(o->vol_amp));
    return OK;
}
