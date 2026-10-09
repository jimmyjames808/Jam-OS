/* shell: the history of the lines typed (sh.h), kept across restarts in
 * /data/etc/history (the ROADMAP's follow-up, the owner's 2026-10-08):
 *
 *   - the file is the lines, oldest first, one a line; every terminal's
 *     shell appends each line it runs as it runs it (no sync: fat writes
 *     it out with everything else, and init syncs /data before a reboot);
 *   - a shell starts with the shared list: the file read when /data is
 *     there (at its first prompt that finds it: the boot's first shell
 *     starts before /data is mounted, and what it ran before goes into the
 *     file then); what other terminals run later is theirs until a new
 *     shell reads it;
 *   - the last HIST_KEEP lines are kept, each once: a repeat moves to the
 *     newest place (the list read from the file keeps each line's newest
 *     place too), and the file is written again whole, so, once it has
 *     grown past HIST_REWRITE lines (appends repeat lines and grow it);
 *   - a line typed with a leading space is run but not remembered;
 *   - `history -c` forgets them all, the file too.
 *
 * Two shells appending at the same moment can lose one of the two lines
 * (both write at the file's end as they found it): it is a history. */
#include <settings.h>
#include "sh.h"

#define HIST_FILE    "/data/etc/history"
#define HIST_KEEP    500                      /* lines kept */
#define HIST_REWRITE (2 * HIST_KEEP)          /* the file written again past this */
#define HIST_READ    (512u << 10)             /* the most of the file read */

static char *hist[HIST_KEEP];   /* oldest first */
static unsigned nhist;
static bool loaded;             /* the file has been read (or found missing on /data) */
static unsigned file_lines;     /* lines in the file, as far as we know */

/* line's place in the list, or -1. */
static int find(const char *line)
{
    for (unsigned i = 0; i < nhist; i++)
        if (!strcmp(hist[i], line))
            return (int)i;
    return -1;
}

/* line (n bytes) as the newest: a repeat moved there, the oldest dropped
 * past HIST_KEEP. false: no memory. */
static bool push(const char *line, size_t n)
{
    char *copy = malloc(n + 1);
    if (!copy)
        return false;
    memcpy(copy, line, n);
    copy[n] = '\0';
    int at = find(copy);
    if (at >= 0) {
        free(hist[at]);
        memmove(&hist[at], &hist[at + 1], (nhist - (unsigned)at - 1) * sizeof(hist[0]));
        nhist--;
    } else if (nhist == HIST_KEEP) {
        free(hist[0]);
        memmove(&hist[0], &hist[1], (HIST_KEEP - 1) * sizeof(hist[0]));
        nhist--;
    }
    hist[nhist++] = copy;
    return true;
}

/* The whole list into the file (an empty list: an empty file). */
static void write_all(void)
{
    size_t n = 0;
    for (unsigned i = 0; i < nhist; i++)
        n += strlen(hist[i]) + 1;
    char *text = malloc(n + 1);
    if (!text)
        return;
    size_t o = 0;
    for (unsigned i = 0; i < nhist; i++) {
        size_t k = strlen(hist[i]);
        memcpy(text + o, hist[i], k);
        text[o + k] = '\n';
        o += k + 1;
    }
    if (settings_write(HIST_FILE, text, o) == OK)   /* a new file, then its name */
        file_lines = nhist;
    free(text);
}

/* line at the file's end. */
static void append(const char *line)
{
    struct jfile f;
    if (file_open(HIST_FILE, FS_WRITE | FS_CREATE, &f) != OK)
        return;
    uint64_t size = 0;
    size_t n = strlen(line), done = 0;
    if (file_stat(&f, &size, NULL) == OK) {
        char buf[SH_LINE_MAX + 1];
        memcpy(buf, line, n);
        buf[n] = '\n';
        if (file_write(&f, size, buf, n + 1, &done) == OK && done == n + 1)
            file_lines++;
    }
    file_close(&f);
    if (file_lines > HIST_REWRITE)
        write_all();   /* the repeats and the oldest out */
}

/* The file's lines before what is in the list now (typed before /data
 * came): read once /data is there. */
static void load(void)
{
    if (loaded || fs_statfs("/data", NULL, NULL, NULL, NULL) != OK)
        return;
    loaded = true;
    char *own[HIST_KEEP];
    unsigned nown = nhist;
    memcpy(own, hist, nhist * sizeof(hist[0]));
    nhist = 0;
    struct jfile f;
    uint64_t size = 0;
    if (file_open(HIST_FILE, FS_READ, &f) == OK) {
        if (file_stat(&f, &size, NULL) == OK && size) {
            uint64_t from = size > HIST_READ ? size - HIST_READ : 0;
            char *text = malloc((size_t)(size - from) + 1);
            size_t got = 0;
            if (text && file_read(&f, from, text, (size_t)(size - from), &got) == OK) {
                text[got] = '\0';
                char *p = text;
                if (from)   /* the read began inside a line: not a whole one */
                    p = strchr(p, '\n') ? strchr(p, '\n') + 1 : p + got;
                while (*p) {
                    char *e = strchr(p, '\n');
                    size_t n = e ? (size_t)(e - p) : strlen(p);
                    if (n && n <= SH_LINE_MAX && p[0] != ' ')
                        (void)push(p, n);
                    file_lines++;
                    p = e ? e + 1 : p + n;
                }
            }
            free(text);
        }
        file_close(&f);
    }
    for (unsigned i = 0; i < nown; i++) {   /* ours are newer: after them, and into the file */
        (void)push(own[i], strlen(own[i]));
        append(own[i]);
        free(own[i]);
    }
}

void sh_history_prompt(void)
{
    load();
}

void sh_history_add(const char *line)
{
    if (!line[0] || line[0] == ' ')
        return;   /* nothing, or a line that asks not to be remembered */
    load();
    size_t n = strnlen(line, SH_LINE_MAX);
    if (!push(line, n))
        return;
    if (loaded)
        append(hist[nhist - 1]);
}

void sh_history_clear(void)
{
    for (unsigned i = 0; i < nhist; i++)
        free(hist[i]);
    nhist = 0;
    loaded = fs_statfs("/data", NULL, NULL, NULL, NULL) == OK;
    if (loaded)
        write_all();   /* empty */
}

unsigned sh_history_count(void)
{
    return nhist;
}

const char *sh_history_at(unsigned i)
{
    return i < nhist ? hist[i] : NULL;
}

int sh_history_find(const char *prefix, int from, bool up)
{
    size_t n = strlen(prefix);
    for (int i = from + (up ? -1 : 1); i >= 0 && i < (int)nhist; i += up ? -1 : 1)
        if (!strncmp(hist[i], prefix, n))
            return i;
    return -1;
}
