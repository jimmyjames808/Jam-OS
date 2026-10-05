/* fbbench: what the compositor will depend on, measured on the screen
 * itself (docs/G1-PLAN.md, track P0): the framebuffer's write-combining
 * stores at several thread counts and store widths and in tiles, a copy
 * from RAM to it, RAM to RAM, the premultiplied blend per megapixel
 * (scalar, SSE2, AVX2), vmo_read's rate (Q2's fallback), and whole frames
 * as the compositor will paint them, Q2's two ways. The numbers decide the
 * compositor's tile size and replace the plan's estimates.
 *
 * Run it from the shell: `fbbench [-q]` (-q: a tenth of the samples, for
 * QEMU, whose numbers mean nothing). It borrows the screen from the
 * console (console.lend_screen, as libfun's programs do) and draws test
 * patterns on it while it measures (about half a minute on the PC), then
 * gives it back and prints its lines, bench's format, to the terminal and
 * into the kernel log: `grep 'fbbench:'` finds them in the stick's log
 * (or the netlog on the Mac). With no screen to borrow it says why and
 * measures what needs none (RAM, blend, vmo_read).
 *
 * Every sample is one whole piece of work (the screen, a window, a text
 * line), so a line's median is that piece's cost. Threads are not pinned
 * (no system call pins one from user space); the crew of each line has
 * exactly the threads it names (crew.c). The kernels' self-test runs
 * first: a SIMD blend that disagrees with libfun's px_over fails the run.
 *
 * Exit 0 if every line was measured, 1 if one failed (the line says so),
 * 2 for a usage error. */
#include <idl/console.h>
#include "fbbench.h"

#define SAMPLES     64
#define SHORT       1000   /* samples of a line that takes microseconds */
#define NO_SCREEN_W 2560   /* the PC's screen, for the lines that need none */
#define NO_SCREEN_H 1440
#define TILE_MIN    (64 * 64)

struct bench B;
uint32_t *tile_buf[CREW_MAX];

static handle_t lease = HANDLE_INVALID;   /* the screen's lease, while we have it */

/* Borrow the screen; false with why (B.fb stays NULL). */
static bool borrow(char *why, size_t n)
{
    handle_t con = startup_handle(SR_CONSOLE), vmo;
    uint32_t w, h, pitch;
    uint8_t rs, gs, bs;
    uint64_t size;
    if (!con) {
        snprintf(why, n, "no console");
        return false;
    }
    status_t st = console_lend_screen(con, &w, &h, &pitch, &rs, &gs, &bs, &size, &vmo, &lease);
    if (st != OK) {
        snprintf(why, n, "the console didn't lend it: %s", status_str(st));
        return false;
    }
    uint64_t addr = 0, len = (size + 4095) & ~4095ull;
    if (w < 320 || h < 200 || w > 8192 || h > 8192 || pitch < w * 4 || pitch % 4 ||
        size < (uint64_t)pitch * h)
        st = ERR_NOT_SUPPORTED;
    else
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, len, VMAR_READ | VMAR_WRITE,
                          &addr);
    jam_handle_close(vmo);   /* the mapping keeps it */
    if (st != OK) {
        snprintf(why, n, "%ux%u pitch %u: %s", w, h, pitch, status_str(st));
        jam_handle_close(lease);
        lease = HANDLE_INVALID;
        return false;
    }
    B.fb = (uint32_t *)(uintptr_t)addr;
    B.fbpitch = pitch / 4;
    B.w = (int)w;
    B.h = (int)h;
    out_text("fbbench: screen %ux%u, pitch %u bytes, red at bit %u, green %u, blue %u%s\n", w, h,
             pitch, rs, gs, bs, rs == 16 && gs == 8 && bs == 0 ? " (xrgb, as libfun draws)" : "");
    return true;
}

/* The client's buffer `win` in a VMO of its own (vmo_read reads it). */
static bool make_win(void)
{
    uint64_t bytes = (B.frame + 4095) & ~4095ull, addr = 0;
    if (jam_vmo_create(bytes, 0, HANDLE_INVALID, &B.vmo) != OK)
        return false;
    if (jam_vmar_map(startup_handle(SR_SELF_VMAR), B.vmo, 0, bytes, VMAR_READ | VMAR_WRITE,
                     &addr) != OK)
        return false;
    B.win = (uint32_t *)(uintptr_t)addr;
    return true;
}

