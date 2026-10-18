/* VMAR operations on handles (the system calls' sys_* layer). Mapping needs
 * RIGHT_WRITE on the vmar and, on the VMO handle, RIGHT_MAP plus the right
 * behind each permission asked for (RIGHT_READ, RIGHT_WRITE, RIGHT_EXEC).
 * Write together with execute is an argument error (W^X) unless the VMO is
 * a JIT VMO (vmo_make_jit); aspace_map checks that again.
 * The VMO handle's rights are also recorded as the most a later protect
 * may grant, so a mapping can never gain a permission the handle lacked.
 * ASPACE_KEPT_ONLY (VMAR_KEPT_ONLY) and ASPACE_REPLACE (VMAR_REPLACE) go
 * straight to aspace_map, which checks the VMO's kind and fills the
 * mapping, or replaces what the range held; so do the clone modes
 * (VMAR_CLONE_SHARE, VMAR_CLONE_NONE), which aspace_map decides with the
 * maximum set here. vmar_clone names its destination by a process handle
 * (RIGHT_WRITE), not a vmar: only the process can say it hasn't started,
 * and hold that while the copy is made (process_clone_begin). */
#include <jam/aspace.h>
#include <jam/handle.h>
#include <jam/mm.h>
#include <jam/process.h>
#include <jam/sys.h>
#include <jam/uentry.h>
#include <jam/vmar.h>
#include <jam/vmo.h>

#define VMAR_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE)
#define USER_PERMS  (ASPACE_READ | ASPACE_WRITE | ASPACE_EXEC)

/* On success *as is the vmar's address space and *ref the reference that
 * keeps it alive: kobject_unref it. */
static status_t vmar_get(struct handle_table *t, handle_t h, rights_t need, struct aspace **as,
                         struct kobject **ref)
{
    struct kobject *obj;
    status_t st = handle_get(t, h, OBJ_VMAR, need, &obj, NULL);
    if (st != OK)
        return st;
    *as = vmar_aspace(vmar_from_kobject(obj));
    *ref = obj;
    return OK;
}

static rights_t perm_rights(unsigned perms)
{
    return ((perms & ASPACE_READ) ? RIGHT_READ : 0) | ((perms & ASPACE_WRITE) ? RIGHT_WRITE : 0) |
           ((perms & ASPACE_EXEC) ? RIGHT_EXEC : 0);
}

status_t sys_vmar_create(struct handle_table *t, handle_t *out)
{
    struct vmar *v;
    status_t st = vmar_create(&v);
    if (st != OK)
        return st;
    struct khandle kh = khandle_from_new(vmar_kobject(v), VMAR_RIGHTS);
    st = handle_insert(t, &kh, out);
    if (st != OK)
        khandle_release(&kh);   /* destroys the vmar and its address space */
    return st;
}

status_t sys_vmar_map(struct handle_table *t, handle_t vmar, handle_t vmo, uint64_t vmo_off,
                      uint64_t len, uint32_t flags, uint64_t *addr)
{
    if (flags & ~(USER_PERMS | ASPACE_FIXED | ASPACE_KEPT_ONLY | ASPACE_REPLACE |
                  ASPACE_CLONE_SHARE | ASPACE_CLONE_NONE))
        return ERR_INVALID_ARGS;   /* the CAN bits come from the handle, not the caller */
    bool wx = (flags & ASPACE_WRITE) && (flags & ASPACE_EXEC);
    struct aspace *as;
    struct kobject *vref, *oref;
    status_t st = vmar_get(t, vmar, RIGHT_WRITE, &as, &vref);
    if (st != OK)
        return st;
    rights_t rights;
    st = handle_get(t, vmo, OBJ_VMO, 0, &oref, &rights);
    if (st == OK) {
        /* W^X is an argument error whatever the rights, but on a JIT VMO. */
        if (wx && !vmo_is_jit(vmo_from_kobject(oref)))
            st = ERR_INVALID_ARGS;
        else if ((RIGHT_MAP | perm_rights(flags)) & ~rights)
            st = ERR_ACCESS_DENIED;
        if (st != OK)
            kobject_unref(oref);
    }
    if (st == OK) {
        unsigned can = ((rights & RIGHT_READ) ? ASPACE_CAN_READ : 0) |
                       ((rights & RIGHT_WRITE) ? ASPACE_CAN_WRITE : 0) |
                       ((rights & RIGHT_EXEC) ? ASPACE_CAN_EXEC : 0);
        st = aspace_map(as, vmo_from_kobject(oref), vmo_off, len, flags | can, addr);
        kobject_unref(oref);
    }
    kobject_unref(vref);
    return st;
}

