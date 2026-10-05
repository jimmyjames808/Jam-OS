/* kexec: the checksum, memory-map splitting, the handoff's check (every
 * field a corrupted handoff could get wrong), the region unmapped from
 * every kernel page table, the stored kernel intact, a refused image
 * changing nothing, the next kernel's command line and the crash-loop
 * rule. */
#include <stddef.h>
#include <jam/kexec.h>
#include <jam/kexec_handoff.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/string.h>
#include <jam/vmo.h>

/* x86-64 page-table entry bits, for the walk below. */
#define PTE_P    (1ull << 0)
#define PTE_PS   (1ull << 7)
#define PTE_ADDR 0x000ffffffffff000ull

KTEST(kexec_checksum_words)
{
    uint64_t buf[64];
    for (unsigned i = 0; i < 64; i++)
        buf[i] = i * 0x9e3779b97f4a7c15ull;
    uint64_t sum = kexec_checksum(buf, sizeof(buf), KEXEC_SUM_SEED);
    KT_EQ(kexec_checksum(buf, sizeof(buf), KEXEC_SUM_SEED), sum);
    for (unsigned i = 0; i < 64; i += 7) {
        uint8_t *b = (uint8_t *)&buf[i] + (i % 8);
        *b ^= 1;   /* one bit of one word */
        KT_ASSERT(kexec_checksum(buf, sizeof(buf), KEXEC_SUM_SEED) != sum);
        *b ^= 1;
    }
    KT_EQ(kexec_checksum(buf, sizeof(buf), KEXEC_SUM_SEED), sum);
    /* Two halves chained are the whole. */
    uint64_t half = kexec_checksum(buf, sizeof(buf) / 2, KEXEC_SUM_SEED);
    KT_EQ(kexec_checksum(buf + 32, sizeof(buf) / 2, half), sum);
    /* A struct's own checksum field counts as 0. */
    uint64_t at = 8 * 5, keep = buf[5];
    uint64_t s1 = kexec_struct_sum(buf, sizeof(buf), at);
    buf[5] = 12345;
    KT_EQ(kexec_struct_sum(buf, sizeof(buf), at), s1);
    buf[5] = 0;
    KT_EQ(kexec_checksum(buf, sizeof(buf), KEXEC_SUM_SEED), s1);
    buf[5] = keep;
}

static bool map_is(const struct boot_mem_region *m, size_t n, const uint64_t (*want)[3],
                   size_t wn)
{
    if (n != wn)
        return false;
    for (size_t i = 0; i < n; i++)
        if (m[i].base != want[i][0] || m[i].length != want[i][1] || m[i].type != want[i][2])
            return false;
    return true;
}

KTEST(kexec_memmap_overlay)
{
#define U BOOT_MEM_USABLE
#define R BOOT_MEM_RESERVED
#define F BOOT_MEM_FOREIGN
    struct boot_mem_region m[8] = {
        { 0x0000, 0x1000, R }, { 0x1000, 0x9000, U }, { 0xa000, 0x2000, R },
    };
    size_t n = 3;
    /* Inside one entry: split into three. */
    KT_EQ(kexec_memmap_overlay(m, &n, 8, 0x3000, 0x2000, F), OK);
    static const uint64_t a[][3] = { { 0, 0x1000, R }, { 0x1000, 0x2000, U },
                                     { 0x3000, 0x2000, F }, { 0x5000, 0x5000, U },
                                     { 0xa000, 0x2000, R } };
    KT_ASSERT(map_is(m, n, a, 5));
    /* Across two entries, to the end of the second's. */
    KT_EQ(kexec_memmap_overlay(m, &n, 8, 0x8000, 0x4000, F), OK);
    static const uint64_t b[][3] = { { 0, 0x1000, R }, { 0x1000, 0x2000, U },
                                     { 0x3000, 0x2000, F }, { 0x5000, 0x3000, U },
                                     { 0x8000, 0x2000, F }, { 0xa000, 0x2000, F } };
    KT_ASSERT(map_is(m, n, b, 6));
    kexec_memmap_merge(m, &n);
    static const uint64_t c[][3] = { { 0, 0x1000, R }, { 0x1000, 0x2000, U },
                                     { 0x3000, 0x2000, F }, { 0x5000, 0x3000, U },
                                     { 0x8000, 0x4000, F } };
    KT_ASSERT(map_is(m, n, c, 5));
    /* Outside every entry: nothing changes. */
    KT_EQ(kexec_memmap_overlay(m, &n, 8, 0x100000, 0x1000, F), OK);
    KT_ASSERT(map_is(m, n, c, 5));
    /* Over the cap: refused, unchanged. */
    KT_EQ(kexec_memmap_overlay(m, &n, 6, 0x1800, 0x5000, R), ERR_NO_RESOURCES);
    KT_ASSERT(map_is(m, n, c, 5));
    /* A range that wraps: refused. */
    KT_EQ(kexec_memmap_overlay(m, &n, 8, ~0ull - 10, 0x1000, F), ERR_INVALID_ARGS);
#undef U
#undef R
#undef F
}

