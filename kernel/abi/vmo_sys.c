/* VMO operations on handles (the M5 system calls). */
#include <jam/handle.h>
#include <jam/sys.h>
#include <jam/vmo.h>

#define VMO_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_MAP)

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
     * ask for them. The kernel-internal vmo_create stays unrestricted for
     * driver use. (O3b) */
    if (flags & (VMO_CONTIGUOUS | VMO_DMA32)) {
        struct kobject *cap;
        if (handle_get(t, dma_cap, OBJ_DMA_CAP, 0, &cap, NULL) != OK)
            return ERR_ACCESS_DENIED;
        kobject_unref(cap);
    }
    struct vmo *v;
    status_t st = vmo_create(size, flags, &v);
    if (st != OK)
        return st;
    /* Its pages are charged to the creating table's job (M5; none for a
     * kernel table). */
    st = vmo_set_job(v, t->job);
    if (st != OK) {
        kobject_unref(vmo_kobject(v));
        return st;
    }
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
    status_t st = vmo_get(t, h, RIGHT_WRITE, &v);
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

status_t sys_vmo_decommit(struct handle_table *t, handle_t h, uint64_t offset, uint64_t len)
{
    struct vmo *v;
    status_t st = vmo_get(t, h, RIGHT_WRITE, &v);
    if (st != OK)
        return st;
    st = vmo_decommit(v, offset, len);
    vmo_put(v);
    return st;
}
