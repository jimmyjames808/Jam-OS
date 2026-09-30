/* logd: saves each boot's kernel log as /data/logs/boot-NNNN.txt.
 *
 * init starts it once /data is mounted, with
 *   SR_RESOURCE  the root resource with RIGHT_READ: klog_open (nothing else
 *                of the root's is used);
 *   SR_NS        a namespace holding /data.
 * It opens a kernel log reader and reads from byte 0, so the file starts
 * with what was logged before /data (or logd) existed, as far back as the
 * kernel's ring still holds it; then it follows the log, appending each
 * piece as it arrives. Bytes the ring dropped before logd got to them are
 * marked in the file with a "[logd: N bytes ... lost]" line.
 *
 * Sync: written bytes are synced at most once a second: at once when the
 * last sync is more than a second ago, else when that second is over.
 *
 * Without /data (not mounted, gone, its filesystem restarting, full) logd
 * keeps running and tries again after RETRY_FIRST, doubling up to
 * RETRY_MAX. While it waits it does not read the log: the ring keeps the
 * last 64 KiB, and what falls out of it is lost and marked. It holds one
 * piece of the log (CHUNK bytes) and nothing more, however long /data is
 * away. It says what happened once per change, not once per try: its own
 * lines go into the log it is saving.
 *
 * logd.h has the two startup handles that replace the namespace and the
 * kernel log for tests. */
#include <os.h>
#include "logd.h"

#define CHUNK       4096u            /* bytes of log taken at a time */
#define NOTE_MAX    80u              /* room before a piece for the "lost" line */
#define SYNC_EVERY  NS_PER_S
#define RETRY_FIRST NS_PER_S
#define RETRY_MAX   (8 * NS_PER_S)

/* Where the log comes from. */
struct source {
    handle_t h;       /* a klog reader, or (LOGD_SR_LOG) a channel of text */
    bool     klog;    /* which */
    uint64_t pos;     /* klog: the next byte to read */
    bool     ended;   /* a channel whose other end closed, all of it read */
};

static struct source src;
static char     piece[NOTE_MAX + CHUNK];   /* the piece being written, after its note */
static uint32_t piece_at, piece_len;       /* where it starts in piece[], its length (0: none) */

/* Put the "lost" line right before the piece. */
static void note_lost(uint64_t lost)
{
    char note[NOTE_MAX];
    int n = snprintf(note, sizeof(note), "[logd: %lu bytes of the log were lost]\n",
                     (unsigned long)lost);
    memcpy(piece + NOTE_MAX - n, note, (size_t)n);
    piece_at = NOTE_MAX - (uint32_t)n;
    piece_len += (uint32_t)n;
}

/* A message of the text channel that is bigger than a piece: taken off the
 * queue and counted as lost. */
static void drop_message(uint32_t n, uint32_t nh)
{
    uint8_t *big = malloc(n ? n : 1);
    handle_t *hs = malloc((nh ? nh : 1) * sizeof(handle_t));
    uint32_t n2 = 0, nh2 = 0;
    struct channel_read_args a = {
        .h = src.h, .bytes_cap = n, .bytes = (uint64_t)(uintptr_t)big,
        .actual_bytes = (uint64_t)(uintptr_t)&n2, .handles = (uint64_t)(uintptr_t)hs,
        .handles_cap = nh, .actual_handles = (uint64_t)(uintptr_t)&nh2,
    };
    if (big && hs && jam_channel_read(&a) == OK)
        for (uint32_t i = 0; i < nh2; i++)
            jam_handle_close(hs[i]);
    free(big);
    free(hs);
    note_lost(n);
}

