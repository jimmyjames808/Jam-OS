/* music: playing the list. One mixer stream ("music" in `vol`) for as
 * long as the player plays: each track is a <play_src.h> source read a
 * chunk at a time and written through <audio.h>, which resamples it;
 * audio_set_input switches the resampler between tracks of different
 * rates with nothing lost, so tracks follow each other without a gap.
 * The stream's ring holds 1.37 s, so the player writes up to that far
 * ahead of what is heard: a track's line in the log comes about that
 * long before it is heard, and `status` names the track the mixer says
 * is heard now (each track's first frame in the stream is kept as a
 * mark).
 *
 * When things go wrong:
 *   a file the source refuses (not WAV or MP3 inside, a rate out of
 *     range): skipped with a line, marked bad and never tried again; when
 *     every file is bad the player stops;
 *   a file that can't be opened or read (the stick pulled: the mount
 *     is gone, or its reads fail): skipped with a line; IO_FAILS of them
 *     in a row and the player stops (no retry loop);
 *   a whole pass of the shuffle that played nothing: it stops;
 *   the mixer stream failing (the mixer died and init restarted it, or
 *     nothing was taken for 5 s): a new stream, the same track going on
 *     (what was in the old ring is lost); REOPENS in a row without a
 *     write in between and it stops. */
#include <idl/audio.h>
#include "music.h"

#define CHUNK    1024u   /* frames read and written at a time: 21-128 ms of sound */
#define SCAN_SYNC (500 * NS_PER_MS)   /* `start` reads the folder this long before answering */
#define SCAN_STEP 16u    /* directory entries read between looks at the channel */
#define IO_FAILS 3u
#define REOPENS  3u
#define SOON     (2 * NS_PER_S)

static bool is_io(status_t st)
{
    return st == ERR_PEER_CLOSED || st == ERR_IO || st == ERR_TIMED_OUT || st == ERR_NOT_FOUND ||
           st == ERR_BAD_STATE || st == ERR_NO_MEMORY || st == ERR_CANCELED ||
           st == ERR_ACCESS_DENIED;
}

static void close_src(struct player *p)
{
    if (p->src_open) {
        play_src_close(&p->src);
        file_close(&p->f);
    }
    p->src_open = false;
}

static void title_of(struct player *p, uint32_t i, char *out, size_t size)
{
    track_title(p->folder, p->t.path[i], out, size);
}

void player_stop(struct player *p, const char *note)
{
    if (p->scanning)
        tracks_free(&p->t);   /* a list half read is no use */
    p->scanning = false;
    close_src(p);
    if (p->a_open)
        audio_close(&p->a);   /* what is queued fades out over 5 ms */
    p->a_open = false;
    free(p->pcm);
    p->pcm = NULL;
    p->playing = false;
    p->cur = -1;
    p->nmarks = 0;
    if (note) {
        snprintf(p->note, sizeof(p->note), "%s", note);
        printf("music: stopped: %s\n", note);
    }
}

static status_t open_stream(struct player *p)
{
    status_t st = audio_open_as(&p->a, AUDIO_RATE, 2, "music");
    if (st != OK)
        return st;
    p->a_open = true;
    if (p->volume && (st = audio_set_volume(&p->a, p->volume)) != OK)
        printf("music: can't set the volume (%s)\n", status_str(st));
    return OK;
}

