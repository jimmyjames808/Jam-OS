/* TEST ONLY: ring-3 entry path (Track A of M5). Track B's aspace_map is
 * not available yet, so each test builds its own tiny user address space by
 * hand: a PML4 with the kernel half copied, a code page (machine code
 * written in below), a stack page, and sometimes a data page. A kernel
 * thread registers its page tables with the test CR3 hook, points CR3 at
 * them and enters ring 3 with arch_enter_user.
 *
 * User programs reach the kernel through test-only syscall numbers handled
 * by utest_syscall() (installed on uentry_test_syscall, consulted before
 * the real syscall_dispatch). Everything here compiles out with KTESTS=0. */
#include <jam/cpu.h>
#include <jam/ktest.h>
#include <jam/lapic.h>
#include <jam/kprintf.h>
#include <jam/mm.h>
#include <jam/panic.h>
#include <jam/percpu.h>
#include <jam/sched.h>
#include <jam/spinlock.h>
#include <jam/string.h>
#include <jam/syscall.h>
#include <jam/time.h>
#include <jam/uentry.h>
#include <jam/uentry_test.h>
#include <jam/usercopy.h>
#include <jam/x86.h>

/* ---- test syscall numbers (far above any real one) ------------------------ */
#define TSN_ADD    0x7001   /* return the sum of all six arguments */
#define TSN_REPORT 0x7002   /* prog->report[arg1] = arg0 */
#define TSN_YIELD  0x7003   /* reschedule, then return 0 */
#define TSN_EXIT   0x7004   /* leave ring 3 for good (thread_exit) */
#define TSN_GETCPU 0x7005   /* return this CPU's index */
#define TSN_BADRET 0x7006   /* set the return RIP >= USER_TOP, then return */

/* ---- a hand-built user address space -------------------------------------- */

#define UCODE  0x0000000000400000ull   /* code page */
#define USTACK 0x0000000000410000ull   /* stack page (grows down from top) */
#define UDATA  0x0000000000420000ull   /* scratch data page */

#define PTE_P 1ull
#define PTE_W 2ull
#define PTE_U 4ull
#define PTE_PS (1ull << 7)
#define PTE_ADDR 0x000ffffffffff000ull

struct uspace {
    uint64_t pml4;          /* physical */
    uint64_t code_phys, stack_phys, data_phys;
    void    *code, *stack, *data;   /* HHDM views */
};

struct uprog {
    volatile uint64_t report[8];
    volatile unsigned reported;
};

/* thread -> (page tables, program) so the CR3 hook and the syscall handlers
 * can find them from current_thread(). */
static struct {
    struct thread *t;
    uint64_t       pml4;
    struct uprog  *prog;
} regs[MAX_CPUS];
static spinlock_t reg_lock = SPINLOCK_INIT("utest regs");

static void reg_add(struct thread *t, uint64_t pml4, struct uprog *prog)
{
    uint64_t f = spin_lock_irqsave(&reg_lock);
    for (unsigned i = 0; i < MAX_CPUS; i++)
        if (!regs[i].t) {
            regs[i] = (typeof(regs[0])){ t, pml4, prog };
            spin_unlock_irqrestore(&reg_lock, f);
            return;
        }
    panic("utest: reg table full");
}
static void reg_del(struct thread *t)
{
    uint64_t f = spin_lock_irqsave(&reg_lock);
    for (unsigned i = 0; i < MAX_CPUS; i++)
        if (regs[i].t == t)
            regs[i].t = NULL;
    spin_unlock_irqrestore(&reg_lock, f);
}
static struct uprog *reg_prog(struct thread *t)
{
    for (unsigned i = 0; i < MAX_CPUS; i++)
        if (regs[i].t == t)
            return regs[i].prog;
    return NULL;
}
static uint64_t reg_pml4(struct thread *t)
{
    for (unsigned i = 0; i < MAX_CPUS; i++)
        if (regs[i].t == t)
            return regs[i].pml4;
    return 0;
}

/* The PML4 each CPU currently has loaded for a test thread (0 = kernel
 * tables). Set/cleared only inside the CR3 hook, which runs with interrupts
 * off; read by uspace_destroy to know when a PML4 is safe to free. */
static volatile uint64_t cpu_test_pml4[MAX_CPUS];