/* The next piece of the log into piece[], if there is one. */
static void take(void)
{
    piece_at = NOTE_MAX;
    piece_len = 0;
    if (src.klog) {
        uint64_t first = 0;
        int64_t n = jam_klog_read(src.h, src.pos, piece + NOTE_MAX, CHUNK, &first);
        if (n <= 0)
            return;
        piece_len = (uint32_t)n;
        if (first > src.pos)
            note_lost(first - src.pos);
        src.pos = first + (uint64_t)n;
        return;
    }
    uint32_t n = 0, nh = 0;
    struct channel_read_args a = {
        .h = src.h, .bytes_cap = CHUNK, .bytes = (uint64_t)(uintptr_t)(piece + NOTE_MAX),
        .actual_bytes = (uint64_t)(uintptr_t)&n, .actual_handles = (uint64_t)(uintptr_t)&nh,
    };
    status_t st = jam_channel_read(&a);
    if (st == OK)
        piece_len = n;
    else if (st == ERR_BUFFER_TOO_SMALL)
        drop_message(n, nh);
    else if (st != ERR_SHOULD_WAIT)
        src.ended = true;
}

/* Wait until there is more log (only when `for_log`: without /data it
 * stays where it is), the text channel's other end has closed, or the
 * deadline passes. */
static void wait_for(bool for_log, uint64_t deadline)
{
    signals_t seen = 0;
    if (src.klog && !for_log) {
        jam_nanosleep(deadline);
        return;
    }
    signals_t mask = (for_log ? SIG_READABLE : 0) | (src.klog ? 0 : SIG_PEER_CLOSED);
    status_t st = jam_object_wait_one(src.h, mask, deadline, &seen);
    /* Closed while /data is away: what is still queued can't be saved. */
    if (!for_log && st == OK)
        src.ended = true;
}

static bool open_source(void)
{
    src.h = startup_handle(LOGD_SR_LOG);
    if (src.h)
        return true;
    src.klog = true;
    status_t st = jam_klog_open(startup_handle(SR_RESOURCE), &src.h);
    if (st != OK)
        printf("logd: can't read the kernel log (%s): SR_RESOURCE must be the root with "
               "RIGHT_READ\n", status_str(st));
    return st == OK;
}

/* Where /data stands. */
static const struct store *store;      /* how it is reached */
static bool     up;                    /* the file is open */
static bool     dirty;                 /* written since the last sync */
static uint64_t last_sync;             /* when that was */
static uint64_t retry_at;              /* !up: when to try /data again */
static uint64_t backoff = RETRY_FIRST; /* !up: how long after the next failure */
static status_t said;                  /* the failure last reported (OK: none) */

static status_t try_open(void)
{
    status_t st = logfile_open(store);
    if (st != OK)
        return st;
    printf("logd: writing %s\n", logfile_path());
    up = true;
    said = OK;
    backoff = RETRY_FIRST;
    return OK;
}

/* Write the piece in hand (taking the next one if there is none), and
 * sync if a second has passed since the last sync. */
static status_t save(uint64_t t)
{
    if (!piece_len)
        take();
    if (piece_len) {
        status_t st = logfile_write(piece + piece_at, piece_len);
        if (st != OK)
            return st;   /* the piece stays in hand for the next try */
        piece_len = 0;
        dirty = true;
    }
    if (!dirty || t - last_sync < SYNC_EVERY)
        return OK;
    last_sync = t;
    dirty = false;
    return logfile_sync();
}

/* No /data (any more): close the file, say so once, try again later. */
static void lost_data(status_t st)
{
    logfile_close();
    up = dirty = false;
    if (st != said)
        printf("logd: no %s/logs (%s): the log is not being saved; trying again\n", store->root,
               status_str(st));
    said = st;
    retry_at = now() + backoff;
    backoff = backoff * 2 > RETRY_MAX ? RETRY_MAX : backoff * 2;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    handle_t fs = startup_handle(LOGD_SR_FS);
    store = fs ? store_fs(fs) : &store_ns;
    if (!open_source())
        return 1;
    while (!src.ended) {
        status_t st = OK;
        uint64_t t = now();
        if (!up && t >= retry_at)
            st = try_open();
        if (up)
            st = save(t);
        if (st != OK)
            lost_data(st);
        if (!up)
            wait_for(false, retry_at);
        else if (!piece_len)
            wait_for(true, dirty ? last_sync + SYNC_EVERY : DEADLINE_NEVER);
    }
    if (up) {
        (void)logfile_sync();   /* the end of a test's log: nothing to do about a failure */
        logfile_close();
    }
    return 0;
}
