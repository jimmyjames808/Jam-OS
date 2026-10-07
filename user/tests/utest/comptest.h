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
    bool live;                 /* the desktop's own (/svc/wayland): no job, proc or svc of
                                * ours; each connection is an svc_open of it */
    const uint32_t *image;     /* w * h pixels, 0x00RRGGBB */
    uint64_t image_size;
    int32_t w, h;
};

/* bin/compositor headless at w x h with the desktop off (`nodesk`: the
 * window manager alone, the whole output the windows'; compdesk.c tests the
 * desktop). */
bool ct_start(struct ct_comp *p, int32_t w, int32_t h);
/* The image VMO for p (p->w x p->h): one for the compositor to compose
 * into (SR_USER + 1, theirs), mapped by us too (p->image). */
bool ct_image_for(struct ct_comp *p, handle_t *theirs);
/* The same with one more argument for the compositor (NULL: none). */
bool ct_start_arg(struct ct_comp *p, int32_t w, int32_t h, const char *arg);
/* The same with a state VMO (SR_STATE, a duplicate with svcstate's rights,
 * as init gives one: the arrangement a restarted compositor comes back
 * to); HANDLE_INVALID: none. */
bool ct_start_state(struct ct_comp *p, int32_t w, int32_t h, const char *arg, handle_t state);
/* Close /svc/wayland: the compositor must end with 0 and leave its job empty. */
bool ct_stop(struct ct_comp *p);
/* The compositor job's handles in use now. */
uint64_t ct_handles(const struct ct_comp *p);
/* Wait (up to CT_WAIT) for the compositor's handles to be back at want. */
bool ct_handles_back(const struct ct_comp *p, uint64_t want);
/* The compositor job's message bytes now (what it wrote that nobody has
 * read yet, its port's bindings and packets), and a wait (up to CT_WAIT)
 * for them to be back at want or below: a client gone, however it went,
 * leaves nothing charged to the compositor. */
uint64_t ct_msg_bytes(const struct ct_comp *p);
bool ct_msg_back(const struct ct_comp *p, uint64_t want);

/* One event a client got (the first four non-string arguments' words, its
 * first string argument, and its first array's first words). */
struct ct_event {
    const struct jwl_interface *iface;
    uint16_t op;
    uint32_t id;
    uint32_t u[4];
    char     s[128];
    uint32_t a[8];             /* the array's first words (wl_keyboard.enter: the keys held) */
    uint32_t na;               /* the array's size in words (only the first 8 kept) */
};

#define CT_EVENTS 128
struct ct_client {
    struct jwl_conn *c;
    uint32_t registry, compositor, shm, output;   /* 0 until bound */
    struct ct_event ev[CT_EVENTS];                /* since the last ct_clear; the newest kept */
    unsigned nev;
    bool     errored;                             /* wl_display.error came */
    struct ct_event err;
    handle_t kept;                                /* the first handle an event carried (the
                                                     test closes it), else they are closed */
};

/* A new connection to p: its channel (svc.connect on p's /svc/wayland, or
 * on the desktop's for a live p). */
status_t ct_connect(struct ct_comp *p, handle_t *ch);
/* A client on a new connection to p, or on a connection's channel ch
 * (consumed). */
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

/* The wallpaper's pixel (x, y) on a compositor's w by h output, as look.h
 * describes it (comp_ref.c's reference). */
uint32_t ref_wallpaper_of(int32_t x, int32_t y, int32_t w, int32_t h);

/* A VMO_KEEP_PAGES VMO of size bytes, and a duplicate of a VMO to send in
 * a request (read and map, and transfer). HANDLE_INVALID on failure. */
handle_t ct_kept_vmo(uint64_t size);
handle_t ct_dup_for_pool(handle_t vmo);
/* The job's use of kind (JOB_LIMIT_*) now. */
uint64_t ct_job_used(handle_t job, unsigned kind);
/* A pool of size bytes on k (wl_shm.create_pool): its id; the VMO's
 * handle (ours) in *vmo if vmo isn't NULL, else closed. 0 on failure. */
uint32_t ct_pool(struct ct_client *k, uint32_t size, handle_t *vmo);
