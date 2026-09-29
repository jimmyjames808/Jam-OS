/* The kernel services for the console and the shell
 * (<jam/console_svc.h>): kernel log readers, the screen hand-off, COM1
 * input and debug_command. The system call halves are thin wrappers
 * (kernel/abi/sysc_console.c); the QEMU shell test drives them for real. */
#include <jam/acpi.h>
#include <jam/console_svc.h>
#include <jam/fbcon.h>
#include <jam/handle.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/pci.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/serial.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/userboot.h>
#include <jam/vmo.h>


static bool contains(const char *hay, size_t n, const char *needle)
{
    size_t l = strlen(needle);
    for (size_t i = 0; i + l <= n; i++)
        if (!memcmp(hay + i, needle, l))
            return true;
    return false;
}

/* The ring copy: positions, wrap-around, a reader that fell behind. */
KTEST(console_klog_ring_wraps)
{
    char ring[16], buf[32];
    uint64_t first;
    for (int i = 0; i < 16; i++)
        ring[i] = (char)('a' + i);   /* as if bytes 16..31 were written: 'a' is byte 16 */
    /* 32 bytes written: only 16..31 are kept. */
    KT_EQ(klog_ring_copy(ring, 16, 32, 0, buf, sizeof(buf), &first), 16);
    KT_EQ(first, 16);
    KT_EQ(buf[0], 'a');
    KT_EQ(buf[15], 'p');
    /* From the middle, across the wrap point (byte 28 sits at index 12). */
    KT_EQ(klog_ring_copy(ring, 16, 32, 28, buf, sizeof(buf), &first), 4);
    KT_EQ(first, 28);
    KT_EQ(buf[0], 'm');
    /* 40 written: byte 36 is at index 4 ('e'), and 24..39 are kept. */
    KT_EQ(klog_ring_copy(ring, 16, 40, 30, buf, 8, &first), 8);
    KT_EQ(first, 30);
    KT_EQ(buf[0], 'o');   /* index 14 */
    KT_EQ(buf[2], 'a');   /* index 0: wrapped */
    /* Too old: moved up to the oldest kept byte. */
    KT_EQ(klog_ring_copy(ring, 16, 40, 3, buf, 4, &first), 4);
    KT_EQ(first, 24);
    /* Past the end: nothing, and the end. */
    KT_EQ(klog_ring_copy(ring, 16, 40, 1000, buf, 4, &first), 0);
    KT_EQ(first, 40);
}

/* A reader sees a new line, is signalled within a tick or two, and is
 * quiet again once it has read everything. */
KTEST(console_klog_reader)
{
    struct kobject *r;
    KT_EQ(klog_reader_create(NULL, &r), OK);
    uint64_t start = klog_head();
    char buf[512];
    uint64_t first;
    /* Catch up first. */
    while (klog_reader_read(r, start, buf, sizeof(buf), &first) == sizeof(buf))
        start = first + sizeof(buf);
    start = klog_head();
    klog_reader_read(r, start, buf, sizeof(buf), &first);
    kprintf("console_klog_reader: marker 7f3a\n");
    signals_t seen = 0;
    KT_EQ(object_wait_one(r, SIG_READABLE, uptime_ns() + 1000 * NS_PER_MS, &seen), OK);
    size_t n = klog_reader_read(r, start, buf, sizeof(buf), &first);
    KT_EQ(first, start);
    KT_ASSERT(contains(buf, n, "marker 7f3a"));
    /* Everything read: not readable, unless something else logged since. */
    if (klog_head() == first + n)
        KT_EQ(kobject_signals(r) & SIG_READABLE, 0);
    /* A reader behind the end is raised again by the tick. */
    klog_reader_read(r, 0, buf, 1, &first);
    KT_EQ(object_wait_one(r, SIG_READABLE, uptime_ns() + 1000 * NS_PER_MS, NULL), OK);
    kobject_unref(r);
}

