/* glibc's <execinfo.h> for Jam OS's libc (docs/CR-PLAN.md, "BT notes"):
 * backtrace(), backtrace_symbols() and backtrace_symbols_fd(), which musl
 * does not have. Chromium's StackTrace and V8's use them wherever the C
 * library is glibc.
 *
 * backtrace() walks the stack by the program's own unwind tables: the FDE
 * for a frame's pc is found by .eh_frame_hdr's sorted index (posix.ld keeps
 * the section and its PT_GNU_EH_FRAME, the links ask for it), and its call
 * frame instructions say where the frame's caller left its return address.
 * It is written here, not on _Unwind_Backtrace, for three reasons: the
 * cross GCC's libgcc finds frames only that crtbegin.o registers (jamos-cc
 * links none, so a C program had no unwinder); a crash path wants a walk
 * that takes no lock and calls no function (libunwind's looks frames up
 * through dl_iterate_phdr and a cache under a lock); and the same code then
 * serves a program linked by either compiler. It runs on the caller's
 * stack, with no malloc, no stdio and no lock, so a signal handler may
 * call it. (On Jam OS a handler runs at a call boundary in libc, an
 * ordinary call below the frame that made the call: the layer's and
 * musl's own code carry unwind tables for that, posix.mk.)
 *
 * What it understands is what GCC and clang write for x86-64 C and C++: a
 * CIE of "zR", "zPLR" or "zPLRS", the call frame instructions that set a
 * frame's CFA from a register and an offset, save registers at offsets
 * from it and push and pop rules. A frame that uses an expression for its
 * CFA (a stack realigned through a DRAP register in hand-written
 * assembly) ends the trace; so does a frame without a table (the
 * program's _start, hand-written assembly), whose pc is the last one
 * stored.
 *
 * backtrace_symbols() and backtrace_symbols_fd() print the lines glibc
 * prints for a program that has no symbol it can find: a program on Jam
 * OS keeps its symbol table in its file, not in memory (an ELF loader maps
 * the segments only), so an address in the program is "name() [0xaddr]"
 * with the name argv[0] gives, and one outside it "[0xaddr]". Chromium's
 * StackTrace reads the program's own file for names (base/third_party/
 * symbolize) and does not call these. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <elf.h>
#include <errno.h>
#include <execinfo.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

/* The ELF header the link puts at the start of the first segment (posix.ld:
 * FILEHDR PHDRS), by which the program finds its own program headers. */
extern const char __ehdr_start[] __attribute__((visibility("hidden")));

/* x86-64's DWARF register numbers (the psABI's): the ones backtrace() must
 * know are rbx 3, rbp 6, rsp 7 and r12 to r15; the return address is 16. */
#define NREG  17
#define R_RSP 7
#define R_RA  16

/* ---- reading the tables --------------------------------------------------------------- */

struct cursor {
    const uint8_t *p, *end;
    bool bad;            /* a read ran past the end, or met a form we don't know */
};

static uint8_t get_u8(struct cursor *c)
{
    if (c->p >= c->end) {
        c->bad = true;
        return 0;
    }
    return *c->p++;
}

/* n (at most 8) bytes, little-endian, unsigned. */
static uint64_t get_un(struct cursor *c, unsigned n)
{
    uint64_t v = 0;
    if ((size_t)(c->end - c->p) < n) {
        c->bad = true;
        c->p = c->end;
        return 0;
    }
    memcpy(&v, c->p, n);
    c->p += n;
    return v;
}

static uint64_t get_uleb(struct cursor *c)
{
    uint64_t v = 0;
    for (unsigned shift = 0; c->p < c->end; shift += 7) {
        uint8_t b = *c->p++;
        if (shift < 64)
            v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80))
            return v;
    }
    c->bad = true;
    return 0;
}

static int64_t get_sleb(struct cursor *c)
{
    uint64_t v = 0;
    for (unsigned shift = 0; c->p < c->end; ) {
        uint8_t b = *c->p++;
        if (shift < 64)
            v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            if (shift < 64 && (b & 0x40))
                v |= ~(uint64_t)0 << shift;
            return (int64_t)v;
        }
    }
    c->bad = true;
    return 0;
}

