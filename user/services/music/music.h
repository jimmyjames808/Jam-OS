/* music: what the player's files share (main.c: the loop and the control
 * channel; tracks.c: the folder's list, the shuffle and the titles;
 * player.c: playing it through <audio.h>; spectrum.c: the bands of what is
 * heard, for `levels`). */
#pragma once

#include <audio.h>
#include <os.h>
#include <play_src.h>

#define MAX_TRACKS 4096u   /* files kept from one folder; the rest are left out (logged) */
#define MAX_DEPTH  16u     /* folders below the one started */
#define MAX_MARKS  8u      /* tracks in the stream's ring at once (short ones) */
#define TITLE_MAX  128u
#define NOTE_MAX   128u
#define HIST_MAX   64u     /* tracks remembered for `prev` */
#define SLEEP_MAX_S 86400u /* the sleep timer's longest */

/* The folder's files. */
struct tracks {
    char    **path;        /* MAX_TRACKS entries, each malloc'd: absolute paths */
    uint8_t  *bad;         /* per track: refused by the source (never tried again) */
    uint32_t  count, nbad;
    bool      truncated;   /* more than MAX_TRACKS were found */
    /* The shuffle: order[] is a permutation of 0 .. count - 1, played from
     * pos; after the last a new one, whose first is never the one just
     * played. */
    uint32_t *order;
    uint32_t  pos;
    bool      ordered;     /* by path instead of shuffled (`play` with order 1) */
    int64_t   last;        /* the track last handed out (-1: none) */
    uint64_t  rng;
    struct scan *scan;     /* while the folder is being read (tracks.c) */
};

/* Start reading folder (absolute) into t: every .mp3 and .wav below it,
 * in directory order. Frees what t held before. ERR_NOT_FOUND,
 * ERR_WRONG_TYPE (a file), ERR_NO_MEMORY. */
status_t tracks_scan_begin(struct tracks *t, const char *folder);
/* Read up to `entries` more directory entries. ERR_SHOULD_WAIT: more to
 * read; OK: the list is complete (t->count possibly 0), ready to shuffle;
 * ERR_NO_MEMORY (t emptied). */
status_t tracks_scan_step(struct tracks *t, unsigned entries);
void     tracks_free(struct tracks *t);
/* The next track to play (an index into t->path), shuffling when a pass
 * is done; -1 when every track is bad. *new_pass: a new shuffle began. */
int64_t  tracks_next(struct tracks *t, bool *new_pass);
/* Shuffle from scratch with this seed (the scan leaves it unshuffled). */
void     tracks_shuffle(struct tracks *t, uint64_t seed);
/* Play in the order of the paths (byte order), over and over, instead. */
void     tracks_sort(struct tracks *t);
/* Make track i the next one handed out: shuffled, it swaps places with
 * the first; in order, the list goes on from it. */
void     tracks_first(struct tracks *t, uint32_t i);
/* The index of path in the list, or -1. */
int64_t  tracks_find(const struct tracks *t, const char *path);
/* "Artist - Title" for the file at path under folder (see tracks.c). */
void     track_title(const char *folder, const char *path, char *out, size_t size);
/* path has a .mp3 or .wav ending, any case. */
bool     track_wanted(const char *name);

/* One track's place in the stream: where in the stream's frames (48 kHz)
 * it starts, so that what is heard (the mixer's `played`) names it. */
struct mark {
    uint32_t track;
    uint32_t serial;       /* `levels`' serial: new for each opening of a track */
    int64_t  start;        /* stream frames; negative after a stream reopen mid-track */
    uint64_t length_ms;    /* 0: unknown */
};

/* spectrum.c: the bands of what is written, kept by stream frame. */
#define SPEC_N     2048u   /* the FFT's size, samples */
#define SPEC_HOP   512u    /* samples between results at 48 kHz or below */
#define SPEC_BANDS 64u    /* music.idl `spectrum`; `levels` has them four to a band */
#define SPEC_RING  512u    /* results kept: 2.7 s and more, past the stream's 1.37 s ring */
#define SPEC_STALE (AUDIO_RATE / 5)   /* a result older than 0.2 s is not what is heard */

struct spec_entry {
    int64_t at;                   /* the stream frame of its window's middle */
    uint8_t band[SPEC_BANDS];     /* 0..255, low first (music.idl `spectrum`) */
    uint8_t level;                /* 0..255 */
};

