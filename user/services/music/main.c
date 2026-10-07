/* music: the background music player (docs/history/A2-PLAN.md, "Music player").
 * init starts it in shell mode, like the mixer, with
 *   SR_USER + 0   the server end of the `music` channel (abi/idl/music.idl);
 *                 init keeps both ends, so a restarted player serves the
 *                 same channel, which init publishes as /svc/music
 *   SR_NS         every mount read-only, kept up to date by init as
 *                 sticks come and go (/data, /usbN), and /svc/audio, the
 *                 mixer: its one stream ("music")
 * The `music` channel also answers the svc protocol's connect: each
 * opener of /svc/music (libos's svc_open) gets a channel of its own
 * (CLIENTS at once), which speaks `music` and goes when the opener closes
 * it, so a call cut short leaves its late answer with the opener.
 * It runs in a job of its own under init's, not the shell's, so it plays
 * on while the shell runs other commands, after Ctrl+C and across a
 * restart of the shell; only `music stop` (or `kill music`) stops it.
 *
 * One thread: while it plays, every step writes a chunk (blocking at most
 * about a mixer period while the stream's ring is full) and then answers
 * whatever is queued on the channel, so `stop` and `next` take effect
 * within about 50 ms; while it reads a big folder, each step reads a few
 * entries of it. Stopped or paused, it waits on the channel (and for the
 * sleep timer's end, if it is set). A view (jamjar) asks `stereo` (or
 * `spectrum`, or `levels`) many times a second: it is answered from what
 * the player keeps, plus one position call to the mixer. Each track's
 * start is one line in the log ("[music] music: track 3: Artist -
 * Title (3:45)"), which the console shows above the prompt. */
#include <idl/music.h>
#include "music.h"

#define CLIENTS 8          /* channels handed out by svc.connect at once */
#define BUDGET  32         /* requests from one channel per turn: playback goes on between */
#define KEY_CTL 0u         /* port keys: the shared channel, then 1 + a client's slot */

static struct player P;
static handle_t port;
static handle_t clients[CLIENTS];   /* our ends of the channels openers got (0: free) */
static bool     more;               /* a channel still had requests when its turn ended */

static void put(uint8_t *out, size_t size, const char *s)
{
    size_t n = s ? strnlen(s, size - 1) : 0;
    memcpy(out, s ? s : "", n);
    memset(out + n, 0, size - n);
}

/* A path from the wire: NUL-terminated within its 256 bytes, and absolute
 * (or empty, if `empty` may be). */
static bool path_arg(const uint8_t in[256], char out[FS_PATH_MAX], bool empty)
{
    if (strnlen((const char *)in, FS_PATH_MAX) == FS_PATH_MAX)
        return false;
    memcpy(out, in, FS_PATH_MAX);
    return out[0] == '/' || (empty && !out[0]);
}

static status_t on_start(void *ctx, const uint8_t folder[256], uint32_t *found, uint8_t *reading)
{
    struct player *p = ctx;
    char path[FS_PATH_MAX];
    if (!path_arg(folder, path, false))
        return ERR_INVALID_ARGS;
    bool scanning = false;
    status_t st = player_start(p, path, found, &scanning);
    *reading = scanning;
    return st;
}

static status_t on_play(void *ctx, const uint8_t folder[256], const uint8_t first[256],
                        uint8_t order, uint32_t *found, uint8_t *reading)
{
    struct player *p = ctx;
    char path[FS_PATH_MAX], file[FS_PATH_MAX];
    if (!path_arg(folder, path, false) || !path_arg(first, file, true) || order > 1)
        return ERR_INVALID_ARGS;
    bool scanning = false;
    status_t st = player_play(p, path, file, order, found, &scanning);
    *reading = scanning;
    return st;
}

static status_t on_stop(void *ctx, uint8_t *was_playing)
{
    struct player *p = ctx;
    *was_playing = p->playing || p->scanning;
    player_sleep(p, 0);
    if (*was_playing) {
        player_stop(p, NULL);
        printf("music: stopped\n");
    }
    return OK;
}

static status_t on_next(void *ctx)
{
    return player_next(ctx);
}

static status_t on_prev(void *ctx)
{
    return player_prev(ctx);
}

static status_t on_pause(void *ctx, uint8_t on, uint8_t *paused)
{
    struct player *p = ctx;
    if (on > 1)
        return ERR_INVALID_ARGS;
    status_t st = player_pause(p, on);
    *paused = p->paused;
    return st;
}

