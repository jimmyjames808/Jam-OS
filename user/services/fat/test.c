/* fat: test powers, for the tests of a restart (utest's fat_restart, which
 * starts fat itself over a RAM disk and plays devmgr: the state, the
 * keeper, the restarts). They come as argv words after the name, which
 * only whoever starts fat writes: devmgr never does, and no client can
 * reach them.
 *
 *   die=<point>:<n>   the process ends the n-th time it reaches <point>
 *                     (FAT_DIE_*: held, commit, send, reply, answered),
 *                     as a kill there would end it: what a successor then
 *                     finds is what a kill at that instruction leaves;
 *   crash-on=<name>   an fs.stat of a path whose last name is <name>
 *                     crashes fat (an invalid instruction, which the
 *                     kernel kills it for): the bad request of the plan's
 *                     rule, every time it is run;
 *   fixed-time        every timestamp fat writes (and the volume's serial
 *                     number, which a format makes from the time) is
 *                     2026-01-01 00:00:00, so two runs of one script leave
 *                     disks that are the same byte for byte. */
#include "fat.h"

#define DIE_PREFIX   "die="
#define CRASH_PREFIX "crash-on="
#define FIXED_TIME   "fixed-time"

static const char *const point_names[FAT_DIE_COUNT] = {
    [FAT_DIE_HELD] = "held", [FAT_DIE_COMMIT] = "commit", [FAT_DIE_SEND] = "send",
    [FAT_DIE_REPLY] = "reply", [FAT_DIE_ANSWERED] = "answered",
};

static struct {
    int         point;    /* the FAT_DIE_* to end at; -1: none */
    uint32_t    at;       /* ... the at-th time it is reached */
    uint32_t    reached;  /* times it was reached so far */
    const char *crash;    /* crash-on's name, or NULL */
    bool        fixed;    /* fixed-time */
} t = { .point = -1 };

bool test_fixed_time(void)
{
    return t.fixed;
}

static bool die_word(const char *w)
{
    for (int p = 0; p < FAT_DIE_COUNT; p++) {
        size_t n = strlen(point_names[p]);
        if (strncmp(w, point_names[p], n) || w[n] != ':')
            continue;
        uint32_t at = 0;
        for (const char *d = w + n + 1; *d >= '0' && *d <= '9' && at < 100000; d++)
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
    if (!strncmp(w, DIE_PREFIX, strlen(DIE_PREFIX)))
        return die_word(w + strlen(DIE_PREFIX));
    if (!strncmp(w, CRASH_PREFIX, strlen(CRASH_PREFIX)) && w[strlen(CRASH_PREFIX)]) {
        t.crash = w + strlen(CRASH_PREFIX);
        return true;
    }
    return false;
}

void test_die(enum fat_die point)
{
    if ((int)point != t.point || ++t.reached != t.at)
        return;
    printf("fat %s: test: ending at %s %u\n", vol.name, point_names[point], t.at);
    jam_process_exit(FAT_EXIT_TEST);
}

void test_crash(const char *path)
{
    if (!t.crash)
        return;
    const char *last = strrchr(path, '/');
    if (strcmp(last ? last + 1 : path, t.crash))
        return;
    printf("fat %s: test: crashing on %s\n", vol.name, path);
    __builtin_trap();   /* ud2: the kernel kills the process, a crash */
}