/* Every buffer, with its pattern (which also commits its pages). */
static bool buffers(void)
{
    uint64_t px = (uint64_t)B.w * (uint64_t)B.h;
    B.frame = px * 4;
    B.opaque = big_alloc(B.frame);
    B.copy = big_alloc(B.frame);
    B.shown = big_alloc(B.frame);
    if (!B.opaque || !B.copy || !B.shown || !make_win())
        return false;
    uint64_t tile = (uint64_t)B.w * BAND > TILE_MIN ? (uint64_t)B.w * BAND : TILE_MIN;
    for (uint32_t i = 0; i < B.ncpu; i++) {
        tile_buf[i] = big_alloc(tile * 4);
        if (!tile_buf[i])
            return false;
        pattern_xrgb(tile_buf[i], tile, 100 + i);
    }
    pattern_xrgb(B.opaque, px, 1);
    pattern_argb(B.win, px, 2);
    memcpy(B.copy, B.opaque, B.frame);
    memcpy(B.shown, B.opaque, B.frame);
    return true;
}

static void header(void)
{
    char stamp[24];
    uint64_t ps = timing_stamp_ps();
    snprintf(stamp, sizeof(stamp), "%lu.%lu ns", (unsigned long)(ps / 1000),
             (unsigned long)(ps / 100 % 10));
    out_text("fbbench: %u CPUs (all of them = the most threads a line uses), AVX2 %s, TSC %lu "
             "MHz (measured), timestamp %s (subtracted)\n", B.ncpu, B.avx2 ? "yes" : "no",
             (unsigned long)timing_tsc_mhz(), stamp);
    out_text("fbbench: median and p99 of %u samples a line (%u for the short ones), each the "
             "whole piece of work once, after a 20 ms warm-up; GB/s at the median (10^9 bytes)\n",
             B.samples, B.short_samples);
    out_text("fbbench: threads not pinned; a whole screen is cut into %d-row bands, handed out "
             "one at a time\n", BAND);
}

static int usage(void)
{
    printf("usage: fbbench [-q]   (-q: a tenth of the samples)\n");
    return 2;
}

int main(int argc, char **argv)
{
    B.samples = SAMPLES;
    B.short_samples = SHORT;
    if (argc == 2 && !strcmp(argv[1], "-q")) {
        B.samples = SAMPLES / 10;
        B.short_samples = SHORT / 10;
    } else if (argc != 1)
        return usage();
    B.ncpu = fun_cpu_count();
    B.avx2 = fun_has_avx2();
    status_t pr = jam_thread_set_priority(startup_handle(SR_SELF_THREAD), THREAD_PRIO_USER_MAX);
    char why[160];
    if (!kernels_selftest(why, sizeof(why))) {
        printf("fbbench: kernels: FAILED: %s\n", why);
        return 1;
    }
    printf("fbbench: kernels agree with px_over; borrowing the screen to measure (test "
           "patterns on it until the lines print)\n");
    timing_init();
    if (!borrow(why, sizeof(why))) {
        out_text("fbbench: no screen (%s): measuring what needs none\n", why);
        B.w = NO_SCREEN_W;
        B.h = NO_SCREEN_H;
    }
    if (pr != OK)
        out_text("fbbench: priority 16 (24 refused: %s)\n", status_str(pr));
    bool ok = buffers();
    if (ok) {
        header();
        ok &= lines_fb();
        lines_ram();
        lines_blend();
        ok &= lines_vmo();
        ok &= lines_frames();
    } else {
        out_text("fbbench: FAILED: no memory for the buffers (%lu bytes each)\n",
                 (unsigned long)B.frame);
    }
    if (lease != HANDLE_INVALID)
        jam_handle_close(lease);   /* the console redraws its text */
    out_flush();
    printf("fbbench: %s\n", ok ? "done" : "done, with failures");
    return ok ? 0 : 1;
}
