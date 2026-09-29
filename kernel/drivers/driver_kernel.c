/* <jam/driver.h> in the kernel build: drivers as kernel processes (the
 * model is in <jam/driver_kernel.h>).
 *
 * Each call works on the current thread's process: its handle table for
 * handles, its context (struct kdrv, process_kernel_ctx) for the heap, the
 * kernel mappings and the driver's name. A thread that isn't in a driver's
 * kernel process gets ERR_BAD_STATE (NULL from drv_malloc; drv_log falls
 * back to kprintf).
 *
 * Kills: every call first checks whether the thread was cancelled
 * (process_kill cancels every thread of the process) and, if so, leaves
 * through uthread_exit_current, as a user thread leaves on its way back to
 * ring 3. Blocking calls use the cancellable waits, so a kill interrupts
 * them (ERR_CANCELED) and the thread leaves right there. Drivers hold no
 * kernel locks across calls, so leaving from a call's entry is safe.
 *
 * What the driver owns and the process teardown releases (kdrv_fini, after
 * the handle table is gone and before SIG_TERMINATED): its kernel mappings
 * (drv_vmo_map, drv_mmio_map and the heap's chunks) and the heap VMO. The
 * heap's pages, like every VMO the driver creates, are charged to its job.
 *
 * Locking: kdrv.lock (a mutex, "driver") guards the heap and the mapping
 * list; mapping and unmapping happen under it (they may sleep, and nothing
 * else of the driver's state is touched meanwhile). */
#include <jam/driver.h>
#include <jam/driver_kernel.h>
#include <jam/interrupt.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/sys.h>
#include <jam/time.h>
#include <jam/vmo.h>

_Static_assert(DRV_VMO_CONTIGUOUS == VMO_CONTIGUOUS && DRV_VMO_DMA32 == VMO_DMA32,
               "driver.h's VMO flags are the kernel's");

#define KDRV_MAGIC   0x6b647276u   /* "kdrv" */
#define HEAP_MAX     (16ull << 20) /* the heap VMO's size: pages commit as chunks map */
#define HEAP_CHUNK   (64ull << 10) /* mapped at a time (more for a bigger block) */
#define HALIGN       16u
#define HSPLIT_MIN   64u
#define HMAGIC_USED  0x6b75736564627573ull
#define HMAGIC_FREE  0x6b66726565667265ull
#define LOG_LINE     256

struct kmap {                 /* a kernel mapping the driver owns */
    struct list_node node;
    struct vmo      *vmo;     /* the mapping's own reference keeps it */
    void            *va;      /* what vmo_map_kernel returned (the driver's pointer) */
    uint64_t         len;
    bool             heap;    /* a heap chunk: not the driver's to unmap */
};

struct hblock {
    uint64_t size;            /* payload bytes, a multiple of HALIGN */
    uint64_t magic;
};

struct kdrv {
    uint32_t            magic;
    char                name[PROCESS_NAME_MAX];
    driver_main_fn      main;
    struct driver_start start;
    struct job         *job;          /* the process's (no reference: it outlives us) */
    struct mutex        lock;
    struct list_node    maps;         /* struct kmap (lock) */
    struct vmo         *heap;         /* (lock) NULL until the first drv_malloc */
    uint64_t            heap_mapped;  /* (lock) bytes of it mapped so far */
    uint8_t            *bump, *bump_end;   /* (lock) the current chunk's unused rest */
    struct hblock      *free_list;    /* (lock) */
};

/* ---- the current driver -------------------------------------------------------- */

static struct kdrv *self(void)
{
    struct process *p = process_current();
    struct kdrv *d = p ? process_kernel_ctx(p) : NULL;
    return d && d->magic == KDRV_MAGIC ? d : NULL;
}

/* A killed driver thread leaves here (the kernel build's "return to
 * user" check). */
static void check_killed(void)
{
    if (thread_cancel_pending() && self())
        uthread_exit_current();
}

