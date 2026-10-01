/* jamjar: the player, from a thread of its own.
 *
 * The player (bin/music) answers its channel between chunks of sound, so
 * a call can wait up to a mixer period (43 ms). The drawing must never
 * wait for that, so this thread makes every call: the commands the UI
 * queues (link_cmd), `stereo` about 30 times a second, and `status` when
 * the track heard changes (and once a second, for the folder and the
 * note). What it learns goes into a snapshot that link_get copies out.
 *
 * Sharing: `lock` (a spinlock: every hold is a copy of a few hundred
 * bytes, never a call) guards snap, the queue and the result. The UI
 * signals `wake` after queueing, so a command goes out at once rather
 * than at the next poll. */
#include <idl/music.h>
#include "jamjar.h"

#define POLL_NS     (33 * NS_PER_MS)
#define STATUS_NS   NS_PER_S
#define CALL_NS     (5 * NS_PER_S)     /* the player may be reading a folder */
#define QUEUE       8
#define LINK_STACK  (64 * 1024)

static struct {
    handle_t    ch, wake;
    bool        lock;
    struct snap snap;                  /* lock */
    struct cmd  q[QUEUE];              /* lock */
    unsigned    nq;                    /* lock */
    char        result[96];            /* lock: the last command's outcome, until read */
} L;

static void lock(void)
{
    while (__atomic_test_and_set(&L.lock, __ATOMIC_ACQUIRE))
        __builtin_ia32_pause();
}

static void unlock(void)
{
    __atomic_clear(&L.lock, __ATOMIC_RELEASE);
}

