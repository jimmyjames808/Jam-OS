/* After a kexec: what the previous kernel left this one (kernel/kexec/
 * jump.c wrote it), read once at boot.
 *
 * The previous kernel's memory is this kernel's RAM now, except two kinds
 * of page its memory map types BOOT_MEM_CRASH_LOG: its crash record (the
 * handoff's record_phys) and its log ring. They are mapped like any RAM
 * but kept out of the allocator until crashlog_init has read them; then
 * they are freed. All of it is untrusted, as anything another kernel left
 * behind: the record must lie in a CRASH_LOG range with its magic,
 * version, size and checksum right, the ring must lie in CRASH_LOG ranges
 * and be a power of two, the position must be in order, the name a name
 * and the message printable.
 *
 * A record that says "reboot" needs nothing more. One that says "panic"
 * has its ring copied once, oldest byte first, into a VMO (a struct
 * crashlog_header, <jam/startup.h>, then the text), which userboot hands
 * init as SR_CRASHLOG; init hands it to logd, which saves it as
 * /data/logs/<name>-crash.txt, and the shell says what happened. The
 * panic also counts for the crash-loop rule (kexec_crash_loop). */
#include <stddef.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/startup.h>
#include <jam/string.h>
#include <jam/vmo.h>
#include "kexec_internal.h"

#define RING_MIN (4ull << 10)
#define RING_MAX (1ull << 20)

_Static_assert(CRASHLOG_NAME == KEXEC_NAME && CRASHLOG_MESSAGE == KEXEC_MESSAGE,
               "the crash log's header carries the record's strings as they are");

static struct vmo *log_vmo;     /* header + text, for userboot (taken once) */
static bool after_panic;        /* this boot was started by a panic */
static uint32_t panics;         /* ... the record's count */

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

/* Terminated within its field, and printable ASCII. */
static bool text_ok(const char *s, size_t cap)
{
    size_t i = 0;
    for (; i < cap && s[i]; i++)
        if (s[i] < 0x20 || s[i] > 0x7e)
            return false;
    return i < cap;
}

/* The record at pa, checked; NULL if it can be used, else why not. */
static const char *check_record(const struct boot_info *bi, uint64_t pa,
                                struct kexec_crash_record *r)
{
    if (!in_crash_log(bi, pa, sizeof(*r)))
        return "it is not where the handoff says";
    memcpy(r, phys_to_virt(pa), sizeof(*r));   /* once: the checks and the use see one copy */
    if (r->magic != KEXEC_RECORD_MAGIC || r->version != KEXEC_RECORD_VERSION ||
        r->size != sizeof(*r))
        return "it is not one";
    if (r->checksum != kexec_struct_sum(r, sizeof(*r),
                                        offsetof(struct kexec_crash_record, checksum)))
        return "its checksum is wrong";
    if (r->kind != KEXEC_RECORD_REBOOT && r->kind != KEXEC_RECORD_PANIC)
        return "it says neither reboot nor panic";
    if (r->ring_size < RING_MIN || r->ring_size > RING_MAX ||
        (r->ring_size & (r->ring_size - 1)) || !in_crash_log(bi, r->ring_phys, r->ring_size))
        return "the log ring is not where it says";
    if (r->panic_at > r->head)
        return "its log positions are out of order";
    if (!name_ok(r->name) || !text_ok(r->message, KEXEC_MESSAGE))
        return "its log name or message is not text";
    return NULL;
}

/* The ring as text, oldest byte first, after a header, into a VMO. */
static status_t copy_ring(const struct kexec_crash_record *r)
{
    uint64_t kept = r->head < r->ring_size ? r->head : r->ring_size;
    uint64_t first = r->head - kept;
    struct crashlog_header h = {
        .magic = CRASHLOG_MAGIC, .version = CRASHLOG_VERSION, .panics = r->panics,
        .text_len = kept, .panic_at = r->panic_at > first ? r->panic_at - first : 0,
        .lost = first, .uptime_ns = r->uptime_ns,
    };
    memcpy(h.name, r->name, sizeof(h.name));
    memcpy(h.message, r->message, sizeof(h.message));
    status_t st = vmo_create(sizeof(h) + kept, 0, &log_vmo);
    if (st == OK)
        st = vmo_write(log_vmo, 0, &h, sizeof(h));
    const char *ring = phys_to_virt(r->ring_phys);
    uint64_t at = first & (r->ring_size - 1);
    uint64_t part = kept < r->ring_size - at ? kept : r->ring_size - at;   /* to the ring's end */
    if (st == OK)
        st = vmo_write(log_vmo, sizeof(h), ring + at, part);
    if (st == OK && kept > part)
        st = vmo_write(log_vmo, sizeof(h) + part, ring, kept - part);
    if (st != OK && log_vmo) {
        kobject_unref(vmo_kobject(log_vmo));
        log_vmo = NULL;
    }
    return st;
}

/* What the record says, read and logged. */
static void read_record(const struct boot_info *bi)
{
    struct kexec_crash_record r;
    const char *why = check_record(bi, bi->kexec_record, &r);
    if (why) {
        kprintf("kexec: the previous kernel's crash record is no use (%s): if it panicked, "
                "its log is lost\n", why);
        return;
    }
    uint64_t ms = r.uptime_ns / 1000000;
    if (r.kind == KEXEC_RECORD_REBOOT) {
        kprintf("kexec: started by a reboot (the previous kernel ran %lu.%03lu s)\n",
                ms / 1000, ms % 1000);
        return;
    }
    after_panic = true;
    panics = r.panics;
    kprintf("kexec: the previous kernel panicked after %lu.%03lu s (panic %u in a row): %s\n",
            ms / 1000, ms % 1000, r.panics, r.message);
    status_t st = copy_ring(&r);
    if (st != OK)
        kprintf("kexec: no memory for its log (%s): it is lost\n", status_str(st));
    else
        kprintf("kexec: its log: %lu bytes, log file \"%s\", for logd to save\n",
                r.head < r.ring_size ? r.head : r.ring_size, r.name);
}

void crashlog_init(const struct boot_info *bi)
{
    if (bi->kexec_record)
        read_record(bi);
    /* Read (or no use): the record's and the ring's pages are free memory now. */
    uint64_t freed = 0;
    for (size_t i = 0; i < bi->memmap_count; i++)
        if (bi->memmap[i].type == BOOT_MEM_CRASH_LOG)
            freed += pmm_add_range(bi->memmap[i].base, bi->memmap[i].length);
    if (freed)
        kprintf("kexec: the previous kernel's record and log ring freed (%lu KiB)\n",
                freed >> 10);
}

bool crashlog_after_panic(void)
{
    return after_panic;
}

uint32_t crashlog_panics(void)
{
    return panics;
}

struct vmo *crashlog_take_vmo(void)
{
    struct vmo *v = log_vmo;
    log_vmo = NULL;
    return v;
}
