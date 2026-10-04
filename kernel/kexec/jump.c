/* kexec's two ways in: a panic and kexec_reboot, both into the stored
 * kernel. Both end in the trampoline (tramp.S) through its alias T, with
 * interrupts off and the other CPUs halted.
 *
 * The panic path takes no lock and allocates nothing: kexec_panic_begin
 * decides (the state, the crash-loop rule, the checksum read through the
 * window's own page-table entries, region.c, and the BSP waiting) before
 * the panic prints a line, so a panic that will jump draws nothing
 * (fbcon_go_dark) and one that won't draws its screen as always, with the
 * reason. The jump itself (kexec_panic_jump) is plain stores into the
 * crash record, one config write per PCI function
 * (pci_panic_bus_master_off), an INIT to the other CPUs, the screen
 * filled with the splash background, and the trampoline.
 *
 * The last three are made on the bootstrap processor. An INIT sent to the
 * BSP doesn't park it as it does an AP: it starts the firmware's reset
 * vector (QEMU) or resets the board, and a CPU merely left halted in this
 * kernel's memory would run whatever the next kernel writes there after
 * an SMI. So the BSP, halted by the NMI like the rest, waits in
 * kexec_halted_wait while a stored kernel exists, and a jump decided on an
 * AP is handed to it (HAND_JUMP): the next kernel always starts on the
 * BSP, as after Limine, and starts every AP itself.
 *
 * A jump decided and then not made would leave the machine dark (the
 * panic draws nothing once it has decided) and halted for good. So the
 * BSP waits for the hand-over at most HANDOVER_WAIT_S, and a fault on the
 * panicking CPU after the decision (debug/panic.c: kexec_panic_failed)
 * gives up too: either way a firmware reset, which loses the log but
 * brings the machine back.
 *
 * The crash record is a page of this kernel's own, typed CRASH_LOG in the
 * stored kernel's memory map with the log ring, its address in the
 * handoff: how this kernel ended (a reboot or a panic), where the log ring
 * is, how far it was written, where the panic's lines start, the panic's
 * message, the name logd gave this boot's log file, the panics in a row
 * and this kernel's uptime. Its fixed fields are written when a kernel is
 * stored, the rest just before the jump, its checksum last. */
#include <stddef.h>
#include <jam/console_svc.h>
#include <jam/fbcon.h>
#include <jam/iommu.h>
#include <jam/ipi.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/lapic.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/pci.h>
#include <jam/percpu.h>
#include <jam/serial.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/x86.h>
#include "kexec_internal.h"

/* The BSP's longest wait for the CPU that decided to jump: it prints the
 * panic first (synchronously on the serial port: a few KiB, well under a
 * second at 115200 baud) and turns bus mastering off. */
#define HANDOVER_WAIT_S 10

/* A page of its own: the next kernel frees exactly this page. */
static union {
    struct kexec_crash_record r;
    uint8_t                   page[PAGE_SIZE];
} rec __attribute__((aligned(PAGE_SIZE)));

static uint64_t panic_at;         /* the log's head when the panic began */
static const char *why_not;       /* kexec_panic_begin said no: why (NULL: no stored kernel) */

/* The BSP's part when another CPU decides (kexec_halted_wait). */
enum { HAND_WAIT, HAND_HALT, HAND_JUMP };
static int bsp_waiting;           /* the BSP is in kexec_halted_wait */
static int handover = HAND_WAIT;  /* what it is to do */
static int test_break;            /* KEXEC_TEST_*: a crash test breaks the jump */

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

bool kexec_crash_loop(bool after_panic, uint32_t panics_before, uint64_t uptime)
{
    if (!after_panic)
        return false;
    return uptime < KEXEC_LOOP_NS || panics_before + 1 >= KEXEC_LOOP_PANICS;
}

static uint64_t uptime(void)
{
    return tsc_hz ? uptime_ns() : 0;   /* no clock yet: as if just started */
}

