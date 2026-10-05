/* wltest internals (user/tests/wltest): main.c (arguments, the
 * interactive window, printing events), spawn.c (a headless compositor of
 * our own, for tests before init starts one) and script.c (the scripted
 * checks). */
#pragma once

#include <jwl_client.h>
#include <os.h>

/* A compositor wltest started itself: `bin/compositor headless`, given
 * the server end of a channel as its /svc/wayland (SR_USER + 0), in a job
 * of its own. Started again by comp_connect when it has died, as init
 * restarts the real one. */
struct comp_child {
    handle_t svc;          /* our end of its /svc/wayland; HANDLE_INVALID: none */
    handle_t proc, job;    /* the running one */
    int32_t  w, h;         /* its headless output */
    unsigned starts;       /* compositors started */
};

/* Start one. Errors: channel_create's, job_create's, spawn's. */
status_t comp_start(struct comp_child *k);
/* Kill the running one (its clients see their connections close). */
void     comp_kill(struct comp_child *k);
/* Close its /svc/wayland and wait for it to end; *k is cleared. */
void     comp_stop(struct comp_child *k);
/* A jwl_client_config.connect: a connection to k's compositor, started
 * again first if it died. */
status_t comp_connect(void *ctx, handle_t *out);

/* Print what the client bound, one "wltest: connected: " line. */
void     wl_print_info(const struct jwl_client_info *in);
/* Print one event as a "wltest: " line. */
void     wl_print_event(const struct jwl_event *ev);
/* Dispatch and print events until *flag or deadline: true if *flag. */
bool     wl_wait_flag(struct jwl_client *c, const bool *flag, uint64_t deadline);
/* The next event of type, printing every event on the way; false at
 * deadline or if the client dies. */
bool     wl_wait_event(struct jwl_client *c, uint32_t type, uint64_t deadline,
                       struct jwl_event *out);
/* The animated pattern, frame t, into fr. */
void     wl_draw(const struct jwl_frame *fr, uint32_t t);

/* The scripted checks (script.c): 0 when every one passed. k: the
 * compositor we started (the reconnect check kills it), or NULL. */
int      wl_script(const struct jwl_client_config *cfg, struct comp_child *k, unsigned frames);
