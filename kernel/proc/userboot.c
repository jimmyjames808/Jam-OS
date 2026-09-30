/* userboot: start a user program from bootfs (see userboot.h).
 *
 * The ELF is checked by elf_parse (every field treated as hostile) and then
 * applied segment by segment:
 *   - read-only and executable segments whose memory size equals their
 *     file size map the bootfs pages themselves (a physical VMO over the
 *     file, VMO offset = file offset rounded down: the parser guarantees
 *     address and offset are congruent mod 4 KiB). The mapping is never
 *     writable and can't become so (no ASPACE_CAN_WRITE), so the unprotected
 *     bootfs pages stay intact;
 *   - anything writable, or with a zero-filled tail (bss), gets a fresh VMO
 *     charged to the process's job, with the file bytes copied in. The page
 *     after .data's file bytes holds other bytes of the file, so it must
 *     never be mapped from bootfs.
 * The stack is a VMO of USERBOOT_STACK_PAGES pages plus one more mapped
 * with no access at all below it: a guard page, so running off the bottom
 * is a fault (and a kill), not a silent overwrite.
 *
 * The startup message (<jam/startup.h>) goes on a fresh channel; the
 * process gets the other end in rdi. The kernel's end is dropped at once:
 * a queued message stays readable after its sender is gone. */
#include <jam/aspace.h>
#include <jam/bootfs.h>
#include <jam/channel.h>
#include <jam/elf.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/report.h>
#include <jam/resource.h>
#include <jam/resource_impl.h>
#include <jam/startup.h>
#include <jam/string.h>
#include <jam/time.h>
#include <jam/userboot.h>
#include <jam/vmar.h>
#include <jam/vmo.h>

#define STARTUP_MSG_MAX 8192
#define VMAR_HANDLE_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE)
/* bootfs: map it (read, execute), never write it. */
#define BOOTFS_RIGHTS (RIGHTS_BASIC | RIGHT_READ | RIGHT_MAP | RIGHT_EXEC)

static status_t map_segment(struct aspace *as, struct job *job, struct vmo *file,
                            const uint8_t *img, const struct elf_segment *s)
{
    uint64_t lo = ALIGN_DOWN(s->vaddr, PAGE_SIZE);
    uint64_t hi = ALIGN_UP(s->vaddr + s->memsz, PAGE_SIZE);
    uint64_t addr = lo;
    unsigned perms = s->flags & (ASPACE_READ | ASPACE_WRITE | ASPACE_EXEC);
    if (!(perms & ASPACE_WRITE) && s->memsz == s->filesz) {
        unsigned can = ASPACE_CAN_READ | ((perms & ASPACE_EXEC) ? ASPACE_CAN_EXEC : 0);
        return aspace_map(as, file, ALIGN_DOWN(s->file_off, PAGE_SIZE), hi - lo,
                          perms | can | ASPACE_FIXED, &addr);
    }
    struct vmo *v;
    status_t st = vmo_create(hi - lo, 0, &v);
    if (st != OK)
        return st;
    st = vmo_set_job(v, job);
    if (st == OK && s->filesz)
        st = vmo_write(v, s->vaddr - lo, img + s->file_off, s->filesz);
    if (st == OK) {
        unsigned can = ASPACE_CAN_READ | ((perms & ASPACE_WRITE) ? ASPACE_CAN_WRITE : 0);
        st = aspace_map(as, v, 0, hi - lo, perms | can | ASPACE_FIXED, &addr);
    }
    kobject_unref(vmo_kobject(v));   /* the mapping holds its own reference */
    return st;
}

static status_t map_stack(struct aspace *as, struct job *job, uint64_t *top)
{
    uint64_t pages = USERBOOT_STACK_PAGES + 1;   /* + the guard */
    uint64_t base = USERBOOT_STACK_TOP - pages * PAGE_SIZE;
    struct vmo *v;
    status_t st = vmo_create(pages * PAGE_SIZE, 0, &v);
    if (st != OK)
        return st;
    st = vmo_set_job(v, job);
    uint64_t a = base;
    if (st == OK)   /* the guard: mapped, but with no access at all */
        st = aspace_map(as, v, 0, PAGE_SIZE, ASPACE_FIXED, &a);
    a = base + PAGE_SIZE;
    if (st == OK)
        st = aspace_map(as, v, PAGE_SIZE, USERBOOT_STACK_PAGES * PAGE_SIZE,
                        ASPACE_READ | ASPACE_WRITE | ASPACE_CAN_READ | ASPACE_CAN_WRITE |
                            ASPACE_FIXED,
                        &a);
    kobject_unref(vmo_kobject(v));
    *top = USERBOOT_STACK_TOP;
    return st;
}

