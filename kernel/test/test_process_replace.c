/* process_replace, exec in place (docs/FORK-PLAN.md section 2, track FK3;
 * object/process.c's "exec in place"):
 *
 *   replace_keeps_identity
 *       the process keeps its kernel object, koid, pid, job and creation
 *       time; a port binding and a wait made on it before see the new
 *       image's end; it takes the new name and loses [jit]; the image is
 *       consumed (DEAD, exit 0); the table holds arg0 alone, under its old
 *       value, every other old value stale, the peer of a closed end told;
 *       the new image starts with rdi = arg0, rsi = arg1, rsp = stack,
 *       every other register 0, the default MXCSR, x87 control word and
 *       xmm0; the old thread's tid word is never stored to.
 *   replace_ends_threads
 *       other threads spinning in ring 3 on other CPUs, in a futex wait,
 *       a channel_call nobody answers, object_wait_one, sleep_until and a
 *       page fault waiting for a pager all end (dead, SIG_TERMINATED); a
 *       thread made but never started can't be started; a thread started
 *       in the new image afterwards runs.
 *   replace_frees_old
 *       the old address space goes with every page it was charged for (a
 *       64-page VMO only it mapped, its tables): the job pays for the new
 *       image and one thread, to the page.
 *   replace_refusals_change_nothing
 *       each refusal (entry, arg0 bad, without RIGHT_TRANSFER or naming
 *       proc, proc's rights, type and state: itself, started, with a
 *       thread, another job's, killed; a kernel thread calling) returns
 *       its error with every thread running, every handle where it was
 *       (arg0 under its value) and proc NEW with its address space; a
 *       replace after them works.
 *   replace_two_at_once
 *       two threads replacing at once, each with its own image: exactly
 *       one image runs, alone.
 *   replace_racing_kill
 *       a kill at a different moment each round of a replace that must
 *       end other threads: the process dies killed, both images go and
 *       the job is empty.
 *
 * Real processes run small programs assembled here (rp_blob): the old
 * image at O_CODE, the new one at N_CODE (the same code VMO mapped twice),
 * each with its own stack and data page. The test drives them through
 * their data pages. The names start "replace_", so `ktest=replace` runs
 * them. */
#include <jam/abi.h>
#include <jam/aspace.h>
#include <jam/aspace_vmo.h>
#include <jam/channel.h>
#include <jam/event.h>
#include <jam/handle.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/object.h>
#include <jam/pager.h>
#include <jam/percpu.h>
#include <jam/port.h>
#include <jam/process.h>
#include <jam/sched.h>
#include <jam/string.h>
#include <jam/syscall_nums.h>
#include <jam/time.h>
#include <jam/uentry.h>
#include <jam/vmo.h>

#define PG 4096ull

/* The two images (each in one 2 MiB stretch, a 1 GiB apart). Plain
 * numbers: the programs use them too. */
#define O_CODE      0x10000000   /* the programs, as the old image maps them */
#define O_STACK     0x10010000
#define O_DATA      0x10030000
#define O_MORE      0x10040000   /* a pager's page, a JIT VMO */
#define O_BIG       0x40000000   /* replace_frees_old's VMO */
#define N_CODE      0x20000000   /* the same programs, as the new image maps them */
#define N_STACK     0x20010000
#define N_DATA      0x20030000
#define CODE_PAGES  2
#define STACK_PAGES 16
#define DATA_PAGES  2
#define BIG_PAGES   64
#define O_STACK_TOP (O_STACK + STACK_PAGES * PG)
#define N_STACK_TOP (N_STACK + STACK_PAGES * PG)

/* The old image's data page. */
#define OD_GO      0x000   /* the test bumps it: replace with the words below */
#define OD_SEQ     0x008   /* the last OD_GO the main thread acted on */
#define OD_PROC    0x010
#define OD_ENTRY   0x018
#define OD_STACK   0x020
#define OD_ARG0    0x028
#define OD_ARG1    0x030
#define OD_RET     0x038   /* what process_replace returned */
#define OD_UP      0x040   /* the main thread is set up */
#define OD_READY   0x048   /* helpers about to block */
#define OD_SPIN    0x050   /* h_spin's rounds */
#define OD_FUTEX   0x058   /* h_futex's word (0) */
#define OD_EVENT   0x060   /* h_wait's event */
#define OD_ARG0OK  0x068   /* handle_info(OD_ARG0) after a replace came back */
#define OD_GO2     0x070   /* h_replace: as OD_GO, with OD_PROC2 */
#define OD_SEQ2    0x078
#define OD_PROC2   0x080
#define OD_RET2    0x088
#define OD_MXCSR   0x090   /* the main thread's MXCSR, x87 control word, xmm0 */
#define OD_FCW     0x098
#define OD_XMM     0x0a0
#define OD_INFO    0x100   /* handle_info's answer */
#define OD_CALL    0x200   /* h_call's struct channel_call_args */
#define OD_CBUF    0x300   /* its request, then its reply buffer */
#define OD_FSWORD  0x800   /* its FS base points here */

/* The new image's data page. */
#define ND_UP      0x000   /* DONE: it ran */
#define ND_GO      0x008   /* the test: end, by ND_HOW */
#define ND_HOW     0x010   /* 0: thread_exit; else process_exit(ND_HOW) */
#define ND_HRET    0x018   /* handle_info(rdi) */
#define ND_STALE   0x020   /* old handle values that named something */
#define ND_NOLD    0x028   /* values in ND_OLD */
#define ND_MXCSR   0x030
#define ND_FCW     0x038
#define ND_XMM0    0x040
#define ND_TIDW    0x048   /* the old main thread's tid word points here */
#define ND_REGS    0x080   /* rax rbx rcx rdx rsi rdi rbp rsp r8 .. r15 at the entry */
#define ND_INFO    0x100   /* handle_info(rdi)'s answer */
#define ND_INFO2   0x180
#define ND_OLD     0x200   /* old handle values, 8 bytes each */

