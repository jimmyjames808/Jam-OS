/* VMAR operations on handles (the M5 system calls). Mapping needs
 * RIGHT_WRITE on the vmar and, on the VMO handle, RIGHT_MAP plus the right
 * behind each permission asked for (RIGHT_READ, RIGHT_WRITE, RIGHT_EXEC).
 * The VMO handle's rights are also recorded as the most a later protect
 * may grant, so a mapping can never gain a permission the handle lacked. */
#include <jam/aspace.h>
#include <jam/handle.h>
#include <jam/sys.h>
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
    if (flags & ~(USER_PERMS | ASPACE_FIXED))
        return ERR_INVALID_ARGS;   /* the CAN bits come from the handle, not the caller */
    if ((flags & ASPACE_WRITE) && (flags & ASPACE_EXEC))
        return ERR_INVALID_ARGS;   /* W^X is an argument error whatever the rights */
    struct aspace *as;
    struct kobject *vref, *oref;
    status_t st = vmar_get(t, vmar, RIGHT_WRITE, &as, &vref);
    if (st != OK)
        return st;
    rights_t rights;
    st = handle_get(t, vmo, OBJ_VMO, RIGHT_MAP | perm_rights(flags), &oref, &rights);
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