/* CR3 to load when switching TO `next` (see uentry_test.h). Tracks this
 * CPU's loaded test PML4 itself, so leaving a test thread always restores
 * the kernel tables even if a joiner already unregistered the old thread. */
static uint64_t utest_cr3(struct thread *next)
{
    uint32_t cpu = percpu_index();   /* stable: called with interrupts off */
    uint64_t pml4 = reg_pml4(next);
    if (pml4) {
        __atomic_store_n(&cpu_test_pml4[cpu], pml4, __ATOMIC_RELEASE);
        return pml4;
    }
    if (__atomic_load_n(&cpu_test_pml4[cpu], __ATOMIC_RELAXED)) {
        __atomic_store_n(&cpu_test_pml4[cpu], 0, __ATOMIC_RELEASE);
        return vmm_kernel_pml4();
    }
    return 0;
}

/* Load `pml4` on this CPU now and record it, with interrupts off so a
 * switch can't interleave. Used by a test thread once it has registered,
 * before its first entry to ring 3. */
static void utest_load_cr3(uint64_t pml4)
{
    uint64_t f = irq_save();
    __atomic_store_n(&cpu_test_pml4[percpu_index()], pml4, __ATOMIC_RELEASE);
    write_cr3(pml4);
    irq_restore(f);
}

static bool utest_syscall(struct syscall_frame *f, int64_t *ret)
{
    switch (f->nr) {
    case TSN_ADD:
        *ret = (int64_t)(f->args[0] + f->args[1] + f->args[2] + f->args[3] +
                         f->args[4] + f->args[5]);
        return true;
    case TSN_REPORT: {
        struct uprog *p = reg_prog(current_thread());
        if (p && f->args[1] < 8) {
            p->report[f->args[1]] = f->args[0];
            __atomic_add_fetch(&p->reported, 1, __ATOMIC_RELEASE);
        }
        *ret = 0;
        return true;
    }
    case TSN_YIELD:
        thread_yield();
        *ret = 0;
        return true;
    case TSN_GETCPU:
        *ret = percpu_index();
        return true;
    case TSN_BADRET:
        /* Corrupt the return RIP. The syscall exit guard must kill the
         * thread rather than let sysret fault in ring 0 on the user stack. */
        f->user_rip = USER_TOP + 0x1000;
        *ret = 0;
        return true;
    case TSN_EXIT:
        /* Stay registered until the joiner unregisters us: the CR3 hook
         * needs our entry to restore the kernel page tables when this CPU
         * switches to the next thread. */
        thread_exit();   /* phase 2: process exit; here just this thread */
    default:
        return false;
    }
}

static void utest_init(void)
{
    /* Install the test hooks once. The CR3 hook must be live before any
     * user thread is created; the syscall hook before it makes a call. */
    uentry_test_cr3 = utest_cr3;
    uentry_test_syscall = utest_syscall;
}

/* ---- page tables ---------------------------------------------------------- */

static uint64_t table_alloc(void)
{
    uint64_t pa = pmm_alloc_page_phys(PMM_ZERO);
    if (!pa)
        panic("utest: out of memory for page tables");
    return pa;
}

/* Map one 4 KiB user page (its own leaf tables created on the way). */
static void umap(struct uspace *u, uint64_t va, uint64_t pa, bool writable, bool exec)
{
    uint64_t *t = phys_to_virt(u->pml4);
    for (int level = 4; level > 1; level--) {
        uint64_t *e = &t[(va >> (12 + 9 * (level - 1))) & 511];
        if (!(*e & PTE_P))
            *e = table_alloc() | PTE_P | PTE_W | PTE_U;
        t = phys_to_virt(*e & PTE_ADDR);
    }
    uint64_t bits = PTE_P | PTE_U;
    if (writable)
        bits |= PTE_W;
    if (!exec && cpu_features.nx)
        bits |= (1ull << 63);
    t[(va >> 12) & 511] = pa | bits;
}