/* A read that runs into a gap (the log overwrote the text it
 * was about to copy, between two 512-byte steps) returns the text up to
 * the gap, and the reader's SIG_READABLE comes from where THAT ends: more
 * log after it, so readable. The next read resumes at the oldest text
 * kept, and reading to the end clears it. The sink writes more than the
 * whole ring (KLOG_SIZE) after the first step. */
struct gap_sink {
    char    *buf;     /* where the sink copies text to */
    unsigned calls;   /* times the sink was called */
};

static status_t flood_sink(void *ctx, uint64_t off, const char *text, size_t n)
{
    struct gap_sink *g = ctx;
    memcpy(g->buf + off, text, n);
    if (g->calls++ == 0) {
        uint64_t h0 = klog_head();
        for (unsigned i = 0; klog_head() - h0 < KLOG_SIZE + 4096; i++)
            kprintf("console_klog_read_after_gap: filler line %4u "
                    "................................................................\n", i);
    }
    return OK;
}

static status_t null_sink(void *ctx, uint64_t off, const char *text, size_t n)
{
    (void)ctx, (void)off, (void)text, (void)n;
    return OK;
}

KTEST(console_klog_read_after_gap)
{
    struct kobject *r;
    KT_EQ(klog_reader_create(NULL, &r), OK);
    uint64_t pos = klog_head();
    for (int i = 0; i < 8; i++)   /* > 512 bytes: the first step is full */
        kprintf("console_klog_read_after_gap: before the gap, line %d ..................\n", i);
    char *buf = kmalloc(4096);
    KT_ASSERT(buf);
    struct gap_sink g = { buf, 0 };
    uint64_t first, done;
    KT_EQ(klog_reader_read_to(r, pos, 4096, flood_sink, &g, &first, &done), OK);
    KT_EQ(first, pos);
    KT_EQ(done, 512);   /* one step, then the gap */
    KT_ASSERT(contains(buf, done, "before the gap, line 0"));
    KT_ASSERT(first + done < klog_head());
    KT_ASSERT(kobject_signals(r) & SIG_READABLE);   /* text remains: readable */

    /* Resume: past the gap (at the oldest byte kept), then to the end. */
    uint64_t at = first + done, f2;
    KT_EQ(klog_reader_read_to(r, at, 4096, null_sink, NULL, &f2, &done), OK);
    KT_ASSERT(f2 > at);
    KT_EQ(done, 4096);
    KT_ASSERT(kobject_signals(r) & SIG_READABLE);
    at = f2 + done;
    for (int i = 0; i < 64; i++) {
        KT_EQ(klog_reader_read_to(r, at, 64 * 1024, null_sink, NULL, &f2, &done), OK);
        KT_EQ(f2, at);
        at = f2 + done;
        if (at == klog_head())
            break;
    }
    /* At the end: not readable, unless something else logged meanwhile
     * (live: user space; the tick raises it again then). */
    if (klog_head() == at)
        KT_EQ(kobject_signals(r) & SIG_READABLE, 0);
    kfree(buf);
    kobject_unref(r);
}

/* Take the screen, fail a second take, give it back by closing the
 * owner's last handle, and again by tearing down a handle table that holds
 * it (what a dying owner's process does). */
