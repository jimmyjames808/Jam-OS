/* fetch: the handles, where the body goes, and the lines (fetch.h says
 * what the program is). A file is written through a 64 KiB buffer, so
 * the stick sees large writes; the pipe gets messages of FETCH_MSG bytes,
 * each waited for while the shell's channel is full. A progress line every
 * SAY_EVERY while it runs, and one at the end with the size, the time and
 * the speed (MB/s: 10^6 bytes a second). */
#include <wants.h>
#include "fetch.h"

/* What it is given (<wants.h>): the network and the resolver. Never the
 * listen permission: fetch only connects. */
JAM_WANTS("svc net\n"
          "svc dns\n");

#define ROLE_FILE (SR_USER + 0)
#define ROLE_BUF  (SR_USER + 1)
#define ROLE_BODY (SR_USER + 3)
#define OUT_BUF   (64u * 1024)
#define PIPE_WAIT (30 * NS_PER_S)   /* the shell takes its pipe's bytes long before this */

static struct {
    struct jfile file;      /* the file (to_file) */
    bool         to_file;
    handle_t     body;      /* or the body's channel */
    uint8_t     *buf;       /* bytes not yet written to the file */
    size_t       held;
    uint64_t     written;   /* bytes of the body so far */
    int64_t      length;    /* the body's length, -1 unknown */
    uint64_t     t0, said;  /* the body's start, the last progress line */
} out;

/* n bytes in ns as MB/s, in tenths (a byte a microsecond is 1 MB/s). */
static uint64_t rate10(uint64_t n, uint64_t ns)
{
    uint64_t us = ns / 1000;
    return n * 10 / (us ? us : 1);
}

/* "12.3 MiB" (or "512 bytes") into buf. */
static const char *size_text(uint64_t n, char *buf, size_t cap)
{
    if (n < 10 * 1024)
        snprintf(buf, cap, "%lu bytes", (unsigned long)n);
    else if (n < 1ull << 20)
        snprintf(buf, cap, "%lu.%lu KiB", (unsigned long)(n >> 10),
                 (unsigned long)((n & 1023) * 10 >> 10));
    else
        snprintf(buf, cap, "%lu.%lu MiB", (unsigned long)(n >> 20),
                 (unsigned long)(((n >> 10) & 1023) * 10 >> 10));
    return buf;
}

static void progress(bool last)
{
    uint64_t t = now();
    if (!last && t - out.said < SAY_EVERY)
        return;
    out.said = t;
    char a[24], b[24];
    uint64_t r = rate10(out.written, t - out.t0);
    if (last)
        printf("fetch: %lu bytes (%s) in %lu.%02lu s, %lu.%lu MB/s\n",
               (unsigned long)out.written, size_text(out.written, a, sizeof(a)),
               (unsigned long)((t - out.t0) / NS_PER_S),
               (unsigned long)((t - out.t0) % NS_PER_S / (10 * NS_PER_MS)), (unsigned long)(r / 10),
               (unsigned long)(r % 10));
    else if (out.length > 0)
        printf("fetch: %s of %s (%lu%%), %lu.%lu MB/s\n", size_text(out.written, a, sizeof(a)),
               size_text((uint64_t)out.length, b, sizeof(b)),
               (unsigned long)(out.written * 100 / (uint64_t)out.length), (unsigned long)(r / 10),
               (unsigned long)(r % 10));
    else
        printf("fetch: %s so far, %lu.%lu MB/s\n", size_text(out.written, a, sizeof(a)),
               (unsigned long)(r / 10), (unsigned long)(r % 10));
}

static status_t flush_file(void)
{
    size_t done = 0;
    status_t st = out.held ? file_write(&out.file, out.written - out.held, out.buf, out.held, &done)
                           : OK;
    if (st == OK && done != out.held)
        st = ERR_IO;
    if (st != OK)
        printf("fetch: writing the file: %s\n", st == ERR_NO_SPACE ? "the stick is full"
                                                                   : status_str(st));
    out.held = 0;
    return st;
}

