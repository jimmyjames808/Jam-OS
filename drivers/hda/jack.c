/* hda: the jacks. Which pins are jacks with presence detection, their
 * unsolicited response tags, reading their presence, the debounce, the
 * polling fallback and the log lines (spec 7.3: Unsolicited Response,
 * Pin Sense; 7.3.4.9: pin capabilities).
 *
 * The jacks: every pin whose configuration default says a jack is there
 * (port connectivity jack, or jack and fixed), whose configuration
 * default does not say "no presence detection" (misc bit 8) and whose
 * pin capabilities have Presence Detect Capable. On the PC's ALC897 that
 * is seven: the rear line-outs 14 (green), 15 (black), 16 (orange), the
 * rear mic 18, the front mic 19, the rear line-in 1a and the front
 * headphones 1b. Each gets a tag of its own, in node order from 1, so an
 * unsolicited response (tag in bits 31:26) names its pin.
 *
 * Presence: GET_PIN_SENSE, bit 31. A pin whose capabilities have Trigger
 * Required gets SET_PIN_SENSE (Execute, right channel 0) first; the spec
 * ties the trigger to the impedance measurement (bits 30:0) and gives no
 * settle time, so the driver triggers every such pin of a read at once,
 * waits JACK_SETTLE_NS, then reads them. The ALC897's jack pins all have
 * the trigger bit and none the impedance one: the trigger costs one verb.
 *
 * Unsolicited responses (if the controller takes them: the rings and the
 * MSI): SET_UNSOLICITED_ENABLE (bit 7 on, 5:0 the tag) on every jack pin
 * whose widget capabilities say it can send them; the RIRB interrupt
 * queues them (ctrl.c) and hda_jacks_run reads that pin's presence at once.
 *
 * The debounce: a presence that differs from the jack's state starts a
 * pending change; it is read again JACK_DEBOUNCE_NS later and the change
 * is kept (and logged) only if it still differs then. A read that agrees
 * with the state in between cancels it, so a plug's contact bounce gives
 * one change, not several.
 *
 * The polling fallback, per jack: a pin without unsolicited responses is
 * read every JACK_POLL_NS. A pin with them is polled too until they are
 * proven: its first unsolicited response, with the RIRB interrupt seen
 * working, stops its polling. A change the poll finds with no unsolicited
 * response from that pin since the last change means they don't work
 * there: it stays polled for good (logged). So detection works either way,
 * and the log says which way each jack ended up.
 *
 * Nothing here changes what is heard: the only SET verbs are
 * SET_UNSOLICITED_ENABLE and SET_PIN_SENSE (the self-test checks that
 * against a fake codec). An unplug while playing is logged and nothing
 * else: the stream and the mixer keep running. */
#include "hda.h"

/* ---- which pins, and their names ------------------------------------------------ */

static bool is_jack(const struct widget *w)
{
    unsigned conn = CFG_CONN(w->config);
    return WCAP_TYPE(w->caps) == W_PIN && (w->pincaps & PINCAP_PRESENCE) &&
           (conn == CONN_JACK || conn == 3) && !(w->config & CFG_NO_PRESENCE);
}

void hda_jacks_add(struct jacks *js, const struct codec *c)
{
    for (unsigned i = 0; i < c->nw && js->n < MAX_JACKS; i++) {
        const struct widget *w = &c->w[i];
        if (!is_jack(w))
            continue;
        struct jack *j = &js->j[js->n++];
        *j = (struct jack){ .cad = c->cad, .nid = w->nid, .tag = (uint8_t)js->n,
                            .config = w->config, .pincaps = w->pincaps,
                            .can_unsol = (w->caps & WCAP_UNSOL) != 0 };
    }
}

struct jack *hda_jack_for(struct jacks *js, unsigned cad, uint32_t resp)
{
    unsigned tag = UNSOL_TAG(resp);
    for (unsigned i = 0; i < js->n; i++)
        if (js->j[i].cad == cad && js->j[i].tag == tag && tag)
            return &js->j[i];
    return NULL;
}