#define DONE 0x600d
#define A_SIGNALED 8   /* SIG_SIGNALED as the assembler takes it */
_Static_assert(A_SIGNALED == SIG_SIGNALED, "the programs' constants");

#define CHAN_RIGHTS  (RIGHTS_BASIC | RIGHT_READ | RIGHT_WRITE | RIGHT_SIGNAL)
#define EVENT_RIGHTS (RIGHTS_BASIC | RIGHT_SIGNAL)

#define S_(x) #x
#define S(x) S_(x)
#define OSET(name, off) ".set " #name ", " S(O_DATA) " + " S(off) "\n"
#define NSET(name, off) ".set " #name ", " S(N_DATA) " + " S(off) "\n"

/* The programs: position-independent, the data pages by their absolute
 * addresses; they use no stack. */
__asm__(
    ".pushsection .rodata.replace_progs, \"a\"\n"
    ".balign 64\n"
    OSET(od_go, OD_GO) OSET(od_seq, OD_SEQ) OSET(od_proc, OD_PROC) OSET(od_entry, OD_ENTRY)
    OSET(od_stack, OD_STACK) OSET(od_arg0, OD_ARG0) OSET(od_arg1, OD_ARG1) OSET(od_ret, OD_RET)
    OSET(od_up, OD_UP) OSET(od_ready, OD_READY) OSET(od_spin, OD_SPIN) OSET(od_event, OD_EVENT)
    OSET(od_arg0ok, OD_ARG0OK) OSET(od_go2, OD_GO2) OSET(od_seq2, OD_SEQ2)
    OSET(od_proc2, OD_PROC2) OSET(od_ret2, OD_RET2) OSET(od_mxcsr, OD_MXCSR)
    OSET(od_fcw, OD_FCW) OSET(od_xmm, OD_XMM)
    NSET(nd_up, ND_UP) NSET(nd_go, ND_GO) NSET(nd_how, ND_HOW) NSET(nd_hret, ND_HRET)
    NSET(nd_stale, ND_STALE) NSET(nd_nold, ND_NOLD) NSET(nd_mxcsr, ND_MXCSR)
    NSET(nd_fcw, ND_FCW) NSET(nd_xmm0, ND_XMM0) NSET(nd_regs, ND_REGS) NSET(nd_old, ND_OLD)
    "rp_blob:\n"

    /* ---- the old image ------------------------------------------------ */
    "o_main:\n"                         /* an FS base, a tid word, FPU state, then replaces */
    "    movq $" S(O_DATA) " + " S(OD_FSWORD) ", %rdi\n"
    "    movl $" S(SYS_thread_set_fs_base) ", %eax\n"
    "    syscall\n"
    "    movq $" S(N_DATA) " + " S(ND_TIDW) ", %rdi\n"
    "    movl $" S(SYS_thread_set_tid_word) ", %eax\n"
    "    syscall\n"
    "    ldmxcsr od_mxcsr\n"
    "    fldcw od_fcw\n"
    "    movq od_xmm, %xmm0\n"
    "    movq $1, od_up\n"
    "1:  pause\n"
    "    movq od_go, %r15\n"
    "    cmpq od_seq, %r15\n"
    "    je 1b\n"
    "    movq od_proc, %rdi\n"
    "    movq od_entry, %rsi\n"
    "    movq od_stack, %rdx\n"
    "    movq od_arg0, %r10\n"
    "    movq od_arg1, %r8\n"
    "    movabsq $0x1111111111111111, %rbx\n"
    "    movq %rbx, %rbp\n"
    "    movq %rbx, %r9\n"
    "    movq %rbx, %r12\n"
    "    movq %rbx, %r13\n"
    "    movq %rbx, %r14\n"
    "    movl $" S(SYS_process_replace) ", %eax\n"
    "    syscall\n"
    "    movq %rax, od_ret\n"           /* it came back: is arg0 still there? */
    "    movq od_arg0, %rdi\n"
    "    movq $" S(O_DATA) " + " S(OD_INFO) ", %rsi\n"
    "    movl $" S(SYS_handle_info) ", %eax\n"
    "    syscall\n"
    "    movq %rax, od_arg0ok\n"
    "    movq %r15, od_seq\n"
    "    jmp 1b\n"

    "h_replace:\n"                      /* a second thread replacing: OD_PROC2, rsi = 2 */
    "1:  pause\n"
    "    movq od_go2, %r15\n"
    "    cmpq od_seq2, %r15\n"
    "    je 1b\n"
    "    movq od_proc2, %rdi\n"
    "    movq od_entry, %rsi\n"
    "    movq od_stack, %rdx\n"
    "    xorl %r10d, %r10d\n"
    "    movl $2, %r8d\n"
    "    movl $" S(SYS_process_replace) ", %eax\n"
    "    syscall\n"
    "    movq %rax, od_ret2\n"
    "    movq %r15, od_seq2\n"
    "    jmp 1b\n"

    "h_spin:\n"                         /* ring 3 only */
    "1:  lock incq od_spin\n"
    "    pause\n"
    "    jmp 1b\n"

    "h_futex:\n"
    "    lock incq od_ready\n"
    "1:  movq $" S(O_DATA) " + " S(OD_FUTEX) ", %rdi\n"
    "    xorl %esi, %esi\n"
    "    movq $-1, %rdx\n"
    "    xorl %r10d, %r10d\n"
    "    movl $" S(SYS_futex_wait) ", %eax\n"
    "    syscall\n"
    "    jmp 1b\n"

    "h_call:\n"                         /* nobody answers */
    "    lock incq od_ready\n"
    "1:  movq $" S(O_DATA) " + " S(OD_CALL) ", %rdi\n"
    "    movl $" S(SYS_channel_call) ", %eax\n"
    "    syscall\n"
    "    jmp 1b\n"

    "h_wait:\n"                         /* an event nobody signals */
    "    lock incq od_ready\n"
    "1:  movq od_event, %rdi\n"
    "    movl $" S(A_SIGNALED) ", %esi\n"
    "    movq $-1, %rdx\n"
    "    xorl %r10d, %r10d\n"
    "    movl $" S(SYS_object_wait_one) ", %eax\n"
    "    syscall\n"
    "    jmp 1b\n"

    "h_sleep:\n"
    "    lock incq od_ready\n"
    "1:  movq $-1, %rdi\n"
    "    movl $" S(SYS_sleep_until) ", %eax\n"
    "    syscall\n"
    "    jmp 1b\n"

    "h_pager:\n"                        /* a pager that never answers */
    "    lock incq od_ready\n"
    "1:  movq " S(O_MORE) ", %rax\n"
    "    jmp 1b\n"

    "h_touch:\n"                        /* every page of O_BIG, then spin */
    "    movq $" S(O_BIG) ", %rax\n"
    "    movl $" S(BIG_PAGES) ", %ecx\n"
    "1:  movq $1, (%rax)\n"
    "    addq $4096, %rax\n"
    "    decl %ecx\n"
    "    jnz 1b\n"
    "    lock incq od_ready\n"
    "2:  pause\n"
    "    jmp 2b\n"

    /* ---- the new image ------------------------------------------------ */
    "n_main:\n"                         /* what it was given, its handles, then waits */
    "    movq %rax, nd_regs + 0\n"
    "    movq %rbx, nd_regs + 8\n"
    "    movq %rcx, nd_regs + 16\n"
    "    movq %rdx, nd_regs + 24\n"
    "    movq %rsi, nd_regs + 32\n"
    "    movq %rdi, nd_regs + 40\n"
    "    movq %rbp, nd_regs + 48\n"
    "    movq %rsp, nd_regs + 56\n"
    "    movq %r8, nd_regs + 64\n"
    "    movq %r9, nd_regs + 72\n"
    "    movq %r10, nd_regs + 80\n"
    "    movq %r11, nd_regs + 88\n"
    "    movq %r12, nd_regs + 96\n"
    "    movq %r13, nd_regs + 104\n"
    "    movq %r14, nd_regs + 112\n"
    "    movq %r15, nd_regs + 120\n"
    "    stmxcsr nd_mxcsr\n"
    "    fnstcw nd_fcw\n"
    "    movq %xmm0, nd_xmm0\n"
    "    movq $" S(N_DATA) " + " S(ND_INFO) ", %rsi\n"
    "    movl $" S(SYS_handle_info) ", %eax\n"
    "    syscall\n"
    "    movq %rax, nd_hret\n"
    "    xorl %ebx, %ebx\n"
    "2:  cmpq nd_nold, %rbx\n"
    "    jae 4f\n"
    "    movq nd_old(,%rbx,8), %rdi\n"
    "    movq $" S(N_DATA) " + " S(ND_INFO2) ", %rsi\n"
    "    movl $" S(SYS_handle_info) ", %eax\n"
    "    syscall\n"
    "    cmpq $" S(ERR_BAD_HANDLE) ", %rax\n"
    "    je 3f\n"
    "    incq nd_stale\n"
    "3:  incq %rbx\n"
    "    jmp 2b\n"
    "4:  movq $" S(DONE) ", nd_up\n"
    "5:  pause\n"
    "    cmpq $0, nd_go\n"
    "    je 5b\n"
    "    movq nd_how, %rdi\n"
    "    testq %rdi, %rdi\n"
    "    jz 6f\n"
    "    movl $" S(SYS_process_exit) ", %eax\n"
    "    syscall\n"
    "6:  movl $" S(SYS_thread_exit) ", %eax\n"
    "    syscall\n"
    "    ud2\n"

    "n_spin:\n"
    "1:  pause\n"
    "    jmp 1b\n"
    "rp_blob_end:\n"
    ".popsection\n");

