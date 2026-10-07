/* jam_window_memory_v1 (abi/wayland/jam-window-memory-v1.xml; comp.h):
 * the key each toplevel is known by across the compositor's restarts
 * (wmsave.c). The manager's data is its client; it keeps nothing of its
 * own (a key is the window's: wm_window.key), so binding it costs only an
 * id. identify answers the toplevel's key on the manager; a toplevel
 * whose surface went (inert) gets 0. */
#include <jwl/jam_window_memory_v1.h>
#include "xdg.h"

status_t memory_bind(struct comp_client *cl, uint32_t id, uint32_t version)
{
    (void)version;
    return jwl_map_set_data(&cl->conn->map, id, cl);
}

static status_t on_destroy(void *data, uint32_t self)
{
    (void)data;
    (void)self;   /* libjwl freed the id; nothing else is ours */
    return OK;
}

static status_t on_identify(void *data, uint32_t self, uint32_t toplevel, uint32_t key_hi,
                            uint32_t key_lo)
{
    struct comp_client *cl = data;
    struct xdg_surf *x = comp_object(cl, toplevel, &jwl_xdg_toplevel_interface);
    uint64_t key = 0;
    if (x && x->ww && x->surface)
        key = wm_save_key(x->ww, (uint64_t)key_hi << 32 | key_lo);
    return jwl_jam_window_memory_v1_send_key(cl->conn, self, toplevel, (uint32_t)(key >> 32),
                                             (uint32_t)key);
}

static const struct jwl_jam_window_memory_v1_requests memory_ops = {
    .destroy = on_destroy, .identify = on_identify,
};

status_t memory_request(struct comp_client *cl, struct jwl_msg *m)
{
    (void)cl;   /* the object's data is the client */
    return jwl_jam_window_memory_v1_dispatch_request(&memory_ops, m->data, m->id, m->opcode,
                                                     m->args);
}
