/* ELF parser: the real programs in bootfs parse into the plan their
 * headers describe, and corrupted images (directed and random) are refused
 * without a single read outside the image. */
#include <jam/aspace.h>
#include <jam/bootfs.h>
#include <jam/elf.h>
#include <jam/elf64.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/string.h>
#include <jam/uentry.h>
#include <jam/vmo.h>

static void read_ehdr(const void *img, struct elf64_ehdr *eh)
{
    memcpy(eh, img, sizeof(*eh));
}

static void read_phdr(const void *img, const struct elf64_ehdr *eh, unsigned i,
                      struct elf64_phdr *ph)
{
    memcpy(ph, (const uint8_t *)img + eh->e_phoff + i * sizeof(*ph), sizeof(*ph));
}

/* Everything a loader relies on, checked independently of elf.c. */
static void check_plan(const struct elf_plan *p, uint64_t size)
{
    KT_ASSERT(p->nseg >= 1 && p->nseg <= ELF_MAX_SEGMENTS);
    bool entry_ok = false;
    for (unsigned i = 0; i < p->nseg; i++) {
        const struct elf_segment *s = &p->seg[i];
        KT_ASSERT(user_range_ok(s->vaddr, s->memsz) && s->memsz > 0);
        KT_ASSERT(s->filesz <= s->memsz);
        KT_ASSERT(s->file_off <= size && s->filesz <= size - s->file_off);
        KT_ASSERT(s->flags & ASPACE_READ);
        KT_ASSERT(!((s->flags & ASPACE_WRITE) && (s->flags & ASPACE_EXEC)));
        KT_ASSERT((s->vaddr - s->file_off) % PAGE_SIZE == 0);
        if (i)
            KT_ASSERT(s->vaddr / PAGE_SIZE >
                      (p->seg[i - 1].vaddr + p->seg[i - 1].memsz - 1) / PAGE_SIZE);
        if ((s->flags & ASPACE_EXEC) && p->entry >= s->vaddr && p->entry - s->vaddr < s->memsz)
            entry_ok = true;
    }
    KT_ASSERT(entry_ok);
}

static void check_program(const char *name)
{
    const void *img;
    uint64_t size;
    struct elf_plan plan;
    KT_EQ(bootfs_data(name, &img, &size), OK);
    KT_EQ(elf_parse(img, size, &plan), OK);
    check_plan(&plan, size);

    /* The plan is the ELF's PT_LOAD headers, in order. */
    struct elf64_ehdr eh;
    read_ehdr(img, &eh);
    KT_EQ(plan.entry, eh.e_entry);
    unsigned n = 0;
    for (unsigned i = 0; i < eh.e_phnum; i++) {
        struct elf64_phdr ph;
        read_phdr(img, &eh, i, &ph);
        if (ph.p_type != PT_LOAD)
            continue;
        const struct elf_segment *s = &plan.seg[n++];
        KT_EQ(s->vaddr, ph.p_vaddr);
        KT_EQ(s->memsz, ph.p_memsz);
        KT_EQ(s->file_off, ph.p_offset);
        KT_EQ(s->filesz, ph.p_filesz);
    }
    KT_EQ(n, plan.nseg);

    /* user/linker.ld: text R+X at 0x400000 holding the entry, rodata R,
     * data (+bss) R+W. */
    KT_EQ(plan.nseg, 3);
    KT_EQ(plan.seg[0].vaddr, 0x400000);
    KT_EQ(plan.seg[0].flags, ASPACE_READ | ASPACE_EXEC);
    KT_EQ(plan.seg[1].flags, ASPACE_READ);
    KT_EQ(plan.seg[2].flags, ASPACE_READ | ASPACE_WRITE);
    KT_ASSERT(plan.entry >= plan.seg[0].vaddr &&
              plan.entry < plan.seg[0].vaddr + plan.seg[0].memsz);
}

KTEST(elf_programs)
{
    check_program("bin/init");
    check_program("bin/utest");
    const void *cfg;
    uint64_t size;
    struct elf_plan plan;
    const char *why;
    KT_EQ(bootfs_data("init.cfg", &cfg, &size), OK);
    KT_EQ(elf_check(cfg, size, &plan, &why), ERR_INVALID_ARGS);
}

/* ---- corruption -------------------------------------------------------- */

static uint64_t rng_state = 0x243f6a8885a308d3ull;

