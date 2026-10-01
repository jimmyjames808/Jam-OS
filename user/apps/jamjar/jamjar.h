/* jamjar: what its files share. The library (library.c) and its names
 * (names.c), what the columns show (view.c), the player as last heard
 * (link.c, a thread of its own), where everything goes (layout.c), the
 * jam at the bottom (simmer.c), the album art (art.c), the roulette
 * (roulette.c), the picture (draw.c, panels.c, nowplaying.c), keys and the mouse
 * (input.c), the loop (main.c) and the self-test (selftest.c).
 * docs/history/MUSIC-GUI.md is the design. */
#pragma once

#include <fun.h>

/* ---- the palette (docs/logo/) ------------------------------------------------------- */

#define C_BG      0x1e1a1du   /* the dark */
#define C_BG2     0x161214u   /* ... darker, the screen's bottom */
#define C_PANEL   0x2a2328u   /* a card */
#define C_ROW     0x3a2f35u   /* a selected row, unfocused */
#define C_LINE    0x463a41u   /* outlines, the empty part of a bar */
#define C_CREAM   0xf6efe6u   /* text */
#define C_DIM     0xa89aa1u   /* quieter text */
#define C_FAINT   0x6f6168u   /* hints */
#define C_BERRY0  0x8e1b3au   /* the berries, darkest first */
#define C_BERRY1  0xa9244au
#define C_BERRY2  0xc8284fu
#define C_ROSE    0xf06483u   /* the jam's lit rim, a focused row's edge */
#define C_GOLD    0xd9a032u

/* ---- names (names.c) -------------------------------------------------------------------- */

#define NAME_MAX 160   /* bytes of a display name, NUL included */
#define KEY_MAX  160   /* ... of its folded search key */

/* A folder or file name for display: '_' as a space, runs of spaces as
 * one, none at the ends. n: bytes of in (it need not end there). */
void     name_tidy(const char *in, size_t n, char *out, size_t cap);
/* A file's name as a title: tidied, without its ending ("Kill_Jay_Z.mp3")
 * and a leading track number ("01 - ", "3. "). */
void     name_track(const char *file, size_t n, char *out, size_t cap);
/* An album folder's name: a leading year ("2017 4-44", "2017_4-44") goes to
 * year (4 digits and a NUL; "" if none), the rest, tidied, to name. */
void     name_album(const char *dir, size_t n, char *name, size_t cap, char year[5]);
/* The search key of a name: lower-case letters, digits and '$', accented
 * Latin letters as their base letter ("JAŸ-Z" -> "jay z", "Fünf" ->
 * "funf"), anything else one space between words. A query is folded the
 * same way, so "jay z" and "Jay-Z" both find it. */
void     name_fold(const char *in, char *out, size_t cap);
/* Whether key holds q (both folded); "" is in everything. */
bool     name_has(const char *key, const char *q);
/* FNV-1a, 64 bits. */
uint64_t name_hash(const char *s);
/* An album's art seed: the hash of the last two names of its folder
 * dir[0..n) ("Artist/Album"), so the same album hashes the same whatever
 * folder the library was read from. */
uint64_t album_hash(const char *dir, size_t n);
/* Artist, album (and its year) and title of an absolute path, as the
 * library names them: the two folders above the file. Missing ones "". */
struct track_names {
    char artist[NAME_MAX], album[NAME_MAX], year[5], title[NAME_MAX];
};
void     names_of_path(const char *path, struct track_names *out);

/* ---- the library (library.c) ------------------------------------------------------------ */

#define LIB_MAX_FILES 4096u   /* as the player's MAX_TRACKS */
#define LIB_MAX_DEPTH 8u

struct lib_track {
    char    *path;                 /* absolute */
    char     name[NAME_MAX];       /* the title */
    char     key[KEY_MAX];         /* name, folded */
    uint32_t album;                /* its album */
};
struct lib_album {
    char    *dir;                  /* absolute: what `play` is given */
    char     name[NAME_MAX];
    char     key[KEY_MAX];
    char     year[5];              /* "" if none */
    uint32_t artist;
    uint32_t first, n;             /* its tracks: track[first .. first + n) */
    uint64_t hash;                 /* album_hash of its folder: the art */
};
struct lib_artist {
    char    *dir;
    char     name[NAME_MAX];
    char     key[KEY_MAX];
    uint32_t first, n;             /* its albums */
    uint32_t tracks;
};

