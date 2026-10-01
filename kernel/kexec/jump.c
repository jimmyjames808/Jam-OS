/* kexec's jumps: the panic path into the crash kernel, and kexec_reboot
 * into a reboot image. Both end in the trampoline (tramp.S) through its
 * alias T, with interrupts off and the other CPUs halted.
 *
 * The panic path, from the decision on (kexec_panic_jump), takes no lock,
 * allocates nothing and calls only what reads memory it owns: the crash
 * record's stores, the checksum through the window's own page-table
 * entries (region.c), one config write per PCI function
 * (pci_panic_bus_master_off) and the jump. The lines it prints come
 * before the decision, or after a failed check, when the panic goes on
 * as it always did.
 *
 * The crash record is a page of this kernel's own, given to the crash
 * kernel read-only (its memory map's BOOT_MEM_CRASH_LOG, its command
 * line's crashlog=<phys>): where the log ring is, how far it was written,
 * where the panic's lines start, and the name logd gave this boot's log
 * file. Its fixed fields are written when a crash kernel is loaded, the
 * rest just before the jump, its checksum last. */
#include <stddef.h>
#include <jam/console_svc.h>
#include <jam/fbcon.h>
#include <jam/ipi.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/pci.h>
#include <jam/percpu.h>
#include <jam/serial.h>
#include <jam/x86.h>
#include "kexec_internal.h"

/* A page of its own: the crash kernel maps exactly this page. */
static union {
    struct kexec_crash_record r;
    uint8_t                   page[PAGE_SIZE];
} rec __attribute__((aligned(PAGE_SIZE)));

static uint64_t panic_at, tail_at;   /* log positions the panic noted */

uint64_t kx_record_phys(void)
{
    rec.r.magic = KEXEC_RECORD_MAGIC;
    rec.r.version = KEXEC_RECORD_VERSION;
    rec.r.size = sizeof(rec.r);
    rec.r.ring_phys = kx_kernel_phys(klog_ring());
    rec.r.ring_size = KLOG_SIZE;
    return kx_kernel_phys(&rec);
}

status_t kexec_set_log_name(const char *name, size_t len)
{
    if (!len || len >= KEXEC_NAME)
        return ERR_INVALID_ARGS;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_';
        if (!ok)
            return ERR_INVALID_ARGS;
    }
    /* One writer (logd), and the panic path reads it: the NUL goes first,
     * so a reader racing a rename sees a shorter name, never no end. */
    __atomic_store_n(&rec.r.name[len], '\0', __ATOMIC_RELAXED);
    for (size_t i = 0; i < len; i++)
        __atomic_store_n(&rec.r.name[i], name[i], __ATOMIC_RELAXED);
    return OK;
}

void kexec_panic_begin(void)
{
    panic_at = klog_head();
}

void kexec_panic_tail(void)
{
    tail_at = klog_head();
}

/* Into the trampoline at T, which loads the new CR3 and never returns. */
_Noreturn static void jump(void)
{
    void (*tramp)(uint64_t, uint64_t, uint64_t, uint64_t) =
        (void (*)(uint64_t, uint64_t, uint64_t, uint64_t))kx_tramp_va();
    tramp(kx.cr3, kx.entry, kx.handoff, kx.stack_top);
    for (;;)
        hlt();
}

/* Is what is loaded still what was loaded? No lock: the other CPUs are
 * halted (region.c). */
static bool intact(void)
{
    return kx_sum_region(true) == kx.sum && kx_tramp_sum() == kx.tramp_sum;
}

void kexec_panic_jump(void)
{
    int s = __atomic_load_n(&kx_state, __ATOMIC_ACQUIRE);
    if (s == KX_OFF || s == KX_EMPTY)
        return;   /* there never was one: the boot log says why */
    if (s != KX_ARMED) {
        kprintf("\ncrash kernel: none (%s): this log is not saved\n",
                s == KX_IMAGE ? "a kexec reboot image is loaded instead" : "being replaced");
        return;
    }
    kprintf("\ncrash kernel: checking it, then starting it to save this log...\n");

    /* The decision: from here no lock and no allocation. */
    rec.r.head = klog_head();
    rec.r.panic_at = panic_at;
    rec.r.tail_at = tail_at;
    rec.r.checksum = kexec_struct_sum(&rec.r, sizeof(rec.r),
                                      offsetof(struct kexec_crash_record, checksum));
    if (!intact()) {
        kprintf("crash kernel: checksum mismatch (its reserved memory was changed): not "
                "used, this log is not saved\n");
        return;
    }
    pci_panic_bus_master_off();
    jump();
}

status_t kexec_reboot(void)
{
    mutex_lock(&kx_lock);
    if (__atomic_load_n(&kx_state, __ATOMIC_ACQUIRE) != KX_IMAGE) {
        mutex_unlock(&kx_lock);
        return ERR_BAD_STATE;
    }
    __atomic_store_n(&kx_state, KX_JUMPING, __ATOMIC_RELEASE);
    kprintf("kexec: starting the loaded kernel on cpu %u\n", this_cpu()->index);
    serial_set_async(false);   /* the ring written out: what follows is synchronous */
    cli();
    ipi_halt_others();
    /* A halted CPU may have held the log's or the screen's lock. */
    klog_force_unlock();
    fbcon_force_unlock();
    if (!intact()) {
        kprintf("kexec: the loaded kernel's memory was changed: a firmware reboot instead\n");
        machine_reboot();
    }
    pci_panic_bus_master_off();
    jump();
}
