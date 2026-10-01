/* music: what the player's files share (main.c: the loop and the control
 * channel; tracks.c: the folder's list, the shuffle and the titles;
 * player.c: playing it through <audio.h>). */
#pragma once

#include <audio.h>
#include <os.h>
#include <play_src.h>

#define MAX_TRACKS 4096u   /* files kept from one folder; the rest are left out (logged) */
#define MAX_DEPTH  16u     /* folders below the one started */
#define MAX_MARKS  8u      /* tracks in the stream's ring at once (short ones) */
#define TITLE_MAX  128u
#define NOTE_MAX   128u

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
    int64_t   last;        /* the track last handed out (-1: none) */
    uint64_t  rng;
};

/* Read folder (absolute) into t: every .mp3 and .wav below it, in
 * directory order. OK with t->count possibly 0; ERR_NOT_FOUND,
 * ERR_WRONG_TYPE (a file), ERR_NO_MEMORY. Frees what t held before. */
status_t tracks_scan(struct tracks *t, const char *folder);
void     tracks_free(struct tracks *t);
/* The next track to play (an index into t->path), shuffling when a pass
 * is done; -1 when every track is bad. *new_pass: a new shuffle began. */
int64_t  tracks_next(struct tracks *t, bool *new_pass);
/* Shuffle from scratch with this seed (tracks_scan leaves it unshuffled). */
void     tracks_shuffle(struct tracks *t, uint64_t seed);
/* "Artist - Title" for the file at path under folder (see tracks.c). */
void     track_title(const char *folder, const char *path, char *out, size_t size);
/* path has a .mp3 or .wav ending, any case. */
bool     track_wanted(const char *name);

/* One track's place in the stream: where in the stream's frames (48 kHz)
 * it starts, so that what is heard (the mixer's `played`) names it. */
struct mark {
    uint32_t track;
    int64_t  start;        /* stream frames; negative after a stream reopen mid-track */
    uint64_t length_ms;    /* 0: unknown */
};

struct player {
    handle_t ctl;                 /* the music channel's server end */
    struct tracks t;
    char     folder[FS_PATH_MAX];
    bool     playing;
    int32_t  volume;              /* centibels */
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
    int16_t *pcm;                 /* a chunk */
};

/* player.c */
status_t player_start(struct player *p, const char *folder, uint32_t *found);
void     player_stop(struct player *p, const char *note);
status_t player_next(struct player *p);
/* One chunk: open the next track if none is open, read it, write it.
 * Blocks at most about a mixer period (43 ms) while the ring is full. */
void     player_step(struct player *p);
/* The mark of the track heard now (NULL: none), and how far into it. */
const struct mark *player_heard(struct player *p, uint64_t *elapsed_ms);
status_t player_set_volume(struct player *p, int32_t cb, int32_t *out);
