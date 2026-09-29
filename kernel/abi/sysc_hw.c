/* System calls: hardware (M6, Track C): resources, PCI config access, MMIO
 * VMOs and DMA. The rules all sysc_* follow are in sysc.h; the sys_* half
 * works on a handle table with kernel pointers so kernel tests can drive
 * it (<jam/resource_impl.h>).
 *
 * Rights:
 *   resource_create        RIGHT_SLICE on the parent
 *   pci_enum               RIGHT_INSPECT on a RES_PCI
 *   pci_device_open        RIGHT_SLICE on a RES_PCI
 *   pci_config_read        RIGHT_READ on a RES_PCI_DEV
 *   pci_config_write       RIGHT_WRITE on a RES_PCI_DEV (+ the filter; with
 *                          RIGHT_MANAGE too, the PM power state may change)
 *   pci_bar_resource       RIGHT_SLICE on a RES_PCI_DEV
 *   pci_bus_master         RIGHT_MANAGE on a RES_PCI_DEV, and only to turn
 *                          it OFF (M7: on is the current dma_cap's)
 *   dma_cap_create         RIGHT_MANAGE on a RES_PCI_DEV (devmgr's copy)
 *   dma_cap_bus_master     a dma_cap handle (any rights): its function's
 *                          current cap (M7)
 *   vmo_create_physical    RIGHT_MAP on a RES_ROOT / RES_MMIO
 *   vmo_pin / vmo_unpin    RIGHT_WRITE on the VMO; a bound dma_cap (unpin:
 *                          the one the pin was made with)
 * A resource made from a handle gets that handle's rights (masked to
 * RES_RIGHTS), so a device handle without RIGHT_MANAGE only yields BAR
 * resources without it; a physical VMO made from a resource handle
 * without RIGHT_DUPLICATE / RIGHT_TRANSFER lacks them too (M7). Every new resource and dma_cap costs the caller's
 * job one JOB_LIMIT_HANDLES unit (on top of the handle slot). */
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/syscall_impl.h>
#include <jam/vmo.h>
#include "sysc.h"

#define PHYS_VMO_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_MAP)
#define VMO_HANDLE_RIGHTS_PIN RIGHT_WRITE

/* Insert a fresh object (refs = 1) into t; on failure it is gone. */
static status_t insert_new(struct handle_table *t, struct kobject *obj, rights_t rights,
                           handle_t *out)
{
    struct khandle kh = khandle_from_new(obj, rights);
    status_t st = handle_insert(t, &kh, out);
    if (st != OK)
        khandle_release(&kh);
    return st;
}

/* A fresh resource: charge the table's job, then insert it. */
static status_t publish_res(struct handle_table *t, struct kobject *res, rights_t rights,
                            handle_t *out)
{
    status_t st = resource_set_job(res, t->job);
    if (st != OK) {
        kobject_unref(res);
        return st;
    }
    return insert_new(t, res, rights & RES_RIGHTS, out);
}

static status_t get_res(struct handle_table *t, handle_t h, rights_t need, struct kobject **obj,
                        rights_t *rights)
{
    return handle_get(t, h, OBJ_RESOURCE, need, obj, rights);
}

/* A RES_PCI_DEV handle's function; *obj holds a reference. */
static status_t get_dev(struct handle_table *t, handle_t h, rights_t need, struct kobject **obj,
                        struct pci_dev **d)
{
    status_t st = get_res(t, h, need, obj, NULL);
    if (st != OK)
        return st;
    if (!(*d = resource_pci_dev(*obj))) {
        kobject_unref(*obj);
        return ERR_WRONG_TYPE;
    }
    return OK;
}

status_t sys_resource_create(struct handle_table *t, handle_t parent, uint32_t kind,
                             uint64_t base, uint64_t size, handle_t *out)
{
    struct kobject *p, *res;
    rights_t r;
    status_t st = get_res(t, parent, RIGHT_SLICE, &p, &r);
    if (st != OK)
        return st;
    st = resource_create(p, kind, base, size, &res);
    kobject_unref(p);
    return st == OK ? publish_res(t, res, r, out) : st;
}

status_t sys_pci_enum(struct handle_table *t, handle_t pci, uint32_t index,
                      struct pci_dev_info *out)
{
    struct kobject *p;
    status_t st = get_res(t, pci, RIGHT_INSPECT, &p, NULL);
    if (st != OK)
        return st;
    struct pci_dev *d = NULL;
    if (resource_kind(p) != RES_PCI)
        st = ERR_WRONG_TYPE;
    else if (index >= pci_count() || !(d = pci_get(index)))
        st = ERR_OUT_OF_RANGE;
    else {
        *out = d->info;
        struct dma_quarantine_stats q;
        dma_quarantine_stats(d, &q);
        out->dma_quarantined = (uint32_t)q.pages;
        out->dma_changed = (uint32_t)q.changed;
    }
    kobject_unref(p);
    return st;
}

