/* jamjar: the library, read from the music folder: artists, their albums,
 * the albums' tracks.
 *
 * The walk: every .mp3 and .wav file below the root (any case; names
 * starting with '.' left out, as the player does), at most LIB_MAX_DEPTH
 * folders down, a few directory entries per call (lib_step), so the
 * window is up at once and fills in. Then the build: a file's album is
 * the folder it is in and its artist the folder above that, as music
 * libraries are laid out (Artist/Album/Song.mp3). A file right in an
 * artist's folder is in that artist's "(loose tracks)", one right in the
 * root has "(no artist)". The album's folder is what `play` is given, so
 * playing an album plays exactly its folder.
 *
 * Order: artists by folder name, their albums by folder name (a leading
 * year makes that their release order), tracks by file name; each
 * comparison ignores ASCII case (and bytes past ASCII sort after it:
 * "¥$" comes last). The files are sorted once by (artist folder, album
 * folder, path), which makes every artist and every album a run of
 * neighbours. */
#include "jamjar.h"

struct lib_scan {
    char     dir[FS_PATH_MAX];               /* the folder being read (and its parents) */
    size_t   len[LIB_MAX_DEPTH + 1];         /* level k is dir[0 .. len[k]) */
    uint32_t idx[LIB_MAX_DEPTH + 1];         /* the next entry to read at each level */
    unsigned depth;
    bool     done;
    struct fs_entry e;
};

static bool wanted(const char *name)
{
    size_t n = strlen(name);
    if (name[0] == '.' || n < 5)
        return false;
    char x[5];
    for (int i = 0; i < 4; i++) {
        char c = name[n - 4 + i];
        x[i] = c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
    }
    x[4] = '\0';
    return !strcmp(x, ".mp3") || !strcmp(x, ".wav");
}

void lib_free(struct library *l)
{
    for (uint32_t i = 0; l->files && i < l->nfiles; i++)
        free(l->files[i]);
    for (uint32_t i = 0; l->album && i < l->nalbums; i++)
        free(l->album[i].dir);
    for (uint32_t i = 0; l->artist && i < l->nartists; i++)
        free(l->artist[i].dir);
    free(l->files);
    free(l->track);
    free(l->album);
    free(l->artist);
    free(l->scan);
    char root[FS_PATH_MAX];
    memcpy(root, l->root, sizeof(root));
    memset(l, 0, sizeof(*l));
    memcpy(l->root, root, sizeof(root));
}

static void set_root(struct library *l, const char *root)
{
    size_t n = strnlen(root, FS_PATH_MAX - 1);
    memcpy(l->root, root, n);
    l->root[n] = '\0';
    while (n > 1 && l->root[n - 1] == '/')
        l->root[--n] = '\0';
}

status_t lib_begin(struct library *l, const char *root)
{
    lib_free(l);
    set_root(l, root);
    bool dir = false;
    status_t st = fs_stat(l->root, NULL, &dir, NULL);
    if (st == OK && !dir)
        st = ERR_WRONG_TYPE;
    if (st != OK)
        return st;
    l->files = malloc(LIB_MAX_FILES * sizeof(*l->files));
    l->scan = calloc(1, sizeof(*l->scan));
    if (!l->files || !l->scan) {
        lib_free(l);
        return ERR_NO_MEMORY;
    }
    memcpy(l->scan->dir, l->root, sizeof(l->root));
    l->scan->len[0] = strlen(l->root);
    return OK;
}

static status_t add_file(struct library *l, const char *path)
{
    if (l->nfiles == LIB_MAX_FILES) {
        l->truncated = true;
        return OK;
    }
    size_t n = strlen(path) + 1;
    char *p = malloc(n);
    if (!p)
        return ERR_NO_MEMORY;
    memcpy(p, path, n);
    l->files[l->nfiles++] = p;
    return OK;
}