static void uspace_create(struct uspace *u)
{
    utest_init();
    memset(u, 0, sizeof(*u));
    u->pml4 = table_alloc();
    /* Share the kernel half (entries 256-511) with the running tables. */
    uint64_t *dst = phys_to_virt(u->pml4);
    uint64_t *src = phys_to_virt(vmm_kernel_pml4());
    for (int i = 256; i < 512; i++)
        dst[i] = src[i];

    u->code_phys = table_alloc();
    u->stack_phys = table_alloc();
    u->data_phys = table_alloc();
    u->code = phys_to_virt(u->code_phys);
    u->stack = phys_to_virt(u->stack_phys);
    u->data = phys_to_virt(u->data_phys);
    umap(u, UCODE, u->code_phys, false, true);    /* RX */
    umap(u, USTACK, u->stack_phys, true, false);  /* RW */
    umap(u, UDATA, u->data_phys, true, false);    /* RW */
}

/* Free the user-half page tables (entries 0-255) and the PML4. Leaf data
 * pages (code/stack/data) are freed by the caller; the level-1 tables that
 * point at them are freed here. */
static void free_tables(uint64_t phys, int level, bool user_half_only)
{
    if (level > 1) {
        uint64_t *t = phys_to_virt(phys);
        int last = user_half_only ? 256 : 512;
        for (int i = 0; i < last; i++)
            if ((t[i] & PTE_P) && !(t[i] & PTE_PS))
                free_tables(t[i] & PTE_ADDR, level - 1, false);
    }
    pmm_free_page_phys(phys);
}

static void uspace_destroy(struct uspace *u)
{
    /* A thread that just exited may still have this PML4 in CR3 until its
     * CPU switches away (the CR3 hook clears the record then). Freeing the
     * page tables under a live CR3 would corrupt that CPU, so wait. */
    for (;;) {
        bool in_use = false;
        for (uint32_t i = 0; i < cpu_count; i++)
            if (__atomic_load_n(&cpu_test_pml4[i], __ATOMIC_ACQUIRE) == u->pml4)
                in_use = true;
        if (!in_use)
            break;
        thread_yield();
    }
    free_tables(u->pml4, 4, true);
    pmm_free_page_phys(u->code_phys);
    pmm_free_page_phys(u->stack_phys);
    pmm_free_page_phys(u->data_phys);
}

/* ---- a tiny x86-64 emitter for the user programs -------------------------- */

struct emit { uint8_t *p, *end; };
static void eb(struct emit *e, uint8_t b)
{
    if (e->end && e->p >= e->end)
        panic("utest: user program overran its code page");
    *e->p++ = b;
}
/* mov <reg>, imm64. `ro` is the {REX, B8+rd} pair for the register. */
static void mov_imm(struct emit *e, const uint8_t ro[2], uint64_t imm)
{
    eb(e, ro[0]);
    eb(e, ro[1]);
    for (int i = 0; i < 8; i++)
        eb(e, (imm >> (8 * i)) & 0xff);
}
static const uint8_t R_RAX[2] = { 0x48, 0xb8 }, R_RCX[2] = { 0x48, 0xb9 };
static const uint8_t R_RDX[2] = { 0x48, 0xba }, R_RSI[2] = { 0x48, 0xbe };
static const uint8_t R_RDI[2] = { 0x48, 0xbf }, R_R8[2] = { 0x49, 0xb8 };
static const uint8_t R_R9[2] = { 0x49, 0xb9 }, R_R10[2] = { 0x49, 0xba };

static void e_syscall(struct emit *e) { eb(e, 0x0f); eb(e, 0x05); }
static void e_call_nr(struct emit *e, uint64_t nr) { mov_imm(e, R_RAX, nr); e_syscall(e); }
/* mov rdi, rax; mov rsi, slot; call TSN_REPORT (reports whatever is in rax). */
static void e_report_rax(struct emit *e, uint64_t slot)
{
    eb(e, 0x48); eb(e, 0x89); eb(e, 0xc7);   /* mov rdi, rax */
    mov_imm(e, R_RSI, slot);
    e_call_nr(e, TSN_REPORT);
}
/* movd r32, xmmN and movd xmmN, r32 use 66 0F 7E / 66 0F 6E, modrm C0|reg<<3
 * with rm = eax. */
static void e_movd_from_xmm(struct emit *e, unsigned x)
{
    eb(e, 0x66); eb(e, 0x0f); eb(e, 0x7e); eb(e, 0xc0 | (x << 3));
}
static void e_movd_to_xmm(struct emit *e, unsigned x)
{
    eb(e, 0x66); eb(e, 0x0f); eb(e, 0x6e); eb(e, 0xc0 | (x << 3));
}

