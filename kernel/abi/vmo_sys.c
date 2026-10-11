/* VMO operations on handles (the system calls' sys_* layer). Reading needs
 * RIGHT_READ; writing and committing RIGHT_WRITE; resizing and decommitting
 * RIGHT_WRITE | RIGHT_RESIZE, so a handle duplicated without RIGHT_RESIZE
 * changes a buffer's contents but never takes pages out from under someone
 * else's mapping of it. A VMO_KEEP_PAGES VMO refuses decommit and shrink
 * whatever the handle's rights (vmo.c), so the same holds for every handle
 * to it, its creator's included. */
#include <jam/dbghook.h>
#include <jam/handle.h>
#include <jam/mm.h>
#include <jam/pager.h>
#include <jam/resource.h>
#include <jam/sys.h>
#include <jam/sysinfo.h>
#include <jam/vmo.h>
#include <jam/vmo_verify.h>

#define VMO_RIGHTS  (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_MAP | RIGHT_RESIZE)
#define RESIZE_NEED (RIGHT_WRITE | RIGHT_RESIZE)

/* On success *v holds a reference: vmo_put it. */
static status_t vmo_get(struct handle_table *t, handle_t h, rights_t need, struct vmo **v)
{
    struct kobject *obj;
    status_t st = handle_get(t, h, OBJ_VMO, need, &obj, NULL);
    if (st == OK)
        *v = vmo_from_kobject(obj);
    return st;
}

static void vmo_put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

/* A VMO for t (flags already checked): its pages charged to t's job (none
 * for a kernel table), a VMO_KEEP_PAGES one's all at once, here. */
static status_t create(struct handle_table *t, uint64_t size, uint32_t flags, handle_t *out)
{
    struct vmo *v;
    status_t st = vmo_create_for(t->job, size, flags, &v);
    if (st != OK)
        return st;
    struct khandle kh = khandle_from_new(vmo_kobject(v), VMO_RIGHTS);
    st = handle_insert(t, &kh, out);
    if (st != OK)
        khandle_release(&kh);   /* destroys the VMO */
    return st;
}

status_t sys_vmo_create(struct handle_table *t, uint64_t size, uint32_t flags, handle_t *out)
{
    if (flags & ~VMO_KEEP_PAGES)
        return ERR_INVALID_ARGS;   /* VMO_CONTIGUOUS, VMO_DMA32: sys_dma_vmo_create */
    return create(t, size, flags, out);
}

/* VMO_CONTIGUOUS / VMO_DMA32 hand out scarce contiguous / sub-4 GiB
 * memory, so a caller of the sys layer must present a DMA capability
 * bound to a PCI function (a driver's; an unbound one is a kernel test's,
 * and pins nothing either) to ask for them. The kernel-internal vmo_create
 * stays unrestricted: kernel code is trusted to ask only for what it
 * needs. */
status_t sys_dma_vmo_create(struct handle_table *t, handle_t dma, uint64_t size, uint32_t flags,
                            handle_t *out)
{
    if (flags & ~(VMO_CONTIGUOUS | VMO_DMA32))
        return ERR_INVALID_ARGS;
    struct kobject *cap;
    status_t st = handle_get(t, dma, OBJ_DMA_CAP, 0, &cap, NULL);
    if (st != OK)
        return st;
    bool bound = dma_cap_device(cap) != NULL;
    kobject_unref(cap);
    return bound ? create(t, size, flags, out) : ERR_ACCESS_DENIED;
}

status_t sys_vmo_read(struct handle_table *t, handle_t h, uint64_t offset, void *buf,
                      uint64_t len)
{
    struct vmo *v;
    status_t st = vmo_get(t, h, RIGHT_READ, &v);
    if (st != OK)
        return st;
    st = vmo_read(v, offset, buf, len);
    vmo_put(v);
    return st;
}

status_t sys_vmo_write(struct handle_table *t, handle_t h, uint64_t offset, const void *buf,
                       uint64_t len)
{
    struct vmo *v;
    status_t st = vmo_get(t, h, RIGHT_WRITE, &v);
    if (st != OK)
        return st;
    st = vmo_write(v, offset, buf, len);
    vmo_put(v);
    return st;
}

status_t sys_vmo_get_size(struct handle_table *t, handle_t h, uint64_t *size)
{
    struct vmo *v;
    status_t st = vmo_get(t, h, 0, &v);
    if (st != OK)
        return st;
    *size = vmo_size(v);
    vmo_put(v);
    return OK;
}

status_t sys_vmo_set_size(struct handle_table *t, handle_t h, uint64_t size)
{
    struct vmo *v;
    status_t st = vmo_get(t, h, RESIZE_NEED, &v);
    if (st != OK)
        return st;
    st = vmo_set_size(v, size);
    vmo_put(v);
    return st;
}

status_t sys_vmo_commit(struct handle_table *t, handle_t h, uint64_t offset, uint64_t len)
{
    struct vmo *v;
    status_t st = vmo_get(t, h, RIGHT_WRITE, &v);
    if (st != OK)
        return st;
    st = vmo_commit(v, offset, len);
    vmo_put(v);
    return st;
}