status_t sys_vmar_unmap(struct handle_table *t, handle_t vmar, uint64_t addr, uint64_t len)
{
    struct aspace *as;
    struct kobject *ref;
    status_t st = vmar_get(t, vmar, RIGHT_WRITE, &as, &ref);
    if (st != OK)
        return st;
    st = aspace_unmap(as, addr, len);
    kobject_unref(ref);
    return st;
}

status_t sys_vmar_protect(struct handle_table *t, handle_t vmar, uint64_t addr, uint64_t len,
                          uint32_t flags)
{
    if (flags & ~USER_PERMS)
        return ERR_INVALID_ARGS;
    struct aspace *as;
    struct kobject *ref;
    status_t st = vmar_get(t, vmar, RIGHT_WRITE, &as, &ref);
    if (st != OK)
        return st;
    st = aspace_protect(as, addr, len, flags);   /* ERR_ACCESS_DENIED past the map-time rights */
    kobject_unref(ref);
    return st;
}

int64_t sys_vmar_resident(struct handle_table *t, struct aspace *self, handle_t vmar,
                          uint64_t addr, uint64_t len, uint8_t *out)
{
    if (!len || ((addr | len) & (PAGE_SIZE - 1)))
        return ERR_INVALID_ARGS;
    if (len / PAGE_SIZE > VMAR_RESIDENT_MAX || !user_range_ok(addr, len))
        return ERR_OUT_OF_RANGE;
    struct aspace *as;
    struct kobject *ref;
    status_t st = vmar_get(t, vmar, RIGHT_READ, &as, &ref);
    if (st != OK)
        return st;
    /* Only the caller's own: a parent's handle of its child's address
     * space must not tell which of the child's pages it touched. */
    int64_t n = as == self ? (int64_t)aspace_resident(as, addr, len / PAGE_SIZE, out)
                           : ERR_ACCESS_DENIED;
    kobject_unref(ref);
    return n;
}

/* The VMOs first (each a reference, in r), then dst held NEW
 * (process_clone_begin) for the copy, then the reserved slots filled or
 * given back: nothing that can fail comes after the copy. */
status_t sys_vmar_clone(struct handle_table *t, struct aspace *src, handle_t dst,
                        const handle_t *vmos, const handle_t *children, uint32_t n)
{
    struct aspace_clone_req *r = n ? kmalloc((size_t)n * sizeof(*r)) : NULL;
    status_t st = !src ? ERR_BAD_STATE : n && !r ? ERR_NO_MEMORY : OK;
    uint32_t got = 0;
    for (; st == OK && got < n; got++) {
        struct kobject *o;
        st = handle_get(t, vmos[got], OBJ_VMO, RIGHT_READ | RIGHT_DUPLICATE, &o, NULL);
        if (st != OK)
            break;
        r[got] = (struct aspace_clone_req){ .v = vmo_from_kobject(o), .index = got };
    }
    struct kobject *po = NULL;
    if (st == OK)
        st = handle_get(t, dst, OBJ_PROCESS, RIGHT_WRITE, &po, NULL);
    struct aspace *das;
    if (st == OK && (st = process_clone_begin(process_from_kobject(po), &das)) == OK) {
        st = aspace_clone(src, das, r, n);
        process_clone_end(process_from_kobject(po), das, st == OK);
    }
    if (po)
        kobject_unref(po);
    for (uint32_t i = 0; i < n; i++) {
        if (st != OK) {
            handle_commit(t, children[i]);   /* given back unused */
            continue;
        }
        struct khandle kh = khandle_from_new(vmo_kobject(r[i].child), VMO_RIGHTS);
        handle_t h;
        if (handle_untake(t, children[r[i].index], &kh, &h) != OK)
            khandle_release(&kh);   /* not a slot we reserved: can't happen */
    }
    for (uint32_t i = 0; i < got; i++)
        kobject_unref(vmo_kobject(r[i].v));
    kfree(r);
    return st;
}
