/* netstack: a socket's ring memory (sockmem.h has the model). */
#include "sockmem.h"

status_t sockmem_make(struct sockmem *m, struct sockring *r, uint32_t framing, uint32_t tx,
                      uint32_t rx)
{
    uint64_t va = 0, bytes = sockring_bytes(tx, rx);
    status_t st = jam_vmo_create(bytes, 0, HANDLE_INVALID, &m->vmo);
    if (st == OK)
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), m->vmo, 0, bytes,
                          VMAR_READ | VMAR_WRITE, &va);
    if (st == OK) {
        m->map = (uint8_t *)(uintptr_t)va;
        m->bytes = bytes;
        st = sockring_make(r, m->map, framing, tx, rx);
    }
    if (st == OK)
        st = jam_event_create(&m->to_stack);
    if (st == OK)
        st = jam_event_create(&m->to_prog);
    return st;
}

void sockmem_drop(struct sockmem *m, handle_t port, uint64_t key)
{
    if (m->to_stack)
        (void)jam_port_unbind(port, m->to_stack, key);   /* not bound yet: nothing to undo */
    if (m->map)
        (void)jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)m->map,
                             m->bytes);   /* our own mapping: nothing else to do */
    if (m->vmo)
        (void)jam_vmo_set_size(m->vmo, 0);   /* nothing to keep if it fails: it closes next */
    handle_t hs[] = { m->vmo, m->to_stack, m->to_prog };
    for (unsigned k = 0; k < 3; k++)
        if (hs[k])
            jam_handle_close(hs[k]);
    *m = (struct sockmem){ 0 };
}

status_t sockmem_handles(const struct sockmem *m, handle_t hs[3])
{
    hs[0] = hs[1] = hs[2] = HANDLE_INVALID;
    status_t st = jam_handle_duplicate(m->vmo, SOCKRING_VMO_RIGHTS, &hs[0]);
    if (st == OK)
        st = jam_handle_duplicate(m->to_stack, SOCKRING_TO_STACK_RIGHTS, &hs[1]);
    if (st == OK)
        st = jam_handle_duplicate(m->to_prog, SOCKRING_TO_PROG_RIGHTS, &hs[2]);
    if (st != OK) {
        for (unsigned k = 0; k < 3; k++)
            if (hs[k])
                jam_handle_close(hs[k]);
        hs[0] = hs[1] = hs[2] = HANDLE_INVALID;
    }
    return st;
}

bool sockmem_budget_ok(const struct opener *o, uint8_t cls, uint64_t bytes)
{
    uint64_t total = pg.held[CLASS_PROG].ring_bytes + pg.held[CLASS_SYS].ring_bytes;
    if (total + bytes > SOCKRING_TOTAL_BYTES ||
        (o && o->ring_bytes + bytes > SOCKRING_OPENER_BYTES))
        return false;
    return progs_share_ok(cls, 0, 0, bytes);
}