struct spectrum {
    float    win[SPEC_N];         /* the Hann window */
    float    cosv[SPEC_N / 2], sinv[SPEC_N / 2];   /* the FFT's twiddles */
    uint16_t rev[SPEC_N];         /* bit reversal */
    float    re[SPEC_N], im[SPEC_N];   /* the FFT's work */
    float    in[SPEC_N];          /* the last SPEC_N mono samples, a ring from pos */
    uint32_t pos, fill;           /* where the next goes; how many are valid */
    uint32_t since, hop;          /* samples since the last result; between results */
    float    sq;                  /* sum of squares since the last result ... */
    uint32_t nsq;                 /* ... of this many samples */
    uint32_t rate;                /* the input's, which the bins below are for */
    uint16_t bin0[SPEC_BANDS], bin1[SPEC_BANDS];   /* each band's bins, inclusive */
    bool     narrow[SPEC_BANDS];  /* ... or, narrower than a bin or so, read at its middle */
    float    kc[SPEC_BANDS];      /* the middle, in bins */
    double   wide[SPEC_BANDS];    /* the width, in bins */
    float    tilt[SPEC_BANDS];    /* dB added to each band */
    struct spec_entry ring[SPEC_RING];   /* the results, oldest first from head - count */
    uint32_t head, count;
};

void spec_init(struct spectrum *s);
/* frames of `channels` interleaved samples at `rate`, whose first lands on
 * stream frame `at` (48 kHz). */
void spec_feed(struct spectrum *s, const int16_t *pcm, size_t frames, unsigned channels,
               uint32_t rate, int64_t at);
/* The result for stream frame `heard`: false (and *out zeroed) if there is
 * none within SPEC_STALE before it. */
bool spec_at(const struct spectrum *s, int64_t heard, struct spec_entry *out);
/* Drop the results from stream frame `from` on (written ahead, then
 * discarded), and start the window again. */
void spec_cut(struct spectrum *s, int64_t from);
/* Drop everything (a new stream). */
void spec_reset(struct spectrum *s);

struct player {
    handle_t ctl;                 /* the music channel's server end */
    struct tracks t;
    char     folder[FS_PATH_MAX];
    bool     scanning;            /* reading the folder (playing starts after) */
    bool     playing;
    bool     paused;              /* playing, but the stream is stopped (`pause`) */
    int32_t  volume;              /* centibels */
    char     first[FS_PATH_MAX];  /* `play`: the file to play first once the list is read */
    bool     ordered;             /* `play`: in path order, not shuffled */
    uint64_t sleep_at;            /* the sleep timer's end (uptime ns); 0: off */
    bool     fading;              /* the sleep fade has turned the stream down ... */
    int32_t  fade_cb;             /* ... to this (centibels) */
    uint32_t started;             /* tracks started since `start` */
    char     note[NOTE_MAX];      /* why it stopped by itself */
    /* While playing. */
    struct audio_out a;
    bool     a_open;
    struct jfile f;
    struct play_src src;
    bool     src_open;
    int64_t  cur;                 /* the track being written (-1: none) */
    uint64_t cur_frames;          /* of it read so far (its own rate) */
    uint32_t io_fails;            /* tracks in a row that couldn't be read */
    uint32_t reopens;             /* stream reopens in a row without a write */
    uint64_t pass_frames;         /* frames written since the last shuffle */
    struct mark marks[MAX_MARKS]; /* oldest first */
    uint32_t nmarks;
    uint32_t serial;              /* the last mark's serial */
    uint64_t heard_at;            /* the stream frame heard at the last player_heard */
    /* `prev`: the tracks opened, oldest first, and those to play before the
     * list goes on (a stack: the top plays next). */
    uint32_t hist[HIST_MAX], nhist;
    uint32_t ahead[HIST_MAX], nahead;
    int16_t *pcm;                 /* a chunk */
    struct spectrum *spec;        /* the bands (malloc'd once: 30 KiB) */
};

/* player.c */
/* Stop whatever plays and start reading folder; then up to SCAN_SYNC of
 * the reading in this call: *scanning false and *found the tracks (it
 * plays if there are any), or *scanning true and *found so far (the loop
 * reads on with player_scan and plays when it is done). */
status_t player_start(struct player *p, const char *folder, uint32_t *found, bool *scanning);
/* The same with `play`'s options: first ("" none) plays first; ordered:
 * by path, not shuffled. */
status_t player_play(struct player *p, const char *folder, const char *first, bool ordered,
                     uint32_t *found, bool *scanning);
/* While p->scanning: a few more entries; at the end, play (or stop with a
 * note if there is nothing to play). */
void     player_scan(struct player *p);
void     player_stop(struct player *p, const char *note);
status_t player_next(struct player *p);
status_t player_prev(struct player *p);
status_t player_pause(struct player *p, bool on);
/* The sleep timer: off with 0. */
void     player_sleep(struct player *p, uint32_t seconds);
/* The sleep timer's fade and end: called whenever the loop comes round. */
void     player_sleep_tick(struct player *p);
/* One chunk: open the next track if none is open, read it, write it.
 * Blocks at most about a mixer period (43 ms) while the ring is full. */
void     player_step(struct player *p);
/* The mark of the track heard now (NULL: none), and how far into it;
 * p->heard_at gets the stream frame heard. */
const struct mark *player_heard(struct player *p, uint64_t *elapsed_ms);
status_t player_set_volume(struct player *p, int32_t cb, int32_t *out);