/* ---- running a user thread ------------------------------------------------- */

struct urun {
    struct uspace *u;
    struct uprog  *prog;
    uint64_t       entry, arg0, arg1;
};

static void user_thread(void *arg)
{
    struct urun *r = arg;
    struct thread *t = current_thread();
    if (fpu_ustate_alloc(t) != OK)
        panic("utest: no FPU area");
    reg_add(t, r->u->pml4, r->prog);
    /* Point CR3 at the test tables now; the hook keeps them loaded across
     * any later switch and restores the kernel tables when we leave. */
    utest_load_cr3(r->u->pml4);
    arch_enter_user(r->entry, USTACK + PAGE_SIZE, r->arg0, r->arg1);
}

/* Spawn the user thread on `cpu`, wait for it to leave ring 3 (exit or a
 * fault kill), and clean up its registration. */
static struct thread *user_spawn(struct urun *r, uint32_t cpu)
{
    cpumask_t m;
    cpumask_one(&m, cpu);
    return thread_create_on("utest-user", user_thread, r, PRIO_DEFAULT, &m);
}

static void user_join(struct thread *t)
{
    thread_join(t);
    reg_del(t);
}

/* ---- tests ---------------------------------------------------------------- */

KTEST(uentry_syscall_roundtrip)
{
    struct uspace u;
    uspace_create(&u);
    struct uprog prog = { 0 };

    struct emit e = { u.code, (uint8_t *)u.code + PAGE_SIZE };
    /* six distinct args -> TSN_ADD -> report the sum, then the CPU, exit. */
    mov_imm(&e, R_RDI, 1);
    mov_imm(&e, R_RSI, 2);
    mov_imm(&e, R_RDX, 4);
    mov_imm(&e, R_R10, 8);       /* 4th arg is r10 in the syscall ABI */
    mov_imm(&e, R_R8, 16);
    mov_imm(&e, R_R9, 32);
    e_call_nr(&e, TSN_ADD);
    e_report_rax(&e, 0);         /* report[0] = 63 */
    e_call_nr(&e, TSN_GETCPU);
    e_report_rax(&e, 1);         /* report[1] = cpu index */
    e_call_nr(&e, TSN_EXIT);

    struct urun r = { &u, &prog, UCODE, 0, 0 };
    struct thread *t = user_spawn(&r, 1 % cpu_count);
    user_join(t);

    KT_EQ(prog.reported, 2);
    KT_EQ(prog.report[0], 63);
    KT_EQ(prog.report[1], 1 % cpu_count);
    uspace_destroy(&u);
}

/* Many syscalls from user threads on every CPU at once. */
#define NCALLS 40   /* keep the emitted code within one page */
static void loop_prog(struct emit *e)
{
    /* rbx counts; but we have no rbx imm helper: use rcx as counter via
     * repeated ADD calls with a fixed value and report the total. Simpler:
     * call TSN_ADD NCALLS times each returning 6, accumulate is hard without
     * scratch, so just make NCALLS calls and report the last CPU index. */
    for (int i = 0; i < NCALLS; i++) {
        mov_imm(e, R_RDI, 1);
        mov_imm(e, R_RSI, 1);
        mov_imm(e, R_RDX, 1);
        mov_imm(e, R_R10, 1);
        mov_imm(e, R_R8, 1);
        mov_imm(e, R_R9, 1);
        e_call_nr(e, TSN_ADD);   /* returns 6 each time */
    }
    e_report_rax(e, 0);          /* report[0] = 6 */
    e_call_nr(e, TSN_GETCPU);
    e_report_rax(e, 1);
    e_call_nr(e, TSN_EXIT);
}

KTEST(uentry_syscall_smp)
{
    enum { N = 8 };
    struct uspace u[N];
    struct uprog prog[N] = { 0 };
    struct urun r[N];
    struct thread *t[N];
    /* Each program is the same; NCALLS*~60 bytes fits in a 4 KiB page. */
    for (int i = 0; i < N; i++) {
        uspace_create(&u[i]);
        struct emit e = { u[i].code, (uint8_t *)u[i].code + PAGE_SIZE };
        loop_prog(&e);
        r[i] = (struct urun){ &u[i], &prog[i], UCODE, 0, 0 };
        t[i] = user_spawn(&r[i], i % cpu_count);
    }
    for (int i = 0; i < N; i++) {
        user_join(t[i]);
        KT_EQ(prog[i].reported, 2);
        KT_EQ(prog[i].report[0], 6);
        KT_EQ(prog[i].report[1], i % cpu_count);
        uspace_destroy(&u[i]);
    }
}

