/* hdatest: the HD Audio output stream (drivers/hda, abi/idl/hda.idl) from
 * user space. The shell's `hdatest` runs it with devmgr's channels;
 * tools/hda-stream-test.sh runs it in QEMU with the codec's samples going
 * to a WAV file, and checks that file against the pattern written here.
 *
 * It finds the hda driver among devmgr's (GET_SERVICE 0xffff/0xffff: the
 * one answering hda.dump; the dump's first line has its PCI ids) and
 * checks:
 *   open_close    the ring's size and period; a second open, another
 *                 format, the stream methods on the driver's own channel
 *                 and wait_period while stopped are refused; a fresh
 *                 stream is at frame 0; after a close it opens again
 *   pattern       PATTERN_FRAMES frames of a known pattern written ahead
 *                 of the play position with wait_period, then a ring and
 *                 a bit of nothing written: the position advances at 48 kHz
 *                 within 2 %, each answer's offset matches its frames,
 *                 stop holds the position (the WAV file must hold the
 *                 pattern exactly, then silence: the driver's clear-behind)
 *   close_stops   the stream channel closed while running: the stream is
 *                 released (it opens again at frame 0)
 *   kill          devmgr kills the driver mid-stream: the stream channel
 *                 reports ERR_PEER_CLOSED, the restarted driver opens and
 *                 plays a new stream, and the dead driver's pinned pages
 *                 leave the DMA quarantine unwritten
 * The summary goes to the RESULTS box; exit 0 if nothing failed. */
#define CHECK_PROG "hdatest"
#define CHECK_CUR  cur
#include <check.h>
#include <devmgr.h>
#include <idl/hda.h>
#include <os.h>

#define PATTERN_FRAMES 48000u   /* one second */
#define RATE           48000u
#define SOON           (5 * NS_PER_S)
#define OPEN_WAIT      (15 * NS_PER_S)   /* a restarted driver resets and reads the codecs first */

static const char *cur;
static unsigned passed, failed;
static handle_t dm;                      /* devmgr's control channel */
static handle_t hda;                     /* the driver's DR_SERVE */
static uint16_t vid, did;                /* its PCI ids */

/* An open stream: its channel and the mapped ring. */
struct out {
    handle_t ch, vmo;
    uint64_t va;
    uint32_t size, period;
};

static uint64_t soon(void) { return now() + SOON; }

/* The pattern: never zero, so the capture's first sample is its first. */
static int16_t pat_l(uint64_t i) { return (int16_t)(1 + i % 30000); }
static int16_t pat_r(uint64_t i) { return (int16_t)(-1 - (int64_t)(i % 20000)); }

/* Four hex digits at s. */
static uint16_t hex4(const char *s)
{
    uint16_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0')
                     : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : 0;
        v = (uint16_t)(v << 4 | d);
    }
    return v;
}

/* The controller's ids from the dump's first line ("controller vvvv:dddd..."). */
static bool parse_ids(handle_t text, uint32_t len)
{
    char line[24];
    if (len < sizeof(line) || jam_vmo_read(text, 0, line, sizeof(line)) != OK ||
        memcmp(line, "controller ", 11) || line[15] != ':')
        return false;
    vid = hex4(line + 11);
    did = hex4(line + 16);
    return vid && did;
}

/* hda's channel from devmgr. */
static bool find_hda(void)
{
    for (uint32_t n = 0; n < 32; n++) {
        struct devmgr_rep r;
        handle_t ch, text;
        uint32_t nh = 0, len = 0, codecs = 0;
        status_t st = devmgr_call(dm, DEVMGR_GET_SERVICE, 0xffff, 0xffff, n, &r, &ch, 1, &nh,
                                  soon());
        if (st == ERR_NOT_FOUND)
            break;
        if (st != OK || nh != 1)
            continue;
        st = hda_dump_until(ch, now() + OPEN_WAIT, &text, &len, &codecs);
        if (st == OK) {
            bool ok = parse_ids(text, len) && codecs;
            jam_handle_close(text);
            if (ok) {
                hda = ch;
                return true;
            }
        }
        jam_handle_close(ch);
    }
    return false;
}

static status_t out_open(struct out *o)
{
    *o = (struct out){ 0 };
    status_t st = hda_open_output_until(hda, now() + OPEN_WAIT, RATE, 2, 16, &o->ch, &o->vmo,
                                        &o->size, &o->period);
    if (st != OK)
        return st;
    st = jam_vmar_map(startup_handle(SR_SELF_VMAR), o->vmo, 0, o->size, VMAR_READ | VMAR_WRITE,
                      &o->va);
    if (st != OK) {
        jam_handle_close(o->ch);
        jam_handle_close(o->vmo);
        *o = (struct out){ 0 };
    }
    return st;
}