extern const uint8_t rp_blob[], rp_blob_end[], o_main[], h_replace[], h_spin[], h_futex[],
    h_call[], h_wait[], h_sleep[], h_pager[], h_touch[], n_main[], n_spin[];

/* Where label l runs in the old image, and in the new. */
#define AT_O(l) (O_CODE + (uint64_t)((l) - rp_blob))
#define AT_N(l) (N_CODE + (uint64_t)((l) - rp_blob))

/* ---- the two processes ----------------------------------------------------- */

#define RP_THREADS 12

struct rp {
    struct job     *j;
    struct process *p;        /* the process replaced */
    struct process *n;        /* the image it takes, made unstarted */
    struct vmo     *code, *ostack, *odata, *nstack, *ndata;
    struct uthread *u[RP_THREADS];   /* u[0] p's main thread */
    unsigned        nu;
    handle_t        hn;       /* p's handle to n */
    uint64_t        go;       /* the last OD_GO */
    uint64_t        n_pages;  /* what n's address space was charged as it was made */
};

static void put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

static struct vmo *new_vmo(uint64_t pages)
{
    struct vmo *v;
    KT_EQ(vmo_create(pages * PG, 0, &v), OK);
    return v;
}

static uint64_t vget(struct vmo *v, uint64_t off)
{
    uint64_t w = 0;
    KT_EQ(vmo_read(v, off, &w, 8), OK);
    return w;
}

static void vset(struct vmo *v, uint64_t off, uint64_t val)
{
    KT_EQ(vmo_write(v, off, &val, 8), OK);
}

/* Wait until v's word at off is at least want. */
static void wait_word(struct vmo *v, uint64_t off, uint64_t want)
{
    uint64_t deadline = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    while (vget(v, off) < want) {
        KT_ASSERT(uptime_ns() < deadline);
        thread_sleep_ms(1);
    }
}

static void map_at(struct process *p, struct vmo *v, uint64_t at, uint64_t len, unsigned perms)
{
    struct aspace *as = process_aspace(p);
    KT_ASSERT(as);
    uint64_t a = at;
    KT_EQ(aspace_map(as, v, 0, len, perms | ASPACE_FIXED, &a), OK);
    aspace_unref(as);
}

