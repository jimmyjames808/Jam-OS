/* <jam/driver.h>'s implementation: every call is a system call, or libos
 * (malloc, vsnprintf, threads).
 *
 * Part of libos, so ordinary programs can use it too: a client calling a
 * driver through a generated <idl/...> stub goes through drv_channel_call.
 * A driver's own process also links user/lib/driver_crt.c, whose main()
 * turns the startup message into the struct driver_start and sets
 * libos_driver_start.
 *
 * A thread started with drv_thread_start keeps its 64 KiB stack (from the
 * heap) after it exits: libos can't free a stack it may still be running
 * on. */
#include <jam/driver.h>
#include <os.h>
#include "driver_start.h"

#define THREAD_STACK   (64u << 10)
#define LOG_LINE       256
/* <jam/vmo.h>'s vmo_create flags (the kernel's header; user code doesn't
 * see it): contiguous / DMA32 memory needs the driver's DMA capability. */
#define VMO_NEEDS_DMA_CAP 0x3u

const struct driver_start *libos_driver_start;

handle_t drv_handle(const struct driver_start *s, uint32_t role)
{
    for (uint32_t i = 0; i < s->nhandles && i < DRV_MAX_HANDLES; i++)
        if (s->handles[i].role == role)
            return s->handles[i].h;
    return HANDLE_INVALID;
}

/* ---- basics ------------------------------------------------------------------ */

static void out(bool report_it, const char *fmt, va_list ap)
{
    char buf[LOG_LINE];
    size_t n = 0;
    /* The kernel prefixes log lines with "[process]"; a RESULTS line says
     * whose it is itself. */
    if (report_it && libos_driver_start && libos_driver_start->name)
        n = (size_t)snprintf(buf, LOG_LINE - 1, "%s: ", libos_driver_start->name);
    if (n > LOG_LINE - 2)
        n = LOG_LINE - 2;
    int m = vsnprintf(buf + n, LOG_LINE - 1 - n, fmt, ap);
    n += m < 0 ? 0 : (size_t)m;
    if (n > LOG_LINE - 2)
        n = LOG_LINE - 2;
    if (!n || buf[n - 1] != '\n')
        buf[n++] = '\n';
    if (report_it)
        jam_debug_report(buf, n);
    else
        jam_debug_write(buf, n);
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
    return now();
}

status_t drv_sleep_until(uint64_t deadline_ns)
{
    return jam_nanosleep(deadline_ns);
}

_Noreturn void drv_exit(int code)
{
    jam_process_exit(code);
}

status_t drv_thread_start(const char *name, void (*fn)(void *), void *arg)
{
    if (!fn)
        return ERR_INVALID_ARGS;
    void *stack = malloc(THREAD_STACK);
    if (!stack)
        return ERR_NO_MEMORY;
    handle_t t;
    status_t st = thread_spawn(name ? name : "driver", fn, arg, stack, THREAD_STACK, &t);
    if (st != OK) {
        free(stack);
        return st;
    }
    jam_handle_close(t);
    return OK;
}

void *drv_malloc(size_t n)
{
    return malloc(n);
}

void drv_free(void *p)
{
    free(p);
}

int drv_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    return vsnprintf(buf, size, fmt, ap);
}

int drv_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- handles, channels, ports, waiting ----------------------------------------- */

status_t drv_handle_close(handle_t h)
{
    return jam_handle_close(h);
}

status_t drv_handle_duplicate(handle_t h, rights_t rights, handle_t *out)
{
    return jam_handle_duplicate(h, rights, out);
}

status_t drv_channel_create(handle_t *a, handle_t *b)
{
    return jam_channel_create(a, b);
}

status_t drv_channel_write(handle_t h, const void *bytes, uint32_t n, const handle_t *hs,
                           uint32_t nh)
{
    return jam_channel_write(h, bytes, n, hs, nh);
}

status_t drv_channel_read(handle_t h, void *bytes, uint32_t cap, uint32_t *actual, handle_t *hs,
                          uint32_t hcap, uint32_t *hactual)
{
    struct channel_read_args a = {
        .h = h,
        .bytes_cap = cap,
        .bytes = (uint64_t)(uintptr_t)bytes,
        .actual_bytes = (uint64_t)(uintptr_t)actual,
        .handles = (uint64_t)(uintptr_t)hs,
        .handles_cap = hcap,
        .actual_handles = (uint64_t)(uintptr_t)hactual,
    };
    return jam_channel_read(&a);
}

status_t drv_channel_call(handle_t h, void *wbytes, uint32_t wn, void *rbytes, uint32_t rcap,
                          uint32_t *ractual, uint64_t deadline_ns)
{
    struct channel_call_args a = {
        .h = h,
        .wn = wn,
        .wbytes = (uint64_t)(uintptr_t)wbytes,
        .rcap = rcap,
        .rbytes = (uint64_t)(uintptr_t)rbytes,
        .ractual = (uint64_t)(uintptr_t)ractual,
        .deadline_ns = deadline_ns,
    };
    return jam_channel_call(&a);
}