/* A khandle with a new reference on obj. */
static struct khandle kh_ref(struct kobject *obj, rights_t rights)
{
    kobject_ref(obj);
    return khandle_from_new(obj, rights);
}

static void release_all(struct khandle *khs, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        khandle_release(&khs[i]);
}

static void release_extra(struct userboot_handle *extra, unsigned nextra)
{
    for (unsigned i = 0; i < nextra; i++)
        khandle_release(&extra[i].kh);
}

/* The handles a program's startup message carries, with their roles. */
struct startup_handles {
    uint32_t       roles[STARTUP_MAX_HANDLES];   /* enum startup_role of each */
    struct khandle khs[STARTUP_MAX_HANDLES];     /* send_startup consumes them */
    unsigned       n;                            /* entries in use */
};

/* Build the startup message and put it on a new channel; *child gets the
 * process's end. The khandles are consumed either way. */
static status_t send_startup(const char *const *argv, unsigned argc, struct startup_handles *h,
                             struct khandle *child)
{
    uint64_t strings = 0;
    for (unsigned i = 0; i < argc; i++)
        strings += strlen(argv[i]) + 1;
    uint64_t size = sizeof(struct startup_msg) + strings;
    if (h->n > STARTUP_MAX_HANDLES || size > STARTUP_MSG_MAX) {
        release_all(h->khs, h->n);
        return ERR_OUT_OF_RANGE;
    }
    uint8_t *buf = kzalloc(size);
    if (!buf) {
        release_all(h->khs, h->n);
        return ERR_NO_MEMORY;
    }
    struct startup_msg *m = (struct startup_msg *)buf;
    m->magic = STARTUP_MAGIC;
    m->version = STARTUP_VERSION;
    m->argc = argc;
    m->envc = 0;
    m->nhandles = h->n;
    for (unsigned i = 0; i < h->n; i++)
        m->roles[i] = h->roles[i];
    m->strings_len = (uint32_t)strings;
    char *s = (char *)(m + 1);
    for (unsigned i = 0; i < argc; i++) {
        size_t l = strlen(argv[i]) + 1;
        memcpy(s, argv[i], l);
        s += l;
    }

    struct channel *mine, *theirs;
    status_t st = channel_create(&mine, &theirs);
    if (st == OK) {
        st = channel_write(mine, buf, (uint32_t)size, h->khs, h->n);
        kobject_unref((struct kobject *)mine);   /* the queued message outlives it */
        if (st == OK)
            *child = khandle_from_new((struct kobject *)theirs, RIGHTS_BASIC | RIGHTS_IO);
        else
            kobject_unref((struct kobject *)theirs);
    }
    kfree(buf);
    release_all(h->khs, h->n);   /* no-ops for the ones the message took */
    return st;
}

/* The process name: argv[0] (or the path) without its directories. */
static const char *program_name(const char *path, const char *const *argv, unsigned argc)
{
    const char *name = argc ? argv[0] : path;
    for (const char *c = name; *c; c++)
        if (*c == '/')
            name = c + 1;   /* "bin/utest" -> "utest" */
    return name;
}

/* The program's own handles: its process, address space, first thread, job
 * and bootfs. ERR_NO_MEMORY if the address space's handle can't be made;
 * the rest are added anyway (the caller releases whatever h holds). */
