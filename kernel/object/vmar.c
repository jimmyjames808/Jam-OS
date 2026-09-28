/* VMAR objects (see vmar.h). All the work is in mm/aspace.c; this only ties
 * an address space's lifetime to the object's references. Destroy runs from
 * object teardown (preemption off): aspace_unref never sleeps, so that is
 * fine, and the last reference to an address space can't still be loaded
 * on a CPU (threads hold their own references while they run on it). */
#include <jam/aspace.h>
#include <jam/mm.h>
#include <jam/vmar.h>

struct vmar {
    struct kobject base;
    struct aspace *as;
};

static void vmar_destroy(struct kobject *obj)
{
    struct vmar *v = (struct vmar *)obj;
    aspace_unref(v->as);
    kfree(v);
}

static const struct kobject_ops vmar_ops = {
    .name = "vmar",
    .destroy = vmar_destroy,
};

status_t vmar_create_for(struct aspace *as, struct vmar **out)
{
    struct vmar *v = kzalloc(sizeof(*v));
    if (!v)
        return ERR_NO_MEMORY;
    kobject_init(&v->base, OBJ_VMAR, &vmar_ops, "vmar", 0);
    aspace_ref(as);
    v->as = as;
    *out = v;
    return OK;
}

status_t vmar_create(struct vmar **out)
{
    struct aspace *as;
    status_t st = aspace_create(&as);
    if (st != OK)
        return st;
    st = vmar_create_for(as, out);
    aspace_unref(as);   /* the vmar's reference (if any) keeps it */
    return st;
}

struct aspace *vmar_aspace(struct vmar *v)
{
    return v->as;
}