/* Every call: leave if killed; the table, or NULL outside a driver. */
static struct handle_table *enter(void)
{
    check_killed();
    return self() ? process_handles(process_current()) : NULL;
}

#define ENTER(t)                               \
    struct handle_table *t = enter();          \
    if (!t)                                    \
        return ERR_BAD_STATE

/* After a wait: a kill makes it return ERR_CANCELED; leave then. */
static status_t waited(status_t st)
{
    if (st == ERR_CANCELED)
        check_killed();
    return st;
}

handle_t drv_handle(const struct driver_start *s, uint32_t role)
{
    for (uint32_t i = 0; i < s->nhandles && i < DRV_MAX_HANDLES; i++)
        if (s->handles[i].role == role)
            return s->handles[i].h;
    return HANDLE_INVALID;
}

/* ---- basics ------------------------------------------------------------------ */

/* Format "<prefix><fmt...>\n" into buf (LOG_LINE); returns the length. */
static size_t format_line(char *buf, const char *prefix, const char *fmt, va_list ap)
{
    size_t n = 0;
    if (prefix)
        n = (size_t)ksnprintf(buf, LOG_LINE - 1, "%s: ", prefix);
    if (n > LOG_LINE - 2)
        n = LOG_LINE - 2;
    int m = kvsnprintf(buf + n, LOG_LINE - 1 - n, fmt, ap);
    n += m < 0 ? 0 : (size_t)m;
    if (n > LOG_LINE - 2)
        n = LOG_LINE - 2;
    if (!n || buf[n - 1] != '\n')
        buf[n++] = '\n';
    buf[n] = '\0';
    return n;
}

static void out(bool report_it, const char *fmt, va_list ap)
{
    check_killed();
    struct kdrv *d = self();
    char buf[LOG_LINE];
    /* A report is a line of its own in the RESULTS box: say whose. The log
     * line gets "[name]" from process_debug_write. */
    size_t n = format_line(buf, report_it && d ? d->name : NULL, fmt, ap);
    if (d)
        process_debug_write(process_current(), buf, n, report_it);
    else
        kprintf("driver?: %s", buf);
}

void drv_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    out(false, fmt, ap);
    va_end(ap);
}

void drv_report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    out(true, fmt, ap);
    va_end(ap);
}

uint64_t drv_clock_ns(void)
{
    check_killed();   /* polling loops read the clock: a kill gets them too */
    return uptime_ns();
}

status_t drv_sleep_until(uint64_t deadline_ns)
{
    ENTER(t);
    (void)t;
    while (uptime_ns() < deadline_ns)
        if (thread_block_cancellable(NULL, NULL, deadline_ns) != OK)
            return waited(ERR_CANCELED);
    return OK;
}

_Noreturn void drv_exit(int code)
{
    if (!self())
        panic("drv_exit outside a driver (thread \"%s\")", current_thread()->name);
    process_kill(process_current(), code, false);   /* cancels every thread, this one too */
    uthread_exit_current();
}

status_t drv_thread_start(const char *name, void (*fn)(void *), void *arg)
{
    ENTER(t);
    (void)t;
    if (!fn)
        return ERR_INVALID_ARGS;
    struct uthread *u;
    status_t st = uthread_create(process_current(), name ? name : "driver", &u);
    if (st != OK)
        return st;
    st = uthread_start_kernel(u, fn, arg);
    kobject_unref(uthread_kobject(u));   /* a running thread holds its own */
    return st;
}

/* ---- heap -------------------------------------------------------------------- */