static status_t add_own_handles(struct startup_handles *h, struct process *p, struct aspace *as,
                                struct uthread *u, struct job *job)
{
    status_t st = OK;
    struct vmar *vm;
    struct vmo *image;
    uint64_t isz;
    h->roles[h->n] = SR_SELF_PROCESS;
    h->khs[h->n++] = kh_ref(process_kobject(p), PROCESS_RIGHTS);
    if (vmar_create_for(as, &vm) == OK) {
        h->roles[h->n] = SR_SELF_VMAR;
        h->khs[h->n++] = khandle_from_new(vmar_kobject(vm), VMAR_HANDLE_RIGHTS);
    } else {
        st = ERR_NO_MEMORY;
    }
    h->roles[h->n] = SR_SELF_THREAD;
    h->khs[h->n++] = kh_ref(uthread_kobject(u), THREAD_RIGHTS);
    h->roles[h->n] = SR_JOB;
    h->khs[h->n++] = kh_ref(job_kobject(job), JOB_RIGHTS_OWN);   /* no RIGHT_MANAGE */
    if (bootfs_image(&image, &isz) == OK) {
        h->roles[h->n] = SR_BOOTFS;
        h->khs[h->n++] = khandle_from_new(vmo_kobject(image), BOOTFS_RIGHTS);
    }
    return st;
}

/* Move the caller's extra handles into h while st is OK and they fit;
 * the rest are released (ERR_OUT_OF_RANGE if one didn't fit). */
static status_t add_extra_handles(struct startup_handles *h, struct userboot_handle *extra,
                                  unsigned nextra, status_t st)
{
    for (unsigned i = 0; i < nextra; i++) {
        if (st == OK && h->n < STARTUP_MAX_HANDLES) {
            h->roles[h->n] = extra[i].role;
            h->khs[h->n++] = extra[i].kh;
            extra[i].kh.obj = NULL;
        } else {
            khandle_release(&extra[i].kh);
            if (st == OK)
                st = ERR_OUT_OF_RANGE;
        }
    }
    return st;
}

status_t userboot_spawn(const char *path, const char *const *argv, unsigned argc,
                        struct job *job, struct userboot_handle *extra, unsigned nextra,
                        const cpumask_t *mask, struct process **out)
{
    const void *img;
    uint64_t size;
    struct elf_plan plan;
    struct vmo *file = NULL;
    uint64_t fsize;
    status_t st = bootfs_data(path, &img, &size);
    if (st == OK)
        st = elf_parse(img, size, &plan);
    if (st == OK)
        st = bootfs_find(path, &file, &fsize);
    if (st != OK) {
        release_extra(extra, nextra);
        return st;
    }

    struct process *p;
    st = process_create(job, program_name(path, argv, argc), &p);
    if (st != OK) {
        kobject_unref(vmo_kobject(file));
        release_extra(extra, nextra);
        return st;
    }
    struct aspace *as = process_aspace(p);
    if (!as)
        st = ERR_BAD_STATE;   /* its job was killed already */
    for (unsigned i = 0; st == OK && i < plan.nseg; i++)
        st = map_segment(as, job, file, img, &plan.seg[i]);
    kobject_unref(vmo_kobject(file));
    uint64_t stack_top = 0;
    if (st == OK)
        st = map_stack(as, job, &stack_top);

    struct uthread *u = NULL;
    if (st == OK)
        st = uthread_create(p, process_name(p), &u);

    struct startup_handles h;
    h.n = 0;
    if (st == OK)
        st = add_own_handles(&h, p, as, u, job);
    st = add_extra_handles(&h, extra, nextra, st);
    struct khandle child = { NULL, 0 };
    if (st == OK)
        st = send_startup(argv, argc, &h, &child);
    else
        release_all(h.khs, h.n);
    if (st == OK)
        st = process_start(p, u, plan.entry, stack_top, &child, 0, mask);
    khandle_release(&child);   /* a no-op once the process has it */

    if (u)
        kobject_unref(uthread_kobject(u));
    if (as)
        aspace_unref(as);
    if (st != OK) {
        process_kill(p, PROCESS_KILLED_CODE, true);   /* never started: torn down here */
        kobject_unref(process_kobject(p));
        return st;
    }
    *out = p;
    return OK;
}

