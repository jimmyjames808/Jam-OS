/* fork: a copy of this process (<os.h> jam_fork; docs/FORK-PLAN.md
 * section 1). fork_entry.S saves the calling thread's registers in a block
 * on its stack and calls jam_fork_parent, which:
 *
 *   1. makes the child: its startup channel, its SR_NS channel, its
 *      signal channel if asked, the process in our job (SR_JOB) or the
 *      one the caller made for it (a->job), and its thread;
 *   2. writes the fork message (<forkmsg.h>) on the startup channel: the
 *      child's own process, address space and thread, and duplicates of
 *      ours by the list below;
 *   3. takes the C library's locks (its prepare), copies our address space
 *      into the child's at one instant (vmar_clone), lets the C library
 *      write its own messages (give) and gives its locks back (parent);
 *   4. snapshots our namespace (ns_prepare_in of NS_ALL: every entry
 *      duplicated, into our stack: nothing in the parent's fork allocates,
 *      as musl's fork holds its allocator's lock across the whole call),
 *      starts the child's thread at jam_fork_trampoline with
 *      the startup channel and the block's address, a little below the
 *      block on its copy of our stack, and sends the namespace on SR_NS.
 *
 * The child (jam_fork_child, on the trampoline) sets its FS base, reads the
 * fork message, makes its handles the startup roles in place of ours,
 * makes libjam's modules fresh (the namespace comes again on the new SR_NS;
 * svc_get's channels, fs_readdir's batch and the hand-over's buffer are
 * dropped), runs the C library's child hook, and the trampoline returns 0
 * with the block's registers.
 *
 * Locks: the C library's are held across the clone (its prepare and
 * parent, in its own order: the POSIX layer's descriptor table, signal and
 * children's locks and mem.c's; libos's heap), so its memory is whole in
 * the copy. libjam holds none of its own across it: every module of
 * libjam whose state a child can't use (its handle values are ours)
 * starts again empty in the child, so a lock another thread held there,
 * or a table it was changing, doesn't matter: ns.c's two, fs.c's and
 * fdmsg.c's are made fresh (fork.h). printf's SR_STDOUT (libos's) is looked
 * up again. Nothing libjam allocates is touched in the child (the C
 * library's allocator may still be locked there until its child hook).
 * Threads libjam started aren't in the child: a module that starts one
 * starts it again at its next use. */
#include <stddef.h>
#include <forkmsg.h>
#include "fork.h"
#include "ns.h"

#define CHILD_GAP  128                        /* bytes between the block and the child's stack */
#define NS_RIGHTS  (RIGHT_READ | RIGHT_WAIT)  /* the child's SR_NS end, as spawn gives it */
#define PASSABLE   (RIGHT_DUPLICATE | RIGHT_TRANSFER)

_Static_assert(offsetof(struct fork_block, rbx) == FORK_B_RBX &&
               offsetof(struct fork_block, rbp) == FORK_B_RBP &&
               offsetof(struct fork_block, r12) == FORK_B_R12 &&
               offsetof(struct fork_block, r13) == FORK_B_R13 &&
               offsetof(struct fork_block, r14) == FORK_B_R14 &&
               offsetof(struct fork_block, r15) == FORK_B_R15 &&
               offsetof(struct fork_block, rsp) == FORK_B_RSP &&
               offsetof(struct fork_block, rip) == FORK_B_RIP &&
               offsetof(struct fork_block, mxcsr) == FORK_B_MXCSR &&
               offsetof(struct fork_block, fpucw) == FORK_B_FPUCW &&
               offsetof(struct fork_block, fs_base) == FORK_B_FS_BASE &&
               offsetof(struct fork_block, ctx) == FORK_B_CTX &&
               sizeof(struct fork_block) == FORK_BLOCK, "fork_entry.S's block");

