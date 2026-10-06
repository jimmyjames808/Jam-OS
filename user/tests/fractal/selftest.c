/* fractal: the self-test (`run fractal --selftest`): the maths, the kernels
 * against each other, the parallel render against one thread, and libfun's
 * pool resting its workers. */
#include "fractal.h"

/* An n x n grid of points around the centre in two modes: how many agree
 * (both in, or both out within 0.05 of an iteration); the worst difference
 * among the rest; how many different values mode a gave. */
static int compare_modes(int ma, int mb, int n, int maxit, double *worst, int *distinct_a)
{
    struct kview a = kv, b = kv;
    a.mode = ma;
    b.mode = mb;
    a.maxit = b.maxit = maxit;
    a.avx2 = b.avx2 = false;
    if (ma == M_PERTURB || mb == M_PERTURB)
        ref_build(&ref, &a);
    static float seen[64 * 64];
    int same = 0, distinct = 0;
    *worst = 0;
    for (int j = 0; j < n; j++) {
        double ox[64], oy[64];
        float ra[64], rb[64];
        uint64_t it = 0;
        for (int i = 0; i < n; i++) {
            ox[i] = (i - n / 2) * 7.3;   /* spread over a few hundred pixels */
            oy[i] = (j - n / 2) * 7.3;
        }
        eval_points(&a, &ref, n, ox, oy, ra, &it);
        eval_points(&b, &ref, n, ox, oy, rb, &it);
        for (int i = 0; i < n; i++) {
            /* the same, or escaping at the same step within 0.05 of a step */
            double d = ra[i] > rb[i] ? ra[i] - rb[i] : rb[i] - ra[i];
            if ((ra[i] < 0 && rb[i] < 0) || (ra[i] >= 0 && rb[i] >= 0 && d < 0.05))
                same++;
            else if (ra[i] >= 0 && rb[i] >= 0 && d > *worst)
                *worst = d;
            bool dup = false;
            for (int k = 0; k < distinct && !dup; k++)
                dup = seen[k] == ra[i];
            if (!dup)
                seen[distinct++] = ra[i];
        }
    }
    *distinct_a = distinct;
    return same;
}

/* Known points. */
static void test_points(void)
{
    uint64_t it = 0;
    fun_check(it_double(0, 0, false, 0, 0, 1000, &it) < 0 &&
                  it_double(-1, 0, false, 0, 0, 1000, &it) < 0 &&
                  it_double(-2, 0, false, 0, 0, 1000, &it) < 0 &&
                  it_double(0.25, 0, false, 0, 0, 1000, &it) < 0,
              "0, -1, -2, 1/4 are in the set");
    float e1 = it_double(1, 0, false, 0, 0, 1000, &it);
    float e2 = it_double(0.26, 0, false, 0, 0, 1000, &it);
    fun_check(e1 >= 0 && e1 < 6 && e2 > 10 && e2 < 100, "1 escapes at once, 0.26 after a while");
    fun_check(it_double(-0.75, 0.1, false, 0, 0, 5000, &it) > 20,
              "-0.75 + 0.1i (near the neck) escapes slowly");
    fun_check(it_double(0, 0, true, -1, 0, 2000, &it) < 0 &&
                  it_double(1.5, 1.5, true, -1, 0, 2000, &it) >= 0,
              "Julia c = -1 (the basilica): 0 stays, 1.5 + 1.5i escapes");
}

/* The 4-lane kernel gives exactly the plain one's answers. */
static void test_avx2(bool avx2)
{
    if (avx2) {
        bool same = true;
        uint64_t s = 99, i1 = 0, i2 = 0;
        for (int k = 0; k < 4000; k += 4) {
            double pr[4], pi[4];
            float o4[4];
            bool jul = k >= 2000;
            for (int l = 0; l < 4; l++) {
                pr[l] = (double)(rng_next(&s) % 100000) / 100000 * 3 - 2.2;
                pi[l] = (double)(rng_next(&s) % 100000) / 100000 * 2.4 - 1.2;
            }
            it_double4(pr, pi, jul, -0.8, 0.156, 3000, o4, &i1);
            for (int l = 0; l < 4; l++)
                same &= it_double(pr[l], pi[l], jul, -0.8, 0.156, 3000, &i2) == o4[l];
        }
        fun_check(same && i1 == i2, "AVX2 kernel == plain kernel (4000 points, bit for bit)");
    } else {
        say("fractal: selftest: (no AVX2 here: the 4-lane kernel isn't tested)\n");
    }
}

