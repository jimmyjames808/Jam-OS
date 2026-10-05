/* wlapps: libfun's apps as windows on a compositor of our own, headless,
 * and a picture of the result on the screen (tools/shell-tests/wlapps.txt).
 *
 *     run wlapps [size=WxH] [win=WxH] [settle=S] [hold=S] [tile] <app>...
 *
 * It starts `bin/compositor headless size=WxH` (default 1280x800) with an
 * image of ours to compose into (SR_USER + 1), publishes the compositor in
 * its own namespace as /svc/wayland, and starts each app (bin/<app>, no
 * arguments; `demo` gets seconds=60) in a job of their own with only that
 * service in their namespace and $FUN_WINDOW=<win> (default 600x380), so
 * libfun opens each a window of that size. No console either: an app that
 * can't open a window can't fall back to borrowing the screen, it ends.
 * After `settle` seconds (default 5) every app must still run and the
 * image must have the apps' pixels in it: more than a tenth of each
 * window's area changed from the compositor's first picture (the
 * wallpaper, taken before the apps start). Then it borrows the
 * real screen (its own namespace without the compositor again) and shows
 * the image there for `hold` seconds (default 4), for QEMU's screendump,
 * and ends the apps and the compositor.
 *
 * `tile` asks the compositor to tile the windows side by side (its
 * `layout=tiling`: the window manager's tiling mode, where a window that
 * keeps its size sits centred in its tile) instead of the floating
 * default.
 *
 * Prints "wlapps: verdict: PASS", "... FAIL: <why>" or "... SKIP: <why>"
 * (the compositor offers no windows: no xdg_wm_base, before the window
 * manager is built), before it shows the picture; "wlapps: done" at the
 * end. */
#include <fun.h>
#include <jwl_client.h>
#include <os.h>

#define MAX_APPS   4
#define CONNECT_NS (5 * NS_PER_S)
#define FIRST_NS   (10 * NS_PER_S)   /* the compositor's first picture, at most this long */

struct opts {
    int32_t w, h;          /* the headless output */
    char    win[24];       /* FUN_WINDOW=... */
    int32_t ww, wh;        /* ... its numbers */
    unsigned settle, hold; /* seconds */
    bool    tile;
    const char *apps[MAX_APPS];
    unsigned napps;
};

/* "<w>x<h>", each 1 to 8192. */
static bool parse_size(const char *s, int32_t *w, int32_t *h)
{
    int32_t v[2] = { 0, 0 };
    for (int i = 0; i < 2; i++, s++) {
        while (*s >= '0' && *s <= '9' && v[i] <= 8192)
            v[i] = v[i] * 10 + (*s++ - '0');
        if (v[i] < 1 || v[i] > 8192 || *s != (i ? '\0' : 'x'))
            return false;
    }
    *w = v[0];
    *h = v[1];
    return true;
}

static bool parse(int argc, char **argv, struct opts *o)
{
    *o = (struct opts){ .w = 1280, .h = 800, .ww = 600, .wh = 380, .settle = 5, .hold = 4 };
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "size=", 5) && parse_size(a + 5, &o->w, &o->h))
            continue;
        if (!strncmp(a, "win=", 4) && parse_size(a + 4, &o->ww, &o->wh))
            continue;
        if (!strncmp(a, "settle=", 7) || !strncmp(a, "hold=", 5))
            continue;   /* arg_num reads them */
        if (!strcmp(a, "tile"))
            o->tile = true;
        else if (a[0] >= 'a' && a[0] <= 'z' && o->napps < MAX_APPS && !strchr(a, '='))
            o->apps[o->napps++] = a;
        else
            return false;
    }
    o->settle = (unsigned)arg_num(argc, argv, "settle", 5);
    o->hold = (unsigned)arg_num(argc, argv, "hold", 4);
    snprintf(o->win, sizeof(o->win), "FUN_WINDOW=%dx%d", o->ww, o->wh);
    return o->napps > 0;
}

/* ---- the compositor ---------------------------------------------------------------------- */