static void out_close(struct out *o)
{
    if (o->va)
        jam_vmar_unmap(startup_handle(SR_SELF_VMAR), o->va, o->size);
    if (o->vmo)
        jam_handle_close(o->vmo);
    if (o->ch)
        jam_handle_close(o->ch);
    *o = (struct out){ 0 };
}

/* Pattern frames [from, to) into the ring (frames past the pattern: none). */
static void put(struct out *o, uint64_t from, uint64_t to)
{
    int16_t *ring = (int16_t *)(uintptr_t)o->va;
    uint64_t frames = o->size / 4;
    for (uint64_t f = from; f < to && f < PATTERN_FRAMES; f++) {
        ring[2 * (f % frames)] = pat_l(f);
        ring[2 * (f % frames) + 1] = pat_r(f);
    }
}

/* ---- the tests ------------------------------------------------------------------ */

static bool t_open_close(void)
{
    struct out o, o2;
    CHECK_ST(out_open(&o), OK);
    CHECK_EQ(o.size, 65536);
    CHECK_EQ(o.period, 16384);
    CHECK_ST(out_open(&o2), ERR_BAD_STATE);
    handle_t a, b;
    uint32_t sz, pd;
    CHECK_ST(hda_open_output_until(hda, soon(), 44100, 2, 16, &a, &b, &sz, &pd),
             ERR_NOT_SUPPORTED);
    CHECK_ST(hda_start_until(hda, soon()), ERR_NOT_SUPPORTED);
    uint64_t frames = 1;
    uint32_t off = 1;
    CHECK_ST(hda_position_until(o.ch, soon(), &frames, &off), OK);
    CHECK_EQ(frames, 0);
    CHECK_EQ(off, 0);
    CHECK_ST(hda_wait_period_until(o.ch, soon(), 0, &frames, &off), ERR_BAD_STATE);
    CHECK_ST(hda_stop_until(o.ch, soon()), OK);
    out_close(&o);
    uint64_t end = now() + SOON;   /* the close reaches the driver on another channel */
    status_t st;
    while ((st = out_open(&o)) == ERR_BAD_STATE && now() < end)
        jam_nanosleep(now() + 10 * NS_PER_MS);
    CHECK_ST(st, OK);
    out_close(&o);
    return true;
}

/* Keep writing ahead until `until` frames have played; the rate over the
 * run into *hz. */
static bool play(struct out *o, uint64_t until, uint64_t *hz)
{
    uint64_t frames = 0, written = o->size / 4, t0 = 0, f0 = 0, t1 = 0, f1 = 0;
    uint32_t off = 0;
    while (frames < until) {
        CHECK_ST(hda_wait_period_until(o->ch, soon(), frames, &frames, &off), OK);
        uint64_t t = now();
        CHECK_EQ(off, (frames * 4) % o->size);
        if (!t0) {
            t0 = t;
            f0 = frames;
        }
        t1 = t;
        f1 = frames;
        put(o, written, frames + o->size / 4);
        written = frames + o->size / 4;
    }
    CHECK(t1 > t0 + NS_PER_S / 2);
    *hz = (f1 - f0) * NS_PER_S / (t1 - t0);
    return true;
}

static bool t_pattern(void)
{
    struct out o;
    CHECK_ST(out_open(&o), OK);
    put(&o, 0, o.size / 4);
    CHECK_ST(hda_start_until(o.ch, soon()), OK);
    uint64_t hz = 0, frames, frames2;
    uint32_t off;
    bool ok = play(&o, PATTERN_FRAMES + o.size / 4 + o.period / 4, &hz);
    CHECK(ok);
    printf("hdatest: pattern: %u frames played ahead, position rate %lu Hz\n", PATTERN_FRAMES,
           (unsigned long)hz);
    CHECK(hz >= RATE * 98 / 100 && hz <= RATE * 102 / 100);
    CHECK_ST(hda_stop_until(o.ch, soon()), OK);
    CHECK_ST(hda_position_until(o.ch, soon(), &frames, &off), OK);
    jam_nanosleep(now() + 100 * NS_PER_MS);
    CHECK_ST(hda_position_until(o.ch, soon(), &frames2, &off), OK);
    CHECK_EQ(frames2, frames);
    CHECK_ST(hda_wait_period_until(o.ch, soon(), frames, &frames2, &off), ERR_BAD_STATE);
    out_close(&o);
    return true;
}

