/* hda: the Intel High Definition Audio driver (drv/hda), started by devmgr
 * for the board's HD Audio controller (vendor 8086, class 04 03 00).
 *
 * Handles (roles from <jam/driver.h>):
 *   DR_BAR(0)   the controller's registers (16 KiB; the first page is all
 *               there is)
 *   DR_DMA      its dma_cap: bus mastering goes on after the reset
 *   DR_PCIDEV   its function (the ids, for the log)
 *   DR_SERVE    the channel it serves abi/idl/hda.idl on
 *   DR_IRQ(0)   its MSI: the output stream's period interrupts and the
 *               RIRB's (unsolicited responses: the jacks) (irq.c)
 *
 * What it does: check the path finder against its fixtures, reset the
 * controller, find the codecs, print each codec's widget graph and the
 * path it offers to the log (dump.c), set up the best path (the front
 * headphone jack's, see path.c) muted and with the pin's output off, and
 * put a RESULTS line out; then serve `hda.dump`, `hda.info`, query
 * channels (everything but the stream: the shell's, through the mixer) and
 * the output stream on the path's DAC (stream.c, irq.c) until devmgr closes
 * the channel, close the stream, stop the command rings, put the
 * controller back into reset and exit 0. The path is unmuted (at the
 * gain, -30 dB unless set_gain says otherwise) only while the stream
 * runs (stream.c, verbs.c's hda_output_*); the output stage (the pin's
 * output and EAPD) goes on once after the set-up, with every amp muted,
 * and off at exit, so no stream hears it power up. The jacks with presence
 * detection (jack.c) are watched from the loop's start: each change is
 * logged ("headphones plugged in (front, pin 1b)"), found by unsolicited
 * responses or by polling, and `hda.jacks` answers their states. verbs.c
 * is the only way to a codec and lets through
 * GET verbs and an allow-list of SET verbs; nothing else the firmware set
 * up (configuration defaults, other pins, GPIOs) changes. A restart is a
 * bind from scratch. */
#include <idl/hda.h>
#include "hda.h"

#define DUMP_MAX (64 * 1024)   /* hda.dump's text: a Realtek codec's dump is ~8 KiB */

#define TEXT_MAX 240           /* hda.info's text */

struct state {
    struct hda    hda;
    struct codec *codec;       /* one codec's graph, reused for each */
    struct codec *best;        /* the graph of the path's codec, read back once it is set up */
    struct codec *scratch;     /* the path self-test's and the dump round trip's */
    struct path   path;        /* the path chosen at start; rule PATH_NONE: none */
    bool          tested;      /* the path finder passed its self-test */
    status_t      set;         /* setting the path up: OK, or why not */
    struct output out;         /* the path as the stream opens it, and the gain */
    struct jacks  jacks;       /* every codec's jacks with presence detection */
    bool          jacks_ok;    /* the jack self-test passed: they are watched */
};

/* At start: codec c's path, checked against c's own dump parsed back,
 * kept if it is better than the best so far (the lowest codec wins a tie). */
static void consider(struct state *s, struct out *o, const struct path *p)
{
    if (!hda_path_roundtrip(s->codec, p, s->scratch, o))
        s->tested = false;
    if (p->rule != PATH_NONE && (s->path.rule == PATH_NONE || p->rule < s->path.rule)) {
        s->path = *p;
        *s->best = *s->codec;
    }
}

/* The controller line, every codec's graph and the path it offers into o;
 * at start (`choosing`) the best path is kept too. Returns how many codecs
 * answered. A codec that does not answer (the PC's link has one, likely
 * the disabled iGPU's HDMI codec) is skipped. */
static uint32_t dump_all(struct state *s, struct out *o, bool choosing)
{
    struct hda *h = &s->hda;
    hda_dump_ctrl(o, h, "");
    uint32_t answered = 0;
    for (unsigned cad = 0; cad < HDA_MAX_CODECS; cad++) {
        if (!(h->codec_mask & (1u << cad)) || (!h->rings && !h->immediate_ok))
            continue;
        status_t st = hda_read_codec(h, cad, s->codec);
        if (st != OK) {
            out_line(o, "codec %u: does not answer (%s): skipped", cad, status_str(st));
            continue;
        }
        hda_dump_codec(o, s->codec);
        struct path p;
        (void)hda_path_find(s->codec, &p);   /* its line says when there is none */
        hda_dump_path(o, s->codec, &p);
        if (choosing) {
            consider(s, o, &p);
            if (s->jacks_ok)
                hda_jacks_add(&s->jacks, s->codec);
        }
        answered++;
    }
    out_line(o, "%u codec(s) answered; %u verb(s) timed out, %u unsolicited response(s)", answered,
             h->timeouts, h->unsol);
    return answered;
}

