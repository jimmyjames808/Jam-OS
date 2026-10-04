/* storm: copy a file while services are killed again and again, then say
 * whether the copy is whole (docs/M11.6-PLAN.md, "The demonstration and
 * its tests"):
 *
 *   storm <from> <to> <kills/s> [service...]
 *   storm mixer <kills/s> <seconds>
 *
 * The copy runs on the shell's own thread, a piece at a time as `cp` does
 * (any size). A thread of storm's own, which serves nothing, kills the
 * services at a fixed rate through init's control channel (initctl.kill:
 * the path `kill` takes), taking turns; by default the filesystem service
 * of each side ("fat-usb0" for /usb0, "fat-data" for /data). A kill that
 * takes longer than the period is followed by the next at once, never by
 * a burst to catch up. Rate 0: no kills at all (the baseline line).
 *
 * Kill to first answer is measured from storm's side, as a client feels
 * it. The clock is read before initctl.kill, which returns once the
 * process is dead (devmgr waits for a filesystem service) or, for the
 * mixer, once the kernel has told its threads to leave (a thread acts on
 * that before it next runs user code, so it never takes another request);
 * then one cheap call goes to the killed service (fs.statfs on its mount,
 * audioctl.streams for the mixer) and the clock is read when it is
 * answered. One sample covers the kill path, the spare's promotion and
 * whatever the successor finishes first (the copy's request in progress
 * among it). Any other service is killed but not measured.
 *
 * After the copy, with no more kills, both files are read again from
 * their mounts and hashed (SHA-256, as sha256sum prints it): a read that
 * went wrong under the kills shows as DIFFERENT even if its wrong bytes
 * were copied faithfully. The result line also goes to the kernel log
 * (so to /data/logs), for the record.
 *
 * `storm mixer` kills the mixer for that many seconds while whatever is
 * playing plays, then reads the mixer's own lines from the kernel log
 * since it started: its restarts, the least written-ahead audio left at
 * one ("lead left"), and every period it was late for.
 *
 * Ctrl+C stops it: the kill thread finishes the kill in hand, and storm
 * says what it did so far (130). */
#include <fs_idl.h>
#include <idl/audioctl.h>
#include <idl/initctl.h>
#include "sh.h"

#define MAX_TARGETS  4                    /* services killed in turn */
#define NAME_MAX     32                   /* initctl.kill's name field */
#define MAX_RATE     1000                 /* kills a second */
#define MAX_SECONDS  3600                 /* storm mixer's length */
#define MAX_SAMPLES  65536                /* kill-to-first-answer samples kept */
#define KILL_WAIT    (20 * NS_PER_S)      /* initctl.kill (devmgr waits up to 15 s), the call after */
#define STOP_POLL    (50 * NS_PER_MS)     /* the kill thread looks for a stop this often */
#define THREAD_STACK (64u << 10)
#define THREAD_WAIT  (2 * KILL_WAIT + 5 * NS_PER_S)   /* its last kill and call, then it ends */
#define MIXER_HZ     48000                /* the mixer's output rate: its "lead left" frames */

enum probe { PROBE_NONE, PROBE_FS, PROBE_MIXER };

struct target {
    char       name[NAME_MAX];   /* initctl.kill's name: "fat-data", "mixer" */
    enum probe probe;            /* the call that measures its first answer */
    handle_t   ch;               /* that call's channel (an fs mount's is ours to close) */
};

struct storm {
    struct target t[MAX_TARGETS];
    unsigned      ntargets;
    handle_t      ctl;           /* init's control channel (the shell's) */
    uint64_t      period;        /* ns between kills; 0: no kills */
    bool          stop;          /* main -> kill thread: finish (atomic) */
    bool          orphaned;      /* main: the kill thread didn't end, so all of this stays */
    /* Written by the kill thread only; read by main once it has ended. */
    uint32_t      kills;         /* kills init said were done */
    uint32_t      refused;       /* kills init refused, or not done in time */
    status_t      refused_st;    /* the first refusal's status */
    unsigned      refused_t;     /* ... and its target */
    uint32_t      unanswered;    /* kills whose first call failed */
    status_t      unanswered_st; /* the first such call's status */
    uint32_t      unmeasured;    /* kills with no call to measure them */
    uint32_t     *us;            /* kill to first answer, microseconds */
    uint32_t      nus;           /* samples in us[] (at most MAX_SAMPLES) */
};

