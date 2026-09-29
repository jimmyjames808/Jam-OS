/* life: the self-test (`run life --selftest`). */
#include "life.h"

/* A glider moves one cell down and right every 4 generations. */
static void test_glider(void)
{
    world_alloc(256);
    world_clear();
    stamp(glider, 3, 10, 10, 0);
    uint64_t pop = 0;
    for (int i = 0; i < 4; i++)
        pop = step(true);
    bool same = pop == 5;
    for (uint32_t j = 0; j < 3; j++)
        for (uint32_t i = 0; i < 3; i++)
            same &= get(cur, 11 + i, 11 + j) == (glider[j][i] == 'O');
    fun_check(same, "glider: 4 generations = moved by (+1, +1), 5 cells");
    /* ... and wraps round the torus: after 4 * 256 generations it is back. */
    for (int i = 0; i < 4 * 256 - 4; i++)
        step(true);
    same = population(cur) == 5;
    for (uint32_t j = 0; j < 3; j++)
        for (uint32_t i = 0; i < 3; i++)
            same &= get(cur, 10 + i, 10 + j) == (glider[j][i] == 'O');
    fun_check(same, "glider: 1024 generations on a 256-torus = back home");
}

/* A blinker at the left/right word seam (x = 63, 64, 65), and the ages. */
static void test_blinker_ages(void)
{
    world_clear();
    set(cur, 63, 50, true), set(cur, 64, 50, true), set(cur, 65, 50, true);
    step(true);
    bool ok = get(cur, 64, 49) && get(cur, 64, 50) && get(cur, 64, 51) && population(cur) == 3;
    step(true);
    ok &= get(cur, 63, 50) && get(cur, 64, 50) && get(cur, 65, 50) && population(cur) == 3;
    fun_check(ok, "blinker across a word boundary: period 2");
    /* Ages: its middle never changes (7 for good after 7 generations); its
     * ends change every generation (0); a cell far away stays 7. */
    for (int i = 0; i < 8; i++)
        step(true);
    ok = age_of(64, 50) == 7 && age_of(63, 50) == 0 && age_of(64, 49) == 0 &&
         age_of(200, 200) == 7 && get(cur, 63, 50);
    step(true);
    ok &= age_of(64, 50) == 7 && age_of(63, 50) == 0 && !get(cur, 63, 50) && get(cur, 64, 49);
    fun_check(ok, "ages: a blinker's middle is 7, its ends are always 0");
    world_clear();
    stamp(glider, 3, 100, 100, 0);
    for (int i = 0; i < 3; i++)
        step(true);
    ok = true;
    for (uint32_t y = 90; y < 120; y++)
        for (uint32_t x = 90; x < 120; x++)
            if (get(cur, x, y))
                ok &= age_of(x, y) <= 3;   /* a glider has no cell older than its period */
    fun_check(ok && population(cur) == 5, "ages: a glider's cells are all young");
}

/* The R-pentomino settles at generation 1103 with 116 cells. */
static void test_rpentomino(void)
{
    world_alloc(1024);
    world_clear();
    stamp(rpent, 3, 512, 512, 0);
    /* It settles into 116 cells (6 of them gliders flying off) at generation
     * 1103: find the last generation whose population differs from gen 1500's. */
    uint64_t t0 = now(), last_change = 0, last_pop = 0, pop = 0;
    while (gen < 1500) {
        pop = step(true);
        if (pop != last_pop)
            last_change = gen;
        last_pop = pop;
    }
    uint64_t ms = (now() - t0) / NS_PER_MS;
    char what[96];
    snprintf(what, sizeof(what), "R-pentomino: 116 cells for good from gen %lu (1103-ish)",
             (unsigned long)last_change);
    fun_check(pop == 116 && last_change >= 1100 && last_change <= 1106, what);
    say("life: selftest: 1500 generations of 1024x1024 in %lu ms\n", (unsigned long)ms);
}

/* Every CPU together computes exactly what one CPU does alone (ages too). */
static void test_parallel(void)
{
    world_alloc(512);
    soup_seed = 12345;
    soup();
    uint64_t bytes = (uint64_t)W * WW * 8, words = bytes / 8;
    uint64_t *copy = big_alloc(bytes * 4);
    memcpy(copy, cur, bytes);
    for (int k = 0; k < 3; k++)
        memcpy(copy + (k + 1) * words, age[k], bytes);
    for (int i = 0; i < 64; i++)
        step(true);
    uint64_t *par = big_alloc(bytes * 4);
    memcpy(par, cur, bytes);
    for (int k = 0; k < 3; k++)
        memcpy(par + (k + 1) * words, age[k], bytes);
    memcpy(cur, copy, bytes);
    memset(old, 0, bytes);
    for (int k = 0; k < 3; k++)
        memcpy(age[k], copy + (k + 1) * words, bytes);
    for (int i = 0; i < 64; i++)
        step(false);
    bool ok = !memcmp(par, cur, bytes) && population(cur) > 1000;
    for (int k = 0; k < 3; k++)
        ok &= !memcmp(par + (k + 1) * words, age[k], bytes);
    fun_check(ok, "random soup 512x512, 64 generations: all CPUs == one CPU");
}

/* How much faster all CPUs are on a big world (information only). */
static void speed(uint32_t n)
{
    world_alloc(4096);
    soup();
    uint64_t t0 = now();
    for (int i = 0; i < 4; i++)
        step(false);
    uint64_t one = now() - t0;
    t0 = now();
    for (int i = 0; i < 4; i++)
        step(true);
    uint64_t all = now() - t0;
    ages = false;
    t0 = now();
    for (int i = 0; i < 4; i++)
        step(true);
    uint64_t noage = now() - t0;
    ages = true;
    uint64_t x10 = all ? one * 10 / all : 0;
    say("life: selftest: 4096x4096: %lu ms/gen on 1 CPU, %lu.%lu ms/gen on %u threads (%lu.%lux); "
        "%lu.%lu ms/gen without ages\n",
        (unsigned long)(one / 4000000), (unsigned long)(all / 4000000),
        (unsigned long)(all / 400000 % 10), n, (unsigned long)(x10 / 10),
        (unsigned long)(x10 % 10), (unsigned long)(noage / 4000000),
        (unsigned long)(noage / 400000 % 10));
}

int life_selftest(void)
{
    fun_selftest_begin("life", 58);
    uint32_t n = pool_start(0);
    say("life: selftest on %u threads (%u CPUs by CPUID)\n", n, fun_cpu_count());
    test_glider();
    test_blinker_ages();
    test_rpentomino();
    test_parallel();
    speed(n);
    return fun_selftest_end();
}
