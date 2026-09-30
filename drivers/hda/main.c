/* hda: the Intel High Definition Audio driver (drv/hda), started by devmgr
 * for the board's HD Audio controller (vendor 8086, class 04 03 00).
 *
 * Handles (roles from <jam/driver.h>):
 *   DR_BAR(0)   the controller's registers (16 KiB; the first page is all
 *               there is)
 *   DR_DMA      its dma_cap: bus mastering goes on after the reset
 *   DR_PCIDEV   its function (the ids, for the log)
 *   DR_SERVE    the channel it serves abi/idl/hda.idl on
 *   DR_IRQ(0)   its MSI: not used yet (the probe polls)
 *
 * What it does today, and all it does: reset the controller, find the
 * codecs, print each codec's widget graph to the log (dump.c) with a
 * RESULTS line, and serve `hda.dump` (the same lines, read again) until
 * devmgr closes the channel; then stop the command rings, put the
 * controller back into reset and exit 0. It sends the codecs GET verbs
 * only (ctrl.c's hda_get refuses any other), so it makes no sound and
 * changes no routing, gain, pin control, EAPD or power state; what the
 * firmware set up is still set up afterwards, except what the link reset
 * itself resets (spec 4.3). A restart is a bind from scratch. */
#include <idl/hda.h>
#include "hda.h"

#define DUMP_MAX (64 * 1024)   /* hda.dump's text: a Realtek codec's dump is ~8 KiB */

struct state {
    struct hda    hda;
    struct codec *codec;       /* one codec's graph, reused for each */
};

/* The controller line and every codec's graph into o. Returns how many
 * codecs answered. */
static uint32_t dump_all(struct state *s, struct out *o)
{
    struct hda *h = &s->hda;
    hda_dump_ctrl(o, h, "");
    uint32_t answered = 0;
    for (unsigned cad = 0; cad < HDA_MAX_CODECS; cad++) {
        if (!(h->codec_mask & (1u << cad)) || (!h->rings && !h->immediate_ok))
            continue;
        status_t st = hda_read_codec(h, cad, s->codec);
        if (st != OK) {
            out_line(o, "codec %u: does not answer (%s)", cad, status_str(st));
            continue;
        }
        hda_dump_codec(o, s->codec);
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
    *out_codecs = dump_all(s, &o);
    drv_vmo_unmap(m, DUMP_MAX);
    *out_text = vmo;
    *out_length = (uint32_t)o.len;
    return OK;
}

static const struct hda_ops ops = {
    .dump = do_dump,
};

/* The first codec's ids for the RESULTS line. */
static void report(struct state *s, uint32_t answered, uint64_t ms)
{
    struct hda *h = &s->hda;
    uint32_t v = 0;
    unsigned cad = h->codec_mask ? (unsigned)__builtin_ctz(h->codec_mask) : 0;
    if (answered)
        (void)hda_param(h, cad, 0, P_VENDOR, &v);   /* 0 in the line if it fails */
    drv_report("controller %04x:%04x, %u codec(s) (first %04x:%04x), commands through %s, "
               "%lu ms", h->vendor, h->device, answered, v >> 16, v & 0xffff,
               h->rings ? "CORB/RIRB" : h->immediate_ok ? "immediate" : "nothing",
               (unsigned long)ms);
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
    }
    uint64_t t0 = drv_clock_ns();
    status_t st = hda_ctrl_start(h, bar);
    if (st != OK) {
        drv_report("controller %04x:%04x: start failed (%s)", h->vendor, h->device,
                   status_str(st));
        return 3;
    }
    struct out o = { .log = true };
    uint32_t answered = dump_all(s, &o);
    report(s, answered, (drv_clock_ns() - t0) / NS_PER_MS);
    return 0;
}

int driver_main(const struct driver_start *ds)
{
    struct state *s = drv_malloc(sizeof(*s));
    struct codec *c = drv_malloc(sizeof(*c));
    if (!s || !c)
        return 1;
    *s = (struct state){ .codec = c };
    int r = start(ds, s);
    if (r) {
        hda_ctrl_stop(&s->hda);
        return r;
    }
    handle_t ch = drv_handle(ds, DR_SERVE);
    status_t st = ch == HANDLE_INVALID ? OK : hda_serve(ch, &ops, s);
    hda_ctrl_stop(&s->hda);
    drv_log("stopped: controller back in reset (%s)", st == OK ? "client closed" : status_str(st));
    return st == OK ? 0 : 1;
}
