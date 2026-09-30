/* fat: what stands between a client's bytes and FatFs. Paths are made
 * canonical here ("." and ".." resolved, so nothing FatFs sees can name a
 * place outside the volume), names are checked against FAT's long-name
 * rules (FatFs would quietly drop a trailing dot or space; here it is an
 * error), FRESULT codes become ERR_* and FAT timestamps Unix seconds. */
#include "fat.h"

static bool name_ok(const char *name, size_t len)
{
    if (len == 0 || name[len - 1] == '.' || name[len - 1] == ' ')
        return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f || strchr("\"*:<>?\\|", c))
            return false;
    }
    return true;
}

bool path_is_root(const char *p)
{
    return p[0] == '/' && p[1] == '\0';
}

/* Drop the last "/name" of out (len bytes so far); the root stays. */
static size_t pop(const char *out, size_t len)
{
    while (len > 0 && out[len - 1] != '/')
        len--;
    return len > 0 ? len - 1 : 0;
}

status_t path_resolve(const uint8_t in[FS_PATH_MAX], char out[FS_PATH_MAX])
{
    const char *s = (const char *)in;
    size_t n = strnlen(s, FS_PATH_MAX);
    if (n == FS_PATH_MAX || (n > 0 && s[0] != '/'))
        return ERR_INVALID_ARGS;
    size_t len = 0;   /* out holds "/a/b" without a NUL; "" is the root */
    for (size_t i = 0; i < n;) {
        while (i < n && s[i] == '/')
            i++;
        size_t start = i;
        while (i < n && s[i] != '/')
            i++;
        size_t seg = i - start;
        if (seg == 0 || (seg == 1 && s[start] == '.'))
            continue;
        if (seg == 2 && s[start] == '.' && s[start + 1] == '.') {
            len = pop(out, len);
            continue;
        }
        if (!name_ok(s + start, seg))
            return ERR_INVALID_ARGS;
        /* Never longer than the input: each kept name costs "/name". */
        out[len++] = '/';
        memcpy(out + len, s + start, seg);
        len += seg;
    }
    if (len == 0)
        out[len++] = '/';
    out[len] = '\0';
    return OK;
}

/* The next character of a UTF-8 string, upper-cased as FatFs compares
 * names; a byte that is not valid UTF-8 counts as itself. */
static uint32_t next_folded(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    uint32_t c = s[0];
    unsigned more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
    if (more)
        c &= 0x3fu >> more;
    unsigned i = 1;
    for (; i <= more && (s[i] & 0xc0) == 0x80; i++)
        c = c << 6 | (s[i] & 0x3f);
    if (i <= more) {   /* cut short: not UTF-8 */
        c = s[0];
        i = 1;
    }
    *p += i;
    return ff_wtoupper(c);
}

bool path_inside(const char *p, const char *dir)
{
    if (path_is_root(dir))
        return !path_is_root(p);
    while (*dir) {
        if (!*p || next_folded(&p) != next_folded(&dir))
            return false;
    }
    return *p == '/';
}

status_t fr_status(FRESULT r)
{
    switch (r) {
    case FR_OK:                  return OK;
    case FR_DISK_ERR:            return ERR_IO;
    case FR_INT_ERR:             return ERR_IO;            /* the FAT structure is broken */
    case FR_NOT_READY:           return ERR_IO;
    case FR_NO_FILE:             return ERR_NOT_FOUND;
    case FR_NO_PATH:             return ERR_NOT_FOUND;
    case FR_INVALID_NAME:        return ERR_INVALID_ARGS;
    case FR_DENIED:              return ERR_ACCESS_DENIED; /* refined where it means "full" */
    case FR_EXIST:               return ERR_ALREADY_EXISTS;
    case FR_INVALID_OBJECT:      return ERR_BAD_STATE;
    case FR_WRITE_PROTECTED:     return ERR_ACCESS_DENIED;
    case FR_INVALID_DRIVE:       return ERR_INTERNAL;      /* fat has one drive: its own bug */
    case FR_NOT_ENABLED:         return ERR_INTERNAL;
    case FR_NO_FILESYSTEM:       return ERR_IO;
    case FR_MKFS_ABORTED:        return ERR_IO;
    case FR_TIMEOUT:             return ERR_TIMED_OUT;
    case FR_LOCKED:              return ERR_BAD_STATE;     /* the file is open */
    case FR_NOT_ENOUGH_CORE:     return ERR_NO_MEMORY;
    case FR_TOO_MANY_OPEN_FILES: return ERR_NO_RESOURCES;
    case FR_INVALID_PARAMETER:   return ERR_INVALID_ARGS;
    }
    return ERR_INTERNAL;
}

/* Days from 1970-01-01 to y-m-d in the Gregorian calendar (Howard
 * Hinnant's days_from_civil: years run March to February, so the leap day
 * is the last of the year). */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* FAT: date = (year - 1980) << 9 | month << 5 | day; time = hour << 11 |
 * minute << 5 | seconds / 2. */
uint64_t fat_unix_time(WORD date, WORD time)
{
    unsigned month = date >> 5 & 15, day = date & 31;
    if (month < 1 || month > 12 || day < 1)
        return 0;
    int64_t days = days_from_civil(1980 + (date >> 9), month, day);
    return (uint64_t)days * 86400 + (time >> 11) * 3600u + (time >> 5 & 63) * 60u +
           (time & 31) * 2u;
}
