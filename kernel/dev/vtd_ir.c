/* VT-d interrupt remapping entries (see vtd_ir.h): the encoders for an
 * IRTE, a remappable MSI and a remappable I/O APIC entry, and the table
 * with its index allocator.
 *
 * Writing an entry the unit may read at any moment (VT-d 9.9 and 5.1.4; the
 * invalidation is 6.5.2's interrupt entry cache descriptor):
 *   - making one present: the upper half (source validation) first, then
 *     the lower half with P. x86 stores are seen in program order, so the
 *     unit finds either a not-present entry or the whole new one;
 *   - changing a present one: only its lower half may change (destination,
 *     vector, trigger mode), in one 64-bit store, so the unit sees the old
 *     entry or the new one, never a mix;
 *   - clearing one: the lower half (P) first, then the upper.
 * Each is flushed (the 16 bytes lie in one cache line: entries are
 * 16-byte aligned) and then invalidated in the unit's interrupt entry
 * cache, which may hold the old entry, present or not. */
#include <jam/dmar.h>
#include <jam/mm.h>
#include <jam/string.h>
#include <jam/vtd.h>

#include "vtd_ir.h"

#define IRTE_PER_PAGE (PAGE_SIZE / sizeof(struct vtd_irte))

/* ---- encoders (pure) --------------------------------------------------------------- */

status_t vtd_ir_source_from_scope(const struct dmar_scope *s, struct vtd_ir_source *out)
{
    if (s->type != DMAR_SCOPE_IOAPIC && s->type != DMAR_SCOPE_HPET)
        return ERR_INVALID_ARGS;
    if (s->path_full != 1 || s->path_len != 1)
        return ERR_NOT_SUPPORTED;
    if (s->path[0].dev > 31 || s->path[0].fn > 7)
        return ERR_INVALID_ARGS;
    *out = vtd_ir_source_device(s->start_bus, s->path[0].dev, s->path[0].fn);
    return OK;
}

/* The upper half: who may use the entry. */
static status_t encode_source(const struct vtd_ir_source *src, uint64_t *out)
{
    if (src->svt == VTD_IR_SVT_RID && src->sq > VTD_IR_SQ_NOT_F)
        return ERR_INVALID_ARGS;
    if (src->svt == VTD_IR_SVT_BUS && (src->sq != 0 || (src->sid >> 8) > (src->sid & 0xff)))
        return ERR_INVALID_ARGS;
    if (src->svt != VTD_IR_SVT_RID && src->svt != VTD_IR_SVT_BUS)
        return ERR_INVALID_ARGS;
    *out = src->sid | (uint64_t)src->sq << VTD_IRTE_SQ_SHIFT |
           (uint64_t)src->svt << VTD_IRTE_SVT_SHIFT;
    return OK;
}

status_t vtd_irte_encode(const struct vtd_irte_spec *s, bool eim, struct vtd_irte *out)
{
    if (s->vector < VTD_IR_VECTOR_MIN)
        return ERR_INVALID_ARGS;   /* 0-31 are the CPU's exceptions */
    if (!eim && s->dest > 0xff)
        return ERR_OUT_OF_RANGE;
    uint64_t hi;
    status_t st = encode_source(&s->src, &hi);
    if (st != OK)
        return st;
    uint64_t dest = eim ? (uint64_t)s->dest << VTD_IRTE_DST_SHIFT
                        : (uint64_t)s->dest << VTD_IRTE_XDST_SHIFT;
    /* Fixed delivery (DLM 0), physical destination (DM 0), no redirection
     * hint, remapped format (IM 0), fault processing on (FPD 0). */
    out->lo = VTD_IRTE_P | (s->level ? VTD_IRTE_TM : 0) |
              (uint64_t)s->vector << VTD_IRTE_V_SHIFT | dest;
    out->hi = hi;
    return OK;
}

status_t vtd_ir_msi_encode(uint32_t index, struct vtd_ir_msi *out)
{
    if (index > 0xffff)
        return ERR_OUT_OF_RANGE;
    uint32_t a = VTD_IR_MSI_BASE | (index & 0x7fff) << VTD_IR_MSI_H_SHIFT | VTD_IR_MSI_FORMAT;
    if (index & 0x8000)
        a |= VTD_IR_MSI_H15;
    out->address = a;
    out->data = 0;
    return OK;
}

status_t vtd_ir_rte_encode(const struct vtd_ir_rte_spec *s, uint64_t *out)
{
    if (s->index > 0xffff)
        return ERR_OUT_OF_RANGE;
    if (s->vector < VTD_IR_VECTOR_MIN)
        return ERR_INVALID_ARGS;
    uint64_t v = s->vector | VTD_IR_RTE_FORMAT |
                 (uint64_t)(s->index & 0x7fff) << VTD_IR_RTE_I_SHIFT;
    if (s->index & 0x8000)
        v |= VTD_IR_RTE_I15;
    if (s->level)
        v |= VTD_IR_RTE_LEVEL;
    if (s->active_low)
        v |= VTD_IR_RTE_ACTIVE_LOW;
    if (s->masked)
        v |= VTD_IR_RTE_MASKED;
    *out = v;
    return OK;
}