/* The roles a child gets a duplicate of ours for (section 1.6's table),
 * each only if our handle has RIGHT_DUPLICATE and RIGHT_TRANSFER (only
 * duplicates are passed, and a message can't carry one without
 * transfer). The child's own process, address space and thread, its
 * SR_NS and its SR_SIGNALS are made for it. Every other role gets
 * nothing: our root (SR_RESOURCE: it has no RIGHT_TRANSFER, and a
 * posix_spawn child gets no root either), every service's (keep, state,
 * standby, devmgr, audio, the crash log) and SR_USER's, which mean
 * something to our starter and us only. */
static const uint32_t dup_roles[] = {
    SR_JOB,          /* its job (job_of): the fork's charge is ours either way (section 1.9) */
    SR_STDOUT,
    SR_CONSOLE,
    SR_BOOTFS,
    SR_SELF_IMAGE,   /* its /proc/self/exe: the same pages, never writable */
    /* CJR's handle holding the JIT power alone (its option (b); FORK-PLAN
     * Q4 (A): every forked child keeps it) goes here once CJ2 gives it a
     * startup role. */
};

/* What jam_fork_parent makes for the child, ours until moved or closed. */
struct kid {
    handle_t proc, vmar, thread;   /* process_create's, thread_create's */
    handle_t ch[2];                /* its startup channel: ours, its */
    handle_t ns[2];                /* its SR_NS: ours (we write the namespace), its */
    handle_t sig[2];               /* its SR_SIGNALS: the client end (the caller's), its */
};

/* The fork message being built: its handles are its own from the moment
 * they are put in (closed if it is never written). */
struct out {
    struct fork_msg m;
    handle_t        h[FORK_MAX_HANDLES];
    rights_t        rights[FORK_MAX_HANDLES];
};

static void name_of(const struct jam_fork_args *a, char name[PROCESS_NAME_MAX + 1])
{
    struct process_info pi;
    const char *n = a->name;
    if (!n)
        n = jam_process_get_info(startup_handle(SR_SELF_PROCESS), &pi) == OK ? pi.name : "fork";
    size_t l = strnlen(n, PROCESS_NAME_MAX);
    memcpy(name, n, l);
    name[l] = '\0';
}

/* The job the child goes in: the one the caller made for it, or ours. */
static handle_t job_of(const struct jam_fork_args *a)
{
    return a->job ? a->job : startup_handle(SR_JOB);
}

static status_t make_kid(const struct jam_fork_args *a, struct kid *k)
{
    char name[PROCESS_NAME_MAX + 1];
    name_of(a, name);
    status_t st = jam_channel_create(&k->ch[0], &k->ch[1]);
    if (st == OK)
        st = jam_channel_create(&k->ns[0], &k->ns[1]);
    if (st == OK && a->signals)
        st = jam_channel_create(&k->sig[0], &k->sig[1]);
    if (st == OK)
        st = jam_process_create(job_of(a), name, strlen(name), 0, &k->proc, &k->vmar);
    if (st == OK)
        st = jam_thread_create(k->proc, name, strlen(name), 0, &k->thread);
    return st;
}

/* Close what is still ours of k, except the process. */
static void close_kid(struct kid *k)
{
    handle_t *hs[] = { &k->vmar, &k->thread, &k->ch[0], &k->ch[1], &k->ns[0], &k->ns[1],
                       &k->sig[0], &k->sig[1] };
    for (unsigned i = 0; i < sizeof(hs) / sizeof(hs[0]); i++)
        if (*hs[i]) {
            jam_handle_close(*hs[i]);
            *hs[i] = HANDLE_INVALID;
        }
}

static void put(struct out *o, uint32_t role, handle_t h, rights_t rights)
{
    uint32_t i = o->m.nhandles++;
    o->h[i] = h;
    o->rights[i] = rights;
    o->m.roles[i] = role;
}

/* *h (made for the child) into the message: the message's from here on. */
static void move(struct out *o, uint32_t role, handle_t *h, rights_t rights)
{
    if (*h)
        put(o, role, *h, rights);
    *h = HANDLE_INVALID;
}