/* A user busy-loop is preempted by the timer and resumes, and thread_cancel
 * stops it at its next return to user mode (the M5 kill mechanism). */
KTEST(uentry_preempt_and_cancel)
{
    if (cpu_count < 2)
        return;   /* needs a CPU to watch from */
    struct uspace u;
    uspace_create(&u);
    struct uprog prog = { 0 };

    /* mov rcx, &counter ; L: inc qword [rcx] ; jmp L  (never leaves ring 3) */
    struct emit e = { u.code, (uint8_t *)u.code + PAGE_SIZE };
    mov_imm(&e, R_RCX, UDATA);
    uint8_t *loop = e.p;
    eb(&e, 0x48); eb(&e, 0xff); eb(&e, 0x01);   /* inc qword [rcx] */
    eb(&e, 0xeb); eb(&e, (uint8_t)(loop - (e.p + 1)));   /* jmp loop */

    volatile uint64_t *counter = u.data;
    *counter = 0;
    struct urun r = { &u, &prog, UCODE, 0, 0 };
    struct thread *t = user_spawn(&r, 1);

    thread_sleep_ms(20);
    uint64_t a = *counter;
    thread_sleep_ms(20);
    uint64_t b = *counter;
    KT_ASSERT(a > 0);    /* it ran in ring 3 */
    KT_ASSERT(b > a);    /* and was preempted and resumed (we got CPU back, it kept going) */

    thread_cancel(t);    /* stops at the next timer tick's return-to-user */
    user_join(t);        /* would hang if cancel didn't take */
    uspace_destroy(&u);
}

/* Two user threads on ONE CPU with distinct SSE registers: each writes
 * xmm0-3, yields to the other, and must read its own values back. */
KTEST(uentry_fpu_switch)
{
    if (cpu_count < 2)
        return;
    enum { N = 2 };
    struct uspace u[N];
    struct uprog prog[N] = { 0 };
    struct urun r[N];
    struct thread *t[N];
    uint32_t base[N] = { 0x11110000, 0x22220000 };

    for (int i = 0; i < N; i++) {
        uspace_create(&u[i]);
        struct emit e = { u[i].code, (uint8_t *)u[i].code + PAGE_SIZE };
        for (unsigned x = 0; x < 4; x++) {
            mov_imm(&e, R_RAX, base[i] + x);
            e_movd_to_xmm(&e, x);
        }
        e_call_nr(&e, TSN_YIELD);   /* hand the CPU to the other thread */
        e_call_nr(&e, TSN_YIELD);
        for (unsigned x = 0; x < 4; x++) {
            e_movd_from_xmm(&e, x);   /* eax = xmm[x] low dword */
            e_report_rax(&e, x);
        }
        e_call_nr(&e, TSN_EXIT);
        r[i] = (struct urun){ &u[i], &prog[i], UCODE, 0, 0 };
        /* Both pinned to CPU 1 so they interleave through the yields. */
        t[i] = user_spawn(&r[i], 1);
    }
    for (int i = 0; i < N; i++)
        user_join(t[i]);
    for (int i = 0; i < N; i++) {
        KT_EQ(prog[i].reported, 4);
        for (unsigned x = 0; x < 4; x++)
            KT_EQ(prog[i].report[x], base[i] + x);
        uspace_destroy(&u[i]);
    }
}

