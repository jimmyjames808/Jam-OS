/* A crash kernel's side of kexec: the crashed kernel's log in, the final
 * screen out.
 *
 * The crashed kernel's memory is foreign here (its memory map says so,
 * and nothing maps it) except two kinds of page, typed
 * BOOT_MEM_CRASH_LOG and mapped read-only by vmm_init: its crash record
 * (crashlog=<phys> on the command line, kexec/jump.c) and its log ring.
 * All of it is untrusted, as anything a dying kernel left behind: the
 * record must lie in a CRASH_LOG range with its magic, version, size and
 * checksum right, the ring must lie in CRASH_LOG ranges and be a power of
 * two, the positions must be in order, the name must be a name. Then the
 * ring is copied once, oldest byte first, into a VMO (a struct
 * crashlog_header, <jam/startup.h>, then the text), which userboot hands
 * init as SR_CRASHLOG and init hands logd to save (`logd crash`).
 *
 * When init has ended, crashlog_finish draws the last screen: the crashed
 * kernel's panic lines (from the panic's first byte to its copy of the log
 * tail), then the RESULTS box (logd's line says where the log went, or why
 * it didn't), then a halt or, after crash_reboot=<s> seconds (the crashed
 * kernel's panic_reboot), a firmware reboot into the normal kernel. */
#include <stddef.h>
#include <jam/cmdline.h>
#include <jam/fbcon.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/report.h>
#include <jam/startup.h>
#include <jam/string.h>
#include <jam/vmo.h>
#include <jam/x86.h>
#include "kexec_internal.h"

#define RING_MIN (4ull << 10)
#define RING_MAX (1ull << 20)

extern const char jamos_version[];

static struct vmo *log_vmo;      /* header + text, for userboot */
static char *text;               /* the text (kernel memory), for the last screen */
static uint64_t text_len;        /* its bytes */
static uint64_t panic_off;       /* the panic's lines: [panic_off, panic_end) of text */
static uint64_t panic_end;

/* Is [pa, pa + len) inside one BOOT_MEM_CRASH_LOG range? */
static bool in_crash_log(const struct boot_info *bi, uint64_t pa, uint64_t len)
{
    for (size_t i = 0; i < bi->memmap_count; i++) {
        const struct boot_mem_region *r = &bi->memmap[i];
        if (r->type == BOOT_MEM_CRASH_LOG && pa >= r->base && len <= r->length &&
            pa - r->base <= r->length - len)
            return true;
    }
    return false;
}

static bool name_ok(const char *n)
{
    size_t i = 0;
    for (; i < KEXEC_NAME && n[i]; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
            return false;
    }
    return i < KEXEC_NAME;
}

/* The record at pa, checked; NULL with *why if it can't be used. */
static const char *check_record(const struct boot_info *bi, uint64_t pa,
                                struct kexec_crash_record *r)
{
    if (!pa || !in_crash_log(bi, pa, sizeof(*r)))
        return "no crash record where the command line says";
    memcpy(r, phys_to_virt(pa), sizeof(*r));   /* once: the checks and the use see one copy */
    if (r->magic != KEXEC_RECORD_MAGIC || r->version != KEXEC_RECORD_VERSION ||
        r->size != sizeof(*r))
        return "the crash record is not one";
    if (r->checksum != kexec_struct_sum(r, sizeof(*r),
                                        offsetof(struct kexec_crash_record, checksum)))
        return "the crash record's checksum is wrong";
    if (r->ring_size < RING_MIN || r->ring_size > RING_MAX ||
        (r->ring_size & (r->ring_size - 1)) || !in_crash_log(bi, r->ring_phys, r->ring_size))
        return "the log ring is not where the record says";
    if (r->panic_at > r->head || (r->tail_at && (r->tail_at < r->panic_at ||
                                                 r->tail_at > r->head)))
        return "the record's log positions are out of order";
    if (!name_ok(r->name))
        return "the record's log name is not a name";
    return NULL;
}

/* The ring as text, oldest byte first, after a header, into a VMO. */
static status_t copy_ring(const struct kexec_crash_record *r)
{
    uint64_t kept = r->head < r->ring_size ? r->head : r->ring_size;
    uint64_t first = r->head - kept;
    struct crashlog_header h = {
        .magic = CRASHLOG_MAGIC, .version = CRASHLOG_VERSION, .text_len = kept,
        .panic_at = r->panic_at > first ? r->panic_at - first : 0, .lost = first,
    };
    memcpy(h.name, r->name, sizeof(h.name));
    text = kmalloc(kept ? kept : 1);
    if (!text)
        return ERR_NO_MEMORY;
    const char *ring = phys_to_virt(r->ring_phys);
    for (uint64_t i = 0; i < kept; i++)
        text[i] = ring[(first + i) & (r->ring_size - 1)];
    text_len = kept;
    panic_off = h.panic_at;
    panic_end = r->tail_at > first ? r->tail_at - first : kept;
    status_t st = vmo_create(sizeof(h) + kept, 0, &log_vmo);
    if (st == OK)
        st = vmo_write(log_vmo, 0, &h, sizeof(h));
    if (st == OK)
        st = vmo_write(log_vmo, sizeof(h), text, kept);
    if (st != OK && log_vmo) {
        kobject_unref(vmo_kobject(log_vmo));
        log_vmo = NULL;
    }
    return st;
}

void crashlog_init(const struct boot_info *bi)
{
    kprintf("crash kernel: the kernel before this one panicked; this one saves its log\n");
    struct kexec_crash_record r;
    const char *why = check_record(bi, cmdline_get_u64("crashlog", 0, 0), &r);
    if (why) {
        report("crash: the crashed kernel's log can't be read: %s", why);
        return;
    }
    status_t st = copy_ring(&r);
    if (st != OK) {
        report("crash: no memory for the crashed kernel's log (%s)", status_str(st));
        return;
    }
    kprintf("crash kernel: its log: %lu bytes (%lu before them lost), the panic at byte %lu, "
            "log file \"%s\"\n", text_len, r.head - text_len, panic_off, r.name);
}

struct vmo *crashlog_vmo(void)
{
    if (log_vmo)
        kobject_ref(vmo_kobject(log_vmo));
    return log_vmo;
}

_Noreturn void crashlog_finish(void)
{
    cli();   /* the last screen: nothing else runs on this, the only CPU */
    fbcon_set_colors(0xffffff, 0x8b0000);
    fbcon_clear();
    kprintf("\n  *** JAM OS: THE KERNEL BEFORE THIS ONE PANICKED *** (this is the crash "
            "kernel; its panic screen was:)\n");
    if (text && panic_end > panic_off)
        klog_write_raw(text + panic_off, panic_end - panic_off);   /* timestamped already */
    else
        kprintf("\n  (its log could not be read: see the RESULTS box)\n");
    report_print(jamos_version);
    panic_halt_or_reboot(cmdline_get_u64("crash_reboot", 0, 0));
}
