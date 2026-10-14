/* Starting programs: libos's ELF loader (the user-space counterpart of the
 * kernel's userboot, see kernel/proc/userboot.c for the same rules).
 *
 * The program is a range of a VMO (struct image): a file of the bootfs
 * image (the SR_BOOTFS VMO, mapped once by bootfs_default), a range the
 * caller names, or a file read through the namespace into a VMO made for
 * it. A VMO other than the boot image's (or another physical one, which
 * vmo_read refuses and no pager backs) is never mapped here: its ELF
 * headers and writable segments are read from it with vmo_read. A
 * verified VMO (vmo_make_exec_verified) fetches each page from its pager
 * and checks it when first touched, and a page that doesn't match kills
 * whoever touched it through a mapping; through vmo_read it is ERR_IO for
 * the spawn instead, so a changed program fails to start rather than
 * taking its starter down. The steps are all ordinary system calls:
 * process_create gives the new process and a vmar for its address space;
 * each PT_LOAD segment is
 * mapped into that vmar (read-only and executable ones straight from the
 * program's VMO, writable ones copied into a fresh VMO), then a stack with
 * a guard page below it, a thread, and a startup message on a new channel
 * whose other end process_start hands to the program in rdi (and after
 * it, for a POSIX program, the hand-over messages: <posixfds.h>).
 *
 * Code is mapped executable only from a VMO handle with RIGHT_EXEC, and
 * only the bootfs image's has it (the kernel gives it to no VMO a process
 * makes): a program from any other VMO fails at its first executable
 * segment with ERR_ACCESS_DENIED.
 *
 * Memory: the VMOs made here (data, bss, stack, a file's copy) are created
 * by the CALLER, so their pages are charged to the caller's job, not the
 * child's. What the child allocates itself (its heap, its own VMOs) is the
 * child's.
 *
 * The kernel re-checks everything that matters (W^X, rights, ranges); the
 * checks here only keep this code from reading outside the file. Nothing
 * here uses malloc, so spawning doesn't grow the caller's heap. */
#include <jam/bootfs.h>
#include <os.h>
#include <posixfds.h>
#include "ns.h"

#define STACK_TOP   0x00007ff000000000ull
#define MAX_SEGS    8
#define MSG_MAX     2048
#define FILE_MAX    (1ull << 30)    /* the biggest program read from a file: VMO_VERIFIED_MAX */
/* What the child's SR_SELF_IMAGE can do (as far as our handle can): run
 * the program again, never change it. */
#define IMAGE_RIGHTS (RIGHT_READ | RIGHT_MAP | RIGHT_EXEC | RIGHT_DUPLICATE | RIGHT_TRANSFER)
#define BOOT_PREFIX "/boot/"         /* a boot-image program's path: /boot/<its name> */
/* What the child's SR_NS end can do: take the messages we write. */
#define NS_RIGHTS   (RIGHT_READ | RIGHT_WAIT)

/* Where the program's bytes are. */
struct image {
    handle_t       vmo;      /* the VMO holding them */
    uint64_t       off;      /* where they start in it, page-aligned */
    const uint8_t *data;     /* the same bytes mapped here (the boot image, or a physical
                              * VMO); NULL for any other VMO (read with vmo_read, never
                              * mapped here) */
    uint64_t       size;     /* how many */
    uint64_t       mapped;   /* bytes at data to unmap afterwards (0: bootfs's mapping, kept) */
    bool           own_vmo;  /* vmo was made here (a file's copy): closed afterwards */
};

/* The ELF64 bits we read (the kernel's elf.c has the full checks). */
struct ehdr {
    uint8_t  ident[16];                /* "\x7fELF", class 2: 64-bit */
    uint16_t type, machine;            /* 2: executable; 62: x86-64 */
    uint32_t version;                  /* the ELF version (not checked) */
    uint64_t entry, phoff, shoff;      /* entry point; program / section header offsets */
    uint32_t flags;                    /* none on x86-64 */
    uint16_t ehsize, phentsize, phnum; /* this header's size; a program header's; how many */
    uint16_t shentsize, shnum, shstrndx;   /* section headers (not used) */
};

struct phdr {
    uint32_t type, flags;              /* PT_*; PF_* */
    uint64_t offset, vaddr, paddr;     /* in the file; where it goes; (not used) */
    uint64_t filesz, memsz;            /* bytes in the file; in memory (the rest zero) */
    uint64_t align;                    /* (not used) */
};

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4