static status_t do_dump(void *ctx, handle_t *out_text, uint32_t *out_length, uint32_t *out_codecs)
{
    struct state *s = ctx;
    handle_t vmo;
    void *m = NULL;
    status_t st = drv_vmo_create(DUMP_MAX, 0, &vmo);
    if (st != OK)
        return st;
    if ((st = drv_vmo_map(vmo, 0, DUMP_MAX, VMAR_READ | VMAR_WRITE, &m)) != OK) {
        drv_handle_close(vmo);
        return st;
    }
    struct out o = { .buf = m, .cap = DUMP_MAX, .log = false };
    *out_codecs = dump_all(s, &o, false);
    drv_vmo_unmap(m, DUMP_MAX);
    *out_text = vmo;
    *out_length = (uint32_t)o.len;
    return OK;
}

static void copy_text(uint8_t *to, size_t size, const char *from)
{
    size_t i = 0;
    for (; i + 1 < size && from[i]; i++)
        to[i] = (uint8_t)from[i];
    to[i] = 0;
}

/* The path in words and what is set on it: hda.info's text and the log's. */
static void path_text(const struct state *s, char *buf, size_t size)
{
    const struct path *p = &s->path;
    char nodes[64], state[190];
    if (p->rule == PATH_NONE || s->set != OK) {
        drv_snprintf(buf, size, "none (%s)", !s->tested ? "the path self-test failed"
                     : p->rule == PATH_NONE ? "no output pin reaches an analog DAC"
                     : status_str(s->set));
        return;
    }
    hda_path_str(s->best, p, nodes, sizeof(nodes));
    hda_path_state(s->best, p, state, sizeof(state));
    drv_snprintf(buf, size, "codec %u %s (%s), muted: %s", p->cad, nodes,
                 hda_path_rule_name(p->rule), state);
}

/* The jack at the path's pin, or NULL (no path, or the pin has no presence detection). */
static const struct jack *path_jack(const struct state *s)
{
    const struct path *p = &s->path;
    if (p->rule == PATH_NONE || s->set != OK || !p->n)
        return NULL;
    for (unsigned i = 0; i < s->jacks.n; i++)
        if (s->jacks.j[i].cad == p->cad && s->jacks.j[i].nid == p->nid[p->n - 1])
            return &s->jacks.j[i];
    return NULL;
}

static status_t do_info(void *ctx, uint32_t *out_codec, uint32_t *out_pin, uint32_t *out_dac,
                        uint32_t *out_pcm, uint32_t *out_formats, uint32_t *out_amp,
                        uint32_t *out_jack, uint32_t *out_count, uint8_t out_nodes[8],
                        uint8_t out_text[TEXT_MAX])
{
    struct state *s = ctx;
    const struct path *p = &s->path;
    char text[TEXT_MAX];
    path_text(s, text, sizeof(text));
    copy_text(out_text, TEXT_MAX, text);
    *out_jack = path_jack(s) ? path_jack(s)->state : JACK_UNKNOWN;
    const struct widget *dac = p->n ? hda_widget(s->best, p->nid[0]) : NULL;
    if (p->rule == PATH_NONE || s->set != OK || !dac)
        return OK;   /* pin 0: no path; the text says why */
    *out_codec = p->cad;
    *out_pin = p->nid[p->n - 1];
    *out_dac = dac->nid;
    *out_pcm = hda_output_pcm(&s->out);
    *out_formats = dac->formats;
    *out_amp = dac->caps & WCAP_OUT_AMP ? dac->amp_out : 0;
    *out_count = p->n;
    for (unsigned i = 0; i < p->n && i < 8; i++)
        out_nodes[i] = p->nid[i];
    return OK;
}

static status_t do_get_gain(void *ctx, int32_t *out_gain, uint32_t *out_step, int32_t *out_min,
                            int32_t *out_max)
{
    struct state *s = ctx;
    status_t st = hda_output_gain(&s->out, out_gain, out_min, out_max);
    *out_step = s->out.step;
    return st;
}

static status_t do_set_gain(void *ctx, int32_t centibels, int32_t *out_gain, uint32_t *out_step,
                            int32_t *out_min, int32_t *out_max)
{
    struct state *s = ctx;
    status_t st = hda_output_set_gain(&s->hda, &s->out, centibels);
    if (st == ERR_NOT_FOUND || st == ERR_NOT_SUPPORTED)
        return st;
    (void)do_get_gain(ctx, out_gain, out_step, out_min, out_max);
    char db[16];
    hda_db_str(db, sizeof(db), *out_gain);
    drv_log("output: gain set to %s dB (step %u)%s%s", db, *out_step,
            st == OK ? "" : ": sending it failed: ", st == OK ? "" : status_str(st));
    return st;
}