static status_t on_sleep(void *ctx, uint32_t seconds, uint32_t *out)
{
    if (seconds > SLEEP_MAX_S)
        return ERR_OUT_OF_RANGE;
    player_sleep(ctx, seconds);
    *out = seconds;
    return OK;
}

/* playing as `status` and `levels` say it. */
static uint8_t state(const struct player *p)
{
    return p->scanning ? 2 : p->paused ? 3 : p->playing;
}

static uint32_t sleep_left(const struct player *p)
{
    uint64_t t = now();
    return p->sleep_at > t ? (uint32_t)((p->sleep_at - t + NS_PER_S - 1) / NS_PER_S) : 0;
}

/* What `levels`, `spectrum` and `stereo` share: the state, and the bands heard now
 * (zeros if none). */
static void heard(struct player *p, uint8_t *playing, uint32_t *serial, uint64_t *elapsed_ms,
                  uint64_t *length_ms, int32_t *volume, uint32_t *sleep_s, struct spec_entry *e)
{
    *playing = state(p);
    *volume = p->volume;
    *sleep_s = sleep_left(p);
    *serial = 0;
    *length_ms = 0;
    memset(e, 0, sizeof(*e));
    const struct mark *m = player_heard(p, elapsed_ms);
    if (!m)
        return;
    *serial = m->serial;
    *length_ms = m->length_ms;
    if (!p->paused && p->spec && !spec_at(p->spec, (int64_t)p->heard_at, e))
        memset(e, 0, sizeof(*e));
}

static status_t on_levels(void *ctx, uint8_t *playing, uint32_t *serial, uint64_t *elapsed_ms,
                          uint64_t *length_ms, int32_t *volume, uint32_t *sleep_s,
                          uint8_t bands[16], uint8_t *level)
{
    struct spec_entry e;
    heard(ctx, playing, serial, elapsed_ms, length_ms, volume, sleep_s, &e);
    for (unsigned i = 0; i < 16; i++) {
        const uint8_t *q = e.band + 4 * i;
        uint8_t a = q[0] > q[1] ? q[0] : q[1], b = q[2] > q[3] ? q[2] : q[3];
        bands[i] = a > b ? a : b;
    }
    *level = e.level;
    return OK;
}

static status_t on_spectrum(void *ctx, uint8_t *playing, uint32_t *serial,
                            uint64_t *elapsed_ms, uint64_t *length_ms, int32_t *volume,
                            uint32_t *sleep_s, uint8_t bands[64], uint8_t *level)
{
    struct spec_entry e;
    heard(ctx, playing, serial, elapsed_ms, length_ms, volume, sleep_s, &e);
    memcpy(bands, e.band, 64);
    *level = e.level;
    return OK;
}

static status_t on_stereo(void *ctx, uint8_t *playing, uint32_t *serial, uint64_t *elapsed_ms,
                          uint64_t *length_ms, int32_t *volume, uint32_t *sleep_s,
                          uint8_t left[64], uint8_t right[64], uint8_t *level)
{
    struct spec_entry e;
    heard(ctx, playing, serial, elapsed_ms, length_ms, volume, sleep_s, &e);
    memcpy(left, e.left, 64);
    memcpy(right, e.right, 64);
    *level = e.level;
    return OK;
}

_Static_assert(SPEC_BANDS == 64, "music.idl's spectrum and stereo have 64 bands, levels 16");

static status_t on_status(void *ctx, uint8_t *playing, uint32_t *tracks, uint32_t *bad,
                          uint32_t *started, uint64_t *elapsed_ms, uint64_t *length_ms,
                          int32_t *volume, uint8_t folder[256], uint8_t path[256],
                          uint8_t title[128], uint8_t note[128])
{
    struct player *p = ctx;
    *playing = state(p);
    *tracks = p->t.count;
    *bad = p->t.nbad;
    *started = p->started;
    *volume = p->volume;
    *length_ms = 0;
    put(folder, 256, p->folder);
    put(note, 128, p->note);
    const struct mark *m = player_heard(p, elapsed_ms);
    if (!m) {
        put(path, 256, NULL);
        put(title, 128, NULL);
        return OK;
    }
    char t[TITLE_MAX];
    track_title(p->folder, p->t.path[m->track], t, sizeof(t));
    put(path, 256, p->t.path[m->track]);
    put(title, 128, t);
    *length_ms = m->length_ms;
    return OK;
}