static uint64_t down(uint64_t x) { return x & ~(PAGE_SIZE - 1); }
static uint64_t up(uint64_t x) { return (x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); }

/* A process's or thread's name as the kernel takes it: cut to
 * PROCESS_NAME_MAX bytes here (the kernel refuses a longer one,
 * ERR_OUT_OF_RANGE). */
static uint64_t name_len(const char *name)
{
    size_t n = strlen(name);
    return n < PROCESS_NAME_MAX ? n : PROCESS_NAME_MAX;
}

/* n bytes of the program at off (inside it: the callers check) into buf. */
static status_t image_read(const struct image *img, uint64_t off, void *buf, uint64_t n)
{
    if (img->data) {
        memcpy(buf, img->data + off, n);
        return OK;
    }
    return jam_vmo_read(img->vmo, img->off + off, buf, n);
}

/* n bytes of the program at off into v at at: written from the boot
 * image's mapping, or read from the program's VMO straight into v, mapped
 * here for the copy (v is ours alone; the program's VMO is never mapped
 * here). */
static status_t image_copy(const struct image *img, uint64_t off, handle_t v, uint64_t at,
                           uint64_t n)
{
    if (img->data)
        return jam_vmo_write(v, at, img->data + off, n);
    handle_t self = startup_handle(SR_SELF_VMAR);
    uint64_t lo = down(at), len = up(at + n) - lo, addr = 0;
    status_t st = jam_vmar_map(self, v, lo, len, VMAR_READ | VMAR_WRITE, &addr);
    if (st != OK)
        return st;
    st = jam_vmo_read(img->vmo, img->off + off, (uint8_t *)(uintptr_t)addr + (at - lo), n);
    jam_vmar_unmap(self, addr, len);
    return st;
}

/* Map one PT_LOAD segment of the program into vmar. */
static status_t load_segment(handle_t vmar, const struct image *img, const struct phdr *ph)
{
    if (ph->filesz > ph->memsz || ph->offset > img->size ||
        ph->filesz > img->size - ph->offset)
        return ERR_INVALID_ARGS;
    uint64_t lo = down(ph->vaddr), hi = up(ph->vaddr + ph->memsz), addr = lo;
    uint32_t perms = VMAR_READ | ((ph->flags & PF_W) ? VMAR_WRITE : 0) |
                     ((ph->flags & PF_X) ? VMAR_EXEC : 0);
    if (!(ph->flags & PF_W) && ph->memsz == ph->filesz) {
        /* Straight from the program's VMO: read-only (or execute), never
         * written. */
        return jam_vmar_map(vmar, img->vmo, img->off + down(ph->offset), hi - lo,
                            perms | VMAR_FIXED, &addr);
    }
    handle_t v;
    status_t st = jam_vmo_create(hi - lo, 0, &v);
    if (st != OK)
        return st;
    if (ph->filesz)
        st = image_copy(img, ph->offset, v, ph->vaddr - lo, ph->filesz);
    if (st == OK)
        st = jam_vmar_map(vmar, v, 0, hi - lo, perms | VMAR_FIXED, &addr);
    jam_handle_close(v);   /* the mapping keeps it */
    return st;
}

static status_t load_elf(handle_t vmar, const struct image *img, uint64_t *entry)
{
    uint64_t size = img->size;
    struct ehdr eh;
    if (size < sizeof(eh))
        return ERR_INVALID_ARGS;
    status_t st = image_read(img, 0, &eh, sizeof(eh));
    if (st != OK)
        return st;
    if (memcmp(eh.ident, "\x7f" "ELF", 4) || eh.ident[4] != 2 || eh.type != 2 ||
        eh.machine != 62 || eh.phentsize != sizeof(struct phdr) || eh.phnum == 0 ||
        eh.phnum > 64 || eh.phoff > size || eh.phnum * sizeof(struct phdr) > size - eh.phoff)
        return ERR_INVALID_ARGS;
    unsigned loads = 0;
    for (unsigned i = 0; i < eh.phnum; i++) {
        struct phdr ph;
        st = image_read(img, eh.phoff + i * sizeof(ph), &ph, sizeof(ph));
        if (st != OK)
            return st;
        if (ph.type != PT_LOAD)
            continue;
        if (++loads > MAX_SEGS)
            return ERR_INVALID_ARGS;
        st = load_segment(vmar, img, &ph);
        if (st != OK)
            return st;
    }
    *entry = eh.entry;
    return loads ? OK : ERR_INVALID_ARGS;
}