status_t drv_channel_call_h(handle_t h, void *wbytes, uint32_t wn, void *rbytes, uint32_t rcap,
                            uint32_t *ractual, handle_t *rh, uint32_t rhcap, uint32_t *rhactual,
                            uint64_t deadline_ns)
{
    struct channel_call_args a = {
        .h = h,
        .wn = wn,
        .wbytes = (uint64_t)(uintptr_t)wbytes,
        .rcap = rcap,
        .rbytes = (uint64_t)(uintptr_t)rbytes,
        .ractual = (uint64_t)(uintptr_t)ractual,
        .rh = (uint64_t)(uintptr_t)rh,
        .rhcap = rhcap,
        .rhactual = (uint64_t)(uintptr_t)rhactual,
        .deadline_ns = deadline_ns,
    };
    return jam_channel_call(&a);
}

status_t drv_port_create(handle_t *out)
{
    return jam_port_create(out);
}

status_t drv_port_bind(handle_t port, handle_t obj, uint64_t key, signals_t mask, uint32_t flags)
{
    return jam_port_bind(port, obj, key, mask, flags);
}

status_t drv_port_wait(handle_t port, uint64_t deadline_ns, struct port_packet *out)
{
    return jam_port_wait(port, deadline_ns, out);
}

status_t drv_object_wait_one(handle_t h, signals_t mask, uint64_t deadline_ns,
                             signals_t *observed)
{
    return jam_object_wait_one(h, mask, deadline_ns, observed);
}

/* ---- memory ------------------------------------------------------------------ */

status_t drv_vmo_create(uint64_t size, uint32_t flags, handle_t *out)
{
    handle_t dma = HANDLE_INVALID;
    if ((flags & VMO_NEEDS_DMA_CAP) && libos_driver_start)
        dma = drv_handle(libos_driver_start, DR_DMA);
    return jam_vmo_create(size, flags, dma, out);
}

status_t drv_vmo_read(handle_t vmo, uint64_t off, void *buf, uint64_t len)
{
    return jam_vmo_read(vmo, off, buf, len);
}

status_t drv_vmo_write(handle_t vmo, uint64_t off, const void *buf, uint64_t len)
{
    return jam_vmo_write(vmo, off, buf, len);
}

/* Map the pages holding [off, off + len) of vmo; *addr points at off
 * itself. */
static status_t map_pages(handle_t vmo, uint64_t off, uint64_t len, uint32_t flags, void **addr)
{
    if (!addr || !len || off + len < off)
        return ERR_INVALID_ARGS;
    uint64_t first = off & ~(PAGE_SIZE - 1);
    uint64_t span = ((off + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)) - first;
    uint64_t va = 0;
    status_t st = jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, first, span, flags, &va);
    if (st == OK)
        *addr = (void *)(uintptr_t)(va + (off - first));
    return st;
}

status_t drv_vmo_map(handle_t vmo, uint64_t off, uint64_t len, uint32_t flags, void **addr)
{
    if ((flags & ~(VMAR_READ | VMAR_WRITE)) || !(flags & VMAR_READ))
        return ERR_INVALID_ARGS;
    return map_pages(vmo, off, len, flags, addr);
}

status_t drv_vmo_unmap(void *addr, uint64_t len)
{
    uint64_t a = (uint64_t)(uintptr_t)addr, first = a & ~(PAGE_SIZE - 1);
    if (!len || a + len < a)
        return ERR_INVALID_ARGS;
    uint64_t span = ((a + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)) - first;
    return jam_vmar_unmap(startup_handle(SR_SELF_VMAR), first, span);
}

status_t drv_vmo_pin(handle_t vmo, handle_t dma, uint64_t off, uint64_t len, uint64_t *addrs,
                     uint64_t *pin_id)
{
    return jam_vmo_pin(vmo, dma, off, len, addrs, pin_id);
}

status_t drv_vmo_unpin(handle_t vmo, handle_t dma, uint64_t pin_id)
{
    return jam_vmo_unpin(vmo, dma, pin_id);
}

status_t drv_dma_bus_master(handle_t dma, uint32_t on)
{
    return jam_dma_cap_bus_master(dma, on);
}

status_t drv_mmio_map(handle_t bar, uint64_t off, uint64_t len, uint32_t cache,
                      volatile void **addr)
{
    if (!addr || !len || off + len < off)
        return ERR_INVALID_ARGS;
    uint64_t first = off & ~(PAGE_SIZE - 1);
    uint64_t span = ((off + len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1)) - first;
    handle_t vmo;
    status_t st = jam_vmo_create_physical(bar, first, span, cache, &vmo);
    if (st != OK)
        return st;
    void *va = NULL;
    st = map_pages(vmo, off - first, len, VMAR_READ | VMAR_WRITE, &va);
    jam_handle_close(vmo);   /* the mapping keeps it */
    if (st == OK)
        *addr = va;
    return st;
}

/* ---- device ------------------------------------------------------------------ */

status_t drv_interrupt_ack(handle_t irq)
{
    return jam_interrupt_ack(irq);
}

status_t drv_pci_config_read(handle_t dev, uint32_t off, uint32_t width, uint32_t *value)
{
    return jam_pci_config_read(dev, off, width, value);
}

status_t drv_pci_config_write(handle_t dev, uint32_t off, uint32_t width, uint32_t value)
{
    return jam_pci_config_write(dev, off, width, value);
}
