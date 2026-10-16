/* init's default subtree mounts of /jam (docs/M13.6-PLAN.md, Q3 and U7a):
 * ported software and `#!` lines name /usr, /var and /home, so when the
 * Jam OS drive's ext volume is mounted at /jam (volumes, mounts.c), init
 * gives its namespace those names as views of the volume's directories
 * (fs.subview of /jam/usr and so on): each is a mount whose root is that
 * directory, so `..` stops there and nothing above it can be named. They
 * go into init's namespace like volumes' own mounts, so every program init
 * starts, and every shell it follows, has them. /tmp is tmp.c's: a view of
 * /jam/tmp too while /jam is read-write, and there whatever else is
 * mounted (U7b).
 *
 * The table below is a default: which names, and what lives under /usr, is
 * M13.7's to settle.
 *
 * What is done, on a thread of its own (the calls go to the volume's
 * service, and init's loop must not wait): the directories made if they
 * aren't there (a read-only /jam has those it has), the views made and
 * mounted. The loop is told when they are in (KEY_MOUNTS, so mounts_event
 * says the change is whole and the followers are updated), and the log
 * says what happened.
 *
 * /jam leaving (jam_gone) takes the views out with it. A thread still at
 * work is told by the generation: it notices before it mounts anything and
 * takes back what it did, and a /jam that came back meanwhile starts the
 * work again. */
#include "init.h"

#define STACK        (64u << 10)   /* fs calls carry 4 KiB path buffers */
#define THREAD_END   (2 * NS_PER_S)    /* a finished thread ends within this */

static const struct {
    const char *mount;   /* the name in the namespace */
    const char *dir;     /* the directory of /jam it shows */
} views[] = {
    { "/usr", "usr" },
    { "/var", "var" },
    { "/home", "home" },
};
#define NVIEWS (sizeof(views) / sizeof(views[0]))

static struct {
    handle_t port;           /* where KEY_MOUNTS goes */
    uint64_t key;
    bool     present;        /* /jam is in the namespace (atomic: the thread reads it) */
    uint32_t gen;            /* bumped by every change of presence (atomic) */
    bool     running;        /* the thread is at work (atomic) */
    handle_t thread;
    bool     changed;        /* the thread mounted views: the loop tells the followers (atomic) */
    bool     said;           /* the log line below is for the loop to say (atomic) */
    char     line[160];      /* ... written before `said` is set */
    uint8_t  stack[STACK] __attribute__((aligned(16)));
} jm;

static bool current(uint32_t gen)
{
    return __atomic_load_n(&jm.gen, __ATOMIC_ACQUIRE) == gen;
}

static void wake(void)
{
    struct port_packet pkt = { .key = jm.key, .type = PORT_PACKET_USER };
    for (int i = 0; i < 100 && jam_port_queue(jm.port, &pkt) != OK; i++)
        jam_sleep_until(now() + 10 * NS_PER_MS);   /* a full port: the loop is far behind */
}

/* A line for the loop to say (a thread's printf isn't safe against init's own). */
static void say_later(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say_later(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(jm.line, sizeof(jm.line), fmt, ap);
    va_end(ap);
    __atomic_store_n(&jm.said, true, __ATOMIC_RELEASE);
}

/* name and a space on the end of the list in buf (cut if it doesn't fit). */
static void add_name(char *buf, size_t cap, const char *name)
{
    size_t n = strlen(buf);
    snprintf(buf + n, cap - n, "%s ", name);
}

/* ---- the thread --------------------------------------------------------------------- */

/* One pass for generation gen: the directories and the views. The views
 * are mounted only while gen is the current one, and taken back if it
 * moved meanwhile. */
static void pass(uint32_t gen)
{
    handle_t h[NVIEWS] = { 0 };
    char path[40], names[48] = "";
    unsigned made = 0;
    for (unsigned i = 0; i < NVIEWS && current(gen); i++) {
        snprintf(path, sizeof(path), "/jam/%s", views[i].dir);
        status_t st = fs_mkdir_mode(path, FS_MODE_DIR_DEFAULT);
        if (st != OK && st != ERR_ALREADY_EXISTS && st != ERR_ACCESS_DENIED) {
            say_later("init: /jam/%s can't be made (%s): no %s", views[i].dir, status_str(st),
                      views[i].mount);
            continue;
        }
        st = fs_subview(path, 0, &h[i]);
        if (st != OK) {
            h[i] = HANDLE_INVALID;
            say_later("init: %s can't be a view of %s (%s)", views[i].mount, path, status_str(st));
        }
    }
    for (unsigned i = 0; i < NVIEWS && current(gen); i++)
        if (h[i]) {
            if (ns_mount(views[i].mount, h[i]) == OK) {
                made++;
                add_name(names, sizeof(names), views[i].mount);
            }
            h[i] = HANDLE_INVALID;
        }
    for (unsigned i = 0; i < NVIEWS; i++)
        if (h[i])
            jam_handle_close(h[i]);   /* gen moved on: not mounted */
    if (made && current(gen))
        say_later("init: /jam: mounted as views: %s", names);
    if (!current(gen)) {   /* /jam went (or came again) while we worked: back out */
        for (unsigned i = 0; i < NVIEWS; i++)
            (void)ns_unmount(views[i].mount);
    }
    __atomic_store_n(&jm.changed, true, __ATOMIC_RELEASE);
    wake();
}

static void thread_main(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t gen = __atomic_load_n(&jm.gen, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&jm.present, __ATOMIC_ACQUIRE))
            pass(gen);
        __atomic_store_n(&jm.running, false, __ATOMIC_RELEASE);
        bool again = false;
        if (!current(gen)) {
            bool was = false;
            again = __atomic_compare_exchange_n(&jm.running, &was, true, false,
                                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }
        if (!again)
            return;
    }
}

/* ---- init's side ---------------------------------------------------------------------- */

void jam_mounted(handle_t port, uint64_t key)
{
    jm.port = port;
    jm.key = key;
    __atomic_store_n(&jm.present, true, __ATOMIC_RELEASE);
    __atomic_add_fetch(&jm.gen, 1, __ATOMIC_ACQ_REL);
    bool was = false;
    if (!__atomic_compare_exchange_n(&jm.running, &was, true, false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return;   /* the thread is at work: it sees the new generation and starts again */
    status_t st = OK;
    if (jm.thread) {   /* the last one has finished (running was false): its stack is free once it ends */
        signals_t seen;
        st = jam_object_wait_one(jm.thread, SIG_TERMINATED, now() + THREAD_END, &seen);
        jam_handle_close(jm.thread);
        jm.thread = HANDLE_INVALID;
    }
    if (st == OK)
        st = thread_spawn("jam views", thread_main, NULL, jm.stack, sizeof(jm.stack), &jm.thread);
    if (st != OK) {
        __atomic_store_n(&jm.running, false, __ATOMIC_RELEASE);
        jm.thread = HANDLE_INVALID;
        init_say("init: no thread for /jam's views (%s): no /usr, /var or /home",
                 status_str(st));
    }
}

void jam_gone(void)
{
    __atomic_store_n(&jm.present, false, __ATOMIC_RELEASE);
    __atomic_add_fetch(&jm.gen, 1, __ATOMIC_ACQ_REL);
    for (unsigned i = 0; i < NVIEWS; i++)
        (void)ns_unmount(views[i].mount);
}

bool jam_changed(void)
{
    if (__atomic_exchange_n(&jm.said, false, __ATOMIC_ACQ_REL))
        init_say("%s", jm.line);
    return __atomic_exchange_n(&jm.changed, false, __ATOMIC_ACQ_REL);
}