/* lock held: map the next chunk of the heap VMO, at least `need` bytes. */
static bool heap_grow(struct kdrv *d, uint64_t need)
{
    uint64_t len = ALIGN_UP(need, PAGE_SIZE);
    if (len < HEAP_CHUNK)
        len = HEAP_CHUNK;
    if (!d->heap) {
        struct vmo *v;
        if (vmo_create(HEAP_MAX, 0, &v) != OK)
            return false;
        if (vmo_set_job(v, d->job) != OK) {
            kobject_unref(vmo_kobject(v));
            return false;
        }
        d->heap = v;
    }
    if (len > HEAP_MAX - d->heap_mapped)
        return false;
    struct kmap *m = kzalloc(sizeof(*m));
    if (!m)
        return false;
    void *va;
    if (vmo_map_kernel(d->heap, d->heap_mapped, len, VM_WRITE, &va) != OK) {
        kfree(m);   /* the job said no, most likely */
        return false;
    }
    m->vmo = d->heap;
    m->va = va;
    m->len = len;
    m->heap = true;
    list_add_tail(&d->maps, &m->node);
    d->heap_mapped += len;
    d->bump = va;
    d->bump_end = (uint8_t *)va + len;
    return true;
}

void *drv_malloc(size_t n)
{
    check_killed();
    struct kdrv *d = self();
    if (!d || n > HEAP_MAX)
        return NULL;
    uint64_t size = ALIGN_UP(n ? n : 1, HALIGN);
    mutex_lock(&d->lock);
    struct hblock *b = NULL;
    for (struct hblock **pp = &d->free_list; *pp; pp = (struct hblock **)(*pp + 1)) {
        if ((*pp)->size >= size) {
            b = *pp;
            *pp = *(struct hblock **)(b + 1);
            if (b->size >= size + sizeof(struct hblock) + HSPLIT_MIN) {
                struct hblock *rest = (struct hblock *)((uint8_t *)(b + 1) + size);
                rest->size = b->size - size - sizeof(struct hblock);
                rest->magic = HMAGIC_FREE;
                *(struct hblock **)(rest + 1) = d->free_list;
                d->free_list = rest;
                b->size = size;
            }
            break;
        }
    }
    if (!b) {
        uint64_t total = sizeof(struct hblock) + size;
        if ((uint64_t)(d->bump_end - d->bump) < total && !heap_grow(d, total)) {
            mutex_unlock(&d->lock);
            return NULL;
        }
        b = (struct hblock *)d->bump;
        d->bump += total;
        b->size = size;
    }
    b->magic = HMAGIC_USED;
    mutex_unlock(&d->lock);
    return b + 1;
}

void drv_free(void *p)
{
    check_killed();
    struct kdrv *d = self();
    if (!p || !d)
        return;
    struct hblock *b = (struct hblock *)p - 1;
    mutex_lock(&d->lock);
    if (b->magic != HMAGIC_USED) {
        mutex_unlock(&d->lock);
        kprintf("[%s] drv_free(%p): not an allocated block (ignored)\n", d->name, p);
        return;
    }
    b->magic = HMAGIC_FREE;
    *(struct hblock **)(b + 1) = d->free_list;
    d->free_list = b;
    mutex_unlock(&d->lock);
}

/* ---- handles, channels, ports, waiting ----------------------------------------- */

status_t drv_handle_close(handle_t h)
{
    ENTER(t);
    return handle_close(t, h);
}

status_t drv_handle_duplicate(handle_t h, rights_t rights, handle_t *out)
{
    ENTER(t);
    return out ? handle_duplicate(t, h, rights, out) : ERR_INVALID_ARGS;
}

status_t drv_channel_create(handle_t *a, handle_t *b)
{
    ENTER(t);
    return a && b ? sys_channel_create(t, a, b) : ERR_INVALID_ARGS;
}

status_t drv_channel_write(handle_t h, const void *bytes, uint32_t n, const handle_t *hs,
                           uint32_t nh)
{
    ENTER(t);
    return sys_channel_write(t, h, bytes, n, hs, nh);
}

status_t drv_channel_read(handle_t h, void *bytes, uint32_t cap, uint32_t *actual, handle_t *hs,
                          uint32_t hcap, uint32_t *hactual)
{
    ENTER(t);
    return sys_channel_read(t, h, bytes, cap, actual, hs, hcap, hactual);
}