/* A user #UD / #GP / NULL read kills only that thread; the kernel lives. */
static void fault_case(void (*build)(struct emit *e))
{
    struct uspace u;
    uspace_create(&u);
    struct uprog prog = { 0 };
    struct emit e = { u.code, (uint8_t *)u.code + PAGE_SIZE };
    build(&e);
    struct urun r = { &u, &prog, UCODE, 0, 0 };
    struct thread *t = user_spawn(&r, 1 % cpu_count);
    user_join(t);   /* the fault kill calls thread_exit, so the join returns */
    uspace_destroy(&u);
}
static void build_ud(struct emit *e) { eb(e, 0x0f); eb(e, 0x0b); }   /* ud2 */
static void build_gp(struct emit *e)
{
    /* wrmsr in ring 3 is #GP (privileged). */
    mov_imm(e, R_RCX, MSR_EFER);
    eb(e, 0x0f); eb(e, 0x30);
}
static void build_null(struct emit *e)
{
    eb(e, 0x48); eb(e, 0x8b); eb(e, 0x04); eb(e, 0x25);   /* mov rax, [disp32] */
    eb(e, 0); eb(e, 0); eb(e, 0); eb(e, 0);               /* address 0 */
}

KTEST(uentry_user_ud)  { fault_case(build_ud); }
KTEST(uentry_user_gp)  { fault_case(build_gp); }
KTEST(uentry_user_null){ fault_case(build_null); }

/* A user thread whose sysret RIP is set out of range must not fault in ring
 * 0: it is killed instead. TSN_EXIT-less program that returns past its code
 * into the unmapped guard is one case; here we jump to USER_TOP directly. */
KTEST(uentry_bad_return)
{
    struct uspace u;
    uspace_create(&u);
    struct uprog prog = { 0 };
    struct emit e = { u.code, (uint8_t *)u.code + PAGE_SIZE };
    /* mov rax, USER_TOP ; jmp rax  -> instruction fetch fault at USER_TOP,
     * which is a user-mode #PF (killed), exercising the fetch path; the
     * sysret-RIP guard is covered by the syscall exit check in uentry.c. */
    mov_imm(&e, R_RAX, USER_TOP);
    eb(&e, 0xff); eb(&e, 0xe0);   /* jmp rax */
    struct urun r = { &u, &prog, UCODE, 0, 0 };
    struct thread *t = user_spawn(&r, 1 % cpu_count);
    user_join(t);
    uspace_destroy(&u);
}

/* ---- user copies ---------------------------------------------------------- */

/* copy_from_user/copy_to_user must run under the target address space. We
 * borrow a user address space by pointing this kernel thread's CR3 at it
 * (registered with the hook) without ever entering ring 3. */
struct copyctx {
    struct uspace *u;
    volatile status_t r_good, r_unmapped, r_kernel, r_noncanon, r_crosstop, r_tostack;
    volatile bool     data_ok;
    volatile status_t r_str_ok, r_str_nonul, r_str_bad;
    volatile bool     str_ok;
};

static void copy_thread(void *arg)
{
    struct copyctx *c = arg;
    struct uspace *u = c->u;
    reg_add(current_thread(), u->pml4, NULL);
    utest_load_cr3(u->pml4);

    /* Seed the user data page (through its HHDM alias) and read it back
     * with copy_from_user from the user VA. */
    char *seed = u->data;
    for (int i = 0; i < 64; i++)
        seed[i] = (char)(i + 1);
    char buf[64];
    c->r_good = copy_from_user(buf, UDATA, 64);
    c->data_ok = memcmp(buf, seed, 64) == 0;

    /* An unmapped user page: fixup returns ERR_INVALID_ARGS, no panic. */
    c->r_unmapped = copy_from_user(buf, UDATA + PAGE_SIZE, 8);
    /* A kernel address: rejected by the range check. */
    c->r_kernel = copy_from_user(buf, 0xffff800000000000ull, 8);
    /* A non-canonical address. */
    c->r_noncanon = copy_from_user(buf, 0x8000000000000000ull, 8);
    /* A range that starts in the user page but crosses USER_TOP. */
    c->r_crosstop = copy_from_user(buf, USER_TOP - 4, 64);
    /* copy_to_user into the user stack page (writable). */
    c->r_tostack = copy_to_user(USTACK, buf, 64);

    /* strings: a good NUL-terminated one, one with no NUL in range, and a
     * bad pointer. */
    memcpy(seed, "hello", 6);
    char sbuf[16];
    size_t len = 0;
    c->r_str_ok = copy_str_from_user(sbuf, UDATA, sizeof(sbuf), &len);
    c->str_ok = c->r_str_ok == OK && len == 5 && strcmp(sbuf, "hello") == 0;
    memset(seed, 'x', 64);   /* no NUL in the first 16 bytes */
    c->r_str_nonul = copy_str_from_user(sbuf, UDATA, sizeof(sbuf), &len);
    c->r_str_bad = copy_str_from_user(sbuf, UDATA + PAGE_SIZE, sizeof(sbuf), &len);

    thread_exit();   /* the joiner unregisters us (see TSN_EXIT) */
}