/* The handle is taken out of the table, then the VMO is checked while
 * its one handle is out: it must be that one (no duplicate anywhere: one
 * made by another thread before the removal counts, and none can be made
 * from h after it), and vmo_seal must succeed (nothing maps or pins it,
 * no write is running). Only then does the handle go back, as one that
 * may execute and not write (nor resize): a duplicate a sibling thread
 * makes of it is of a sealed VMO (M12.1 kernel core, finding 4: with the
 * check after the insert, a duplicate made in between kept RIGHT_EXEC on
 * a VMO the failed call never sealed). The seal covers a call that took
 * its reference before the removal (a vmo_write or a writable vmar_map
 * from another thread, past its handle_get): the VMO itself refuses it
 * from then on. */
status_t sys_vmo_make_exec(struct handle_table *t, handle_t h, handle_t root, handle_t *out)
{
    /* h is gone whatever the status (M12's K-C, as a send's handles). */
    struct vmo *v;
    status_t st = sysinfo_check_root(t, root, RIGHT_ROOT_VMEX);
    if (st == OK)
        st = vmo_get(t, h, RIGHT_READ | RIGHT_MAP, &v);
    if (st != OK) {
        handle_close(t, h);   /* a bad value names nothing */
        return st;
    }
    struct khandle kh = { NULL, 0 };
    st = handle_remove(t, h, &kh);
    DBG_HOOK(DBG_VMO_EXEC_CHECK, v);
    if (st == OK && (kobject_handles(vmo_kobject(v)) != 1 || vmo_seal(v) != OK))
        st = ERR_BAD_STATE;
    if (st == OK) {
        kh.rights = (kh.rights & ~(RIGHT_WRITE | RIGHT_RESIZE)) | RIGHT_EXEC;
        st = handle_insert(t, &kh, out);
    }
    khandle_release(&kh);   /* a no-op once inserted; else h is closed */
    vmo_put(v);
    return st;
}

/* The table for v from the VMO behind handle `table` (RIGHT_READ), its
 * pages charged to t's job. */
static status_t copy_table(struct handle_table *t, handle_t table, struct vmo *v,
                           struct vmo_verify **out)
{
    struct vmo *tv;
    status_t st = vmo_get(t, table, RIGHT_READ, &tv);
    if (st != OK)
        return st;
    st = vmo_verify_new(t->job, tv, vmo_size(v) >> PAGE_SHIFT, out);
    vmo_put(tv);
    return st;
}

/* sys_vmo_make_exec for a pager VMO, sealed with a table of its pages'
 * hashes (<jam/vmo_verify.h>) instead of as it is. The table is copied in
 * first, with the handle still in the table and no lock held (a table
 * that is a pager VMO itself waits for its pages); then the handle comes
 * out, the VMO is checked and sealed, and the handle goes back with
 * RIGHT_EXEC, in sys_vmo_make_exec's order and for its reasons. */
status_t sys_vmo_make_exec_verified(struct handle_table *t, handle_t h, handle_t root,
                                    handle_t table, uint32_t flags, handle_t *out)
{
    /* h is gone whatever the status, as for vmo_make_exec. */
    struct vmo *v = NULL;
    struct vmo_verify *vf = NULL;
    status_t st = flags ? ERR_INVALID_ARGS : sysinfo_check_root(t, root, RIGHT_ROOT_VMEX);
    if (st == OK)
        st = vmo_get(t, h, RIGHT_READ | RIGHT_MAP, &v);
    if (st == OK && !vmo_is_pager(v))
        st = ERR_NOT_SUPPORTED;   /* a VMO of its own: vmo_make_exec */
    if (st == OK)
        st = copy_table(t, table, v, &vf);
    if (st != OK) {
        if (v)
            vmo_put(v);
        handle_close(t, h);   /* a bad value names nothing */
        return st;
    }
    struct khandle kh = { NULL, 0 };
    st = handle_remove(t, h, &kh);
    DBG_HOOK(DBG_VMO_EXEC_CHECK, v);
    if (st == OK && kobject_handles(vmo_kobject(v)) != 1)
        st = ERR_BAD_STATE;
    if (st == OK && (st = vmo_seal_verified(v, vf)) == OK) {
        vf = NULL;   /* v's now */
        kh.rights = (kh.rights & ~(RIGHT_WRITE | RIGHT_RESIZE)) | RIGHT_EXEC;
        st = handle_insert(t, &kh, out);
    }
    khandle_release(&kh);   /* a no-op once inserted; else h is closed */
    if (vf)
        vmo_verify_free(vf);
    vmo_put(v);
    return st;
}

status_t sys_vmo_decommit(struct handle_table *t, handle_t h, uint64_t offset, uint64_t len)
{
    struct vmo *v;
    status_t st = vmo_get(t, h, RESIZE_NEED, &v);
    if (st != OK)
        return st;
    st = vmo_decommit(v, offset, len);
    vmo_put(v);
    return st;
}
