/* music: the background music player (docs/A2-PLAN.md, "Music player").
 * init starts it in shell mode, like the mixer, with
 *   SR_USER + 0   the server end of the `music` channel (abi/idl/music.idl);
 *                 init keeps both ends, so a restarted player serves the
 *                 same channel and the shell's `music` reaches it
 *   SR_AUDIO      the mixer's `audio` channel: its one stream ("music")
 *   SR_NS         init's whole namespace, kept up to date by init as
 *                 sticks come and go (/data, /usbN)
 * It runs in a job of its own under init's, not the shell's, so it plays
 * on while the shell runs other commands, after Ctrl+C and across a
 * restart of the shell; only `music stop` (or `kill music`) stops it.
 *
 * One thread: while it plays, every step writes a chunk (blocking at most
 * about a mixer period while the stream's ring is full) and then answers
 * whatever is queued on the channel, so `stop` and `next` take effect
 * within about 50 ms; while it reads a big folder, each step reads a few
 * entries of it. Stopped, it waits on the channel. Each track's
 * start is one line in the log ("[music] music: track 3: Artist -
 * Title (3:45)"), which the console shows above the prompt. */
#include <idl/music.h>
#include "music.h"

static struct player P;

static void put(uint8_t *out, size_t size, const char *s)
{
    size_t n = s ? strnlen(s, size - 1) : 0;
    memcpy(out, s ? s : "", n);
    memset(out + n, 0, size - n);
}

static status_t on_start(void *ctx, const uint8_t folder[256], uint32_t *found, uint8_t *reading)
{
    struct player *p = ctx;
    char path[FS_PATH_MAX];
    if (strnlen((const char *)folder, FS_PATH_MAX) == FS_PATH_MAX)
        return ERR_INVALID_ARGS;
    memcpy(path, folder, FS_PATH_MAX);
    if (path[0] != '/')
        return ERR_INVALID_ARGS;
    bool scanning = false;
    status_t st = player_start(p, path, found, &scanning);
    *reading = scanning;
    return st;
}

static status_t on_stop(void *ctx, uint8_t *was_playing)
{
    struct player *p = ctx;
    *was_playing = p->playing || p->scanning;
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

static status_t on_status(void *ctx, uint8_t *playing, uint32_t *tracks, uint32_t *bad,
                          uint32_t *started, uint64_t *elapsed_ms, uint64_t *length_ms,
                          int32_t *volume, uint8_t folder[256], uint8_t path[256],
                          uint8_t title[128], uint8_t note[128])
{
    struct player *p = ctx;
    *playing = p->scanning ? 2 : p->playing;
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
    .set_volume = on_volume,
};

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    struct player *p = &P;
    p->ctl = startup_handle(SR_USER + 0);
    p->cur = -1;
    if (!p->ctl) {
        printf("music: started without its channel (SR_USER + 0): nothing to serve\n");
        return 1;
    }
    printf("music: ready (`music start [folder]` in the shell)\n");
    for (;;) {
        status_t st;
        while ((st = music_serve_one(p->ctl, &ops, p)) == OK) {
        }
        if (st != ERR_SHOULD_WAIT) {
            printf("music: reading its channel: %s: ending\n", status_str(st));
            player_stop(p, NULL);
            return 1;
        }
        if (p->scanning) {
            player_scan(p);
            continue;
        }
        if (p->playing) {
            player_step(p);
            continue;
        }
        signals_t seen = 0;
        st = jam_object_wait_one(p->ctl, SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER, &seen);
        if (st != OK) {
            printf("music: waiting on its channel: %s: ending\n", status_str(st));
            return 1;
        }
    }
}