status_t userboot_root_job(struct job **out)
{
    struct job *j;
    status_t st = job_create(NULL, &j);
    if (st != OK)
        return st;
    /* Leave the kernel 32 MiB (or a quarter, if memory is small) that user
     * code can never take, so running out is a job's problem first. The
     * rest is split so the three memory-like limits can't add up to more
     * than it (kernel objects and queued messages are memory too): a
     * handle unit stands for at most
     * JOB_OBJECT_BYTES of kernel memory and a message byte for one byte, so
     * those budgets (1/16 and 1/8 of it, capped at 16 MiB and 64 MiB) come
     * off the page limit. Threads are charged pages for their stacks, so
     * their limit is only a sanity cap. */
    uint64_t total, free;
    pmm_stats(&total, &free);
    uint64_t keep = free / 4 < 8192 ? free / 4 : 8192;
    uint64_t budget = free - keep;
    uint64_t handle_pages = budget / 16 < 4096 ? budget / 16 : 4096;
    uint64_t msg_pages = budget / 8 < 16384 ? budget / 8 : 16384;
    job_set_limit(j, JOB_LIMIT_PAGES, budget - handle_pages - msg_pages);
    job_set_limit(j, JOB_LIMIT_HANDLES, handle_pages * (PAGE_SIZE / JOB_OBJECT_BYTES));
    job_set_limit(j, JOB_LIMIT_THREADS, 4096);
    job_set_limit(j, JOB_LIMIT_MSG_BYTES, msg_pages * PAGE_SIZE);
    *out = j;
    return OK;
}

bool userboot_run_init(uint64_t timeout_s, const char *arg)
{
    const void *img;
    uint64_t size;
    if (bootfs_data("bin/init", &img, &size) != OK) {
        report("init: no bin/init in bootfs");
        return false;
    }
    struct job *root;
    if (userboot_root_job(&root) != OK) {
        report("init: no memory for the root job");
        return false;
    }
    const char *const argv[] = { "init", arg };
    /* init holds the root of hardware authority (SR_RESOURCE) and slices
     * it for devmgr. */
    struct userboot_handle extra[1];
    unsigned nextra = 0;
    struct kobject *res = resource_root();
    if (res) {
        extra[0].role = SR_RESOURCE;
        extra[0].kh = khandle_from_new(res, RES_RIGHTS);
        nextra = 1;
    }
    struct process *p;
    status_t st = userboot_spawn("bin/init", argv, arg ? 2 : 1, root, extra, nextra, NULL, &p);
    if (st != OK) {
        report("init: could not start bin/init (%s)", status_str(st));
        job_unref(root);
        return false;
    }
    uint64_t t0 = uptime_ns();
    st = object_wait_one(process_kobject(p), SIG_TERMINATED,
                         timeout_s ? t0 + timeout_s * 1000000000ull : DEADLINE_NEVER, NULL);
    bool ok = false;
    if (st != OK) {
        report("init: still running after %lu s: killed (with everything it started)",
               timeout_s);
        job_kill(root, NULL);   /* init's job is the root: every user process */
        object_wait_one(process_kobject(p), SIG_TERMINATED, DEADLINE_NEVER, NULL);
    } else {
        struct process_info info;
        process_get_info(p, &info);
        uint64_t ms = (uptime_ns() - t0) / 1000000;
        if (info.killed)
            report("init: was killed after %lu ms", ms);
        else
            report("init: exited with code %ld after %lu ms", info.exit_code, ms);
        ok = !info.killed && info.exit_code == 0;
    }
    kobject_unref(process_kobject(p));

    /* Everything init and its children had is gone now: its root job must
     * be back to zero on every count (a leak check for the whole run). */
    struct job_info ji;
    job_get_info(root, &ji);
    bool clean = true;
    for (unsigned k = 1; k < JOB_LIMIT_COUNT; k++)
        clean &= ji.used[k] == 0;
    report("init: root job afterwards: %lu pages, %lu handles, %lu threads, %lu message bytes%s",
           ji.used[JOB_LIMIT_PAGES], ji.used[JOB_LIMIT_HANDLES], ji.used[JOB_LIMIT_THREADS],
           ji.used[JOB_LIMIT_MSG_BYTES], clean ? " (clean)" : " (LEAKED)");
    job_unref(root);
    return ok && clean;
}