/* A stack of `bytes` (whole pages, at most SPAWN_STACK_MAX) with a
 * no-access guard page below it; *top is 16-aligned. */
static status_t make_stack(handle_t vmar, uint64_t bytes, uint64_t *top)
{
    uint64_t size = bytes + PAGE_SIZE, base = STACK_TOP - size, addr = base;
    handle_t v;
    status_t st = jam_vmo_create(size, 0, &v);
    if (st != OK)
        return st;
    st = jam_vmar_map(vmar, v, 0, size, VMAR_READ | VMAR_WRITE | VMAR_FIXED, &addr);
    jam_handle_close(v);
    if (st == OK)
        st = jam_vmar_protect(vmar, base, PAGE_SIZE, 0);   /* the guard */
    *top = STACK_TOP;
    return st;
}

/* The caller's handles a spawn that never got as far as sending them
 * closes: the extras and the hand-over's. */
static void close_extras(const struct spawn_args *a)
{
    for (unsigned i = 0; i < a->nextra; i++)
        jam_handle_close(a->extra[i].h);
    posixfds_discard(a->handover);
}

/* The process name: a->name, or the last part of the path. */
static const char *child_name(const struct spawn_args *a)
{
    const char *name = a->name;
    if (!name) {
        name = a->path;
        for (const char *c = a->path; *c; c++)
            if (*c == '/')
                name = c + 1;
    }
    return name;
}

/* The startup message's header, argv and environment into msg (MSG_MAX
 * bytes); *len: its length so far (the handles' roles are filled later).
 * ERR_OUT_OF_RANGE if it doesn't fit. */
static status_t build_startup_msg(const struct spawn_args *a, uint8_t *msg, size_t *len)
{
    struct startup_msg *m = (struct startup_msg *)msg;
    memset(m, 0, sizeof(*m));
    m->magic = STARTUP_MAGIC;
    m->version = STARTUP_VERSION;
    m->argc = (uint32_t)a->argc;
    size_t n = sizeof(*m);
    for (int i = 0; i < a->argc; i++) {
        size_t l = strlen(a->argv[i]) + 1;
        if (n + l > MSG_MAX)
            return ERR_OUT_OF_RANGE;
        memcpy(msg + n, a->argv[i], l);
        n += l;
    }
    for (unsigned i = 0; a->envp && a->envp[i]; i++) {
        size_t l = strlen(a->envp[i]) + 1;
        if (n + l > MSG_MAX || m->argc + m->envc >= 128)
            return ERR_OUT_OF_RANGE;
        memcpy(msg + n, a->envp[i], l);
        n += l;
        m->envc++;
    }
    m->strings_len = (uint32_t)(n - sizeof(*m));
    *len = n;
    return OK;
}

/* The child while spawn() makes it: every handle here is ours to close
 * (HANDLE_INVALID: none, or handed over). */
struct child {
    handle_t proc, vmar, thread;   /* from process_create and thread_create */
    handle_t ch[2];                /* the startup channel: ours, the child's */
    handle_t ns[2];                /* its SR_NS channel: ours (we write), the child's */
    handle_t ns_back;              /* a read-only duplicate of ns[1], for ns_update */
    uint64_t entry, stack;         /* where its thread starts; its stack top */
};

/* The handles for the startup message, in its order. */
struct handout {
    handle_t h[STARTUP_MAX_HANDLES];        /* ours to close until the message is written */
    rights_t rights[STARTUP_MAX_HANDLES];   /* what the child's copy of h[i] gets */
    unsigned n;                             /* entries */
};

/* A VMO's range as a program: some bytes, at most FILE_MAX, from a page
 * boundary. */
static status_t check_image(const struct image *img)
{
    if (!img->size || img->size > FILE_MAX || (img->off & (PAGE_SIZE - 1)))
        return ERR_INVALID_ARGS;
    return OK;
}

/* A caller's VMO range: read with vmo_read where the VMO allows it. A
 * physical VMO (the boot image's own, SR_BOOTFS, named by range) refuses
 * vmo_read (ERR_NOT_SUPPORTED); it has no pager and no page that can fail
 * a check, so it is mapped here read-only, as the boot image always is. */
