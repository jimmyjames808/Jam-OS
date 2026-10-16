/* Pager operations on handles (the system calls' sys_* layer,
 * <jam/pager.h>). Making a pager needs RIGHT_ROOT_PAGER on the root: a
 * pager VMO makes whoever touches it wait for the pager, so only the
 * filesystems the system trusts (volumes' fat) may make them. Reading
 * requests needs RIGHT_READ on the pager; making VMOs and answering,
 * RIGHT_WRITE. A pager VMO's handle never carries RIGHT_WRITE,
 * RIGHT_RESIZE or RIGHT_EXEC (PAGER_VMO_RIGHTS). */
#include <jam/handle.h>
#include <jam/pager.h>
#include <jam/sysinfo.h>
#include <jam/vmo.h>

/* On success *pg holds a reference: kobject_unref it. */
static status_t pager_get(struct handle_table *t, handle_t h, rights_t need, struct pager **pg)
{
    struct kobject *obj;
    status_t st = handle_get(t, h, OBJ_PAGER, need, &obj, NULL);
    if (st == OK)
        *pg = pager_from_kobject(obj);
    return st;
}

/* A new object (the caller's only reference) into t: on a failure it is
 * gone. */
static status_t insert(struct handle_table *t, struct kobject *obj, rights_t rights, handle_t *out)
{
    struct khandle kh = khandle_from_new(obj, rights);
    status_t st = handle_insert(t, &kh, out);
    if (st != OK)
        khandle_release(&kh);
    return st;
}

status_t sys_pager_create(struct handle_table *t, handle_t root, uint64_t max_pages,
                          uint64_t answer_ns, uint32_t flags, handle_t *out)
{
    status_t st = sysinfo_check_root(t, root, RIGHT_ROOT_PAGER);
    if (st != OK)
        return st;
    if (flags)
        return ERR_INVALID_ARGS;
    struct pager *pg;
    if ((st = pager_create(t->job, max_pages, answer_ns, &pg)) != OK)
        return st;
    return insert(t, pager_kobject(pg), PAGER_RIGHTS, out);
}

status_t sys_pager_create_vmo(struct handle_table *t, handle_t pager, uint64_t key, uint64_t size,
                              uint32_t flags, handle_t *out)
{
    struct pager *pg;
    status_t st = pager_get(t, pager, RIGHT_WRITE, &pg);
    if (st != OK)
        return st;
    struct vmo *v;
    st = flags & ~(PAGER_VMO_WRITABLE | PAGER_VMO_FIND) ? ERR_INVALID_ARGS
         : flags                                         ? ERR_NOT_SUPPORTED   /* (WK1) */
                                                         : pager_create_vmo(pg, key, size, &v);
    kobject_unref(pager_kobject(pg));
    if (st != OK)
        return st;
    return insert(t, vmo_kobject(v), PAGER_VMO_RIGHTS, out);
}

status_t sys_pager_next(struct handle_table *t, handle_t pager, uint32_t flags,
                        struct pager_request *out)
{
    struct pager *pg;
    status_t st = pager_get(t, pager, RIGHT_READ, &pg);
    if (st != OK)
        return st;
    st = pager_next(pg, flags, out);
    kobject_unref(pager_kobject(pg));
    return st;
}

status_t sys_pager_supply(struct handle_table *t, handle_t pager, uint64_t key, uint64_t offset,
                          const struct pager_src *src, uint64_t len)
{
    struct pager *pg;
    status_t st = pager_get(t, pager, RIGHT_WRITE, &pg);
    if (st != OK)
        return st;
    st = pager_supply(pg, key, offset, src, len);
    kobject_unref(pager_kobject(pg));
    return st;
}

status_t sys_pager_op(struct handle_table *t, handle_t pager, uint64_t key, uint32_t op,
                      uint64_t offset, uint64_t len, int64_t arg)
{
    struct pager *pg;
    status_t st = pager_get(t, pager, RIGHT_WRITE, &pg);
    if (st != OK)
        return st;
    st = pager_op(pg, key, op, offset, len, arg);
    kobject_unref(pager_kobject(pg));
    return st;
}
