/* What the files of kernel/kexec share (the public side is <jam/kexec.h>):
 *   region.c    the reservation, the window that writes and reads the
 *               region, the trampoline's alias, the checksum
 *   memmap.c    memory maps: one range overlaid with a type, merging
 *   image.c     a kernel image laid out in the region: segments, modules,
 *               page tables, the handoff
 *   load.c      the stored kernel at boot, and its replacement (kexec_load);
 *               the next kernel's command line
 *   jump.c      the panic path and kexec_reboot: the record, the checks,
 *               bus mastering off, the jump (tramp.S)
 *   crashlog.c  the next kernel's side: the record and the log it got
 *
 * Lock: kx_lock (a mutex) around every state change and every use of the
 * window. The panic path takes nothing: it reads kx_state, and from ARMED
 * on uses kx (written before ARMED was stored, with release order) and
 * the window's page table directly. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/boot.h>
#include <jam/kexec.h>
#include <jam/kexec_handoff.h>
#include <jam/sched.h>
#include <jam/status.h>

#define KX_WINDOW    (2ull << 20)          /* the window: 2 MiB, one page table */
#define KX_STACK     (64ull << 10)         /* the new kernel's first stack (Limine's size) */
#define KX_TABLES    128u                  /* pages kept for its page tables */
#define KX_KERNEL_LO 0xffffffff80000000ull /* a kernel's segments lie in [LO, HI] */
#define KX_KERNEL_HI 0xfffffffffffff000ull
#define KX_KERNEL_MAX (64ull << 20)        /* its image's span at most */

enum kx_state { KX_OFF, KX_EMPTY, KX_LOADING, KX_ARMED, KX_JUMPING };

/* The region and what is loaded in it. Written with kx_lock held while the
 * state is LOADING; read by the panic path once it is ARMED. */
struct kx_region {
    uint64_t base, size;   /* physical; 2 MiB aligned, a multiple of 2 MiB */
    uint64_t loaded;       /* bytes from base the checksum covers */
    uint64_t sum;          /* their checksum (kexec_checksum, seed 0) */
    uint64_t tramp_sum;    /* the trampoline page's */
    uint64_t cr3;          /* the jump: the new kernel's PML4 (physical) */
    uint64_t entry;        /* its _start (virtual) */
    uint64_t handoff;      /* the handoff, in the new HHDM */
    uint64_t stack_top;    /* the stack's top, in the new HHDM */
};

extern struct kx_region kx;
extern int kx_state;                     /* enum kx_state; atomic */
extern struct mutex kx_lock;
extern const struct boot_info *kx_boot;  /* this kernel's boot info (static memory) */

/* ---- region.c ---- */

/* At boot, once: the window's virtual range and page table, and the
 * trampoline's alias T (kexec_tramp's page, read-execute). */
void     kx_window_init(void);
/* Into the region at off (kx_lock held): copy len bytes, or zero them.
 * Each 2 MiB is mapped, written and unmapped with a TLB shootdown. */
void     kx_write(uint64_t off, const void *src, uint64_t len);
void     kx_zero(uint64_t off, uint64_t len);
/* The checksum of [base, base + loaded): with kx_lock held (nolock false),
 * or on the panic path (nolock true: the window's entries stored directly,
 * only this CPU's TLB flushed, so the other CPUs must be stopped). */
uint64_t kx_sum_region(bool nolock);
/* The trampoline page: its alias T in this kernel (and in every image's
 * page tables), its physical address, and its checksum. */
uint64_t kx_tramp_va(void);
uint64_t kx_tramp_phys(void);
uint64_t kx_tramp_sum(void);
/* The physical address of something in this kernel's image (a static). */
uint64_t kx_kernel_phys(const void *va);

/* ---- image.c ---- */

/* What goes into the region. */
struct kx_image {
    const void *kernel;        /* the kernel ELF file, in kernel memory */
    uint64_t    kernel_size;
    const void *bootfs;        /* the bootfs image */
    uint64_t    bootfs_size;
    const char *cmdline;       /* NUL-terminated, < KEXEC_CMDLINE */
};

/* Check the image, build what it needs, then (state LOADING from then on)
 * write it into the region and fill kx; kx_lock held. A failure comes
 * before anything is written, so what was loaded stays loaded:
 * ERR_INVALID_ARGS (a bad ELF, bootfs or command line), ERR_NO_RESOURCES
 * (too big for the region, or a memory map or page tables that don't
 * fit), ERR_NO_MEMORY. Logs why it failed. */
status_t kx_build(const struct kx_image *im);

/* ---- jump.c ---- */

/* The crash record's physical address (for the next kernel's handoff and
 * memory map), with its fixed fields filled in. */
uint64_t kx_record_phys(void);
