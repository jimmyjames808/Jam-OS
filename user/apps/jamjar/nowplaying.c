/* jamjar: the now playing card: the album's jar label, the names, the
 * progress bar and the times, the buttons (stop, back, play/pause,
 * next), the volume slider and the play order. The names come from the
 * path the player says it is playing (names_of_path), so they show even
 * for a folder outside the library. */
#include "jamjar.h"

uint64_t now_elapsed(const struct app *a, uint64_t t)
{
    const struct snap *s = &a->snap;
    uint64_t e = s->elapsed_ms;
    if (s->playing == 1 && t > s->at)   /* a player gone quiet: the clock stops */
        e += (t - s->at < SNAP_STALE_NS ? t - s->at : SNAP_STALE_NS) / NS_PER_MS;
    return s->length_ms && e > s->length_ms ? s->length_ms : e;
}

static void mss(uint64_t ms, char *buf, size_t cap)
{
    uint64_t s = ms / 1000;
    snprintf(buf, cap, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

/* ---- the buttons ------------------------------------------------------------------- */

static void icon(int b, bool paused, float cx, float cy, float k, uint32_t c)
{
    if (b == BTN_STOP) {
        panel(&scr.s, (int)(cx - k * 0.5f), (int)(cy - k * 0.5f), (int)k, (int)k, (int)(k / 6), c,
              256);
    } else if (b == BTN_PLAY && paused) {
        float t[6] = { cx - k * 0.45f, cy - k * 0.62f, cx - k * 0.45f, cy + k * 0.62f,
                       cx + k * 0.62f, cy };
        poly_aa(&scr.s, t, 3, c, 255);
    } else if (b == BTN_PLAY) {
        panel(&scr.s, (int)(cx - k * 0.5f), (int)(cy - k * 0.55f), (int)(k * 0.34f),
              (int)(k * 1.1f), (int)(k / 8), c, 256);
        panel(&scr.s, (int)(cx + k * 0.16f), (int)(cy - k * 0.55f), (int)(k * 0.34f),
              (int)(k * 1.1f), (int)(k / 8), c, 256);
    } else {
        float d = b == BTN_NEXT ? 1.0f : -1.0f;   /* a triangle and a bar */
        float t[6] = { cx - d * k * 0.45f, cy - k * 0.5f, cx - d * k * 0.45f, cy + k * 0.5f,
                       cx + d * k * 0.3f, cy };
        poly_aa(&scr.s, t, 3, c, 255);
        float bx = cx + d * k * 0.3f;
        line_aa(&scr.s, bx + d * k * 0.08f, cy - k * 0.5f, bx + d * k * 0.08f, cy + k * 0.5f,
                k * 0.16f, c, 255);
    }
}

static void buttons(const struct app *a)
{
    const struct layout *lo = &a->lo;
    bool playing = a->snap.playing == 1;
    for (int b = 0; b < NBTNS; b++) {
        const struct rect *r = &lo->btn[b];
        float cx = (float)r->x + (float)r->w / 2, cy = (float)r->y + (float)r->h / 2;
        float rad = (float)r->w / 2;
        bool hover = rect_has(r, a->mx, a->my);
        if (b == BTN_PLAY) {
            disc_aa(&scr.s, cx, cy + 2.0f * lo->u, rad, 0x000000, 90);
            disc_aa(&scr.s, cx, cy, rad, hover ? C_ROSE : C_BERRY2, 255);
        } else {
            disc_aa(&scr.s, cx, cy, rad, hover ? C_LINE : C_ROW, 255);
        }
        icon(b, !playing, cx, cy, rad * (b == BTN_PLAY ? 0.62f : 0.5f), C_CREAM);
    }
}

/* ---- the bars ------------------------------------------------------------------------ */

static void progress(const struct app *a, uint64_t t)
{
    const struct layout *lo = &a->lo;
    const struct rect *r = &lo->progress;
    int u = lo->u;
    uint64_t el = now_elapsed(a, t), len = a->snap.length_ms;
    panel(&scr.s, r->x, r->y, r->w, r->h, r->h / 2, C_LINE, 256);
    int fw = len ? (int)((uint64_t)r->w * el / len) : 0;
    if (fw > r->h)
        panel(&scr.s, r->x, r->y, fw, r->h, r->h / 2, C_BERRY2, 256);
    if (len)
        disc_aa(&scr.s, (float)(r->x + fw), (float)r->y + (float)r->h / 2, 7.0f * u, C_GOLD,
                255);
    char e[16], l[16];
    mss(el, e, sizeof(e));
    if (len)
        mss(len, l, sizeof(l));
    else
        snprintf(l, sizeof(l), "%s", a->snap.path[0] ? "?" : "");
    int ty = r->y + r->h + 8 * u;
    text(&scr.s, r->x, ty, u, C_DIM, a->snap.path[0] ? e : "");
    text(&scr.s, r->x + r->w - text_width(u, l), ty, u, C_DIM, l);
}

/* The slider's position for a volume: 0..1, -60 dB at the left end and
 * silence just past it. */
static float vol_pos(int32_t cb)
{
    return cb <= -600 ? 0.0f : 1.0f + (float)cb / 600.0f;
}

int32_t vol_at(const struct layout *lo, int x)
{
    float t = (float)(x - lo->vol.x) / (float)(lo->vol.w > 0 ? lo->vol.w : 1);
    if (t <= 0.01f)
        return -960;
    t = t > 1.0f ? 1.0f : t;
    int32_t cb = (int32_t)(-600.0f * (1.0f - t));
    return cb / 5 * 5;
}

void vol_text(int32_t cb, char *out, size_t cap)
{
    int32_t m = cb < 0 ? -cb : cb;
    snprintf(out, cap, "%s%d.%d dB", cb < 0 ? "-" : "", (int)(m / 10), (int)(m % 10));
}

/* A loudspeaker at (x, y) with `waves` arcs in front of it. */
static void speaker(float x, float y, float u, int waves)
{
    fill(&scr.s, (int)(x - 6 * u), (int)(y - 3 * u), (int)(4 * u + 1), (int)(6 * u), C_DIM);
    float cone[8] = { x - 2 * u, y - 3 * u, x + 4 * u, y - 8 * u, x + 4 * u, y + 8 * u,
                      x - 2 * u, y + 3 * u };
    poly_aa(&scr.s, cone, 4, C_DIM, 255);
    for (int w = 1; w <= waves; w++) {
        float rad = (float)(4 + 4 * w) * u, px = 0, py = 0;
        for (int k = 0; k <= 6; k++) {
            double ang = -0.75 + 1.5 * k / 6;
            float qx = x + 4 * u + rad * (float)cosd(ang), qy = y + rad * (float)sind(ang);
            if (k)
                line_aa(&scr.s, px, py, qx, qy, 1.5f * u, C_DIM, 255);
            px = qx;
            py = qy;
        }
    }
}

static void volume(const struct app *a)
{
    const struct layout *lo = &a->lo;
    const struct rect *r = &lo->vol;
    int u = lo->u;
    int32_t cb = a->drag_vol ? a->vol_shown : a->snap.volume;
    float sx = (float)(r->x - 32 * u), cy = (float)r->y + (float)r->h / 2;
    speaker(sx, cy, (float)u, cb <= -960 ? 0 : cb < -300 ? 1 : 2);
    panel(&scr.s, r->x, r->y + r->h / 4, r->w, r->h / 2, r->h / 4, C_LINE, 256);
    int fw = (int)(vol_pos(cb) * (float)r->w);
    if (fw > 0)
        panel(&scr.s, r->x, r->y + r->h / 4, fw, r->h / 2, r->h / 4, C_GOLD, 256);
    disc_aa(&scr.s, (float)(r->x + fw), cy, 8.0f * u,
            a->drag_vol || rect_has(r, a->mx, a->my) ? C_CREAM : C_GOLD, 255);
    char db[24];
    if (cb <= -960)
        snprintf(db, sizeof(db), "muted");
    else
        vol_text(cb, db, sizeof(db));
    text(&scr.s, r->x + r->w + 16 * u, r->y + (r->h - TEXT_H(u)) / 2, u, C_DIM, db);
}

static void order_pill(const struct app *a)
{
    const struct rect *r = &a->lo.mode;
    int u = a->lo.u;
    bool hover = rect_has(r, a->mx, a->my);
    panel(&scr.s, r->x, r->y, r->w, r->h, r->h / 2, hover ? C_LINE : C_ROW, 256);
    const char *what = a->ordered ? "in order" : "shuffle";
    struct rect t = *r;
    text_in(&scr.s, &t, u, a->ordered ? C_CREAM : C_GOLD, what);
}

/* ---- the names ---------------------------------------------------------------------- */

static const char *state_word(uint8_t playing)
{
    static const char *const w[] = { "STOPPED", "NOW PLAYING", "READING THE FOLDER", "PAUSED" };
    return playing < 4 ? w[playing] : "";
}

static void names(const struct app *a)
{
    const struct layout *lo = &a->lo;
    const struct rect *r = &lo->title;
    int u = lo->u, y = r->y, w = r->w;
    const struct snap *s = &a->snap;
    if (!s->link) {
        text(&scr.s, r->x, y, u, C_FAINT, "NO PLAYER");
        text_clip(&scr.s, r->x, y + 28 * u, lo->tb, C_DIM, w, "Library only");
        text_clip(&scr.s, r->x, y + 70 * u, u, C_FAINT, w, "start jamjar from the shell");
        text_clip(&scr.s, r->x, y + 92 * u, u, C_FAINT, w, "(its `jamjar` command) to play");
        return;
    }
    if (snap_stale(s, now()))
        text(&scr.s, r->x, y, u, C_ROSE, "THE PLAYER DOESN'T ANSWER");
    else
        text(&scr.s, r->x, y, u, s->playing == 1 ? C_GOLD : C_FAINT, state_word(s->playing));
    y += 28 * u;
    if (!s->path[0]) {
        text_clip(&scr.s, r->x, y, lo->tb, C_DIM, w, "Nothing playing");
        text_clip(&scr.s, r->x, y + 46 * u, u, C_FAINT, w, "Enter plays the selection,");
        text_clip(&scr.s, r->x, y + 68 * u, u, C_FAINT, w, "a plays everything, r spins");
        if (s->note[0])
            text_clip(&scr.s, r->x, y + 100 * u, u, C_ROSE, w, s->note);
        return;
    }
    const struct track_names *n = &a->now;
    text_clip(&scr.s, r->x, y, lo->tb, C_CREAM, w, n->title);
    y += TEXT_H(lo->tb) + 10 * u;
    text_clip(&scr.s, r->x, y, u, C_CREAM, w, n->artist[0] ? n->artist : "(no artist)");
    y += TEXT_H(u) + 6 * u;
    char line[NAME_MAX + 16];
    snprintf(line, sizeof(line), "%s%s%s", n->album, n->year[0] ? "  " : "", n->year);
    text_clip(&scr.s, r->x, y, u, C_DIM, w, line);
}

/* Under the controls, if there is room: where the music comes from, and
 * the keys most wanted. */
static void info(const struct app *a)
{
    const struct rect *r = &a->lo.info;
    int u = a->lo.u;
    if (r->h <= 0)
        return;
    const struct snap *s = &a->snap;
    char line[FS_PATH_MAX + 64];
    fill(&scr.s, r->x, r->y, r->w, u, C_LINE);
    int y = r->y + 12 * u;
    if (s->folder[0] && s->playing) {
        const char *base = strrchr(s->folder, '/');
        char name[NAME_MAX], year[5];
        base = base && base[1] ? base + 1 : s->folder;
        name_album(base, strlen(base), name, sizeof(name), year);
        snprintf(line, sizeof(line), "from \a%s\a   %u track%s", name, s->tracks,
                 s->tracks == 1 ? "" : "s");
        text2(&scr.s, r->x, y, u, C_DIM, C_CREAM, false, line);
    } else {
        text(&scr.s, r->x, y, u, C_DIM, a->lib.ready ? "pick something to play" : "");
    }
    y += TEXT_H(u) + 10 * u;
    if (y + TEXT_H(u) <= r->y + r->h)
        text_clip(&scr.s, r->x, y, u, C_FAINT, r->w,
                  "space pause   n next   p back   / search   r roulette   ? keys");
}

void draw_now(struct app *a, uint64_t t)
{
    const struct layout *lo = &a->lo;
    int u = lo->u;
    panel(&scr.s, lo->now.x, lo->now.y, lo->now.w, lo->now.h, 14 * u, C_PANEL, 256);
    const struct rect *ar = &lo->art;
    const char *slash = strrchr(a->snap.path, '/');
    if (a->snap.path[0] && slash) {
        art_cover(&scr.s, ar->x, ar->y, ar->w, album_hash(a->snap.path,
                  (size_t)(slash - a->snap.path)), a->snap.path, C_PANEL);
    } else {
        panel(&scr.s, ar->x, ar->y, ar->w, ar->h, ar->w / 7, C_ROW, 256);
        art_mark(&scr.s, ar->x + ar->w / 5, ar->y + ar->h / 5, ar->w * 3 / 5);
    }
    names(a);
    progress(a, t);
    buttons(a);
    volume(a);
    order_pill(a);
    info(a);
}
