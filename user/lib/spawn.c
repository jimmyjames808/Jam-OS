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
 * it, for a POSIX program, the hand-over messages: <posixfds.h>). argv
 * and an environment too big for the message go in a VMO of their own
 * (SR_ARGS, <roles.h>), up to Linux's 2 MiB, which the child copies.
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
#define MSG_MAX     2048             /* a startup message's bytes; more go in SR_ARGS's VMO */
#define MSG_STRINGS 128              /* its strings (libos's start takes no more) */
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

/* argv and the environment: how many strings (*n, *envc of them the
 * environment's) and their bytes with their NULs. */
static void count_strings(const struct spawn_args *a, uint64_t *n, uint32_t *envc,
                          uint64_t *bytes)
{
    *bytes = 0;
    *envc = 0;
    for (int i = 0; i < a->argc; i++)
        *bytes += strlen(a->argv[i]) + 1;
    for (; a->envp && a->envp[*envc] && *envc < STARTUP_ARGS_MAX; (*envc)++)
        *bytes += strlen(a->envp[*envc]) + 1;
    *n = (uint64_t)(a->argc > 0 ? a->argc : 0) + *envc;
}

/* Every string of argv and then of the environment (envc of them) to p. */
static void copy_strings(const struct spawn_args *a, uint32_t envc, uint8_t *p)
{
    for (int i = 0; i < a->argc; i++) {
        size_t l = strlen(a->argv[i]) + 1;
        memcpy(p, a->argv[i], l);
        p += l;
    }
    for (uint32_t i = 0; i < envc; i++) {
        size_t l = strlen(a->envp[i]) + 1;
        memcpy(p, a->envp[i], l);
        p += l;
    }
}

/* The startup message's header into msg, and argv and the environment
 * (envc strings, bytes long) after it unless they go in a VMO (in_vmo);
 * *len: its length so far (the handles' roles are filled later). The
 * caller checked that they fit MSG_MAX. */
static void build_startup_msg(const struct spawn_args *a, bool in_vmo, uint32_t envc,
                              uint64_t bytes, uint8_t *msg, size_t *len)
{
    struct startup_msg *m = (struct startup_msg *)msg;
    memset(m, 0, sizeof(*m));
    m->magic = STARTUP_MAGIC;
    m->version = STARTUP_VERSION;
    if (!in_vmo) {
        m->argc = (uint32_t)a->argc;
        m->envc = envc;
        m->strings_len = (uint32_t)bytes;
        copy_strings(a, envc, msg + sizeof(*m));
    }
    *len = sizeof(*m) + m->strings_len;
}

/* SR_ARGS's VMO (<roles.h>): argv and the environment (envc strings,
 * bytes long) and the program's path exe (NULL: none), written through a
 * mapping of it here. *out: the VMO. */
static status_t make_args(const struct spawn_args *a, uint32_t envc, uint64_t bytes,
                          const char *exe, handle_t *out)
{
    struct startup_args hd = {
        .magic = STARTUP_ARGS_MAGIC, .version = STARTUP_ARGS_VERSION,
        .argc = (uint32_t)a->argc, .envc = envc, .strings_len = (uint32_t)bytes,
        .exe_len = exe ? (uint32_t)strlen(exe) + 1 : 0,
    };
    handle_t self = startup_handle(SR_SELF_VMAR), v = HANDLE_INVALID;
    uint64_t size = up(sizeof(hd) + bytes + hd.exe_len), addr = 0;
    status_t st = jam_vmo_create(size, 0, &v);
    if (st == OK)
        st = jam_vmar_map(self, v, 0, size, VMAR_READ | VMAR_WRITE, &addr);
    if (st != OK) {
        if (v)
            jam_handle_close(v);
        return st;
    }
    uint8_t *p = (uint8_t *)(uintptr_t)addr;
    memcpy(p, &hd, sizeof(hd));
    copy_strings(a, envc, p + sizeof(hd));
    if (exe)
        memcpy(p + sizeof(hd) + bytes, exe, hd.exe_len);
    jam_vmar_unmap(self, addr, size);
    *out = v;
    return OK;
}

/* The child while spawn() makes it: every handle here is ours to close
 * (HANDLE_INVALID: none, or handed over). */