/* ---- the table ------------------------------------------------------------------------ */

static unsigned order_of(uint32_t entries)
{
    unsigned order = 0;
    while ((IRTE_PER_PAGE << order) < entries)
        order++;
    return order;
}

static void flush(const struct vtd_ir_table *t, const void *va, size_t len)
{
    if (t->ops->flush)
        t->ops->flush(t->ctx, va, len);
}

static bool bit(const uint64_t *map, uint32_t i)
{
    return (map[i / 64] >> (i % 64)) & 1;
}

status_t vtd_ir_table_init(struct vtd_ir_table *t, uint32_t entries, bool eim,
                           const struct vtd_ir_ops *ops, void *ctx)
{
    if (entries < VTD_IR_MIN_ENTRIES || entries > VTD_IR_MAX_ENTRIES ||
        (entries & (entries - 1)) || !ops->invalidate)
        return ERR_INVALID_ARGS;
    uint64_t *used = kzalloc(entries / 8);
    if (!used)
        return ERR_NO_MEMORY;
    unsigned order = order_of(entries);
    struct page *pg = pmm_alloc_pages(order, PMM_ZERO);
    if (!pg) {
        kfree(used);
        return ERR_NO_MEMORY;
    }
    memset(t, 0, sizeof(*t));
    t->entries = page_to_virt(pg);
    t->phys = page_to_phys(pg);
    t->size = entries;
    t->eim = eim;
    t->ops = ops;
    t->ctx = ctx;
    spin_init(&t->lock, "vtd irte");
    t->used = used;
    t->used[0] = 1;   /* entry 0: never handed out */
    t->next = 1;
    flush(t, t->entries, (size_t)entries * sizeof(struct vtd_irte));
    return OK;
}

void vtd_ir_table_destroy(struct vtd_ir_table *t)
{
    pmm_free_pages(virt_to_page(t->entries), order_of(t->size));
    kfree(t->used);
    t->entries = NULL;
    t->used = NULL;
}

uint64_t vtd_ir_irta(const struct vtd_ir_table *t)
{
    unsigned s = 0;   /* the table holds 2^(S + 1) entries */
    while ((2u << s) < t->size)
        s++;
    return t->phys | (t->eim ? VTD_IRTA_EIME : 0) | s;
}

status_t vtd_ir_alloc(struct vtd_ir_table *t, uint32_t *out_index)
{
    spin_lock(&t->lock);
    if (t->nused == t->size - 1) {
        spin_unlock(&t->lock);
        return ERR_NO_RESOURCES;
    }
    uint32_t i = t->next;
    while (bit(t->used, i))   /* ends: some entry is free */
        i = (i + 1) & (t->size - 1);
    t->used[i / 64] |= 1ull << (i % 64);
    t->nused++;
    t->next = (i + 1) & (t->size - 1);
    spin_unlock(&t->lock);
    *out_index = i;
    return OK;
}

/* Is `index` an entry someone allocated? */
static bool allocated(struct vtd_ir_table *t, uint32_t index)
{
    if (index == 0 || index >= t->size)
        return false;
    spin_lock(&t->lock);
    bool yes = bit(t->used, index);
    spin_unlock(&t->lock);
    return yes;
}

/* Store one half of an entry the unit may be reading. */
static void put(uint64_t *half, uint64_t v)
{
    *(volatile uint64_t *)half = v;
}

status_t vtd_ir_set(struct vtd_ir_table *t, uint32_t index, const struct vtd_irte_spec *s)
{
    if (!allocated(t, index))
        return ERR_INVALID_ARGS;
    struct vtd_irte e;
    status_t st = vtd_irte_encode(s, t->eim, &e);
    if (st != OK)
        return st;
    struct vtd_irte *slot = &t->entries[index];
    if (slot->lo & VTD_IRTE_P) {
        if (slot->hi != e.hi)
            return ERR_BAD_STATE;
        put(&slot->lo, e.lo);
    } else {
        put(&slot->hi, e.hi);
        put(&slot->lo, e.lo);
    }
    flush(t, slot, sizeof(*slot));
    return t->ops->invalidate(t->ctx, index, 1);
}

status_t vtd_ir_free(struct vtd_ir_table *t, uint32_t index)
{
    if (!allocated(t, index))
        return ERR_INVALID_ARGS;
    struct vtd_irte *slot = &t->entries[index];
    put(&slot->lo, 0);
    put(&slot->hi, 0);
    flush(t, slot, sizeof(*slot));
    status_t st = t->ops->invalidate(t->ctx, index, 1);
    if (st != OK)
        return st;   /* the index stays taken: the unit may still cache the entry */
    spin_lock(&t->lock);
    t->used[index / 64] &= ~(1ull << (index % 64));
    t->nused--;
    spin_unlock(&t->lock);
    return OK;
}