/* One message of the body down the pipe's channel, waiting for room. */
static status_t to_pipe(const uint8_t *data, size_t n)
{
    uint64_t deadline = now() + PIPE_WAIT;
    for (;;) {   /* each turn writes, or waits for room (to the deadline) */
        status_t st = jam_channel_write(out.body, data, (uint32_t)n, NULL, 0);
        if (st != ERR_SHOULD_WAIT && st != ERR_NO_RESOURCES)
            return st;
        if (conn_stopped())
            return ERR_CANCELED;
        signals_t seen;
        st = jam_object_wait_one(out.body, SIG_WRITABLE | SIG_PEER_CLOSED,
                                 now() + 50 * NS_PER_MS < deadline ? now() + 50 * NS_PER_MS
                                                                    : deadline, &seen);
        if (st == ERR_TIMED_OUT && now() >= deadline)
            return st;
    }
}

status_t fetch_out(const uint8_t *data, size_t n)
{
    status_t st = OK;
    while (n && st == OK) {   /* each turn takes a buffer's or a message's worth */
        size_t k;
        if (out.to_file) {
            k = OUT_BUF - out.held < n ? OUT_BUF - out.held : n;
            memcpy(out.buf + out.held, data, k);
            out.held += k;
            out.written += k;
            if (out.held == OUT_BUF)
                st = flush_file();
        } else {
            k = n < FETCH_MSG ? n : FETCH_MSG;
            st = to_pipe(data, k);
            out.written += k;
        }
        data += k;
        n -= k;
    }
    progress(false);
    return st;
}

void fetch_started(int64_t length, unsigned status)
{
    char a[24];
    out.length = length;
    out.t0 = out.said = now();
    if (length >= 0)
        printf("fetch: %u, %s\n", status, size_text((uint64_t)length, a, sizeof(a)));
    else
        printf("fetch: %u, length not given\n", status);
}

/* The body's way out from the handles the shell gave: false (said) if none. */
static bool open_out(const char *mode)
{
    out.length = -1;
    if (!strcmp(mode, "pipe")) {
        out.body = startup_handle(ROLE_BODY);
        return out.body != HANDLE_INVALID;
    }
    out.buf = malloc(OUT_BUF);
    status_t st = out.buf ? file_adopt(startup_handle(ROLE_FILE), startup_handle(ROLE_BUF),
                                       FS_WRITE, &out.file)
                          : ERR_NO_MEMORY;
    if (st != OK)
        printf("fetch: the file to write isn't usable (%s)\n", status_str(st));
    out.to_file = st == OK;
    return st == OK;
}

int main(int argc, char **argv)
{
    struct http_url url, final;
    struct conn c;
    if (argc != 3 || (strcmp(argv[2], "file") && strcmp(argv[2], "pipe")) ||
        http_url_parse(argv[1], &url) != OK) {
        printf("fetch: started wrongly (the shell's `fetch` starts it)\n");
        return 1;
    }
    if (!open_out(argv[2]))
        return 1;
    status_t st = conn_init(&c);
    if (st != OK)
        printf("fetch: no wait set (%s)\n", status_str(st));
    if (st == OK)
        st = get_url(&c, &url, &final);   /* it says what went wrong */
    if (st == OK && out.to_file && (st = flush_file()) == OK && (st = file_sync(&out.file)) != OK)
        printf("fetch: writing the file: %s\n", status_str(st));
    if (st == OK)
        progress(true);
    if (st == ERR_CANCELED)
        printf("fetch: stopped after %lu bytes\n", (unsigned long)out.written);
    if (out.to_file)
        file_close(&out.file);
    netwait_destroy(c.w);
    return st == OK ? 0 : st == ERR_CANCELED ? EXIT_STOPPED : 1;
}
