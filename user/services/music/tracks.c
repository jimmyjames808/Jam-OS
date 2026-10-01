/* music: the folder's list of files, the shuffle, and the titles.
 *
 * The list: every file below the folder whose name ends in .mp3 or .wav
 * (any case), at most MAX_DEPTH folders down; names starting with '.'
 * (macOS's .DS_Store and ._ files, .Spotlight-V100) are left out, files
 * and folders alike. The names are only ever bytes in a path handed to
 * the file calls: spaces, quotes, '$' and UTF-8 never pass through
 * anything that parses them.
 *
 * The shuffle: Fisher-Yates over the list with a seed from the clock;
 * every track is played once before the next shuffle, and a new shuffle
 * never starts with the track just played (with two or more tracks).
 *
 * The title: from the path, as music libraries lay files out
 * (Artist/Album/N. Title.mp3): the file's name without its ending and
 * without a leading track number ("1. ", "01 - ", "7-"), and the artist
 * is the folder above the album's, when the file is that deep below the
 * folder started: "Artist - Title". Shallower: the title alone. */
#include "music.h"

static void free_paths(struct tracks *t)
{
    for (uint32_t i = 0; t->path && i < t->count; i++)
        free(t->path[i]);
    free(t->path);
    free(t->bad);
    free(t->order);
    t->path = NULL;
    t->bad = NULL;
    t->order = NULL;
    t->count = t->nbad = 0;
}

void tracks_free(struct tracks *t)
{
    free_paths(t);
    t->truncated = false;
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? (char)(c - 'A' + 'a') : c;
}

bool track_wanted(const char *name)
{
    if (name[0] == '.')
        return false;
    size_t n = strlen(name);
    if (n < 5)
        return false;
    const char *e = name + n - 4;
    char x[5] = { lower(e[0]), lower(e[1]), lower(e[2]), lower(e[3]), 0 };
    return !strcmp(x, ".mp3") || !strcmp(x, ".wav");
}

static status_t add(struct tracks *t, const char *path)
{
    if (t->count == MAX_TRACKS) {
        if (!t->truncated)
            printf("music: more than %u files: the rest are left out\n", MAX_TRACKS);
        t->truncated = true;
        return OK;
    }
    size_t n = strlen(path) + 1;
    char *p = malloc(n);
    if (!p)
        return ERR_NO_MEMORY;
    memcpy(p, path, n);
    t->path[t->count++] = p;
    return OK;
}

/* dir's files into t, and its folders' (depth: how far down dir is). */
static status_t walk(struct tracks *t, char *dir, unsigned depth)
{
    struct fs_entry *e = malloc(sizeof(*e));
    if (!e)
        return ERR_NO_MEMORY;
    size_t len = strlen(dir);
    status_t st = OK;
    for (uint32_t i = 0; st == OK; i++) {
        status_t r = fs_readdir(len ? dir : "/", i, e);
        if (r == ERR_NOT_FOUND)
            break;   /* past the last */
        if (r != OK) {
            printf("music: %s: can't read the folder (%s)\n", dir, status_str(r));
            break;   /* what it had so far stays */
        }
        if (e->name[0] == '.' || (!e->is_dir && !track_wanted(e->name)))
            continue;
        size_t n = strlen(e->name);
        if (len + 1 + n >= FS_PATH_MAX) {
            printf("music: %s/%s: the path is too long: left out\n", dir, e->name);
            continue;
        }
        dir[len] = '/';
        memcpy(dir + len + 1, e->name, n + 1);
        if (!e->is_dir)
            st = add(t, dir);
        else if (depth + 1 > MAX_DEPTH)
            printf("music: %s: more than %u folders down: left out\n", dir, MAX_DEPTH);
        else
            st = walk(t, dir, depth + 1);
        dir[len] = '\0';
    }
    free(e);
    return st;
}