/* ---- the kill thread ----------------------------------------------------------------- */

static bool stopping(struct storm *s)
{
    return __atomic_load_n(&s->stop, __ATOMIC_ACQUIRE);   /* kills_stop's release store */
}

/* The call that says the killed service answers again. */
static status_t probe(const struct target *t, uint64_t deadline)
{
    if (t->probe == PROBE_FS) {
        uint64_t total = 0, free_bytes = 0;
        uint8_t ro = 0, label[16];
        return fs_statfs_until(t->ch, deadline, &total, &free_bytes, &ro, label);
    }
    uint32_t count = 0;
    int32_t master = 0;
    uint8_t list[640];
    return audioctl_streams_until(t->ch, deadline, &count, &master, list);
}

static void kill_one(struct storm *s, unsigned i)
{
    const struct target *t = &s->t[i];
    uint8_t name[NAME_MAX] = { 0 };
    memcpy(name, t->name, strlen(t->name));
    uint64_t koid = 0, t0 = now();
    status_t st = initctl_kill_until(s->ctl, t0 + KILL_WAIT, name, &koid);
    if (st != OK) {
        if (!s->refused++) {
            s->refused_st = st;
            s->refused_t = i;
        }
        return;
    }
    s->kills++;
    if (t->probe == PROBE_NONE) {
        s->unmeasured++;
        return;
    }
    st = probe(t, now() + KILL_WAIT);
    uint64_t us = (now() - t0) / NS_PER_US;
    if (st != OK) {
        if (!s->unanswered++)
            s->unanswered_st = st;
    } else if (s->nus < MAX_SAMPLES) {
        s->us[s->nus++] = us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
    }
}

static void kill_main(void *arg)
{
    struct storm *s = arg;
    uint64_t next = now() + s->period;
    for (unsigned k = 0;; k = (k + 1) % s->ntargets) {
        while (!stopping(s) && now() < next) {
            uint64_t t = now() + STOP_POLL;
            (void)jam_nanosleep(t < next ? t : next);   /* woken early: the loop looks again */
        }
        if (stopping(s))
            break;
        kill_one(s, k);
        uint64_t t = now();
        next += s->period;
        if (next < t)
            next = t;   /* behind: the next kill now, no burst to catch up */
    }
}

static status_t kills_start(struct storm *s, void **stack, handle_t *thread)
{
    *stack = malloc(THREAD_STACK);
    status_t st = *stack ? thread_spawn("storm kills", kill_main, s, *stack, THREAD_STACK, thread)
                         : ERR_NO_MEMORY;
    if (st != OK) {
        free(*stack);
        *stack = NULL;
    }
    return st;
}

/* Ask the kill thread to stop, wait for it and free its stack. If it
 * doesn't end (a call stuck past every deadline) it keeps its stack and s
 * (orphaned): freeing what a running thread uses would be worse than the
 * leak. */
static void kills_stop(struct storm *s, handle_t thread, void *stack)
{
    __atomic_store_n(&s->stop, true, __ATOMIC_RELEASE);   /* stopping() acquires it */
    if (!thread)
        return;
    signals_t seen;
    status_t st = jam_object_wait_one(thread, SIG_TERMINATED, now() + THREAD_WAIT, &seen);
    jam_handle_close(thread);
    if (st == OK) {
        free(stack);
        return;
    }
    s->orphaned = true;
    sh_tty("storm: the kill thread didn't end (%s): its memory is left as it is\n",
           status_str(st));
}

/* ---- the targets --------------------------------------------------------------------- */

