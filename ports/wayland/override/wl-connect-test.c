/* wl-connect-test: libwayland's wl_display_connect, from a tiny program
 * (G2's GP1, docs/G2-PLAN.md). It connects to the compositor the way any
 * Wayland client does (WAYLAND_DISPLAY, which Jam OS's shell sets to
 * /svc/wayland), asks for the registry, and lists the globals the
 * compositor offers. The registry's events reach the listener through
 * libwayland's dispatcher, which calls it with ffi_call: so one run proves
 * the connection (GS1's socket), libwayland's marshalling and libffi.
 *
 *     wl-connect-test            list the globals; exit 0 if wl_compositor,
 *                                wl_shm and xdg_wm_base are among them
 *
 * Output: "connected", one "global <name> <interface> v<version>" line per
 * global, then "wl-connect-test: ok" or what is missing. Exit 1 if it
 * can't connect (the reason is printed: no compositor, not allowed, no
 * WAYLAND_DISPLAY). This is Jam OS's (BSD-2-Clause), not libwayland's. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-client.h>

struct seen {
    int      globals;
    uint32_t compositor, shm, xdg_wm_base;   /* the versions bound, 0 if absent */
};

static void on_global(void *data, struct wl_registry *registry, uint32_t name,
                      const char *interface, uint32_t version)
{
    struct seen *s = data;
    (void)registry;
    printf("global %u %s v%u\n", name, interface, version);
    s->globals++;
    if (strcmp(interface, "wl_compositor") == 0)
        s->compositor = version;
    else if (strcmp(interface, "wl_shm") == 0)
        s->shm = version;
    else if (strcmp(interface, "xdg_wm_base") == 0)
        s->xdg_wm_base = version;
}

static void on_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener listener = { on_global, on_global_remove };

int main(void)
{
    const char *display = getenv("WAYLAND_DISPLAY");
    struct wl_display *wl = wl_display_connect(NULL);
    if (!wl) {
        fprintf(stderr, "wl-connect-test: can't connect to %s: %s\n",
                display ? display : "(WAYLAND_DISPLAY is not set)", strerror(errno));
        return 1;
    }
    printf("connected\n");
    struct seen seen = { 0, 0, 0, 0 };
    struct wl_registry *registry = wl_display_get_registry(wl);
    wl_registry_add_listener(registry, &listener, &seen);
    if (wl_display_roundtrip(wl) < 0) {
        fprintf(stderr, "wl-connect-test: the registry round trip failed: %s\n", strerror(errno));
        return 1;
    }
    int bad = 0;
    if (!seen.compositor) {
        fprintf(stderr, "wl-connect-test: no wl_compositor\n");
        bad = 1;
    }
    if (!seen.shm) {
        fprintf(stderr, "wl-connect-test: no wl_shm\n");
        bad = 1;
    }
    if (!seen.xdg_wm_base) {
        fprintf(stderr, "wl-connect-test: no xdg_wm_base\n");
        bad = 1;
    }
    wl_registry_destroy(registry);
    wl_display_disconnect(wl);
    if (!bad)
        printf("wl-connect-test: ok (%d globals)\n", seen.globals);
    return bad;
}
