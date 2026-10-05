/* The paint clock (comp.h): when the loop paints, and the frame callbacks'
 * turn after it.
 *
 * There is no vsync on the GOP framebuffer, so the compositor keeps its own
 * clock: it paints when there is damage (or a visible surface's frame
 * callbacks wait for a paint), at most once per period: 60 a second by
 * default, display.hz in the settings to change it (init passes it on;
 * clock_set_hz). Periods count from a paint's start, so a steady stream of
 * commits paints at exactly the rate. A paint after an idle period runs at
 * once: typing shows without waiting for a tick.
 *
 * After a paint, the frame callbacks of the visible surfaces it showed are
 * answered with the paint's time (surfaces_frame_done); a surface nobody
 * sees (off the screen, or under an opaque window: window_covered) gets
 * them at most once a second, so a hidden animation slows down instead of
 * spinning or stopping.
 *
 * The cost of painting is counted (comp.stats: paints, pixels, the last
 * and the worst time) and summed up in one log line every LOG_EVERY while
 * anything is painted: frames, pixels a frame, the mean and worst time,
 * and how many went the full-screen way. */
#include "paint.h"

#define HZ_DEFAULT 60
#define HZ_MAX     240
#define LOG_EVERY  (10 * NS_PER_S)

/* The paints since the last log line. */
static struct {
    uint64_t since;      /* when the window began (0: no paint yet) */
    uint64_t frames, px, ns, worst, full;
} sum;

void clock_set_hz(uint32_t hz)
{
    hz = hz < 1 ? 1 : hz > HZ_MAX ? HZ_MAX : hz;
    comp.period_ns = NS_PER_S / hz;
}

void clock_init(uint32_t hz)
{
    clock_set_hz(hz ? hz : HZ_DEFAULT);
}

/* One paint's cost into the stats and the log line's sums. */
static void count(uint64_t t)
{
    uint64_t ns = paint_last.ns;
    scene.paints++;
    comp.stats.paints++;
    comp.stats.last_paint_ns = ns;
    if (ns > comp.stats.worst_paint_ns)
        comp.stats.worst_paint_ns = ns;
    if (!sum.since)
        sum.since = t;
    sum.frames++;
    sum.px += paint_last.px;
    sum.ns += ns;
    sum.worst = ns > sum.worst ? ns : sum.worst;
    sum.full += paint_last.direct > 0;
    if (t - sum.since < LOG_EVERY)
        return;
    printf("compositor: paint: %lu frames in %lu ms, %lu px a frame, mean %lu us, worst %lu us, "
           "%lu full screen\n", (unsigned long)sum.frames,
           (unsigned long)((t - sum.since) / NS_PER_MS), (unsigned long)(sum.px / sum.frames),
           (unsigned long)(sum.ns / sum.frames / 1000), (unsigned long)(sum.worst / 1000),
           (unsigned long)sum.full);
    memset(&sum, 0, sizeof(sum));
}

void clock_turn(void)
{
    uint64_t t = now();
    bool want = !damage_empty(&scene.damage) || surfaces_waiting_paint();
    if (want && t >= scene.last_paint_ns + comp.period_ns) {
        scene.last_paint_ns = t;
        (void)paint_frame();
        count(t);
        surfaces_frame_done(now(), true);
    } else if (t >= surfaces_hidden_deadline()) {
        surfaces_frame_done(t, false);
    }
}

uint64_t clock_deadline(void)
{
    uint64_t d = surfaces_hidden_deadline();
    if (!damage_empty(&scene.damage) || surfaces_waiting_paint()) {
        uint64_t p = scene.last_paint_ns + comp.period_ns;
        d = p < d ? p : d;
    }
    return d;
}