status_t sys_pci_device_open(struct handle_table *t, handle_t pci, uint32_t index, handle_t *out)
{
    struct kobject *p, *dev;
    rights_t r;
    status_t st = get_res(t, pci, RIGHT_SLICE, &p, &r);
    if (st != OK)
        return st;
    st = resource_pci_device(p, index, &dev);
    kobject_unref(p);
    return st == OK ? publish_res(t, dev, r, out) : st;
}

status_t sys_pci_config_read(struct handle_table *t, handle_t dev, uint32_t off, uint32_t width,
                             uint32_t *value)
{
    struct kobject *obj;
    struct pci_dev *d;
    status_t st = get_dev(t, dev, RIGHT_READ, &obj, &d);
    if (st != OK)
        return st;
    st = pci_cfg_access_ok(off, width);
    if (st == OK)
        *value = pci_cfg_read(d, off, width);
    kobject_unref(obj);
    return st;
}

status_t sys_pci_config_write(struct handle_table *t, handle_t dev, uint32_t off, uint32_t width,
                              uint32_t value)
{
    struct kobject *obj;
    struct pci_dev *d;
    rights_t r;
    status_t st = get_res(t, dev, RIGHT_WRITE, &obj, &r);
    if (st != OK)
        return st;
    if (!(d = resource_pci_dev(obj))) {
        kobject_unref(obj);
        return ERR_WRONG_TYPE;
    }
    bool manage = r & RIGHT_MANAGE, power = false;
    struct pci_saved_config saved;
    st = pci_cfg_access_ok(off, width);
    if (st == OK) {
        /* Check and write under the command lock: the command register's
         * kernel bits can't change between the two. */
        uint64_t f = pci_cmd_lock();
        st = pci_cfg_write_allowed_as(d, off, width, value, pci_cfg_read, manage);
        if (st == OK && manage &&
            (power = pci_cfg_write_changes_power(d, off, width, value, pci_cfg_read)))
            pci_save_config(d, &saved);
        if (st == OK)
            pci_cfg_write(d, off, width, value);
        pci_cmd_unlock(f);
    }
    if (power) {
        /* A power-state change (devmgr waking a function, M7): the
         * function may not be touched for 10 ms (PCI PM 1.2, D3hot -> D0
         * recovery; the longest), and D3hot -> D0 resets it unless it has
         * No_Soft_Reset: put back the BARs and command register. */
        thread_sleep_ms(10);
        uint64_t f = pci_cmd_lock();
        if (pci_restore_config(d, &saved))
            kprintf("pci: %02x:%02x.%x: reset by its power-state change; BARs and command "
                    "register restored\n", d->info.bus, d->info.dev, d->info.fn);
        pci_cmd_unlock(f);
    }
    kobject_unref(obj);
    return st;
}

status_t sys_pci_bar_resource(struct handle_table *t, handle_t dev, uint32_t bar, handle_t *out)
{
    struct kobject *obj, *res;
    rights_t r;
    status_t st = get_res(t, dev, RIGHT_SLICE, &obj, &r);
    if (st != OK)
        return st;
    st = resource_pci_bar(obj, bar, &res);
    if (st == OK) {
        /* A BAR is only useful with memory decode on (firmware usually left
         * it on; BAR sizing restored it). The kernel owns that bit, so it
         * turns it on here; the display and bridges are refused inside. */
        uint64_t f = pci_cmd_lock();
        (void)pci_enable_memory(resource_pci_dev(obj));
        pci_cmd_unlock(f);
    }
    kobject_unref(obj);
    return st == OK ? publish_res(t, res, r, out) : st;
}

status_t sys_pci_bus_master(struct handle_table *t, handle_t dev, uint32_t enable)
{
    struct kobject *obj;
    struct pci_dev *d;
    status_t st = get_dev(t, dev, RIGHT_MANAGE, &obj, &d);
    if (st != OK)
        return st;
    if (enable) {
        /* M7: only the function's current dma_cap turns it on, once its
         * driver has quiesced the device (dma_cap_bus_master). */
        kobject_unref(obj);
        return ERR_ACCESS_DENIED;
    }
    uint64_t f = pci_cmd_lock();
    st = pci_set_bus_master(d, false);
    pci_cmd_unlock(f);
    kobject_unref(obj);
    return st;
}

status_t sys_dma_cap_bus_master(struct handle_table *t, handle_t dma, uint32_t on)
{
    struct kobject *cap;
    status_t st = handle_get(t, dma, OBJ_DMA_CAP, 0, &cap, NULL);
    if (st != OK)
        return st;
    st = dma_cap_bus_master(cap, on != 0);
    kobject_unref(cap);
    return st;
}

