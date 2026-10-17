/* System calls: VMOs and address spaces. The rules all sysc_* follow are
 * in sysc.h.
 *
 * vmo_read / vmo_write move data through a bounce buffer, one page at a
 * time, so no VMO lock is ever held across a user copy (the copy may fault
 * on the caller's own mapping of that same VMO, which takes it). Pages a
 * VMO commits are charged to the job of the process that created it. */
#include <jam/aspace.h>
#include <jam/mm.h>
#include <jam/pager.h>
#include <jam/process.h>
#include <jam/sys.h>
#include <jam/syscall_impl.h>
#include <jam/vmo.h>
#include "sysc.h"

#define SMALL 256

int64_t sysc_vmo_create(uint64_t size, uint32_t flags, uint64_t out)
{
    SYSC_TABLE(t);
    handle_t h;
    status_t st = sys_vmo_create(t, size, flags, &h);
    return st == OK ? sysc_put_handle(t, out, h) : st;
}

int64_t sysc_dma_vmo_create(handle_t dma, uint64_t size, uint32_t flags, uint64_t out)
{
    SYSC_TABLE(t);
    handle_t h;
    status_t st = sys_dma_vmo_create(t, dma, size, flags, &h);
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

int64_t sysc_vmo_make_exec(handle_t h, handle_t root, uint64_t out)
{
    SYSC_TABLE(t);
    handle_t nh;
    status_t st = sys_vmo_make_exec(t, h, root, &nh);
    return st == OK ? sysc_put_handle(t, out, nh) : st;
}

int64_t sysc_vmo_make_exec_verified(handle_t h, handle_t root, handle_t table, uint32_t flags,
                                    uint64_t out)
{
    SYSC_TABLE(t);
    handle_t nh;
    status_t st = sys_vmo_make_exec_verified(t, h, root, table, flags, &nh);
    return st == OK ? sysc_put_handle(t, out, nh) : st;
}

int64_t sysc_vmo_decommit(handle_t h, uint64_t offset, uint64_t len)
{
    SYSC_TABLE(t);
    return sys_vmo_decommit(t, h, offset, len);
}

/* Writable pager VMOs' msync (docs/WRITEMAP-PLAN.md, vmo_pager_sync). */
int64_t sysc_vmo_sync(handle_t vmo, uint64_t offset, uint64_t len, uint32_t flags)
{
    SYSC_TABLE(t);
    struct vmo *v;
    status_t st = get_vmo(t, vmo, RIGHT_READ, &v);
    if (st != OK)
        return st;
    st = vmo_pager_sync(v, offset, len, flags);   /* (may wait: no lock held) */
    kobject_unref(vmo_kobject(v));
    return st;
}

int64_t sysc_vmo_create_child(handle_t h, uint32_t flags, uint64_t offset, uint64_t size,
                              uint64_t out)
{
    SYSC_TABLE(t);
    handle_t c;
    status_t st = sys_vmo_create_child(t, h, flags, offset, size, &c);
    return st == OK ? sysc_put_handle(t, out, c) : st;
}

int64_t sysc_vmo_make_jit(handle_t h, handle_t root, uint64_t out)
{
    SYSC_TABLE(t);
    handle_t nh;
    status_t st = sys_vmo_make_jit(t, h, root, &nh);
    return st == OK ? sysc_put_handle(t, out, nh) : st;
}

/* ---- address spaces ---------------------------------------------------------- */

int64_t sysc_vmar_map(handle_t vmar, handle_t vmo, uint64_t vmo_off, uint64_t len,
                      uint32_t flags, uint64_t addr)
{
    SYSC_TABLE(t);
    uint64_t a = 0;
    if ((flags & VMAR_FIXED) && copy_from_user(&a, addr, sizeof(a)) != OK)
        return ERR_INVALID_ARGS;
    /* A replace can't be undone by an unmap (what it replaced is gone), so
     * its answer, the address it was given, is written first. Only for a
     * replace that can be made (with VMAR_FIXED): one without is refused
     * with the caller's word left as it was. */
    bool replace = (flags & VMAR_REPLACE) && (flags & VMAR_FIXED);
    if (replace && copy_to_user(addr, &a, sizeof(a)) != OK)
        return ERR_INVALID_ARGS;
    status_t st = sys_vmar_map(t, vmar, vmo, vmo_off, len, flags, &a);
    if (st == OK && replace)
        return OK;   /* a is what *addr holds already */
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

/* mincore's kernel half (abi/syscalls.def). The bytes are made in a
 * kernel buffer under the address space's locks and copied out after,
 * with none held: the copy may fault in the same address space. */
int64_t sysc_vmar_resident(handle_t vmar, uint64_t addr, uint64_t len, uint64_t out)
{
    SYSC_TABLE(t);
    uint8_t *buf = kmalloc(VMAR_RESIDENT_MAX);
    if (!buf)
        return ERR_NO_MEMORY;
    struct aspace *self = process_aspace(process_current());
    int64_t n = self ? sys_vmar_resident(t, self, vmar, addr, len, buf) : ERR_BAD_STATE;
    if (self)
        aspace_unref(self);
    if (n > 0 && copy_out(out, buf, (size_t)n) != OK)
        n = ERR_INVALID_ARGS;
    kfree(buf);
    return n;
}

/* The address space's clone (docs/FORK-PLAN.md section 1.1; abi/syscalls.def
 * has the contract). The children's slots are reserved and their values
 * written out before the copy, so nothing can fail after it. */
int64_t sysc_vmar_clone(handle_t dst, uint64_t vmos, uint64_t children, uint32_t n,
                        uint32_t flags)
{
    SYSC_TABLE(t);
    if (flags)
        return ERR_INVALID_ARGS;
    if (n > VMAR_CLONE_MAX)
        return ERR_OUT_OF_RANGE;
    handle_t *kv = n ? kmalloc(2 * (size_t)n * sizeof(handle_t)) : NULL;
    if (n && !kv)
        return ERR_NO_MEMORY;
    handle_t *kc = kv ? kv + n : NULL;
    status_t st = copy_in(kv, vmos, (size_t)n * sizeof(handle_t)) != OK ? ERR_INVALID_ARGS : OK;
    if (st == OK && n)
        st = handle_reserve(t, n, kc);
    if (st == OK && copy_out(children, kc, (size_t)n * sizeof(handle_t)) != OK) {
        for (uint32_t i = 0; i < n; i++)
            handle_commit(t, kc[i]);
        st = ERR_INVALID_ARGS;
    }
    if (st == OK) {
        struct aspace *src = process_aspace(process_current());
        st = sys_vmar_clone(t, src, dst, kv, kc, n);
        if (src)
            aspace_unref(src);
    }
    kfree(kv);
    return st;
}