/* A pointer in the form enc says (DW_EH_PE_*): the format in the low four
 * bits, what it is relative to in bits 4 to 6 (data: the base for
 * "datarel", the start of .eh_frame_hdr). 0xff is "no value". The forms
 * only a personality or a language-specific area uses with an indirection
 * (bit 7) still have the size of their format, so skipping one works. */
static uint64_t get_encoded(struct cursor *c, uint8_t enc, uintptr_t data)
{
    if (enc == 0xff)
        return 0;
    uintptr_t at = (uintptr_t)c->p;
    uint64_t v;
    switch (enc & 0x0f) {
    case 0x00: case 0x04: case 0x0c: v = get_un(c, 8); break;
    case 0x01: v = get_uleb(c); break;
    case 0x02: v = get_un(c, 2); break;
    case 0x03: v = get_un(c, 4); break;
    case 0x09: v = (uint64_t)get_sleb(c); break;
    case 0x0a: v = (uint64_t)(int64_t)(int16_t)get_un(c, 2); break;
    case 0x0b: v = (uint64_t)(int64_t)(int32_t)get_un(c, 4); break;
    default: c->bad = true; return 0;
    }
    switch (enc & 0x70) {
    case 0x00: break;
    case 0x10: v += at; break;
    case 0x30: v += data; break;
    default: c->bad = true; return 0;   /* textrel, funcrel, aligned */
    }
    return v;
}

/* ---- the program's own image ---------------------------------------------------------- */

struct image {
    uintptr_t bias;            /* where the program is, less where it was linked (0: ET_EXEC) */
    uintptr_t lo, hi;          /* its loaded segments: lo up to hi */
    const uint8_t *hdr;        /* .eh_frame_hdr, or NULL */
};

static void image_get(struct image *im)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)__ehdr_start;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)((const char *)eh + eh->e_phoff);
    bool first = true;
    im->bias = 0;
    im->lo = ~(uintptr_t)0;
    im->hi = 0;
    im->hdr = NULL;
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            if (first)
                im->bias = (uintptr_t)eh - (ph[i].p_vaddr - ph[i].p_offset);
            first = false;
        }
    }
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            uintptr_t lo = ph[i].p_vaddr + im->bias, hi = lo + ph[i].p_memsz;
            if (lo < im->lo)
                im->lo = lo;
            if (hi > im->hi)
                im->hi = hi;
        } else if (ph[i].p_type == PT_GNU_EH_FRAME) {
            im->hdr = (const uint8_t *)(ph[i].p_vaddr + im->bias);
        }
    }
}

/* ---- the unwind tables ---------------------------------------------------------------- */

struct cie {
    uint64_t code_align;
    int64_t  data_align;
    unsigned ra;               /* the return address's register */
    uint8_t  fde_enc;          /* how an FDE's pc range is written */
    bool     z;                /* the augmentation data has a length ("z") */
    const uint8_t *insn, *end; /* the initial instructions */
};

/* Read the CIE at p; false for one we don't understand. */
static bool cie_parse(const uint8_t *p, struct cie *ci)
{
    uint32_t len;
    memcpy(&len, p, 4);
    if (len == 0 || len == 0xffffffffu)   /* the end, or a 64-bit length */
        return false;
    struct cursor c = { p + 4, p + 4 + len, false };
    if (get_un(&c, 4) != 0)               /* a CIE has id 0 */
        return false;
    uint8_t version = get_u8(&c);
    if (version != 1 && version != 3)
        return false;
    const char *aug = (const char *)c.p;
    while (get_u8(&c) != 0)
        ;
    ci->code_align = get_uleb(&c);
    ci->data_align = get_sleb(&c);
    ci->ra = version == 1 ? get_u8(&c) : (unsigned)get_uleb(&c);
    ci->fde_enc = 0;
    ci->z = aug[0] == 'z';
    if (ci->z) {
        uint64_t n = get_uleb(&c);
        if (c.bad || n > (size_t)(c.end - c.p))
            return false;
        struct cursor a = { c.p, c.p + n, false };
        c.p += n;
        for (const char *s = aug + 1; *s && !a.bad; s++) {
            if (*s == 'R') {
                ci->fde_enc = get_u8(&a);
            } else if (*s == 'L') {
                (void)get_u8(&a);
            } else if (*s == 'P') {
                uint8_t enc = get_u8(&a);
                (void)get_encoded(&a, enc & 0x7f, 0);
            } else if (*s != 'S') {
                break;   /* one we don't know: the length lets us go on without it */
            }
        }
        if (a.bad)
            return false;
    } else if (aug[0]) {
        return false;    /* the old "eh" augmentation */
    }
    if (c.bad)
        return false;
    ci->insn = c.p;
    ci->end = c.end;
    return true;
}