static status_t caller_image(struct image *img)
{
    status_t st = check_image(img);
    uint8_t probe;
    if (st == OK)
        st = jam_vmo_read(img->vmo, img->off, &probe, 1);
    if (st != ERR_NOT_SUPPORTED)
        return st;
    uint64_t addr = 0, len = up(img->size);
    st = jam_vmar_map(startup_handle(SR_SELF_VMAR), img->vmo, img->off, len, VMAR_READ, &addr);
    if (st != OK)
        return st;
    img->data = (const uint8_t *)(uintptr_t)addr;
    img->mapped = len;
    return OK;
}

/* The program a names: the caller's VMO range, a file in the namespace
 * (an absolute path) or a bootfs file. release_image after any outcome. */
static status_t find_image(const struct spawn_args *a, struct image *img)
{
    memset(img, 0, sizeof(*img));
    if (a->vmo) {
        img->vmo = a->vmo;
        img->off = a->offset;
        img->size = a->size;
        return caller_image(img);
    }
    if (a->path[0] == '/') {
        status_t st = file_read_vmo(a->path, FILE_MAX, &img->vmo, &img->size);
        if (st != OK)
            return st;
        img->own_vmo = true;
        return check_image(img);
    }
    const struct bootfs_view *fs;
    const void *data;
    status_t st = bootfs_default(&fs);
    if (st == OK)
        st = bootfs_lookup(fs, a->path, &data, &img->size);
    if (st != OK)
        return st;
    img->vmo = startup_handle(SR_BOOTFS);
    img->data = data;
    img->off = (uint64_t)((const uint8_t *)data - fs->base);
    return OK;
}

/* The child keeps what it mapped of the VMO; our view of a physical
 * one and a copy made here go. */
static void release_image(struct image *img)
{
    if (img->mapped)
        jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)img->data, img->mapped);
    if (img->own_vmo && img->vmo)
        jam_handle_close(img->vmo);
}

/* The stack's size spawn_args asks for, in whole pages; 0 if too big. */
static uint64_t stack_bytes(const struct spawn_args *a)
{
    if (!a->stack_size)
        return SPAWN_STACK_DEFAULT;
    return a->stack_size > SPAWN_STACK_MAX ? 0 : up(a->stack_size);
}

/* The program into c's address space, its stack and its thread. */
static status_t load_child(const struct spawn_args *a, const struct image *img, const char *name,
                           struct child *c)
{
    status_t st = load_elf(c->vmar, img, &c->entry);
    if (st == OK)
        st = make_stack(c->vmar, stack_bytes(a), &c->stack);
    if (st == OK)
        st = jam_thread_create(c->proc, name, name_len(name), 0, &c->thread);
    return st;
}

/* The rights the child's SR_SELF_IMAGE gets: IMAGE_RIGHTS as far as our
 * handle of the program's VMO has them; 0 for none. Only a caller's VMO
 * holding the program from its start, and executable: the VMO the child
 * runs from, never another (a boot-image program is SR_BOOTFS's, found
 * by its name; a file read here can't run). */
static rights_t image_rights(const struct spawn_args *a, const struct image *img)
{
    struct handle_info info;
    if (!a->vmo || a->offset || img->data || jam_handle_info(a->vmo, &info) != OK ||
        !(info.rights & RIGHT_EXEC))
        return 0;
    return info.rights & IMAGE_RIGHTS;
}

/* The handles, with their roles into m: duplicates of ours, the vmar
 * itself (we don't keep it), its end of the namespace channel, its own
 * program (image: the rights, 0 for none), then the extras. st: whether
 * the steps so far worked; after a failure only the extras are gathered
 * (to be closed). Returns st, or the failure of a duplicate. */