static void result(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void result(const char *fmt, ...)
{
    char buf[sizeof(L.result)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lock();
    memcpy(L.result, buf, sizeof(buf));
    unlock();
}

static void put(uint8_t out[256], const char *s)
{
    memset(out, 0, 256);
    memcpy(out, s, strnlen(s, 255));
}

static const char *why(status_t st)
{
    return st == ERR_NOT_FOUND ? "no such folder, or no audio output" : status_str(st);
}

static void play(const struct cmd *c)
{
    uint8_t folder[256], first[256], reading = 0;
    uint32_t found = 0;
    put(folder, c->folder);
    put(first, c->first);
    status_t st = music_play_until(L.ch, now() + CALL_NS, folder, first, c->ordered, &found,
                                   &reading);
    if (st != OK)
        result("can't play it: %s", why(st));
    else if (!found && !reading)
        result("nothing to play there");
}

static void run(const struct cmd *c)
{
    uint64_t dl = now() + CALL_NS;
    status_t st = OK;
    uint8_t u8 = 0;
    int32_t i32 = 0;
    uint32_t u32 = 0;
    switch (c->kind) {
    case CMD_PLAY: play(c); return;
    case CMD_STOP: st = music_stop_until(L.ch, dl, &u8); break;
    case CMD_NEXT: st = music_next_until(L.ch, dl); break;
    case CMD_PREV: st = music_prev_until(L.ch, dl); break;
    case CMD_PAUSE: st = music_pause_until(L.ch, dl, (uint8_t)c->arg, &u8); break;
    case CMD_VOLUME: st = music_set_volume_until(L.ch, dl, c->arg, &i32); break;
    case CMD_SLEEP: st = music_sleep_until(L.ch, dl, (uint32_t)c->arg, &u32); break;
    }
    if (st == ERR_BAD_STATE)
        result("nothing is playing");
    else if (st != OK)
        result("the player said: %s", status_str(st));
}

/* `status`: the path, folder and note. */
static void ask_status(void)
{
    uint8_t playing = 0, folder[256], path[256], title[128], note[128];
    uint32_t tracks = 0, bad = 0, started = 0;
    uint64_t elapsed = 0, length = 0;
    int32_t volume = 0;
    if (music_status_until(L.ch, now() + CALL_NS, &playing, &tracks, &bad, &started, &elapsed,
                           &length, &volume, folder, path, title, note) != OK)
        return;
    folder[255] = path[255] = note[127] = 0;
    lock();
    memcpy(L.snap.path, path, sizeof(L.snap.path));
    memcpy(L.snap.folder, folder, sizeof(L.snap.folder));
    memcpy(L.snap.note, note, sizeof(L.snap.note));
    L.snap.tracks = tracks;
    unlock();
}

/* `stereo`; true if the track heard (or the state) changed. */
static bool ask_stereo(void)
{
    struct snap s;
    lock();
    s = L.snap;
    unlock();
    uint8_t playing = 0, left[BARS], right[BARS], level = 0;
    uint32_t serial = 0, sleep_s = 0;
    uint64_t el = 0, len = 0;
    int32_t vol = 0;
    status_t st = music_stereo_until(L.ch, now() + CALL_NS, &playing, &serial, &el, &len, &vol,
                                     &sleep_s, left, right, &level);
    bool changed = st == OK && (serial != s.serial || playing != s.playing);
    lock();
    L.snap.answered = st == OK;
    if (st == OK) {
        L.snap.playing = playing;
        L.snap.serial = serial;
        L.snap.elapsed_ms = el;
        L.snap.length_ms = len;
        L.snap.at = now();
        L.snap.volume = vol;
        L.snap.sleep_s = sleep_s;
        memcpy(L.snap.left, left, sizeof(left));
        memcpy(L.snap.right, right, sizeof(right));
        L.snap.level = level;
    }
    L.snap.gen++;
    unlock();
    return changed;
}

static bool pop(struct cmd *c)
{
    lock();
    bool any = L.nq > 0;
    if (any) {
        *c = L.q[0];
        memmove(L.q, L.q + 1, (L.nq - 1) * sizeof(L.q[0]));
        L.nq--;
    }
    unlock();
    return any;
}

static void link_main(void *arg)
{
    (void)arg;
    uint64_t last_status = 0;
    static struct cmd c;
    for (;;) {
        bool did = false;
        while (pop(&c)) {
            run(&c);
            did = true;
        }
        if (ask_stereo() || did || now() - last_status > STATUS_NS) {
            ask_status();
            last_status = now();
        }
        (void)jam_event_signal(L.wake, SIG_SIGNALED, 0);
        lock();
        bool more = L.nq > 0;   /* queued after the last pop: don't sleep on it */
        unlock();
        if (more)
            continue;
        (void)jam_object_wait_one(L.wake, SIG_SIGNALED, now() + POLL_NS, NULL);
    }
}

void link_start(handle_t music)
{
    L.ch = music;
    L.snap.link = false;
    if (!music || jam_event_create(&L.wake) != OK)
        return;
    void *stack = malloc(LINK_STACK);
    handle_t th;
    L.snap.link = true;   /* before the thread reads the snapshot */
    if (!stack || thread_spawn("link", link_main, NULL, stack, LINK_STACK, &th) != OK) {
        L.snap.link = false;
        free(stack);
        return;
    }
    jam_handle_close(th);
}

bool snap_stale(const struct snap *s, uint64_t t)
{
    return s->link && s->playing && t > s->at && t - s->at > SNAP_STALE_NS;
}

void link_get(struct snap *out)
{
    lock();
    *out = L.snap;
    unlock();
}

bool link_cmd(const struct cmd *c)
{
    if (!L.snap.link)
        return false;
    lock();
    bool ok = true;
    if (c->kind == CMD_VOLUME && L.nq && L.q[L.nq - 1].kind == CMD_VOLUME)
        L.q[L.nq - 1] = *c;   /* a drag: only the latest matters */
    else if (L.nq < QUEUE)
        L.q[L.nq++] = *c;
    else
        ok = false;
    unlock();
    if (ok)
        (void)jam_event_signal(L.wake, 0, SIG_SIGNALED);
    return ok;
}

bool link_result(char *out, size_t cap)
{
    lock();
    bool any = L.result[0] != '\0';
    if (any) {
        snprintf(out, cap, "%s", L.result);
        L.result[0] = '\0';
    }
    unlock();
    return any;
}