/* A handoff kexec_handoff_check accepts. */
static void valid_handoff(struct kexec_handoff *h)
{
    memset(h, 0, sizeof(*h));
    h->magic = KEXEC_HANDOFF_MAGIC;
    h->version = KEXEC_HANDOFF_VERSION;
    h->size = sizeof(*h);
    h->hhdm_offset = 0xffff800000000000ull;
    h->kernel_virt_base = 0xffffffff80000000ull;
    h->cpu_count = 1;
    h->cpus[0] = (struct kexec_cpu){ 0, 0 };
    h->memmap_count = 1;
    h->memmap[0] = (struct kexec_mem){ 0x100000, 0x1000000, KEXEC_MEM_USABLE, 0 };
    h->module_count = 1;
    h->modules[0] = (struct kexec_module){ .phys = 0x200000, .size = 0x1000 };
    memcpy(h->loader_name, "test", 5);
    memcpy(h->cmdline, "shell", 6);
}

static void reseal(struct kexec_handoff *h)
{
    h->checksum = kexec_struct_sum(h, sizeof(*h), offsetof(struct kexec_handoff, checksum));
}

KTEST(kexec_handoff_check)
{
    struct kexec_handoff *h = kzalloc(sizeof(*h));
    KT_ASSERT(h);
    valid_handoff(h);
    reseal(h);
    KT_ASSERT(kexec_handoff_check(h, sizeof(*h)) == NULL);
    KT_ASSERT(kexec_handoff_check(h, sizeof(*h) - 8) != NULL);   /* too short */
    h->cmdline[0] = 'S';   /* changed after the checksum */
    KT_ASSERT(kexec_handoff_check(h, sizeof(*h)) != NULL);
    valid_handoff(h);   /* with a framebuffer: 800x600, 32 bpp, 3200 bytes a line */
    h->fb = (struct kexec_fb){ .phys = 0x80000000, .width = 800, .height = 600, .pitch = 3200,
                               .bpp = 32 };
    reseal(h);
    KT_ASSERT(kexec_handoff_check(h, sizeof(*h)) == NULL);

    /* Each one wrong, with a checksum that matches: the field checks. */
    for (int k = 0; k < 16; k++) {
        valid_handoff(h);
        switch (k) {
        case 0:  h->magic ^= 1; break;
        case 1:  h->version = KEXEC_HANDOFF_VERSION + 1; break;
        case 2:  h->size -= 8; break;
        case 3:  h->flags = 1u << 5; break;
        case 4:  h->x2apic = 2; break;
        case 5:  h->cpu_count = 0; break;
        case 6:  h->memmap_count = KEXEC_MAX_MEMMAP + 1; break;
        case 7:  h->memmap[0].type = 99; break;
        case 8:  h->memmap[0].base = ~0ull - 4; break;   /* wraps */
        case 9:  memset(h->cmdline, 'a', KEXEC_CMDLINE); break;
        case 10: memset(h->modules[0].path, 'a', KEXEC_STR); break;
        case 11: h->fb = (struct kexec_fb){ .phys = 0x80000000, .width = 0, .height = 600 };
                 break;
        case 12: h->hhdm_offset = 0x1000; break;
        case 13: h->module_count = KEXEC_MAX_MODULES + 1; break;
        case 14: /* the pitch in pixels, not bytes: lines would overlap */
                 h->fb = (struct kexec_fb){ .phys = 0x80000000, .width = 800, .height = 600,
                                            .pitch = 800, .bpp = 32 };
                 break;
        case 15: h->fb = (struct kexec_fb){ .phys = 0x80000000, .width = 800, .height = 600,
                                            .pitch = 3200, .bpp = 0 };
                 break;
        }
        reseal(h);
        if (kexec_handoff_check(h, sizeof(*h)) == NULL)
            kprintf("kexec_handoff_check: corruption %d was accepted\n", k);
        KT_ASSERT(kexec_handoff_check(h, sizeof(*h)) != NULL);
    }
    kfree(h);
}

