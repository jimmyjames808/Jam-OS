/* heaptest: a program with a malloc of its own, as Chromium's is
 * (PartitionAlloc), checking that the call layer never calls it
 * (docs/CR-PLAN.md, LH; user/posix/sys/heap.c). Its malloc takes its lock
 * and then calls into the layer, as PartitionAlloc maps its pages with
 * its lock held. A call of malloc from inside malloc on one thread can
 * only be the layer's: it is counted and served from a spare arena (a
 * real allocator would wait for its own lock for ever there), and the
 * run fails.
 *
 *   heaptest own      three threads in malloc and free, each malloc that
 *                     needs pages calling mmap and every eighth opening,
 *                     reading and closing /proc/self/status, its lock
 *                     held; munmap and mmap of a page every 32nd
 *   heaptest handler  a SIGUSR1 handler that opens and reads
 *                     /proc/self/status, run while the main thread is
 *                     inside malloc with its lock held, busy in its own
 *                     code (another thread's pthread_kill raises it)
 *
 * Ends "heaptest: <mode>: PASS" (exit 0), or with a FAILED line for each
 * check that didn't hold (exit 1). posixtest's mem section runs both. */
#include <stdio.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PG        4096ul
#define HEAD      16ul               /* a block's header: 16-byte aligned blocks */
#define CLASSES   16u                /* 32 bytes to 1 MiB, headers included */
#define CHUNK     (256ul << 10)      /* mapped for the classes when they run out */
#define BIG       (1ul << 20)        /* past this, a mapping of its own */
#define MAGIC     0x68656170u
#define SPARE     (4ul << 20)        /* the spare arena for calls from inside malloc */
#define THREADS   3
#define ROUNDS    3000
#define WAIT_TSC  (30ull << 30)      /* about 10 s at 3 GHz: the handler's signal never came */

static int fails;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            fails++;                                                     \
            printf("heaptest: FAILED line %d: %s\n", __LINE__, #c);      \
        }                                                                \
    } while (0)

/* ---- the allocator -------------------------------------------------------------------- */

struct head {
    uint32_t cls;     /* a class, or CLASSES: a mapping of its own */
    uint32_t magic;
    uint64_t len;     /* a mapping of its own: its length */
};

static volatile int lock_word;
static __thread int inside;            /* this thread is in malloc or free */
static void *free_list[CLASSES];
static char *chunk_next, *chunk_end;
static unsigned long mallocs;
static volatile unsigned long reentered, layer_calls;
static char spare[SPARE] __attribute__((aligned(16)));
static unsigned long spare_used;

/* The handler mode's: the main thread waits inside malloc for the signal. */
static volatile int armed, in_malloc, handler_done, handler_ok;
static pthread_t main_thread;

static void lock(void)
{
    while (__atomic_exchange_n(&lock_word, 1, __ATOMIC_ACQUIRE))
        sched_yield();   /* not held: another thread has it */
}

static void unlock(void)
{
    __atomic_store_n(&lock_word, 0, __ATOMIC_RELEASE);
}

/* A call from inside malloc or free on this thread: the layer's. */
static void *from_inside(size_t n)
{
    __atomic_add_fetch(&reentered, 1, __ATOMIC_RELAXED);
    unsigned long at = __atomic_fetch_add(&spare_used, (n + 15) & ~15ul, __ATOMIC_RELAXED);
    return at + n <= SPARE ? spare + at : NULL;
}

static int in_spare(const void *p)
{
    return (const char *)p >= spare && (const char *)p < spare + SPARE;
}

static unsigned class_of(size_t total)
{
    unsigned c = 0;
    while (((size_t)32 << c) < total)
        c++;
    return c;
}

/* The layer, called with our lock held, as PartitionAlloc's mmap is. */
static void poke_layer(void)
{
    char buf[128];
    int fd = open("/proc/self/status", O_RDONLY);
    if (fd >= 0) {
        (void)read(fd, buf, sizeof(buf));
        close(fd);
    }
    layer_calls++;
}

static void wait_for_handler(void)
{
    in_malloc = 1;
    for (uint64_t t0 = __builtin_ia32_rdtsc(); !handler_done;) {
        if (__builtin_ia32_rdtsc() - t0 > WAIT_TSC)
            break;   /* the main thread checks handler_done */
        __builtin_ia32_pause();
    }
}