/* Is what is stored still what was stored? No lock: the other CPUs are
 * halted (region.c). */
static bool intact(void)
{
    return kx_sum_region(true) == kx.sum && kx_tramp_sum() == kx.tramp_sum;
}

/* The jump's CPU, if not this one, is waiting for it (the BSP). */
static bool bsp_ready(void)
{
    return lapic_is_bsp() || __atomic_load_n(&bsp_waiting, __ATOMIC_ACQUIRE);
}

bool kexec_panic_begin(void)
{
    panic_at = klog_head();
    int s = __atomic_load_n(&kx_state, __ATOMIC_ACQUIRE);
    if (s == KX_OFF || s == KX_EMPTY)
        return false;   /* there never was one: the boot log says why */
    if (s != KX_ARMED)
        why_not = s == KX_LOADING ? "the stored kernel was being replaced"
                                  : "a reboot was starting the stored kernel";
    else if (kexec_crash_loop(crashlog_after_panic(), crashlog_panics(), uptime()))
        why_not = uptime() < KEXEC_LOOP_NS
                      ? "this boot started after a panic less than 30 s ago (a crash loop)"
                      : "the third panic in a row (a crash loop)";
    else if (!intact())
        why_not = "the stored kernel's checksum no longer matches (its memory was changed)";
    else if (!bsp_ready())
        why_not = "the boot CPU didn't stop for the jump";
    if (why_not)   /* the BSP, if it waits for the jump, halts instead */
        __atomic_store_n(&handover, HAND_HALT, __ATOMIC_RELEASE);
    return !why_not;
}

const char *kexec_panic_why_not(void)
{
    return why_not;
}

void kexec_message_clean(char *dst, size_t size, const char *src)
{
    if (!size)
        return;
    size_t i = 0;
    for (; src[i] && i + 1 < size; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = c == '\n' ? ' ' : c < 0x20 || c > 0x7e ? '?' : (char)c;
    }
    dst[i] = '\0';
}

void kexec_panic_message(const char *msg)
{
    if (rec.r.message[0])
        return;   /* the first one says what happened */
    kexec_message_clean(rec.r.message, KEXEC_MESSAGE, msg);
}

/* The record's last fields and its checksum. */
static void seal(uint32_t kind)
{
    rec.r.kind = kind;
    rec.r.panics = kind == KEXEC_RECORD_PANIC ? crashlog_panics() + 1 : 0;
    rec.r.uptime_ns = uptime();
    rec.r.head = klog_head();
    rec.r.panic_at = kind == KEXEC_RECORD_PANIC ? panic_at : rec.r.head;
    if (kind != KEXEC_RECORD_PANIC)
        rec.r.message[0] = '\0';
    rec.r.checksum = kexec_struct_sum(&rec.r, sizeof(rec.r),
                                      offsetof(struct kexec_crash_record, checksum));
}

/* On the BSP: the other CPUs, halted by NMI, are sent INIT: they wait
 * for a SIPI from now on, running nothing (the next kernel may reuse the
 * memory they halted in, and starts them itself). The screen turns the
 * splash background. Then into the trampoline at T, which loads the new
 * CR3 and never returns. */
_Noreturn static void jump_here(void)
{
    lapic_send_init_others();
    fbcon_fill_splash_bg();
    void (*tramp)(uint64_t, uint64_t, uint64_t, uint64_t) =
        (void (*)(uint64_t, uint64_t, uint64_t, uint64_t))kx_tramp_va();
    tramp(kx.cr3, kx.entry, kx.handoff, kx.stack_top);
    for (;;)
        hlt();
}

/* No jump after all: a firmware reset rather than a dark, silent hang.
 * A stuck CPU may hold the log's or the serial port's lock; the screen
 * stays dark (the reset follows at once). */
