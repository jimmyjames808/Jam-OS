/* xdg.h: xdg-shell's objects, shared by xdg.c (xdg_wm_base, xdg_positioner,
 * xdg_popup, each client's state, the ping clock) and xdgtop.c
 * (xdg_surface and xdg_toplevel: roles, configures, commits).
 *
 * Ownership. Each client's xdg-shell objects hang off its struct
 * xdg_client (comp_client.xdg) and go with it (xdg_teardown). An
 * xdg_surface points at its wl_surface until the wl_surface goes (the
 * role's `gone`): from then it is inert, and requests on it and on its
 * role object do nothing. The wl_surface points back through role_ops and
 * role_data from get_xdg_surface on (an xdg_surface is the start of a
 * role: without one, a commit is the protocol's not_constructed error),
 * and its role (comp_surface.role) is fixed by get_toplevel or get_popup. */
#pragma once

#include "wm.h"

#define XDG_BASES_MAX       16u   /* per client: xdg_wm_base objects bound */
#define XDG_POSITIONERS_MAX 64u   /* per client */
#define XDG_CONFIGS_MAX     8u    /* configures sent and not acked yet: more wait for an ack */

struct xdg_client;

/* An xdg_wm_base. */
struct xdg_base {
    struct xdg_client *xc;
    struct xdg_base *next;
    uint32_t id;
    uint32_t nsurfaces;            /* xdg_surfaces made through it, alive */
    uint32_t ping;                 /* the serial of the ping not answered yet, 0: none */
    uint64_t ping_ns;              /* when it went */
    bool late;                     /* past XDG_PING_NS: its client's windows are marked */
};

/* An xdg_positioner: only what get_popup checks. */
struct xdg_pos {
    struct xdg_client *xc;
    struct xdg_pos *next;
    uint32_t id;
    bool size_set, rect_set;       /* set_size and set_anchor_rect came */
};

enum xdg_role {
    XDG_ROLE_NONE,
    XDG_ROLE_TOPLEVEL,
    XDG_ROLE_POPUP,
};

/* A configure sent and not acked yet. */
struct xdg_sent {
    uint32_t serial;
    struct wm_config cfg;
    bool stale;                    /* sent before the toplevel was unmapped: acking it maps nothing */
};

/* An xdg_surface, and its role object (one, for its whole life). */
struct xdg_surf {
    struct comp_client *cl;
    struct xdg_client *xc;         /* cl's xdg-shell state */
    struct xdg_surf *next;
    struct xdg_base *base;         /* the xdg_wm_base that made it */
    uint32_t id;
    struct comp_surface *surface;  /* NULL once the wl_surface went: inert */
    enum xdg_role role;
    uint32_t role_id;              /* the xdg_toplevel or xdg_popup; 0 once destroyed */
    bool constructed;              /* get_toplevel or get_popup came (once only) */
    struct wm_window *ww;          /* the toplevel's, while role_id is live and the surface is */
    /* configures */
    bool initial;                  /* the initial commit came: configures may go */
    bool configured;               /* a configure since the initial commit was acked */
    struct xdg_sent sent[XDG_CONFIGS_MAX];
    uint32_t nsent;                /* oldest first */
    struct wm_config last;         /* the last one sent (has_last) */
    bool has_last;
    struct wm_config wanted;       /* the window manager's latest */
    bool dirty;                    /* wanted waits for room in sent[] */
    struct wm_config acked;        /* the newest acked, for the next commit (acked_new) */
    bool acked_new;
    uint32_t shown;                /* the states the shown buffer was drawn for */
    /* double-buffered: applied at commit */
    bool geom_set, limits_set;
    struct comp_box geom_pending, geom;   /* set_window_geometry (geom: x, y, w, h as a box) */
    int32_t min_w, min_h, max_w, max_h;   /* set_min_size, set_max_size */
};

struct xdg_client {
    struct comp_client *cl;
    uint32_t nbases, npositioners, nsurfs;
    struct xdg_base *bases;
    struct xdg_pos *positioners;
    struct xdg_surf *surfs;
};

/* xdg.c */
/* cl's xdg-shell state, made the first time. NULL: no memory. */
struct xdg_client *xdg_client_of(struct comp_client *cl);
/* x goes from its client's list and is freed (its role and surface left already). */
void xdg_surf_free(struct xdg_surf *x);
/* Ask x's client whether it is alive (a close came): a ping on x's base,
 * unless one is out already. */
void xdg_ping(struct xdg_surf *x);
/* get_popup's role, xdg.c's: the popup made and dismissed at once. */
status_t xdg_popup_create(struct xdg_surf *x, uint32_t id, uint32_t parent, uint32_t positioner);

/* xdgtop.c */
/* get_xdg_surface: an xdg_surface for wl_surface surface. OK, or a protocol error posted. */
status_t xdg_surf_create(struct comp_client *cl, struct xdg_base *b, uint32_t id,
                         uint32_t surface);
/* x's role and surface let go (teardown: dead, nothing sent). */
void xdg_surf_detach(struct xdg_surf *x);
/* The role ops of an xdg_surface before its role, and of a popup. */
extern const struct comp_role_ops xdg_bare_ops, xdg_popup_ops;
