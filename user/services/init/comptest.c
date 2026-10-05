/* init's hidden comptest mode (init.h): the kernel's `comptest` boot word
 * makes init start bin/compositor alone, with the screen (the root
 * resource's RIGHT_ROOT_SCREEN) and no console, running its test scene:
 * windows of known pixels with no client behind them (the compositor's
 * testscene.c), each step held on the screen for a few seconds so that
 * tools/comp-test.sh can take QEMU screenshots and check them against the
 * steps the log describes; then the compositor times whole frames
 * (`compositor: bench:` lines) and ends. The kernel draws its log again
 * once it has gone, and init's line goes into the RESULTS box.
 *
 * Starting the compositor on a plain boot is not done here. */
#include <os.h>
#include "init.h"

#define WAIT_S 120   /* the whole scene, the holds and the bench included */

/* The scene (testscene.c's commands). Every step fits an 800x600 screen. */
static const char *const scene_args[] = {
    "bin/compositor", "testscene",
    /* Step 1: two opaque floating windows and a translucent one over both
     * (the first focused), on the wallpaper; the arrow on the first's
     * circles, which show their symbols. */
    "win=60,80,420,260,ff2a6f97,tf", "win=330,220,380,240,ff8a4f2a,t",
    "win=200,150,300,200,a0e0a040,t", "cursor=118,74", "paint", "hold=4000",
    /* Step 2: the second moved, a tiled window's look below the others,
     * the first raised, no cursor. */
    "move=2,420,60", "win=520,400,240,150,ff3a7a50,g", "raise=2", "raise=3", "raise=1",
    "nocursor", "paint", "hold=4000",
    /* Step 3: a full-screen opaque window over all of it, the cursor on it. */
    "fullscreen=ff336699", "cursor=100,120", "paint", "hold=4000",
    "bench",
};

bool run_comptest(void)
{
    handle_t job = HANDLE_INVALID, proc = HANDLE_INVALID, mine = HANDLE_INVALID;
    struct spawn_handle x[2] = { { SR_RESOURCE, HANDLE_INVALID }, { SR_USER + 0, HANDLE_INVALID } };
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    if (st == OK)
        st = jam_handle_duplicate(startup_handle(SR_RESOURCE), RIGHTS_BASIC | RIGHT_ROOT_SCREEN,
                                  &x[0].h);
    if (st == OK)   /* /svc/wayland's server end: nobody connects */
        st = jam_channel_create(&mine, &x[1].h);
    if (st == OK) {
        struct spawn_args a = {
            .path = "bin/compositor", .argc = sizeof(scene_args) / sizeof(scene_args[0]),
            .argv = scene_args, .job = job, .extra = x, .nextra = 2,
        };
        st = spawn(&a, &proc);   /* consumes x */
        x[0].h = x[1].h = HANDLE_INVALID;
    }
    struct process_info info = { 0 };
    if (st == OK)
        st = spawn_wait(proc, WAIT_S * NS_PER_S, &info);
    if (st == ERR_TIMED_OUT && jam_job_kill(job) == OK)
        init_say("comptest: the compositor still ran after %d s: killed", WAIT_S);
    else if (st != OK)
        init_say("comptest: can't run the compositor (%s)", status_str(st));
    else
        init_say("comptest: the compositor %s %ld", info.killed ? "was killed, code"
                                                                : "exited with code",
                 (long)info.exit_code);
    handle_t left[] = { x[0].h, x[1].h, mine, proc, job };
    for (unsigned k = 0; k < sizeof(left) / sizeof(left[0]); k++)
        if (left[k] != HANDLE_INVALID)
            jam_handle_close(left[k]);
    return st == OK && !info.killed && info.exit_code == 0;
}