/* The programs at code_at, with a stack and a data page after them. */
static void image(struct process *p, struct vmo *code, uint64_t code_at, struct vmo *stack,
                  struct vmo *data)
{
    map_at(p, code, code_at, CODE_PAGES * PG, ASPACE_READ | ASPACE_EXEC);
    map_at(p, stack, code_at + (O_STACK - O_CODE), STACK_PAGES * PG,
           ASPACE_READ | ASPACE_WRITE);
    map_at(p, data, code_at + (O_DATA - O_CODE), DATA_PAGES * PG, ASPACE_READ | ASPACE_WRITE);
}

/* A handle in p's table to obj (a reference of its own). */
static handle_t insert(struct process *p, struct kobject *obj, rights_t rights)
{
    kobject_ref(obj);
    struct khandle kh = khandle_from_new(obj, rights);
    handle_t h;
    KT_EQ(handle_insert(process_handles(p), &kh, &h), OK);
    return h;
}

static uint32_t handles_in(struct process *p)
{
    struct handle_table *t = process_handles(p);
    uint64_t f = spin_lock_irqsave(&t->lock);
    uint32_t n = t->used;
    spin_unlock_irqrestore(&t->lock, f);
    return n;
}

static struct process_info info_of(struct process *p)
{
    struct process_info i;
    process_get_info(p, &i);
    return i;
}

/* A job; p ("rp-old") with the old image; n (`name`) with the new one in
 * the same job, unstarted, and p's handle to it. */
static void rp_new(struct rp *r, const char *name)
{
    *r = (struct rp){ 0 };
    r->j = kt_fresh_job();
    KT_EQ(process_create(r->j, "rp-old", &r->p), OK);
    uint64_t len = (uint64_t)(rp_blob_end - rp_blob);
    KT_ASSERT(len <= CODE_PAGES * PG);
    r->code = new_vmo(CODE_PAGES);
    KT_EQ(vmo_write(r->code, 0, rp_blob, len), OK);
    r->ostack = new_vmo(STACK_PAGES);
    r->odata = new_vmo(DATA_PAGES);
    r->nstack = new_vmo(STACK_PAGES);
    r->ndata = new_vmo(DATA_PAGES);
    image(r->p, r->code, O_CODE, r->ostack, r->odata);
    uint64_t before = job_used(r->j, JOB_LIMIT_PAGES);
    KT_EQ(process_create(r->j, name, &r->n), OK);
    image(r->n, r->code, N_CODE, r->nstack, r->ndata);
    r->n_pages = job_used(r->j, JOB_LIMIT_PAGES) - before;
    r->hn = insert(r->p, process_kobject(r->n), PROCESS_RIGHTS);
    vset(r->odata, OD_MXCSR, 0x3f80);   /* round down: not the default */
    vset(r->odata, OD_FCW, 0x037b);     /* zero divide unmasked */
    vset(r->odata, OD_XMM, 0x5eed5eed5eed5eedull);
}

/* p's main thread (on `cpu` unless it is -1), set up and waiting. */
static void rp_start_on(struct rp *r, int cpu)
{
    KT_EQ(uthread_create(r->p, "main", &r->u[0]), OK);
    r->nu = 1;
    struct khandle none = { NULL, 0 };
    cpumask_t m;
    if (cpu >= 0)
        cpumask_one(&m, (uint32_t)cpu);
    KT_EQ(process_start(r->p, r->u[0], AT_O(o_main), O_STACK_TOP, &none, 0, cpu >= 0 ? &m : NULL),
          OK);
    wait_word(r->odata, OD_UP, 1);
}

static void rp_start(struct rp *r)
{
    rp_start_on(r, -1);
}

/* Another thread of p at prog, on `cpu` unless it is -1. */
static struct uthread *rp_thread(struct rp *r, const uint8_t *prog, int cpu)
{
    KT_ASSERT(r->nu < RP_THREADS);
    struct uthread *u;
    KT_EQ(uthread_create(r->p, "helper", &u), OK);
    cpumask_t m;
    if (cpu >= 0)
        cpumask_one(&m, (uint32_t)cpu);
    KT_EQ(uthread_start(u, AT_O(prog), O_STACK_TOP - r->nu * PG, 0, 0, 0, cpu >= 0 ? &m : NULL),
          OK);
    r->u[r->nu++] = u;
    return u;
}

/* The main thread calls process_replace with these (no wait). */
static void rp_replace(struct rp *r, handle_t proc, uint64_t entry, uint64_t stack, handle_t arg0,
                       uint64_t arg1)
{
    vset(r->odata, OD_PROC, proc);
    vset(r->odata, OD_ENTRY, entry);
    vset(r->odata, OD_STACK, stack);
    vset(r->odata, OD_ARG0, arg0);
    vset(r->odata, OD_ARG1, arg1);
    vset(r->odata, OD_GO, ++r->go);
}

/* It came back: what it returned. */
static int64_t rp_returned(struct rp *r)
{
    wait_word(r->odata, OD_SEQ, r->go);
    return (int64_t)vget(r->odata, OD_RET);
}

/* The new image is running (data: its data page). */
static void rp_new_up(struct vmo *data)
{
    wait_word(data, ND_UP, DONE);
}

/* p's end: SIG_TERMINATED, its info. */
static struct process_info rp_dead(struct process *p)
{
    KT_EQ(object_wait_one(process_kobject(p), SIG_TERMINATED,
                          uptime_ns() + kt_patience_ms(10000) * NS_PER_MS, NULL),
          OK);
    struct process_info i = info_of(p);
    KT_EQ(i.state, PROCESS_DEAD);
    return i;
}

/* The new image (data page `data`) ends: how 0 by thread_exit, else
 * process_exit(how). */
static struct process_info rp_finish(struct rp *r, struct vmo *data, uint64_t how)
{
    vset(data, ND_HOW, how);
    vset(data, ND_GO, 1);
    struct process_info i = rp_dead(r->p);
    KT_ASSERT(!i.killed);
    KT_EQ(i.exit_code, (int64_t)how);
    return i;
}