struct comp {
    handle_t job, proc;    /* its job and process */
    handle_t image;        /* the VMO it composes into */
    uint32_t *px;          /* ... mapped here */
    uint32_t *first;       /* its first picture (the wallpaper), ours */
};

static status_t start_compositor(const struct opts *o, struct comp *k)
{
    uint64_t bytes = ((uint64_t)o->w * (uint64_t)o->h * 4 + 4095) & ~4095ull, addr = 0;
    handle_t mine = HANDLE_INVALID, theirs = HANDLE_INVALID, dup = HANDLE_INVALID;
    status_t st = jam_vmo_create(bytes, 0, HANDLE_INVALID, &k->image);
    if (st == OK)
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), k->image, 0, bytes, VMAR_READ, &addr);
    if (st == OK)
        st = jam_handle_duplicate(k->image, RIGHT_SAME, &dup);
    if (st == OK)
        st = jam_channel_create(&mine, &theirs);
    if (st == OK)
        st = jam_job_create(startup_handle(SR_JOB), 0, &k->job);
    char size[32];
    snprintf(size, sizeof(size), "size=%dx%d", o->w, o->h);
    const char *argv[] = { "bin/compositor", "headless", size, "layout=tiling" };
    struct spawn_handle x[] = { { SR_USER + 0, theirs }, { SR_USER + 1, dup } };
    struct spawn_args a = { .path = "bin/compositor", .argc = o->tile ? 4 : 3, .argv = argv,
                            .job = k->job, .extra = x, .nextra = 2 };
    if (st == OK) {
        st = spawn(&a, &k->proc);   /* consumes x either way */
        theirs = dup = HANDLE_INVALID;
    }
    if (st == OK) {
        st = ns_svc_set(JWL_SERVICE, mine, true);   /* consumes mine either way */
        mine = HANDLE_INVALID;
    }
    handle_t left[] = { mine, theirs, dup };
    for (unsigned i = 0; i < 3; i++)
        if (left[i] != HANDLE_INVALID)
            jam_handle_close(left[i]);
    k->px = (uint32_t *)(uintptr_t)addr;
    return st;
}

/* The compositor's first picture, once all of it is painted (the image
 * starts all 0, which no pixel of the wallpaper is): a copy of ours. */
static status_t first_picture(const struct opts *o, struct comp *k)
{
    uint64_t n = (uint64_t)o->w * (uint64_t)o->h, until = now() + FIRST_NS, i = 0;
    const volatile uint32_t *px = k->px;   /* the compositor writes it */
    while (i < n && now() < until) {
        if (px[i] & 0xffffff)
            i++;
        else
            jam_nanosleep(now() + NS_PER_MS);
    }
    if (i < n)
        return ERR_TIMED_OUT;
    k->first = big_alloc(n * 4);
    if (!k->first)
        return ERR_NO_MEMORY;
    memcpy(k->first, k->px, n * 4);
    return OK;
}

/* Whether the compositor offers windows: a client of our own asks. */
static bool offers_windows(void)
{
    struct jwl_client_config cfg = { .name = "wlapps", .no_reconnect = true };
    struct jwl_client *c;
    if (jwl_client_connect(&cfg, now() + CONNECT_NS, &c) != OK)
        return false;
    bool yes = jwl_client_info(c)->wm_base_version > 0;
    jwl_client_destroy(c);
    return yes;
}

/* ---- the apps ------------------------------------------------------------------------------ */

static status_t start_apps(const struct opts *o, handle_t job, handle_t *procs)
{
    static const char *const ns[] = { "/svc/" JWL_SERVICE, NULL };
    const char *const envp[] = { o->win, NULL };
    for (unsigned i = 0; i < o->napps; i++) {
        char path[48];
        snprintf(path, sizeof(path), "bin/%s", o->apps[i]);
        bool demo = !strcmp(o->apps[i], "demo");
        const char *argv[] = { o->apps[i], "seconds=60" };
        struct spawn_args a = { .path = path, .argc = demo ? 2 : 1, .argv = argv, .job = job,
                                .envp = envp, .ns = ns };
        status_t st = spawn(&a, &procs[i]);
        if (st != OK) {
            printf("wlapps: verdict: FAIL: can't start %s (%s)\n", path, status_str(st));
            return st;
        }
    }
    return OK;
}

