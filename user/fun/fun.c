/* The fun apps' shared code: see fun.h. */
#include "fun.h"
#include <idl/console.h>

const uint32_t fun_palette[16] = {   /* user/console/main.c rgb[] */
    0x101018, 0xcc4444, 0x44aa44, 0xccaa33, 0x4466cc, 0xaa44aa, 0x44aaaa, 0xb0b0b0,
    0x707070, 0xff6666, 0x66dd66, 0xffdd55, 0x6699ff, 0xdd77dd, 0x66dddd, 0xf0f0f0,
};

/* ---- the terminal ------------------------------------------------------------------ */

static void term_send(struct term *t)
{
    if (t->nout && t->con)
        console_write(t->con, (uint16_t)t->nout, (const uint8_t *)t->out);
    t->bytes_sent += t->nout;
    t->nout = 0;
}

static void term_out(struct term *t, const char *s, uint32_t n)
{
    while (n) {
        uint32_t room = sizeof(t->out) - t->nout, k = n < room ? n : room;
        memcpy(t->out + t->nout, s, k);
        t->nout += k;
        s += k;
        n -= k;
        if (t->nout == sizeof(t->out))
            term_send(t);
    }
}

static void term_outs(struct term *t, const char *s)
{
    term_out(t, s, (uint32_t)strlen(s));
}

status_t term_open(struct term *t)
{
    memset(t, 0, sizeof(*t));
    t->con = startup_handle(SR_CONSOLE);
    if (!t->con)
        return ERR_NOT_FOUND;
    uint16_t c = 0, r = 0;
    status_t st = console_size(t->con, &c, &r);
    if (st != OK)
        return st;
    if (c < 40 || r < 20)
        return ERR_OUT_OF_RANGE;
    t->cols = c;
    t->rows = r;
    t->cell = calloc((size_t)c * r, sizeof(struct tcell));
    t->shown = calloc((size_t)c * r, sizeof(struct tcell));   /* ch 0: send everything once */
    if (!t->cell || !t->shown)
        return ERR_NO_MEMORY;
    for (uint32_t i = 0; i < t->cols * t->rows; i++)
        t->cell[i] = (struct tcell){ ' ', C_WHITE, C_BLACK };
    /* Keys first: the alternate screen belongs to the focused key channel. */
    if ((st = console_open_keys(t->con, &t->keys)) != OK)
        return st;
    term_outs(t, "\033[?1049h");
    term_send(t);   /* on its own: the console mirrors this write to COM1, not the frames */
    term_outs(t, "\033[?25l\033[0m\033[2J");
    term_send(t);
    t->open = true;
    return OK;
}

void term_close(struct term *t)
{
    if (!t->open)
        return;
    term_outs(t, "\033[?2026l\033[0m\033[?25h\033[?1049l");
    term_send(t);
    jam_handle_close(t->keys);
    t->keys = HANDLE_INVALID;
    t->open = false;
}

static void sgr(struct term *t, uint8_t fg, uint8_t bg)
{
    char b[16];
    int n = snprintf(b, sizeof(b), "\033[%u;%um", fg < 8 ? 30u + fg : 90u + fg - 8,
                     bg < 8 ? 40u + bg : 100u + bg - 8);
    term_out(t, b, (uint32_t)n);
}

static void glyph(struct term *t, uint8_t ch)
{
    static const uint8_t low[] = { 0, 0x80, 0x84, 0x88, 0x91, 0x92, 0x93 };
    if (ch >= G_UPPER && ch <= G_DARK) {
        char u[3] = { (char)0xe2, (char)0x96, (char)low[ch] };
        term_out(t, u, 3);
    } else {
        char c = ch >= 0x20 && ch < 0x7f ? (char)ch : ' ';
        term_out(t, &c, 1);
    }
}

void term_flush(struct term *t)
{
    if (!t->open)
        return;
    term_outs(t, "\033[?2026h");
    int fg = -1, bg = -1;
    uint32_t cx = ~0u, cy = ~0u;
    for (uint32_t y = 0; y < t->rows; y++) {
        for (uint32_t x = 0; x < t->cols; x++) {
            struct tcell *c = &t->cell[y * t->cols + x], *s = &t->shown[y * t->cols + x];
            if (c->ch == s->ch && c->fg == s->fg && c->bg == s->bg)
                continue;
            if (cy != y || cx != x) {
                char b[24];
                int n = snprintf(b, sizeof(b), "\033[%u;%uH", y + 1, x + 1);
                term_out(t, b, (uint32_t)n);
            }
            /* A space or a full block shows one colour only. */
            if (c->ch == ' ' ? c->bg != bg : c->ch == G_FULL ? c->fg != fg
                                                             : c->fg != fg || c->bg != bg) {
                sgr(t, c->fg, c->bg);
                fg = c->fg;
                bg = c->bg;
            }
            glyph(t, c->ch);
            *s = *c;
            cx = x + 1;
            cy = y;
        }
    }
    term_outs(t, "\033[?2026l");
    term_send(t);
}