KTEST(console_screen_take_release)
{
    struct boot_framebuffer fb;
    if (!fbcon_geometry(&fb)) {
        kprintf("console_screen_take_release: no framebuffer: skipped\n");
        return;
    }
    struct fb_info info;
    struct vmo *v, *v2;
    struct kobject *owner, *owner2;
    if (fbcon_is_taken()) {
        /* `ktest` from the shell: the console owns the screen. */
        KT_EQ(screen_take(NULL, &info, &v, &owner), ERR_BAD_STATE);
        kprintf("console_screen_take_release: the console owns the screen: only that checked\n");
        return;
    }
    KT_EQ(screen_take(NULL, &info, &v, &owner), OK);
    KT_ASSERT(fbcon_is_taken());
    KT_EQ(info.width, fb.width);
    KT_EQ(info.pitch, fb.pitch);
    KT_ASSERT(info.size >= (uint64_t)fb.pitch * fb.height);
    KT_EQ(screen_take(NULL, &info, &v2, &owner2), ERR_BAD_STATE);
    kprintf("console_screen_take_release: this line is kept, not drawn\n");
    KT_ASSERT(fbcon_is_taken());
    kobject_unref(vmo_kobject(v));
    /* Handle count 1 -> 0 gives it back. */
    struct khandle kh = khandle_from_new(owner, RIGHTS_BASIC);
    khandle_release(&kh);
    KT_ASSERT(!fbcon_is_taken());

    /* The holder dies: its handle table goes, and with it the screen. */
    KT_EQ(screen_take(NULL, &info, &v, &owner), OK);
    struct handle_table t;
    handle_table_init(&t);
    struct khandle k2 = khandle_from_new(owner, RIGHTS_BASIC);
    handle_t h;
    KT_EQ(handle_insert(&t, &k2, &h), OK);
    struct khandle k3 = khandle_from_new(vmo_kobject(v), RIGHTS_BASIC);
    KT_EQ(handle_insert(&t, &k3, &h), OK);
    KT_ASSERT(fbcon_is_taken());
    handle_table_destroy(&t);
    KT_ASSERT(!fbcon_is_taken());

    /* An owner that never got a handle. */
    KT_EQ(screen_take(NULL, &info, &v, &owner), OK);
    kobject_unref(vmo_kobject(v));
    screen_owner_drop(owner);
    KT_ASSERT(!fbcon_is_taken());
}

/* COM1 input: one reader; bytes in -> readable -> read -> quiet; a full
 * ring drops (and counts); closing lets the next reader in. */
KTEST(console_serial_rx_ring)
{
    struct kobject *in, *in2;
    status_t st = serial_in_create(NULL, &in);
    if (st == ERR_NOT_FOUND) {
        kprintf("console_serial_rx_ring: no UART: skipped\n");
        return;
    }
    if (st == ERR_BAD_STATE) {
        /* `ktest` from the shell: serialin reads COM1 (one reader at a time). */
        kprintf("console_serial_rx_ring: COM1 has a reader (serialin): only that checked\n");
        return;
    }
    KT_EQ(st, OK);
    KT_EQ(serial_in_create(NULL, &in2), ERR_BAD_STATE);
    struct khandle kh = khandle_from_new(in, RIGHTS_BASIC);
    KT_EQ(kobject_signals(in) & SIG_READABLE, 0);
    serial_rx_inject("abc", 3);
    KT_EQ(object_wait_one(in, SIG_READABLE, uptime_ns() + 100 * NS_PER_MS, NULL), OK);
    char buf[64];
    KT_EQ(serial_in_read(in, buf, 2), 2);
    KT_ASSERT(kobject_signals(in) & SIG_READABLE);   /* 'c' still waiting */
    KT_EQ(serial_in_read(in, buf + 2, sizeof(buf) - 2), 1);
    KT_ASSERT(!memcmp(buf, "abc", 3));
    KT_EQ(kobject_signals(in) & SIG_READABLE, 0);
    KT_EQ(serial_in_read(in, buf, sizeof(buf)), 0);

    /* Overflow: the ring holds 4096; the rest is dropped and counted. */
    uint64_t dropped = serial_rx_dropped();
    char *big = kmalloc(5000);
    KT_ASSERT(big);
    memset(big, 'x', 5000);
    serial_rx_inject(big, 5000);
    size_t total = 0, n;
    while ((n = serial_in_read(in, big, 5000)) > 0)
        total += n;
    kfree(big);
    KT_EQ(total, 4096);
    KT_EQ(serial_rx_dropped() - dropped, 5000 - 4096);

    khandle_release(&kh);   /* zero handles: the receive side stops */
    serial_rx_inject("zz", 2);   /* nobody reading: ignored */
    KT_EQ(serial_in_create(NULL, &in2), OK);
    struct khandle kh2 = khandle_from_new(in2, RIGHTS_BASIC);
    KT_EQ(serial_in_read(in2, buf, sizeof(buf)), 0);   /* nothing left over */
    khandle_release(&kh2);
}