status_t sys_vmo_create_physical(struct handle_table *t, handle_t res, uint64_t offset,
                                 uint64_t size, uint32_t cache, handle_t *out)
{
    unsigned vm_cache;
    switch (cache) {
    case VMO_CACHE_WB: vm_cache = 0; break;
    case VMO_CACHE_UC: vm_cache = VM_UC; break;
    case VMO_CACHE_WC: vm_cache = VM_WC; break;
    default: return ERR_INVALID_ARGS;
    }
    if (size == 0 || ((offset | size) & (PAGE_SIZE - 1)))
        return ERR_INVALID_ARGS;
    struct kobject *obj;
    rights_t rr;
    status_t st = get_res(t, res, RIGHT_MAP, &obj, &rr);
    if (st != OK)
        return st;
    uint64_t base, rsize;
    st = resource_range(obj, &base, &rsize);
    if (st == OK && (offset > rsize || size > rsize - offset))
        st = ERR_OUT_OF_RANGE;
    if (st == OK)
        st = resource_check_mmio(obj, base + offset, size);
    kobject_unref(obj);
    if (st == OK)
        st = resource_phys_mappable(base + offset, size);
    if (st != OK)
        return st;

    struct vmo *v;
    st = vmo_create_physical(base + offset, size, vm_cache, &v);
    if (st != OK)
        return st;
    st = vmo_set_job(v, t->job);   /* the struct only: it owns no pages */
    if (st != OK) {
        kobject_unref(vmo_kobject(v));
        return st;
    }
    /* M7: registers from a resource that can't be passed on can't be
     * passed on as a VMO either (a driver's BAR: review of M6 phase 2,
     * finding 2). */
    rights_t keep = PHYS_VMO_RIGHTS & ~((RIGHT_DUPLICATE | RIGHT_TRANSFER) & ~rr);
    return insert_new(t, vmo_kobject(v), keep, out);
}

status_t sys_dma_cap_create(struct handle_table *t, handle_t dev, handle_t *out)
{
    struct kobject *obj, *cap;
    struct pci_dev *d;
    status_t st = get_dev(t, dev, RIGHT_MANAGE, &obj, &d);
    if (st != OK)
        return st;
    st = dma_cap_create_for(d, &cap);
    kobject_unref(obj);
    if (st != OK)
        return st;
    st = dma_cap_set_job(cap, t->job);
    if (st != OK) {
        kobject_unref(cap);
        return st;
    }
    return insert_new(t, cap, DMA_CAP_RIGHTS, out);
}

/* vmo_pin on handles; on success *keep / *keep_cap (if keep is non-NULL)
 * hold references on the VMO and the cap, so the pin can be undone even if
 * the handles go meanwhile. */
static status_t pin(struct handle_table *t, handle_t vmo, handle_t dma, uint64_t offset,
                    uint64_t len, uint64_t *addrs, uint64_t *pin_id, struct vmo **keep,
                    struct kobject **keep_cap)
{
    if (len == 0 || (len & (PAGE_SIZE - 1)))
        return ERR_INVALID_ARGS;
    if ((len >> PAGE_SHIFT) > VMO_PIN_SYS_MAX_PAGES)
        return ERR_OUT_OF_RANGE;
    struct kobject *vo, *cap;
    status_t st = handle_get(t, vmo, OBJ_VMO, VMO_HANDLE_RIGHTS_PIN, &vo, NULL);
    if (st != OK)
        return st;
    st = handle_get(t, dma, OBJ_DMA_CAP, 0, &cap, NULL);
    if (st != OK) {
        kobject_unref(vo);
        return st;
    }
    if (!dma_cap_device(cap))
        st = ERR_ACCESS_DENIED;   /* unbound caps are for kernel tests only */
    else
        st = vmo_pin(vmo_from_kobject(vo), cap, offset, len, addrs, len >> PAGE_SHIFT, pin_id);
    if (st == OK && keep) {
        *keep = vmo_from_kobject(vo);
        *keep_cap = cap;
    } else {
        kobject_unref(vo);
        kobject_unref(cap);
    }
    return st;
}

status_t sys_vmo_pin(struct handle_table *t, handle_t vmo, handle_t dma, uint64_t offset,
                     uint64_t len, uint64_t *addrs, uint64_t *pin_id)
{
    return pin(t, vmo, dma, offset, len, addrs, pin_id, NULL, NULL);
}

status_t sys_vmo_unpin(struct handle_table *t, handle_t vmo, handle_t dma, uint64_t pin_id)
{
    struct kobject *vo, *cap;
    status_t st = handle_get(t, vmo, OBJ_VMO, VMO_HANDLE_RIGHTS_PIN, &vo, NULL);
    if (st != OK)
        return st;
    st = handle_get(t, dma, OBJ_DMA_CAP, 0, &cap, NULL);
    if (st == OK) {
        st = vmo_unpin(vmo_from_kobject(vo), cap, pin_id);
        kobject_unref(cap);
    }
    kobject_unref(vo);
    return st;
}

