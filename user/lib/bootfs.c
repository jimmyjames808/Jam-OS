/* Reading files from bootfs in user space: map the image VMO (SR_BOOTFS)
 * read-only and look names up in its entry table (format in
 * <jam/bootfs.h>). The kernel validated the image at boot; the checks here
 * only make sure a lookup can never point outside the mapping. */
#include <jam/bootfs.h>
#include <os.h>


status_t bootfs_open(handle_t vmo, struct bootfs_view *out)
{
    handle_t vmar = startup_handle(SR_SELF_VMAR);
    uint64_t size, addr = 0;
    if (vmar == HANDLE_INVALID)
        return ERR_BAD_HANDLE;
    status_t st = jam_vmo_get_size(vmo, &size);
    if (st != OK)
        return st;
    if (size < sizeof(struct bootfs_header) || (size & (PAGE_SIZE - 1)))
        return ERR_INVALID_ARGS;
    st = jam_vmar_map(vmar, vmo, 0, size, VMAR_READ, &addr);
    if (st != OK)
        return st;

    const struct bootfs_header *h = (const struct bootfs_header *)(uintptr_t)addr;
    if (memcmp(h->magic, BOOTFS_MAGIC, 8) || h->version != BOOTFS_VERSION ||
        h->size > size ||
        h->count > (h->size - sizeof(*h)) / sizeof(struct bootfs_entry)) {
        jam_vmar_unmap(vmar, addr, size);
        return ERR_INVALID_ARGS;
    }
    out->base = (const uint8_t *)(uintptr_t)addr;
    out->size = h->size;
    out->count = h->count;
    return OK;
}

status_t bootfs_default(const struct bootfs_view **out)
{
    static struct bootfs_view view;
    static int state;   /* 0 not tried, 1 mapped, -1 failed */
    static status_t why;
    if (!state) {
        why = bootfs_open(startup_handle(SR_BOOTFS), &view);
        state = why == OK ? 1 : -1;
    }
    *out = &view;
    return state > 0 ? OK : why;
}

status_t bootfs_lookup(const struct bootfs_view *fs, const char *name, const void **data,
                       uint64_t *size)
{
    const struct bootfs_entry *e =
        (const struct bootfs_entry *)(fs->base + sizeof(struct bootfs_header));
    for (uint32_t i = 0; i < fs->count; i++, e++) {
        if (strnlen(e->name, BOOTFS_NAME_MAX) == BOOTFS_NAME_MAX ||
            strcmp(e->name, name) != 0)
            continue;
        if (e->offset > fs->size || e->size > fs->size - e->offset)
            return ERR_INVALID_ARGS;
        *data = fs->base + e->offset;
        *size = e->size;
        return OK;
    }
    return ERR_NOT_FOUND;
}
