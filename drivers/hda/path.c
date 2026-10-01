/* hda: the path from a DAC to the headphones (spec chapter 7), found in a
 * codec's graph as graph.c read it. Pure: struct codec in, struct path
 * out, no verbs, so fixtures.c can check it against codecs read elsewhere
 * (the PC's included).
 *
 * The pin, by its configuration default (the board's own description of
 * its jacks), best first:
 *   1. a headphone jack on the front panel: default device hp-out,
 *      location external front, port connectivity jack;
 *   2. any hp-out pin (a board that describes its front jack badly);
 *   3. a line-out pin (QEMU's hda-duplex and hda-output have only that);
 *   4. a speaker pin (QEMU's hda-micro).
 * A pin qualifies only if it is an analog output (pin caps "out", not
 * digital, HDMI or DisplayPort) and its connectivity is not "none". Within
 * a rule the lowest association, then sequence, then node id wins (the
 * spec's order for jacks that belong together). A pin that reaches no DAC
 * is passed over for the next.
 *
 * The walk: breadth-first back from the pin through connection lists,
 * across mixers and selectors only, at most PATH_MAX_NODES - 1 widgets
 * deep, to an analog DAC. It never enters a widget whose connection list
 * holds a pin: that is an input or loopback mixer (the ALC897's 0b sums
 * the jacks' inputs), and routing through it would put the inputs on the
 * headphones. Of the DACs reached, the one no other pin with its output on
 * already uses wins, then the shortest path, then the lowest node.
 *
 * On the PC's ALC897 this is DAC 02 -> mixer 0c -> pin 1b. Mixer 0c also
 * feeds the rear line-out pin 14 (its only input) and is the selected
 * input of pins 18, 19, 1a: any of them with its output on would play the
 * same sound. Their outputs are off, and the driver leaves them so;
 * struct path's `also` lists them so the log says so. */
#include "hda.h"

#define NODES 128   /* node ids are 7 bits */

/* A breadth-first walk from one node back through connection lists. */
struct walk {
    bool    seen[NODES];
    uint8_t depth[NODES];   /* widgets from the start */
    uint8_t prev[NODES];    /* the node it was reached from (toward the start) */
    uint8_t via[NODES];     /* its index in prev's connection list */
};

static unsigned wtype(const struct widget *w)
{
    return WCAP_TYPE(w->caps);
}

static unsigned nconn(const struct widget *w)
{
    return w->nconn < MAX_CONN ? w->nconn : MAX_CONN;
}

/* A mixer or selector whose connection list holds a pin: it sums inputs. */
static bool takes_pin(const struct codec *c, const struct widget *w)
{
    for (unsigned k = 0; k < nconn(w); k++) {
        const struct widget *v = hda_widget(c, w->conn[k]);
        if (v && wtype(v) == W_PIN)
            return true;
    }
    return false;
}

static bool analog_dac(const struct widget *w)
{
    return wtype(w) == W_OUT && !(w->caps & WCAP_DIGITAL);
}

/* The connections the walk follows from w: all of them, or with
 * `selected` only the one a pin or selector has selected now (a mixer
 * sums all of its inputs either way). *first, *end: the index range. */
static void follow(const struct widget *w, bool selected, unsigned *first, unsigned *end)
{
    *first = 0;
    *end = nconn(w);
    if (selected && wtype(w) != W_MIXER && w->has_sel && w->conn_sel < *end) {
        *first = w->conn_sel;
        *end = w->conn_sel + 1u;
    }
}

/* Visit v, reached from u through u's connection k. */
static void visit(const struct codec *c, struct walk *wk, unsigned u, unsigned k, unsigned v,
                  unsigned *queue, unsigned *tail)
{
    const struct widget *w = hda_widget(c, v);
    if (v >= NODES || wk->seen[v] || !w)
        return;
    wk->seen[v] = true;
    wk->depth[v] = (uint8_t)(wk->depth[u] + 1);
    wk->prev[v] = (uint8_t)u;
    wk->via[v] = (uint8_t)k;
    unsigned t = wtype(w);
    if ((t == W_MIXER || t == W_SELECTOR) && !takes_pin(c, w) &&
        wk->depth[v] + 1u < PATH_MAX_NODES)
        queue[(*tail)++] = v;   /* each node is queued once: tail < NODES */
}

static void walk(const struct codec *c, unsigned start, bool selected, struct walk *wk)
{
    unsigned queue[NODES], head = 0, tail = 0;
    *wk = (struct walk){ .seen = { false } };
    wk->seen[start] = true;
    queue[tail++] = start;
    while (head < tail) {
        unsigned u = queue[head++], first, end;
        const struct widget *w = hda_widget(c, u);
        follow(w, selected, &first, &end);
        for (unsigned k = first; k < end; k++)
            visit(c, wk, u, k, w->conn[k], queue, &tail);
    }
}