/* ---- the system calls ---------------------------------------------------------- */

int64_t sysc_resource_create(handle_t parent, uint32_t kind, uint64_t base, uint64_t size,
                             uint64_t out)
{
    SYSC_TABLE(t);
    handle_t h;
    status_t st = sys_resource_create(t, parent, kind, base, size, &h);
    return st == OK ? sysc_put_handle(t, out, h) : st;
}

int64_t sysc_pci_enum(handle_t pci, uint32_t index, uint64_t out)
{
    SYSC_TABLE(t);
    struct pci_dev_info info;
    status_t st = sys_pci_enum(t, pci, index, &info);
    if (st == OK && copy_to_user(out, &info, sizeof(info)) != OK)
        return ERR_INVALID_ARGS;
    return st;
}

int64_t sysc_pci_device_open(handle_t pci, uint32_t index, uint64_t out)
{
    SYSC_TABLE(t);
    handle_t h;
    status_t st = sys_pci_device_open(t, pci, index, &h);
    return st == OK ? sysc_put_handle(t, out, h) : st;
}

int64_t sysc_pci_config_read(handle_t dev, uint32_t offset, uint32_t width, uint64_t value)
{
    SYSC_TABLE(t);
    uint32_t v;
    status_t st = sys_pci_config_read(t, dev, offset, width, &v);
    if (st == OK && copy_to_user(value, &v, sizeof(v)) != OK)
        return ERR_INVALID_ARGS;
    return st;
}

int64_t sysc_pci_config_write(handle_t dev, uint32_t offset, uint32_t width, uint32_t value)
{
    SYSC_TABLE(t);
    return sys_pci_config_write(t, dev, offset, width, value);
}

int64_t sysc_pci_bar_resource(handle_t dev, uint32_t bar, uint64_t out)
{
    SYSC_TABLE(t);
    handle_t h;
    status_t st = sys_pci_bar_resource(t, dev, bar, &h);
    return st == OK ? sysc_put_handle(t, out, h) : st;
}

int64_t sysc_pci_bus_master(handle_t dev, uint32_t enable)
{
    SYSC_TABLE(t);
    return sys_pci_bus_master(t, dev, enable);
}

int64_t sysc_dma_cap_bus_master(handle_t dma, uint32_t on)
{
    SYSC_TABLE(t);
    return sys_dma_cap_bus_master(t, dma, on);
}

int64_t sysc_vmo_create_physical(handle_t res, uint64_t offset, uint64_t size, uint32_t cache,
                                 uint64_t out)
{
    SYSC_TABLE(t);
    handle_t h;
    status_t st = sys_vmo_create_physical(t, res, offset, size, cache, &h);
    return st == OK ? sysc_put_handle(t, out, h) : st;
}

int64_t sysc_dma_cap_create(handle_t dev, uint64_t out)
{
    SYSC_TABLE(t);
    handle_t h;
    status_t st = sys_dma_cap_create(t, dev, &h);
    return st == OK ? sysc_put_handle(t, out, h) : st;
}

int64_t sysc_vmo_pin(handle_t vmo, handle_t dma, uint64_t offset, uint64_t len, uint64_t addrs,
                     uint64_t pin_id)
{
    SYSC_TABLE(t);
    if (len == 0 || (len & (PAGE_SIZE - 1)))
        return ERR_INVALID_ARGS;
    if ((len >> PAGE_SHIFT) > VMO_PIN_SYS_MAX_PAGES)
        return ERR_OUT_OF_RANGE;
    uint64_t n = len >> PAGE_SHIFT;
    uint64_t *buf = kmalloc(n * sizeof(uint64_t));
    if (!buf)
        return ERR_NO_MEMORY;
    uint64_t id = 0;
    struct vmo *v = NULL;
    struct kobject *cap = NULL;
    status_t st = pin(t, vmo, dma, offset, len, buf, &id, &v, &cap);
    if (st == OK) {
        /* Nobody learned the pin if a copy fails: undo it (on the objects
         * themselves, whatever happened to the handles meanwhile). */
        if (copy_to_user(addrs, buf, n * sizeof(uint64_t)) != OK ||
            copy_to_user(pin_id, &id, sizeof(id)) != OK) {
            vmo_unpin(v, cap, id);
            st = ERR_INVALID_ARGS;
        }
        kobject_unref(vmo_kobject(v));
        kobject_unref(cap);
    }
    kfree(buf);
    return st;
}

int64_t sysc_vmo_unpin(handle_t vmo, handle_t dma, uint64_t pin_id)
{
    SYSC_TABLE(t);
    return sys_vmo_unpin(t, vmo, dma, pin_id);
}
