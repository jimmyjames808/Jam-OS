/* VMO operations on handles (the system calls' sys_* layer). Reading needs
 * RIGHT_READ; writing and committing RIGHT_WRITE; resizing and decommitting
 * RIGHT_WRITE | RIGHT_RESIZE, so a handle duplicated without RIGHT_RESIZE
 * changes a buffer's contents but never takes pages out from under someone
 * else's mapping of it. A VMO_KEEP_PAGES VMO refuses decommit and shrink
 * whatever the handle's rights (vmo.c), so the same holds for every handle
 * to it, its creator's included. */
#include <jam/handle.h>
#include <jam/sys.h>
#include <jam/sysinfo.h>
#include <jam/vmo.h>

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

status_t sys_vmo_create(struct handle_table *t, uint64_t size, uint32_t flags,
                        handle_t dma_cap, handle_t *out)
{
    /* VMO_CONTIGUOUS / VMO_DMA32 hand out scarce contiguous / sub-4 GiB
     * memory, so a caller of the sys layer must present a DMA capability to
     * ask for them. The kernel-internal vmo_create stays unrestricted:
     * kernel code is trusted to ask only for what it needs. */
    if (flags & (VMO_CONTIGUOUS | VMO_DMA32)) {
        struct kobject *cap;
        if (handle_get(t, dma_cap, OBJ_DMA_CAP, 0, &cap, NULL) != OK)
            return ERR_ACCESS_DENIED;
        kobject_unref(cap);
    }
    /* Its pages are charged to the creating table's job (none for a
     * kernel table); a VMO_KEEP_PAGES one's all at once, here. */
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

/* The handle is swapped for one that may execute and not write (nor
 * resize), then the VMO is checked: it must be its one handle, and
 * vmo_seal must succeed (nothing maps or pins it, no write is running).
 * Checked after the swap, so a duplicate made meanwhile by another thread
 * counts as a second handle (refused) instead of slipping past with
 * RIGHT_WRITE. The seal covers a call that took its reference before the
 * swap (a vmo_write or a writable vmar_map from another thread, past its
 * handle_get): the VMO itself refuses it from then on. */
status_t sys_vmo_make_exec(struct handle_table *t, handle_t h, handle_t root, handle_t *out)
{
    status_t st = sysinfo_check_root(t, root, RIGHT_ROOT_VMEX);
    if (st != OK)
        return st;
    struct vmo *v;
    st = vmo_get(t, h, RIGHT_READ | RIGHT_MAP, &v);
    if (st != OK)
        return st;
    struct khandle kh;
    st = handle_remove(t, h, &kh);
    if (st == OK) {
        kh.rights = (kh.rights & ~(RIGHT_WRITE | RIGHT_RESIZE)) | RIGHT_EXEC;
        st = handle_insert(t, &kh, out);
        if (st != OK)
            khandle_release(&kh);
    }
    if (st == OK && (kobject_handles(vmo_kobject(v)) != 1 || vmo_seal(v) != OK)) {
        handle_close(t, *out);
        st = ERR_BAD_STATE;
    }
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