/* The rule for one register in a row of the table: how to get its value in
 * the caller's frame. */
enum { RULE_SAME, RULE_UNDEF, RULE_OFFSET, RULE_VAL_OFFSET, RULE_REG, RULE_BAD };

struct rule {
    uint8_t kind;
    int64_t arg;               /* RULE_OFFSET, RULE_VAL_OFFSET: from the CFA; RULE_REG: the register */
};

/* One row of the unwind table: the CFA, and a rule per register. */
struct row {
    bool        cfa_ok;        /* false: an expression, which we don't run */
    uint8_t     cfa_reg;
    int64_t     cfa_off;
    struct rule reg[NREG];
};

#define SAVED_MAX 4   /* remember_state's depth; the compilers' output uses 1 or 2 */

static void rule_set(struct row *row, uint64_t reg, uint8_t kind, int64_t arg)
{
    if (reg < NREG)
        row->reg[reg] = (struct rule){ kind, arg };
}

/* Run call frame instructions from c until the row covers pc (the first
 * advance past it) or they end. loc is the address the first row starts
 * at. init is the CIE's row (what DW_CFA_restore goes back to), NULL while
 * running the CIE's own. False for an instruction we can't follow. */
static bool cfi_run(struct cursor *c, const struct cie *ci, uint64_t loc, uint64_t pc,
                    struct row *row, const struct row *init)
{
    struct row saved[SAVED_MAX];
    unsigned nsaved = 0;
    while (c->p < c->end && !c->bad) {
        uint8_t op = *c->p++;
        unsigned lo = op & 0x3f;
        switch (op & 0xc0) {
        case 0x40:   /* DW_CFA_advance_loc */
            loc += (uint64_t)lo * ci->code_align;
            if (loc > pc)
                return true;
            continue;
        case 0x80:   /* DW_CFA_offset */
            rule_set(row, lo, RULE_OFFSET, (int64_t)get_uleb(c) * ci->data_align);
            continue;
        case 0xc0:   /* DW_CFA_restore */
            if (lo < NREG)
                row->reg[lo] = init ? init->reg[lo] : (struct rule){ RULE_SAME, 0 };
            continue;
        }
        uint64_t reg, delta;
        switch (op) {
        case 0x00:   /* DW_CFA_nop */
            break;
        case 0x02: case 0x03: case 0x04:   /* DW_CFA_advance_loc1, 2, 4 */
            delta = get_un(c, op == 0x02 ? 1 : op == 0x03 ? 2 : 4);
            loc += delta * ci->code_align;
            if (loc > pc)
                return !c->bad;
            break;
        case 0x05:   /* DW_CFA_offset_extended */
            reg = get_uleb(c);
            rule_set(row, reg, RULE_OFFSET, (int64_t)get_uleb(c) * ci->data_align);
            break;
        case 0x06:   /* DW_CFA_restore_extended */
            reg = get_uleb(c);
            if (reg < NREG)
                row->reg[reg] = init ? init->reg[reg] : (struct rule){ RULE_SAME, 0 };
            break;
        case 0x07:   /* DW_CFA_undefined */
            rule_set(row, get_uleb(c), RULE_UNDEF, 0);
            break;
        case 0x08:   /* DW_CFA_same_value */
            rule_set(row, get_uleb(c), RULE_SAME, 0);
            break;
        case 0x09:   /* DW_CFA_register */
            reg = get_uleb(c);
            rule_set(row, reg, RULE_REG, (int64_t)get_uleb(c));
            break;
        case 0x0a:   /* DW_CFA_remember_state */
            if (nsaved == SAVED_MAX)
                return false;
            saved[nsaved++] = *row;
            break;
        case 0x0b:   /* DW_CFA_restore_state */
            if (nsaved == 0)
                return false;
            *row = saved[--nsaved];
            break;
        case 0x0c:   /* DW_CFA_def_cfa */
            row->cfa_reg = (uint8_t)get_uleb(c);
            row->cfa_off = (int64_t)get_uleb(c);
            row->cfa_ok = true;
            break;
        case 0x0d:   /* DW_CFA_def_cfa_register */
            row->cfa_reg = (uint8_t)get_uleb(c);
            row->cfa_ok = true;
            break;
        case 0x0e:   /* DW_CFA_def_cfa_offset */
            row->cfa_off = (int64_t)get_uleb(c);
            break;
        case 0x0f:   /* DW_CFA_def_cfa_expression: a block we don't run */
            row->cfa_ok = false;
            c->p += (size_t)(c->end - c->p) < (delta = get_uleb(c)) ? (size_t)(c->end - c->p) : delta;
            break;
        case 0x10: case 0x16:   /* DW_CFA_expression, DW_CFA_val_expression */
            reg = get_uleb(c);
            rule_set(row, reg, RULE_BAD, 0);
            c->p += (size_t)(c->end - c->p) < (delta = get_uleb(c)) ? (size_t)(c->end - c->p) : delta;
            break;
        case 0x11:   /* DW_CFA_offset_extended_sf */
            reg = get_uleb(c);
            rule_set(row, reg, RULE_OFFSET, get_sleb(c) * ci->data_align);
            break;
        case 0x12:   /* DW_CFA_def_cfa_sf */
            row->cfa_reg = (uint8_t)get_uleb(c);
            row->cfa_off = get_sleb(c) * ci->data_align;
            row->cfa_ok = true;
            break;
        case 0x13:   /* DW_CFA_def_cfa_offset_sf */
            row->cfa_off = get_sleb(c) * ci->data_align;
            break;
        case 0x14:   /* DW_CFA_val_offset */
            reg = get_uleb(c);
            rule_set(row, reg, RULE_VAL_OFFSET, (int64_t)get_uleb(c) * ci->data_align);
            break;
        case 0x15:   /* DW_CFA_val_offset_sf */
            reg = get_uleb(c);
            rule_set(row, reg, RULE_VAL_OFFSET, get_sleb(c) * ci->data_align);
            break;
        case 0x2e:   /* DW_CFA_GNU_args_size */
            (void)get_uleb(c);
            break;
        case 0x2f:   /* DW_CFA_GNU_negative_offset_extended */
            reg = get_uleb(c);
            rule_set(row, reg, RULE_OFFSET, -(int64_t)get_uleb(c) * ci->data_align);
            break;
        default:
            return false;
        }
    }
    return !c->bad;
}