/* One entry of the folder at the top of the walk's stack. */
static status_t scan_one(struct library *l, struct lib_scan *sc)
{
    unsigned k = sc->depth;
    size_t len = sc->len[k];
    sc->dir[len] = '\0';
    if (fs_readdir(len ? sc->dir : "/", sc->idx[k]++, &sc->e) != OK) {
        if (k == 0)
            sc->done = true;   /* past the last (or unreadable): what it has stays */
        else
            sc->depth--;
        return OK;
    }
    const char *name = sc->e.name;
    if (name[0] == '.' || (!sc->e.is_dir && !wanted(name)))
        return OK;
    size_t n = strlen(name);
    if (len + 1 + n >= FS_PATH_MAX)
        return OK;
    sc->dir[len] = '/';
    memcpy(sc->dir + len + 1, name, n + 1);
    if (!sc->e.is_dir)
        return add_file(l, sc->dir);
    if (k + 1 <= LIB_MAX_DEPTH) {
        sc->depth = k + 1;
        sc->len[k + 1] = len + 1 + n;
        sc->idx[k + 1] = 0;
    }
    return OK;
}

/* ---- the build ------------------------------------------------------------------------ */

/* A file and where its artist's and album's folders end in its path. */
struct sort_file {
    const char *path;
    uint16_t    art, alb;   /* lengths of the artist's and album's folders */
};

/* The folders of path below root (rl bytes): the album's is the file's
 * folder and the artist's the one above it; a file one folder below the
 * root is that folder's (the artist's) loose track, and one right in the
 * root has neither (both lengths rl). */
static void folders(const char *path, size_t rl, uint16_t *art, uint16_t *alb)
{
    size_t a = (size_t)(strrchr(path, '/') - path);
    size_t r = a;
    while (r > rl && path[r - 1] != '/')
        r--;
    r = r > rl ? r - 1 : rl;   /* the '/' before the album's folder, or the root */
    if (r == rl && a > rl)
        r = a;                 /* root/Artist/song: the album is the artist's folder */
    *alb = (uint16_t)a;
    *art = (uint16_t)r;
}

/* Compare a[0..an) and b[0..bn), '/' before anything, ignoring ASCII case
 * if `fold` (then two names differing only in case are told apart by a
 * second, exact comparison, so each stays a run of its own). */
static int cmp_n(const char *a, size_t an, const char *b, size_t bn, bool fold)
{
    for (size_t i = 0;; i++) {
        if (i == an || i == bn)
            return (int)(i != an) - (int)(i != bn);
        int x = (uint8_t)a[i], y = (uint8_t)b[i];
        x = x == '/' ? 1 : fold && x >= 'A' && x <= 'Z' ? x + 32 : x;
        y = y == '/' ? 1 : fold && y >= 'A' && y <= 'Z' ? y + 32 : y;
        if (x != y)
            return x - y;
    }
}

static int cmp_part(const char *a, size_t an, const char *b, size_t bn)
{
    int c = cmp_n(a, an, b, bn, true);
    return c ? c : cmp_n(a, an, b, bn, false);
}

static int cmp_file(const struct sort_file *a, const struct sort_file *b)
{
    int c = cmp_part(a->path, a->art, b->path, b->art);
    if (!c)
        c = cmp_part(a->path, a->alb, b->path, b->alb);
    if (!c)
        c = cmp_part(a->path, strlen(a->path), b->path, strlen(b->path));
    return c;
}

/* Shell sort: no recursion, no extra memory; 4096 files in a few ms. */
static void sort_files(struct sort_file *f, uint32_t n)
{
    static const uint32_t gaps[] = { 1750, 701, 301, 132, 57, 23, 10, 4, 1 };
    for (unsigned g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++)
        for (uint32_t i = gaps[g]; i < n; i++) {
            struct sort_file x = f[i];
            uint32_t j = i;
            for (; j >= gaps[g] && cmp_file(&f[j - gaps[g]], &x) > 0; j -= gaps[g])
                f[j] = f[j - gaps[g]];
            f[j] = x;
        }
}