/* debug_command: the parse, a real run in the kernel thread; run from the
 * shell (inside a debug_command), a second one is refused. */
KTEST(console_debug_command)
{
    KT_EQ(dbgcmd_check("ktest", 5), OK);
    KT_EQ(dbgcmd_check("ktest m4", 8), OK);
    KT_EQ(dbgcmd_check("bench", 5), OK);
    KT_EQ(dbgcmd_check("bench x", 7), ERR_INVALID_ARGS);
    KT_EQ(dbgcmd_check("stress 30", 9), OK);
    KT_EQ(dbgcmd_check("stress 0", 8), ERR_INVALID_ARGS);
    KT_EQ(dbgcmd_check("stress 601", 10), ERR_INVALID_ARGS);
    KT_EQ(dbgcmd_check("stress", 6), ERR_INVALID_ARGS);
    KT_EQ(dbgcmd_check("devices", 7), OK);
    KT_EQ(dbgcmd_check("ps", 2), OK);
    KT_EQ(dbgcmd_check("mem", 3), OK);
    KT_EQ(dbgcmd_check("panic", 5), OK);
    KT_EQ(dbgcmd_check("kill x", 6), OK);
    KT_EQ(dbgcmd_check("kill", 4), ERR_INVALID_ARGS);
    KT_EQ(dbgcmd_check("kill a b", 8), ERR_INVALID_ARGS);
    KT_EQ(dbgcmd_check("reboot", 6), ERR_NOT_SUPPORTED);
    KT_EQ(dbgcmd_check("", 0), ERR_INVALID_ARGS);
    KT_EQ(dbgcmd_run("rm -rf", 6, NULL), ERR_NOT_SUPPORTED);
    if (dbgcmd_busy()) {
        /* This is `ktest` from the shell: one command at a time. */
        KT_EQ(dbgcmd_run("devices", 7, NULL), ERR_BAD_STATE);
        return;
    }
    KT_EQ(dbgcmd_run("devices", 7, NULL), (int64_t)pci_count());
    KT_EQ(dbgcmd_run("ps", 2, NULL), 0);
    /* A job tree to list. */
    struct job *root, *j;
    KT_EQ(userboot_root_job(&root), OK);
    KT_EQ(job_create(root, &j), OK);
    KT_EQ(dbgcmd_run("ps", 2, root), 0);
    KT_EQ(dbgcmd_run("kill nobody", 11, root), ERR_NOT_FOUND);
    job_unref(j);
    job_unref(root);
    KT_ASSERT(!dbgcmd_busy());
}

/* The reset methods are described (the reset itself is the QEMU shell
 * test: `reboot` must end QEMU). */
KTEST(console_reboot_describe)
{
    char buf[128];
    reboot_describe(buf, sizeof(buf));
    kprintf("console_reboot_describe: %s\n", buf);
    KT_ASSERT(contains(buf, strlen(buf), "0xCF9"));
    /* A reset register in memory is written through an uncached mapping. */
    if (acpi.has_reset_reg && acpi.reset_reg.space == 0) {
        KT_ASSERT(acpi.reset_mmio);
        KT_EQ(vmm_translate(vmm_kernel_pml4(), (uint64_t)acpi.reset_mmio), acpi.reset_reg.address);
        KT_ASSERT(!memcmp(vmm_cache_type(vmm_kernel_pml4(), (uint64_t)acpi.reset_mmio), "UC", 2));
    } else {
        KT_ASSERT(!acpi.reset_mmio);
    }
}