status_t drv_channel_call(handle_t h, void *wbytes, uint32_t wn, void *rbytes, uint32_t rcap,
                          uint32_t *ractual, uint64_t deadline_ns)
{
    ENTER(t);
    return waited(sys_channel_call(t, h, wbytes, wn, NULL, 0, rbytes, rcap, ractual, NULL, 0,
                                   NULL, deadline_ns));
}

status_t drv_port_create(handle_t *out)
{
    ENTER(t);
    return out ? sys_port_create(t, out) : ERR_INVALID_ARGS;
}

status_t drv_port_bind(handle_t port, handle_t obj, uint64_t key, signals_t mask, uint32_t flags)
{
    ENTER(t);
    return sys_port_bind(t, port, obj, key, mask, flags);
}

status_t drv_port_wait(handle_t port, uint64_t deadline_ns, struct port_packet *out)
{
    ENTER(t);
    if (!out)
        return ERR_INVALID_ARGS;
    return waited(sys_port_wait(t, port, deadline_ns, out));
}

status_t drv_object_wait_one(handle_t h, signals_t mask, uint64_t deadline_ns,
                             signals_t *observed)
{
    ENTER(t);
    signals_t seen = 0;
    status_t st = waited(sys_object_wait_one(t, h, mask, deadline_ns, &seen));
    if (observed)
        *observed = seen;
    return st;
}

/* ---- memory ------------------------------------------------------------------ */

status_t drv_vmo_create(uint64_t size, uint32_t flags, handle_t *out)
{
    ENTER(t);
    if (!out)
        return ERR_INVALID_ARGS;
    /* Contiguous / DMA32 memory needs a DMA capability: the driver's own. */
    handle_t dma = flags & (VMO_CONTIGUOUS | VMO_DMA32) ? drv_handle(&self()->start, DR_DMA)
                                                        : HANDLE_INVALID;
    return sys_vmo_create(t, size, flags, dma, out);
}

status_t drv_vmo_read(handle_t vmo, uint64_t off, void *buf, uint64_t len)
{
    ENTER(t);
    return sys_vmo_read(t, vmo, off, buf, len);
}

status_t drv_vmo_write(handle_t vmo, uint64_t off, const void *buf, uint64_t len)
{
    ENTER(t);
    return sys_vmo_write(t, vmo, off, buf, len);
}

/* Map v's [off, off + len) into the kernel for the driver and record it. On
 * success the mapping holds its own reference on v. */
static status_t map_for(struct kdrv *d, struct vmo *v, uint64_t off, uint64_t len,
                        unsigned vm_flags, void **addr)
{
    struct kmap *m = kzalloc(sizeof(*m));
    if (!m)
        return ERR_NO_MEMORY;
    mutex_lock(&d->lock);
    void *va;
    status_t st = vmo_map_kernel(v, off, len, vm_flags, &va);
    if (st == OK) {
        m->vmo = v;
        m->va = va;
        m->len = len;
        list_add_tail(&d->maps, &m->node);
        *addr = va;
    }
    mutex_unlock(&d->lock);
    if (st != OK)
        kfree(m);
    return st;
}

status_t drv_vmo_map(handle_t vmo, uint64_t off, uint64_t len, uint32_t flags, void **addr)
{
    ENTER(t);
    if (!addr || !len || (flags & ~(VMAR_READ | VMAR_WRITE)) || !(flags & VMAR_READ))
        return ERR_INVALID_ARGS;
    bool w = flags & VMAR_WRITE;
    struct kobject *obj;
    status_t st = handle_get(t, vmo, OBJ_VMO, RIGHT_MAP | RIGHT_READ | (w ? RIGHT_WRITE : 0), &obj,
                             NULL);
    if (st != OK)
        return st;
    st = map_for(self(), vmo_from_kobject(obj), off, len, w ? VM_WRITE : 0, addr);
    kobject_unref(obj);
    return st;
}