struct lib_scan;   /* library.c: the walk while it is going */

struct library {
    char               root[FS_PATH_MAX];
    struct lib_track  *track;
    struct lib_album  *album;
    struct lib_artist *artist;
    uint32_t           ntracks, nalbums, nartists;
    bool               ready;      /* built: the arrays above are valid */
    bool               truncated;  /* more than LIB_MAX_FILES files */
    status_t           err;        /* the root couldn't be read (ready, empty) */
    /* While the folder is read. */
    struct lib_scan   *scan;
    char             **files;
    uint32_t           nfiles;
};

/* Start reading root (absolute). ERR_NOT_FOUND, ERR_WRONG_TYPE, ERR_NO_MEMORY. */
status_t lib_begin(struct library *l, const char *root);
/* Read up to `entries` more directory entries: ERR_SHOULD_WAIT while there
 * are more; OK when the library is built (l->ready). */
status_t lib_step(struct library *l, unsigned entries);
/* Build it from these paths (absolute, under root) at once: the self-test. */
status_t lib_build(struct library *l, const char *root, const char *const *paths, uint32_t n);
void     lib_free(struct library *l);
/* The track with this path, or -1. */
int64_t  lib_find(const struct library *l, const char *path);

/* ---- what the columns show (view.c) ----------------------------------------------------- */

enum { COL_ARTIST, COL_ALBUM, COL_TRACK, NCOLS };
#define ROW_ALL UINT32_MAX   /* the artist column's first row: every artist */
#define QUERY_MAX 48

struct view {
    char      query[QUERY_MAX];    /* as typed */
    char      qkey[KEY_MAX];       /* ... folded */
    uint8_t  *tvis, *avis, *rvis;  /* per track, album, artist: shown under the query */
    uint32_t *row[NCOLS];          /* each column's rows: indices (ROW_ALL first in artists) */
    uint32_t  nrows[NCOLS];
    int       sel[NCOLS];          /* the selected row of each column */
    int       top[NCOLS];          /* the first row on the screen */
    int       col;                 /* the column the keys move in */
};

/* For this library (after it is built); false if out of memory. */
bool view_init(struct view *v, const struct library *l);
void view_free(struct view *v);
/* A new query: the columns keep what matches, the selections where they can. */
void view_query(struct view *v, const struct library *l, const char *query);
/* Select row `row` of column c (clamped) and refill the columns right of it. */
void view_select(struct view *v, const struct library *l, int c, int row);
/* Keep the selected row of each column among `rows` visible ones. */
void view_scroll(struct view *v, int rows);
/* What the selected row of column c is: an index, ROW_ALL, or -1 for none. */
int64_t view_item(const struct view *v, int c);
/* Select the track with index t (and its album and artist); false if the
 * query hides it. */
bool view_locate(struct view *v, const struct library *l, uint32_t t);

/* ---- the player (link.c) ------------------------------------------------------------------ */

/* The player as last heard: music.idl's `levels` many times a second and
 * its `status` when the track changes. */
struct snap {
    bool     link;                 /* there is a player to ask */
    bool     answered;             /* it answered the last call */
    uint8_t  playing;              /* 0 stopped, 1 playing, 2 reading, 3 paused */
    uint32_t serial;               /* changes with the track heard */
    uint64_t elapsed_ms, length_ms;
    uint64_t at;                   /* when elapsed_ms was true (uptime ns) */
    int32_t  volume;               /* centibels */
    uint32_t sleep_s;              /* 0: off */
    uint8_t  bands[16], level;
    char     path[FS_PATH_MAX];    /* the track heard ("" none) */
    char     folder[FS_PATH_MAX];
    char     note[128];            /* why it stopped by itself */
    uint32_t tracks;               /* in the folder playing */
    uint32_t gen;                  /* counts updates */
};

