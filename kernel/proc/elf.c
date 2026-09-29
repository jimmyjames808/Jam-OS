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

status_t elf_check(const void *image, uint64_t size, struct elf_plan *out, const char **why)
{
#define REJECT(msg)              \
    do {                         \
        *why = (msg);            \
        return ERR_INVALID_ARGS; \
    } while (0)

    const uint8_t *img = image;
    struct elf64_ehdr eh;
    if (size < sizeof(eh))
        REJECT("shorter than the ELF header");
    memcpy(&eh, img, sizeof(eh));
    if (memcmp(eh.e_ident, "\x7f" "ELF", 4))
        REJECT("not an ELF file");
    if (eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != ELFDATA2LSB ||
        eh.e_ident[EI_VERSION] != EV_CURRENT)
        REJECT("not a little-endian ELF64 file");
    if (eh.e_ident[EI_OSABI] != ELFOSABI_SYSV && eh.e_ident[EI_OSABI] != ELFOSABI_GNU)
        REJECT("unknown OS ABI");
    if (eh.e_type != ET_EXEC)
        REJECT("not a static executable (ET_EXEC)");
    if (eh.e_machine != EM_X86_64 || eh.e_version != EV_CURRENT)
        REJECT("not an x86-64 executable");
    if (eh.e_ehsize != sizeof(struct elf64_ehdr))
        REJECT("bad ELF header size");
    if (eh.e_phentsize != sizeof(struct elf64_phdr))
        REJECT("bad program header size");
    if (eh.e_phnum == 0 || eh.e_phnum > ELF_MAX_PHDRS)
        REJECT("no or too many program headers");
    if (eh.e_phoff % 8)
        REJECT("misaligned program header table");
    /* No overflow: phnum * 56 is at most 3584. */
    if (eh.e_phoff > size || (uint64_t)eh.e_phnum * sizeof(struct elf64_phdr) > size - eh.e_phoff)
        REJECT("program headers outside the file");

    struct elf_plan plan = { .entry = eh.e_entry, .nseg = 0 };
    bool entry_ok = false;
    for (unsigned i = 0; i < eh.e_phnum; i++) {
        struct elf64_phdr ph;
        memcpy(&ph, img + eh.e_phoff + (uint64_t)i * sizeof(ph), sizeof(ph));
        switch (ph.p_type) {
        case PT_LOAD:
            break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
            REJECT("dynamically linked");
        case PT_TLS:
            REJECT("uses thread-local storage (not supported)");
        default:
            continue;   /* PT_NOTE, PT_PHDR, PT_GNU_STACK...: nothing to load */
        }

        if (plan.nseg == ELF_MAX_SEGMENTS)
            REJECT("too many PT_LOAD segments");
        if (!(ph.p_flags & PF_R))
            REJECT("segment is not readable");
        if ((ph.p_flags & PF_W) && (ph.p_flags & PF_X))
            REJECT("segment is writable and executable");
        if (ph.p_memsz == 0)
            REJECT("empty segment");
        if (ph.p_filesz > ph.p_memsz)
            REJECT("segment file size exceeds its memory size");
        if (!user_range_ok(ph.p_vaddr, ph.p_memsz))
            REJECT("segment outside the user address range");
        if (ph.p_offset > size || ph.p_filesz > size - ph.p_offset)
            REJECT("segment data outside the file");
        if (ph.p_align > 1 && (ph.p_align & (ph.p_align - 1)))
            REJECT("segment alignment is not a power of two");
        if ((ph.p_vaddr - ph.p_offset) % PAGE_SIZE)
            REJECT("segment address and file offset differ mod 4 KiB");
        if (plan.nseg) {
            const struct elf_segment *prev = &plan.seg[plan.nseg - 1];
            /* Page-granular: two segments may not share a page, since a
             * page has one set of permissions. user_range_ok above keeps
             * these sums below USER_TOP. */
            if (ALIGN_DOWN(ph.p_vaddr, PAGE_SIZE) < ALIGN_UP(prev->vaddr + prev->memsz, PAGE_SIZE))
                REJECT("segments overlap, share a page or are out of order");
        }

        struct elf_segment *s = &plan.seg[plan.nseg++];
        s->vaddr = ph.p_vaddr;
        s->memsz = ph.p_memsz;
        s->file_off = ph.p_offset;
        s->filesz = ph.p_filesz;
        s->flags = ASPACE_READ | (ph.p_flags & PF_W ? ASPACE_WRITE : 0) |
                   (ph.p_flags & PF_X ? ASPACE_EXEC : 0);
        if ((s->flags & ASPACE_EXEC) && eh.e_entry >= s->vaddr && eh.e_entry - s->vaddr < s->memsz)
            entry_ok = true;
    }
    if (plan.nseg == 0)
        REJECT("no PT_LOAD segments");
    if (!entry_ok)
        REJECT("entry point is not inside an executable segment");

    *out = plan;
    *why = NULL;
    return OK;
#undef REJECT
}

status_t elf_parse(const void *image, uint64_t size, struct elf_plan *out)
{
    const char *why;
    status_t st = elf_check(image, size, out, &why);
    if (st != OK)
        kprintf("elf: rejected: %s\n", why);
    return st;
}
