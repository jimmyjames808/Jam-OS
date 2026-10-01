/* logd crash: in a crash kernel's boot, save the crashed kernel's log.
 *
 * init starts it once /data is mounted, with SR_CRASHLOG: a read-only VMO
 * holding a struct crashlog_header (<jam/startup.h>) and the text, the
 * crashed kernel's log ring from its oldest byte (the kernel checked the
 * ring and copied it: kernel/kexec/crashlog.c). The file is
 * /data/logs/<name>-crash.txt, <name> being the crashed boot's own log
 * file's ("boot-0042"), or for a boot that had none (or whose crash file
 * is somehow there already: nothing is overwritten) the next free number
 * (logfile.c): a few lines saying what it is, then the text as it was.
 * Synced, then one RESULTS line (debug_report) says where it went, or
 * why it didn't: the crash kernel's last screen shows it. */
#include <os.h>
#include "logd.h"

#define CHUNK   4096u
#define TEXT_MAX (1u << 20)   /* the kernel's ring is 64 KiB; anything far bigger is wrong */

/* "crash: ..." into the RESULTS box and the log. */
static void tell(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void tell(const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    jam_debug_report(buf, (uint64_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
}

static bool header_ok(const struct crashlog_header *h, uint64_t vmo_size)
{
    if (h->magic != CRASHLOG_MAGIC || h->version != CRASHLOG_VERSION || h->reserved ||
        h->text_len > TEXT_MAX || h->text_len > vmo_size - sizeof(*h) ||
        h->panic_at > h->text_len || strnlen(h->name, sizeof(h->name)) == sizeof(h->name))
        return false;
    for (const char *p = h->name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return false;
    return true;
}

/* The lines before the text. */
static int intro(char *buf, size_t size, const struct crashlog_header *h)
{
    int n = snprintf(buf, size,
                     "Jam OS crash log: the end of the kernel log of %s, saved by the crash "
                     "kernel after that kernel panicked.\n",
                     h->name[0] ? h->name : "a boot that had no log file of its own");
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

int logd_crash(void)
{
    handle_t vmo = startup_handle(SR_CRASHLOG);
    struct crashlog_header h;
    uint64_t size = 0;
    if (!vmo || jam_vmo_get_size(vmo, &size) != OK || size < sizeof(h) ||
        jam_vmo_read(vmo, 0, &h, sizeof(h)) != OK || !header_ok(&h, size)) {
        tell("crash: the log was NOT saved: logd got no crashed kernel's log it could read");
        return 1;
    }
    const struct store *s = &store_ns;
    char path[96] = "";
    uint64_t have = 0;
    status_t st = logfile_crash_path(s, h.name, path, sizeof(path));
    /* A boot is named once, so its crash file can't be there yet; if it
     * is (a name someone else gave the kernel), it stays, and this log
     * takes the next free number instead. */
    if (st == OK && h.name[0] && s->stat(path) == OK)
        st = logfile_crash_path(s, "", path, sizeof(path));
    if (st == OK)
        st = s->open(path, FS_WRITE | FS_CREATE | FS_TRUNCATE, &have);
    if (st == OK) {
        st = write_all(s, vmo, &h);
        s->close();
    }
    if (st != OK) {
        tell("crash: the log was NOT saved: %s %s (%s)", path[0] ? "writing" : "finding a name on",
             path[0] ? path : "/data/logs", status_str(st));
        return 1;
    }
    tell("crash: the crashed kernel's log is saved as %s (%lu bytes)", path,
         (unsigned long)h.text_len);
    return 0;
}