/* Does any leaf of the kernel half of the kernel's tables map a page of
 * [base, base + size)? Walks every present entry (no recursion: four
 * nested loops, one per level). */
static bool kernel_maps(uint64_t base, uint64_t size)
{
    const uint64_t *l4 = phys_to_virt(vmm_kernel_pml4());
    for (unsigned a = 256; a < 512; a++) {
        if (!(l4[a] & PTE_P))
            continue;
        const uint64_t *l3 = phys_to_virt(l4[a] & PTE_ADDR);
        for (unsigned b = 0; b < 512; b++) {
            if (!(l3[b] & PTE_P))
                continue;
            uint64_t pa = l3[b] & PTE_ADDR & ~((1ull << 30) - 1);
            if (l3[b] & PTE_PS) {
                if (pa < base + size && base < pa + (1ull << 30))
                    return true;
                continue;
            }
            const uint64_t *l2 = phys_to_virt(l3[b] & PTE_ADDR);
            for (unsigned c = 0; c < 512; c++) {
                if (!(l2[c] & PTE_P))
                    continue;
                pa = l2[c] & PTE_ADDR & ~((1ull << 21) - 1);
                if (l2[c] & PTE_PS) {
                    if (pa < base + size && base < pa + (1ull << 21))
                        return true;
                    continue;
                }
                const uint64_t *l1 = phys_to_virt(l2[c] & PTE_ADDR);
                for (unsigned d = 0; d < 512; d++)
                    if ((l1[d] & PTE_P) && (l1[d] & PTE_ADDR) >= base &&
                        (l1[d] & PTE_ADDR) < base + size)
                        return true;
            }
        }
    }
    return false;
}

KTEST(kexec_region_unmapped)
{
    uint64_t base, size;
    if (!kexec_region(&base, &size)) {
        kprintf("kexec_region_unmapped: no region (crashkernel=0): nothing to check\n");
        return;
    }
    KT_ASSERT(!(base & ((2ull << 20) - 1)) && size >= (32ull << 20));
    KT_ASSERT(base + size <= (4ull << 30));
    /* RAM to the rules that keep MMIO off it, not RAM the PMM manages. */
    KT_ASSERT(pmm_range_has_ram(base, size));
    /* Not in the HHDM: every 2 MiB, and the first and last page. */
    for (uint64_t off = 0; off < size; off += 2ull << 20)
        KT_EQ(vmm_translate(vmm_kernel_pml4(), hhdm_offset + base + off), UINT64_MAX);
    KT_EQ(vmm_translate(vmm_kernel_pml4(), hhdm_offset + base + size - PAGE_SIZE), UINT64_MAX);
    /* Nor anywhere else in the kernel's half (which every address space
     * shares). The walk does find what is mapped: this function's page. */
    KT_ASSERT(!kernel_maps(base, size));
    uint64_t self = vmm_translate(vmm_kernel_pml4(), (uint64_t)(uintptr_t)&kernel_maps);
    KT_ASSERT(self != UINT64_MAX && kernel_maps(ALIGN_DOWN(self, PAGE_SIZE), PAGE_SIZE));
}