static status_t on_volume(void *ctx, int32_t cb, int32_t *out)
{
    return player_set_volume(ctx, cb, out);
}

static const struct music_ops ops = {
    .start = on_start, .stop = on_stop, .next = on_next, .status = on_status,
    .set_volume = on_volume, .prev = on_prev, .play = on_play, .levels = on_levels,
    .pause = on_pause, .sleep = on_sleep, .spectrum = on_spectrum, .stereo = on_stereo,
};

static uint32_t dispatch(void *ctx, const void *req, uint32_t n, void *rep, handle_t *rhs,
                         uint32_t *rhn)
{
    return music_dispatch(&ops, ctx, req, n, rep, rhs, rhn);
}

/* svc.connect: a new channel for one opener, served like the shared one. */
static status_t on_connect(void *ctx, handle_t *out)
{
    (void)ctx;
    for (unsigned i = 0; i < CLIENTS; i++) {
        if (clients[i])
            continue;
        handle_t mine, theirs;
        status_t st = jam_channel_create(&mine, &theirs);
        if (st == OK)
            st = jam_port_bind(port, mine, 1 + i, SIG_READABLE | SIG_PEER_CLOSED,
                               PORT_BIND_PERSISTENT);
        if (st != OK) {
            if (mine) {
                jam_handle_close(mine);
                jam_handle_close(theirs);
            }
            return st;
        }
        clients[i] = mine;
        *out = theirs;
        return OK;
    }
    return ERR_NO_RESOURCES;
}

/* Up to BUDGET requests from ch: ERR_SHOULD_WAIT once it is empty (or the
 * budget is spent: `more` says so), else the read's status. */
static status_t serve_some(struct player *p, handle_t ch)
{
    status_t st = OK;
    for (unsigned n = 0; n < BUDGET && st == OK; n++)
        st = svc_serve_request(ch, dispatch, on_connect, p);
    if (st == OK) {
        more = true;   /* a client writing flat out doesn't hold up the music */
        return ERR_SHOULD_WAIT;
    }
    return st;
}

/* Answer what is queued on every channel, a budget each. The shared
 * channel's end (init gave up on us): its status; else ERR_SHOULD_WAIT. */
static status_t serve_all(struct player *p)
{
    more = false;
    status_t st = serve_some(p, p->ctl);
    if (st != ERR_SHOULD_WAIT)
        return st;
    for (unsigned i = 0; i < CLIENTS; i++) {
        if (clients[i] && serve_some(p, clients[i]) != ERR_SHOULD_WAIT) {
            /* its opener is gone; the binding would keep the channel (and
             * its charge) until unbound: closing alone doesn't */
            (void)jam_port_unbind(port, clients[i], 1 + i);
            jam_handle_close(clients[i]);
            clients[i] = HANDLE_INVALID;
        }
    }
    return ERR_SHOULD_WAIT;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct player *p = &P;
    p->ctl = startup_handle(SR_USER + 0);
    p->cur = -1;
    /* The bands are a nicety: without the memory `levels` answers zeros. */
    if ((p->spec = malloc(sizeof(*p->spec))) != NULL)
        spec_init(p->spec);
    if (!p->ctl) {
        printf("music: started without its channel (SR_USER + 0): nothing to serve\n");
        return 1;
    }
    status_t st = jam_port_create(&port);
    if (st == OK)
        st = jam_port_bind(port, p->ctl, KEY_CTL, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    if (st != OK) {
        printf("music: no port for its channels (%s)\n", status_str(st));
        return 1;
    }
    printf("music: ready (`music start [folder]` in the shell)\n");
    for (;;) {
        st = serve_all(p);
        if (st != ERR_SHOULD_WAIT) {
            printf("music: reading its channel: %s: ending\n", status_str(st));
            player_stop(p, NULL);
            return 1;
        }
        player_sleep_tick(p);
        if (p->scanning) {
            player_scan(p);
            continue;
        }
        if (p->playing && !p->paused) {
            player_step(p);
            continue;
        }
        if (more)
            continue;   /* requests left over from their turn: no sleeping on them */
        /* Stopped or paused: a channel, or the sleep timer's end. */
        struct port_packet pkt;
        uint64_t until = p->sleep_at ? p->sleep_at : DEADLINE_NEVER;
        st = jam_port_wait(port, until, &pkt);
        if (st != OK && st != ERR_TIMED_OUT) {
            printf("music: waiting on its channels: %s: ending\n", status_str(st));
            return 1;
        }
    }
}