static bool t_close_stops(void)
{
    struct out o;
    uint64_t frames = 0;
    uint32_t off;
    CHECK_ST(out_open(&o), OK);
    CHECK_ST(hda_start_until(o.ch, soon()), OK);
    CHECK_ST(hda_wait_period_until(o.ch, soon(), 0, &frames, &off), OK);
    CHECK(frames > 0);
    out_close(&o);   /* running: the driver stops it */
    uint64_t end = now() + SOON;
    status_t st;
    while ((st = out_open(&o)) == ERR_BAD_STATE && now() < end)
        jam_nanosleep(now() + 10 * NS_PER_MS);
    CHECK_ST(st, OK);
    CHECK_ST(hda_position_until(o.ch, soon(), &frames, &off), OK);
    CHECK_EQ(frames, 0);
    out_close(&o);
    return true;
}

/* The dead driver's pins: back out of the quarantine, none written. */
static bool quarantine_clean(void)
{
    struct devmgr_rep r = { 0 };
    uint64_t end = now() + 10 * NS_PER_S;
    do {
        CHECK_ST(devmgr_call(dm, DEVMGR_SUPERVISION, vid, did, 0, &r, NULL, 0, NULL, soon()), OK);
        if (!r.d)
            break;
        jam_nanosleep(now() + 100 * NS_PER_MS);
    } while (now() < end);
    printf("hdatest: kill: state %u, %u restart(s), quarantine %u page(s) held, %u written\n",
           r.a, r.b, r.d, r.e);
    CHECK_EQ(r.a, DEVMGR_SUP_RUNNING);
    CHECK(r.b >= 1);
    CHECK_EQ(r.d, 0);
    CHECK_EQ(r.e, 0);
    return true;
}

static bool t_kill(void)
{
    struct out o;
    uint64_t frames = 0;
    uint32_t off;
    CHECK_ST(out_open(&o), OK);
    put(&o, 0, o.size / 4);
    CHECK_ST(hda_start_until(o.ch, soon()), OK);
    CHECK_ST(hda_wait_period_until(o.ch, soon(), 0, &frames, &off), OK);
    struct devmgr_rep r;
    CHECK_ST(devmgr_call(dm, DEVMGR_KILL, vid, did, 0, &r, NULL, 0, NULL, soon()), OK);
    CHECK_ST(hda_position_until(o.ch, soon(), &frames, &off), ERR_PEER_CLOSED);
    out_close(&o);   /* nothing written to the old ring: it is quarantined */
    /* The channel the restart will serve (0xffff/0xffff finds running drivers only). */
    handle_t ch;
    uint32_t nh = 0;
    CHECK_ST(devmgr_call(dm, DEVMGR_GET_SERVICE, vid, did, 0, &r, &ch, 1, &nh, soon()), OK);
    CHECK_EQ(nh, 1);
    jam_handle_close(hda);
    hda = ch;
    CHECK_ST(out_open(&o), OK);
    CHECK_ST(hda_start_until(o.ch, soon()), OK);
    CHECK_ST(hda_wait_period_until(o.ch, soon(), 0, &frames, &off), OK);
    CHECK_ST(hda_wait_period_until(o.ch, soon(), frames, &frames, &off), OK);
    CHECK(frames >= 2 * o.period / 4);
    out_close(&o);
    return quarantine_clean();
}

static void run(const char *name, bool (*fn)(void))
{
    cur = name;
    uint64_t t0 = now();
    if (fn()) {
        passed++;
        printf("hdatest: %s ok (%lu ms)\n", name, (unsigned long)((now() - t0) / NS_PER_MS));
    } else {
        failed++;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    char line[120];
    dm = startup_handle(SR_DEVMGR_CTL);
    if (!dm || !find_hda()) {
        int n = snprintf(line, sizeof(line), "hdatest: no hda driver (no HD Audio controller, "
                         "no codec, or no devmgr): skipped");
        jam_debug_report(line, (uint64_t)n);
        return 0;
    }
    printf("hdatest: hda driver for %04x:%04x\n", vid, did);
    run("open_close", t_open_close);
    run("pattern", t_pattern);
    run("close_stops", t_close_stops);
    run("kill", t_kill);
    if (hda)
        jam_handle_close(hda);
    int n = failed ? snprintf(line, sizeof(line), "hdatest: %u passed, %u FAILED", passed, failed)
                   : snprintf(line, sizeof(line), "hdatest: %u passed", passed);
    jam_debug_report(line, (uint64_t)n);
    return failed ? 1 : 0;
}