static void *alloc(size_t n)
{
    if (inside)
        return from_inside(n);
    inside = 1;
    void *r = NULL;
    size_t total = n + HEAD;
    if (total > BIG) {
        size_t len = (total + PG - 1) & ~(PG - 1);
        struct head *h = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (h != MAP_FAILED) {
            *h = (struct head){ CLASSES, MAGIC, len };
            r = h + 1;
        }
        inside = 0;
        return r;
    }
    lock();
    unsigned c = class_of(total);
    size_t size = (size_t)32 << c;
    unsigned long k = ++mallocs;
    struct head *h = free_list[c];
    if (h) {
        free_list[c] = *(void **)h;
    } else {
        if ((size_t)(chunk_end - chunk_next) < size) {
            char *m = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            layer_calls++;
            if (m != MAP_FAILED) {
                chunk_next = m;
                chunk_end = m + CHUNK;
            }
        }
        if ((size_t)(chunk_end - chunk_next) >= size) {
            h = (struct head *)chunk_next;
            chunk_next += size;
        }
    }
    if (h) {
        *h = (struct head){ c, MAGIC, 0 };
        r = h + 1;
    }
    if (k % 8 == 0)
        poke_layer();
    if (k % 32 == 0) {
        void *m = mmap(NULL, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m != MAP_FAILED)
            munmap(m, PG);
        layer_calls += 2;
    }
    if (armed && pthread_equal(pthread_self(), main_thread)) {
        armed = 0;
        wait_for_handler();
    }
    unlock();
    inside = 0;
    return r;
}

void free(void *p)
{
    if (!p || in_spare(p))
        return;
    struct head *h = (struct head *)p - 1;
    if (h->magic != MAGIC) {
        static const char line[] = "heaptest: FAILED: a free of a block not ours\n";
        (void)write(2, line, sizeof(line) - 1);
        _exit(1);
    }
    if (h->cls == CLASSES) {
        munmap(h, h->len);
        return;
    }
    int was = inside;
    inside = 1;
    lock();
    *(void **)h = free_list[h->cls];
    free_list[h->cls] = h;
    unlock();
    inside = was;
}

/* (calloc and realloc call alloc, not malloc: GCC makes a malloc and a
 * memset of zero into a call of calloc, which would be calloc's own.) */
void *malloc(size_t n)
{
    return alloc(n);
}

void *calloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size)
        return NULL;
    void *p = alloc(n * size);
    if (p && !in_spare(p))
        memset(p, 0, n * size);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (!p)
        return alloc(n);
    size_t have;
    if (in_spare(p)) {
        have = 0;   /* its size isn't kept: nothing of it is copied */
    } else {
        struct head *h = (struct head *)p - 1;
        have = (h->cls == CLASSES ? h->len : (size_t)32 << h->cls) - HEAD;
        if (n <= have)
            return p;
    }
    void *r = alloc(n);
    if (r)
        memcpy(r, p, have < n ? have : n);
    free(p);
    return r;
}

/* ---- the modes ------------------------------------------------------------------------ */

static void *churn(void *arg)
{
    unsigned seed = (unsigned)(uintptr_t)arg;
    void *keep[16] = { 0 };
    for (unsigned i = 0; i < ROUNDS; i++) {
        seed = seed * 1103515245u + 12345u;
        unsigned slot = (seed >> 8) % 16;
        free(keep[slot]);
        keep[slot] = malloc(16 + (seed >> 12) % 6000);
        if (keep[slot])
            memset(keep[slot], 1, 16);
    }
    for (unsigned i = 0; i < 16; i++)
        free(keep[i]);
    return NULL;
}

static void own(void)
{
    pthread_t t[THREADS];
    unsigned made = 0;
    for (unsigned i = 0; i < THREADS; i++)
        made += pthread_create(&t[made], NULL, churn, (void *)(uintptr_t)(i + 1)) == 0;
    CHECK(made == THREADS);
    for (unsigned i = 0; i < made; i++)
        pthread_join(t[i], NULL);
    printf("heaptest: own: %lu mallocs made %lu calls into the layer with malloc's lock held\n",
           mallocs, (unsigned long)layer_calls);
}

static void on_usr1(int sig)
{
    (void)sig;
    char buf[256];
    int fd = open("/proc/self/status", O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf)) : -1;
    if (fd >= 0)
        close(fd);
    handler_ok = n > 5 && !memcmp(buf, "Name:", 5);
    handler_done = 1;
}

static void *kicker(void *arg)
{
    (void)arg;
    while (!in_malloc)
        sched_yield();
    pthread_kill(main_thread, SIGUSR1);
    return NULL;
}

static void handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    main_thread = pthread_self();
    pthread_t t;
    int made = pthread_create(&t, NULL, kicker, NULL) == 0;
    CHECK(made);
    armed = 1;
    void *p = malloc(100);   /* waits inside, its lock held, for the handler */
    CHECK(p);
    CHECK(handler_done);
    CHECK(handler_ok);
    free(p);
    if (made)
        pthread_join(t, NULL);
    printf("heaptest: handler: SIGUSR1's handler opened and read /proc/self/status while "
           "this thread was inside malloc\n");
}

int main(int argc, char **argv)
{
    const char *mode = argc == 2 ? argv[1] : "";
    if (!strcmp(mode, "own")) {
        own();
    } else if (!strcmp(mode, "handler")) {
        handler();
    } else {
        fprintf(stderr, "usage: heaptest own | handler\n");
        return 2;
    }
    CHECK(reentered == 0);
    if (reentered)
        printf("heaptest: %s: the layer called malloc %lu times from inside malloc\n", mode,
               (unsigned long)reentered);
    printf("heaptest: %s: %s\n", mode, fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
