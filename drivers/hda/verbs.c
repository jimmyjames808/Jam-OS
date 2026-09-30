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
 * The one sequence so far programs the path path.c found, silent: power
 * up, the pin's output off, every amp on the path muted, the path's
 * inputs selected. The pin's output and the unmuting come with the
 * stream; the stream tag and format of the DAC are left as they are. */
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