void term_put(struct term *t, int x, int y, uint8_t ch, uint8_t fg, uint8_t bg)
{
    if (x < 0 || y < 0 || (uint32_t)x >= t->cols || (uint32_t)y >= t->rows)
        return;
    t->cell[(uint32_t)y * t->cols + (uint32_t)x] = (struct tcell){ ch, fg, bg };
}

void term_fill(struct term *t, int x, int y, int w, int h, uint8_t ch, uint8_t fg, uint8_t bg)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            term_put(t, i, j, ch, fg, bg);
}

int term_text(struct term *t, int x, int y, const char *s, uint8_t fg, uint8_t bg)
{
    for (; *s; s++, x++)
        term_put(t, x, y, (uint8_t)*s, fg, bg);
    return x;
}

int term_textf(struct term *t, int x, int y, uint8_t fg, uint8_t bg, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return term_text(t, x, y, buf, fg, bg);
}

/* ---- keys ---------------------------------------------------------------------------- */

int key_decode(const struct input_key_event *ev)
{
    if (ev->state == INPUT_KEY_UP)
        return KEY_NONE;
    uint32_t cp = ev->codepoint;
    if (cp == 0x1b || ev->usage == 0x29 || cp == 3)
        return KEY_QUIT;
    if ((ev->mods & INPUT_MOD_CTRL) && (cp == 'c' || ev->usage == 0x06))
        return KEY_QUIT;
    switch (ev->usage) {
    case 0x52: return KEY_UP;
    case 0x51: return KEY_DOWN;
    case 0x50: return KEY_LEFT;
    case 0x4f: return KEY_RIGHT;
    case 0x28: case 0x58: return KEY_ENTER;
    case 0x4b: return KEY_PGUP;
    case 0x4e: return KEY_PGDN;
    case 0x4a: return KEY_HOME;
    case 0x57: return '+';   /* keypad */
    case 0x56: return '-';
    }
    if (cp == '\n' || cp == '\r')
        return KEY_ENTER;
    if (cp >= 0x20 && cp < 0x7f)
        return (int)cp;
    return KEY_NONE;
}

int term_key(struct term *t, uint64_t deadline)
{
    for (;;) {
        struct input_key_event ev;
        uint32_t n = 0;
        struct channel_read_args a = {
            .h = t->keys, .bytes_cap = sizeof(ev), .bytes = (uint64_t)(uintptr_t)&ev,
            .actual_bytes = (uint64_t)(uintptr_t)&n,
        };
        status_t st = jam_channel_read(&a);
        if (st == OK) {
            int k = n == sizeof(ev) ? key_decode(&ev) : KEY_NONE;
            if (k != KEY_NONE)
                return k;
            continue;
        }
        if (st != ERR_SHOULD_WAIT)
            return KEY_QUIT;   /* the console went away */
        signals_t seen;
        st = jam_object_wait_one(t->keys, SIG_READABLE | SIG_PEER_CLOSED, deadline, &seen);
        if (st == ERR_TIMED_OUT)
            return KEY_NONE;
        if (st != OK)
            return KEY_QUIT;
    }
}

/* ---- CPUs and the thread pool ---------------------------------------------------------- */

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

uint32_t fun_cpu_count(void)
{
    uint32_t r[4], n = 0;
    cpuid(0, 0, r);
    uint32_t max = r[0];
    /* V2 extended topology (0x1F) or extended topology (0xB): the last level
     * before "invalid" counts every logical CPU in the package. */
    for (uint32_t leaf = 0x1f; leaf >= 0xb && !n; leaf = leaf == 0x1f ? 0xb : 0) {
        if (max < leaf)
            continue;
        for (uint32_t sub = 0; sub < 8; sub++) {
            cpuid(leaf, sub, r);
            if (((r[2] >> 8) & 0xff) == 0)
                break;
            if (r[1] & 0xffff)
                n = r[1] & 0xffff;
        }
    }
    if (!n) {
        cpuid(1, 0, r);
        n = (r[1] >> 16) & 0xff;
    }
    if (n < 1)
        n = 1;
    if (n > FUN_MAX_THREADS)
        n = FUN_MAX_THREADS;
    return n;
}

#define POOL_STACK (64u << 10)
#define POOL_SPIN  20000   /* pauses (~1 ms) before a worker sleeps */

static struct {
    uint32_t n;
    handle_t ev[FUN_MAX_THREADS];
    volatile uint32_t sleeping[FUN_MAX_THREADS];
    volatile uint32_t phase, next, done, items;
    void (*fn)(uint32_t, uint32_t, void *);
    void *arg;
} pool = { .n = 1 };
uint32_t pool_items_by[FUN_MAX_THREADS];