status_t drv_vmo_unmap(void *addr, uint64_t len)
{
    ENTER(t);
    (void)t;
    struct kdrv *d = self();
    mutex_lock(&d->lock);
    struct kmap *found = NULL;
    for (struct list_node *n = d->maps.next; n != &d->maps; n = n->next) {
        struct kmap *m = container_of(n, struct kmap, node);
        if (!m->heap && m->va == addr) {
            found = m;
            break;
        }
    }
    /* The kernel build unmaps whole mappings only (what one map call made). */
    status_t st = !found ? ERR_NOT_FOUND : found->len != len ? ERR_INVALID_ARGS : OK;
    if (st == OK) {
        list_del(&found->node);
        st = vmo_unmap_kernel(found->vmo, found->va);
    }
    mutex_unlock(&d->lock);
    if (st == OK)
        kfree(found);
    return st;
}

/* Pins, interrupts and config space go through the same sys_* functions as
 * the system calls (rights, unbound-cap refusal, the pin size limit, the
 * config write filter and its command-register lock): the kernel build
 * must not be a weaker door than the process build. (Review of M6 phase 1:
 * it had its own, smaller config filter with no lock.) */
status_t drv_vmo_pin(handle_t vmo, handle_t dma, uint64_t off, uint64_t len, uint64_t *addrs,
                     uint64_t *pin_id)
{
    ENTER(t);
    if (!addrs || !pin_id)
        return ERR_INVALID_ARGS;
    return sys_vmo_pin(t, vmo, dma, off, len, addrs, pin_id);
}

status_t drv_vmo_unpin(handle_t vmo, handle_t dma, uint64_t pin_id)
{
    ENTER(t);
    return sys_vmo_unpin(t, vmo, dma, pin_id);
}

status_t drv_dma_bus_master(handle_t dma, uint32_t on)
{
    ENTER(t);
    return sys_dma_cap_bus_master(t, dma, on);
}

status_t drv_mmio_map(handle_t bar, uint64_t off, uint64_t len, uint32_t cache,
                      volatile void **addr)
{
    ENTER(t);
    unsigned vm_cache = cache == VMO_CACHE_UC ? VM_UC : cache == VMO_CACHE_WC ? VM_WC
                      : cache == VMO_CACHE_WB ? 0 : ~0u;
    if (!addr || !len || vm_cache == ~0u)
        return ERR_INVALID_ARGS;
    struct kobject *res;
    status_t st = handle_get(t, bar, OBJ_RESOURCE, RIGHT_MAP, &res, NULL);
    if (st != OK)
        return st;
    uint64_t base = 0, size = 0;
    st = resource_kind(res) == RES_MMIO ? resource_mmio_range(res, &base, &size)
                                        : ERR_WRONG_TYPE;
    if (st == OK && (off > size || len > size - off))
        st = ERR_OUT_OF_RANGE;
    /* Whole pages: the VMO covers them, the pointer is to `off` itself. */
    uint64_t phys = base + off, first = ALIGN_DOWN(phys, PAGE_SIZE);
    uint64_t span = ALIGN_UP(phys + len, PAGE_SIZE) - first;
    if (st == OK)
        st = resource_check_mmio(res, first, span);
    /* The same rule as vmo_create_physical: no RAM, no MSI-X table or PBA
     * page, no MMIO the kernel drives (LAPIC/MSI window, I/O APICs, HPET,
     * ECAM). */
    if (st == OK)
        st = resource_phys_mappable(first, span);
    struct vmo *v = NULL;
    if (st == OK)
        st = vmo_create_physical(first, span, vm_cache, &v);
    void *va = NULL;
    if (st == OK) {
        st = map_for(self(), v, phys - first, len, VM_WRITE, &va);
        kobject_unref(vmo_kobject(v));   /* the mapping keeps it */
    }
    kobject_unref(res);
    if (st == OK)
        *addr = va;
    return st;
}