static void rp_end(struct rp *r)
{
    for (unsigned i = 0; i < r->nu; i++)
        kobject_unref(uthread_kobject(r->u[i]));
    kobject_unref(process_kobject(r->p));
    kobject_unref(process_kobject(r->n));
    put(r->code);
    put(r->ostack);
    put(r->odata);
    put(r->nstack);
    put(r->ndata);
    kt_job_is_empty(r->j);
    job_unref(r->j);
}

static uint32_t other_cpu(uint32_t k)
{
    return cpu_count > 1 ? 1 + k % (cpu_count - 1) : 0;
}

/* ---- the tests --------------------------------------------------------------- */

KTEST(replace_keeps_identity)
{
    struct rp r;
    rp_new(&r, "rp-new");
    /* The old image maps JIT code: [jit] until the replace. */
    struct vmo *jv = new_vmo(1);
    KT_EQ(vmo_make_jit(jv), OK);
    map_at(r.p, jv, O_MORE, PG, ASPACE_READ | ASPACE_EXEC);
    put(jv);
    /* p's handles: the hand-over channel's end (arg0), itself, an event,
     * and a channel end whose peer the test keeps. */
    struct channel *c0, *c1, *c2, *c3;
    KT_EQ(channel_create(&c0, &c1), OK);
    KT_EQ(channel_create(&c2, &c3), OK);
    handle_t a0 = insert(r.p, (struct kobject *)c1, CHAN_RIGHTS);
    handle_t hself = insert(r.p, process_kobject(r.p), PROCESS_RIGHTS);
    struct event *ev;
    KT_EQ(event_create(&ev), OK);
    handle_t hev = insert(r.p, (struct kobject *)ev, EVENT_RIGHTS);
    handle_t hc2 = insert(r.p, (struct kobject *)c2, CHAN_RIGHTS);
    kobject_unref((struct kobject *)ev);
    kobject_unref((struct kobject *)c1);   /* p's table keeps them */
    kobject_unref((struct kobject *)c2);
    /* A parent's handle to p (a binding ends with an object's last handle,
     * and p's own go at the replace), and a binding made through it. */
    struct handle_table parent;
    handle_table_init(&parent);
    kobject_ref(process_kobject(r.p));
    struct khandle kp = khandle_from_new(process_kobject(r.p), PROCESS_RIGHTS);
    handle_t hp;
    KT_EQ(handle_insert(&parent, &kp, &hp), OK);
    struct port *port;
    KT_EQ(port_create(&port), OK);
    KT_EQ(port_bind_held(port, process_kobject(r.p), 77, SIG_TERMINATED, PORT_BIND_ONCE), OK);

    rp_start(&r);
    struct process_info before = info_of(r.p);
    struct thread_info tbefore;
    uthread_get_info(r.u[0], &tbefore);
    KT_ASSERT(before.flags & PROCESS_FLAG_JIT);
    const handle_t old[] = { r.hn, hself, hev, hc2 };
    for (unsigned i = 0; i < 4; i++)
        vset(r.ndata, ND_OLD + i * 8, old[i]);
    vset(r.ndata, ND_NOLD, 4);
    vset(r.ndata, ND_TIDW, ~0ull);
    rp_replace(&r, r.hn, AT_N(n_main), N_STACK_TOP, a0, 0x1234);
    rp_new_up(r.ndata);

    /* The same process, its new name, no [jit]. */
    struct process_info after = info_of(r.p);
    KT_EQ(after.koid, before.koid);
    KT_EQ(after.pid, before.pid);
    KT_EQ(after.start_ns, before.start_ns);
    KT_ASSERT(after.cpu_ns >= before.cpu_ns);
    KT_EQ(after.state, PROCESS_RUNNING);
    KT_EQ(after.threads, 1);
    KT_EQ(after.flags, 0);
    KT_ASSERT(!strcmp(after.name, "rp-new"));
    KT_EQ(process_job_koid(r.p), job_kobject(r.j)->koid);
    struct thread_info tafter;
    uthread_get_info(r.u[0], &tafter);
    KT_EQ(tafter.tid, tbefore.tid);
    KT_EQ(tafter.state, THREAD_RUNNING);
    /* The image is used up. */
    struct process_info ni = info_of(r.n);
    KT_EQ(ni.state, PROCESS_DEAD);
    KT_EQ(ni.exit_code, 0);
    KT_ASSERT(!ni.killed);
    KT_ASSERT(kobject_signals(process_kobject(r.n)) & SIG_TERMINATED);
    /* Only arg0 is left, under its value; c2 closed, so c3 is told. */
    KT_EQ(handles_in(r.p), 1);
    struct kobject *o;
    KT_EQ(handle_table_find(process_handles(r.p), OBJ_CHANNEL, &o), OK);
    KT_ASSERT(o == (struct kobject *)c1);
    kobject_unref(o);
    KT_ASSERT(kobject_signals((struct kobject *)c3) & SIG_PEER_CLOSED);
    KT_ASSERT(!(kobject_signals((struct kobject *)c0) & SIG_PEER_CLOSED));
    /* What the new image was given and found. */
    for (unsigned i = 0; i < 16; i++) {
        uint64_t v = vget(r.ndata, ND_REGS + i * 8);
        uint64_t want = i == 4 ? 0x1234 : i == 5 ? a0 : i == 7 ? N_STACK_TOP : 0;
        KT_EQ(v, want);
    }
    KT_EQ(vget(r.ndata, ND_MXCSR) & 0xffffffff, 0x1f80);
    KT_EQ(vget(r.ndata, ND_FCW) & 0xffff, 0x037f);
    KT_EQ(vget(r.ndata, ND_XMM0), 0);
    KT_EQ((int64_t)vget(r.ndata, ND_HRET), OK);
    struct handle_info hi;
    KT_EQ(vmo_read(r.ndata, ND_INFO, &hi, sizeof(hi)), OK);
    KT_EQ(hi.type, OBJ_CHANNEL);
    KT_EQ(hi.koid, ((struct kobject *)c1)->koid);
    KT_EQ(vget(r.ndata, ND_STALE), 0);

    /* Its end, by thread_exit: the old tid word is never stored to, and
     * the binding and a wait made before the replace see it. */
    rp_finish(&r, r.ndata, 0);
    KT_EQ(vget(r.ndata, ND_TIDW), ~0ull);
    struct port_packet pkt;
    KT_EQ(port_wait(port, uptime_ns() + kt_patience_ms(2000) * NS_PER_MS, &pkt), OK);
    KT_EQ(pkt.key, 77);
    KT_EQ(pkt.type, PORT_PACKET_SIGNAL);
    KT_ASSERT(pkt.signal.observed & SIG_TERMINATED);
    uint8_t buf[16];
    uint32_t nb;
    KT_EQ(channel_read(c0, buf, sizeof(buf), &nb, NULL, 0, NULL), ERR_PEER_CLOSED);
    handle_table_destroy(&parent);
    kobject_unref((struct kobject *)port);
    kobject_unref((struct kobject *)c0);
    kobject_unref((struct kobject *)c3);
    rp_end(&r);
}