/* The apps all still run (none fell over without a window). */
static bool all_running(const struct opts *o, const handle_t *procs)
{
    for (unsigned i = 0; i < o->napps; i++) {
        struct process_info info;
        if (jam_process_get_info(procs[i], &info) != OK || info.state == PROCESS_DEAD) {
            printf("wlapps: verdict: FAIL: %s ended (code %ld)\n", o->apps[i],
                   (long)info.exit_code);
            return false;
        }
    }
    return true;
}

/* The image's pixels changed since the compositor's first picture. */
static uint64_t drawn(const struct opts *o, const struct comp *k)
{
    uint64_t n = 0;
    for (uint64_t i = 0; i < (uint64_t)o->w * (uint64_t)o->h; i++)
        n += (k->px[i] & 0xffffff) != (k->first[i] & 0xffffff);
    return n;
}

/* The image on the real screen, centred, for `hold` seconds. */
static void show(const struct opts *o, const uint32_t *px)
{
    (void)ns_svc_remove(JWL_SERVICE);   /* our own libfun borrows the screen */
    if (gfx_open_on(0) != OK) {
        printf("wlapps: can't borrow the screen to show the picture\n");
        return;
    }
    struct surf img = { (uint32_t *)px, o->w, o->h, o->w };
    int x = (scr.w - o->w) / 2, y = (scr.h - o->h) / 2;
    blit(&scr.s, x, y, &img, &(struct rect){ 0, 0, o->w, o->h });
    gfx_present();
    printf("wlapps: showing the composition (%dx%d) for %u s\n", o->w, o->h, o->hold);
    (void)gfx_key(now() + (uint64_t)o->hold * NS_PER_S);
    gfx_close();
}

/* Wait the settle time, then the verdict: true for PASS. */
static bool check(const struct opts *o, const struct comp *k, const handle_t *procs)
{
    jam_nanosleep(now() + (uint64_t)o->settle * NS_PER_S);
    if (!all_running(o, procs))
        return false;
    uint64_t n = drawn(o, k), want = (uint64_t)o->ww * (uint64_t)o->wh / 10 * o->napps;
    printf("wlapps: %llu pixels drawn by the windows (want more than %llu)\n",
           (unsigned long long)n, (unsigned long long)want);
    if (n <= want) {
        printf("wlapps: verdict: FAIL: the windows aren't in the picture\n");
        return false;
    }
    printf("wlapps: verdict: PASS\n");
    return true;
}

int main(int argc, char **argv)
{
    struct opts o;
    if (!parse(argc, argv, &o)) {
        printf("usage: wlapps [size=WxH] [win=WxH] [settle=S] [hold=S] [tile] <app>...\n");
        return 2;
    }
    pool_start(0);
    struct comp k = { .proc = HANDLE_INVALID };
    status_t st = start_compositor(&o, &k);
    if (st == OK)
        st = first_picture(&o, &k);
    if (st != OK) {
        printf("wlapps: verdict: FAIL: can't start the compositor (%s)\n", status_str(st));
        return 1;
    }
    bool pass = false, skip = !offers_windows(), started = false;
    handle_t apps_job = HANDLE_INVALID, procs[MAX_APPS];
    if (skip)
        printf("wlapps: verdict: SKIP: the compositor offers no windows (no xdg_wm_base)\n");
    else if (jam_job_create(startup_handle(SR_JOB), 0, &apps_job) == OK)
        started = start_apps(&o, apps_job, procs) == OK;
    if (started) {
        pass = check(&o, &k, procs);
        show(&o, k.px);   /* pass or fail: the picture says why */
    }
    if (apps_job != HANDLE_INVALID)
        (void)jam_job_kill(apps_job);   /* the apps: their windows go */
    (void)jam_job_kill(k.job);          /* and the compositor */
    printf("wlapps: done\n");
    return pass || skip ? 0 : 1;
}