/* "/data/x" -> "data"; false for a path at the root. */
static bool mount_name(const char *abs, char *out, size_t cap)
{
    const char *p = abs + 1, *e = strchr(p, '/');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (!n || n >= cap)
        return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

/* "data", "esp", "usb0".."usb9": a mount whose filesystem service devmgr
 * runs and init's kill names by its mount ("fat-data"). */
static bool fat_mount(const char *m)
{
    return !strcmp(m, "data") || !strcmp(m, "esp") ||
           (!strncmp(m, "usb", 3) && m[3] >= '0' && m[3] <= '9' && !m[4]);
}

/* Add the service `name`, with the call that measures it when storm has
 * one. false (said): too many, or a name storm won't kill. */
static bool add_target(struct storm *s, const char *name)
{
    for (unsigned i = 0; i < s->ntargets; i++)
        if (!strcmp(s->t[i].name, name))
            return true;
    if (s->ntargets == MAX_TARGETS || strlen(name) >= NAME_MAX) {
        sh_tty("storm: at most %u services, each a name of up to %u characters\n", MAX_TARGETS,
               NAME_MAX - 1);
        return false;
    }
    if (!strcmp(name, "shell") || !strcmp(name, "init")) {
        sh_tty("storm: not killing %s (%s)\n", name,
               !strcmp(name, "shell") ? "storm runs in it" : "nobody restarts it");
        return false;
    }
    struct target *t = &s->t[s->ntargets++];
    *t = (struct target){ .probe = PROBE_NONE };
    memcpy(t->name, name, strlen(name) + 1);
    char path[NS_NAME_MAX];
    if (!strcmp(name, "mixer") && sh_audio_ctl()) {
        t->probe = PROBE_MIXER;
        t->ch = sh_audio_ctl();   /* the shell's own channel: not ours to close */
    } else if (!strncmp(name, "fat-", 4) && strlen(name + 4) + 2 <= sizeof(path)) {
        snprintf(path, sizeof(path), "/%s", name + 4);
        if (ns_channel(path, &t->ch) == OK)
            t->probe = PROBE_FS;
    }
    return true;
}

/* The filesystem service of the mount holding abs, if it has one. */
static bool add_side(struct storm *s, const char *abs)
{
    char m[NS_NAME_MAX], name[NAME_MAX];
    if (!mount_name(abs, m, sizeof(m)) || !fat_mount(m))
        return true;
    snprintf(name, sizeof(name), "fat-%s", m);
    return add_target(s, name);
}

static void targets_close(struct storm *s)
{
    for (unsigned i = 0; i < s->ntargets; i++)
        if (s->t[i].probe == PROBE_FS)
            jam_handle_close(s->t[i].ch);
    s->ntargets = 0;
}

/* ---- the numbers --------------------------------------------------------------------- */

static void sift(uint32_t *v, size_t at, size_t n)
{
    for (size_t c; (c = 2 * at + 1) < n; at = c) {
        if (c + 1 < n && v[c + 1] > v[c])
            c++;
        if (v[at] >= v[c])
            return;
        uint32_t x = v[at];
        v[at] = v[c];
        v[c] = x;
    }
}

/* Heap sort, ascending: no recursion, no extra memory. */
static void sort_u32(uint32_t *v, size_t n)
{
    for (size_t i = n / 2; i-- > 0;)
        sift(v, i, n);
    for (size_t end = n; end > 1; end--) {
        uint32_t x = v[0];
        v[0] = v[end - 1];
        v[end - 1] = x;
        sift(v, 0, end - 1);
    }
}

/* "; 31 kills (9.6/s); kill to first answer median 5512 us, p99 9876 us,
 * worst 10234 us" into buf, for ns of storming (nearest-rank percentiles). */
static void kills_text(struct storm *s, uint64_t ns, char *buf, size_t cap)
{
    if (!s->period) {
        snprintf(buf, cap, "; no kills");
        return;
    }
    uint64_t tenths = ns ? (uint64_t)s->kills * 10 * NS_PER_S / ns : 0;
    size_t n = (size_t)snprintf(buf, cap, "; %u kills (%lu.%lu/s)", s->kills,
                                (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
    if (n >= cap)
        return;
    if (!s->nus) {
        snprintf(buf + n, cap - n, "; no kill to first answer measured");
        return;
    }
    sort_u32(s->us, s->nus);
    uint32_t median = s->us[(s->nus + 1) / 2 - 1];
    uint32_t p99 = s->us[(s->nus * 99 + 99) / 100 - 1];
    snprintf(buf + n, cap - n, "; kill to first answer median %u us, p99 %u us, worst %u us",
             median, p99, s->us[s->nus - 1]);
}

static unsigned long rate_of(const struct storm *s)
{
    return (unsigned long)(s->period ? NS_PER_S / s->period : 0);
}

/* A line on the screen (or the pipe) and in the kernel log. */
static void say_logged(const char *line)
{
    sh_say("%s\n", line);
    printf("%s\n", line);
}

/* The kills that didn't go as planned, if any. */
static void say_trouble(const struct storm *s)
{
    if (s->refused)
        sh_say("storm: %u kill(s) refused or not done in time (the first: %s, %s)\n", s->refused,
               s->t[s->refused_t].name, status_str(s->refused_st));
    if (s->unanswered)
        sh_say("storm: %u kill(s) not answered after (the first: %s)\n", s->unanswered,
               status_str(s->unanswered_st));
    if (s->unmeasured)
        sh_say("storm: %u kill(s) not measured (storm has no call to ask those services)\n",
               s->unmeasured);
    if (s->kills > s->nus + s->unanswered + s->unmeasured)
        sh_say("storm: kill to first answer from the first %u kills only\n", MAX_SAMPLES);
}

/* ---- the copy ------------------------------------------------------------------------ */

struct copy {
    const char *from, *to;       /* absolute paths */
    uint64_t    bytes;           /* copied */
    uint64_t    ns;              /* from the first open to the last close */
    status_t    st;              /* the copy's outcome */
    const char *failed;          /* what failed: a path, or "the copy" */
};

static void copy_run(struct copy *c)
{
    struct jfile in, out;
    uint64_t t0 = now();
    c->failed = c->from;
    c->st = file_open(c->from, FS_READ, &in);
    if (c->st != OK)
        return;
    c->failed = c->to;
    c->st = file_open(c->to, FS_WRITE | FS_CREATE | FS_TRUNCATE, &out);
    if (c->st == OK) {
        c->failed = "the copy";   /* a read or a write: sh_copy doesn't say which */
        c->st = sh_copy(&in, &out, &c->bytes);
        file_close(&out);   /* fat syncs the file here: part of the copy's time */
    }
    file_close(&in);
    c->ns = now() - t0;
}

/* Both files read back and hashed, said as sha256sum says them; *same if
 * they match. ERR_CANCELED: Ctrl+C. */
static status_t hash_both(const struct copy *c, bool *same)
{
    uint8_t d[2][SHA256_BYTES];
    const char *path[2] = { c->from, c->to };
    for (unsigned i = 0; i < 2; i++) {
        status_t st = sh_sha256_file(path[i], d[i]);
        if (st != OK) {
            if (st == ERR_CANCELED)
                sh_tty("storm: stopped by Ctrl+C while %s was read back (the copy had ended)\n",
                       path[i]);
            else
                sh_tty("storm: %s: can't read it back (%s)\n", path[i], sh_why(st));
            return st;
        }
        char hex[2 * SHA256_BYTES + 1];
        sha256_hex(d[i], hex);
        sh_say("%s  %s\n", hex, path[i]);
    }
    *same = !memcmp(d[0], d[1], SHA256_BYTES);
    return OK;
}

/* "4194304 bytes in 3.21 s, 1.31 MB/s" (a byte a microsecond is 1 MB/s). */
static void rate_text(const struct copy *c, char *buf, size_t cap)
{
    uint64_t us = c->ns / NS_PER_US ? c->ns / NS_PER_US : 1;
    uint64_t mb100 = c->bytes * 100 / us;
    uint64_t cs = c->ns / (10 * NS_PER_MS);
    snprintf(buf, cap, "%lu bytes in %lu.%02lu s, %lu.%02lu MB/s", (unsigned long)c->bytes,
             (unsigned long)(cs / 100), (unsigned long)(cs % 100), (unsigned long)(mb100 / 100),
             (unsigned long)(mb100 % 100));
}

/* The two paths, absolute; false (said) if they can't be a copy. */
static bool copy_paths(char **argv, char *from, char *to)
{
    bool dir = false;
    uint64_t size = 0;
    if (!sh_resolve(argv[1], from, SH_PATH_MAX) || !sh_dest(from, argv[2], to, SH_PATH_MAX)) {
        sh_tty("storm: the path is too long\n");
        return false;
    }
    status_t st = sh_stat(from, &dir, &size);
    if (st != OK || dir) {
        sh_tty("storm: %s: %s\n", argv[1], st != OK ? sh_why(st) : "a directory (only files)");
        return false;
    }
    if (!strcmp(from, to)) {
        sh_tty("storm: %s and %s are the same file\n", argv[1], argv[2]);
        return false;
    }
    return true;
}

/* The services to kill: those named, else each side's filesystem service. */
static bool copy_targets(struct storm *s, int argc, char **argv, const char *from, const char *to)
{
    for (int i = 4; i < argc; i++)
        if (!add_target(s, argv[i]))
            return false;
    if (argc == 4 && (!add_side(s, from) || !add_side(s, to)))
        return false;
    if (s->period && !s->ntargets) {
        sh_tty("storm: neither side has a filesystem service to kill: name the services\n");
        return false;
    }
    return true;
}

/* The copy has ended: its line, after its hashes if it got to the end. */
static int copy_said(struct storm *s, const struct copy *c)
{
    char line[320], rate[80], kills[160];
    rate_text(c, rate, sizeof(rate));
    kills_text(s, c->ns, kills, sizeof(kills));
    say_trouble(s);
    if (c->st != OK) {
        snprintf(line, sizeof(line), "storm: %lu kills/s: %s: %s, after %s%s", rate_of(s),
                 c->failed, c->st == ERR_CANCELED ? "stopped by Ctrl+C" : sh_why(c->st), rate,
                 kills);
        say_logged(line);
        return c->st == ERR_CANCELED ? 130 : 1;
    }
    bool same = false;
    status_t st = hash_both(c, &same);
    if (st != OK)
        return st == ERR_CANCELED ? 130 : 1;
    snprintf(line, sizeof(line), "storm: %lu kills/s: %s%s; %s", rate_of(s), rate, kills,
             same ? "MATCH" : "DIFFERENT");
    say_logged(line);
    return same ? 0 : 1;
}

static int storm_copy(struct storm *s, int argc, char **argv)
{
    char from[SH_PATH_MAX], to[SH_PATH_MAX];
    if (!copy_paths(argv, from, to) || !copy_targets(s, argc, argv, from, to))
        return 1;
    void *stack = NULL;
    handle_t thread = 0;
    sh_flush();
    status_t st = s->period ? kills_start(s, &stack, &thread) : OK;
    if (st != OK) {
        sh_tty("storm: can't start the kill thread (%s)\n", status_str(st));
        return 1;
    }
    struct copy c = { .from = from, .to = to };
    copy_run(&c);
    kills_stop(s, thread, stack);
    return copy_said(s, &c);
}

/* ---- the mixer ----------------------------------------------------------------------- */

/* Where key starts in the n bytes at p, or NULL (a kernel log line has no NUL). */
static const char *find(const char *p, size_t n, const char *key)
{
    size_t k = strlen(key);
    for (size_t i = 0; i + k <= n; i++)
        if (!memcmp(p + i, key, k))
            return p + i;
    return NULL;
}

/* The number right after `key` in the n bytes at p, or false. */
static bool number_after(const char *p, size_t n, const char *key, uint64_t *out)
{
    const char *at = find(p, n, key), *end = p + n;
    if (!at)
        return false;
    at += strlen(key);
    if (at == end || *at < '0' || *at > '9')
        return false;
    for (*out = 0; at < end && *at >= '0' && *at <= '9'; at++)
        *out = *out * 10 + (uint64_t)(*at - '0');
    return true;
}

/* The mixer's own lines since `from` in the kernel log (mixer/adopt.c's
 * restart line, output.c's late lines): its restarts after a kill, the
 * least written-ahead audio left at one, the periods it was late for. */
static void mixer_lines(uint64_t from, char *buf, size_t cap)
{
    char *log = NULL;
    size_t got = 0;
    uint64_t first = 0;
    if (sh_klog_read(from, &log, &got, &first) != OK) {
        snprintf(buf, cap, "; the kernel log can't be read");
        return;
    }
    unsigned restarts = 0, running = 0, late = 0;
    uint64_t least = UINT64_MAX, v;
    struct sh_lines l = { log, log + got };
    const char *p;
    size_t n;
    while (sh_next_line(&l, &p, &n)) {
        if (find(p, n, "mixer: restart (killed")) {
            restarts++;
            if (number_after(p, n, "lead left ", &v)) {
                running++;
                least = v < least ? v : least;
            }
        }
        if (find(p, n, "mixer: late:") || find(p, n, "frames late: the driver"))
            late++;
    }
    free(log);
    if (!running)
        snprintf(buf, cap, "; the mixer's lines: %u restart(s), none with the output running; "
                 "%u late period(s)", restarts, late);
    else
        snprintf(buf, cap, "; the mixer's lines: %u restart(s), %u with the output running, "
                 "least lead left %lu frames (%lu ms); %u late period(s)", restarts, running,
                 (unsigned long)least, (unsigned long)(least * 1000 / MIXER_HZ), late);
}

static int storm_mixer(struct storm *s, uint64_t seconds)
{
    if (!sh_audio_ctl()) {
        sh_tty("storm: there is no mixer\n");
        return 1;
    }
    if (!add_target(s, "mixer"))
        return 1;
    uint64_t from = sh_klog_end(), t0 = now();
    void *stack = NULL;
    handle_t thread = 0;
    sh_flush();
    status_t st = s->period ? kills_start(s, &stack, &thread) : OK;
    if (st != OK) {
        sh_tty("storm: can't start the kill thread (%s)\n", status_str(st));
        return 1;
    }
    bool whole = sh_sleep(seconds * NS_PER_S);
    kills_stop(s, thread, stack);
    uint64_t ns = now() - t0;
    char line[400], kills[160], mixer[200];
    kills_text(s, ns, kills, sizeof(kills));
    mixer_lines(from, mixer, sizeof(mixer));
    say_trouble(s);
    snprintf(line, sizeof(line), "storm: mixer: %lu kills/s for %lu s%s%s%s", rate_of(s),
             (unsigned long)(ns / NS_PER_S), whole ? "" : " (stopped by Ctrl+C)", kills, mixer);
    say_logged(line);
    return whole ? 0 : 130;
}

/* ---- the command --------------------------------------------------------------------- */

static bool parse_rate(const char *arg, struct storm *s)
{
    uint64_t rate;
    if (!sh_parse_u64(arg, &rate) || rate > MAX_RATE) {
        sh_tty("storm: %s: kills a second, 0 to %u\n", arg, MAX_RATE);
        return false;
    }
    s->period = rate ? NS_PER_S / rate : 0;
    if (s->period && !s->ctl) {
        sh_tty("storm: no control channel from init: nothing can be killed\n");
        return false;
    }
    return true;
}

/* 0, 1 or 130 as the storm went; 2 for a usage error. */
static int storm_run(struct storm *s, int argc, char **argv)
{
    if (strcmp(argv[1], "mixer"))
        return parse_rate(argv[3], s) ? storm_copy(s, argc, argv) : 2;
    uint64_t seconds = 0;
    if (!parse_rate(argv[2], s))
        return 2;
    if (!sh_parse_u64(argv[3], &seconds) || !seconds || seconds > MAX_SECONDS) {
        sh_tty("storm: %s: seconds, 1 to %u\n", argv[3], MAX_SECONDS);
        return 2;
    }
    return storm_mixer(s, seconds);
}

SH_CMD(storm)
{
    bool mixer = argc >= 2 && !strcmp(argv[1], "mixer");
    if ((mixer && argc != 4) || argc < 4 || argc > 4 + MAX_TARGETS) {
        sh_tty("usage: storm <from> <to> <kills/s> [service...] | storm mixer <kills/s> "
               "<seconds>\n");
        return 2;
    }
    struct storm *s = calloc(1, sizeof(*s));
    uint32_t *us = malloc(MAX_SAMPLES * sizeof(uint32_t));
    if (!s || !us) {
        free(s);
        free(us);
        sh_tty("storm: out of memory\n");
        return 1;
    }
    s->us = us;
    s->ctl = sh_initctl();
    int ret = storm_run(s, argc, argv);
    if (s->orphaned)
        return ret;   /* the kill thread may still use s, its targets and us */
    targets_close(s);
    free(s->us);
    free(s);
    return ret;
}
