/* jamjar: names for display and for search, from folder and file names.
 *
 * The owner's library is laid out Artist/Year Album/Song_Name.mp3, as his
 * downloader writes it: '_' for spaces, a year before the album's name,
 * no track numbers (other libraries have them: "01 - Song.mp3"). Display
 * names undo that; the bytes of a path are never changed, only what is
 * shown. UTF-8 stays UTF-8 (libfun draws it). The search key folds case
 * and accents so that typing plain ASCII finds "JAŸ-Z" and "Fünf". */
#include "jamjar.h"

void name_tidy(const char *in, size_t n, char *out, size_t cap)
{
    size_t o = 0;
    bool space = true;   /* drops leading spaces */
    for (size_t i = 0; i < n && in[i] && o + 1 < cap; i++) {
        char c = in[i] == '_' ? ' ' : in[i];
        if (c == ' ' && space)
            continue;
        space = c == ' ';
        out[o++] = c;
    }
    while (o && out[o - 1] == ' ')
        o--;
    out[o] = '\0';
}

/* A leading track number and its separator ("1. ", "01 - ", "7-", "~9. "),
 * as the player's titles skip it; a number then a space alone ("99
 * Problems") is part of the title. */
static const char *skip_number(const char *s)
{
    const char *p = s + (*s == '~' || *s == '#'), *digits = p;
    while (*p >= '0' && *p <= '9')
        p++;
    if (p == digits || p - digits > 3)
        return s;
    while (*p == ' ' || *p == '_')
        p++;
    if (*p != '.' && *p != '-')
        return s;
    p++;
    while (*p == ' ' || *p == '_')
        p++;
    return *p ? p : s;
}

void name_track(const char *file, size_t n, char *out, size_t cap)
{
    char buf[NAME_MAX];
    size_t k = n < sizeof(buf) - 1 ? n : sizeof(buf) - 1;
    memcpy(buf, file, k);
    buf[k] = '\0';
    char *dot = strrchr(buf, '.');
    if (dot && dot != buf)
        *dot = '\0';
    const char *t = skip_number(buf);
    name_tidy(t, strlen(t), out, cap);
}

void name_album(const char *dir, size_t n, char *name, size_t cap, char year[5])
{
    year[0] = '\0';
    bool has_year = n > 5 && (dir[4] == ' ' || dir[4] == '_');
    for (int i = 0; i < 4 && has_year; i++)
        has_year = dir[i] >= '0' && dir[i] <= '9';
    has_year = has_year && (dir[0] == '1' || dir[0] == '2');
    if (has_year) {
        memcpy(year, dir, 4);
        year[4] = '\0';
        dir += 5;
        n -= 5;
    }
    name_tidy(dir, n, name, cap);
    if (!name[0] && year[0])
        snprintf(name, cap, "%s", year);
}

/* U+00C0 .. U+017F as their base letters, lower case (' ': none). */
static const char fold_latin[] =
    "aaaaaaaceeeeiiiidnooooo ouuuuytsaaaaaaaceeeeiiiidnooooo ouuuuyty"
    "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijjkkklllllll"
    "lllnnnnnnnnnoooooooorrrrrrssssssssttttttuuuuuuuuuuuuwwyyyzzzzzzs";

/* A code point's folded character, 0 for none. */
static char fold_cp(uint32_t cp)
{
    if (cp >= 'A' && cp <= 'Z')
        return (char)(cp - 'A' + 'a');
    if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9') || cp == '$')
        return (char)cp;
    if (cp >= 0xc0 && cp < 0x180)
        return fold_latin[cp - 0xc0] == ' ' ? 0 : fold_latin[cp - 0xc0];
    switch (cp) {
    case 0xa5: return 'y';   /* ¥ */
    case 0xa9: return 'c';
    case 0xae: return 'r';
    case 0xb5: return 'u';
    case 0xb2: return '2';
    case 0xb3: return '3';
    case 0xb9: return '1';
    }
    return 0;
}

void name_fold(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    bool gap = false;
    while (*in && o + 2 < cap) {
        uint32_t cp = utf8_next(&in);
        char c = cp == '_' ? 0 : fold_cp(cp);
        if (!c) {
            gap = o > 0;
            continue;
        }
        if (gap)
            out[o++] = ' ';
        gap = false;
        out[o++] = c;
    }
    out[o] = '\0';
}

bool name_has(const char *key, const char *q)
{
    return !*q || strstr(key, q) != NULL;
}

uint64_t name_hash(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return h;
}

uint64_t album_hash(const char *dir, size_t n)
{
    /* The last two names: "Artist/Album" (or one, near the root). */
    size_t slashes = 0, s = n;
    while (s > 0 && !(dir[s - 1] == '/' && ++slashes == 2))
        s--;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = s; i < n; i++)
        h = (h ^ (uint8_t)dir[i]) * 0x100000001b3ull;
    return h;
}

void names_of_path(const char *path, struct track_names *out)
{
    memset(out, 0, sizeof(*out));
    /* The last three names: artist / album / file. */
    const char *name[3] = { NULL, NULL, NULL };
    size_t len[3] = { 0, 0, 0 };
    for (const char *p = path; *p;) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        name[0] = name[1];
        len[0] = len[1];
        name[1] = name[2];
        len[1] = len[2];
        name[2] = p;
        len[2] = n;
        p += n;
    }
    if (name[2])
        name_track(name[2], len[2], out->title, sizeof(out->title));
    if (name[1])
        name_album(name[1], len[1], out->album, sizeof(out->album), out->year);
    if (name[0])
        name_tidy(name[0], len[0], out->artist, sizeof(out->artist));
}