const char *hda_jack_name(const struct jack *j)
{
    static const char *const names[16] = {
        "line-out", "speaker", "headphones", "CD input", "S/PDIF out", "digital out",
        "modem line", "modem handset", "line-in", "aux input", "microphone", "telephony",
        "S/PDIF in", "digital in", "device 14", "device 15",
    };
    return names[CFG_DEVICE(j->config)];
}

/* The location (configuration default 29:24): its side, and "internal"
 * etc. for the gross locations other than external. */
static const char *side(uint32_t config)
{
    static const char *const sides[16] = {
        "", "rear", "front", "left", "right", "top", "bottom", "special",
        "special", "special", "", "", "", "", "", "",
    };
    unsigned loc = CFG_LOCATION(config);
    if (loc >> 4 == 1)
        return "internal";
    return sides[loc & 0xf][0] ? sides[loc & 0xf] : "somewhere";
}

static const char *colour(uint32_t config)
{
    static const char *const colours[16] = {
        "", "black", "grey", "blue", "green", "red", "orange", "yellow",
        "purple", "pink", "", "", "", "", "white", "",
    };
    return colours[config >> 12 & 0xf];
}

void hda_jack_where(const struct jacks *js, const struct jack *j, char *buf, size_t size)
{
    bool twin = false;
    for (unsigned i = 0; i < js->n; i++) {
        const struct jack *k = &js->j[i];
        if (k != j && CFG_DEVICE(k->config) == CFG_DEVICE(j->config) &&
            CFG_LOCATION(k->config) == CFG_LOCATION(j->config))
            twin = true;
    }
    const char *c = colour(j->config);
    drv_snprintf(buf, size, "%s%s%s", side(j->config), twin && c[0] ? " " : "", twin ? c : "");
}

/* ---- the state of one jack (pure) -------------------------------------------------- */

/* One presence read at `now`; true if the state changed (the first read
 * always sets it; a change after that only once it has held a debounce). */
static bool jack_sample(struct jack *j, bool present, uint64_t now)
{
    uint8_t s = present ? JACK_IN : JACK_OUT;
    if (j->state == JACK_UNKNOWN) {
        j->state = s;
        j->pending = false;
        return true;
    }
    if (s == j->state) {
        j->pending = false;
        return false;
    }
    if (!j->pending) {
        j->pending = true;
        j->pending_at = now;
        return false;
    }
    if (now - j->pending_at < JACK_DEBOUNCE_NS)
        return false;
    j->state = s;
    j->pending = false;
    j->changes++;
    return true;
}

static bool polled(const struct jack *j)
{
    return j->mode != JM_UNSOL;
}

uint64_t hda_jacks_deadline(const struct jacks *js)
{
    uint64_t d = DEADLINE_NEVER;
    for (unsigned i = 0; i < js->n; i++) {
        const struct jack *j = &js->j[i];
        if (j->pending && j->pending_at + JACK_DEBOUNCE_NS < d)
            d = j->pending_at + JACK_DEBOUNCE_NS;
        if (polled(j) && js->next_poll < d)
            d = js->next_poll;
    }
    return d;
}

/* ---- reading, and the lines ------------------------------------------------------- */