static status_t do_set_bits(void *ctx, uint32_t bits, uint32_t *out_bits, uint32_t *out_pcm)
{
    struct state *s = ctx;
    if (bits && bits != 16 && bits != 20 && bits != 24 && bits != 32)
        return ERR_INVALID_ARGS;
    if (bits && bits != s->out.max_bits) {
        s->out.max_bits = bits;
        drv_log("output: streams use at most %u-bit samples from the next open", bits);
    }
    *out_bits = s->out.max_bits;
    *out_pcm = hda_output_pcm(&s->out);
    return OK;
}

#define JACK_TEXT 1024   /* hda.jacks's text */

static status_t do_jacks(void *ctx, uint32_t *out_count, uint32_t *out_state,
                         uint32_t *out_changes, uint8_t out_pins[16], uint8_t out_states[16],
                         uint8_t out_text[JACK_TEXT])
{
    struct state *s = ctx;
    const struct jacks *js = &s->jacks;
    const struct jack *pj = path_jack(s);
    char *text = drv_malloc(JACK_TEXT);
    if (!text)
        return ERR_NO_MEMORY;
    size_t len = (size_t)drv_snprintf(text, JACK_TEXT, "%u jack(s) with presence detection; "
        "unsolicited responses %s (%u received, %u RIRB interrupt(s)); a jack not shown to send "
        "them is polled every %u ms\n",
        js->n, !s->jacks_ok ? "unused: the jack self-test failed" : js->unsol ? "on" : "off",
        s->hda.unsol, js->irq_seen, (unsigned)(JACK_POLL_NS / NS_PER_MS));
    static const char *const states[3] = { "unknown", "unplugged", "plugged in" };
    for (unsigned i = 0; i < js->n && i < 16 && len < JACK_TEXT; i++) {
        const struct jack *j = &js->j[i];
        char where[32];
        hda_jack_where(js, j, where, sizeof(where));
        out_pins[i] = j->nid;
        out_states[i] = j->state;
        len += (size_t)drv_snprintf(text + len, JACK_TEXT - len, "pin %02x %s (%s): %s, %u "
                                    "change(s); %s, tag %u, %u response(s)%s\n", j->nid,
                                    hda_jack_name(j), where, states[j->state % 3], j->changes,
                                    hda_jack_mode_str(j->mode), j->tag, j->unsols,
                                    j == pj ? "; the path's pin" : "");
    }
    copy_text(out_text, JACK_TEXT, text);
    drv_free(text);
    *out_count = js->n;
    *out_state = pj ? pj->state : JACK_UNKNOWN;
    *out_changes = pj ? pj->changes : 0;
    return OK;
}

static const struct hda_ops ops = {
    .dump = do_dump,
    .jacks = do_jacks,
    .info = do_info,
    .set_gain = do_set_gain,
    .get_gain = do_get_gain,
    .set_bits = do_set_bits,
};

/* The chosen path set up muted (verbs.c), read back, and logged. */
static void set_path(struct state *s, struct out *o)
{
    s->set = !s->tested ? ERR_BAD_STATE : s->path.rule == PATH_NONE ? ERR_NOT_FOUND : OK;
    if (s->set == OK) {
        s->set = hda_path_program_muted(&s->hda, s->best, &s->path);
        hda_path_read_back(&s->hda, s->best, &s->path);
    }
    char text[TEXT_MAX];
    path_text(s, text, sizeof(text));
    out_line(o, "path: %s", text);
    const struct widget *pin = s->set == OK ? hda_widget(s->best, s->path.nid[s->path.n - 1])
                                            : NULL;
    if (pin && (pin->pin_ctl & PINCTL_OUT))
        out_line(o, "path: pin %02x kept its output on (the codec ignores its pin control): "
                 "only the path's amps keep it quiet", pin->nid);
    hda_output_init(&s->out, s->set == OK ? s->best : NULL, s->set == OK ? &s->path : NULL);
    int32_t cb, lo, hi;
    if (hda_output_gain(&s->out, &cb, &lo, &hi) == OK) {
        char db[16], dlo[16], dhi[16];
        hda_db_str(db, sizeof(db), cb);
        hda_db_str(dlo, sizeof(dlo), lo);
        hda_db_str(dhi, sizeof(dhi), hi);
        out_line(o, "path: plays at %s dB (node %02x's output amp, step %u; %s to %s dB), "
                 "unmuted only while a stream runs", db, s->out.vol, s->out.step, dlo, dhi);
    }
    else if (s->set == OK)
        out_line(o, "path: no amp on it has gain steps: it plays at 0 dB");
    /* The output stage on now, every amp still muted, so it has settled
     * long before a stream unmutes them (hda.h, OUTPUT_SETTLE_NS). */
    if (s->set == OK && s->out.mutes) {
        status_t st = hda_output_stage(&s->hda, &s->out, true);
        out_line(o, "path: the output stage (pin %02x's output, EAPD) %s with every amp muted; "
                 "streams unmute the amps from %u ms on", s->path.nid[s->path.n - 1],
                 st == OK ? "on" : "not on", (unsigned)(OUTPUT_SETTLE_NS / NS_PER_MS));
    } else if (s->set == OK) {
        out_line(o, "path: no amp on it can mute it: the pin's output goes on only while a "
                 "stream runs");
    }
}