status_t tracks_scan(struct tracks *t, const char *folder)
{
    tracks_free(t);
    bool is_dir = false;
    status_t st = fs_stat(folder, NULL, &is_dir, NULL);
    if (st != OK)
        return st;
    if (!is_dir)
        return ERR_WRONG_TYPE;
    t->path = malloc(MAX_TRACKS * sizeof(*t->path));
    char *dir = malloc(FS_PATH_MAX);
    if (!t->path || !dir) {
        free(dir);
        tracks_free(t);
        return ERR_NO_MEMORY;
    }
    size_t n = strlen(folder);
    memcpy(dir, folder, n + 1);
    while (n > 1 && dir[n - 1] == '/')
        dir[--n] = '\0';   /* "/data/music/" */
    if (n == 1)
        dir[0] = '\0';     /* "/": its entries are "/data", ... */
    st = walk(t, dir, 0);
    free(dir);
    if (st == OK && t->count) {
        t->bad = calloc(t->count, 1);
        t->order = malloc(t->count * sizeof(*t->order));
        if (!t->bad || !t->order)
            st = ERR_NO_MEMORY;
    }
    if (st != OK) {
        tracks_free(t);
        return st;
    }
    for (uint32_t i = 0; i < t->count; i++)
        t->order[i] = i;
    t->pos = 0;
    t->last = -1;
    return OK;
}

/* splitmix64 */
static uint64_t rnd(uint64_t *s)
{
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint32_t below(uint64_t *s, uint32_t n)
{
    return (uint32_t)(rnd(s) % n);   /* n <= 4096: the bias is below 1e-15 */
}

static void shuffle(struct tracks *t)
{
    for (uint32_t i = t->count; i > 1; i--) {
        uint32_t j = below(&t->rng, i), x = t->order[i - 1];
        t->order[i - 1] = t->order[j];
        t->order[j] = x;
    }
    /* Never the same track twice in a row across two shuffles. */
    if (t->count > 1 && t->last >= 0 && t->order[0] == (uint32_t)t->last) {
        uint32_t j = 1 + below(&t->rng, t->count - 1), x = t->order[0];
        t->order[0] = t->order[j];
        t->order[j] = x;
    }
    t->pos = 0;
}

void tracks_shuffle(struct tracks *t, uint64_t seed)
{
    t->rng = seed;
    t->last = -1;
    shuffle(t);
}

int64_t tracks_next(struct tracks *t, bool *new_pass)
{
    *new_pass = false;
    if (!t->count || t->nbad >= t->count)
        return -1;
    for (;;) {
        if (t->pos >= t->count) {
            shuffle(t);
            *new_pass = true;
        }
        uint32_t i = t->order[t->pos++];
        if (!t->bad[i]) {
            t->last = i;
            return i;
        }
    }
}

/* A leading track number and its separator: "1. ", "01 - ", "7-", "3 . ",
 * also marked with '~' or '#' ("~9. Runaway": the owner's library has one).
 * A number followed by a space alone ("99 Problems") is part of the title. */
static const char *skip_number(const char *s)
{
    const char *p = s + (*s == '~' || *s == '#'), *digits = p;
    while (*p >= '0' && *p <= '9')
        p++;
    if (p == digits || p - digits > 3)
        return s;
    while (*p == ' ')
        p++;
    if (*p != '.' && *p != '-')
        return s;
    p++;
    while (*p == ' ')
        p++;
    return *p ? p : s;
}

void track_title(const char *folder, const char *path, char *out, size_t size)
{
    size_t fl = strlen(folder);
    while (fl > 1 && folder[fl - 1] == '/')
        fl--;
    const char *rel = path;
    if (!strncmp(path, folder, fl) && (path[fl] == '/' || fl == 1))
        rel = path + fl + (path[fl] == '/');
    /* Up to the last three names of rel: artist / album / file. */
    const char *name[3] = { NULL, NULL, NULL };
    size_t len[3] = { 0, 0, 0 };
    const char *p = rel;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        name[0] = name[1];
        len[0] = len[1];
        name[1] = name[2];
        len[1] = len[2];
        name[2] = p;
        len[2] = n;
        p = e ? e + 1 : p + n;
    }
    if (!name[2]) {
        snprintf(out, size, "%s", path);
        return;
    }
    /* The file's name without its ending and its number. */
    char file[FS_PATH_MAX];
    size_t n = len[2] < sizeof(file) ? len[2] : sizeof(file) - 1;
    memcpy(file, name[2], n);
    file[n] = '\0';
    char *dot = NULL;
    for (char *q = file; *q; q++)
        if (*q == '.')
            dot = q;
    if (dot && dot != file)
        *dot = '\0';
    const char *title = skip_number(file);
    if (name[0])
        snprintf(out, size, "%.*s - %s", (int)len[0], name[0], title);
    else
        snprintf(out, size, "%s", title);
}