static char *dup_n(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

/* The last name of path[0..n). */
static const char *last_name(const char *path, size_t n, size_t *len)
{
    size_t s = n;
    while (s > 0 && path[s - 1] != '/')
        s--;
    *len = n - s;
    return path + s;
}

static status_t new_artist(struct library *l, const struct sort_file *f, size_t rl)
{
    struct lib_artist *a = &l->artist[l->nartists];
    memset(a, 0, sizeof(*a));
    if (!(a->dir = dup_n(f->path, f->art)))
        return ERR_NO_MEMORY;
    size_t n;
    const char *name = last_name(f->path, f->art, &n);
    if (f->art <= rl)
        snprintf(a->name, sizeof(a->name), "(no artist)");
    else
        name_tidy(name, n, a->name, sizeof(a->name));
    name_fold(a->name, a->key, sizeof(a->key));
    a->first = l->nalbums;
    l->nartists++;
    return OK;
}

static status_t new_album(struct library *l, const struct sort_file *f, size_t rl)
{
    struct lib_album *b = &l->album[l->nalbums];
    memset(b, 0, sizeof(*b));
    if (!(b->dir = dup_n(f->path, f->alb)))
        return ERR_NO_MEMORY;
    size_t n;
    const char *name = last_name(f->path, f->alb, &n);
    if (f->alb == f->art || f->alb <= rl)
        snprintf(b->name, sizeof(b->name), "(loose tracks)");
    else
        name_album(name, n, b->name, sizeof(b->name), b->year);
    name_fold(b->name, b->key, sizeof(b->key));
    b->artist = l->nartists - 1;
    b->first = l->ntracks;
    b->hash = album_hash(b->dir, f->alb);
    l->artist[b->artist].n++;
    l->nalbums++;
    return OK;
}

/* The arrays from the sorted files. */
static status_t group(struct library *l, const struct sort_file *f, uint32_t n)
{
    size_t rl = strlen(l->root);
    l->track = calloc(n ? n : 1, sizeof(*l->track));
    l->album = calloc(n ? n : 1, sizeof(*l->album));
    l->artist = calloc(n ? n : 1, sizeof(*l->artist));
    if (!l->track || !l->album || !l->artist)
        return ERR_NO_MEMORY;
    status_t st = OK;
    for (uint32_t i = 0; i < n && st == OK; i++) {
        bool art = !i || cmp_n(f[i].path, f[i].art, f[i - 1].path, f[i - 1].art, false);
        bool alb = art || cmp_n(f[i].path, f[i].alb, f[i - 1].path, f[i - 1].alb, false);
        if (art)
            st = new_artist(l, &f[i], rl);
        if (st == OK && alb)
            st = new_album(l, &f[i], rl);
        if (st != OK)
            break;
        struct lib_track *t = &l->track[l->ntracks++];
        t->path = (char *)f[i].path;
        size_t fn;
        const char *file = last_name(f[i].path, strlen(f[i].path), &fn);
        name_track(file, fn, t->name, sizeof(t->name));
        name_fold(t->name, t->key, sizeof(t->key));
        t->album = l->nalbums - 1;
        l->album[t->album].n++;
        l->artist[l->album[t->album].artist].tracks++;
    }
    return st;
}

/* Sort l->files and make the arrays; the tracks' paths are l->files'. */
static status_t build(struct library *l)
{
    size_t rl = strlen(l->root);
    struct sort_file *f = malloc((l->nfiles ? l->nfiles : 1) * sizeof(*f));
    if (!f)
        return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < l->nfiles; i++) {
        f[i].path = l->files[i];
        folders(l->files[i], rl, &f[i].art, &f[i].alb);
    }
    sort_files(f, l->nfiles);
    status_t st = group(l, f, l->nfiles);
    free(f);
    l->ready = st == OK;
    return st;
}

status_t lib_step(struct library *l, unsigned entries)
{
    struct lib_scan *sc = l->scan;
    if (!sc)
        return l->ready ? OK : ERR_BAD_STATE;
    status_t st = OK;
    for (unsigned i = 0; i < entries && !sc->done && st == OK; i++)
        st = scan_one(l, sc);
    if (st == OK && !sc->done)
        return ERR_SHOULD_WAIT;
    free(l->scan);
    l->scan = NULL;
    if (st == OK)
        st = build(l);
    if (st != OK) {
        lib_free(l);
        l->err = st;
        l->ready = true;   /* empty */
    }
    return OK;
}

status_t lib_build(struct library *l, const char *root, const char *const *paths, uint32_t n)
{
    lib_free(l);
    set_root(l, root);
    l->files = malloc((n ? n : 1) * sizeof(*l->files));
    if (!l->files)
        return ERR_NO_MEMORY;
    status_t st = OK;
    for (uint32_t i = 0; i < n && st == OK; i++)
        st = add_file(l, paths[i]);
    if (st == OK)
        st = build(l);
    if (st != OK)
        lib_free(l);
    return st;
}

int64_t lib_find(const struct library *l, const char *path)
{
    for (uint32_t i = 0; l->ready && i < l->ntracks; i++)
        if (!strcmp(l->track[i].path, path))
            return i;
    return -1;
}
