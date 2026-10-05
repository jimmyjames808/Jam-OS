/* The test power `testwin` (comp.h): windows without xdg-shell, at places
 * a test chooses, so the seat's tests (focus, keys, the pointer and its
 * grab) don't depend on how the window manager arranges windows. Only a
 * process that starts the compositor chooses its arguments, and init
 * never passes this one.
 *
 * With it, a surface that has no role when it commits a buffer becomes a
 * window (role COMP_ROLE_TEST) at the x and y of that commit's attach,
 * with no decorations; it is mapped while it has a buffer, and goes with
 * its surface. Nothing else about it is special: the scene, the seat and
 * painting treat it as any window. */
#include "seat.h"

static status_t testwin_role_commit(struct comp_surface *s)
{
    if (!s->window) {
        if (!s->buffer)
            return OK;
        struct comp_window *w;
        if (window_create(s, s->dx, s->dy, &w) != OK)
            return comp_no_memory(s->client, s->id, "no memory for a window");
    }
    window_map(s->window, s->buffer != NULL);
    return OK;
}

static const struct comp_role_ops testwin_role = {
    .name = "test window", .commit = testwin_role_commit, .gone = NULL,
};

status_t testwin_commit(struct comp_surface *s)
{
    if (!comp.testwin || s->role_ops || s->role != COMP_ROLE_NONE || !s->buffer)
        return OK;
    if (surface_set_role(s, COMP_ROLE_TEST, &testwin_role, NULL) != OK)
        return OK;
    return testwin_role_commit(s);
}
