/* utest's compositor tests (comp.c, comp_bad.c): bin/compositor run
 * headless in a job of its own, driven by test clients over real
 * channels through libjwl and the generated stubs (<jwl/wayland.h>), as
 * any Wayland program would. What the two files share. */
#pragma once

#include <jwl.h>
#include <jwl/wayland.h>

#define CT_WAIT (10 * NS_PER_S)   /* longest wait for anything the compositor owes us */

/* A compositor: its job and process, our end of /svc/wayland, and its
 * headless image mapped read-only. */
struct ct_comp {
    handle_t job, proc, svc;
    const uint32_t *image;     /* w * h pixels, 0x00RRGGBB */
    uint64_t image_size;
    int32_t w, h;
};

bool ct_start(struct ct_comp *p, int32_t w, int32_t h);
/* Close /svc/wayland: the compositor must end with 0 and leave its job empty. */
bool ct_stop(struct ct_comp *p);
/* The compositor job's handles in use now. */
uint64_t ct_handles(const struct ct_comp *p);
/* Wait (up to CT_WAIT) for the compositor's handles to be back at want. */
bool ct_handles_back(const struct ct_comp *p, uint64_t want);

/* One event a client got (the first four non-string arguments' words, and
 * its first string argument). */
struct ct_event {
    const struct jwl_interface *iface;
    uint16_t op;
    uint32_t id;
    uint32_t u[4];
    char     s[128];
};

#define CT_EVENTS 128
struct ct_client {
    struct jwl_conn *c;
    uint32_t registry, compositor, shm, output;   /* 0 until bound */
    struct ct_event ev[CT_EVENTS];                /* since the last ct_clear; the newest kept */
    unsigned nev;
    bool     errored;                             /* wl_display.error came */
    struct ct_event err;
};

/* A new connection to p (svc.connect on /svc/wayland), or a client on a
 * connection's channel ch (consumed). */
bool     ct_open(struct ct_comp *p, struct ct_client *k);
bool     ct_adopt(handle_t ch, struct ct_client *k);
void     ct_close(struct ct_client *k);
/* get_registry, then wl_compositor 4, wl_shm 1 and wl_output 3 bound by
 * their names (1, 2, 3), and a round trip. */
bool     ct_bind_all(struct ct_client *k);
void     ct_clear(struct ct_client *k);
/* wl_display.sync and the events up to its done: OK; ERR_INVALID_ARGS if
 * an error came instead; else the wait's status. */
status_t ct_roundtrip(struct ct_client *k);
/* The first event (iface, op) on object id (0: any) since ct_clear, or
 * NULL. */
const struct ct_event *ct_find(const struct ct_client *k, const struct jwl_interface *iface,
                               uint16_t op, uint32_t id);
/* Wait up to timeout for one; NULL if it didn't come. */
const struct ct_event *ct_await(struct ct_client *k, const struct jwl_interface *iface,
                                uint16_t op, uint32_t id, uint64_t timeout);
/* Flush, then wait for wl_display.error naming object with code. */
bool     ct_expect_error(struct ct_client *k, uint32_t object, uint32_t code);
/* A new id of ours for iface at version (jwl_conn_make). */
uint32_t ct_new(struct ct_client *k, const struct jwl_interface *iface, uint32_t version);

/* A VMO_KEEP_PAGES VMO of size bytes, and a duplicate of a VMO to send in
 * a request (read and map, and transfer). HANDLE_INVALID on failure. */
handle_t ct_kept_vmo(uint64_t size);
handle_t ct_dup_for_pool(handle_t vmo);
/* The job's use of kind (JOB_LIMIT_*) now. */
uint64_t ct_job_used(handle_t job, unsigned kind);
/* A pool of size bytes on k (wl_shm.create_pool): its id; the VMO's
 * handle (ours) in *vmo if vmo isn't NULL, else closed. 0 on failure. */
uint32_t ct_pool(struct ct_client *k, uint32_t size, handle_t *vmo);
