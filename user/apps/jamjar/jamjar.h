/* jamjar: what its files share. The library (library.c) and its names
 * (names.c), what the columns show (view.c), the player as last heard
 * (link.c, a thread of its own), where everything goes (layout.c), the
 * stereo bars at the bottom (bars.c) and the sunburst of the big view
 * (burst.c), the album art (art.c, cover.c, id3.c; decoder.c starts the
 * helper that decodes the pictures, bin/jamcover), the roulette
 * (roulette.c), the picture (draw.c, panels.c, nowplaying.c), keys and the mouse
 * (input.c), the loop (main.c) and the self-test (selftest.c).
 * docs/history/MUSIC-GUI.md is the design. */
#pragma once

#include <fun.h>
#include <jamcover.h>

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

#define BARS 64   /* bars a channel: the bands of the player's `stereo` */

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

/* The player as last heard: music.idl's `stereo` many times a second and
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
    uint8_t  left[BARS], right[BARS];   /* what each channel has in each band, 0..255 */
    uint8_t  level;
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
/* A snapshot of a player that was playing and has not answered for
 * SNAP_STALE_NS (it is down, or hangs): not to be shown as what plays now.
 * (A `play` of a big folder keeps it busy for about 0.5 s.) */
#define SNAP_STALE_NS (1500 * NS_PER_MS)
bool snap_stale(const struct snap *s, uint64_t t);
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
    struct rect jam;                /* the stereo bars */
    /* The big view (f): the sunburst in a circle, the cover and the names
     * in a column at its left. */
    struct rect big_art, big_text;  /* the cover; the title, artist and album */
    int         burst_x, burst_y;   /* the sunburst's centre ... */
    int         burst_r;            /* ... and how far it may reach */
};
/* Everything on a w x h screen at UI scale ui. Nothing overlaps or leaves
 * the screen, from 1024x600 up. */
void layout_make(struct layout *l, int w, int h, int ui);

/* ---- the bars (bars.c) and the sunburst (burst.c) --------------------------------------- */

enum { CH_LEFT, CH_RIGHT, NCH };

struct bars {
    float v[NCH][BARS];  /* each bar's height now, 0..1, per channel */
    float avg[BARS];     /* each band's long average, both channels (normalisation) */
    float gain[BARS];    /* ... and the gain it gets from it */
    float spin;          /* the sunburst's turn, radians */
    float speed;         /* ... and how fast it turns now, radians a second */
};

void bars_init(struct bars *b);
/* dt seconds on, toward the player's bands of each channel (live false:
 * toward zero, and the sunburst slows to a stop). */
void bars_step(struct bars *b, const uint8_t left[BARS], const uint8_t right[BARS], bool live,
               float dt);
/* Into r of the screen: the left channel's bars up from the middle, the
 * right's down. */
void bars_draw(const struct bars *b, const struct surf *dst, const struct rect *r);
/* Still moving (frames needed even with nothing heard). */
bool bars_busy(const struct bars *b);
/* The bass (the mean of bands 2-5, both channels) and the mean of every
 * bar, 0..1. */
float bars_bass(const struct bars *b);
float bars_energy(const struct bars *b);
/* The self-test: where bar i of the strip in r is, [x0, x1) across, and
 * the y of the line between the channels. */
void bars_where(const struct rect *r, int i, int *x0, int *x1, int *mid);

/* The sunburst: a disc pulsing with the bass and 128 rays, the left
 * channel's 64 bands round one half and the right's round the other
 * (bass meets bass at the bottom, the highs at the top, before it turns),
 * centred on (cx, cy) and never further than `reach` from it; `grow`
 * 0..1 scales it (the big view coming). */
void burst_draw(const struct bars *b, const struct surf *dst, int cx, int cy, int reach,
                float grow);

/* ---- album art (art.c) ------------------------------------------------------------------- */

/* The flavour of an album's jar: two fruit colours and a name. */
void art_flavour(uint64_t hash, uint32_t *c0, uint32_t *c1, char *name, size_t cap);
/* The jar label of hash, size x size at x, y on s, over background bg
 * (cached: drawing the same one again is a copy). */
void art_draw(const struct surf *s, int x, int y, int size, uint64_t hash, uint32_t bg);
/* The album's real cover from the track at path, if it has one and it is
 * read; its jar label until then, and for good if it has none. */
void art_cover(const struct surf *s, int x, int y, int size, uint64_t hash, const char *path,
               uint32_t bg);
/* The Jam OS mark (the seven drupelets) in a box `size` wide. */
void art_mark(const struct surf *s, int x, int y, int size);
/* Bytes of the pictures art_draw and art_cover keep (the self-test). */
size_t art_cache_bytes(void);
/* The self-test: call `between` in art_cover after it has asked cover.c
 * what is ready and before it draws that (NULL: none), to change the
 * covers' state at exactly that point. */
