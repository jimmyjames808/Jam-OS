/* jamjar: what the three columns show. Artists (with "All" first), the
 * albums of the artist selected (every album for "All"), the tracks of the
 * album selected; and the search, which keeps in each column what
 * matches: a track whose name or whose album's or artist's name has the
 * query, an album that matches or holds such a track, an artist likewise.
 * Pure bookkeeping on the library's arrays: the self-test drives it. */
#include "jamjar.h"

void view_free(struct view *v)
{
    free(v->tvis);
    free(v->avis);
    free(v->rvis);
    for (int c = 0; c < NCOLS; c++)
        free(v->row[c]);
    memset(v, 0, sizeof(*v));
}

bool view_init(struct view *v, const struct library *l)
{
    memset(v, 0, sizeof(*v));
    v->tvis = malloc(l->ntracks + 1);
    v->avis = malloc(l->nalbums + 1);
    v->rvis = malloc(l->nartists + 1);
    v->row[COL_ARTIST] = malloc((l->nartists + 1) * sizeof(uint32_t));
    v->row[COL_ALBUM] = malloc((l->nalbums + 1) * sizeof(uint32_t));
    v->row[COL_TRACK] = malloc((l->ntracks + 1) * sizeof(uint32_t));
    if (!v->tvis || !v->avis || !v->rvis || !v->row[0] || !v->row[1] || !v->row[2]) {
        view_free(v);
        return false;
    }
    view_query(v, l, "");
    return true;
}

int64_t view_item(const struct view *v, int c)
{
    if (v->sel[c] < 0 || (uint32_t)v->sel[c] >= v->nrows[c])
        return -1;
    uint32_t x = v->row[c][v->sel[c]];
    return x == ROW_ALL ? (int64_t)ROW_ALL : (int64_t)x;
}

/* The row of column c that holds item x, or -1. */
static int row_of(const struct view *v, int c, int64_t x)
{
    for (uint32_t i = 0; x >= 0 && i < v->nrows[c]; i++)
        if (v->row[c][i] == (uint32_t)x)
            return (int)i;
    return -1;
}

/* Column c filled again for the selection left of it, keeping its own
 * selected item if it is still there. */
static void fill_col(struct view *v, const struct library *l, int c)
{
    int64_t keep = view_item(v, c), parent = view_item(v, c - 1);
    uint32_t n = 0;
    if (c == COL_ALBUM) {
        uint32_t from = 0, to = l->nalbums;
        if (parent >= 0 && parent != ROW_ALL) {
            from = l->artist[parent].first;
            to = from + l->artist[parent].n;
        }
        for (uint32_t b = from; parent >= 0 && b < to; b++)
            if (v->avis[b])
                v->row[c][n++] = b;
    } else {
        uint32_t from = parent >= 0 ? l->album[parent].first : 0;
        uint32_t to = parent >= 0 ? from + l->album[parent].n : 0;
        for (uint32_t t = from; t < to; t++)
            if (v->tvis[t])
                v->row[c][n++] = t;
    }
    v->nrows[c] = n;
    int r = row_of(v, c, keep);
    v->sel[c] = r >= 0 ? r : 0;
}

void view_select(struct view *v, const struct library *l, int c, int row)
{
    if (c < 0 || c >= NCOLS)
        return;
    int n = (int)v->nrows[c];
    v->sel[c] = row < 0 ? 0 : row >= n ? (n ? n - 1 : 0) : row;
    for (int k = c + 1; k < NCOLS; k++)
        fill_col(v, l, k);
}

/* Which items the query keeps. */
static void match(struct view *v, const struct library *l)
{
    const char *q = v->qkey;
    for (uint32_t r = 0; r < l->nartists; r++)
        v->rvis[r] = name_has(l->artist[r].key, q);
    for (uint32_t b = 0; b < l->nalbums; b++)
        v->avis[b] = v->rvis[l->album[b].artist] || name_has(l->album[b].key, q);
    /* rvis and avis say "matches by itself or through a parent" here; only
     * once every track is decided do they take in the tracks below them. */
    for (uint32_t t = 0; t < l->ntracks; t++)
        v->tvis[t] = v->avis[l->track[t].album] || name_has(l->track[t].key, q);
    for (uint32_t t = 0; t < l->ntracks; t++) {
        uint32_t b = l->track[t].album;
        if (v->tvis[t]) {
            v->avis[b] = 1;
            v->rvis[l->album[b].artist] = 1;
        }
    }
}

void view_query(struct view *v, const struct library *l, const char *query)
{
    snprintf(v->query, sizeof(v->query), "%s", query);
    name_fold(query, v->qkey, sizeof(v->qkey));
    int64_t keep = view_item(v, COL_ARTIST);
    match(v, l);
    uint32_t n = 0;
    v->row[COL_ARTIST][n++] = ROW_ALL;
    for (uint32_t r = 0; r < l->nartists; r++)
        if (v->rvis[r])
            v->row[COL_ARTIST][n++] = r;
    v->nrows[COL_ARTIST] = n;
    int r = row_of(v, COL_ARTIST, keep);
    view_select(v, l, COL_ARTIST, r >= 0 ? r : 0);
}

void view_scroll(struct view *v, int rows)
{
    for (int c = 0; c < NCOLS; c++) {
        int n = (int)v->nrows[c], top = v->top[c];
        if (rows < 1)
            rows = 1;
        if (v->sel[c] < top)
            top = v->sel[c];
        if (v->sel[c] >= top + rows)
            top = v->sel[c] - rows + 1;
        if (top > n - rows)
            top = n - rows;
        v->top[c] = top < 0 ? 0 : top;
    }
}

bool view_locate(struct view *v, const struct library *l, uint32_t t)
{
    if (t >= l->ntracks)
        return false;
    uint32_t b = l->track[t].album, r = l->album[b].artist;
    int ra = row_of(v, COL_ARTIST, r);
    if (ra < 0)
        return false;
    view_select(v, l, COL_ARTIST, ra);
    int rb = row_of(v, COL_ALBUM, b);
    if (rb < 0)
        return false;
    view_select(v, l, COL_ALBUM, rb);
    int rt = row_of(v, COL_TRACK, t);
    if (rt < 0)
        return false;
    view_select(v, l, COL_TRACK, rt);
    return true;
}