KTEST(replace_ends_threads)
{
    struct rp r;
    rp_new(&r, "rp-new");
    struct pager *pg;
    KT_EQ(pager_create(r.j, 16, PAGER_ANSWER_MAX_NS, &pg), OK);
    struct vmo *pv;
    KT_EQ(pager_create_vmo(pg, 9, PG, &pv), OK);
    map_at(r.p, pv, O_MORE, PG, ASPACE_READ);
    put(pv);
    struct channel *s0, *s1;
    KT_EQ(channel_create(&s0, &s1), OK);
    handle_t hs1 = insert(r.p, (struct kobject *)s1, CHAN_RIGHTS);
    kobject_unref((struct kobject *)s1);
    struct channel_call_args ca = {
        .h = hs1, .wr_len = 16, .wr_bytes = O_DATA + OD_CBUF, .rd_cap = 64,
        .rd_bytes = O_DATA + OD_CBUF + 64, .deadline_ns = UINT64_MAX,
    };
    KT_EQ(vmo_write(r.odata, OD_CALL, &ca, sizeof(ca)), OK);
    struct event *ev;
    KT_EQ(event_create(&ev), OK);
    vset(r.odata, OD_EVENT, insert(r.p, (struct kobject *)ev, EVENT_RIGHTS));
    kobject_unref((struct kobject *)ev);

    rp_start(&r);
    struct uthread *never;
    KT_EQ(uthread_create(r.p, "never", &never), OK);
    rp_thread(&r, h_spin, (int)other_cpu(0));
    rp_thread(&r, h_spin, (int)other_cpu(1));
    rp_thread(&r, h_spin, (int)other_cpu(2));
    rp_thread(&r, h_futex, -1);
    rp_thread(&r, h_call, -1);
    rp_thread(&r, h_wait, -1);
    rp_thread(&r, h_sleep, -1);
    rp_thread(&r, h_pager, -1);
    wait_word(r.odata, OD_READY, 5);
    wait_word(r.odata, OD_SPIN, 1000);
    struct pager_request q;
    kt_pager_next(pg, &q);   /* the fault is waiting for it */
    KT_EQ(q.key, 9);
    KT_EQ(object_wait_one((struct kobject *)s0, SIG_READABLE,
                          uptime_ns() + kt_patience_ms(5000) * NS_PER_MS, NULL),
          OK);
    thread_sleep_ms(20);   /* the rest in their waits */
    KT_EQ(info_of(r.p).threads, r.nu);

    uint64_t t0 = uptime_ns();
    rp_replace(&r, r.hn, AT_N(n_main), N_STACK_TOP, HANDLE_INVALID, 0);
    rp_new_up(r.ndata);
    /* (Threads in ring 3 stop at their CPU's next tick, as for a kill.) */
    kprintf("ktest replace_ends_threads: %u threads ended and the image running in %lu us\n",
            r.nu - 1, (unsigned long)((uptime_ns() - t0) / 1000));
    KT_EQ(info_of(r.p).threads, 1);
    KT_EQ(job_used(r.j, JOB_LIMIT_THREADS), 1);
    for (unsigned i = 1; i < r.nu; i++) {
        struct thread_info ti;
        uthread_get_info(r.u[i], &ti);
        KT_EQ(ti.state, THREAD_DEAD);
        KT_ASSERT(kobject_signals(uthread_kobject(r.u[i])) & SIG_TERMINATED);
    }
    struct thread_info ti;
    uthread_get_info(never, &ti);
    KT_EQ(ti.state, THREAD_DEAD);
    KT_EQ(uthread_start(never, AT_N(n_spin), N_STACK_TOP - PG, 0, 0, 0, NULL), ERR_BAD_STATE);
    kobject_unref(uthread_kobject(never));
    /* The new image may have threads of its own. */
    struct uthread *late;
    KT_EQ(uthread_create(r.p, "late", &late), OK);
    KT_EQ(uthread_start(late, AT_N(n_spin), N_STACK_TOP - PG, 0, 0, 0, NULL), OK);
    KT_EQ(info_of(r.p).threads, 2);
    rp_finish(&r, r.ndata, 5);   /* process_exit ends `late` too */
    kobject_unref(uthread_kobject(late));

    /* The call's request, still queued (charged to the job until read). */
    uint8_t buf[64];
    uint32_t nb;
    KT_EQ(channel_read(s0, buf, sizeof(buf), &nb, NULL, 0, NULL), OK);
    KT_EQ(nb, 16);
    kobject_unref((struct kobject *)s0);
    kt_pager_close(pg);
    rp_end(&r);
}