/* Double-double arithmetic. */
static void test_dd(void)
{
    char what[128], s1[64];
    dd third = dd_div(dd_d(1), dd_d(3)), one = dd_add_d(dd_mul_d(third, 3), -1);
    dd sq = dd_sqr(dd_add_d(dd_d(1), 0x1p-40));   /* (1 + 2^-40)^2 = 1 + 2^-39 + 2^-80 */
    dd rest = dd_add_d(dd_add_d(sq, -1), -0x1p-39);
    dd tenth = dd_div(dd_d(1), dd_d(10)), back = dd_add_d(dd_mul_d(tenth, 10), -1);
    snprintf(what, sizeof(what),
             "double-double: 3/3 - 1 = %d e-33, 10/10 - 1 = %d e-33, 2^-80 kept",
             (int)(one.hi * 1e33), (int)(back.hi * 1e33));
    fun_check(one.hi < 1e-31 && one.hi > -1e-31 && back.hi < 1e-31 && back.hi > -1e-31 &&
                  rest.hi == 0x1p-80,
              what);
    dd_str(s1, sizeof(s1), tour_x, 30);
    snprintf(what, sizeof(what), "double-double prints %s", s1);
    fun_check(!strncmp(s1, "-0.7756837680090537974694835039", 31), what);
}

/* The three kernels against each other, where both are valid. */
static void test_kernels(void)
{
    char what[128];
    double worst;
    int distinct, dist_d;
    PH = 1440;
    view.julia = false;
    view.cx = dd_d(-0.743643887037151);
    view.cy = dd_d(0.131825904205330);
    view.zoom = 1e6;
    view.maxit = 0;
    apply_view();
    int same = compare_modes(M_DOUBLE, M_DD, 40, 4000, &worst, &distinct);
    snprintf(what, sizeof(what), "zoom 1e6: double == double-double at %d of 1600 points", same);
    fun_check(same >= 1600 * 97 / 100, what);
    same = compare_modes(M_DOUBLE, M_PERTURB, 40, 4000, &worst, &distinct);
    snprintf(what, sizeof(what), "zoom 1e6: double == perturbation at %d of 1600 points", same);
    fun_check(same >= 1600 * 97 / 100, what);
    view.julia = true;
    view.jr = -0.8;
    view.ji = 0.156;
    view.cx = dd_d(0.3);
    view.cy = dd_d(0.1);
    view.zoom = 1e5;
    apply_view();
    same = compare_modes(M_DOUBLE, M_DD, 40, 3000, &worst, &distinct);
    snprintf(what, sizeof(what), "Julia, zoom 1e5: double == double-double at %d of 1600", same);
    fun_check(same >= 1600 * 97 / 100, what);
    /* Deep: where double can't go, perturbation and double-double agree. */
    view.julia = false;
    view.cx = tour_x;
    view.cy = tour_y;
    view.zoom = 1e20;
    apply_view();
    uint64_t t0 = now();
    same = compare_modes(M_PERTURB, M_DD, 24, 6000, &worst, &distinct);
    uint64_t ms = (now() - t0) / NS_PER_MS;
    compare_modes(M_DOUBLE, M_DOUBLE, 24, 6000, &worst, &dist_d);
    snprintf(what, sizeof(what), "zoom 1e20: perturbation == double-double at %d of 576 (%lu ms)",
             same, (unsigned long)ms);
    fun_check(same >= 576 * 95 / 100, what);
    snprintf(what, sizeof(what), "zoom 1e20: %d different values in double-double, %d in double",
             distinct, dist_d);
    fun_check(distinct > 300 && dist_d < distinct / 4, what);
    view.zoom = 1e27;
    apply_view();
    same = compare_modes(M_PERTURB, M_DD, 24, 9000, &worst, &distinct);
    snprintf(what, sizeof(what),
             "zoom 1e27: perturbation == double-double at %d of 576 (%d values)",
             same, distinct);
    fun_check(same >= 576 * 90 / 100 && distinct > 300, what);
}

