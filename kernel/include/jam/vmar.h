/* VMARs: a handle to an address space (jam/aspace.h). There is one flat
 * address space per process and no nested regions, so a vmar is just the
 * object that carries rights to map into, unmap from and protect it. The
 * vmar holds a reference on its address space. */
#pragma once

#include <jam/object.h>
#include <jam/status.h>

struct aspace;

/* Embeds struct kobject first (type OBJ_VMAR), so the casts below are safe. */
struct vmar;

static inline struct kobject *vmar_kobject(struct vmar *v) { return (struct kobject *)v; }
/* NULL unless obj is a vmar. */
static inline struct vmar *vmar_from_kobject(struct kobject *obj)
{
    return obj && obj->type == OBJ_VMAR ? (struct vmar *)obj : NULL;
}

/* A vmar over a new, empty address space. The caller gets the only
 * reference. */
status_t vmar_create(struct vmar **out);
/* A vmar over an existing address space (takes its own reference): for
 * handing a process its own address space. */
status_t vmar_create_for(struct aspace *as, struct vmar **out);
/* The address space (no new reference: valid while the vmar is). */
struct aspace *vmar_aspace(struct vmar *v);