/* The rule pin w qualifies under, or PATH_NONE. */
static unsigned pin_rule(const struct widget *w)
{
    uint32_t cfg = w->config;
    if (wtype(w) != W_PIN || (w->caps & WCAP_DIGITAL) || !(w->pincaps & PINCAP_OUT) ||
        (w->pincaps & (PINCAP_HDMI | PINCAP_DP)) || CFG_CONN(cfg) == CONN_NONE)
        return PATH_NONE;
    if (CFG_DEVICE(cfg) == DEV_HP_OUT && CFG_LOCATION(cfg) == LOC_EXT_FRONT &&
        CFG_CONN(cfg) == CONN_JACK)
        return PATH_FRONT_HP;
    if (CFG_DEVICE(cfg) == DEV_HP_OUT)
        return PATH_HP;
    if (CFG_DEVICE(cfg) == DEV_LINE_OUT)
        return PATH_LINE_OUT;
    if (CFG_DEVICE(cfg) == DEV_SPEAKER)
        return PATH_SPEAKER;
    return PATH_NONE;
}

/* The order pins are tried in: rule, association, sequence, node. */
static uint32_t pin_key(const struct widget *w)
{
    return pin_rule(w) << 16 | CFG_ASSOC(w->config) << 12 | CFG_SEQ(w->config) << 8 | w->nid;
}

/* The DACs other pins with their output on reach through what they have
 * selected now: used[dac] = true. */
static void mark_used(const struct codec *c, unsigned pin, bool *used)
{
    struct walk wk;
    for (unsigned i = 0; i < c->nw; i++) {
        const struct widget *w = &c->w[i];
        if (w->nid == pin || wtype(w) != W_PIN || !(w->pin_ctl & PINCTL_OUT))
            continue;
        walk(c, w->nid, true, &wk);
        for (unsigned v = 0; v < NODES; v++)
            if (wk.seen[v] && hda_widget(c, v) && analog_dac(hda_widget(c, v)))
                used[v] = true;
    }
}

/* The best DAC the walk from the pin reached, or 0. */
static unsigned best_dac(const struct codec *c, const struct walk *wk, const bool *used)
{
    unsigned best = 0;
    uint32_t best_key = ~0u;
    for (unsigned v = 0; v < NODES; v++) {
        const struct widget *w = wk->seen[v] ? hda_widget(c, v) : NULL;
        if (!w || !analog_dac(w) || wk->depth[v] == 0)
            continue;
        uint32_t key = (uint32_t)used[v] << 16 | (uint32_t)wk->depth[v] << 8 | v;
        if (key < best_key) {
            best_key = key;
            best = v;
        }
    }
    return best;
}

/* Other output-capable pins whose selected input is a node of the path. */
static void find_also(const struct codec *c, struct path *p)
{
    unsigned pin = p->nid[p->n - 1];
    for (unsigned i = 0; i < c->nw && p->nalso < PATH_MAX_ALSO; i++) {
        const struct widget *w = &c->w[i];
        if (w->nid == pin || wtype(w) != W_PIN || !(w->pincaps & PINCAP_OUT) || !nconn(w))
            continue;
        unsigned sel = w->has_sel && w->conn_sel < nconn(w) ? w->conn_sel : 0;
        for (unsigned j = 0; j + 1 < p->n; j++)
            if (w->conn[sel] == p->nid[j])
                p->also[p->nalso++] = w->nid;
    }
}

/* The path from pin to its best DAC into *p. false: it reaches none. */
static bool path_from(const struct codec *c, const struct widget *pin, struct path *p)
{
    struct walk wk;
    bool used[NODES] = { false };
    mark_used(c, pin->nid, used);
    walk(c, pin->nid, false, &wk);
    unsigned dac = best_dac(c, &wk, used);
    if (!dac)
        return false;
    unsigned n = wk.depth[dac] + 1u;
    *p = (struct path){ .cad = c->cad, .rule = (uint8_t)pin_rule(pin), .n = (uint8_t)n,
                        .dac_shared = used[dac] };
    unsigned v = dac;
    for (unsigned i = 0; i < n; i++) {
        p->nid[i] = (uint8_t)v;
        if (i > 0)
            p->in[i] = wk.via[p->nid[i - 1]];
        v = wk.prev[v];
    }
    find_also(c, p);
    return true;
}

status_t hda_path_find(const struct codec *c, struct path *out)
{
    uint32_t tried = 0;   /* pins with keys up to this one reach no DAC (every key is > 0) */
    for (unsigned round = 0; round < c->nw; round++) {
        const struct widget *next = NULL;
        for (unsigned i = 0; i < c->nw; i++) {
            const struct widget *w = &c->w[i];
            if (pin_rule(w) != PATH_NONE && pin_key(w) > tried &&
                (!next || pin_key(w) < pin_key(next)))
                next = w;
        }
        if (!next)
            break;
        if (path_from(c, next, out))
            return OK;
        tried = pin_key(next);
    }
    *out = (struct path){ .cad = c->cad, .rule = PATH_NONE };
    return ERR_NOT_FOUND;
}