/* The FDE covering pc, by .eh_frame_hdr's table (sorted pairs of 32-bit
 * offsets from the header: the function's start, its FDE); NULL if none. */
static const uint8_t *fde_find(const struct image *im, uintptr_t pc)
{
    if (!im->hdr || pc < im->lo || pc >= im->hi)
        return NULL;
    const uint8_t *hdr = im->hdr;
    if (hdr[0] != 1 || hdr[3] != 0x3b)   /* version 1; the table as sdata4, datarel */
        return NULL;
    struct cursor c = { hdr + 4, hdr + 4 + 32, false };
    (void)get_encoded(&c, hdr[1], (uintptr_t)hdr);          /* .eh_frame's address */
    uint64_t count = get_encoded(&c, hdr[2], (uintptr_t)hdr);
    if (c.bad || hdr[2] == 0xff)
        return NULL;
    const uint8_t *tab = c.p;
    uint64_t lo = 0, hi = count;
    while (lo < hi) {   /* the last entry that starts at or before pc */
        uint64_t mid = lo + (hi - lo) / 2;
        int32_t start;
        memcpy(&start, tab + mid * 8, 4);
        if ((uintptr_t)hdr + (uintptr_t)(intptr_t)start <= pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL;
    int32_t off;
    memcpy(&off, tab + (lo - 1) * 8 + 4, 4);
    return hdr + (intptr_t)off;
}

/* The row of the unwind table for pc, from its FDE: false if there is
 * none or we can't read it. */
static bool row_for(const struct image *im, uintptr_t pc, struct row *row)
{
    const uint8_t *fde = fde_find(im, pc);
    if (!fde)
        return false;
    uint32_t len, back;
    memcpy(&len, fde, 4);
    memcpy(&back, fde + 4, 4);
    if (len == 0 || len == 0xffffffffu)
        return false;
    struct cie ci;
    if (!cie_parse(fde + 4 - back, &ci))
        return false;
    struct cursor c = { fde + 8, fde + 4 + len, false };
    uint64_t begin = get_encoded(&c, ci.fde_enc, 0);
    uint64_t range = get_encoded(&c, ci.fde_enc & 0x0f, 0);
    if (ci.z) {
        uint64_t n = get_uleb(&c);
        if (n > (size_t)(c.end - c.p))
            return false;
        c.p += n;
    }
    if (c.bad || pc < begin || pc - begin >= range)
        return false;
    memset(row, 0, sizeof *row);
    struct cursor cc = { ci.insn, ci.end, false };
    if (!cfi_run(&cc, &ci, 0, ~(uint64_t)0, row, NULL))
        return false;
    struct row init = *row;
    return cfi_run(&c, &ci, begin, pc, row, &init);
}

/* One frame up: r holds the registers as the frame had them and pc is
 * the frame's own (a return address); on true they are the caller's. The
 * lookup is by pc - 1, the call instruction, as a return address may be
 * the first byte of the next function (after a call that never returns). */
static bool unwind_step(const struct image *im, uint64_t r[NREG], uintptr_t *pc)
{
    struct row row;
    if (!row_for(im, *pc - 1, &row) || !row.cfa_ok || row.cfa_reg >= NREG)
        return false;
    uint64_t cfa = r[row.cfa_reg] + (uint64_t)row.cfa_off;
    if (cfa <= r[R_RSP])   /* the stack grows down: a caller's frame is above its callee's */
        return false;
    uint8_t ra_kind = row.reg[R_RA].kind;
    if (ra_kind != RULE_OFFSET && ra_kind != RULE_VAL_OFFSET && ra_kind != RULE_REG)
        return false;   /* undefined: the outermost frame; or none we know */
    uint64_t n[NREG];
    for (unsigned i = 0; i < NREG; i++) {
        const struct rule *rl = &row.reg[i];
        switch (rl->kind) {
        case RULE_SAME:
            n[i] = r[i];
            break;
        case RULE_OFFSET:
            memcpy(&n[i], (const void *)(uintptr_t)(cfa + (uint64_t)rl->arg), 8);
            break;
        case RULE_VAL_OFFSET:
            n[i] = cfa + (uint64_t)rl->arg;
            break;
        case RULE_REG:
            n[i] = (uint64_t)rl->arg < NREG ? r[rl->arg] : 0;
            break;
        default:   /* undefined, or an expression: the caller's value is not known */
            n[i] = 0;
            break;
        }
    }
    if (n[R_RA] == 0)
        return false;
    n[R_RSP] = cfa;
    for (unsigned i = 0; i < NREG; i++)
        r[i] = n[i];
    *pc = (uintptr_t)n[R_RA];
    return true;
}

/* ---- backtrace ------------------------------------------------------------------------ */

/* What backtrace's entry saves: the registers a caller expects kept, its
 * stack pointer as it will be after the return, and its return address. */
struct entry {
    uint64_t rbx, rbp, r12, r13, r14, r15, rsp, rip;
};

__attribute__((visibility("hidden")))
int __jam_backtrace_walk(void **buf, int size, const struct entry *e)
{
    struct image im;
    image_get(&im);
    uint64_t r[NREG] = { 0 };
    r[3] = e->rbx;
    r[6] = e->rbp;
    r[R_RSP] = e->rsp;
    r[12] = e->r12;
    r[13] = e->r13;
    r[14] = e->r14;
    r[15] = e->r15;
    uintptr_t pc = e->rip;
    int n = 0;
    while (n < size) {
        buf[n++] = (void *)pc;
        if (!unwind_step(&im, r, &pc))
            break;
    }
    return n;
}

/* backtrace itself is assembly: the walk needs the registers as the
 * caller left them, which a C function's own prologue may already have
 * changed. It saves them in a struct entry on the stack and calls the walk
 * with it; rdi (the array) and esi (its size) pass through. */
__asm__(
    ".section .text.backtrace, \"ax\", @progbits\n"
    ".globl backtrace\n"
    ".type backtrace, @function\n"
    "backtrace:\n"
    "    .cfi_startproc\n"
    "    subq $72, %rsp\n"           /* the struct (64 bytes), and 16-aligned at the call */
    "    .cfi_adjust_cfa_offset 72\n"
    "    movq %rbx, 0(%rsp)\n"
    "    movq %rbp, 8(%rsp)\n"
    "    movq %r12, 16(%rsp)\n"
    "    movq %r13, 24(%rsp)\n"
    "    movq %r14, 32(%rsp)\n"
    "    movq %r15, 40(%rsp)\n"
    "    leaq 80(%rsp), %rax\n"      /* rsp once backtrace has returned */
    "    movq %rax, 48(%rsp)\n"
    "    movq 72(%rsp), %rax\n"      /* the return address */
    "    movq %rax, 56(%rsp)\n"
    "    movq %rsp, %rdx\n"
    "    call __jam_backtrace_walk\n"
    "    addq $72, %rsp\n"
    "    .cfi_adjust_cfa_offset -72\n"
    "    ret\n"
    "    .cfi_endproc\n"
    ".size backtrace, . - backtrace\n"
    ".text\n");

/* ---- the lines ------------------------------------------------------------------------ */

/* The program's name as glibc names its main object: argv[0]. */
static const char *program_name(void)
{
    return program_invocation_name ? program_invocation_name : "";
}

/* "[0x" and the address in hex, "]", as far as cap allows; the length the
 * whole would be. */
static size_t hex_tail(char *out, size_t cap, uintptr_t addr)
{
    char digits[2 * sizeof addr];
    size_t nd = 0;
    do {
        digits[nd++] = "0123456789abcdef"[addr & 15];
        addr >>= 4;
    } while (addr);
    size_t n = 0;
    const char *head = "[0x";
    for (; *head; head++, n++)
        if (n < cap)
            out[n] = *head;
    while (nd--) {
        if (n < cap)
            out[n] = digits[nd];
        n++;
    }
    if (n < cap)
        out[n] = ']';
    return n + 1;
}

static bool in_program(uintptr_t addr)
{
    struct image im;
    image_get(&im);
    return addr >= im.lo && addr < im.hi;
}

void backtrace_symbols_fd(void *const *array, int size, int fd)
{
    for (int i = 0; i < size; i++) {
        char tail[2 * sizeof(uintptr_t) + 8];   /* "() [0x" , the digits, "]\n" */
        size_t n = 0;
        const char *name = "";
        if (in_program((uintptr_t)array[i])) {
            name = program_name();
            tail[n++] = '(';
            tail[n++] = ')';
            tail[n++] = ' ';
        }
        n += hex_tail(tail + n, sizeof tail - n - 1, (uintptr_t)array[i]);
        tail[n++] = '\n';
        struct iovec v[2] = { { (void *)name, strlen(name) }, { tail, n } };
        (void)writev(fd, v, 2);
    }
}

char **backtrace_symbols(void *const *array, int size)
{
    if (size < 0) {
        errno = EINVAL;
        return NULL;
    }
    size_t total = (size_t)size * sizeof(char *);
    size_t name_len = strlen(program_name());
    for (int i = 0; i < size; i++) {
        size_t line = hex_tail(NULL, 0, (uintptr_t)array[i]);
        if (in_program((uintptr_t)array[i]))
            line += name_len + 3;
        total += line + 1;
    }
    char **out = malloc(total ? total : 1);
    if (!out)
        return NULL;
    char *s = (char *)(out + size);
    for (int i = 0; i < size; i++) {
        out[i] = s;
        if (in_program((uintptr_t)array[i])) {
            memcpy(s, program_name(), name_len);
            s += name_len;
            *s++ = '(';
            *s++ = ')';
            *s++ = ' ';
        }
        size_t room = (size_t)(((char *)out + total) - s);
        s += hex_tail(s, room, (uintptr_t)array[i]);
        *s++ = '\0';
    }
    return out;
}