void art_test_hook(void (*between)(void));

/* ---- album covers (id3.c, cover.c, decoder.c) ---------------------------------------- */

/* id3.c: the picture in an MP3's ID3v2 tag. */
#define ID3_HEADER 10
struct id3_pic {
    const uint8_t *data;    /* the image's bytes, inside the tag */
    size_t         len;
    uint8_t        type;    /* the APIC picture type: 3 is the front cover */
    bool           png, jpeg;   /* what its first bytes say it is */
};
/* The whole tag's length (header, body, footer) from its first ID3_HEADER
 * bytes; 0 if they are not an ID3v2.2-2.4 header. */
size_t id3_tag_size(const uint8_t h[ID3_HEADER]);
/* The cover in the tag tag[0..n) (n at least id3_tag_size): the front
 * cover if there is one, else the first PNG or JPEG picture. The tag is
 * changed in place (unsynchronisation undone). false: none. */
bool   id3_cover(uint8_t *tag, size_t n, struct id3_pic *out);

/* decoder.c: the cover helper (bin/jamcover, <jamcover.h>), started on
 * the first decode. The input buffer, JAMCOVER_IN_BYTES (NULL: no
 * memory): put the picture at its start. */
uint8_t *decoder_buffer(void);
/* The picture in the buffer's first len bytes decoded by the helper into
 * small_px (COVER_SMALL squared) and, with large, large_px (COVER_LARGE
 * squared); *w, *h its own size. jamcover.idl's decode errors; any other
 * (ERR_TIMED_OUT: no answer in JAMCOVER_TIMEOUT, ERR_PEER_CLOSED: it
 * crashed) means the helper was killed: the next call starts another. */
status_t decoder_decode(size_t len, bool large, uint32_t *small_px, uint32_t *large_px, int *w,
                        int *h);
/* The self-test: stop the helper; the next one is started with arg
 * (--crash, --hang or NULL), and decodes may take timeout ns (0: the
 * usual). How many helpers were started so far. */
void     decoder_test(const char *arg, uint64_t timeout);
unsigned decoder_test_starts(void);

/* cover.c: covers read and decoded by a thread of its own, kept scaled
 * per album. */
#define COVER_SMALL JAMCOVER_SMALL   /* every album's cover is kept this big ... */
#define COVER_LARGE JAMCOVER_LARGE   /* ... and the few drawn bigger at this size too */
enum { COVER_NONE, COVER_SMALL_KIND, COVER_LARGE_KIND };
/* Start the thread (trace: say each cover read). */
void cover_start(bool trace);
/* Album `hash`'s cover, from the track at `path`, for drawing `size` wide:
 * which image is ready (COVER_*); not ready ones are asked for (the newest
 * asked first). `low`: ask behind everything else (reading ahead). */
int  cover_ready(uint64_t hash, const char *path, int size, bool low);
/* That image scaled to fill dst (a square, its stride its width) with
 * rounded corners over bg; false if it is gone meanwhile, and then dst
 * is not touched. */
bool cover_render(const struct surf *dst, uint64_t hash, int kind, uint32_t bg);
/* The self-test: the covers without their thread and without files.
 * cover_test_work does the thread's next job, with a picture of one
 * colour (made from the path) instead of the file's; false: no job. */
bool cover_test_start(void);
bool cover_test_work(void);

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

/* Spin over the library's albums (false: none to spin); it doesn't land
 * on album `avoid` (the one playing; -1: none) unless it is the only one. */
bool roulette_start(struct roulette *r, const struct library *l, uint64_t seed, int64_t avoid,
                    uint64_t t);
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
    struct bars     bars;
    struct roulette roul;
    bool            searching;      /* typing into the search box */
    bool            help;
    bool            ordered;        /* play in name order (s) */
    bool            full;           /* the big view (f) */
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
/* A volume in centibels as "-12.5 dB" (the sign kept above -1 dB too). */
void    vol_text(int32_t cb, char *out, size_t cap);

/* draw.c: the frame for time t into scr.s. */
void draw_frame(struct app *a, uint64_t t);
/* panels.c and nowplaying.c: the parts of the frame. */
void draw_top(struct app *a);
void draw_library(struct app *a);
void draw_help(const struct app *a);
void draw_now(struct app *a, uint64_t t);
/* The elapsed time now, from the snapshot and the time since it. */
uint64_t now_elapsed(const struct app *a, uint64_t t);

/* selftest.c; covertest.c, its covers part */
int  jamjar_selftest(void);
void test_covers(void);
