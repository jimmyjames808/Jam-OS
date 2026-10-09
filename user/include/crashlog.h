/* The log of a boot that panicked, on the boot after it (kernel/kexec/,
 * docs/history/M8.5-PLAN.md "Revision 2"): what init and logd say about it.
 *
 * The kernel hands init the panicked kernel's log as SR_CRASHLOG, a
 * read-only VMO (struct crashlog_header in <jam/startup.h>, then the
 * text). init starts logd with a duplicate of it and the write end of a
 * channel (LOGD_SR_CRASH_RESULT). logd saves the log as
 * /data/logs/<name>-crash.txt before it opens this boot's own log, then
 * sends one `result` event on the channel and closes it (the protocol
 * `crashres`, abi/idl/logctl.idl); init makes the shell's one-line note
 * from the header and the result. */
#pragma once

#include <os.h>

#define CRASHLOG_PATH_MAX    96   /* a path in a result with its NUL (crashres: 95 bytes) */

/* What logd said (crashres.result), as init reads it. */
struct crashlog_result {
    bool    have;                       /* the event came */
    int32_t status;                     /* OK: saved; else why not */
    char    path[CRASHLOG_PATH_MAX];    /* the file (or the one it tried), NUL-terminated,
                                         * printable ASCII */
};

/* Read the result from ch, init's end of the channel logd got
 * (LOGD_SR_CRASH_RESULT): OK once it has come, into *r (an event that
 * breaks the format is dropped and read past; the status is clamped to an
 * error code, the path made printable); ERR_SHOULD_WAIT: it has not come
 * yet (bind ch and read again); ERR_PEER_CLOSED: logd closed its end
 * without a result. */
status_t crashlog_read_result(handle_t ch, struct crashlog_result *r);

/* Is h, at the start of a VMO of vmo_size bytes, a header to trust? Its
 * fields in range, its name a name, its message printable and terminated.
 * (The kernel checked what it copied; this side checks again, as for any
 * message from another process.) */
static inline bool crashlog_header_ok(const struct crashlog_header *h, uint64_t vmo_size)
{
    if (vmo_size < sizeof(*h) || h->magic != CRASHLOG_MAGIC ||
        h->version != CRASHLOG_VERSION || h->text_len > vmo_size - sizeof(*h) ||
        h->panic_at > h->text_len || strnlen(h->name, sizeof(h->name)) == sizeof(h->name) ||
        strnlen(h->message, sizeof(h->message)) == sizeof(h->message))
        return false;
    for (const char *p = h->name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
            return false;
    for (const char *p = h->message; *p; p++)
        if (*p < 0x20 || *p > 0x7e)
            return false;
    return true;
}