struct child {
    handle_t proc, vmar, thread;   /* from process_create and thread_create */
    handle_t ch[2];                /* the startup channel: ours, the child's */
    handle_t ns[2];                /* its SR_NS channel: ours (we write), the child's */
    handle_t ns_back;              /* a read-only duplicate of ns[1], for ns_update */
    handle_t args;                 /* its SR_ARGS VMO, or none */
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
 * itself (we don't keep it), its end of the namespace channel, its
 * strings' VMO (read only: it copies them), its own program (image: the
 * rights, 0 for none), then the extras. st: whether
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
    if (st == OK && c->args) {
        out->h[out->n] = c->args;
        out->rights[out->n] = RIGHT_READ;
        m->roles[out->n++] = SR_ARGS;
        c->args = HANDLE_INVALID;
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
 * message (nso, made ahead: consumed). The extras are consumed. image:
 * its SR_SELF_IMAGE's rights (0: none). */
static status_t make_child(const struct spawn_args *a, const struct image *img, const char *name,
                           rights_t image, uint8_t *msg, size_t len, struct child *c,
                           struct ns_out *nso)
{
    status_t st = load_child(a, img, name, c);
    struct handout hs = { .n = 0 };
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
    handle_t *hs[] = { &c->ch[0], &c->ch[1], &c->ns[0], &c->ns[1], &c->ns_back, &c->args,
                       &c->thread, &c->vmar };
    for (unsigned i = 0; i < sizeof(hs) / sizeof(hs[0]); i++)
        if (*hs[i]) {
            jam_handle_close(*hs[i]);
            *hs[i] = HANDLE_INVALID;
        }
}

/* Where argv and the environment go, and what else the startup message
 * holds beside our 5 handles, the namespace's and the extras. */
struct layout {
    uint64_t    bytes;    /* the strings' */
    uint32_t    envc;     /* the environment's strings */
    bool        in_vmo;   /* in SR_ARGS's VMO, not the message */
    rights_t    image;    /* SR_SELF_IMAGE's rights; 0: none */
    const char *exe;      /* the path SR_ARGS names (a program with an image and no
                           * hand-over, which would name it, nor a before_start,
                           * which passes one on); NULL: none */
};

/* The layout for a and its program img: ERR_OUT_OF_RANGE if the strings
 * are past STARTUP_ARGS_MAX or the handles don't fit. The strings are in
 * the message when they fit it; else SR_ARGS takes a place. The image
 * comes next, where there is room, then SR_ARGS for the path alone. */
static status_t plan_layout(const struct spawn_args *a, const struct image *img,
                            struct layout *l)
{
    uint64_t n;
    count_strings(a, &n, &l->envc, &l->bytes);
    l->in_vmo = sizeof(struct startup_msg) + l->bytes > MSG_MAX || n > MSG_STRINGS;
    unsigned room = STARTUP_MAX_HANDLES - 5u - (a->ns ? 1u : 0u), need = a->nextra + l->in_vmo;
    if (n > STARTUP_ARGS_MAX / 8 || l->bytes + (n + 2) * 8 > STARTUP_ARGS_MAX || need > room)
        return ERR_OUT_OF_RANGE;
    room -= need;
    l->image = room ? image_rights(a, img) : 0;
    room -= l->image ? 1 : 0;
    l->exe = NULL;
    if (l->image && !a->handover && !a->before_start && a->path[0] == '/' &&
        strlen(a->path) < STARTUP_ARGS_EXE_MAX && (l->in_vmo || room)) {
        l->exe = a->path;
        l->in_vmo = true;
    }
    return OK;
}

status_t spawn(const struct spawn_args *a, handle_t *proc_out)
{
    struct image img;
    struct layout lay = { 0 };
    status_t st = find_image(a, &img);
    if (st == OK)
        st = stack_bytes(a) ? plan_layout(a, &img, &lay) : ERR_OUT_OF_RANGE;
    const char *name = child_name(a);
    _Alignas(8) uint8_t msg[MSG_MAX];   /* the startup message, built on the stack */
    size_t len = 0;
    struct child c = { 0 };
    if (st == OK)
        build_startup_msg(a, lay.in_vmo, lay.envc, lay.bytes, msg, &len);
    if (st == OK && lay.in_vmo)
        st = make_args(a, lay.envc, lay.bytes, lay.exe, &c.args);
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
    st = make_child(a, &img, name, lay.image, msg, len, &c, nso);
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