enum cmd_kind { CMD_PLAY, CMD_STOP, CMD_NEXT, CMD_PREV, CMD_PAUSE, CMD_VOLUME, CMD_SLEEP };
struct cmd {
    enum cmd_kind kind;
    int32_t       arg;                  /* pause: 1/0; volume: centibels; sleep: seconds */
    bool          ordered;              /* play: in order */
    char          folder[FS_PATH_MAX];  /* play */
    char          first[FS_PATH_MAX];   /* play: "" or a file to start with */
};

/* Start the thread on the player's channel (HANDLE_INVALID: none, and
 * link_get says so). */
void link_start(handle_t music);
void link_get(struct snap *out);
/* Queue a command (a volume replaces one still queued); false if full. */
bool link_cmd(const struct cmd *c);
/* The last command's outcome, once ("" none): for the toast. */
bool link_result(char *out, size_t cap);

/* ---- where everything goes (layout.c) ----------------------------------------------------- */

enum { BTN_PREV, BTN_PLAY, BTN_NEXT, BTN_STOP, NBTNS };

struct layout {
    int         w, h, u;            /* the screen; the UI scale */
    int         ts, tb;             /* text scales: small, big (the title) */
    struct rect top, mark, search, status;
    struct rect lib, head[NCOLS], list[NCOLS];
    int         row_h, rows;        /* a list row's height; rows that fit */
    struct rect now, art, title, progress, btn[NBTNS], vol, mode;
    struct rect info;               /* the folder playing and a hint (h 0: no room) */
    struct rect jam;                /* the simmer */
};
/* Everything on a w x h screen at UI scale ui. Nothing overlaps or leaves
 * the screen, from 1024x600 up. */
void layout_make(struct layout *l, int w, int h, int ui);

/* ---- the jam (simmer.c) ------------------------------------------------------------------ */

#define SIM_COLS    256   /* surface points across the width */
#define SIM_BUBBLES 48
#define SIM_DROPS   96
#define SIM_SEEDS   28

/* Positions are fractions of the jam's rectangle: x across, a bubble's y
 * and a seed's d the depth between the surface (0) and the bottom (1), a
 * drop's y the height above the bottom (it may fly past 1); speeds per
 * second (a drop's vy is downward). r: a size, 0..1. */
struct bubble { float x, y, r, vy, ph; bool live; };
struct drop   { float x, y, vx, vy, r; bool live; };
struct seed   { float x, d, r, drift; };

struct simmer {
    float         band[16];         /* smoothed, 0..1 */
    float         avg[16];          /* a slow average: a jump above it is a beat */
    float         level;
    float         rest[SIM_COLS];   /* the surface's shape from the bands (0..1 of the rect) */
    float         h[SIM_COLS];      /* ... plus the ripples on it */
    float         v[SIM_COLS];      /* ... and their speed */
    float         t;                /* seconds, for the slow waves */
    struct bubble bub[SIM_BUBBLES];
    struct drop   drop[SIM_DROPS];
    struct seed   seed[SIM_SEEDS];
    uint64_t      rng;
    int           hgt;              /* the rect height the colours were made for */
    uint32_t      lut[1024];        /* the jam's colour by depth below the surface */
    int           lut_h;
};

void simmer_init(struct simmer *s, uint64_t seed);
/* dt seconds on, with the player's bands and level (zeros: quiet). */
void simmer_step(struct simmer *s, const uint8_t bands[16], uint8_t level, float dt);
/* Into r of the screen. */
void simmer_draw(struct simmer *s, const struct surf *dst, const struct rect *r);
/* A splash at x (0..1 of the width), strength 0..1. */
void simmer_splash(struct simmer *s, float x, float strength);
/* Still moving (frames needed even with no music). */
bool simmer_busy(const struct simmer *s);

/* ---- album art (art.c) ------------------------------------------------------------------- */

/* The flavour of an album's jar: two fruit colours and a name. */
void art_flavour(uint64_t hash, uint32_t *c0, uint32_t *c1, char *name, size_t cap);
/* The jar label of hash, size x size at x, y on s, over background bg
 * (cached: drawing the same one again is a copy). */
