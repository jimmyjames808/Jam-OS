/* logd: the log of the boot before this one, when it panicked
 * (<crashlog.h>): saved before this boot's own log is opened.
 *
 * init starts logd with SR_CRASHLOG: a read-only VMO holding a struct
 * crashlog_header (<jam/startup.h>) and the text, the panicked kernel's
 * log ring from its oldest byte (the kernel checked the ring and copied
 * it: kernel/kexec/crashlog.c). The file is /data/logs/<name>-crash.txt,
 * <name> being the panicked boot's own log file's ("boot-0042"), or for a
 * boot that had none (or whose crash file is somehow there already:
 * nothing is overwritten) the next free number (logfile.c): a few lines
 * saying what it is, then the text as it was, synced. */
#include <crashlog.h>
#include <os.h>
#include "logd.h"

#define CHUNK 4096u

/* The lines before the text. */
static int intro(char *buf, size_t size, const struct crashlog_header *h)
{
    uint64_t ms = h->uptime_ns / NS_PER_MS;
    int n = snprintf(buf, size,
                     "Jam OS crash log: the end of the kernel log of %s, which panicked after "
                     "%lu.%03lu s (panic %u in a row), saved by the boot after it.\n"
                     "The panic: %s\n",
                     h->name[0] ? h->name : "a boot that had no log file of its own",
                     (unsigned long)(ms / 1000), (unsigned long)(ms % 1000), h->panics,
                     h->message[0] ? h->message : "(no message)");
    if (h->lost)
        n += snprintf(buf + n, size - (size_t)n,
                      "The kernel keeps the last %lu bytes of its log: the %lu before them "
                      "are not here%s%s%s.\n", (unsigned long)h->text_len,
                      (unsigned long)h->lost, h->name[0] ? " (" : "",
                      h->name[0] ? h->name : "", h->name[0] ? ".txt has what it saved)" : "");
    n += snprintf(buf + n, size - (size_t)n, "The panic starts at byte %lu of the log below.\n"
                  "--------\n", (unsigned long)h->panic_at);
    return n;
}

/* The intro, then the text, read from the VMO a chunk at a time. */
static status_t write_all(const struct store *s, handle_t vmo, const struct crashlog_header *h)
{
    static char buf[CHUNK];
    uint64_t at = 0;
    int n = intro(buf, sizeof(buf), h);
    status_t st = s->write(at, buf, (uint32_t)n);
    at += (uint64_t)n;
    for (uint64_t off = 0; st == OK && off < h->text_len; off += CHUNK) {
        uint32_t len = h->text_len - off < CHUNK ? (uint32_t)(h->text_len - off) : CHUNK;
        st = jam_vmo_read(vmo, sizeof(*h) + off, buf, len);
        if (st == OK)
            st = s->write(at, buf, len);
        at += len;
    }
    return st == OK ? s->sync() : st;
}

status_t logd_save_crash(const struct store *s, handle_t vmo, char *path, size_t size)
{
    struct crashlog_header h;
    uint64_t vsize = 0;
    path[0] = '\0';
    if (jam_vmo_get_size(vmo, &vsize) != OK || vsize < sizeof(h) ||
        jam_vmo_read(vmo, 0, &h, sizeof(h)) != OK || !crashlog_header_ok(&h, vsize))
        return ERR_INVALID_ARGS;
    uint64_t have = 0;
    status_t st = logfile_crash_path(s, h.name, path, size);
    /* A boot is named once, so its crash file can't be there yet; if it
     * is (a name someone else gave the kernel), it stays, and this log
     * takes the next free number instead. */
    if (st == OK && h.name[0] && s->stat(path) == OK)
        st = logfile_crash_path(s, "", path, size);
    if (st == OK)
        st = s->open(path, FS_WRITE | FS_CREATE | FS_TRUNCATE, &have);
    if (st == OK) {
        st = write_all(s, vmo, &h);
        s->close();
    }
    return st;
}