static status_t gather_handles(const struct spawn_args *a, struct child *c, rights_t image,
                               struct startup_msg *m, struct handout *out, status_t st)
{
    /* The child's own job goes without RIGHT_MANAGE (JOB_RIGHTS_OWN): the
     * limits we set on it are ours to change, not the child's. */
    const struct { uint32_t role; handle_t h; rights_t rights; } dup[] = {
        { SR_SELF_PROCESS, c->proc, RIGHT_SAME }, { SR_SELF_THREAD, c->thread, RIGHT_SAME },
        { SR_JOB, a->job, JOB_RIGHTS_OWN }, { SR_BOOTFS, startup_handle(SR_BOOTFS), RIGHT_SAME },
    };
    for (unsigned i = 0; st == OK && i < sizeof(dup) / sizeof(dup[0]); i++) {
        st = jam_handle_duplicate(dup[i].h, dup[i].rights, &out->h[out->n]);
        if (st == OK) {
            out->rights[out->n] = RIGHT_SAME;
            m->roles[out->n++] = dup[i].role;
        }
    }
    if (st == OK) {
        out->h[out->n] = c->vmar;
        out->rights[out->n] = RIGHT_SAME;
        m->roles[out->n++] = SR_SELF_VMAR;
        c->vmar = HANDLE_INVALID;
    }
    if (st == OK && c->ns[1]) {
        out->h[out->n] = c->ns[1];
        out->rights[out->n] = NS_RIGHTS;
        m->roles[out->n++] = SR_NS;
        c->ns[1] = HANDLE_INVALID;
    }
    if (st == OK && image) {
        st = jam_handle_duplicate(a->vmo, image, &out->h[out->n]);
        if (st == OK) {
            out->rights[out->n] = RIGHT_SAME;
            m->roles[out->n++] = SR_SELF_IMAGE;
        }
    }
    for (unsigned i = 0; i < a->nextra; i++) {
        out->h[out->n] = a->extra[i].h;
        out->rights[out->n] = a->extra_rights && a->extra_rights[i] ? a->extra_rights[i]
                                                                   : RIGHT_SAME;
        m->roles[out->n++] = a->extra[i].role;
    }
    m->nhandles = out->n;
    return st;
}

/* The hand-over on ch, naming the program (its /proc/self/exe) if the
 * caller didn't: a's absolute path, or a boot-image program's as
 * /boot/<name>; none for a caller's VMO with a relative name. */
static status_t write_handover(handle_t ch, const struct spawn_args *a)
{
    struct posixfds_out ho = *a->handover;
    char boot[sizeof(BOOT_PREFIX) + BOOTFS_NAME_MAX];
    if (!ho.exe && a->path[0] == '/') {
        ho.exe = a->path;
    } else if (!ho.exe && !a->vmo && strlen(a->path) < BOOTFS_NAME_MAX) {
        memcpy(boot, BOOT_PREFIX, sizeof(BOOT_PREFIX) - 1);
        memcpy(boot + sizeof(BOOT_PREFIX) - 1, a->path, strlen(a->path) + 1);
        ho.exe = boot;
    }
    return posixfds_write(ch, &ho);   /* all of it before the child runs */
}

/* The message (len bytes of msg, the handles in hs) on a new channel,
 * then the hand-over (consumed whatever happens) and what a's
 * before_start writes, then the thread starts with the other end. A write
 * or a start takes the handles it was given whatever its status (sent, or
 * closed). */
static status_t start_child(const struct spawn_args *a, struct child *c, const uint8_t *msg,
                            size_t len, struct handout *hs)
{
    const struct posixfds_out *handover = a->handover;
    status_t st = jam_channel_create(&c->ch[0], &c->ch[1]);
    if (st == OK) {   /* the extras with the rights the caller chose */
        st = jam_channel_write_rights(c->ch[0], msg, (uint32_t)len, hs->h, hs->rights, hs->n);
        hs->n = 0;   /* in the message now, or closed */
    }
    if (handover && st == OK)
        st = write_handover(c->ch[0], a);
    else if (handover)
        posixfds_discard(handover);
    if (st == OK && a->before_start)
        st = a->before_start(c->ch[0], a->before_start_ctx);
    if (st == OK) {
        st = jam_process_start(c->proc, c->thread, c->entry, c->stack, c->ch[1], 0);
        c->ch[1] = HANDLE_INVALID;   /* the child's now, or closed */
    }
    return st;
}

/* From the new process c->proc to its running thread, then its first ns
 * message (nso, made ahead: consumed). The extras are consumed. */
