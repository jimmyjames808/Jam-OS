/* System calls: VMOs and address spaces. The rules all sysc_* follow are
 * in sysc.h.
 *
 * vmo_read / vmo_write move data through a bounce buffer, one page at a
 * time, so no VMO lock is ever held across a user copy (the copy may fault
 * on the caller's own mapping of that same VMO, which takes it). Pages a
 * VMO commits are charged to the job of the process that created it. */
#include <jam/aspace.h>
#include <jam/mm.h>
#include <jam/sys.h>
#include <jam/syscall_impl.h>
#include <jam/vmo.h>
#include "sysc.h"

#define SMALL 256

int64_t sysc_vmo_create(uint64_t size, uint32_t flags, handle_t dma_cap, uint64_t out)
{
    SYSC_TABLE(t);
    handle_t h;
    status_t st = sys_vmo_create(t, size, flags, dma_cap, &h);
    return st == OK ? sysc_put_handle(t, out, h) : st;
}

static status_t get_vmo(struct handle_table *t, handle_t h, rights_t need, struct vmo **v)
{
    struct kobject *obj;
    status_t st = handle_get(t, h, OBJ_VMO, need, &obj, NULL);
    if (st == OK)
        *v = vmo_from_kobject(obj);
    return st;
}

/* Move len bytes between the VMO at offset and user memory at ubuf. */
static int64_t vmo_rw(handle_t h, uint64_t offset, uint64_t ubuf, uint64_t len, bool write)
{
    SYSC_TABLE(t);
    struct vmo *v;
    status_t st = get_vmo(t, h, write ? RIGHT_WRITE : RIGHT_READ, &v);
    if (st != OK)
        return st;
    if (offset > vmo_size(v) || len > vmo_size(v) - offset)
        st = ERR_OUT_OF_RANGE;   /* up front, so a bad call changes nothing */
    uint8_t small[SMALL];
    uint8_t *kb = len <= SMALL ? small : kmalloc(PAGE_SIZE);
    uint64_t chunk = len <= SMALL ? SMALL : PAGE_SIZE;
    if (st == OK && !kb)
        st = ERR_NO_MEMORY;
    while (st == OK && len) {
        uint64_t n = len < chunk ? len : chunk;
        if (write) {
            st = copy_from_user(kb, ubuf, n);
            if (st == OK)
                st = vmo_write(v, offset, kb, n);
        } else {
            st = vmo_read(v, offset, kb, n);
            if (st == OK)
                st = copy_to_user(ubuf, kb, n);
        }
        offset += n;
        ubuf += n;
        len -= n;
    }
    if (kb && kb != small)
        kfree(kb);
    kobject_unref(vmo_kobject(v));
    return st;
}

int64_t sysc_vmo_read(handle_t h, uint64_t offset, uint64_t buf, uint64_t len)
{
    return vmo_rw(h, offset, buf, len, false);
}

int64_t sysc_vmo_write(handle_t h, uint64_t offset, uint64_t buf, uint64_t len)
{
    return vmo_rw(h, offset, buf, len, true);
}

int64_t sysc_vmo_get_size(handle_t h, uint64_t size)
{
    SYSC_TABLE(t);
    uint64_t s;
    status_t st = sys_vmo_get_size(t, h, &s);
    if (st == OK && copy_to_user(size, &s, sizeof(s)) != OK)
        return ERR_INVALID_ARGS;
    return st;
}

int64_t sysc_vmo_set_size(handle_t h, uint64_t size)
{
    SYSC_TABLE(t);
    return sys_vmo_set_size(t, h, size);
}

int64_t sysc_vmo_commit(handle_t h, uint64_t offset, uint64_t len)
{
    SYSC_TABLE(t);
    return sys_vmo_commit(t, h, offset, len);
}

int64_t sysc_vmo_decommit(handle_t h, uint64_t offset, uint64_t len)
{
    SYSC_TABLE(t);
    return sys_vmo_decommit(t, h, offset, len);
}

/* ---- address spaces ---------------------------------------------------------- */

int64_t sysc_vmar_map(handle_t vmar, handle_t vmo, uint64_t vmo_off, uint64_t len,
                      uint32_t flags, uint64_t addr)
{
    SYSC_TABLE(t);
    uint64_t a = 0;
    if ((flags & VMAR_FIXED) && copy_from_user(&a, addr, sizeof(a)) != OK)
        return ERR_INVALID_ARGS;
    status_t st = sys_vmar_map(t, vmar, vmo, vmo_off, len, flags, &a);
    if (st == OK && copy_to_user(addr, &a, sizeof(a)) != OK) {
        sys_vmar_unmap(t, vmar, a, len);   /* nobody learned where it went */
        return ERR_INVALID_ARGS;
    }
    return st;
}

int64_t sysc_vmar_unmap(handle_t vmar, uint64_t addr, uint64_t len)
{
    SYSC_TABLE(t);
    return sys_vmar_unmap(t, vmar, addr, len);
}

int64_t sysc_vmar_protect(handle_t vmar, uint64_t addr, uint64_t len, uint32_t flags)
{
    SYSC_TABLE(t);
    return sys_vmar_protect(t, vmar, addr, len, flags);
}