KTEST(replace_frees_old)
{
    struct rp r;
    rp_new(&r, "rp-new");
    struct vmo *big;
    KT_EQ(vmo_create_for(r.j, BIG_PAGES * PG, 0, &big), OK);
    map_at(r.p, big, O_BIG, BIG_PAGES * PG, ASPACE_READ | ASPACE_WRITE);
    put(big);   /* only the old image holds it now */
    rp_start(&r);
    rp_thread(&r, h_touch, -1);
    wait_word(r.odata, OD_READY, 1);
    uint64_t used = job_used(r.j, JOB_LIMIT_PAGES);
    KT_ASSERT(used >= r.n_pages + BIG_PAGES + 2 * UTHREAD_KMEM_PAGES);

    rp_replace(&r, r.hn, AT_N(n_main), N_STACK_TOP, HANDLE_INVALID, 0);
    rp_new_up(r.ndata);
    /* The new image's address space (as made, and the tables its first
     * faults built) and one thread: nothing of the old one. */
    struct aspace *as = process_aspace(r.p);
    KT_ASSERT(as);
    uint64_t tables = aspace_pt_pages(as);
    aspace_unref(as);
    KT_EQ(job_used(r.j, JOB_LIMIT_PAGES), r.n_pages + tables + UTHREAD_KMEM_PAGES);
    KT_EQ(job_used(r.j, JOB_LIMIT_HANDLES), 1);   /* the process itself */
    KT_EQ(job_used(r.j, JOB_LIMIT_THREADS), 1);
    rp_finish(&r, r.ndata, 0);
    rp_end(&r);
}

struct refusal {
    const char *what;
    unsigned    proc;    /* index into the handles below */
    bool        bad_entry;
    unsigned    arg0;
    status_t    want;
};

/* The handles the refusals name (in p's table). */
enum { H_N, H_N_RO, H_SELF, H_A0, H_A0_NOMOVE, H_N2, H_N3, H_N4, H_N5, H_BAD, H_NONE, H_COUNT };

static const struct refusal refusals[] = {
    { "entry at USER_TOP", H_N, true, H_A0, ERR_INVALID_ARGS },
    { "arg0 no handle", H_N, false, H_BAD, ERR_BAD_HANDLE },
    { "arg0 without RIGHT_TRANSFER", H_N, false, H_A0_NOMOVE, ERR_ACCESS_DENIED },
    { "arg0 is proc", H_N, false, H_N, ERR_INVALID_ARGS },
    { "proc without RIGHT_WRITE", H_N_RO, false, H_A0, ERR_ACCESS_DENIED },
    { "proc no handle", H_BAD, false, H_A0, ERR_BAD_HANDLE },
    { "proc a channel", H_A0, false, H_NONE, ERR_WRONG_TYPE },
    { "proc itself", H_SELF, false, H_A0, ERR_BAD_STATE },
    { "proc started", H_N2, false, H_A0, ERR_BAD_STATE },
    { "proc with a thread", H_N3, false, H_A0, ERR_BAD_STATE },
    { "proc in another job", H_N4, false, H_A0, ERR_BAD_STATE },
    { "proc killed", H_N5, false, H_A0, ERR_BAD_STATE },
};

KTEST(replace_refusals_change_nothing)
{
    struct rp r;
    rp_new(&r, "rp-new");
    handle_t h[H_COUNT];
    h[H_N] = r.hn;
    h[H_N_RO] = insert(r.p, process_kobject(r.n), PROCESS_RIGHTS & ~RIGHT_WRITE);
    h[H_SELF] = insert(r.p, process_kobject(r.p), PROCESS_RIGHTS);
    struct channel *c0, *c1;
    KT_EQ(channel_create(&c0, &c1), OK);
    h[H_A0] = insert(r.p, (struct kobject *)c1, CHAN_RIGHTS);
    h[H_A0_NOMOVE] = insert(r.p, (struct kobject *)c1, CHAN_RIGHTS & ~RIGHT_TRANSFER);
    kobject_unref((struct kobject *)c1);
    /* n2 started (running the new image's program on its own data page),
     * n3 with a thread object, n4 in another job, n5 killed. */
    struct process *n2, *n3, *n4, *n5;
    struct vmo *d2 = new_vmo(DATA_PAGES), *s2 = new_vmo(STACK_PAGES);
    KT_EQ(process_create(r.j, "rp-n2", &n2), OK);
    image(n2, r.code, N_CODE, s2, d2);
    struct uthread *u2, *u3;
    KT_EQ(uthread_create(n2, "n2", &u2), OK);
    struct khandle none = { NULL, 0 };
    KT_EQ(process_start(n2, u2, AT_N(n_main), N_STACK_TOP, &none, 0, NULL), OK);
    rp_new_up(d2);
    KT_EQ(process_create(r.j, "rp-n3", &n3), OK);
    KT_EQ(uthread_create(n3, "n3", &u3), OK);
    struct job *j2 = kt_fresh_job();
    KT_EQ(process_create(j2, "rp-n4", &n4), OK);
    KT_EQ(process_create(r.j, "rp-n5", &n5), OK);
    process_kill(n5, PROCESS_KILLED_CODE, true);
    h[H_N2] = insert(r.p, process_kobject(n2), PROCESS_RIGHTS);
    h[H_N3] = insert(r.p, process_kobject(n3), PROCESS_RIGHTS);
    h[H_N4] = insert(r.p, process_kobject(n4), PROCESS_RIGHTS);
    h[H_N5] = insert(r.p, process_kobject(n5), PROCESS_RIGHTS);
    h[H_BAD] = 0xdead0001;
    h[H_NONE] = HANDLE_INVALID;

    rp_start(&r);
    rp_thread(&r, h_spin, (int)other_cpu(0));
    uint32_t handles = handles_in(r.p);
    for (unsigned i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
        const struct refusal *c = &refusals[i];
        uint64_t spin = vget(r.odata, OD_SPIN);
        rp_replace(&r, h[c->proc], c->bad_entry ? USER_TOP : AT_N(n_main), N_STACK_TOP,
                   h[c->arg0], 0);
        int64_t got = rp_returned(&r);
        if (got != c->want)
            kprintf("ktest replace_refusals_change_nothing: %s: %ld, not %ld\n", c->what,
                    (long)got, (long)c->want);
        KT_EQ(got, c->want);
        /* Nothing changed: arg0 where it was, every handle, both threads
         * (the other still running), n NEW with its address space. */
        if (c->arg0 == H_A0)
            KT_EQ((int64_t)vget(r.odata, OD_ARG0OK), OK);
        KT_EQ(handles_in(r.p), handles);
        struct process_info pi = info_of(r.p);
        KT_EQ(pi.state, PROCESS_RUNNING);
        KT_EQ(pi.threads, 2);
        KT_ASSERT(!strcmp(pi.name, "rp-old"));
        wait_word(r.odata, OD_SPIN, spin + 1000);
        KT_EQ(info_of(r.n).state, PROCESS_NEW);
        struct aspace *as = process_aspace(r.n);
        KT_ASSERT(as);
        aspace_unref(as);
    }
    /* A kernel thread has no process to replace. */
    kobject_ref(process_kobject(r.n));
    KT_EQ(process_replace(r.n, AT_N(n_main), N_STACK_TOP, HANDLE_INVALID, 0), ERR_BAD_STATE);
    KT_EQ(info_of(r.n).state, PROCESS_NEW);

    /* And after all that, it works. */
    rp_replace(&r, r.hn, AT_N(n_main), N_STACK_TOP, h[H_A0], 7);
    rp_new_up(r.ndata);
    KT_EQ(vget(r.ndata, ND_REGS + 5 * 8), h[H_A0]);
    KT_EQ(handles_in(r.p), 1);
    rp_finish(&r, r.ndata, 0);

    process_kill(n2, PROCESS_KILLED_CODE, true);
    rp_dead(n2);
    kobject_unref(uthread_kobject(u2));
    kobject_unref(uthread_kobject(u3));
    kobject_unref(process_kobject(n2));
    kobject_unref(process_kobject(n3));   /* its last handle went with p's table */
    kobject_unref(process_kobject(n4));
    kobject_unref(process_kobject(n5));
    kobject_unref((struct kobject *)c0);
    put(d2);
    put(s2);
    kt_job_is_empty(j2);
    job_unref(j2);
    rp_end(&r);
}

