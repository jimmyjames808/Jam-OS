/* Starting programs: libos's ELF loader (the user-space counterpart of the
 * kernel's userboot, see kernel/core/userboot.c for the same rules).
 *
 * The program comes from bootfs (the SR_BOOTFS VMO, mapped once by
 * bootfs_default). The steps are all ordinary system
 * calls: process_create gives the new process and a vmar for its address
 * space; each PT_LOAD segment is mapped into that vmar (read-only and
 * executable ones straight from the bootfs VMO, writable ones copied into a
 * fresh VMO), then a stack with a guard page below it, a thread, and a
 * startup message on a new channel whose other end process_start hands to
 * the program in rdi.
 *
 * Memory: the VMOs made here (data, bss, stack) are created by the CALLER,
 * so their pages are charged to the caller's job, not the child's. What
 * the child allocates itself (its heap, its own VMOs) is the child's.
 *
 * The kernel re-checks everything that matters (W^X, rights, ranges); the
 * checks here only keep this code from reading outside the file. Nothing
 * here uses malloc, so spawning doesn't grow the caller's heap. */
#include <os.h>

#define PAGE 4096ull
#define STACK_PAGES 32   /* 128 KiB */
#define STACK_TOP   0x00007ff000000000ull
#define MAX_SEGS    8
#define MSG_MAX     2048

/* The ELF64 bits we read (the kernel's elf.c has the full checks). */
struct ehdr {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
};

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4

static uint64_t down(uint64_t x) { return x & ~(PAGE - 1); }
static uint64_t up(uint64_t x) { return (x + PAGE - 1) & ~(PAGE - 1); }

/* Map one PT_LOAD segment of the file at file_off (in the bootfs VMO)
 * into vmar. */
static status_t load_segment(handle_t vmar, handle_t bootfs_vmo, uint64_t file_off,
                             const uint8_t *file, uint64_t file_size, const struct phdr *ph)
{
    if (ph->filesz > ph->memsz || ph->offset > file_size || ph->filesz > file_size - ph->offset)
        return ERR_INVALID_ARGS;
    uint64_t lo = down(ph->vaddr), hi = up(ph->vaddr + ph->memsz), addr = lo;
    uint32_t perms = VMAR_READ | ((ph->flags & PF_W) ? VMAR_WRITE : 0) |
                     ((ph->flags & PF_X) ? VMAR_EXEC : 0);
    if (!(ph->flags & PF_W) && ph->memsz == ph->filesz) {
        /* Straight from bootfs: read-only (or execute), never written. */
        return jam_vmar_map(vmar, bootfs_vmo, file_off + down(ph->offset), hi - lo,
                            perms | VMAR_FIXED, &addr);
    }
    handle_t v;
    status_t st = jam_vmo_create(hi - lo, 0, HANDLE_INVALID, &v);
    if (st != OK)
        return st;
    if (ph->filesz)
        st = jam_vmo_write(v, ph->vaddr - lo, file + ph->offset, ph->filesz);
    if (st == OK)
        st = jam_vmar_map(vmar, v, 0, hi - lo, perms | VMAR_FIXED, &addr);
    jam_handle_close(v);   /* the mapping keeps it */
    return st;
}

static status_t load_elf(handle_t vmar, handle_t bootfs_vmo, uint64_t file_off,
                         const uint8_t *file, uint64_t size, uint64_t *entry)
{
    struct ehdr eh;
    if (size < sizeof(eh))
        return ERR_INVALID_ARGS;
    memcpy(&eh, file, sizeof(eh));
    if (memcmp(eh.ident, "\x7f" "ELF", 4) || eh.ident[4] != 2 || eh.type != 2 ||
        eh.machine != 62 || eh.phentsize != sizeof(struct phdr) || eh.phnum == 0 ||
        eh.phnum > 64 || eh.phoff > size || eh.phnum * sizeof(struct phdr) > size - eh.phoff)
        return ERR_INVALID_ARGS;
    unsigned loads = 0;
    for (unsigned i = 0; i < eh.phnum; i++) {
        struct phdr ph;
        memcpy(&ph, file + eh.phoff + i * sizeof(ph), sizeof(ph));
        if (ph.type != PT_LOAD)
            continue;
        if (++loads > MAX_SEGS)
            return ERR_INVALID_ARGS;
        status_t st = load_segment(vmar, bootfs_vmo, file_off, file, size, &ph);
        if (st != OK)
            return st;
    }
    *entry = eh.entry;
    return loads ? OK : ERR_INVALID_ARGS;
}