/* ---- device ------------------------------------------------------------------ */

status_t drv_interrupt_ack(handle_t irq)
{
    ENTER(t);
    return sys_interrupt_ack(t, irq);
}

status_t drv_pci_config_read(handle_t dev, uint32_t off, uint32_t width, uint32_t *value)
{
    ENTER(t);
    if (!value)
        return ERR_INVALID_ARGS;
    return sys_pci_config_read(t, dev, off, width, value);
}

status_t drv_pci_config_write(handle_t dev, uint32_t off, uint32_t width, uint32_t value)
{
    ENTER(t);
    return sys_pci_config_write(t, dev, off, width, value);
}

/* ---- kernel processes ------------------------------------------------------------ */

driver_main_fn driver_kernel_find(const char *name)
{
    for (const struct kdriver *k = kdrivers; k->name; k++)
        if (!strcmp(k->name, name))
            return k->main;
    return NULL;
}

static void kdrv_fini(void *ctx)
{
    struct kdrv *d = ctx;
    /* No thread is left: nobody else touches d. */
    while (!list_empty(&d->maps)) {
        struct kmap *m = list_first(&d->maps, struct kmap, node);
        list_del(&m->node);
        if (vmo_unmap_kernel(m->vmo, m->va) != OK)
            kprintf("[%s] teardown: a mapping at %p would not unmap\n", d->name, m->va);
        kfree(m);
    }
    if (d->heap)
        kobject_unref(vmo_kobject(d->heap));   /* its pages leave the job now */
    d->magic = 0;
    kfree(d);
}

static void kdrv_entry(void *arg)
{
    struct kdrv *d = arg;
    drv_exit(d->main(&d->start));
}

status_t driver_kernel_start(const char *name, driver_main_fn fn, struct driver_kernel_handle *hs,
                             unsigned n, struct job *job, struct process **out)
{
    status_t st = fn && name && n <= DRV_MAX_HANDLES && (hs || !n) ? OK : ERR_INVALID_ARGS;
    struct kdrv *d = NULL;
    struct process *p = NULL;
    if (st == OK && !(d = kzalloc(sizeof(*d))))
        st = ERR_NO_MEMORY;
    if (st == OK) {
        d->magic = KDRV_MAGIC;
        size_t len = strlen(name);
        if (len >= sizeof(d->name))
            len = sizeof(d->name) - 1;
        memcpy(d->name, name, len);
        d->main = fn;
        d->start.name = d->name;
        d->job = job;
        mutex_init(&d->lock, "driver");
        list_init(&d->maps);
        st = process_create_kernel(job, d->name, d, kdrv_fini, &p);
        if (st != OK) {
            kfree(d);
            d = NULL;
        }
    }
    for (unsigned i = 0; st == OK && i < n; i++) {
        handle_t hv;
        st = handle_insert(process_handles(p), &hs[i].kh, &hv);
        if (st == OK) {
            d->start.handles[d->start.nhandles].role = hs[i].role;
            d->start.handles[d->start.nhandles].h = hv;
            d->start.nhandles++;
        }
    }
    for (unsigned i = 0; hs && i < n; i++)
        khandle_release(&hs[i].kh);   /* the ones not inserted (a no-op for the rest) */
    struct uthread *u = NULL;
    if (st == OK)
        st = uthread_create(p, d->name, &u);
    if (st == OK)
        st = process_start_kernel(p, u, kdrv_entry, d);
    if (u)
        kobject_unref(uthread_kobject(u));
    if (st != OK) {
        if (p) {
            process_kill(p, PROCESS_KILLED_CODE, true);   /* never ran: tears down (kdrv_fini) now */
            kobject_unref(process_kobject(p));
        }
        return st;
    }
    if (out)
        *out = p;
    else
        kobject_unref(process_kobject(p));
    return OK;
}