KTEST(replace_two_at_once)
{
    enum { ROUNDS = 20 };
    unsigned firsts = 0;
    for (unsigned round = 0; round < ROUNDS; round++) {
        struct rp r;
        rp_new(&r, "rp-n1");
        struct process *n2;
        struct vmo *d2 = new_vmo(DATA_PAGES), *s2 = new_vmo(STACK_PAGES);
        KT_EQ(process_create(r.j, "rp-n2", &n2), OK);
        image(n2, r.code, N_CODE, s2, d2);
        vset(r.odata, OD_PROC2, insert(r.p, process_kobject(n2), PROCESS_RIGHTS));
        rp_start_on(&r, (int)other_cpu(round));
        rp_thread(&r, h_replace, (int)other_cpu(round + 1));
        thread_sleep_ms(1);
        /* Both at once, in either order. */
        vset(r.odata, OD_PROC, r.hn);
        vset(r.odata, OD_ENTRY, AT_N(n_main));
        vset(r.odata, OD_STACK, N_STACK_TOP);
        vset(r.odata, OD_ARG0, HANDLE_INVALID);
        vset(r.odata, OD_ARG1, 1);
        r.go++;
        if (round & 1) {
            vset(r.odata, OD_GO2, 1);
            vset(r.odata, OD_GO, r.go);
        } else {
            vset(r.odata, OD_GO, r.go);
            vset(r.odata, OD_GO2, 1);
        }
        uint64_t deadline = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
        while (vget(r.ndata, ND_UP) != DONE && vget(d2, ND_UP) != DONE) {
            KT_ASSERT(uptime_ns() < deadline);
            thread_sleep_ms(1);
        }
        thread_sleep_ms(10);
        bool first = vget(r.ndata, ND_UP) == DONE;
        KT_ASSERT(first != (vget(d2, ND_UP) == DONE));   /* exactly one ran */
        firsts += first;
        struct vmo *won = first ? r.ndata : d2;
        KT_EQ(vget(won, ND_REGS + 4 * 8), first ? 1 : 2);   /* rsi: whose call */
        struct process_info pi = info_of(r.p);
        KT_EQ(pi.threads, 1);
        KT_ASSERT(!strcmp(pi.name, first ? "rp-n1" : "rp-n2"));
        rp_finish(&r, won, 3);
        KT_EQ(info_of(first ? r.n : n2).exit_code, 0);   /* the one used up */
        kobject_unref(process_kobject(n2));
        put(d2);
        put(s2);
        rp_end(&r);
    }
    kprintf("ktest replace_two_at_once: the main thread's won %u of %u\n", firsts, ROUNDS);
}

KTEST(replace_racing_kill)
{
    enum { ROUNDS = 30 };
    unsigned ran = 0;
    for (unsigned round = 0; round < ROUNDS; round++) {
        struct rp r;
        rp_new(&r, "rp-new");
        rp_start(&r);
        rp_thread(&r, h_spin, (int)other_cpu(round));
        rp_thread(&r, h_spin, (int)other_cpu(round + 1));
        rp_thread(&r, h_futex, -1);
        wait_word(r.odata, OD_READY, 1);
        rp_replace(&r, r.hn, AT_N(n_main), N_STACK_TOP, HANDLE_INVALID, 0);
        uint64_t until = uptime_ns() + round * 400000ull;   /* 0 .. 11.6 ms: a tick and more */
        while (uptime_ns() < until)
            __asm__ volatile("pause");
        process_kill(r.p, PROCESS_KILLED_CODE, true);
        struct process_info pi = rp_dead(r.p);
        KT_ASSERT(pi.killed);
        ran += vget(r.ndata, ND_UP) == DONE;
        /* Used up (DEAD), or never claimed and closed with p's table. */
        KT_ASSERT(info_of(r.n).state >= PROCESS_DYING);
        rp_end(&r);
    }
    kprintf("ktest replace_racing_kill: the new image ran in %u of %u rounds\n", ran, ROUNDS);
}