KTEST(kexec_stored_kernel_intact)
{
    if (!kexec_armed()) {
        kprintf("kexec_stored_kernel_intact: no stored kernel: nothing to check\n");
        return;
    }
    KT_ASSERT(kexec_verify());
}

KTEST(kexec_load_refuses_garbage)
{
    bool armed = kexec_armed();
    struct vmo *k, *b;
    KT_EQ(vmo_create(8192, 0, &k), OK);
    KT_EQ(vmo_create(8192, 0, &b), OK);
    static const char junk[] = "\x7f" "ELF but not really";
    KT_EQ(vmo_write(k, 0, junk, sizeof(junk)), OK);
    status_t st = kexec_load_image(k, b, "");
    uint64_t base, size;
    KT_EQ(st, kexec_region(&base, &size) ? ERR_INVALID_ARGS : ERR_NOT_SUPPORTED);
    /* A refused image changes nothing. */
    KT_EQ(kexec_armed(), armed);
    if (armed)
        KT_ASSERT(kexec_verify());
    kobject_unref(vmo_kobject(b));
    kobject_unref(vmo_kobject(k));
}

/* The next kernel keeps the machine's words, drops the one-time ones, and
 * turns crashtest=<name> into test<name>. */
KTEST(kexec_next_cmdline_words)
{
    char buf[KEXEC_CMDLINE];
    kexec_next_cmdline("ktest=sched loops=3 nopcid  soak=3 verbose crashkernel=64 init testpf "
                       "stress=60 idlespin=5 smp=loader bench", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "nopcid verbose crashkernel=64 idlespin=5 smp=loader"));
    kexec_next_cmdline("shell nosplash crashtest=lockorder", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "shell nosplash testlockorder"));
    kexec_next_cmdline("hidboot keytest", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "hidboot"));
    kexec_next_cmdline("nospare init", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "nospare"));
    kexec_next_cmdline("bench nolockdep", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "nolockdep"));
    kexec_next_cmdline("bench nohandoff", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "nohandoff"));
    /* No compositor stays no compositor; the compositor's test scene is a test. */
    kexec_next_cmdline("nocomp comptest comp", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "nocomp"));
    /* The disk the machine booted from goes on from kernel to kernel. */
    kexec_next_cmdline("shell bootdisk=3792991605 init", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "shell bootdisk=3792991605"));
    kexec_next_cmdline("", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, ""));
    /* Cut to fit, still terminated. */
    kexec_next_cmdline("verbose nosplash", buf, 10);
    KT_ASSERT(strlen(buf) < 10);
    /* A word that only starts like a kept one is not kept. */
    kexec_next_cmdline("verbosely crashkernel= nousbx", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, ""));
}

/* Every vlan word is kept (a VLAN, untagged, off, or one that means
 * off), and before the others, so a reboot never comes back in the
 * build's default mode when this boot chose another. netprobe is not
 * kept. */