_Noreturn static void give_up(const char *why)
{
    klog_force_unlock();
    fbcon_go_dark();
    serial_panic();
    kprintf("kexec: %s: a firmware reboot instead\n", why);
    machine_reboot();
}

/* Bus mastering off, then the jump: here on the BSP, else by the BSP
 * (waiting in kexec_halted_wait), which sends this CPU INIT too. */
_Noreturn static void jump(void)
{
    pci_panic_bus_master_off();
    iommu_jump_off();   /* the next kernel's memory must not be translated by our tables */
    if (lapic_is_bsp())
        jump_here();
    if (__atomic_load_n(&test_break, __ATOMIC_RELAXED) != KEXEC_TEST_STALL)
        __atomic_store_n(&handover, HAND_JUMP, __ATOMIC_RELEASE);
    for (;;) {
        cli();
        hlt();
    }
}

bool kexec_halted_will_wait(void)
{
    int s = __atomic_load_n(&kx_state, __ATOMIC_ACQUIRE);
    if (!lapic_is_bsp() || (s != KX_ARMED && s != KX_JUMPING))
        return false;
    __atomic_store_n(&bsp_waiting, 1, __ATOMIC_RELEASE);
    return true;
}

void kexec_reset_coming(void)
{
    __atomic_store_n(&handover, HAND_HALT, __ATOMIC_RELEASE);
}

_Noreturn void kexec_halted_wait(void)
{
    uint64_t start = rdtsc();
    for (;;) {
        cli();
        int h = __atomic_load_n(&handover, __ATOMIC_ACQUIRE);
        if (h == HAND_JUMP)
            jump_here();   /* kx and the record are written before HAND_JUMP */
        if (h == HAND_HALT)
            halt_forever();
        if (rdtsc() - start > HANDOVER_WAIT_S * tsc_hz)
            give_up("the CPU that stopped this one didn't hand the jump over within 10 s");
        cpu_relax();
    }
}

_Noreturn void kexec_panic_failed(void)
{
    static int once;
    if (__atomic_exchange_n(&once, 1, __ATOMIC_SEQ_CST))
        halt_forever();   /* the reset itself faulted: nothing more to try */
    give_up("a fault on the panicking CPU after it decided to start the stored kernel");
}

_Noreturn void kexec_panic_jump(void)
{
    static uint8_t *volatile null_page;   /* NULL: the test's page fault */
    if (__atomic_load_n(&test_break, __ATOMIC_RELAXED) == KEXEC_TEST_FAULT)
        null_page[8] = 0;   /* a fault after the decision: the nested panic */
    seal(KEXEC_RECORD_PANIC);
    jump();
}

void kexec_test_break(int how)
{
    __atomic_store_n(&test_break, how, __ATOMIC_RELAXED);
}

status_t kexec_reboot(void)
{
    mutex_lock(&kx_lock);
    int s = __atomic_load_n(&kx_state, __ATOMIC_ACQUIRE);
    if (s != KX_ARMED) {
        mutex_unlock(&kx_lock);
        return s == KX_OFF ? ERR_NOT_SUPPORTED : ERR_BAD_STATE;
    }
    __atomic_store_n(&kx_state, KX_JUMPING, __ATOMIC_RELEASE);
    kprintf("kexec: starting the stored kernel on cpu %u\n", this_cpu()->index);
    serial_set_async(false);   /* the ring written out: what follows is synchronous */
    cli();
    ipi_halt_others();
    /* A halted CPU may have held the log's or the screen's lock; and from
     * here the screen shows nothing but the splash background. */
    klog_force_unlock();
    fbcon_go_dark();
    if (!intact() || !bsp_ready()) {
        kprintf("kexec: %s: a firmware reboot instead\n",
                bsp_ready() ? "the stored kernel's memory was changed"
                            : "the boot CPU didn't stop for the jump");
        machine_reboot();
    }
    seal(KEXEC_RECORD_REBOOT);
    jump();
}