void art_draw(const struct surf *s, int x, int y, int size, uint64_t hash, uint32_t bg);
/* The Jam OS mark (the seven drupelets) in a box `size` wide. */
void art_mark(const struct surf *s, int x, int y, int size);

/* ---- the roulette (roulette.c) ----------------------------------------------------------- */

#define ROUL_TILES 64

struct roulette {
    bool     on;
    uint64_t t0;                    /* when it started (uptime ns) */
    uint32_t tile[ROUL_TILES];      /* the reel: album indices */
    uint32_t ntiles;
    uint32_t target;                /* the reel position it lands on */
    float    pos;                   /* the reel's position now, in tiles */
    bool     landed;
    uint64_t landed_at;
    int      last_tick;             /* the tile under the pointer last frame */
    float    kick;                  /* the pointer's bounce, 0..1 */
};

/* Spin over the library's albums (false: none to spin). */
bool roulette_start(struct roulette *r, const struct library *l, uint64_t seed, uint64_t t);
/* On to time t: the album it chose once it has landed and shown it (else -1). */
int64_t roulette_step(struct roulette *r, uint64_t t);
void    roulette_draw(const struct roulette *r, const struct library *l, const struct layout *lo,
                      const struct surf *s);

/* ---- the app (main.c, input.c, draw.c, panels.c) ----------------------------------------- */

#define TOAST_NS  (2500 * NS_PER_MS)

struct app {
    struct library  lib;
    struct view     view;
    bool            view_ok;        /* view_init done */
    struct layout   lo;
    struct snap     snap;
    struct simmer   sim;
    struct roulette roul;
    bool            searching;      /* typing into the search box */
    bool            help;
    bool            ordered;        /* play in name order (s) */
    bool            full;           /* the full jar (f) */
    float           full_t;         /* ... its transition, 0..1 */
    bool            quit;
    bool            trace;          /* say what happens (the tests) */
    char            toast[96];
    uint64_t        toast_at;
    int             mx, my;         /* the pointer (-1: no mouse yet) */
    int             hover_col, hover_row;   /* the row under the pointer (-1) */
    bool            drag_vol;       /* the volume knob is held */
    int32_t         vol_shown;      /* centibels, while dragging */
    int32_t         vol_asked;      /* the volume last asked for (centibels) ... */
    uint64_t        vol_asked_at;   /* ... and when */
    uint32_t        sleep_asked;    /* the sleep timer last asked for (minutes) ... */
    uint64_t        sleep_asked_at; /* ... and when */
    uint64_t        click_at;       /* the last left press (double clicks) */
    int             click_col, click_row;
    uint32_t        heard_serial;   /* the snapshot's serial last seen */
    char            heard_path[FS_PATH_MAX];   /* ... and path */
    int64_t         now_track;      /* the library's track playing, or -1 */
    struct track_names now;         /* its names, from the path */
    uint64_t        trace_at;       /* the last levels line */
};

/* input.c */
void app_key(struct app *a, int k);
void app_mouse(struct app *a, const struct mouse *m);
void app_toast(struct app *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* Play what row `row` of column c is (an artist, an album, a track). */
void app_play_row(struct app *a, int c, int row);
void app_play_album(struct app *a, uint32_t album, const char *first);
/* The volume slider's value for x, in centibels. */
int32_t vol_at(const struct layout *lo, int x);

/* draw.c: the frame for time t into scr.s; where the jam is now (it rises
 * to 30 % of the screen in the full jar). */
void draw_frame(struct app *a, uint64_t t);
struct rect app_jam(const struct app *a);
/* panels.c and nowplaying.c: the parts of the frame. */
void draw_top(struct app *a);
void draw_library(struct app *a);
void draw_help(const struct app *a);
void draw_now(struct app *a, uint64_t t);
/* The elapsed time now, from the snapshot and the time since it. */
uint64_t now_elapsed(const struct app *a, uint64_t t);

/* selftest.c */
int jamjar_selftest(void);
