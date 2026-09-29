/* <jam/driver.h>: the ONLY header a driver includes (M6, Track D).
 *
 * Every driver is written as if it were already a process: it touches the
 * world only through handles. Two implementations of this file's functions:
 *   - kernel build (kernel/drivers/driver_kernel.c): the driver runs as a
 *     kernel thread inside a *kernel process* (a struct process on the
 *     kernel address space, with its own handle table and job), and each
 *     call is the handle-level kernel function;
 *   - process build (user/lib/driver_user.c): each call is a syscall.
 * Moving a driver out of the kernel is a rebuild and a relaunch.
 *
 * Drivers are compiled with only this header (plus <jam/abi.h>,
 * <jam/status.h> and the compiler's freestanding headers) on the include
 * path, and tools/checkdriver.py fails the build if a driver object uses a
 * symbol not declared here. No kmalloc, no kernel structs, no other
 * driver. */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/abi.h>
#include <jam/status.h>

/* ---- start ------------------------------------------------------------------
 * A driver's entry point:  int driver_main(const struct driver_start *s);
 * Its return value is the exit code. The handles it was given are listed by
 * role; roles a driver didn't get are absent (drv_handle returns
 * HANDLE_INVALID). */

#define DR_PCIDEV    1            /* RES_PCI_DEV: its own function (no RIGHT_MANAGE) */
#define DR_SERVE     2            /* channel it serves its own protocol on */
#define DR_DMA       3            /* dma_cap bound to its function (bus master is on) */
#define DR_BAR(n)    (0x10 + (n)) /* RES_MMIO for BAR n (0..5) */
#define DR_IRQ(n)    (0x20 + (n)) /* interrupt object n (MSI 0, or MSI-X n) */

#define DRV_MAX_HANDLES 32

struct driver_start {
    const char *name;             /* the driver's name, for logs */
    uint32_t    nhandles;
    struct { uint32_t role; handle_t h; } handles[DRV_MAX_HANDLES];
};

int driver_main(const struct driver_start *s);
handle_t drv_handle(const struct driver_start *s, uint32_t role);

/* ---- basics ---------------------------------------------------------------- */

void     drv_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Also puts the line into the RESULTS box at the end of the run. */
void     drv_report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint64_t drv_clock_ns(void);                    /* uptime, ns */
status_t drv_sleep_until(uint64_t deadline_ns);
_Noreturn void drv_exit(int code);
/* A driver thread in the same driver (process or kernel process). */
status_t drv_thread_start(const char *name, void (*fn)(void *), void *arg);
/* The driver's own heap (never the kernel's). */
void    *drv_malloc(size_t n);
void     drv_free(void *p);

/* ---- handles, channels, ports, waiting --------------------------------------
 * Same meaning as the syscalls of the same name (abi/syscalls.def). */

status_t drv_handle_close(handle_t h);
status_t drv_handle_duplicate(handle_t h, rights_t rights, handle_t *out);
status_t drv_channel_create(handle_t *a, handle_t *b);
status_t drv_channel_write(handle_t h, const void *bytes, uint32_t n, const handle_t *hs,
                           uint32_t nh);
status_t drv_channel_read(handle_t h, void *bytes, uint32_t cap, uint32_t *actual, handle_t *hs,
                          uint32_t hcap, uint32_t *hactual);
status_t drv_channel_call(handle_t h, void *wbytes, uint32_t wn, void *rbytes, uint32_t rcap,
                          uint32_t *ractual, uint64_t deadline_ns);
/* The same, also receiving up to rhcap handles with the reply (slots for
 * them are reserved before the request is sent, so a full handle table
 * fails the call up front). M7: generated clients of methods that return
 * handles use this. */
status_t drv_channel_call_h(handle_t h, void *wbytes, uint32_t wn, void *rbytes, uint32_t rcap,
                            uint32_t *ractual, handle_t *rh, uint32_t rhcap, uint32_t *rhactual,
                            uint64_t deadline_ns);