static uint64_t rng(void) { return kt_rng(&rng_state); }

/* The fuzz buffer: a VMO mapped into the vmap area, which leaves an
 * unmapped gap after every mapping. Each case puts its image flush
 * against the end, so a read one byte past the image faults (and panics
 * the kernel) instead of silently reading neighbouring memory. */
struct fuzzbuf {
    struct vmo *v;     /* the VMO backing the buffer */
    uint8_t    *base;  /* its kernel mapping */
    uint64_t    cap;   /* mapped bytes */
};

static void fuzzbuf_init(struct fuzzbuf *fb, uint64_t size)
{
    fb->cap = ALIGN_UP(size, PAGE_SIZE);
    KT_EQ(vmo_create(fb->cap, 0, &fb->v), OK);
    KT_EQ(vmo_map_kernel(fb->v, 0, fb->cap, VM_WRITE, (void **)&fb->base), OK);
}

static void fuzzbuf_free(struct fuzzbuf *fb)
{
    KT_EQ(vmo_unmap_kernel(fb->v, fb->base), OK);
    kobject_unref(vmo_kobject(fb->v));
}

/* Copy src[0..len) to the end of the buffer; returns where it went. */
static uint8_t *place(const struct fuzzbuf *fb, const void *src, uint64_t len)
{
    uint8_t *p = fb->base + fb->cap - len;
    memcpy(p, src, len);
    return p;
}

static struct elf64_ehdr *EH(uint8_t *img)
{
    return (struct elf64_ehdr *)img;
}

static struct elf64_phdr *PH(uint8_t *img, unsigned i)
{
    return (struct elf64_phdr *)(img + EH(img)->e_phoff) + i;
}

/* A synthetic static ELF with nseg one-page segments: text R+X first
 * (holding the entry), then read-only ones. */
static uint64_t build_elf(uint8_t *img, unsigned nseg)
{
    uint64_t size = PAGE_SIZE * (nseg + 1);
    memset(img, 0, size);
    struct elf64_ehdr *eh = EH(img);
    memcpy(eh->e_ident, "\x7f" "ELF", 4);
    eh->e_ident[EI_CLASS] = ELFCLASS64;
    eh->e_ident[EI_DATA] = ELFDATA2LSB;
    eh->e_ident[EI_VERSION] = EV_CURRENT;
    eh->e_type = ET_EXEC;
    eh->e_machine = EM_X86_64;
    eh->e_version = EV_CURRENT;
    eh->e_entry = 0x400010;
    eh->e_phoff = sizeof(*eh);
    eh->e_ehsize = sizeof(*eh);
    eh->e_phentsize = sizeof(struct elf64_phdr);
    eh->e_phnum = nseg;
    for (unsigned i = 0; i < nseg; i++) {
        struct elf64_phdr *ph = PH(img, i);
        ph->p_type = PT_LOAD;
        ph->p_flags = i ? PF_R : PF_R | PF_X;
        ph->p_offset = PAGE_SIZE * (i + 1);
        ph->p_vaddr = 0x400000 + PAGE_SIZE * i;
        ph->p_filesz = 64;
        ph->p_memsz = 64;
        ph->p_align = PAGE_SIZE;
    }
    return size;
}

KTEST(elf_synthetic)
{
    struct fuzzbuf fb;
    struct elf_plan plan;
    const char *why;
    fuzzbuf_init(&fb, PAGE_SIZE * (ELF_MAX_SEGMENTS + 2));
    uint8_t *tmp = kmalloc(fb.cap);
    KT_ASSERT(tmp);

    uint64_t size = build_elf(tmp, ELF_MAX_SEGMENTS);
    KT_EQ(elf_check(place(&fb, tmp, size), size, &plan, &why), OK);
    KT_EQ(plan.nseg, ELF_MAX_SEGMENTS);
    check_plan(&plan, size);

    size = build_elf(tmp, ELF_MAX_SEGMENTS + 1);
    KT_EQ(elf_check(place(&fb, tmp, size), size, &plan, &why), ERR_INVALID_ARGS);

    /* Program headers that aren't loaded are skipped; dynamic ones refused. */
    size = build_elf(tmp, 2);
    PH(tmp, 1)->p_type = PT_NOTE;
    KT_EQ(elf_check(place(&fb, tmp, size), size, &plan, &why), OK);
    KT_EQ(plan.nseg, 1);
    PH(tmp, 1)->p_type = PT_INTERP;
    KT_EQ(elf_check(place(&fb, tmp, size), size, &plan, &why), ERR_INVALID_ARGS);
    PH(tmp, 1)->p_type = PT_TLS;
    KT_EQ(elf_check(place(&fb, tmp, size), size, &plan, &why), ERR_INVALID_ARGS);

    /* A segment reaching exactly to USER_TOP is fine; one byte more is not. */
    size = build_elf(tmp, 2);
    PH(tmp, 1)->p_vaddr = USER_TOP - PAGE_SIZE;
    PH(tmp, 1)->p_memsz = PAGE_SIZE;
    KT_EQ(elf_check(place(&fb, tmp, size), size, &plan, &why), OK);
    PH(tmp, 1)->p_memsz = PAGE_SIZE + 1;
    KT_EQ(elf_check(place(&fb, tmp, size), size, &plan, &why), ERR_INVALID_ARGS);

    kfree(tmp);
    fuzzbuf_free(&fb);
}