/* A duplicate of h as the child's role, with rights (RIGHT_SAME: h's). */
static status_t dup(struct out *o, uint32_t role, handle_t h, rights_t rights)
{
    handle_t d;
    status_t st = jam_handle_duplicate(h, RIGHT_SAME, &d);
    if (st == OK)
        put(o, role, d, rights);
    return st;
}

/* The fork message on k's startup channel. */
static status_t write_roles(const struct jam_fork_args *a, struct kid *k)
{
    struct out o = { .m = { .magic = FORK_MAGIC, .version = FORK_VERSION } };
    status_t st = dup(&o, SR_SELF_PROCESS, k->proc, RIGHT_SAME);
    move(&o, SR_SELF_VMAR, &k->vmar, RIGHT_SAME);
    if (st == OK)
        st = dup(&o, SR_SELF_THREAD, k->thread, RIGHT_SAME);
    for (unsigned i = 0; st == OK && i < sizeof(dup_roles) / sizeof(dup_roles[0]); i++) {
        bool own_job = dup_roles[i] == SR_JOB && a->job;   /* made for it: no RIGHT_MANAGE */
        handle_t h = dup_roles[i] == SR_JOB ? job_of(a) : startup_handle(dup_roles[i]);
        struct handle_info hi;
        if (h && jam_handle_info(h, &hi) == OK && (hi.rights & PASSABLE) == PASSABLE)
            st = dup(&o, dup_roles[i], h, own_job ? JOB_RIGHTS_OWN : RIGHT_SAME);
    }
    /* Its SR_NS end goes while nothing is queued on it (a channel end with
     * channels queued can't be sent); the namespace follows the start. */
    move(&o, SR_NS, &k->ns[1], NS_RIGHTS);
    move(&o, SR_SIGNALS, &k->sig[1], RIGHT_SAME);
    if (st != OK) {
        for (uint32_t i = 0; i < o.m.nhandles; i++)
            jam_handle_close(o.h[i]);
        return st;
    }
    /* Its handles are gone whatever the status (a write's are always moved). */
    return jam_channel_write_rights(k->ch[0], &o.m, sizeof(o.m), o.h, o.rights, o.m.nhandles);
}

/* Step 3: the C library's locks around the clone, and its messages. */
static status_t clone(const struct jam_fork_args *a, const struct jam_fork_libc *libc,
                      struct kid *k)
{
    for (uint32_t i = 0; i < a->n; i++)
        a->children[i] = HANDLE_INVALID;
    if (libc && libc->prepare)
        libc->prepare(a->ctx);
    status_t st = jam_vmar_clone(k->proc, a->vmos, a->children, a->n, 0);
    for (uint32_t i = 0; st != OK && i < a->n; i++)
        a->children[i] = HANDLE_INVALID;   /* they name nothing */
    if (st == OK && libc && libc->give)
        st = libc->give(a->ctx, k->ch[0], k->proc);
    if (libc && libc->parent)
        libc->parent(a->ctx, st);
    return st;
}