static void say(const struct jack_io *io, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void say(const struct jack_io *io, const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    drv_vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    io->say(io->ctx, line);
}

const char *hda_jack_mode_str(unsigned mode)
{
    switch (mode) {
    case JM_TRY:    return "unsolicited responses on, polled until one comes";
    case JM_UNSOL:  return "unsolicited responses";
    case JM_MISSED: return "polled (a change came without an unsolicited response)";
    default:        return "polled";
    }
}

/* A change kept: its line, then what it says about the jack's mode. */
static void changed(struct jacks *js, const struct jack_io *io, struct jack *j, bool first)
{
    char where[32];
    hda_jack_where(js, j, where, sizeof(where));
    if (first)
        say(io, "jack: %s (%s, pin %02x): %s; %s, tag %u", hda_jack_name(j), where, j->nid,
            j->state == JACK_IN ? "plugged in" : "unplugged", hda_jack_mode_str(j->mode), j->tag);
    else
        say(io, "%s %s (%s, pin %02x)", hda_jack_name(j),
            j->state == JACK_IN ? "plugged in" : "unplugged", where, j->nid);
    if (!first && j->mode == JM_TRY && !j->heard) {
        j->mode = JM_MISSED;
        say(io, "jack: pin %02x changed without an unsolicited response: polled every %u ms "
            "from now on", j->nid, (unsigned)(JACK_POLL_NS / NS_PER_MS));
    }
    j->heard = false;
}

/* Every jack in `mask` read (the triggers first, one settle for them all)
 * and fed to its state at `now`. */
static void read_set(struct jacks *js, const struct jack_io *io, uint32_t mask, uint64_t now,
                     bool first)
{
    bool triggered = false;
    for (unsigned i = 0; i < js->n; i++) {
        struct jack *j = &js->j[i];
        if ((mask & (1u << i)) && (j->pincaps & PINCAP_TRIGGER) &&
            io->set(io->ctx, j->cad, j->nid, V_SET_PIN_SENSE, 0) == OK)
            triggered = true;
    }
    if (triggered)
        io->sleep(io->ctx, JACK_SETTLE_NS);
    for (unsigned i = 0; i < js->n; i++) {
        struct jack *j = &js->j[i];
        uint32_t v = 0;
        if (!(mask & (1u << i)))
            continue;
        if (io->get(io->ctx, j->cad, j->nid, V_GET_PIN_SENSE, 0, &v) != OK) {
            if (j->errors++ < 3)
                say(io, "jack: pin %02x: reading its presence failed", j->nid);
            continue;
        }
        if (jack_sample(j, (v >> 31) != 0, now))
            changed(js, io, j, first);
    }
}

void hda_jacks_start(struct jacks *js, const struct jack_io *io, bool unsol, uint64_t now)
{
    js->unsol = unsol;
    unsigned on = 0;
    for (unsigned i = 0; i < js->n; i++) {
        struct jack *j = &js->j[i];
        j->mode = JM_POLL;
        if (unsol && j->can_unsol &&
            io->set(io->ctx, j->cad, j->nid, V_SET_UNSOL, 0x80u | j->tag) == OK) {
            j->mode = JM_TRY;
            on++;
        }
    }
    if (!js->n) {
        say(io, "jacks: none with presence detection: nothing to watch");
        return;
    }
    say(io, "jacks: %u with presence detection; unsolicited responses %s; polled every %u ms "
        "%s", js->n, !unsol ? "off (no RIRB interrupt)" : on == js->n ? "on for all"
        : on ? "on for some" : "on for none (no pin can send them)",
        (unsigned)(JACK_POLL_NS / NS_PER_MS), on ? "until each one's first response" : "");
    read_set(js, io, (1u << js->n) - 1, now, true);
    js->next_poll = now + JACK_POLL_NS;
}

void hda_jacks_run(struct jacks *js, const struct jack_io *io, uint64_t now)
{
    uint32_t due = 0;
    unsigned cad;
    uint32_t resp;
    while (io->unsol(io->ctx, &cad, &resp)) {
        struct jack *j = hda_jack_for(js, cad, resp);
        if (!j) {
            if (js->stray++ < 3)
                say(io, "jacks: an unsolicited response %08x from codec %u has no jack's tag", resp,
                    cad);
            continue;
        }
        j->unsols++;
        j->heard = true;
        if (j->mode == JM_TRY && js->irq_seen) {
            j->mode = JM_UNSOL;
            say(io, "jack: pin %02x sends unsolicited responses: no longer polled", j->nid);
        }
        due |= 1u << (j - js->j);
    }
    for (unsigned i = 0; i < js->n; i++)
        if (js->j[i].pending && now - js->j[i].pending_at >= JACK_DEBOUNCE_NS)
            due |= 1u << i;
    if (now >= js->next_poll) {
        js->next_poll = now + JACK_POLL_NS;
        for (unsigned i = 0; i < js->n; i++)
            if (polled(&js->j[i]))
                due |= 1u << i;
    }
    if (due)
        read_set(js, io, due, now, false);
}

void hda_jacks_stop(struct jacks *js, const struct jack_io *io)
{
    for (unsigned i = 0; i < js->n; i++) {
        struct jack *j = &js->j[i];
        if (j->mode != JM_POLL)
            (void)io->set(io->ctx, j->cad, j->nid, V_SET_UNSOL, 0);
    }
}

/* ---- the driver's io ------------------------------------------------------------------ */

static status_t hw_set(void *ctx, unsigned cad, unsigned nid, uint32_t verb, uint32_t payload)
{
    return hda_set(ctx, cad, nid, verb, payload);
}

static status_t hw_get(void *ctx, unsigned cad, unsigned nid, uint32_t verb, uint32_t payload,
                       uint32_t *out)
{
    return hda_get(ctx, cad, nid, verb, payload, out);
}

/* The queue, and if it is empty the RIRB itself: a response the
 * interrupt has not brought yet (or never will, if it doesn't work). */
static bool hw_unsol(void *ctx, unsigned *cad, uint32_t *resp)
{
    struct hda *h = ctx;
    if (!h->uq_n)
        hda_rirb_drain(h);
    return hda_unsol_pop(h, cad, resp);
}

static void hw_sleep(void *ctx, uint64_t ns)
{
    (void)ctx;
    drv_sleep_until(drv_clock_ns() + ns);
}

static void hw_say(void *ctx, const char *line)
{
    (void)ctx;
    drv_log("%s", line);
}

void hda_jack_io(struct jack_io *io, struct hda *h)
{
    *io = (struct jack_io){ .ctx = h, .set = hw_set, .get = hw_get, .unsol = hw_unsol,
                            .sleep = hw_sleep, .say = hw_say };
}

/* ---- the self-test ---------------------------------------------------------------------
 * Run at every driver start, beside the path self-test: the jack table and
 * names on the fixtures, the RIRB's demultiplexer on a fake RIRB, and the
 * whole flow (start, unsolicited responses, debounce, the poll fallback,
 * stop) against a fake codec whose pins' presence the test sets, on a fake
 * clock. The fake codec records every verb: only SET_UNSOLICITED_ENABLE
 * and SET_PIN_SENSE to jack pins, and GET_PIN_SENSE, may be sent. */

struct fake {
    bool     present[128];      /* by node */
    uint32_t q[8];              /* unsolicited responses to hand out (codec 0) */
    unsigned qn;
    unsigned unsol_sets, triggers, gets, bad;
    uint8_t  unsol_now[128];    /* each pin's SET_UNSOLICITED_ENABLE payload now */
    char     last[160];         /* the last line said */
    unsigned lines;
};

static status_t fake_set(void *ctx, unsigned cad, unsigned nid, uint32_t verb, uint32_t payload)
{
    struct fake *f = ctx;
    if (cad != 0 || nid > 0x7f)
        f->bad++;
    else if (verb == V_SET_UNSOL && !(payload & ~0xbfu)) {
        f->unsol_sets++;
        f->unsol_now[nid] = (uint8_t)payload;
    } else if (verb == V_SET_PIN_SENSE && payload == 0)
        f->triggers++;
    else
        f->bad++;
    return OK;
}

static status_t fake_get(void *ctx, unsigned cad, unsigned nid, uint32_t verb, uint32_t payload,
                         uint32_t *out)
{
    struct fake *f = ctx;
    if (cad != 0 || nid > 0x7f || verb != V_GET_PIN_SENSE || payload) {
        f->bad++;
        return ERR_INVALID_ARGS;
    }
    f->gets++;
    *out = f->present[nid] ? 1u << 31 : 0x7fffffffu;
    return OK;
}

static bool fake_unsol(void *ctx, unsigned *cad, uint32_t *resp)
{
    struct fake *f = ctx;
    if (!f->qn)
        return false;
    *cad = 0;
    *resp = f->q[0];
    for (unsigned i = 1; i < f->qn; i++)
        f->q[i - 1] = f->q[i];
    f->qn--;
    return true;
}

static void fake_sleep(void *ctx, uint64_t ns)
{
    (void)ctx;
    (void)ns;
}

static void fake_say(void *ctx, const char *line)
{
    struct fake *f = ctx;
    drv_snprintf(f->last, sizeof(f->last), "%s", line);
    f->lines++;
}

static bool same_str(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The checks: each failure is one line into o (and the count of them). */
struct check {
    struct out *o;
    unsigned failed;
};

static void expect(struct check *k, bool ok, const char *what)
{
    if (ok)
        return;
    k->failed++;
    out_line(k->o, "jack self-test: %s", what);
}

static void expect_line(struct check *k, const struct fake *f, const char *want)
{
    if (same_str(f->last, want))
        return;
    k->failed++;
    out_line(k->o, "jack self-test: said \"%s\", want \"%s\"", f->last, want);
}

/* The table on the ALC897 and on QEMU's codecs. */
static void check_table(struct check *k, struct codec *c, struct jacks *js)
{
    static const char *const qemu[] = { "hda-output", "hda-duplex", "hda-micro" };
    for (unsigned i = 0; i < 3; i++) {
        *js = (struct jacks){ .n = 0 };
        expect(k, hda_fixture(qemu[i], c) == OK, "a QEMU fixture does not parse");
        hda_jacks_add(js, c);
        expect(k, js->n == 0, "a QEMU codec has a jack with presence detection");
    }
    *js = (struct jacks){ .n = 0 };
    expect(k, hda_fixture("alc897", c) == OK, "the ALC897 fixture does not parse");
    hda_jacks_add(js, c);
    static const uint8_t want[] = { 0x14, 0x15, 0x16, 0x18, 0x19, 0x1a, 0x1b };
    bool ok = js->n == sizeof(want);
    for (unsigned i = 0; ok && i < js->n; i++)
        ok = js->j[i].nid == want[i] && js->j[i].tag == i + 1 && js->j[i].can_unsol &&
             (js->j[i].pincaps & PINCAP_TRIGGER);
    expect(k, ok, "the ALC897's jacks are not 14 15 16 18 19 1a 1b, tags 1-7, unsol, trigger");
    if (!ok)
        return;
    char w[32];
    hda_jack_where(js, &js->j[6], w, sizeof(w));
    expect(k, same_str(hda_jack_name(&js->j[6]), "headphones") && same_str(w, "front"),
           "pin 1b is not \"headphones\" at \"front\"");
    hda_jack_where(js, &js->j[0], w, sizeof(w));
    expect(k, same_str(hda_jack_name(&js->j[0]), "line-out") && same_str(w, "rear green"),
           "pin 14 is not \"line-out\" at \"rear green\"");
    hda_jack_where(js, &js->j[4], w, sizeof(w));
    expect(k, same_str(hda_jack_name(&js->j[4]), "microphone") && same_str(w, "front"),
           "pin 19 is not \"microphone\" at \"front\"");
    expect(k, hda_jack_for(js, 0, 7u << 26) == &js->j[6] &&
              hda_jack_for(js, 0, 7u << 26 | 0x3ffffffu) == &js->j[6] &&
              !hda_jack_for(js, 1, 7u << 26) && !hda_jack_for(js, 0, 0) &&
              !hda_jack_for(js, 0, 9u << 26),
           "an unsolicited response's tag finds the wrong jack");
}

/* The RIRB's demultiplexer on fake entries (no registers touched). */
static void check_rirb(struct check *k)
{
    struct hda h = { .rings = false };
    uint32_t out = 0, resp = 0;
    unsigned cad = 9;
    const uint64_t solicited0 = 0, unsol0 = 0x10ull << 32, solicited2 = 2ull << 32;
    expect(k, !hda_rirb_sort(&h, solicited0 | 0xaaaa, -1, &out) && h.late == 1,
           "an answer no command waits for is not counted late");
    expect(k, !hda_rirb_sort(&h, unsol0 | 7u << 26, 0, &out) && h.unsol == 1 && h.uq_n == 1,
           "an unsolicited response is taken for an answer, or not queued");
    expect(k, !hda_rirb_sort(&h, solicited2 | 0xbbbb, 0, &out) && h.late == 2,
           "codec 2's answer is taken for codec 0's");
    expect(k, hda_rirb_sort(&h, solicited0 | 0x1234, 0, &out) && out == 0x1234,
           "codec 0's answer is not taken");
    expect(k, hda_unsol_pop(&h, &cad, &resp) && cad == 0 && resp == 7u << 26 &&
              !hda_unsol_pop(&h, &cad, &resp),
           "the unsolicited queue does not give back what it got");
    for (unsigned i = 0; i < UNSOL_Q + 3; i++)
        (void)hda_rirb_sort(&h, unsol0 | i, -1, &out);
    expect(k, h.uq_n == UNSOL_Q && h.unsol_dropped == 3 && hda_unsol_pop(&h, &cad, &resp) &&
              resp == 0,
           "a full unsolicited queue does not drop the newest, counted");
}

static void push(struct fake *f, unsigned tag)
{
    if (f->qn < 8)
        f->q[f->qn++] = (uint32_t)tag << 26;
}

/* The flow on the ALC897's jacks (js from check_table), the clock in ms. */
static void check_flow(struct check *k, struct jacks *js)
{
    struct fake *f = drv_malloc(sizeof(*f));
    if (!f) {
        expect(k, false, "no memory");
        return;
    }
    *f = (struct fake){ .qn = 0 };
    struct jack_io io = { .ctx = f, .set = fake_set, .get = fake_get, .unsol = fake_unsol,
                          .sleep = fake_sleep, .say = fake_say };
    struct jack *hp = &js->j[6], *mic = &js->j[4];
    const uint64_t ms = NS_PER_MS;
    f->present[0x14] = true;   /* the rear speakers are in */
    hda_jacks_start(js, &io, true, 1000 * ms);
    expect(k, f->unsol_sets == 7 && f->unsol_now[0x1b] == 0x87 && f->triggers == 7 && f->gets == 7,
           "start: not 7 unsolicited enables (1b: 0x87), 7 triggers, 7 reads");
    expect(k, hp->state == JACK_OUT && js->j[0].state == JACK_IN && hp->mode == JM_TRY &&
              hp->changes == 0,
           "start: the first read did not set the states");
    expect_line(k, f, "jack: headphones (front, pin 1b): unplugged; unsolicited responses on, "
                "polled until one comes, tag 7");
    expect(k, hda_jacks_deadline(js) == 1500 * ms, "start: the next poll is not in 500 ms");

    /* Headphones in, the unsolicited response comes (the interrupt works). */
    js->irq_seen = 1;
    f->present[0x1b] = true;
    push(f, 7);
    hda_jacks_run(js, &io, 1100 * ms);
    expect(k, hp->mode == JM_UNSOL && hp->pending && hp->state == JACK_OUT,
           "plug: the response did not prove the pin, or the change was not held");
    expect(k, hda_jacks_deadline(js) == 1180 * ms, "plug: the debounce read is not 80 ms later");
    hda_jacks_run(js, &io, 1150 * ms);
    expect(k, hp->state == JACK_OUT, "plug: kept before the debounce");
    hda_jacks_run(js, &io, 1180 * ms);
    expect(k, hp->state == JACK_IN && hp->changes == 1, "plug: not kept after the debounce");
    expect_line(k, f, "headphones plugged in (front, pin 1b)");

    /* Contact bounce: out for 20 ms, then in again: no change. */
    f->present[0x1b] = false;
    push(f, 7);
    hda_jacks_run(js, &io, 1200 * ms);
    f->present[0x1b] = true;
    push(f, 7);
    hda_jacks_run(js, &io, 1220 * ms);
    hda_jacks_run(js, &io, 1300 * ms);
    expect(k, hp->state == JACK_IN && hp->changes == 1 && !hp->pending,
           "bounce: a 20 ms flicker was taken for a change");

    /* Proven: the headphones are not polled; a change with no response
     * is not seen (the codec would send one). */
    unsigned gets = f->gets;
    f->present[0x1b] = false;
    hda_jacks_run(js, &io, 1500 * ms);   /* the poll */
    hda_jacks_run(js, &io, 1600 * ms);
    expect(k, hp->state == JACK_IN && f->gets - gets == 6,
           "proven: the headphones were polled (or the other 6 were not)");
    push(f, 7);
    hda_jacks_run(js, &io, 1700 * ms);
    hda_jacks_run(js, &io, 1780 * ms);
    expect(k, hp->state == JACK_OUT && hp->changes == 2, "unplug: not kept");
    expect_line(k, f, "headphones unplugged (front, pin 1b)");

    /* The front mic changes with no unsolicited response: the poll finds
     * it, and it is polled for good. */
    f->present[0x19] = true;
    hda_jacks_run(js, &io, 2000 * ms);   /* the poll: pending */
    expect(k, mic->pending && mic->state == JACK_OUT, "poll: the mic's change was not held");
    hda_jacks_run(js, &io, 2080 * ms);
    expect(k, mic->state == JACK_IN && mic->mode == JM_MISSED, "poll: the mic was not changed "
           "and moved to polling");
    expect_line(k, f, "jack: pin 19 changed without an unsolicited response: polled every 500 "
                "ms from now on");
    expect(k, hda_jacks_deadline(js) == 2500 * ms, "poll: the next poll is not at 2500 ms");

    /* A tag no jack has is counted, not taken. */
    push(f, 12);
    hda_jacks_run(js, &io, 2100 * ms);
    expect(k, js->stray == 1, "a stray tag was not counted");

    /* Stop: unsolicited responses off on the 7 pins. */
    unsigned sets = f->unsol_sets;
    hda_jacks_stop(js, &io);
    expect(k, f->unsol_sets - sets == 7 && !f->unsol_now[0x1b] && !f->unsol_now[0x14],
           "stop: unsolicited responses not turned off on every pin");

    /* No interrupt (the immediate interface, or no MSI): polled only, and
     * no SET_UNSOLICITED_ENABLE at all. */
    for (unsigned i = 0; i < js->n; i++)
        js->j[i] = (struct jack){ .cad = js->j[i].cad, .nid = js->j[i].nid, .tag = js->j[i].tag,
                                  .config = js->j[i].config, .pincaps = js->j[i].pincaps,
                                  .can_unsol = true };
    js->irq_seen = 0;
    sets = f->unsol_sets;
    f->present[0x1b] = false;
    hda_jacks_start(js, &io, false, 3000 * ms);
    f->present[0x1b] = true;
    hda_jacks_run(js, &io, 3500 * ms);
    hda_jacks_run(js, &io, 3580 * ms);
    expect(k, f->unsol_sets == sets && hp->mode == JM_POLL && hp->state == JACK_IN &&
              hp->changes == 1,
           "polling only: the plug was not found in 500 ms + the debounce, or an enable was sent");
    expect_line(k, f, "headphones plugged in (front, pin 1b)");
    expect(k, f->bad == 0, "a verb other than 708/709 to a jack pin or 0xf09 was sent");
    drv_free(f);
}

bool hda_jack_selftest(struct codec *scratch, struct out *o)
{
    struct check k = { .o = o };
    struct jacks *js = drv_malloc(sizeof(*js));
    if (!js) {
        out_line(o, "jack self-test: no memory");
        return false;
    }
    check_table(&k, scratch, js);
    check_rirb(&k);
    if (js->n == 7)
        check_flow(&k, js);
    drv_free(js);
    out_line(o, "jack self-test: %s", k.failed ? "FAILED" : "passed (table, RIRB, debounce, "
             "unsolicited and polled)");
    return !k.failed;
}