/* A stack with a no-access guard page below it; *top is 16-aligned. */
static status_t make_stack(handle_t vmar, uint64_t *top)
{
    uint64_t size = (STACK_PAGES + 1) * PAGE, base = STACK_TOP - size, addr = base;
    handle_t v;
    status_t st = jam_vmo_create(size, 0, HANDLE_INVALID, &v);
    if (st != OK)
        return st;
    st = jam_vmar_map(vmar, v, 0, size, VMAR_READ | VMAR_WRITE | VMAR_FIXED, &addr);
    jam_handle_close(v);
    if (st == OK)
        st = jam_vmar_protect(vmar, base, PAGE, 0);   /* the guard */
    *top = STACK_TOP;
    return st;
}

static void close_extras(const struct spawn_args *a)
{
    for (unsigned i = 0; i < a->nextra; i++)
        jam_handle_close(a->extra[i].h);
}

status_t spawn(const struct spawn_args *a, handle_t *proc_out)
{
    const struct bootfs_view *fs;
    const void *data;
    uint64_t size;
    handle_t bootfs_vmo = startup_handle(SR_BOOTFS);
    status_t st = bootfs_default(&fs);
    if (st == OK)
        st = bootfs_lookup(fs, a->path, &data, &size);
    if (st == OK && a->nextra > STARTUP_MAX_HANDLES - 5)
        st = ERR_OUT_OF_RANGE;
    if (st != OK) {
        close_extras(a);
        return st;
    }
    const char *name = a->name;
    if (!name) {
        name = a->path;
        for (const char *c = a->path; *c; c++)
            if (*c == '/')
                name = c + 1;
    }

    /* The startup message, built on the stack. */
    _Alignas(8) uint8_t msg[MSG_MAX];
    struct startup_msg *m = (struct startup_msg *)msg;
    memset(m, 0, sizeof(*m));
    m->magic = STARTUP_MAGIC;
    m->version = STARTUP_VERSION;
    m->argc = (uint32_t)a->argc;
    size_t len = sizeof(*m);
    for (int i = 0; i < a->argc; i++) {
        size_t l = strlen(a->argv[i]) + 1;
        if (len + l > sizeof(msg)) {
            close_extras(a);
            return ERR_OUT_OF_RANGE;
        }
        memcpy(msg + len, a->argv[i], l);
        len += l;
    }
    m->strings_len = (uint32_t)(len - sizeof(*m));

    handle_t proc, vmar, thread = HANDLE_INVALID, ch[2] = { HANDLE_INVALID, HANDLE_INVALID };
    st = jam_process_create(a->job, name, strlen(name), 0, &proc, &vmar);
    if (st != OK) {
        close_extras(a);
        return st;
    }
    uint64_t entry = 0, stack = 0;
    st = load_elf(vmar, bootfs_vmo, (uint64_t)((const uint8_t *)data - fs->base), data, size,
                  &entry);
    if (st == OK)
        st = make_stack(vmar, &stack);
    if (st == OK)
        st = jam_thread_create(proc, name, strlen(name), 0, &thread);

    /* The handles: duplicates of ours, the vmar itself (we don't keep it),
     * then the extras. Until the write succeeds they are all ours to close
     * on failure. */
    handle_t hs[STARTUP_MAX_HANDLES];
    rights_t rs[STARTUP_MAX_HANDLES];
    unsigned n = 0;
    /* The child's own job goes without RIGHT_MANAGE (JOB_RIGHTS_OWN): the
     * limits we set on it are ours to change, not the child's. */
    const struct { uint32_t role; handle_t h; rights_t rights; } dup[] = {
        { SR_SELF_PROCESS, proc, RIGHT_SAME }, { SR_SELF_THREAD, thread, RIGHT_SAME },
        { SR_JOB, a->job, JOB_RIGHTS_OWN }, { SR_BOOTFS, bootfs_vmo, RIGHT_SAME },
    };
    for (unsigned i = 0; st == OK && i < sizeof(dup) / sizeof(dup[0]); i++) {
        st = jam_handle_duplicate(dup[i].h, dup[i].rights, &hs[n]);
        if (st == OK) {
            rs[n] = RIGHT_SAME;
            m->roles[n++] = dup[i].role;
        }
    }
    if (st == OK) {
        hs[n] = vmar;
        rs[n] = RIGHT_SAME;
        m->roles[n++] = SR_SELF_VMAR;
        vmar = HANDLE_INVALID;
    }
    for (unsigned i = 0; i < a->nextra; i++) {
        hs[n] = a->extra[i].h;
        rs[n] = a->extra_rights && a->extra_rights[i] ? a->extra_rights[i] : RIGHT_SAME;
        m->roles[n++] = a->extra[i].role;
    }
    m->nhandles = n;
    if (st == OK)
        st = jam_channel_create(&ch[0], &ch[1]);
    if (st == OK)   /* the extras with the rights the caller chose (M7) */
        st = jam_channel_write_rights(ch[0], msg, (uint32_t)len, hs, rs, n);
    if (st == OK)
        n = 0;   /* all in the message now */
    if (st == OK) {
        st = jam_process_start(proc, thread, entry, stack, ch[1], 0);
        if (st == OK)
            ch[1] = HANDLE_INVALID;   /* the child's now */
    }

    /* Clean up. A message that never reached the child dies with ch[1],
     * handles and all; a process that never started dies with its last
     * handle. */
    for (unsigned i = 0; i < n; i++)
        jam_handle_close(hs[i]);
    if (ch[0])
        jam_handle_close(ch[0]);
    if (ch[1])
        jam_handle_close(ch[1]);
    if (thread)
        jam_handle_close(thread);
    if (vmar)
        jam_handle_close(vmar);
    if (st != OK) {
        jam_handle_close(proc);
        return st;
    }
    *proc_out = proc;
    return OK;
}

