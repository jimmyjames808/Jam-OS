/* Static ELF64 executables: check one and turn it into a load plan
 * (jam/elf.h). The image comes from bootfs or, later, from a user process,
 * so every field is treated as hostile: each read is bounds-checked against
 * the image size first, every sum is checked for overflow, and headers are
 * copied out with memcpy (no alignment assumptions about the image).
 *
 * The rules are the ones the loader relies on: PT_LOAD segments in the
 * user range, in address order and on pages of their own (so each page
 * gets exactly one segment's permissions), never writable and executable,
 * file bytes inside the image, the entry point inside an executable
 * segment. Dynamic linking and TLS are refused: only static programs. */
#include <stdbool.h>
#include <jam/aspace.h>
#include <jam/elf.h>
#include <jam/elf64.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/string.h>
#include <jam/uentry.h>

#define ELF_MAX_PHDRS 64   /* program headers we are willing to walk */

#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))

/* The reason the ELF header is refused, or NULL; then *eh holds it. */
static const char *check_header(const uint8_t *img, uint64_t size, struct elf64_ehdr *eh)
{
    if (size < sizeof(*eh))
        return "shorter than the ELF header";
    memcpy(eh, img, sizeof(*eh));
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4))
        return "not an ELF file";
    if (eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_ident[EI_DATA] != ELFDATA2LSB ||
        eh->e_ident[EI_VERSION] != EV_CURRENT)
        return "not a little-endian ELF64 file";
    if (eh->e_ident[EI_OSABI] != ELFOSABI_SYSV && eh->e_ident[EI_OSABI] != ELFOSABI_GNU)
        return "unknown OS ABI";
    if (eh->e_type != ET_EXEC)
        return "not a static executable (ET_EXEC)";
    if (eh->e_machine != EM_X86_64 || eh->e_version != EV_CURRENT)
        return "not an x86-64 executable";
    if (eh->e_ehsize != sizeof(struct elf64_ehdr))
        return "bad ELF header size";
    if (eh->e_phentsize != sizeof(struct elf64_phdr))
        return "bad program header size";
    if (eh->e_phnum == 0 || eh->e_phnum > ELF_MAX_PHDRS)
        return "no or too many program headers";
    if (eh->e_phoff % 8)
        return "misaligned program header table";
    /* No overflow: phnum * 56 is at most 3584. */
    if (eh->e_phoff > size ||
        (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr) > size - eh->e_phoff)
        return "program headers outside the file";
    return NULL;
}

/* The reason a program header of this type is refused, or NULL; *load
 * says whether it is a segment to load. */
static const char *check_type(uint32_t type, bool *load)
{
    *load = false;
    switch (type) {
    case PT_LOAD:
        *load = true;
        return NULL;
    case PT_DYNAMIC:
    case PT_INTERP:
    case PT_SHLIB:
        return "dynamically linked";
    case PT_TLS:
        return "uses thread-local storage (not supported)";
    default:
        return NULL;   /* PT_NOTE, PT_PHDR, PT_GNU_STACK...: nothing to load */
    }
}

/* The reason a PT_LOAD segment is refused, or NULL; plan holds the
 * segments accepted so far. */
static const char *check_segment(const struct elf64_phdr *ph, uint64_t size,
                                 const struct elf_range *range, const struct elf_plan *plan)
{
    if (plan->nseg == ELF_MAX_SEGMENTS)
        return "too many PT_LOAD segments";
    if (!(ph->p_flags & PF_R))
        return "segment is not readable";
    if ((ph->p_flags & PF_W) && (ph->p_flags & PF_X))
        return "segment is writable and executable";
    if (ph->p_memsz == 0)
        return "empty segment";
    if (ph->p_filesz > ph->p_memsz)
        return "segment file size exceeds its memory size";
    if (ph->p_vaddr < range->lo || ph->p_vaddr > range->hi ||
        ph->p_memsz > range->hi - ph->p_vaddr)
        return "segment outside the allowed address range";
    if (ph->p_offset > size || ph->p_filesz > size - ph->p_offset)
        return "segment data outside the file";
    if (ph->p_align > 1 && (ph->p_align & (ph->p_align - 1)))
        return "segment alignment is not a power of two";
    if ((ph->p_vaddr - ph->p_offset) % PAGE_SIZE)
        return "segment address and file offset differ mod 4 KiB";
    if (plan->nseg) {
        const struct elf_segment *prev = &plan->seg[plan->nseg - 1];
        /* Page-granular: two segments may not share a page, since a
         * page has one set of permissions. The range check above keeps
         * these sums below range->hi. */
        if (ALIGN_DOWN(ph->p_vaddr, PAGE_SIZE) < ALIGN_UP(prev->vaddr + prev->memsz, PAGE_SIZE))
            return "segments overlap, share a page or are out of order";
    }
    return NULL;
}

/* Append a checked segment to the plan; true if it is executable and
 * holds the entry point. */
static bool add_segment(struct elf_plan *plan, const struct elf64_phdr *ph, uint64_t entry)
{
    struct elf_segment *s = &plan->seg[plan->nseg++];
    s->vaddr = ph->p_vaddr;
    s->memsz = ph->p_memsz;
    s->file_off = ph->p_offset;
    s->filesz = ph->p_filesz;
    s->flags = ASPACE_READ | (ph->p_flags & PF_W ? ASPACE_WRITE : 0) |
               (ph->p_flags & PF_X ? ASPACE_EXEC : 0);
    return (s->flags & ASPACE_EXEC) && entry >= s->vaddr && entry - s->vaddr < s->memsz;
}

static status_t reject(const char **why, const char *msg)
{
    *why = msg;
    return ERR_INVALID_ARGS;
}

status_t elf_check_range(const void *image, uint64_t size, const struct elf_range *range,
                         struct elf_plan *out, const char **why)
{
    const uint8_t *img = image;
    struct elf64_ehdr eh;
    const char *bad = check_header(img, size, &eh);
    if (bad)
        return reject(why, bad);

    struct elf_plan plan = { .entry = eh.e_entry, .nseg = 0 };
    bool entry_ok = false;
    for (unsigned i = 0; i < eh.e_phnum; i++) {
        struct elf64_phdr ph;
        memcpy(&ph, img + eh.e_phoff + (uint64_t)i * sizeof(ph), sizeof(ph));
        bool load;
        bad = check_type(ph.p_type, &load);
        if (bad)
            return reject(why, bad);
        if (!load)
            continue;
        bad = check_segment(&ph, size, range, &plan);
        if (bad)
            return reject(why, bad);
        if (add_segment(&plan, &ph, eh.e_entry))
            entry_ok = true;
    }
    if (plan.nseg == 0)
        return reject(why, "no PT_LOAD segments");
    if (!entry_ok)
        return reject(why, "entry point is not inside an executable segment");

    *out = plan;
    *why = NULL;
    return OK;
}

status_t elf_check(const void *image, uint64_t size, struct elf_plan *out, const char **why)
{
    static const struct elf_range user = { USER_BASE, USER_TOP };
    return elf_check_range(image, size, &user, out, why);
}

status_t elf_parse(const void *image, uint64_t size, struct elf_plan *out)
{
    const char *why;
    status_t st = elf_check(image, size, out, &why);
    if (st != OK)
        kprintf("elf: rejected: %s\n", why);
    return st;
}