static void stopped_by_itself(struct player *p, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* The folder is read: play it. */
static status_t play_list(struct player *p)
{
    const char *folder = p->folder;
    status_t st;
    if (!p->t.count) {
        snprintf(p->note, sizeof(p->note), "no .mp3 or .wav files in %s", folder);
        printf("music: %s: nothing to play\n", p->note);
        return OK;
    }
    p->pcm = malloc((size_t)CHUNK * 2 * sizeof(int16_t));
    if (!p->pcm)
        return ERR_NO_MEMORY;
    if ((st = open_stream(p)) != OK) {
        free(p->pcm);
        p->pcm = NULL;
        snprintf(p->note, sizeof(p->note), "no mixer stream (%s)", status_str(st));
        return st;
    }
    /* The seed: the clock in ns (when the owner typed the command), mixed. */
    uint64_t t = now();
    tracks_shuffle(&p->t, t ^ (t << 29) ^ 0x6a616d6d75736963ull);
    p->playing = true;
    p->cur = -1;
    p->io_fails = p->reopens = 0;
    p->pass_frames = 0;
    p->nmarks = 0;
    printf("music: playing %s: %u track%s in shuffle\n", folder, p->t.count,
           p->t.count == 1 ? "" : "s");
    return OK;
}

status_t player_start(struct player *p, const char *folder, uint32_t *found, bool *scanning)
{
    *found = 0;
    *scanning = false;
    if (p->playing || p->scanning) {
        printf("music: stopping %s for %s\n", p->folder, folder);
        player_stop(p, NULL);
    }
    p->note[0] = '\0';
    p->started = 0;
    status_t st = tracks_scan_begin(&p->t, folder);
    if (st != OK) {
        p->folder[0] = '\0';
        return st;
    }
    snprintf(p->folder, sizeof(p->folder), "%s", folder);
    /* A small folder is read here and the answer says how many tracks;
     * a big one goes on in the loop, between the channel's calls. */
    uint64_t until = now() + SCAN_SYNC;
    while ((st = tracks_scan_step(&p->t, SCAN_STEP)) == ERR_SHOULD_WAIT && now() < until) {
    }
    *found = p->t.count;
    if (st == ERR_SHOULD_WAIT) {
        p->scanning = true;
        *scanning = true;
        printf("music: reading %s (%u track%s so far): it plays once it is read\n", folder,
               p->t.count, p->t.count == 1 ? "" : "s");
        return OK;
    }
    if (st != OK) {
        p->folder[0] = '\0';
        return st;
    }
    return play_list(p);
}

void player_scan(struct player *p)
{
    status_t st = tracks_scan_step(&p->t, SCAN_STEP);
    if (st == ERR_SHOULD_WAIT)
        return;
    p->scanning = false;
    if (st != OK) {
        stopped_by_itself(p, "reading %s failed (%s)", p->folder, status_str(st));
        return;
    }
    printf("music: %s read: %u track%s\n", p->folder, p->t.count, p->t.count == 1 ? "" : "s");
    if ((st = play_list(p)) != OK)
        printf("music: can't play %s (%s)\n", p->folder, status_str(st));
}

static void mark(struct player *p, uint32_t track, int64_t start, uint64_t length_ms)
{
    if (p->nmarks == MAX_MARKS) {
        memmove(p->marks, p->marks + 1, (MAX_MARKS - 1) * sizeof(p->marks[0]));
        p->nmarks--;
    }
    p->marks[p->nmarks++] = (struct mark){ track, start, length_ms };
}

static void mss(uint64_t ms, char *buf, size_t size)
{
    uint64_t s = (ms + 500) / 1000;
    snprintf(buf, size, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static void reopen(struct player *p, status_t st);

/* Track i from its beginning. OK; or the file's failure (said in the log,
 * counted). again: after `next`, the track that was queued behind the one
 * skipped starts over. */
static status_t open_track(struct player *p, uint32_t i, bool again)
{
    const char *path = p->t.path[i];
    char title[TITLE_MAX];
    title_of(p, i, title, sizeof(title));
    status_t st = file_open(path, FS_READ, &p->f);
    if (st != OK) {
        printf("music: %s: can't open it (%s): skipped\n", path, status_str(st));
        return st;
    }
    const char *why = "?";
    st = play_src_open(&p->src, &p->f, p->f.size, &why);
    if (st == OK && (st = audio_set_input(&p->a, p->src.rate, p->src.channels)) != OK) {
        play_src_close(&p->src);
        file_close(&p->f);
        if (st != ERR_NOT_SUPPORTED) {
            /* The stream failed, not the file: this track is lost (it is
             * not counted against the file). */
            reopen(p, st);
            return ERR_SHOULD_WAIT;
        }
        printf("music: %s: its rate or channels can't be played: skipped\n", path);
        if (!p->t.bad[i]) {
            p->t.bad[i] = 1;
            p->t.nbad++;
        }
        return st;
    }
    if (st != OK) {
        file_close(&p->f);
        printf("music: %s: %s: skipped\n", path, why);
        if (!is_io(st) && !p->t.bad[i]) {
            p->t.bad[i] = 1;
            p->t.nbad++;
        }
        return st;
    }
    p->src_open = true;
    p->cur = i;
    p->cur_frames = 0;
    uint64_t ms = p->src.frames && p->src.rate ? p->src.frames * 1000 / p->src.rate : 0;
    mark(p, i, (int64_t)p->a.s.write, ms);
    char len[16] = "?";
    if (ms)
        mss(ms, len, sizeof(len));
    if (again) {
        printf("music: from the start: %s (%s)\n", title, len);
    } else {
        p->started++;
        printf("music: track %u: %s (%s)\n", p->started, title, len);
    }
    return OK;
}

static void stopped_by_itself(struct player *p, const char *fmt, ...)
{
    char buf[NOTE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    player_stop(p, buf);
}

/* A file failed to open or to read. */
static void io_failed(struct player *p)
{
    if (++p->io_fails >= IO_FAILS)
        stopped_by_itself(p, "%u files in a row could not be read (was the stick pulled?)",
                          p->io_fails);
}

/* The next track in the shuffle, opened. False: none (stopped, or try
 * again at the next step after a failure). */
static bool open_next(struct player *p)
{
    bool new_pass = false;
    int64_t i = tracks_next(&p->t, &new_pass);
    if (new_pass) {
        if (!p->pass_frames) {
            stopped_by_itself(p, "nothing in %s played", p->folder);
            return false;
        }
        p->pass_frames = 0;
    }
    if (i < 0) {
        stopped_by_itself(p, "none of the %u files in %s is a WAV or MP3 file", p->t.count,
                          p->folder);
        return false;
    }
    status_t st = open_track(p, (uint32_t)i, false);
    if (st == OK)
        return true;
    if (is_io(st) && st != ERR_SHOULD_WAIT)
        io_failed(p);
    return false;
}

/* The stream failed (st): a new one, the track going on. */
static void reopen(struct player *p, status_t st)
{
    printf("music: the mixer stream failed (%s): opening a new one\n", status_str(st));
    audio_close(&p->a);
    p->a_open = false;
    if (++p->reopens > REOPENS) {
        stopped_by_itself(p, "the mixer stream failed %u times in a row (%s)", REOPENS,
                          status_str(st));
        return;
    }
    st = open_stream(p);
    if (st == OK && p->src_open)
        st = audio_set_input(&p->a, p->src.rate, p->src.channels);
    if (st != OK) {
        stopped_by_itself(p, "no new mixer stream (%s)", status_str(st));
        return;
    }
    /* The marks were in the old stream's frames: the track goes on from
     * where it was read up to. */
    p->nmarks = 0;
    if (p->src_open) {
        int64_t back = (int64_t)(p->cur_frames * AUDIO_RATE / p->src.rate);
        uint64_t ms = p->src.frames ? p->src.frames * 1000 / p->src.rate : 0;
        mark(p, (uint32_t)p->cur, -back, ms);
    }
}

void player_step(struct player *p)
{
    if (!p->src_open && !open_next(p))
        return;
    long n = play_src_read(&p->src, p->pcm, CHUNK);
    if (n <= 0) {
        if (n < 0) {
            char at[16];
            mss(p->cur_frames * 1000 / p->src.rate, at, sizeof(at));
            printf("music: %s: stopped at %s: %s\n", p->t.path[p->cur], at,
                   status_str((status_t)n));
        }
        close_src(p);
        if (n < 0)
            io_failed(p);
        return;
    }
    long w = audio_write(&p->a, p->pcm, (size_t)n);
    if (w < 0) {
        reopen(p, (status_t)w);
        return;
    }
    p->cur_frames += (uint64_t)n;
    p->pass_frames += (uint64_t)n;
    p->io_fails = p->reopens = 0;
}

const struct mark *player_heard(struct player *p, uint64_t *elapsed_ms)
{
    *elapsed_ms = 0;
    if (!p->playing || !p->a_open || !p->nmarks)
        return NULL;
    uint64_t written = 0, consumed = 0, played = 0;
    if (audio_stream_position_until(p->a.s.ch, now() + SOON, &written, &consumed, &played) != OK)
        return NULL;
    uint32_t k = 0;
    while (k + 1 < p->nmarks && p->marks[k + 1].start <= (int64_t)played)
        k++;
    /* The ones before the heard one are done with. */
    if (k) {
        memmove(p->marks, p->marks + k, (p->nmarks - k) * sizeof(p->marks[0]));
        p->nmarks -= k;
    }
    int64_t into = (int64_t)played - p->marks[0].start;
    *elapsed_ms = into > 0 ? (uint64_t)into * 1000 / AUDIO_RATE : 0;
    return &p->marks[0];
}

status_t player_next(struct player *p)
{
    if (!p->playing)
        return ERR_BAD_STATE;
    uint64_t ms = 0;
    const struct mark *m = player_heard(p, &ms);
    int64_t heard = m ? (int64_t)m->track : p->cur;
    status_t st = audio_discard(&p->a);
    if (st != OK) {
        reopen(p, st);
        if (!p->playing)
            return OK;
    }
    char title[TITLE_MAX] = "";
    if (heard >= 0)
        title_of(p, (uint32_t)heard, title, sizeof(title));
    printf("music: skipped %s\n", title);
    p->nmarks = 0;
    /* The writer was already on the track after the one heard: it starts
     * over. Else the next step opens the next one. */
    int64_t queued = p->cur;
    bool restart = p->src_open && queued >= 0 && queued != heard;
    close_src(p);
    if (restart && (st = open_track(p, (uint32_t)queued, true)) != OK && is_io(st))
        io_failed(p);
    return OK;
}

status_t player_set_volume(struct player *p, int32_t cb, int32_t *out)
{
    if (cb > 0)
        cb = 0;
    if (cb < -960)
        cb = -960;
    p->volume = cb;
    *out = cb;
    if (p->a_open) {
        status_t st = audio_set_volume(&p->a, cb);
        if (st != OK)
            return st;
    }
    return OK;
}