/* The first codec's ids for the RESULTS line. */
static void report(struct state *s, uint32_t answered, uint64_t ms)
{
    struct hda *h = &s->hda;
    uint32_t v = 0;
    unsigned cad = h->codec_mask ? (unsigned)__builtin_ctz(h->codec_mask) : 0;
    if (answered)
        (void)hda_param(h, cad, 0, P_VENDOR, &v);   /* 0 in the line if it fails */
    /* The path as its nodes, "02-0c-1b": the RESULTS box shows about 120
     * characters of a line. */
    char path[3 * PATH_MAX_NODES + 1] = "none";
    for (unsigned i = 0, len = 0; s->set == OK && i < s->path.n; i++)
        len += (unsigned)drv_snprintf(path + len, sizeof(path) - len, "%s%02x", i ? "-" : "",
                                      s->path.nid[i]);
    drv_report("controller %04x:%04x, %u codec(s) (first %04x:%04x), commands through %s, "
               "%lu ms; path %s%s", h->vendor, h->device, answered, v >> 16, v & 0xffff,
               h->rings ? "CORB/RIRB" : h->immediate_ok ? "immediate" : "nothing",
               (unsigned long)ms, path, s->set == OK ? " muted" : "");
}

static int start(const struct driver_start *ds, struct state *s)
{
    struct hda *h = &s->hda;
    handle_t bar = drv_handle(ds, DR_BAR(0)), dev = drv_handle(ds, DR_PCIDEV);
    h->dma = drv_handle(ds, DR_DMA);
    if (bar == HANDLE_INVALID || h->dma == HANDLE_INVALID) {
        drv_log("missing handles (BAR 0 %#x, DMA %#x)", bar, h->dma);
        return 2;
    }
    uint32_t ids = 0;
    if (dev != HANDLE_INVALID && drv_pci_config_read(dev, 0, 4, &ids) == OK) {
        h->vendor = (uint16_t)ids;
        h->device = (uint16_t)(ids >> 16);
        for (unsigned i = 0; i < 4; i++)
            (void)drv_pci_config_read(dev, 0x40 + 4 * i, 4, &h->cfg40[i]);   /* 0 if refused */
    }
    struct out o = { .log = true };
    s->tested = hda_path_selftest(s->scratch, &o);
    s->jacks_ok = hda_jack_selftest(s->scratch, &o);   /* failed: no jack is watched */
    uint64_t t0 = drv_clock_ns();
    status_t st = hda_ctrl_start(h, bar);
    if (st != OK) {
        drv_report("controller %04x:%04x: start failed (%s)", h->vendor, h->device,
                   status_str(st));
        return 3;
    }
    /* The IOMMU checks (vtdtest.c) run before the path is set up, on the
     * rings just brought up, and leave them as they were. */
    if (drv_has_arg(ds, "vtdtest") && h->codec_mask)
        hda_vtdtest(h, (unsigned)__builtin_ctz(h->codec_mask));
    uint32_t answered = dump_all(s, &o, true);
    set_path(s, &o);
    report(s, answered, (drv_clock_ns() - t0) / NS_PER_MS);
    return 0;
}

int driver_main(const struct driver_start *ds)
{
    struct state *s = drv_malloc(sizeof(*s));
    struct codec *c = drv_malloc(sizeof(*c)), *best = drv_malloc(sizeof(*best));
    struct codec *scratch = drv_malloc(sizeof(*scratch));
    if (!s || !c || !best || !scratch)
        return 1;
    *s = (struct state){ .codec = c, .best = best, .scratch = scratch, .set = ERR_NOT_FOUND };
    int r = start(ds, s);
    if (r) {
        hda_ctrl_stop(&s->hda);
        return r;
    }
    handle_t ch = drv_handle(ds, DR_SERVE);
    status_t st = ch == HANDLE_INVALID ? OK
                : hda_loop(&s->hda, ds, &ops, s, &s->out, &s->jacks);
    if (s->out.stage)   /* the amps were muted when the stream closed */
        (void)hda_output_stage(&s->hda, &s->out, false);
    hda_ctrl_stop(&s->hda);
    drv_log("stopped: controller back in reset (%s)", st == OK ? "client closed" : status_str(st));
    return st == OK ? 0 : 1;
}