status_t spawn_wait(handle_t proc, uint64_t timeout_ns, struct process_info *info)
{
    signals_t seen;
    status_t st = jam_object_wait_one(proc, SIG_TERMINATED,
                                      (uint64_t)jam_clock_get() + timeout_ns, &seen);
    if (st == OK && info)
        st = jam_process_get_info(proc, info);
    return st;
}

/* ---- threads -------------------------------------------------------------- */

/* A new thread starts here: rdi = fn, rsi = arg, rsp 16-aligned minus 8
 * (as if called). */
static _Noreturn void thread_entry(void (*fn)(void *), void *arg)
{
    fn(arg);
    jam_thread_exit();
}

status_t thread_spawn(const char *name, void (*fn)(void *), void *arg, void *stack,
                      size_t stack_size, handle_t *out)
{
    handle_t self = startup_handle(SR_SELF_PROCESS), t;
    status_t st = jam_thread_create(self, name, strlen(name), 0, &t);
    if (st != OK)
        return st;
    uint64_t top = ((uint64_t)(uintptr_t)stack + stack_size) & ~15ull;
    st = jam_thread_start(t, (uint64_t)(uintptr_t)thread_entry, top - 8,
                          (uint64_t)(uintptr_t)fn, (uint64_t)(uintptr_t)arg);
    if (st != OK) {
        jam_handle_close(t);
        return st;
    }
    *out = t;
    return OK;
}