/* All CPUs render exactly what one does (all passes, anti-aliasing too). */
static bool test_render(bool avx2)
{
    char what[128];
    if (!view_alloc(160, 96))
        return false;
    view.julia = false;
    view.cx = dd_d(-0.5);
    view.cy = dd_d(0);
    view.zoom = 1;
    view.ss = 2;
    view.maxit = 500;
    kv.avx2 = avx2;
    render_all(true);
    uint64_t bytes = (uint64_t)PW * PH * 4;
    float *par = big_alloc(bytes);
    memcpy(par, nu, bytes);
    uint32_t aa_par = aa_used;
    render_all(false);
    snprintf(what, sizeof(what), "render: all CPUs == one CPU (160 x 96, all passes, %u AA)",
             aa_used);
    fun_check(!memcmp(par, nu, bytes) && aa_par == aa_used && aa_used > 100, what);
    bool sym = true;
    for (int y = 0; y < PH / 2; y++)
        sym &= !memcmp(nu + (uint64_t)y * PW, nu + (uint64_t)(PH - 1 - y) * PW, (size_t)PW * 4);
    fun_check(sym, "render: the set is mirror-symmetric about the real axis");
    uint32_t inside = 0;
    for (uint64_t i = 0; i < (uint64_t)PW * PH; i++)
        inside += nu[i] < 0;
    uint32_t pct = inside * 100 / (uint32_t)(PW * PH);
    snprintf(what, sizeof(what), "render: %u%% of the full view is inside (expect 10-20)", pct);
    fun_check(pct >= 10 && pct <= 20, what);
    /* Panning keeps what it can: the moved picture == a fresh one. */
    view.ss = 1;
    render_all(true);
    view.cx = dd_add_d(view.cx, 32 * kv.px);
    view.cy = dd_add_d(view.cy, 16 * kv.px);
    shift(32, 16);
    work(~0ull);
    memcpy(par, nu, bytes);
    render_all(true);
    uint32_t agree = 0;   /* (not bit for bit: the centre moved by a rounded amount) */
    for (uint64_t i = 0; i < (uint64_t)PW * PH; i++) {
        float d = par[i] - nu[i];
        agree += (par[i] < 0 && nu[i] < 0) || (d < 1e-3f && d > -1e-3f);
    }
    snprintf(what, sizeof(what), "render: a panned picture == the view drawn afresh (%u%%)",
             agree * 100 / (uint32_t)(PW * PH));
    fun_check(agree >= (uint32_t)(PW * PH) * 99 / 100, what);
    return true;
}

/* How much faster all CPUs are (information only). */
static bool speed(uint32_t n, bool avx2)
{
    if (!view_alloc(320, 176))
        return false;
    view.cx = dd_d(-0.743643887037151);
    view.cy = dd_d(0.131825904205330);
    view.zoom = 2000;
    view.maxit = 2000;
    kv.avx2 = false;
    uint64_t t0 = now();
    uint64_t iters = render_all(false);
    uint64_t one_ns = now() - t0;
    t0 = now();
    render_all(true);
    uint64_t all = now() - t0, all4 = 0;
    if (avx2) {
        kv.avx2 = true;
        t0 = now();
        render_all(true);
        all4 = now() - t0;
    }
    uint64_t x10 = all ? one_ns * 10 / all : 0;
    say("fractal: selftest: seahorse valley 320x176, %lu M iterations: %lu ms on 1 CPU, %lu ms "
        "on %u threads (%lu.%lux)",
        (unsigned long)(iters / 1000000), (unsigned long)(one_ns / 1000000),
        (unsigned long)(all / 1000000), n, (unsigned long)(x10 / 10), (unsigned long)(x10 % 10));
    if (avx2)
        say(", %lu ms with AVX2\n", (unsigned long)(all4 / 1000000));
    else
        say("\n");
    return true;
}

static void nothing(uint32_t item, uint32_t worker, void *arg)
{
    (void)item, (void)worker, (void)arg;
}

/* libfun's pool: after pool_rest (what gfx_key does before the app waits)
 * every worker sleeps at once. With the spin before sleeping made endless,
 * only the rest can put them to sleep, however late it comes. (From life's
 * self-test, removed with life on 2026-10-07.) */
static void test_pool_rest(uint32_t n)
{
    if (n < 2) {
        fun_check(true, "pool_rest: one thread, no workers to rest");
        return;
    }
    pool_set_spin(UINT32_MAX);
    pool_run(nothing, NULL, 4 * n);
    pool_rest();
    uint64_t deadline = now() + 5 * NS_PER_S;
    while (pool_asleep() < n - 1 && now() < deadline)
        jam_nanosleep(now() + NS_PER_MS);
    uint32_t asleep = pool_asleep();
    pool_set_spin(0);   /* back to the default: any still spinning sleep soon */
    say("fractal: pool_rest: %u of %u workers asleep\n", asleep, n - 1);
    fun_check(asleep == n - 1, "pool_rest: every worker asleep at once, not spinning");
}

int fractal_selftest(void)
{
    fun_selftest_begin("fractal", 64);
    uint32_t n = pool_start(0);
    bool avx2 = fun_has_avx2();
    say("fractal: selftest on %u threads (%u CPUs by CPUID), AVX2 %s\n", n, fun_cpu_count(),
        avx2 ? "yes" : "no");
    test_points();
    test_avx2(avx2);
    test_dd();
    test_kernels();
    if (!test_render(avx2) || !speed(n, avx2))
        return 1;
    test_pool_rest(n);
    return fun_selftest_end();
}