static void pool_work(uint32_t me)
{
    uint32_t i, did = 0;
    while ((i = __atomic_fetch_add(&pool.next, 1, __ATOMIC_RELAXED)) < pool.items) {
        pool.fn(i, me, pool.arg);
        did++;
    }
    pool_items_by[me] = did;
    __atomic_fetch_add(&pool.done, 1, __ATOMIC_RELEASE);
}

static void pool_worker(void *a)
{
    uint32_t me = (uint32_t)(uintptr_t)a, seen = 0;
    for (;;) {
        uint32_t ph, spins = 0;
        while ((ph = __atomic_load_n(&pool.phase, __ATOMIC_ACQUIRE)) == seen) {
            if (++spins < POOL_SPIN) {
                __builtin_ia32_pause();
                continue;
            }
            /* Sleep: announce it, clear the event, look once more (pool_run
             * bumps the phase, then signals every worker that announced). */
            __atomic_store_n(&pool.sleeping[me], 1, __ATOMIC_SEQ_CST);
            jam_event_signal(pool.ev[me], SIG_SIGNALED, 0);
            if (__atomic_load_n(&pool.phase, __ATOMIC_SEQ_CST) == seen)
                jam_object_wait_one(pool.ev[me], SIG_SIGNALED, DEADLINE_NEVER, NULL);
            __atomic_store_n(&pool.sleeping[me], 0, __ATOMIC_SEQ_CST);
            spins = 0;
        }
        seen = ph;
        pool_work(me);
    }
}

uint32_t pool_start(uint32_t n)
{
    if (!n)
        n = fun_cpu_count();
    if (n > FUN_MAX_THREADS)
        n = FUN_MAX_THREADS;
    pool.n = 1;
    for (uint32_t i = 1; i < n; i++) {
        void *stack = malloc(POOL_STACK);
        handle_t th;
        if (!stack || jam_event_create(&pool.ev[i]) != OK)
            break;
        if (thread_spawn("worker", pool_worker, (void *)(uintptr_t)i, stack, POOL_STACK, &th) != OK)
            break;
        jam_handle_close(th);
        pool.n = i + 1;
    }
    return pool.n;
}

uint32_t pool_threads(void) { return pool.n; }

void pool_run(void (*fn)(uint32_t, uint32_t, void *), void *arg, uint32_t items)
{
    pool.fn = fn;
    pool.arg = arg;
    pool.items = items;
    pool.next = 0;
    pool.done = 0;
    __atomic_add_fetch(&pool.phase, 1, __ATOMIC_SEQ_CST);
    for (uint32_t i = 1; i < pool.n; i++)
        if (__atomic_load_n(&pool.sleeping[i], __ATOMIC_SEQ_CST))
            jam_event_signal(pool.ev[i], 0, SIG_SIGNALED);
    pool_work(0);
    while (__atomic_load_n(&pool.done, __ATOMIC_ACQUIRE) < pool.n)
        __builtin_ia32_pause();
}

/* ---- odds and ends ---------------------------------------------------------------------- */

void *big_alloc(uint64_t bytes)
{
    bytes = (bytes + 4095) & ~4095ull;
    handle_t v;
    if (jam_vmo_create(bytes, 0, HANDLE_INVALID, &v) != OK)
        return NULL;
    uint64_t addr = 0;
    status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), v, 0, bytes, VMAR_READ | VMAR_WRITE,
                               &addr);
    jam_handle_close(v);   /* the mapping keeps it */
    return st == OK ? (void *)(uintptr_t)addr : NULL;
}

void say(const char *fmt, ...)
{
    char buf[2048];   /* console_write sends a whole 2048-byte buffer */
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = sizeof(buf) - 1;
    handle_t con = startup_handle(SR_CONSOLE);
    if (!con || console_write(con, (uint16_t)n, (const uint8_t *)buf) != OK)
        printf("%s", buf);
}

char *commas(char *buf, size_t n, uint64_t v)
{
    char tmp[32];
    int len = snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)v), o = 0;
    for (int i = 0; i < len && (size_t)o + 1 < n; i++) {
        if (i && (len - i) % 3 == 0 && (size_t)o + 2 < n)
            buf[o++] = ',';
        buf[o++] = tmp[i];
    }
    buf[o] = '\0';
    return buf;
}

bool has_arg(int argc, char **argv, const char *name)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], name))
            return true;
    return false;
}

uint64_t arg_num(int argc, char **argv, const char *name, uint64_t def)
{
    size_t k = strlen(name);
    for (int i = 1; i < argc; i++)
        if (!strncmp(argv[i], name, k) && argv[i][k] == '=') {
            uint64_t v = 0;
            for (const char *p = argv[i] + k + 1; *p >= '0' && *p <= '9'; p++)
                v = v * 10 + (uint64_t)(*p - '0');
            return v;
        }
    return def;
}