KTEST(kexec_next_cmdline_vlan)
{
    char buf[KEXEC_CMDLINE];
    kexec_next_cmdline("shell vlan=21 netprobe", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "vlan=21 shell"));
    kexec_next_cmdline("vlan=off nosplash", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "vlan=off nosplash"));
    kexec_next_cmdline("shell vlan=none", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "vlan=none shell"));
    kexec_next_cmdline("vlan=untagged verbose", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "vlan=untagged verbose"));
    kexec_next_cmdline("verbose vlan vlan= vlan=junk vlanx=3", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "vlan vlan= vlan=junk verbose"));
    kexec_next_cmdline("netprobe", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, ""));
    /* `net` (the PC's network) is kept, so a reboot, a panic and `update`
     * come back with the network; the one-shot tests are not. */
    kexec_next_cmdline("shell net vlan=21", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "vlan=21 shell net"));
    kexec_next_cmdline("netsend vlan=21", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, "vlan=21"));
    kexec_next_cmdline("network nets net=1", buf, sizeof(buf));
    KT_ASSERT(!strcmp(buf, ""));
    /* Short of room: the vlan word goes in, the others as far as they fit. */
    kexec_next_cmdline("verbose nosplash vlan=off", buf, 9);
    KT_ASSERT(!strcmp(buf, "vlan=off"));
}

/* A panic within 30 s of a start that was itself a panic's halts. */
KTEST(kexec_crash_loop_rule)
{
    KT_ASSERT(kexec_crash_loop(true, 1, 0));
    KT_ASSERT(kexec_crash_loop(true, 1, KEXEC_LOOP_NS - 1));
    KT_ASSERT(!kexec_crash_loop(true, 1, KEXEC_LOOP_NS));
    KT_ASSERT(!kexec_crash_loop(false, 0, 0));
    KT_ASSERT(!kexec_crash_loop(false, 0, KEXEC_LOOP_NS * 10));
    /* The third panic in a row halts however late it comes; a boot after
     * a power-on or a reboot (no panic before it) never counts. */
    KT_ASSERT(!kexec_crash_loop(true, KEXEC_LOOP_PANICS - 2, KEXEC_LOOP_NS * 10));
    KT_ASSERT(kexec_crash_loop(true, KEXEC_LOOP_PANICS - 1, KEXEC_LOOP_NS * 10));
    KT_ASSERT(kexec_crash_loop(true, KEXEC_LOOP_PANICS + 5, KEXEC_LOOP_NS * 10));
    KT_ASSERT(!kexec_crash_loop(false, KEXEC_LOOP_PANICS + 5, KEXEC_LOOP_NS * 10));
}

/* A crash record as a dying kernel seals it: a panic, a 64 KiB ring at
 * ring_phys, a name and a message. */
static void record_fill(struct kexec_crash_record *r, uint64_t ring_phys, uint64_t ring_size,
                        const char *message)
{
    memset(r, 0, sizeof(*r));
    r->magic = KEXEC_RECORD_MAGIC;
    r->version = KEXEC_RECORD_VERSION;
    r->size = sizeof(*r);
    r->kind = KEXEC_RECORD_PANIC;
    r->panics = 1;
    r->ring_phys = ring_phys;
    r->ring_size = ring_size;
    r->head = 5000;
    r->panic_at = 4000;
    memcpy(r->name, "boot-0007", 10);
    size_t n = strlen(message);
    memcpy(r->message, message, n < KEXEC_MESSAGE ? n : KEXEC_MESSAGE - 1);
    r->checksum = kexec_struct_sum(r, sizeof(*r), offsetof(struct kexec_crash_record, checksum));
}

/* The next kernel counts every panic its record describes for the
 * crash-loop rule, whatever the panic's message or log ring look like;
 * those decide only whether the log is saved. */