/* One random change in the first hdr_bytes of work: a random byte, a
 * flipped bit, or a 64-bit value at (or near) an edge. */
static void damage(uint8_t *work, uint64_t size, uint64_t hdr_bytes)
{
    static const uint64_t vals[] = {
        0, 1, 8, 0x1000, 0x400000, 0x7fffffffe000ull, 0x7ffffffff000ull,
        0x800000000000ull, 1ull << 63, ~0ull, 0xfffffffffffff000ull, 0xffffffff80000000ull,
    };
    uint64_t off = rng() % hdr_bytes;
    switch (rng() % 3) {
    case 0:
        work[off] = (uint8_t)rng();
        break;
    case 1:
        work[off] ^= (uint8_t)(1u << (rng() % 8));
        break;
    default: {
        uint64_t val = vals[rng() % (sizeof(vals) / sizeof(vals[0]))];
        if (rng() % 2)
            val = val ? val - 1 + rng() % 3 : rng();   /* near the edge */
        off &= ~7ull;
        if (off + 8 <= size)
            memcpy(work + off, &val, 8);
    }
    }
}

KTEST(elf_corrupt)
{
    const void *orig;
    uint64_t size;
    KT_EQ(bootfs_data("bin/init", &orig, &size), OK);
    struct elf64_ehdr eh;
    read_ehdr(orig, &eh);
    KT_ASSERT(eh.e_phnum >= 3);
    struct elf64_phdr ph0, ph1, ph2;
    read_phdr(orig, &eh, 0, &ph0);
    read_phdr(orig, &eh, 1, &ph1);
    read_phdr(orig, &eh, 2, &ph2);

    struct fuzzbuf fb;
    fuzzbuf_init(&fb, size);
    uint8_t *work = kmalloc(size);
    KT_ASSERT(work);
    struct elf_plan plan;
    const char *why;

    /* Truncation: fine as long as the headers and every segment's bytes are
     * still there (the section headers at the end aren't needed). */
    uint64_t need = eh.e_phoff + eh.e_phnum * sizeof(struct elf64_phdr);
    for (unsigned i = 0; i < eh.e_phnum; i++) {
        struct elf64_phdr ph;
        read_phdr(orig, &eh, i, &ph);
        if (ph.p_type == PT_LOAD && ph.p_offset + ph.p_filesz > need)
            need = ph.p_offset + ph.p_filesz;
    }
    const uint64_t cuts[] = { 0, 1, 4, 16, 63, 64, 64 + 55, need - 1, need, size - 1,
                              ph0.p_offset + ph0.p_filesz - 1, ph2.p_offset };
    for (unsigned i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
        uint64_t len = cuts[i];
        status_t want = len >= need ? OK : ERR_INVALID_ARGS;
        KT_EQ(elf_check(place(&fb, orig, len), len, &plan, &why), want);
    }

    /* Directed damage: each case must be refused. */
    enum { N_CASES = 44 };
    for (int c = 0; c < N_CASES; c++) {
        memcpy(work, orig, size);
        struct elf64_ehdr *h = EH(work);
        struct elf64_phdr *t = PH(work, 0), *r = PH(work, 1), *d = PH(work, 2);
        switch (c) {
        case 0:  h->e_ident[0] = 0; break;
        case 1:  h->e_ident[EI_CLASS] = 1; break;
        case 2:  h->e_ident[EI_DATA] = 2; break;
        case 3:  h->e_ident[EI_VERSION] = 0; break;
        case 4:  h->e_ident[EI_OSABI] = 9; break;
        case 5:  h->e_type = ET_DYN; break;
        case 6:  h->e_type = ET_REL; break;
        case 7:  h->e_machine = 3; break;
        case 8:  h->e_version = 0; break;
        case 9:  h->e_ehsize = 0; break;
        case 10: h->e_phentsize = 55; break;
        case 11: h->e_phentsize = 0xffff; break;
        case 12: h->e_phnum = 0; break;
        case 13: h->e_phnum = 0xffff; break;
        case 14: h->e_phoff = size; break;
        case 15: h->e_phoff = size - 8; break;
        case 16: h->e_phoff = 0xfffffffffffffff8ull; break;   /* phoff + table wraps */
        case 17: h->e_phoff += 4; break;                      /* misaligned */
        case 18: h->e_entry = 0; break;
        case 19: h->e_entry = r->p_vaddr; break;              /* in rodata */
        case 20: h->e_entry = d->p_vaddr + 8; break;          /* in data */
        case 21: h->e_entry = t->p_vaddr + t->p_memsz; break; /* one past text */
        case 22: h->e_entry = 0xffffffff80000000ull; break;
        case 23: t->p_flags = PF_R | PF_W | PF_X; break;
        case 24: d->p_flags = PF_R | PF_W | PF_X; break;
        case 25: d->p_flags = 0; break;
        case 26: r->p_type = PT_DYNAMIC; break;
        case 27: t->p_offset = size; break;
        case 28: t->p_offset = 0xfffffffffffff000ull; break;  /* offset + filesz wraps */
        case 29: d->p_filesz = size; break;
        case 30: t->p_filesz = t->p_memsz + 1; break;
        case 31: d->p_memsz = ~0ull; break;                   /* vaddr + memsz wraps */
        case 32: t->p_vaddr = 0; t->p_offset = 0; break;      /* page 0 */
        case 33: d->p_vaddr = USER_TOP; break;
        case 34: d->p_vaddr = 0xffff800000000000ull; break;   /* kernel half */
        case 35: d->p_vaddr = 0x0000800000000000ull; break;   /* non-canonical */
        case 36: r->p_vaddr = t->p_vaddr; break;              /* overlap */
        case 37: r->p_vaddr = t->p_vaddr + PAGE_SIZE * ((t->p_memsz - 1) / PAGE_SIZE); break;
        case 38: {                                             /* out of order */
            struct elf64_phdr tmp = *r;
            *r = *d;
            *d = tmp;
            break;
        }
        case 39: t->p_vaddr += 8; break;                      /* not congruent to offset */
        case 40: t->p_align = 3; break;
        case 41: t->p_memsz = 0; t->p_filesz = 0; break;
        case 42: t->p_flags = PF_X; break;                    /* exec-only: not readable */
        case 43: for (unsigned i = 0; i < h->e_phnum; i++)    /* nothing to load */
                     PH(work, i)->p_type = PT_NOTE;
                 break;
        }
        why = NULL;
        if (elf_check(place(&fb, work, size), size, &plan, &why) != ERR_INVALID_ARGS)
            panic("ktest elf_corrupt: case %d accepted", c);
        KT_ASSERT(why != NULL);
    }

    /* Random damage to the headers, sometimes with a truncation: never a
     * fault, and anything still accepted must pass check_plan. */
    unsigned accepted = 0;
    uint64_t hdr_bytes = eh.e_phoff + eh.e_phnum * sizeof(struct elf64_phdr);
    for (int it = 0; it < 600; it++) {
        memcpy(work, orig, size);
        unsigned n = 1 + rng() % 4;
        for (unsigned k = 0; k < n; k++)
            damage(work, size, hdr_bytes);
        uint64_t len = size;
        if (rng() % 6 == 0)
            len = rng() % (size + 1);
        if (elf_check(place(&fb, work, len), len, &plan, &why) == OK) {
            check_plan(&plan, len);
            accepted++;
        }
    }
    kprintf("elf_corrupt: %d directed cases rejected, 600 random: %u still valid\n", N_CASES,
            accepted);

    kfree(work);
    fuzzbuf_free(&fb);
}