status_t drv_port_create(handle_t *out);
status_t drv_port_bind(handle_t port, handle_t obj, uint64_t key, signals_t mask, uint32_t flags);
status_t drv_port_wait(handle_t port, uint64_t deadline_ns, struct port_packet *out);
status_t drv_object_wait_one(handle_t h, signals_t mask, uint64_t deadline_ns,
                             signals_t *observed);

/* ---- memory --------------------------------------------------------------- */

/* drv_vmo_create flags (the same values as the kernel's VMO_CONTIGUOUS /
 * VMO_DMA32, which drivers can't see): memory a device reaches by DMA.
 * Either one uses the driver's DR_DMA capability (no DR_DMA: refused).
 * Contiguous memory is committed (zeroed) at creation, at most 4 MiB. */
#define DRV_VMO_CONTIGUOUS (1u << 0)
#define DRV_VMO_DMA32      (1u << 1)   /* every page below 4 GiB */

status_t drv_vmo_create(uint64_t size, uint32_t flags, handle_t *out);
status_t drv_vmo_read(handle_t vmo, uint64_t off, void *buf, uint64_t len);
status_t drv_vmo_write(handle_t vmo, uint64_t off, const void *buf, uint64_t len);
/* Map into the driver's address space; flags VMAR_READ/WRITE. */
status_t drv_vmo_map(handle_t vmo, uint64_t off, uint64_t len, uint32_t flags, void **addr);
status_t drv_vmo_unmap(void *addr, uint64_t len);
/* DMA: one device address per page into addrs[len / 4096]. */
status_t drv_vmo_pin(handle_t vmo, handle_t dma, uint64_t off, uint64_t len, uint64_t *addrs,
                     uint64_t *pin_id);
/* Needs the dma_cap the pin was made with (anyone else: ERR_ACCESS_DENIED). */
status_t drv_vmo_unpin(handle_t vmo, handle_t dma, uint64_t pin_id);
/* Registers: map [off, off + len) of a BAR resource uncached (VMO_CACHE_*
 * for others); page-granular inside, the pointer is to `off` itself. */
status_t drv_mmio_map(handle_t bar, uint64_t off, uint64_t len, uint32_t cache,
                      volatile void **addr);

/* ---- device --------------------------------------------------------------- */

/* Interrupts (DR_IRQ(n)): bind the object to a port PORT_BIND_PERSISTENT
 * for SIG_INTERRUPT (drv_object_wait_one on it can miss a fire); each
 * packet's signal.count is the fires coalesced into it. drv_interrupt_ack
 * clears the signal and unmasks: for a plain (edge) MSI a fire before the
 * ack is folded into the current packet, so ack BEFORE scanning the
 * device's status, then scan. */
status_t drv_interrupt_ack(handle_t irq);
/* Its own config space (DR_PCIDEV), filtered by the kernel: BARs, the
 * command register's decode and bus-master bits and the MSI/MSI-X
 * capabilities are read-only to drivers (ERR_ACCESS_DENIED on write). */
status_t drv_pci_config_read(handle_t dev, uint32_t off, uint32_t width, uint32_t *value);
status_t drv_pci_config_write(handle_t dev, uint32_t off, uint32_t width, uint32_t value);

/* MMIO accessors (compiler barriers; UC mappings keep device order). */
static inline uint32_t drv_read32(volatile void *base, uint32_t off)
{
    return *(volatile uint32_t *)((volatile uint8_t *)base + off);
}
static inline void drv_write32(volatile void *base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)((volatile uint8_t *)base + off) = v;
}
static inline uint64_t drv_read64(volatile void *base, uint32_t off)
{
    return *(volatile uint64_t *)((volatile uint8_t *)base + off);
}
static inline void drv_write64(volatile void *base, uint32_t off, uint64_t v)
{
    *(volatile uint64_t *)((volatile uint8_t *)base + off) = v;
}
static inline uint8_t drv_read8(volatile void *base, uint32_t off)
{
    return *((volatile uint8_t *)base + off);
}
static inline void drv_write8(volatile void *base, uint32_t off, uint8_t v)
{
    *((volatile uint8_t *)base + off) = v;
}