KTEST(uentry_user_copies)
{
    struct uspace u;
    uspace_create(&u);
    static struct copyctx c;
    memset(&c, 0, sizeof(c));
    c.u = &u;
    cpumask_t m;
    cpumask_one(&m, 1 % cpu_count);
    struct thread *t = thread_create_on("utest-copy", copy_thread, &c, PRIO_DEFAULT, &m);
    thread_join(t);
    reg_del(t);

    KT_EQ(c.r_good, OK);
    KT_ASSERT(c.data_ok);
    KT_EQ(c.r_unmapped, ERR_INVALID_ARGS);
    KT_EQ(c.r_kernel, ERR_INVALID_ARGS);
    KT_EQ(c.r_noncanon, ERR_INVALID_ARGS);
    KT_EQ(c.r_crosstop, ERR_INVALID_ARGS);
    KT_EQ(c.r_tostack, OK);
    KT_ASSERT(c.str_ok);
    KT_EQ(c.r_str_nonul, ERR_OUT_OF_RANGE);
    KT_EQ(c.r_str_bad, ERR_INVALID_ARGS);
    uspace_destroy(&u);
}

/* A syscall whose return RIP has been set >= USER_TOP must kill the thread,
 * not sysret to a bad address (which would #GP in ring 0 on the user stack
 * and panic the kernel). If the guard failed, this test would panic. */
KTEST(uentry_sysret_guard)
{
    struct uspace u;
    uspace_create(&u);
    struct uprog prog = { 0 };
    struct emit e = { u.code, (uint8_t *)u.code + PAGE_SIZE };
    e_call_nr(&e, TSN_BADRET);   /* the handler corrupts the return RIP */
    e_call_nr(&e, TSN_EXIT);     /* never reached: the guard kills us first */
    struct urun r = { &u, &prog, UCODE, 0, 0 };
    struct thread *t = user_spawn(&r, 1 % cpu_count);
    user_join(t);
    uspace_destroy(&u);
}

/* An NMI arriving while a thread runs in ring 3 must be handled (GS found
 * correctly by the IST entry) and the thread must keep running. */
static volatile unsigned nmi_seen;
static bool count_nmi(struct trap_frame *f)
{
    (void)f;
    __atomic_add_fetch(&nmi_seen, 1, __ATOMIC_RELAXED);
    return true;   /* swallow it: nothing is actually wrong */
}

KTEST(uentry_nmi_in_user)
{
    if (cpu_count < 2)
        return;
    struct uspace u;
    uspace_create(&u);
    struct uprog prog = { 0 };
    /* mov rcx, &counter ; L: inc [rcx] ; jmp L */
    struct emit e = { u.code, (uint8_t *)u.code + PAGE_SIZE };
    mov_imm(&e, R_RCX, UDATA);
    uint8_t *loop = e.p;
    eb(&e, 0x48); eb(&e, 0xff); eb(&e, 0x01);
    eb(&e, 0xeb); eb(&e, (uint8_t)(loop - (e.p + 1)));

    volatile uint64_t *counter = u.data;
    *counter = 0;
    nmi_seen = 0;
    uentry_test_nmi = count_nmi;

    struct urun r = { &u, &prog, UCODE, 0, 0 };
    struct thread *t = user_spawn(&r, 1);
    while (*counter == 0)
        thread_yield();   /* wait until it is looping in ring 3 */

    for (int i = 0; i < 5; i++) {
        lapic_send_nmi(cpus[1]->lapic_id);
        thread_sleep_ms(2);
    }
    uint64_t a = *counter;
    thread_sleep_ms(10);
    KT_ASSERT(nmi_seen > 0);      /* the NMIs were delivered and handled */
    KT_ASSERT(*counter > a);      /* and the user thread survived them */

    thread_cancel(t);
    user_join(t);
    uentry_test_nmi = NULL;
    uspace_destroy(&u);
}