KTEST(kexec_crash_record_counts_any_panic)
{
    struct boot_info *bi = kzalloc(sizeof(*bi));
    uint64_t pa = pmm_alloc_page_phys(PMM_ZERO);
    KT_ASSERT(bi && pa);
    struct kexec_crash_record *r = phys_to_virt(pa);
    /* The record's page, and a pretend ring range the check never reads. */
    uint64_t ring = 1ull << 40;
    bi->memmap[0] = (struct boot_mem_region){ pa, PAGE_SIZE, BOOT_MEM_CRASH_LOG };
    bi->memmap[1] = (struct boot_mem_region){ ring, 64ull << 20, BOOT_MEM_CRASH_LOG };
    bi->memmap_count = 2;
    bi->kexec_record = pa;
    bool panicked, log_ok;

    record_fill(r, ring, 64 << 10, "test panic");
    KT_ASSERT(crashlog_check(bi, &panicked, &log_ok) == NULL && panicked && log_ok);
    /* A message with a tab and UTF-8 (a thread's name, say). */
    record_fill(r, ring, 64 << 10, "user fault in \"\tJA\xc3\x9f-Z\"");
    KT_ASSERT(crashlog_check(bi, &panicked, &log_ok) == NULL && panicked && log_ok);
    /* A ring bigger than this kernel's (another build's). */
    record_fill(r, ring, 2ull << 20, "test panic");
    KT_ASSERT(crashlog_check(bi, &panicked, &log_ok) == NULL && panicked && log_ok);
    /* A ring past what the check allows, or outside CRASH_LOG: the log is
     * lost, the panic still counts. */
    record_fill(r, ring, 32ull << 20, "test panic");
    KT_ASSERT(crashlog_check(bi, &panicked, &log_ok) == NULL && panicked && !log_ok);
    record_fill(r, ring + PAGE_SIZE, 64ull << 20, "test panic");
    KT_ASSERT(crashlog_check(bi, &panicked, &log_ok) == NULL && panicked && !log_ok);
    /* A name that is not one: dropped, the log saved under a number. */
    record_fill(r, ring, 64 << 10, "test panic");
    memcpy(r->name, "../x", 5);
    r->checksum = kexec_struct_sum(r, sizeof(*r), offsetof(struct kexec_crash_record, checksum));
    KT_ASSERT(crashlog_check(bi, &panicked, &log_ok) == NULL && panicked && log_ok);
    /* A reboot: no panic. */
    record_fill(r, ring, 64 << 10, "");
    r->kind = KEXEC_RECORD_REBOOT;
    r->checksum = kexec_struct_sum(r, sizeof(*r), offsetof(struct kexec_crash_record, checksum));
    KT_ASSERT(crashlog_check(bi, &panicked, &log_ok) == NULL && !panicked);
    /* Not the previous kernel's record: no use at all. */
    record_fill(r, ring, 64 << 10, "test panic");
    r->panics++;
    KT_ASSERT(crashlog_check(bi, &panicked, &log_ok) != NULL && !panicked);
    bi->memmap[0].type = BOOT_MEM_USABLE;
    record_fill(r, ring, 64 << 10, "test panic");
    KT_ASSERT(crashlog_check(bi, &panicked, &log_ok) != NULL && !panicked);

    pmm_free_page_phys(pa);
    kfree(bi);
}

/* The dying kernel writes its message as plain text. */
KTEST(kexec_message_clean)
{
    char buf[16];
    kexec_message_clean(buf, sizeof(buf), "a\tb\xc3\x9f\nc");
    KT_ASSERT(!strcmp(buf, "a?b?? c"));
    kexec_message_clean(buf, 6, "longer than that");
    KT_ASSERT(!strcmp(buf, "longe"));
    kexec_message_clean(buf, sizeof(buf), "");
    KT_ASSERT(!strcmp(buf, ""));
}

/* The stored kernel leaves a quarter of the default region free, so a
 * growing kernel or bootfs is noticed here before it no longer fits on
 * the PC (where a panic would then halt and `reboot` use the firmware). */
KTEST(kexec_region_has_room)
{
    uint64_t base, size, used = kexec_stored_bytes();
    if (!kexec_region(&base, &size) || !used) {
        kprintf("kexec_region_has_room: no stored kernel: nothing to check\n");
        return;
    }
    kprintf("kexec_region_has_room: %lu KiB of %lu MiB\n", used >> 10, size >> 20);
    KT_ASSERT(used <= size / 4 * 3);
}
