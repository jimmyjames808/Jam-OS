/* wltest: a headless compositor of our own (wltest.h), started the way
 * init will start the real one: the server end of a channel as its
 * /svc/wayland (SR_USER + 0), which it serves with the svc protocol's
 * connect, one connection per opener. Used by `wltest --spawn` until init
 * publishes /svc/wayland, and by the scripted reconnect check, which kills
 * it and lets comp_connect start the next one. */
#include <idl/svc.h>
#include <os.h>
#include "wltest.h"

#define CONNECT_WAIT (2 * NS_PER_S)
#define END_WAIT     (5 * NS_PER_S)

status_t comp_start(struct comp_child *k)
{
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    st = jam_job_create(startup_handle(SR_JOB), 0, &k->job);
    if (st != OK) {
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    char size[32];
    snprintf(size, sizeof(size), "size=%dx%d", k->w, k->h);
    const char *argv[] = { "bin/compositor", "headless", size };
    struct spawn_handle x[] = { { SR_USER + 0, theirs } };
    struct spawn_args a = { .path = "bin/compositor", .argc = 3, .argv = argv, .job = k->job,
                            .extra = x, .nextra = 1 };
    st = spawn(&a, &k->proc);   /* theirs is consumed either way */
    if (st != OK) {
        jam_handle_close(mine);
        jam_handle_close(k->job);
        k->job = HANDLE_INVALID;
        return st;
    }
    k->svc = mine;
    k->starts++;
    printf("wltest: started a headless compositor (%dx%d), the %u%s\n", k->w, k->h, k->starts,
           k->starts == 1 ? "st" : k->starts == 2 ? "nd" : "th");
    return OK;
}

/* The running one's handles, closed once it has ended. */
static void reap(struct comp_child *k)
{
    struct process_info info;
    if (spawn_wait(k->proc, END_WAIT, &info) != OK)
        printf("wltest: the compositor didn't end within %llu s\n",
               (unsigned long long)(END_WAIT / NS_PER_S));
    jam_handle_close(k->proc);
    jam_handle_close(k->job);
    jam_handle_close(k->svc);
    k->proc = k->job = k->svc = HANDLE_INVALID;
}

void comp_kill(struct comp_child *k)
{
    if (k->proc == HANDLE_INVALID)
        return;
    printf("wltest: killing the compositor\n");
    (void)jam_process_kill(k->proc);   /* already dead: nothing to do */
    reap(k);
}

void comp_stop(struct comp_child *k)
{
    if (k->proc == HANDLE_INVALID)
        return;
    jam_handle_close(k->svc);   /* /svc/wayland gone: it ends */
    k->svc = HANDLE_INVALID;
    reap(k);
}

status_t comp_connect(void *ctx, handle_t *out)
{
    struct comp_child *k = ctx;
    signals_t seen = 0;
    bool dead = k->svc == HANDLE_INVALID ||
                (jam_object_wait_one(k->svc, SIG_PEER_CLOSED, 0, &seen) == OK &&
                 (seen & SIG_PEER_CLOSED));
    if (dead) {
        if (k->proc != HANDLE_INVALID)
            reap(k);
        status_t st = comp_start(k);
        if (st != OK)
            return st;
    }
    return svc_connect_within(k->svc, CONNECT_WAIT, out);
}
