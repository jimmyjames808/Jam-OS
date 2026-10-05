/* sysmon: a live picture of the machine, on the screen borrowed from the console.
 *
 *   sysmon                    (the shell command) the real machine
 *   run sysmon fake=28        a made-up machine with that many CPUs: no
 *                             handle needed (tests, screenshots of the
 *                             PC's 16 P + 12 E layout under QEMU)
 *   run sysmon --selftest     check the figures and the layout, exit 0 if right
 *   ... period=500            milliseconds between readings (100..5000)
 *
 * It shows a tile per CPU (a bar for the load now, a graph of the last
 * half minute; P threads blue, E threads orange), the load of all CPUs
 * together, memory used and free, context switches a second, the uptime,
 * and the processes that used the most CPU in the last interval (ties:
 * the most CPU time ever). q or Esc quits.
 *
 * Authority: the figures are the kernel's sys_info, cpu_stat and
 * proc_list, which need RIGHT_ROOT_SYSINFO on the root resource
 * (SR_RESOURCE): its list asks for that power (`right sysinfo`), and the
 * shell's `sysmon` command hands it the same. Without it the program says
 * so and ends.
 *
 * A reading every half second; the figures are what changed between two
 * readings (model.c). The whole frame is redrawn for each, and
 * gfx_present sends only the pixels that changed. */
#include <wants.h>
#include "sysmon.h"

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("right sysinfo\n"
          "svc wayland\n");

static struct sample readings[2];
static struct model model;

/* What supplies the readings: the kernel, or the made-up machine. */
struct source {
    handle_t root;    /* the root resource to read through */
    uint32_t fake;    /* CPUs of the made-up machine; 0: the real one */
    uint64_t rng;     /* the made-up machine's random state */
};

static status_t take(struct source *src, struct sample *s)
{
    if (!src->fake)
        return sample_take(src->root, s);
    sample_fake(s, src->fake, now(), &src->rng);
    return OK;
}

/* Readings and frames until q. */
static uint32_t watch(struct source *src, uint64_t period)
{
    uint32_t frames = 0;
    for (;; frames++) {
        const struct sample *a = &readings[frames % 2];
        struct sample *b = &readings[(frames + 1) % 2];
        uint64_t next = now() + period;
        int k;
        while ((k = gfx_key(next)) != KEY_NONE)   /* until the next reading is due */
            if (k == KEY_QUIT || k == 'q' || k == 'Q')
                return frames;
        if (src->fake)
            *b = *a;   /* the made-up machine counts on from its last reading */
        if (take(src, b) != OK)
            return frames;
        model_update(&model, a, b);
        draw(&model);
        gfx_present();
    }
}

static int monitor(int argc, char **argv)
{
    struct source src = { startup_handle(SR_RESOURCE), (uint32_t)arg_num(argc, argv, "fake", 0),
                          2025 };
    uint64_t period = arg_num(argc, argv, "period", 500);
    period = (period < 100 ? 100 : period > 5000 ? 5000 : period) * NS_PER_MS;
    status_t st = src.fake || src.root ? take(&src, &readings[0]) : ERR_BAD_HANDLE;
    if (st != OK) {
        say("sysmon: the kernel's figures need the root resource with RIGHT_ROOT_SYSINFO "
            "(%s).\nsysmon: start it from the shell, which hands it one "
            "(or `run sysmon fake=28` for a made-up machine)\n", status_str(st));
        return 1;
    }
    pool_start((uint32_t)arg_num(argc, argv, "threads", 0));
    if ((st = gfx_open()) != OK) {
        say("sysmon: can't borrow the screen (%s)\n", status_str(st));
        return 1;
    }
    model_init(&model, &readings[0]);
    if (!draw_setup(&model)) {
        gfx_close();
        say("sysmon: out of memory\n");
        return 1;
    }
    model_update(&model, &readings[0], &readings[0]);   /* the fixed figures, before any interval */
    draw(&model);
    gfx_present();
    uint64_t t0 = now();
    uint32_t frames = watch(&src, period);
    gfx_close();
    say("sysmon: %u readings of %u CPUs and %u processes in %lu s, %lu MB to the screen\n",
        frames, model.ncpu, model.nproc, (unsigned long)((now() - t0) / NS_PER_S),
        (unsigned long)(scr.bytes >> 20));
    return 0;
}

int main(int argc, char **argv)
{
    gfx_title("Sysmon");
    if (has_arg(argc, argv, "--selftest"))
        return sysmon_selftest();
    return monitor(argc, argv);
}
