/* extfs: test powers, for the tests of a restart (utest's extfs_restart,
 * which starts extfs itself over a RAM disk and plays the spawner: the
 * state, the keeper, the restarts). argv words after the name, which only
 * whoever starts extfs writes; no client can reach them.
 *
 *   die=<point>:<n>   the process ends the n-th time it reaches <point>,
 *                     as a kill there would end it:
 *                       write     a device write has landed (file data, a
 *                                 run of the log, a commit block, a block
 *                                 written home): every step of a commit;
 *                       journal   a request's transaction is committed in
 *                                 the journal, its slot not yet;
 *                       commit    its slot committed, not answered;
 *                       answered  answered, the reply waiting for the next
 *                                 system call;
 *                       map       a file.map made its VMO (and recorded its
 *                                 slot), not committed;
 *                       supply    a page request read from the disk, its
 *                                 pages not supplied;
 *   crash-on=<name>   a request on a path whose last name is <name>
 *                     crashes extfs (an invalid instruction): the bad
 *                     request of the restart rule, every time it runs;
 *   fixed-time        every time extfs writes is 2026-01-01 00:00:00;
 *   tail-lazy         the journal's tail moves every 64 commits only (not
 *                     every few blocks: EXT_JNL_TAIL_BLOCKS), the way the
 *                     steps of utest's extfs_restart_steps were counted:
 *                     a start then replays a long log, and its writes count;
 *   quick-retry       a block call that timed out is tried again after
 *                     1 ms, doubling, not disk.c's RETRY_PAUSE (the tries
 *                     and the budget as they are): utest's extfs_timeout
 *                     cases don't wait out the pauses. */
#include "extfs.h"

#define DIE_PREFIX   "die="
#define CRASH_PREFIX "crash-on="
#define FIXED_TIME   "fixed-time"
#define LAZY_TAIL    "tail-lazy"
#define QUICK_RETRY  "quick-retry"

static const char *const point_names[XF_DIE_COUNT] = {
    [XF_DIE_WRITE] = "write", [XF_DIE_JOURNAL] = "journal", [XF_DIE_COMMIT] = "commit",
    [XF_DIE_ANSWERED] = "answered", [XF_DIE_MAP] = "map", [XF_DIE_SUPPLY] = "supply",
};

static struct {
    int         point;
    uint32_t    at;
    uint32_t    reached;
    const char *crash;
    bool        fixed;
    bool        lazy_tail;
    bool        quick_retry;
} t = { .point = -1 };

bool test_fixed_time(void)
{
    return t.fixed;
}

bool test_lazy_tail(void)
{
    return t.lazy_tail;
}

bool test_quick_retry(void)
{
    return t.quick_retry;
}

static bool die_word(const char *w)
{
    for (int p = 0; p < XF_DIE_COUNT; p++) {
        size_t n = strlen(point_names[p]);
        if (strncmp(w, point_names[p], n) || w[n] != ':')
            continue;
        uint32_t at = 0;
        for (const char *d = w + n + 1; *d >= '0' && *d <= '9' && at < 1000000; d++)
            at = at * 10 + (uint32_t)(*d - '0');
        if (!at)
            return false;
        t.point = p;
        t.at = at;
        return true;
    }
    return false;
}

bool test_word(const char *w)
{
    if (!strcmp(w, FIXED_TIME)) {
        t.fixed = true;
        return true;
    }
    if (!strcmp(w, LAZY_TAIL)) {
        t.lazy_tail = true;
        return true;
    }
    if (!strcmp(w, QUICK_RETRY)) {
        t.quick_retry = true;
        return true;
    }
    if (!strncmp(w, DIE_PREFIX, strlen(DIE_PREFIX)))
        return die_word(w + strlen(DIE_PREFIX));
    if (!strncmp(w, CRASH_PREFIX, strlen(CRASH_PREFIX)) && w[strlen(CRASH_PREFIX)]) {
        t.crash = w + strlen(CRASH_PREFIX);
        return true;
    }
    return false;
}

void test_die(enum xf_die point)
{
    if ((int)point != t.point || ++t.reached != t.at)
        return;
    printf("extfs %s: test: ending at %s %u\n", vol.name, point_names[point], t.at);
    jam_process_exit(EXTFS_EXIT_TEST);
}

void test_crash(const char *path, uint32_t len)
{
    if (!t.crash)
        return;
    uint32_t start = len;
    while (start && path[start - 1] != '/')
        start--;
    size_t n = strlen(t.crash);
    if (len - start != n || memcmp(path + start, t.crash, n))
        return;
    printf("extfs %s: test: crashing on %s\n", vol.name, t.crash);
    __builtin_trap();
}