static status_t make_child(const struct spawn_args *a, const struct image *img, const char *name,
                           uint8_t *msg, size_t len, struct child *c, struct ns_out *nso)
{
    status_t st = load_child(a, img, name, c);
    struct handout hs = { .n = 0 };
    /* Its own program, where the message has room for it after ours (5,
     * the namespace's) and the extras. */
    rights_t image = image_rights(a, img);
    if (a->nextra + 6u + (a->ns ? 1u : 0u) > STARTUP_MAX_HANDLES)
        image = 0;
    st = gather_handles(a, c, image, (struct startup_msg *)msg, &hs, st);
    if (st == OK)
        st = start_child(a, c, msg, len, &hs);
    else
        posixfds_discard(a->handover);
    /* A message that never reached the child dies with ch[1], handles and
     * all. */
    for (unsigned i = 0; i < hs.n; i++)
        jam_handle_close(hs.h[i]);
    if (st == OK && nso) {
        /* Its end holds no message until now: a channel end with channels
         * queued on it can't be sent in a message. */
        st = ns_send_prepared(c->ns[0], NS_MOUNT, nso);
        if (st == ERR_PEER_CLOSED)
            st = OK;   /* it has ended already, without looking */
        else if (st != OK)
            jam_process_kill(c->proc);   /* it would run without its mounts */
    } else if (nso) {
        ns_prepared_drop(nso);
    }
    return st;
}

/* Close what is still ours of c, except the process. */
static void close_child(struct child *c)
{
    handle_t *hs[] = { &c->ch[0], &c->ch[1], &c->ns[0], &c->ns[1], &c->ns_back, &c->thread,
                       &c->vmar };
    for (unsigned i = 0; i < sizeof(hs) / sizeof(hs[0]); i++)
        if (*hs[i]) {
            jam_handle_close(*hs[i]);
            *hs[i] = HANDLE_INVALID;
        }
}

status_t spawn(const struct spawn_args *a, handle_t *proc_out)
{
    struct image img;
    status_t st = find_image(a, &img);
    if (st == OK && (a->nextra > STARTUP_MAX_HANDLES - 5u - (a->ns ? 1u : 0u) ||
                     !stack_bytes(a)))
        st = ERR_OUT_OF_RANGE;
    const char *name = child_name(a);
    _Alignas(8) uint8_t msg[MSG_MAX];   /* the startup message, built on the stack */
    size_t len = 0;
    if (st == OK)
        st = build_startup_msg(a, msg, &len);
    struct child c = { 0 };
    struct ns_out *nso = NULL;   /* its first ns message, views made before it starts */
    if (st == OK && a->ns)
        st = ns_prepare(a->ns, &nso);
    if (st == OK && a->ns)
        st = jam_channel_create(&c.ns[0], &c.ns[1]);
    if (st == OK && a->ns && a->ns_out && a->ns_back_out)
        st = jam_handle_duplicate(c.ns[1], RIGHT_READ, &c.ns_back);
    if (st == OK)
        st = jam_process_create(a->job, name, name_len(name), 0, &c.proc, &c.vmar);
    if (st != OK) {
        close_extras(a);
        close_child(&c);
        release_image(&img);
        if (nso)
            ns_prepared_drop(nso);
        return st;
    }
    st = make_child(a, &img, name, msg, len, &c, nso);
    release_image(&img);
    if (st == OK && a->ns_out) {
        *a->ns_out = c.ns[0];
        c.ns[0] = HANDLE_INVALID;
    }
    if (st == OK && c.ns_back) {
        *a->ns_back_out = c.ns_back;
        c.ns_back = HANDLE_INVALID;
    }
    close_child(&c);
    if (st != OK) {
        jam_handle_close(c.proc);   /* a process that never started dies with its last handle */
        return st;
    }
    *proc_out = c.proc;
    return OK;
}

status_t spawn_wait(handle_t proc, uint64_t timeout_ns, struct process_info *info)
{
    signals_t seen;
    status_t st = jam_object_wait_one(proc, SIG_TERMINATED,
                                      now() + timeout_ns, &seen);
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
    status_t st = jam_thread_create(self, name, name_len(name), 0, &t);
    if (st != OK)
        return st;
    uint64_t top = ((uint64_t)(uintptr_t)stack + stack_size) & ~15ull;
    /* No thread-local storage in libos: FS base 0, as the first thread's. */
    st = jam_thread_start(t, (uint64_t)(uintptr_t)thread_entry, top - 8,
                          (uint64_t)(uintptr_t)fn, (uint64_t)(uintptr_t)arg, 0);
    if (st != OK) {
        jam_handle_close(t);
        return st;
    }
    *out = t;
    return OK;
}