/* jam_fork's parent side (fork_entry.S calls it with the block). */
int64_t jam_fork_parent(struct fork_block *b, const struct jam_fork_args *a)
{
    static const struct jam_fork_args none;
    if (!a)
        a = &none;
    const struct jam_fork_libc *libc = jam_fork_hooks();
    b->fs_base = a->fs_base;
    b->ctx = a->ctx;
    struct kid k = { 0 };
    struct ns_out nso;   /* ours, not the heap's: the C library may hold its allocator's lock */
    status_t st = make_kid(a, &k);
    if (st == OK)
        st = write_roles(a, &k);
    if (st == OK)
        st = clone(a, libc, &k);
    if (st == OK)
        st = ns_prepare_in(NS_ALL, &nso);   /* after the clone: the child's copy holds none of it */
    bool prepared = st == OK;
    if (st == OK) {
        uint64_t sp = ((uint64_t)(uintptr_t)b - CHILD_GAP) & ~15ull;
        st = jam_process_start(k.proc, k.thread, (uint64_t)(uintptr_t)jam_fork_trampoline, sp,
                               k.ch[1], (uint64_t)(uintptr_t)b);
        k.ch[1] = HANDLE_INVALID;   /* the child's now, or closed */
    }
    if (st == OK) {
        st = ns_send_in(k.ns[0], NS_MOUNT, &nso);   /* its handles gone, whatever the status */
        if (st == ERR_PEER_CLOSED)
            st = OK;   /* it has ended already, without looking */
        else if (st != OK)
            jam_process_kill(k.proc);   /* it would run without its namespace */
    }
    if (prepared)
        ns_prepared_close(&nso);   /* none left if it was sent */
    if (st == OK && a->signals) {
        *a->signals = k.sig[0];
        k.sig[0] = HANDLE_INVALID;
    }
    close_kid(&k);
    if (st != OK) {
        if (k.proc)
            jam_handle_close(k.proc);   /* one that never started dies with its last handle */
        return st;
    }
    return k.proc;
}

/* The fork message (its *nh handles into hs, ours to close if it isn't
 * OK), checked: OK, or why not. */
static status_t take_message(handle_t ch, struct fork_msg *m, handle_t *hs, uint32_t *nh)
{
    uint32_t nbytes = 0;
    struct channel_read_args r = {
        .h = ch, .rd_cap = sizeof(*m), .rd_bytes = (uint64_t)(uintptr_t)m,
        .rd_actual = (uint64_t)(uintptr_t)&nbytes, .rd_handles = (uint64_t)(uintptr_t)hs,
        .rd_hcap = FORK_MAX_HANDLES, .rd_hactual = (uint64_t)(uintptr_t)nh,
    };
    status_t st = jam_channel_read(&r);
    if (st == ERR_SHOULD_WAIT) {   /* written before our start: never, but cheap to allow */
        signals_t seen;
        st = jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER, &seen);
        if (st == OK)
            st = jam_channel_read(&r);
    }
    if (st != OK) {
        *nh = 0;   /* no handles came */
        return st;
    }
    if (nbytes != sizeof(*m) || m->txid || m->magic != FORK_MAGIC ||
        m->version != FORK_VERSION || m->nhandles != *nh || *nh > FORK_MAX_HANDLES)
        return ERR_INVALID_ARGS;
    for (uint32_t i = 0; i < *nh; i++) {
        if (m->roles[i] == SR_NONE)
            return ERR_INVALID_ARGS;
        for (uint32_t j = 0; j < i; j++)
            if (m->roles[j] == m->roles[i])
                return ERR_INVALID_ARGS;   /* each role once */
    }
    return OK;
}

/* jam_fork's child side, on the trampoline (fork_entry.S): ch is our
 * startup channel, b our copy of the block. */
void jam_fork_child(handle_t ch, struct fork_block *b)
{
    if (b->fs_base)
        (void)jam_thread_set_fs_base(b->fs_base);   /* the parent's thread had it: valid */
    struct fork_msg m;
    handle_t hs[FORK_MAX_HANDLES];
    uint32_t nh = 0;
    status_t st = take_message(ch, &m, hs, &nh);
    if (st != OK) {
        for (uint32_t i = 0; i < nh && i < FORK_MAX_HANDLES; i++)
            jam_handle_close(hs[i]);
        static const char line[] = "libos: a forked child given no usable fork message\n";
        jam_debug_write(line, sizeof(line) - 1);
        jam_process_exit(FORK_BAD_MESSAGE);
    }
    startup_forked(ch, hs, m.roles, nh);
    ns_forked();
    fs_forked();
    fdmsg_forked();
    const struct jam_fork_libc *libc = jam_fork_hooks();
    if (libc && libc->child)
        libc->child(b->ctx, ch);
}
