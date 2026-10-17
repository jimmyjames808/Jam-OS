/* Fault upcalls (arch/x86_64/upcall.c, docs/SIGNALS-PLAN.md section 1).
 *
 * Real processes (a job each, the address space process_create made,
 * threads started with process_start) run small ring-3 programs assembled
 * here (upc_blob, copied to U_CODE). Each program sets its FS base and,
 * unless the test says not to, its exception entry; it sets every
 * register to a pattern and faults. The entry records what it was given
 * in the data page (U_DATA) and jumps to the test's action (D_ACTION):
 * resume past the fault, edit or spoil the frame first, say UNHANDLED,
 * leave the window, fault again. The test reads the data page, the frame
 * (still on the stack VMO) and the kernel log afterwards. */
#include <stddef.h>
#include <jam/abi.h>
#include <jam/aspace.h>
#include <jam/cpu.h>
#include <jam/handle.h>
#include <jam/klog.h>
#include <jam/kprintf.h>
#include <jam/ktest.h>
#include <jam/mm.h>
#include <jam/object.h>
#include <jam/pager.h>
#include <jam/percpu.h>
#include <jam/process.h>
#include <jam/resource.h>
#include <jam/sched.h>
#include <jam/sha256.h>
#include <jam/string.h>
#include <jam/sys.h>
#include <jam/syscall_impl.h>
#include <jam/syscall_nums.h>
#include <jam/time.h>
#include <jam/uentry.h>
#include <jam/vmo.h>
#include <jam/x86.h>

extern uint64_t user_faults;   /* uentry.c: user threads killed for a fault */

#define PG 4096ull

/* The address space (all in one 2 MiB stretch: one page table). */
#define U_CODE      0x10000000   /* the programs (read, execute), CODE_PAGES */
#define CODE_PAGES  2
#define U_STACK     0x10010000   /* the stack (read, write), STACK_PAGES */
#define STACK_PAGES 16
#define U_STACK_TOP (U_STACK + STACK_PAGES * PG)
#define U_DATA      0x10030000   /* the programs' and the test's words (read, write) */
#define DATA_PAGES  16
#define U_MORE      0x10040000   /* a test's own: read-only, shrunk, a pager's... */
#define U_HOLE      0x10080000   /* nothing, ever */

/* The data page, offsets from U_DATA. */
#define D_ACTION    0x000   /* the entry's action (an address in U_CODE) */
#define D_SKIP      0x008   /* act_skip: bytes past the frame's RIP */
#define D_COUNT     0x010   /* entries so far */
#define D_FRAME     0x018   /* RDI at the last entry */
#define D_EFLAGS    0x020   /* RFLAGS at the entry */
#define D_EMXCSR    0x028   /* MXCSR at the entry (4 bytes) */
#define D_EFCW      0x030   /* the x87 control word at the entry (2 bytes) */
#define D_EXMM0     0x038   /* xmm0's low half at the entry */
#define D_EYMM0     0x040   /* ymm0's upper half's low word at the entry (AVX) */
#define D_EFS       0x048   /* %fs:0 at the entry */
#define D_FAULTER   0x050   /* upc_generic: the faulting code */
#define D_DONE      0x058   /* upc_generic: DONE once past the fault */
#define D_RET       0x060   /* calls' results a program kept */
#define D_RET2      0x068
#define D_RET3      0x070
#define D_RET4      0x078
#define D_AVX       0x080   /* 1: the program may use AVX */
#define D_NOENTRY   0x088   /* 1: upc_setup sets no entry */
#define D_ARG       0x090   /* a test's value (act_poke, act_xor, f_load, upc_nostack) */
#define D_ARGOFF    0x098   /* act_poke, act_xor: the frame offset */
#define D_AVXOFF    0x0a0   /* AVX's upper halves in the XSAVE area */
#define D_MXCSR_IN  0x0a8   /* upc_regs: its MXCSR */
#define D_FCW_IN    0x0b0   /* f_mf: its control word */
#define D_MXCSR_XM  0x0b8   /* f_xm: its MXCSR */
#define D_ONE       0x0c0   /* 1.0f */
#define D_ZERO      0x0c8   /* 0.0 (a double) */
#define D_NX        0x0d0   /* f_pf_nx: where it jumps */
#define D_READY     0x0d8   /* upc_oom: waiting for D_GO */
#define D_GO        0x0e0
#define D_ROUNDS    0x0e8   /* upc_spin: rounds per thread */
#define D_THREADS   0x0f0   /* upc_spin: threads */
#define D_FINISHED  0x0f8   /* upc_spin: threads done */
#define D_EREGS     0x100   /* the 16 registers at the entry, in the frame's order */
#define D_AREGS     0x180   /* the 16 registers after the resume */
#define D_AFLAGS    0x200   /* RFLAGS after the resume */
#define D_ACS       0x208   /* CS after (2 bytes) */
#define D_ASS       0x210   /* SS after (2 bytes) */
#define D_AMXCSR    0x218   /* MXCSR after (4 bytes) */
#define D_BAD       0x220   /* upc_spin: frames or registers that weren't the thread's */
#define D_ERIP      0x230   /* the frame's RIP at the entry (before the action) */
#define D_ERFL      0x238   /* the frame's RFLAGS at the entry */
#define D_COOKIES   0x240   /* upc_spin: each thread's cookie, as its frames said (8) */
#define D_VPAT      0x300   /* upc_regs: the vector registers it loads, 32 bytes each (16) */
#define D_AVEC      0x500   /* upc_regs: the vector registers after the resume (16) */
#define D_FSWORD    0x800   /* the FS base points here */
#define D_WARM      0x808   /* upc_bench: rounds before it times (at least 1) */
#define D_ESTACK    0x810   /* upc_setup: the exception stack's base and size (0: none) */
#define D_ESIZE     0x818
#define D_ESRET     0x820   /* upc_setup: thread_exception_stack's result */
#define D_RCOUNT    0x828   /* act_raised, upc_rbentry: raises taken */
#define D_RKIND     0x830   /* act_raised: the frame's kind */
#define D_RTSC      0x838   /* act_raised: the TSC at its entry */
#define D_LOOPS     0x840   /* upc_rspin: rounds of its loop */
#define D_HSELF     0x848   /* a handle to thread t0 with RIGHT_SIGNAL */
#define D_HNOSIG    0x850   /* one without */
#define D_TSC0      0x858   /* upc_rbench: the TSC before its raise */
#define D_STOP      0x860   /* upc_rstorm: stop */
#define D_RCOUNTS   0x880   /* upc_rstorm: each thread's raises taken (8) */
#define D_RNG       0x900   /* upc_hrand: its random state (xorshift64) */
#define D_HWANT     0x908   /* upc_hrand: frames to give RESUME, then exit 0 */
#define D_HFRAMES   0x910   /* upc_hrand: frames given so far */
#define D_HLEAK     0x918   /* upc_hrand: words of a witness's pattern seen in its frames */
#define D_HBAD      0x920   /* upc_hrand: resumes whose registers weren't the frame's */
#define D_HREFUSED  0x928   /* upc_hrand: RESUMEs that came back (an unreadable frame) */
#define D_HLANDED   0x930   /* upc_hrand: resumes that landed and were checked */
#define D_EXPFL     0x938   /* upc_hrand: RFLAGS the landing must see */
#define D_EXPX0     0x940   /* upc_hrand: xmm0's low half the landing must see */
#define D_VFAULTS   0x948   /* upc_vspin: faults taken with the vector expected */
#define D_EXPECT    0x980   /* upc_hrand: the 16 registers the landing must see */
#define D_SAMPLES   0x1000  /* upc_bench: each timed round's TSC cycles */
#define SAMPLES_MAX ((DATA_PAGES * PG - D_SAMPLES) / 8)

#define DONE 0x600d

/* The frame's offsets the programs use (abi.h's struct exc_frame). */
#define F_SIZE     0
#define F_KIND     4
#define F_VECTOR   8
#define F_COOKIE   32
#define F_XSIZE    40
#define F_R8       48
#define F_R12      80
#define F_R13      88
#define F_R14      96
#define F_R15      104
#define F_RBX      136
#define F_RSP      168
#define F_RIP      176
#define F_RFLAGS   184
#define F_X        192
_Static_assert(offsetof(struct exc_frame, kind) == F_KIND &&
                   offsetof(struct exc_frame, vector) == F_VECTOR &&
                   offsetof(struct exc_frame, r8) == F_R8 &&
                   offsetof(struct exc_frame, r13) == F_R13 &&
                   offsetof(struct exc_frame, r14) == F_R14 &&
                   offsetof(struct exc_frame, r15) == F_R15 &&
                   offsetof(struct exc_frame, cookie) == F_COOKIE &&
                   offsetof(struct exc_frame, xsave_size) == F_XSIZE &&
                   offsetof(struct exc_frame, r12) == F_R12 &&
                   offsetof(struct exc_frame, rbx) == F_RBX &&
                   offsetof(struct exc_frame, rsp) == F_RSP &&
                   offsetof(struct exc_frame, rip) == F_RIP &&
                   offsetof(struct exc_frame, rflags) == F_RFLAGS && EXC_FRAME_XSAVE == F_X,
               "the programs' frame offsets");
#define X_XMM     160   /* in the XSAVE area */
#define X_MXCSR   24
#define X_BV      512
#define X_COMP    520

/* upc_regs's RFLAGS: CF PF AF ZF SF IF DF OF AC and bit 1. */
#define FLAGS_PAT 0x40ed7

/* exception_done's `how` and USER_TOP as the assembler takes them (no C
 * suffixes). */
#define H_RESUME    1
#define H_LEAVE     2
#define H_UNHANDLED 3
#define U_TOP       0x00007fffffffe000
#define K_RAISED    2
#define U_FLAGS_OK  0x240dd5
#define H_SP        0x1001ffc0   /* upc_hrand's stack, put back at each round */
_Static_assert(H_RESUME == EXC_RESUME && H_LEAVE == EXC_LEAVE && H_UNHANDLED == EXC_UNHANDLED &&
                   U_TOP == USER_TOP && K_RAISED == EXC_RAISED && U_FLAGS_OK == USER_RFLAGS_OK &&
                   H_SP == U_STACK_TOP - 64,
               "the programs' constants");

#define S_(x) #x
#define S(x) S_(x)
#define DSET(name, off) ".set " #name ", " S(U_DATA) " + " S(off) "\n"

/* The programs. Position-independent: they reach their own code
 * RIP-relative and the data page by its absolute address. */
__asm__(
    ".pushsection .rodata.upcall_progs, \"a\"\n"
    ".balign 64\n"
    DSET(d_action, D_ACTION) DSET(d_skip, D_SKIP) DSET(d_count, D_COUNT)
    DSET(d_frame, D_FRAME) DSET(d_eflags, D_EFLAGS) DSET(d_emxcsr, D_EMXCSR)
    DSET(d_efcw, D_EFCW) DSET(d_exmm0, D_EXMM0) DSET(d_eymm0, D_EYMM0) DSET(d_efs, D_EFS)
    DSET(d_faulter, D_FAULTER) DSET(d_done, D_DONE) DSET(d_ret, D_RET) DSET(d_ret2, D_RET2)
    DSET(d_ret3, D_RET3) DSET(d_ret4, D_RET4) DSET(d_avx, D_AVX) DSET(d_noentry, D_NOENTRY)
    DSET(d_arg, D_ARG) DSET(d_argoff, D_ARGOFF) DSET(d_avxoff, D_AVXOFF)
    DSET(d_mxcsr_in, D_MXCSR_IN) DSET(d_fcw_in, D_FCW_IN) DSET(d_mxcsr_xm, D_MXCSR_XM)
    DSET(d_one, D_ONE) DSET(d_zero, D_ZERO) DSET(d_nx, D_NX) DSET(d_ready, D_READY)
    DSET(d_go, D_GO) DSET(d_rounds, D_ROUNDS) DSET(d_threads, D_THREADS)
    DSET(d_finished, D_FINISHED) DSET(d_eregs, D_EREGS) DSET(d_aregs, D_AREGS)
    DSET(d_aflags, D_AFLAGS) DSET(d_acs, D_ACS) DSET(d_ass, D_ASS) DSET(d_amxcsr, D_AMXCSR)
    DSET(d_bad, D_BAD) DSET(d_erip, D_ERIP) DSET(d_erfl, D_ERFL) DSET(d_cookies, D_COOKIES) DSET(d_vpat, D_VPAT) DSET(d_avec, D_AVEC)
    DSET(d_fsword, D_FSWORD) DSET(d_warm, D_WARM) DSET(d_samples, D_SAMPLES)
    DSET(d_estack, D_ESTACK) DSET(d_esize, D_ESIZE) DSET(d_esret, D_ESRET)
    DSET(d_rcount, D_RCOUNT) DSET(d_rkind, D_RKIND) DSET(d_rtsc, D_RTSC) DSET(d_loops, D_LOOPS)
    DSET(d_hself, D_HSELF) DSET(d_hnosig, D_HNOSIG) DSET(d_tsc0, D_TSC0) DSET(d_stop, D_STOP)
    DSET(d_rcounts, D_RCOUNTS) DSET(d_rng, D_RNG) DSET(d_hwant, D_HWANT)
    DSET(d_hframes, D_HFRAMES) DSET(d_hleak, D_HLEAK) DSET(d_hbad, D_HBAD)
    DSET(d_hrefused, D_HREFUSED) DSET(d_hlanded, D_HLANDED) DSET(d_expfl, D_EXPFL)
    DSET(d_expx0, D_EXPX0) DSET(d_vfaults, D_VFAULTS) DSET(d_expect, D_EXPECT)
    ".set u_more, " S(U_MORE) "\n"
    ".set u_hole, " S(U_HOLE) "\n"
    "upc_blob:\n"

    /* ---- the entry: what it was given, then the action -------------- */
    "upc_entry:\n"
    "    movq %r8, d_eregs + 0\n"
    "    movq %r9, d_eregs + 8\n"
    "    movq %r10, d_eregs + 16\n"
    "    movq %r11, d_eregs + 24\n"
    "    movq %r12, d_eregs + 32\n"
    "    movq %r13, d_eregs + 40\n"
    "    movq %r14, d_eregs + 48\n"
    "    movq %r15, d_eregs + 56\n"
    "    movq %rdi, d_eregs + 64\n"
    "    movq %rsi, d_eregs + 72\n"
    "    movq %rbp, d_eregs + 80\n"
    "    movq %rbx, d_eregs + 88\n"
    "    movq %rdx, d_eregs + 96\n"
    "    movq %rax, d_eregs + 104\n"
    "    movq %rcx, d_eregs + 112\n"
    "    movq %rsp, d_eregs + 120\n"
    "    movq %rdi, d_frame\n"
    "    pushfq\n"
    "    popq d_eflags\n"
    "    stmxcsr d_emxcsr\n"
    "    fnstcw d_efcw\n"
    "    movq %xmm0, d_exmm0\n"
    "    cmpq $0, d_avx\n"
    "    je 1f\n"
    "    vextractf128 $1, %ymm0, %xmm1\n"
    "    movq %xmm1, d_eymm0\n"
    "1:  movq %fs:0, %rax\n"
    "    movq %rax, d_efs\n"
    "    movq " S(F_RIP) "(%rdi), %rax\n"
    "    movq %rax, d_erip\n"
    "    movq " S(F_RFLAGS) "(%rdi), %rax\n"
    "    movq %rax, d_erfl\n"
    "    lock incq d_count\n"
    "    jmp *d_action\n"

    /* ---- the actions (rdi = the frame) ------------------------------- */
    "act_skip:\n"                       /* past the faulting instruction */
    "    movq d_skip, %rax\n"
    "    addq %rax, " S(F_RIP) "(%rdi)\n"
    "act_resume:\n"
    "    movl $" S(H_RESUME) ", %esi\n"
    "    movl $" S(SYS_exception_done) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret\n"            /* came back: an error */
    "    movl $99, %edi\n"
    "    jmp upc_exit\n"
    "act_exit:\n"                       /* the frame stays for the test */
    "    movl $42, %edi\n"
    "    jmp upc_exit\n"
    "act_unhandled:\n"
    "    movl $" S(H_UNHANDLED) ", %esi\n"
    "    movl $" S(SYS_exception_done) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret\n"
    "    movl $98, %edi\n"
    "    jmp upc_exit\n"
    "act_again:\n"                      /* a fault inside the window */
    "    ud2\n"
    "act_leave:\n"                      /* the window's end, then a fault of the handler's */
    "    cmpq $2, d_count\n"
    "    je 1f\n"
    "    movl $" S(H_LEAVE) ", %esi\n"
    "    movl $" S(SYS_exception_done) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret\n"
    "    movl $" S(H_LEAVE) ", %esi\n"
    "    movl $" S(SYS_exception_done) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret2\n"
    "    movl $" S(H_UNHANDLED) ", %esi\n"
    "    movl $" S(SYS_exception_done) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret3\n"
    "    movl $99, %esi\n"
    "    movl $" S(SYS_exception_done) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret4\n"
    "    ud2\n"
    "1:  movl $44, %edi\n"
    "    jmp upc_exit\n"
    "act_edit:\n"                       /* rbx, xmm2 (and ymm2's upper half), then on */
    "    movabsq $0x0123456789abcdef, %rax\n"
    "    movq %rax, " S(F_RBX) "(%rdi)\n"
    "    movq %rax, " S(F_X) " + " S(X_XMM) " + 32(%rdi)\n"
    "    notq %rax\n"
    "    movq %rax, " S(F_X) " + " S(X_XMM) " + 40(%rdi)\n"
    "    cmpq $0, d_avx\n"
    "    je act_skip\n"
    "    movq d_avxoff, %rcx\n"
    "    addq %rdi, %rcx\n"
    "    movq %rax, " S(F_X) " + 32(%rcx)\n"
    "    jmp act_skip\n"
    "act_flags:\n"                      /* IOPL, NT, RF, VM, VIF, VIP on, IF off */
    "    orq $0x1b7000, " S(F_RFLAGS) "(%rdi)\n"
    "    andq $-513, " S(F_RFLAGS) "(%rdi)\n"
    "    jmp act_skip\n"
    "act_untf:\n"                       /* single-stepping: stop */
    "    andq $-257, " S(F_RFLAGS) "(%rdi)\n"
    "    jmp act_resume\n"
    "act_poke:\n"                       /* frame[D_ARGOFF] = D_ARG */
    "    movq d_argoff, %rcx\n"
    "    movq d_arg, %rax\n"
    "    movq %rax, (%rdi,%rcx)\n"
    "    jmp act_resume\n"
    "act_xor:\n"                        /* frame[D_ARGOFF] ^= D_ARG, past the instruction */
    "    movq d_argoff, %rcx\n"
    "    movq d_arg, %rax\n"
    "    xorq %rax, (%rdi,%rcx)\n"
    "    jmp act_skip\n"

    /* ---- common pieces ----------------------------------------------- */
    "upc_setup:\n"                      /* the FS base; the entry unless D_NOENTRY; */
    "    movq $d_fsword, %rdi\n"        /* the exception stack if D_ESIZE */
    "    movl $" S(SYS_thread_set_fs_base) ", %eax\n"
    "    syscall\n"
    "    cmpq $0, d_noentry\n"
    "    jne 1f\n"
    "    leaq upc_entry(%rip), %rdi\n"
    "    movl $" S(SYS_exception_entry_set) ", %eax\n"
    "    syscall\n"
    "1:  cmpq $0, d_esize\n"
    "    je 2f\n"
    "    movq d_estack, %rdi\n"
    "    movq d_esize, %rsi\n"
    "    movl $" S(SYS_thread_exception_stack) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_esret\n"
    "2:  ret\n"
    "upc_exit:\n"                       /* edi = the exit code */
    "    movl $" S(SYS_process_exit) ", %eax\n"
    "    syscall\n"
    "    ud2\n"
    "upc_pattern:\n"                    /* every register but rsp to its pattern */
    "    movabsq $0x1111111111111111, %r8\n"
    "    movabsq $0x2222222222222222, %r9\n"
    "    movabsq $0x3333333333333333, %r10\n"
    "    movabsq $0x4444444444444444, %r11\n"
    "    movabsq $0x5555555555555555, %r12\n"
    "    movabsq $0x6666666666666666, %r13\n"
    "    movabsq $0x7777777777777777, %r14\n"
    "    movabsq $0x8888888888888888, %r15\n"
    "    movabsq $0x9999999999999999, %rdi\n"
    "    movabsq $0xaaaaaaaaaaaaaaaa, %rsi\n"
    "    movabsq $0xbbbbbbbbbbbbbbbb, %rbp\n"
    "    movabsq $0xcccccccccccccccc, %rbx\n"
    "    movabsq $0xdddddddddddddddd, %rdx\n"
    "    movabsq $0xeeeeeeeeeeeeeeee, %rax\n"
    "    movabsq $0xffffffffffffffff, %rcx\n"
    "    ret\n"

    /* ---- upc_regs: every register through a fault and a resume ------- */
    "upc_regs:\n"
    "    call upc_setup\n"
    "    ldmxcsr d_mxcsr_in\n"
    "    cmpq $0, d_avx\n"
    "    je 1f\n"
    "    vmovdqu d_vpat + 0, %ymm0\n"
    "    vmovdqu d_vpat + 32, %ymm1\n"
    "    vmovdqu d_vpat + 64, %ymm2\n"
    "    vmovdqu d_vpat + 96, %ymm3\n"
    "    vmovdqu d_vpat + 128, %ymm4\n"
    "    vmovdqu d_vpat + 160, %ymm5\n"
    "    vmovdqu d_vpat + 192, %ymm6\n"
    "    vmovdqu d_vpat + 224, %ymm7\n"
    "    vmovdqu d_vpat + 256, %ymm8\n"
    "    vmovdqu d_vpat + 288, %ymm9\n"
    "    vmovdqu d_vpat + 320, %ymm10\n"
    "    vmovdqu d_vpat + 352, %ymm11\n"
    "    vmovdqu d_vpat + 384, %ymm12\n"
    "    vmovdqu d_vpat + 416, %ymm13\n"
    "    vmovdqu d_vpat + 448, %ymm14\n"
    "    vmovdqu d_vpat + 480, %ymm15\n"
    "    jmp 2f\n"
    "1:  movdqu d_vpat + 0, %xmm0\n"
    "    movdqu d_vpat + 32, %xmm1\n"
    "    movdqu d_vpat + 64, %xmm2\n"
    "    movdqu d_vpat + 96, %xmm3\n"
    "    movdqu d_vpat + 128, %xmm4\n"
    "    movdqu d_vpat + 160, %xmm5\n"
    "    movdqu d_vpat + 192, %xmm6\n"
    "    movdqu d_vpat + 224, %xmm7\n"
    "    movdqu d_vpat + 256, %xmm8\n"
    "    movdqu d_vpat + 288, %xmm9\n"
    "    movdqu d_vpat + 320, %xmm10\n"
    "    movdqu d_vpat + 352, %xmm11\n"
    "    movdqu d_vpat + 384, %xmm12\n"
    "    movdqu d_vpat + 416, %xmm13\n"
    "    movdqu d_vpat + 448, %xmm14\n"
    "    movdqu d_vpat + 480, %xmm15\n"
    "2:  call upc_pattern\n"
    "    pushq $" S(FLAGS_PAT) "\n"
    "    popfq\n"
    "upc_regs_fault:\n"
    "    ud2\n"
    "upc_regs_after:\n"
    "    movq %r8, d_aregs + 0\n"
    "    movq %r9, d_aregs + 8\n"
    "    movq %r10, d_aregs + 16\n"
    "    movq %r11, d_aregs + 24\n"
    "    movq %r12, d_aregs + 32\n"
    "    movq %r13, d_aregs + 40\n"
    "    movq %r14, d_aregs + 48\n"
    "    movq %r15, d_aregs + 56\n"
    "    movq %rdi, d_aregs + 64\n"
    "    movq %rsi, d_aregs + 72\n"
    "    movq %rbp, d_aregs + 80\n"
    "    movq %rbx, d_aregs + 88\n"
    "    movq %rdx, d_aregs + 96\n"
    "    movq %rax, d_aregs + 104\n"
    "    movq %rcx, d_aregs + 112\n"
    "    movq %rsp, d_aregs + 120\n"
    "    pushfq\n"
    "    popq d_aflags\n"
    "    cld\n"
    "    movw %cs, d_acs\n"
    "    movw %ss, d_ass\n"
    "    stmxcsr d_amxcsr\n"
    "    cmpq $0, d_avx\n"
    "    je 1f\n"
    "    vmovdqu %ymm0, d_avec + 0\n"
    "    vmovdqu %ymm1, d_avec + 32\n"
    "    vmovdqu %ymm2, d_avec + 64\n"
    "    vmovdqu %ymm3, d_avec + 96\n"
    "    vmovdqu %ymm4, d_avec + 128\n"
    "    vmovdqu %ymm5, d_avec + 160\n"
    "    vmovdqu %ymm6, d_avec + 192\n"
    "    vmovdqu %ymm7, d_avec + 224\n"
    "    vmovdqu %ymm8, d_avec + 256\n"
    "    vmovdqu %ymm9, d_avec + 288\n"
    "    vmovdqu %ymm10, d_avec + 320\n"
    "    vmovdqu %ymm11, d_avec + 352\n"
    "    vmovdqu %ymm12, d_avec + 384\n"
    "    vmovdqu %ymm13, d_avec + 416\n"
    "    vmovdqu %ymm14, d_avec + 448\n"
    "    vmovdqu %ymm15, d_avec + 480\n"
    "    jmp 2f\n"
    "1:  movdqu %xmm0, d_avec + 0\n"
    "    movdqu %xmm1, d_avec + 32\n"
    "    movdqu %xmm2, d_avec + 64\n"
    "    movdqu %xmm3, d_avec + 96\n"
    "    movdqu %xmm4, d_avec + 128\n"
    "    movdqu %xmm5, d_avec + 160\n"
    "    movdqu %xmm6, d_avec + 192\n"
    "    movdqu %xmm7, d_avec + 224\n"
    "    movdqu %xmm8, d_avec + 256\n"
    "    movdqu %xmm9, d_avec + 288\n"
    "    movdqu %xmm10, d_avec + 320\n"
    "    movdqu %xmm11, d_avec + 352\n"
    "    movdqu %xmm12, d_avec + 384\n"
    "    movdqu %xmm13, d_avec + 416\n"
    "    movdqu %xmm14, d_avec + 448\n"
    "    movdqu %xmm15, d_avec + 480\n"
    "2:  xorl %edi, %edi\n"
    "    jmp upc_exit\n"

    /* ---- upc_generic: the patterns, then the test's fault ------------ */
    "upc_generic:\n"
    "    call upc_setup\n"
    "    call upc_pattern\n"
    "    jmp *d_faulter\n"
    "upc_generic_after:\n"
    "    movq $" S(DONE) ", d_done\n"
    "    xorl %edi, %edi\n"
    "    jmp upc_exit\n"

    /* The faults: f_x, then f_x_i (the faulting instruction), f_x_end. */
    "f_ud2:\n"
    "f_ud2_i: ud2\n"
    "f_ud2_end: jmp upc_generic_after\n"
    "f_div: xorl %ecx, %ecx\n"
    "f_div_i: divl %ecx\n"
    "f_div_end: jmp upc_generic_after\n"
    "f_int3:\n"
    "f_int3_i: int3\n"
    "f_int3_end: jmp upc_generic_after\n"
    "f_int3b:\n"
    "f_int3b_i: int $3\n"
    "f_int3b_end: jmp upc_generic_after\n"
    "f_int1:\n"
    "f_int1_i: .byte 0xf1\n"
    "f_int1_end: jmp upc_generic_after\n"
    "f_tf: pushfq\n"
    "    orq $0x100, (%rsp)\n"
    "    popfq\n"
    "f_tf_i: nop\n"
    "f_tf_end: jmp upc_generic_after\n"
    "f_ss: movabsq $0x8000000000000000, %rbp\n"
    "f_ss_i: movq (%rbp), %rax\n"
    "f_ss_end: jmp upc_generic_after\n"
    "f_hlt:\n"
    "f_hlt_i: hlt\n"
    "f_hlt_end: jmp upc_generic_after\n"
    "f_gpnc: movabsq $0x8000000000000000, %rax\n"
    "f_gpnc_i: movq (%rax), %rax\n"
    "f_gpnc_end: jmp upc_generic_after\n"
    "f_gpint:\n"
    "f_gpint_i: int $0x80\n"
    "f_gpint_end: jmp upc_generic_after\n"
    "f_pfmap:\n"
    "f_pfmap_i: movq u_hole, %rax\n"
    "f_pfmap_end: jmp upc_generic_after\n"
    "f_pfro: cmpq $0, u_more\n"
    "f_pfro_i: movq %rax, u_more\n"
    "f_pfro_end: jmp upc_generic_after\n"
    "f_pfnx:\n"
    "f_pfnx_i: jmp *d_nx\n"
    "f_pfnx_end: jmp upc_generic_after\n"
    "f_load: movq d_arg, %rax\n"
    "f_load_i: movq (%rax), %rax\n"
    "f_load_end: jmp upc_generic_after\n"
    "f_mf: fldcw d_fcw_in\n"
    "    fld1\n"
    "    fdivl d_zero\n"
    "f_mf_i: fwait\n"
    "f_mf_end: jmp upc_generic_after\n"
    "f_xm: ldmxcsr d_mxcsr_xm\n"
    "    movss d_one, %xmm0\n"
    "    xorps %xmm1, %xmm1\n"
    "f_xm_i: divss %xmm1, %xmm0\n"
    "f_xm_end: jmp upc_generic_after\n"

    /* ---- the rest ---------------------------------------------------- */
    "upc_nostack:\n"                    /* a fault whose frame can't be written */
    "    call upc_setup\n"
    "    movq d_arg, %rsp\n"
    "upc_nostack_i:\n"
    "    ud2\n"
    "upc_badptr:\n"                     /* calls with pointers to nothing */
    "    call upc_setup\n"
    "    movl $0x1000, %edi\n"
    "    movl $" S(H_RESUME) ", %esi\n"
    "    movl $" S(SYS_exception_done) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret\n"
    "    movl $0x1000, %edi\n"
    "    movl $16, %esi\n"
    "    movl $" S(SYS_debug_write) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret2\n"
    "    xorl %edi, %edi\n"
    "    jmp upc_exit\n"
    "upc_forge:\n"                      /* EXC_RESUME of a frame it was never given */
    "    call upc_setup\n"
    "    movl $u_more, %edi\n"
    "    movl $" S(H_RESUME) ", %esi\n"
    "    movl $" S(SYS_exception_done) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret\n"
    "    movl $97, %edi\n"
    "    jmp upc_exit\n"
    "upc_setargs:\n"                    /* exception_entry_set's refusals; then none */
    "    movl $1, d_noentry\n"
    "    call upc_setup\n"
    "    movabsq $" S(U_TOP) ", %rdi\n"
    "    movl $" S(SYS_exception_entry_set) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret\n"
    "    movabsq $0xffff800000000000, %rdi\n"
    "    movl $" S(SYS_exception_entry_set) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret2\n"
    "    leaq upc_entry(%rip), %rdi\n"
    "    movl $" S(SYS_exception_entry_set) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret3\n"
    "    xorl %edi, %edi\n"
    "    movl $" S(SYS_exception_entry_set) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret4\n"
    "upc_setargs_i:\n"
    "    ud2\n"
    "upc_oom:\n"                        /* a fault on a page the job can't pay for */
    "    call upc_setup\n"
    "    movq $1, d_ready\n"
    "1:  pause\n"
    "    cmpq $0, d_go\n"
    "    je 1b\n"
    "upc_oom_i:\n"
    "    movq u_more, %rax\n"
    "    xorl %edi, %edi\n"
    "    jmp upc_exit\n"

    /* ---- upc_spin: threads faulting and resuming, each checking its own */
    "upc_sentry:\n"                     /* r12 = the thread's index, r14 its xmm0 */
    "    movq " S(F_X) " + " S(X_XMM) "(%rdi), %rax\n"
    "    cmpq %rax, %r14\n"
    "    jne 1f\n"
    "    cmpq " S(F_R12) "(%rdi), %r12\n"
    "    jne 1f\n"
    "    leaq d_cookies(,%r12,8), %rcx\n"
    "    movq " S(F_COOKIE) "(%rdi), %rax\n"
    "    cmpq $0, (%rcx)\n"
    "    jne 3f\n"
    "    movq %rax, (%rcx)\n"
    "3:  cmpq %rax, (%rcx)\n"
    "    je 2f\n"
    "1:  lock incq d_bad\n"
    "2:  addq $2, " S(F_RIP) "(%rdi)\n"
    "    movl $" S(H_RESUME) ", %esi\n"
    "    movl $" S(SYS_exception_done) ", %eax\n"
    "    syscall\n"
    "    lock incq d_bad\n"
    "    ud2\n"
    "upc_spin:\n"                       /* rdi = the thread's index */
    "    movq %rdi, %r12\n"
    "    movl $1, d_noentry\n"
    "    call upc_setup\n"
    "    leaq upc_sentry(%rip), %rdi\n"
    "    movl $" S(SYS_exception_entry_set) ", %eax\n"
    "    syscall\n"
    "    movq d_rounds, %r13\n"
    "    leaq 1(%r12), %r14\n"
    "    movabsq $0x0101010101010101, %rax\n"
    "    imulq %rax, %r14\n"
    "1:  movq %r14, %xmm0\n"
    "    ud2\n"
    "    movq %xmm0, %rax\n"
    "    cmpq %rax, %r14\n"
    "    je 2f\n"
    "    lock incq d_bad\n"
    "2:  decq %r13\n"
    "    jnz 1b\n"
    "    movl $1, %eax\n"
    "    lock xaddq %rax, d_finished\n"
    "    incq %rax\n"
    "    cmpq d_threads, %rax\n"
    "    jne 3f\n"
    "    xorl %edi, %edi\n"
    "    jmp upc_exit\n"
    "3:  movl $" S(SYS_thread_exit) ", %eax\n"
    "    syscall\n"
    "    ud2\n"

    /* ---- upc_bench: bench.c's line, timed around each ud2 ----------- */
    "upc_bentry:\n"
    "    addq $2, " S(F_RIP) "(%rdi)\n"
    "    movl $" S(H_RESUME) ", %esi\n"
    "    movl $" S(SYS_exception_done) ", %eax\n"
    "    syscall\n"
    "    ud2\n"
    "upc_bench:\n"
    "    movl $1, d_noentry\n"
    "    call upc_setup\n"
    "    leaq upc_bentry(%rip), %rdi\n"
    "    movl $" S(SYS_exception_entry_set) ", %eax\n"
    "    syscall\n"
    "    movq d_warm, %r13\n"
    "1:  ud2\n"
    "    decq %r13\n"
    "    jnz 1b\n"
    "    xorl %r12d, %r12d\n"
    "2:  lfence\n"
    "    rdtsc\n"
    "    lfence\n"
    "    shlq $32, %rdx\n"
    "    orq %rax, %rdx\n"
    "    movq %rdx, %r14\n"
    "    ud2\n"
    "    lfence\n"
    "    rdtsc\n"
    "    lfence\n"
    "    shlq $32, %rdx\n"
    "    orq %rax, %rdx\n"
    "    subq %r14, %rdx\n"
    "    movq %rdx, d_samples(,%r12,8)\n"
    "    incq %r12\n"
    "    cmpq d_rounds, %r12\n"
    "    jne 2b\n"
    "    xorl %edi, %edi\n"
    "    jmp upc_exit\n"

    /* ---- exception stacks (SG2) ---------------------------------------- */
    "upc_recurse:\n"                    /* a stack overflow into nothing mapped */
    "    call upc_setup\n"
    "upc_recurse_i:\n"
    "    call upc_recurse_i\n"
    "upc_esargs:\n"                     /* thread_exception_stack's refusals, then none */
    "    call upc_setup\n"
    "    movabsq $(" S(U_TOP) " - 0x1000), %rdi\n"
    "    movl $0x2000, %esi\n"
    "    movl $" S(SYS_thread_exception_stack) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret\n"
    "    movl $u_more, %edi\n"
    "    xorl %esi, %esi\n"
    "    movl $" S(SYS_thread_exception_stack) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret2\n"
    "    movl $0x800, %edi\n"
    "    movl $0x1000, %esi\n"
    "    movl $" S(SYS_thread_exception_stack) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret3\n"
    "    xorl %edi, %edi\n"
    "    xorl %esi, %esi\n"
    "    movl $" S(SYS_thread_exception_stack) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret4\n"
    "    xorl %edi, %edi\n"
    "    jmp upc_exit\n"

    /* ---- raises (SG2) -------------------------------------------------- */
    "act_raised:\n"                     /* a raise: its kind, the TSC, a count; on */
    "    rdtsc\n"
    "    shlq $32, %rdx\n"
    "    orq %rax, %rdx\n"
    "    movq %rdx, d_rtsc\n"
    "    movl " S(F_KIND) "(%rdi), %eax\n"
    "    movq %rax, d_rkind\n"
    "    lock incq d_rcount\n"
    "    jmp act_resume\n"
    "act_winspin:\n"                    /* in the window until D_GO; LEAVE; wait for a raise */
    "    movq $1, d_ready\n"
    "1:  cmpq $0, d_go\n"
    "    je 1b\n"
    "    movq d_rcount, %rax\n"
    "    movq %rax, d_ret2\n"
    "    leaq act_raised(%rip), %rax\n"
    "    movq %rax, d_action\n"
    "    movl $" S(H_LEAVE) ", %esi\n"
    "    movl $" S(SYS_exception_done) ", %eax\n"
    "    syscall\n"
    "upc_rleave_ret:\n"
    "    movq %rax, d_ret\n"
    "    jmp upc_rwait\n"
    "upc_rleave_end:\n"
    "upc_rspin:\n"                      /* its own code, no calls, until D_GO */
    "    call upc_setup\n"
    "    movq $1, d_ready\n"
    "upc_rspin_loop:\n"
    "    incq d_loops\n"
    "    cmpq $0, d_go\n"
    "    je upc_rspin_loop\n"
    "upc_rspin_end:\n"
    "    xorl %edi, %edi\n"
    "    jmp upc_exit\n"
    "upc_rwait:\n"                      /* until a raise was taken */
    "    cmpq $0, d_rcount\n"
    "    je upc_rwait\n"
    "upc_rwait_end:\n"
    "    xorl %edi, %edi\n"
    "    jmp upc_exit\n"
    "upc_rcall:\n"                      /* sleep_until D_ARG, then wait for a raise */
    "    call upc_setup\n"
    "    movq $1, d_ready\n"
    "    movq d_arg, %rdi\n"
    "    movl $" S(SYS_sleep_until) ", %eax\n"
    "    syscall\n"
    "upc_rcall_ret:\n"
    "    movq d_rcount, %rcx\n"         /* raises taken by the call's return: none */
    "    movq %rcx, d_ret2\n"
    "    movq %rax, d_ret\n"
    "    jmp upc_rwait\n"
    "upc_rcall_end:\n"
    "upc_rself:\n"                      /* thread_raise of itself, after D_GO */
    "    call upc_setup\n"
    "1:  cmpq $0, d_go\n"
    "    je 1b\n"
    "    movq d_hnosig, %rdi\n"
    "    movl $" S(SYS_thread_raise) ", %eax\n"
    "    syscall\n"
    "    movq %rax, d_ret\n"
    "    movq d_hself, %rdi\n"
    "    movl $" S(SYS_thread_raise) ", %eax\n"
    "    syscall\n"
    "upc_rself_ret:\n"
    "    movq d_rcount, %rcx\n"         /* not on the call's return */
    "    movq %rcx, d_ret2\n"
    "    movq %rax, d_ret3\n"
    "    jmp upc_rwait\n"
    "upc_rself_end:\n"
    "upc_rbadsp:\n"                     /* RSP at D_ARG (nothing writable) until D_GO */
    "    call upc_setup\n"
    "    movq d_arg, %rsp\n"
    "    movq $1, d_ready\n"
    "1:  cmpq $0, d_go\n"
    "    je 1b\n"
    "    xorl %edi, %edi\n"
    "    jmp upc_exit\n"

    /* ---- upc_rstorm: threads spinning, raised from the kernel ---------- */
    "upc_rsentry:\n"                    /* r12 = the thread's index */
    "    cmpl $" S(K_RAISED) ", " S(F_KIND) "(%rdi)\n"
    "    je 1f\n"
    "    lock incq d_bad\n"
    "1:  lock incq d_rcounts(,%r12,8)\n"
    "    movl $" S(H_RESUME) ", %esi\n"
    "    movl $" S(SYS_exception_done) ", %eax\n"
    "    syscall\n"
    "    lock incq d_bad\n"
    "    ud2\n"
    "upc_rstorm:\n"                     /* rdi = the thread's index; until D_STOP */
    "    movq %rdi, %r12\n"
    "    movl $1, d_noentry\n"
    "    call upc_setup\n"
    "    leaq upc_rsentry(%rip), %rdi\n"
    "    movl $" S(SYS_exception_entry_set) ", %eax\n"
    "    syscall\n"
    "    lock incq d_ready\n"
    "1:  cmpq $0, d_stop\n"
    "    je 1b\n"
    "    movl $1, %eax\n"
    "    lock xaddq %rax, d_finished\n"
    "    incq %rax\n"
    "    cmpq d_threads, %rax\n"
    "    jne 3f\n"
    "    xorl %edi, %edi\n"
    "    jmp upc_exit\n"
    "3:  movl $" S(SYS_thread_exit) ", %eax\n"
    "    syscall\n"
    "    ud2\n"

    /* ---- the raise's benchmark line: t1 raises t0, timed --------------- */
    "upc_rbentry:\n"                    /* t0's: the TSC since the raise, then on */
    "    rdtsc\n"
    "    shlq $32, %rdx\n"
    "    orq %rax, %rdx\n"
    "    subq d_tsc0, %rdx\n"
    "    movq d_rcount, %rax\n"
    "    movq %rdx, d_samples(,%rax,8)\n"
    "    lock incq d_rcount\n"
    "    movl $" S(H_RESUME) ", %esi\n"
    "    movl $" S(SYS_exception_done) ", %eax\n"
    "    syscall\n"
    "    ud2\n"
    "upc_rbspin:\n"                     /* t0: spin until D_GO */
    "    movl $1, d_noentry\n"
    "    call upc_setup\n"
    "    leaq upc_rbentry(%rip), %rdi\n"
    "    movl $" S(SYS_exception_entry_set) ", %eax\n"
    "    syscall\n"
    "    movq $1, d_ready\n"
    "1:  cmpq $0, d_go\n"
    "    je 1b\n"
    "    xorl %edi, %edi\n"
    "    jmp upc_exit\n"
    "upc_rbench:\n"                     /* t1: D_ROUNDS raises, each once the last was taken */
    "1:  cmpq $0, d_ready\n"
    "    je 1b\n"
    "    xorl %r12d, %r12d\n"
    "2:  cmpq %r12, d_rcount\n"
    "    jne 2b\n"
    "    lfence\n"
    "    rdtsc\n"
    "    lfence\n"
    "    shlq $32, %rdx\n"
    "    orq %rax, %rdx\n"
    "    movq %rdx, d_tsc0\n"
    "    movq d_hself, %rdi\n"
    "    movl $" S(SYS_thread_raise) ", %eax\n"
    "    syscall\n"
    "    incq %r12\n"
    "    cmpq d_rounds, %r12\n"
    "    jne 2b\n"
    "3:  cmpq %r12, d_rcount\n"
    "    jne 3b\n"
    "    movq $1, d_go\n"
    "    movl $" S(SYS_thread_exit) ", %eax\n"
    "    syscall\n"
    "    ud2\n"

    /* ---- hostile: every vector from many threads, raised meanwhile ----- */
    "upc_ventry:\n"                     /* r12 = the thread's index, r14 its pattern */
    "    cmpq " S(F_R12) "(%rdi), %r12\n"
    "    jne 1f\n"
    "    cmpq " S(F_R14) "(%rdi), %r14\n"
    "    jne 1f\n"
    "    cmpq " S(F_X) " + " S(X_XMM) " + 240(%rdi), %r14\n"   /* xmm15 */
    "    jne 1f\n"
    "    leaq d_cookies(,%r12,8), %rcx\n"
    "    movq " S(F_COOKIE) "(%rdi), %rax\n"
    "    cmpq $0, (%rcx)\n"
    "    jne 3f\n"
    "    movq %rax, (%rcx)\n"
    "3:  cmpq %rax, (%rcx)\n"
    "    jne 1f\n"
    "    cmpl $" S(K_RAISED) ", " S(F_KIND) "(%rdi)\n"
    "    je 4f\n"
    "    movl " S(F_VECTOR) "(%rdi), %eax\n"   /* a fault: the vector r13 said */
    "    cmpq " S(F_R13) "(%rdi), %rax\n"
    "    jne 1f\n"
    "    lock incq d_vfaults\n"
    "    movq " S(F_R15) "(%rdi), %rax\n"     /* past it by r15's bytes */
    "    addq %rax, " S(F_RIP) "(%rdi)\n"
    "    andq $-257, " S(F_RFLAGS) "(%rdi)\n" /* a single step's TF off */
    "    jmp 2f\n"
    "4:  lock incq d_rcount\n"
    "    jmp 2f\n"
    "1:  lock incq d_bad\n"
    "2:  movl $" S(H_RESUME) ", %esi\n"
    "    movl $" S(SYS_exception_done) ", %eax\n"
    "    syscall\n"
    "    lock incq d_bad\n"
    "    ud2\n"
    "upc_vspin:\n"                      /* rdi = the thread's index; D_ROUNDS or D_STOP */
    "    movq %rdi, %r12\n"
    "    movl $1, d_noentry\n"
    "    call upc_setup\n"
    "    leaq upc_ventry(%rip), %rdi\n"
    "    movl $" S(SYS_exception_entry_set) ", %eax\n"
    "    syscall\n"
    "    leaq 1(%r12), %r14\n"
    "    movabsq $0x0101010101010101, %rax\n"
    "    imulq %rax, %r14\n"
    "    movq %r14, %xmm15\n"
    "    movq d_rounds, %rbx\n"
    "    lock incq d_ready\n"
    "upc_vloop:\n"                      /* each: r13 the vector, r15 the bytes to skip */
    "    movl $6, %r13d\n"
    "    movl $2, %r15d\n"
    "    ud2\n"
    "    xorl %ecx, %ecx\n"
    "    xorl %r13d, %r13d\n"
    "    divl %ecx\n"
    "    movl $3, %r13d\n"
    "    xorl %r15d, %r15d\n"
    "    int3\n"
    "    movl $1, %r13d\n"
    "    .byte 0xf1\n"                  /* int1 */
    "    movl $13, %r13d\n"
    "    movl $1, %r15d\n"
    "    hlt\n"
    "    movl $14, %r13d\n"
    "    movl $8, %r15d\n"
    "    movq u_hole, %rax\n"
    "    movl $6, %r15d\n"
    "    movb %al, upc_vspin(%rip)\n"   /* the code is read-only */
    "    movabsq $0x8000000000000000, %rax\n"
    "    movl $13, %r13d\n"
    "    movl $3, %r15d\n"
    "    movq (%rax), %rax\n"           /* non-canonical */
    "    movabsq $0x8000000000000000, %rbp\n"
    "    movl $12, %r13d\n"
    "    movl $4, %r15d\n"
    "    movq 0(%rbp), %rax\n"          /* non-canonical through RBP: the stack's */
    "    fninit\n"
    "    fldcw d_fcw_in\n"
    "    fldz\n"
    "    fld1\n"
    "    fdiv %st(1), %st\n"            /* 1/0, zero divide unmasked: pending */
    "    movl $16, %r13d\n"
    "    movl $1, %r15d\n"
    "    fwait\n"
    "    fninit\n"
    "    ldmxcsr d_mxcsr_xm\n"
    "    movss d_one, %xmm1\n"
    "    xorps %xmm2, %xmm2\n"
    "    movl $19, %r13d\n"
    "    movl $4, %r15d\n"
    "    divss %xmm2, %xmm1\n"          /* (TCG raises none: then nothing comes) */
    "    pushq $0x1f80\n"
    "    ldmxcsr (%rsp)\n"
    "    popq %rax\n"
    "    movl $1, %r13d\n"
    "    xorl %r15d, %r15d\n"
    "    pushfq\n"
    "    orq $0x100, (%rsp)\n"
    "    popfq\n"
    "    nop\n"                         /* single-stepped: vector 1 after it */
    "    decq %rbx\n"
    "    jz 1f\n"
    "    cmpq $0, d_stop\n"
    "    je upc_vloop\n"
    "1:  movl $1, %eax\n"
    "    lock xaddq %rax, d_finished\n"
    "    incq %rax\n"
    "    cmpq d_threads, %rax\n"
    "    jne 2f\n"
    "    xorl %edi, %edi\n"
    "    jmp upc_exit\n"
    "2:  movl $" S(SYS_thread_exit) ", %eax\n"
    "    syscall\n"
    "    ud2\n"

    /* ---- hostile: random frames given to RESUME ------------------------ */
    "upc_hrnd:\n"                       /* rax = the next random word (xorshift); rdx */
    "    movq d_rng, %rax\n"
    "    movq %rax, %rdx\n"
    "    shlq $13, %rdx\n"
    "    xorq %rdx, %rax\n"
    "    movq %rax, %rdx\n"
    "    shrq $7, %rdx\n"
    "    xorq %rdx, %rax\n"
    "    movq %rax, %rdx\n"
    "    shlq $17, %rdx\n"
    "    xorq %rdx, %rax\n"
    "    movq %rax, d_rng\n"
    "    ret\n"
    "upc_hexp:\n"                       /* what the landing must find: rbx's frame */
    "    xorl %ecx, %ecx\n"
    "1:  movq " S(F_R8) "(%rbx,%rcx,8), %rax\n"
    "    movq %rax, d_expect(,%rcx,8)\n"
    "    incl %ecx\n"
    "    cmpl $16, %ecx\n"
    "    jb 1b\n"
    "    movq " S(F_RFLAGS) "(%rbx), %rax\n"
    "    andq $" S(U_FLAGS_OK) ", %rax\n"
    "    orq $0x202, %rax\n"
    "    movq %rax, d_expfl\n"
    "    movq " S(F_X) " + " S(X_XMM) "(%rbx), %rax\n"
    "    movq %rax, d_expx0\n"
    "    ret\n"
    "upc_hrand:\n"
    "    movl $1, d_noentry\n"
    "    call upc_setup\n"
    "    leaq upc_hentry(%rip), %rdi\n"
    "    movl $" S(SYS_exception_entry_set) ", %eax\n"
    "    syscall\n"
    "upc_hloop:\n"
    "    movl $" S(H_SP) ", %esp\n"
    "    cld\n"
    "    ud2\n"
    "    jmp upc_hloop\n"
    "upc_hland:\n"                      /* a resumed frame: is it exactly what it said? */
    "    movq %r8, d_aregs + 0\n"
    "    movq %r9, d_aregs + 8\n"
    "    movq %r10, d_aregs + 16\n"
    "    movq %r11, d_aregs + 24\n"
    "    movq %r12, d_aregs + 32\n"
    "    movq %r13, d_aregs + 40\n"
    "    movq %r14, d_aregs + 48\n"
    "    movq %r15, d_aregs + 56\n"
    "    movq %rdi, d_aregs + 64\n"
    "    movq %rsi, d_aregs + 72\n"
    "    movq %rbp, d_aregs + 80\n"
    "    movq %rbx, d_aregs + 88\n"
    "    movq %rdx, d_aregs + 96\n"
    "    movq %rax, d_aregs + 104\n"
    "    movq %rcx, d_aregs + 112\n"
    "    movq %rsp, d_aregs + 120\n"
    "    movl $" S(H_SP) ", %esp\n"
    "    pushfq\n"
    "    popq d_aflags\n"
    "    cld\n"
    "    movq %xmm0, %rax\n"
    "    cmpq d_expx0, %rax\n"
    "    jne 2f\n"
    "    movq d_aflags, %rax\n"
    "    cmpq d_expfl, %rax\n"
    "    jne 2f\n"
    "    xorl %ecx, %ecx\n"
    "1:  movq d_aregs(,%rcx,8), %rax\n"
    "    cmpq d_expect(,%rcx,8), %rax\n"
    "    jne 2f\n"
    "    incl %ecx\n"
    "    cmpl $16, %ecx\n"
    "    jb 1b\n"
    "    incq d_hlanded\n"
    "    jmp upc_hloop\n"
    "2:  incq d_hbad\n"
    "    jmp upc_hloop\n"
    "upc_hentry:\n"                     /* rdi = the frame */
    "    movq %rdi, %rbx\n"
    "    movq d_hframes, %rax\n"
    "    cmpq d_hwant, %rax\n"
    "    jb 1f\n"
    "    xorl %edi, %edi\n"
    "    jmp upc_exit\n"
    "1:  incq d_hframes\n"
    "    movl " S(F_SIZE) "(%rbx), %ecx\n"   /* a witness's pattern anywhere in it? */
    "    shrl $3, %ecx\n"
    "    xorl %esi, %esi\n"
    "    movabsq $0x0101010101010101, %r9\n"
    "2:  movq (%rbx,%rsi,8), %rax\n"
    "    movzbl %al, %edx\n"
    "    leal -1(%rdx), %r8d\n"
    "    cmpl $8, %r8d\n"
    "    jae 3f\n"
    "    movq %r9, %r8\n"
    "    imulq %rdx, %r8\n"
    "    cmpq %r8, %rax\n"
    "    jne 3f\n"
    "    incq d_hleak\n"
    "3:  incl %esi\n"
    "    cmpl %ecx, %esi\n"
    "    jb 2b\n"
    "    leaq upc_hland(%rip), %rax\n"
    "    movq %rax, " S(F_RIP) "(%rbx)\n"
    "    xorl %r15d, %r15d\n"           /* 1: single-stepped too */
    "    call upc_hrnd\n"
    "    movq %rax, %r12\n"
    "    movzbl %al, %eax\n"
    "    cmpl $16, %eax\n"
    "    jae 4f\n"                      /* 16 .. 255: random registers, runs on */
    "    cmpl $11, %eax\n"
    "    jb upc_hbad\n"                 /* 0 .. 10: a frame meant to be refused */
    "    movl $1, %r15d\n"             /* 11 .. 15: random, and TF */
    "4:  xorl %r13d, %r13d\n"
    "5:  call upc_hrnd\n"
    "    movq %rax, " S(F_R8) "(%rbx,%r13,8)\n"
    "    incl %r13d\n"
    "    cmpl $15, %r13d\n"            /* r8 .. rcx, not rsp */
    "    jb 5b\n"
    "    testl %r15d, %r15d\n"
    "    jnz 6f\n"
    "    btq $8, %r12\n"
    "    jnc 6f\n"
    "    call upc_hrnd\n"
    "    movq %rax, " S(F_RSP) "(%rbx)\n"   /* anywhere: the landing sets its own */
    "6:  call upc_hrnd\n"
    "    andq $-257, %rax\n"
    "    movq %rax, " S(F_RFLAGS) "(%rbx)\n"
    "    xorl %r13d, %r13d\n"
    "7:  call upc_hrnd\n"
    "    movq %rax, " S(F_X) "(%rbx,%r13,8)\n"
    "    incl %r13d\n"
    "    cmpl $52, %r13d\n"            /* x87 and SSE: 416 bytes */
    "    jb 7b\n"
    "    call upc_hrnd\n"
    "    andl $0xffbf, %eax\n"
    "    movl %eax, " S(F_X) " + " S(X_MXCSR) "(%rbx)\n"
    "    cmpq $0, d_avx\n"
    "    je 9f\n"
    "    movq d_avxoff, %r13\n"
    "    leaq " S(F_X) "(%rbx,%r13), %r13\n"
    "    xorl %r14d, %r14d\n"
    "8:  call upc_hrnd\n"
    "    movq %rax, (%r13,%r14,8)\n"
    "    incl %r14d\n"
    "    cmpl $32, %r14d\n"            /* AVX's upper halves: 256 bytes */
    "    jb 8b\n"
    "9:  call upc_hexp\n"
    "    testl %r15d, %r15d\n"
    "    jz upc_hgo\n"
    "    orq $0x100, " S(F_RFLAGS) "(%rbx)\n"
    "upc_hgo:\n"                        /* RESUME rbx's frame: never back */
    "    movq %rbx, %rdi\n"
    "    movl $" S(H_RESUME) ", %esi\n"
    "    movl $" S(SYS_exception_done) ", %eax\n"
    "    syscall\n"
    "    movl $97, %edi\n"
    "    jmp upc_exit\n"
    "upc_hbad:\n"                       /* eax = 0 .. 10 */
    "    movl %eax, %r13d\n"
    "    call upc_hexp\n"
    "    call upc_hrnd\n"
    "    cmpl $0, %r13d\n"
    "    jne 1f\n"
    "    orq $1, %rax\n"
    "    xorq %rax, " S(F_COOKIE) "(%rbx)\n"
    "    jmp upc_hgo\n"
    "1:  cmpl $1, %r13d\n"
    "    jne 1f\n"
    "    movl %eax, " S(F_X) " + " S(X_MXCSR) "(%rbx)\n"
    "    jmp upc_hgo\n"
    "1:  cmpl $2, %r13d\n"
    "    jne 1f\n"
    "    movq %rax, " S(F_X) " + " S(X_BV) "(%rbx)\n"
    "    jmp upc_hgo\n"
    "1:  cmpl $3, %r13d\n"
    "    jne 1f\n"
    "    movq %rax, %rcx\n"
    "    shrq $8, %rcx\n"
    "    andl $31, %ecx\n"
    "    orb $1, %al\n"
    "    movb %al, " S(F_X) " + " S(X_COMP) "(%rbx,%rcx)\n"   /* XCOMP_BV, reserved */
    "    jmp upc_hgo\n"
    "1:  cmpl $4, %r13d\n"
    "    jne 1f\n"
    "    movl %eax, " S(F_SIZE) "(%rbx)\n"
    "    jmp upc_hgo\n"
    "1:  cmpl $5, %r13d\n"
    "    jne 1f\n"
    "    movl %eax, " S(F_XSIZE) "(%rbx)\n"
    "    jmp upc_hgo\n"
    "1:  cmpl $6, %r13d\n"
    "    jne 1f\n"
    "    movq %rax, " S(F_RIP) "(%rbx)\n"   /* anywhere */
    "    jmp upc_hgo\n"
    "1:  cmpl $7, %r13d\n"
    "    jne 1f\n"
    "    movzbl %al, %eax\n"
    "    movabsq $(" S(U_TOP) " - 128), %rcx\n"
    "    addq %rcx, %rax\n"
    "    movq %rax, " S(F_RIP) "(%rbx)\n"   /* around the user range's top */
    "    jmp upc_hgo\n"
    "1:  cmpl $9, %r13d\n"
    "    ja 2f\n"
    "    je 3f\n"
    "    movq %rax, %rdi\n"                 /* 8: a frame anywhere */
    "    jmp 4f\n"
    "3:  andl $7, %eax\n"
    "    leaq 8(%rbx,%rax,8), %rdi\n"      /* 9: the frame a few words on */
    "4:  movl $" S(H_RESUME) ", %esi\n"
    "    movl $" S(SYS_exception_done) ", %eax\n"
    "    syscall\n"
    "    incq d_hrefused\n"                 /* back: refused, so on with the real one */
    "    jmp upc_hgo\n"
    "2:  xorl %r13d, %r13d\n"              /* 10: the header and registers random */
    "5:  call upc_hrnd\n"
    "    movq %rax, (%rbx,%r13,8)\n"
    "    incl %r13d\n"
    "    cmpl $24, %r13d\n"
    "    jb 5b\n"
    "    jmp upc_hgo\n"
    "upc_blob_end:\n"
    ".popsection\n");

#define LABELS(X)                                                                     \
    X(upc_blob) X(upc_blob_end) X(upc_entry) X(upc_regs) X(upc_regs_fault)              \
    X(upc_regs_after) X(upc_generic) X(upc_generic_after) X(upc_nostack)                \
    X(upc_nostack_i) X(upc_badptr) X(upc_forge) X(upc_setargs) X(upc_setargs_i)         \
    X(upc_oom) X(upc_oom_i) X(upc_spin) X(upc_bench)                                    \
    X(act_skip) X(act_resume) X(act_exit) X(act_unhandled) X(act_again) X(act_leave)     \
    X(act_edit) X(act_flags) X(act_untf) X(act_poke) X(act_xor)                          \
    X(f_ud2) X(f_ud2_i) X(f_ud2_end) X(f_div) X(f_div_i) X(f_div_end)                    \
    X(f_int3) X(f_int3_i) X(f_int3_end) X(f_int3b) X(f_int3b_i) X(f_int3b_end)           \
    X(f_int1) X(f_int1_i) X(f_int1_end) X(f_tf) X(f_tf_i) X(f_tf_end)                    \
    X(f_ss) X(f_ss_i) X(f_ss_end) X(f_hlt) X(f_hlt_i) X(f_hlt_end)                       \
    X(f_gpnc) X(f_gpnc_i) X(f_gpnc_end) X(f_gpint) X(f_gpint_i) X(f_gpint_end)           \
    X(f_pfmap) X(f_pfmap_i) X(f_pfmap_end) X(f_pfro) X(f_pfro_i) X(f_pfro_end)           \
    X(f_pfnx) X(f_pfnx_i) X(f_pfnx_end) X(f_load) X(f_load_i) X(f_load_end)              \
    X(f_mf) X(f_mf_i) X(f_mf_end) X(f_xm) X(f_xm_i) X(f_xm_end)                          \
    X(upc_recurse) X(upc_recurse_i) X(upc_esargs) X(act_raised) X(act_winspin)           \
    X(upc_rspin) X(upc_rspin_loop) X(upc_rspin_end) X(upc_rwait) X(upc_rwait_end)        \
    X(upc_rcall) X(upc_rcall_ret) X(upc_rcall_end) X(upc_rself) X(upc_rself_ret)         \
    X(upc_rself_end) X(upc_rleave_ret) X(upc_rleave_end) X(upc_rbadsp)                   \
    X(upc_rstorm) X(upc_rbspin) X(upc_rbench) X(upc_vspin) X(upc_hrand)
#define DECLARE(l) extern const uint8_t l[];
LABELS(DECLARE)

/* Where label l runs in the programs' address space. */
#define AT(l) (U_CODE + (uint64_t)((l) - upc_blob))

/* ---- a process running one of the programs ------------------------------- */

struct upc {
    struct job     *j;
    struct process *p;
    struct aspace  *as;
    struct vmo     *code, *stack, *data;
    struct uthread *u[8];
    unsigned        nu;
    uint64_t        log0, faults0;
    const char     *name;
};

static void put(struct vmo *v)
{
    kobject_unref(vmo_kobject(v));
}

static void map(struct upc *u, struct vmo *v, uint64_t at, uint64_t len, unsigned perms)
{
    uint64_t a = at;
    KT_EQ(aspace_map(u->as, v, 0, len, perms | ASPACE_FIXED, &a), OK);
}

static void set(struct upc *u, uint64_t off, uint64_t val)
{
    KT_EQ(vmo_write(u->data, off, &val, 8), OK);
}

static uint64_t get(struct upc *u, uint64_t off)
{
    uint64_t w = 0;
    KT_EQ(vmo_read(u->data, off, &w, 8), OK);
    return w;
}

/* The frame's AVX part's offset in the XSAVE area (0: no AVX in XCR0). */
static uint32_t avx_off(void)
{
    if (!cpu_features.xsave || fpu_frame_size() < 832)
        return 0;
    uint32_t a, b, c, d;
    cpuid(0xd, 2, &a, &b, &c, &d);
    return b;
}

static uint64_t pat(unsigned i)   /* register i's pattern (the frame's order) */
{
    return 0x1111111111111111ull * (i + 1);
}

/* A job, a process `name`, its address space with the programs, the stack
 * and the data page (the defaults written in). */
static void upc_new(struct upc *u, const char *name)
{
    *u = (struct upc){ .name = name };
    u->j = kt_fresh_job();
    KT_EQ(process_create(u->j, name, &u->p), OK);
    u->as = process_aspace(u->p);
    KT_ASSERT(u->as);
    uint64_t len = (uint64_t)(upc_blob_end - upc_blob);
    KT_ASSERT(len <= CODE_PAGES * PG);
    KT_EQ(vmo_create(CODE_PAGES * PG, 0, &u->code), OK);
    KT_EQ(vmo_write(u->code, 0, upc_blob, len), OK);
    map(u, u->code, U_CODE, CODE_PAGES * PG, ASPACE_READ | ASPACE_EXEC);
    KT_EQ(vmo_create(STACK_PAGES * PG, 0, &u->stack), OK);
    map(u, u->stack, U_STACK, STACK_PAGES * PG, ASPACE_READ | ASPACE_WRITE);
    KT_EQ(vmo_create(DATA_PAGES * PG, 0, &u->data), OK);
    map(u, u->data, U_DATA, DATA_PAGES * PG, ASPACE_READ | ASPACE_WRITE);
    set(u, D_ACTION, AT(act_skip));
    set(u, D_AVX, avx_off() != 0);
    set(u, D_AVXOFF, avx_off());
    set(u, D_MXCSR_IN, 0x3f80);    /* round down: not the default */
    set(u, D_FCW_IN, 0x037b);      /* zero divide unmasked */
    set(u, D_MXCSR_XM, 0x1d80);    /* zero divide unmasked */
    set(u, D_ONE, 0x3f800000);
    set(u, D_FSWORD, 0x5eed5eed5eed5eedull);
    set(u, D_NX, U_DATA + 0xf00);
    u->log0 = klog_head();
    u->faults0 = __atomic_load_n(&user_faults, __ATOMIC_RELAXED);
}

/* Start the process's first thread at prog with rsi = arg1, on `cpu` if
 * it isn't -1. */
static void upc_start_on(struct upc *u, const uint8_t *prog, uint64_t arg1, int cpu)
{
    KT_EQ(uthread_create(u->p, "t0", &u->u[0]), OK);
    u->nu = 1;
    struct khandle none = { NULL, 0 };
    cpumask_t m;
    if (cpu >= 0)
        cpumask_one(&m, (uint32_t)cpu);
    KT_EQ(process_start(u->p, u->u[0], AT(prog), U_STACK_TOP, &none, arg1, cpu >= 0 ? &m : NULL),
          OK);
}

static void upc_start(struct upc *u, const uint8_t *prog)
{
    upc_start_on(u, prog, 0, -1);
}

/* Wait for the process's end: its info. */
static struct process_info upc_wait(struct upc *u)
{
    KT_EQ(object_wait_one(process_kobject(u->p), SIG_TERMINATED,
                          uptime_ns() + kt_patience_ms(20000) * NS_PER_MS, NULL),
          OK);
    struct process_info info;
    process_get_info(u->p, &info);
    KT_EQ(info.state, PROCESS_DEAD);
    return info;
}

static void upc_end(struct upc *u)
{
    for (unsigned i = 0; i < u->nu; i++)
        kobject_unref(uthread_kobject(u->u[i]));
    aspace_unref(u->as);
    kobject_unref(process_kobject(u->p));
    put(u->code);
    put(u->stack);
    put(u->data);
    kt_job_is_empty(u->j);
    job_unref(u->j);
}

/* It exited with `code`, nobody killed. */
static void exited(struct upc *u, int64_t code)
{
    struct process_info info = upc_wait(u);
    KT_ASSERT(!info.killed);
    KT_EQ(info.exit_code, code);
    KT_EQ(info.flags & PROCESS_FLAG_FAULT, 0);   /* an exit names no fault */
    KT_EQ(info.fault_vector, 0);
    KT_GLOBAL_EQ(__atomic_load_n(&user_faults, __ATOMIC_RELAXED), u->faults0);
}

/* Is `line` in the log since `from`, ending its line (nothing after it)? */
static bool log_has_line(uint64_t from, const char *line)
{
    enum { CAP = 256 * 1024 };
    char *buf = kmalloc(CAP);
    KT_ASSERT(buf);
    size_t m = strlen(line);
    uint64_t deadline = uptime_ns() + kt_patience_ms(2000) * NS_PER_MS;
    bool found = false;
    while (!found && uptime_ns() < deadline) {
        uint64_t first, pos = klog_head() - from > CAP ? klog_head() - CAP : from;
        size_t n = klog_read_at(pos, buf, CAP, &first);
        for (size_t i = 0; i + m < n && !found; i++)
            found = !memcmp(buf + i, line, m) && buf[i + m] == '\n';
        if (!found)
            thread_sleep_ms(5);
    }
    kfree(buf);
    return found;
}

/* It was killed with exactly this line: "user: process "<name>" killed:
 * <why> at rip <rip>, address <addr> (thread "t0")<more>", and its
 * process_info names a fault: its info (fault_vector, fault_status). */
static struct process_info killed(struct upc *u, const char *why, uint64_t rip, uint64_t addr,
                                  const char *more)
{
    struct process_info info = upc_wait(u);
    KT_ASSERT(info.killed);
    KT_EQ(info.exit_code, PROCESS_KILLED_CODE);
    KT_EQ(info.flags & PROCESS_FLAG_FAULT, PROCESS_FLAG_FAULT);
    KT_GLOBAL_EQ(__atomic_load_n(&user_faults, __ATOMIC_RELAXED), u->faults0 + 1);
    char line[200];
    ksnprintf(line, sizeof(line),
              "user: process \"%s\" killed: %s at rip %lx, address %lx (thread \"t0\")%s", u->name,
              why, rip, addr, more);
    if (!log_has_line(u->log0, line))
        ktest_fail("no line \"%s\"", line);
    return info;
}

/* process_info named this fault (vector, a page fault's status). */
static void fault_is(struct process_info info, uint32_t vector, status_t status)
{
    KT_EQ(info.fault_vector, vector);
    KT_EQ(info.fault_status, status);
}

/* The frame the last entry was given (still on the stack), header and
 * all; *at its address. The caller kfrees it. */
static struct exc_frame *frame_of(struct upc *u, uint64_t *at)
{
    *at = get(u, D_FRAME);
    uint64_t size = EXC_FRAME_XSAVE + fpu_frame_size();
    KT_ASSERT(*at >= U_STACK && *at + size <= U_STACK_TOP);
    struct exc_frame *f = kzalloc(size);
    KT_ASSERT(f);
    KT_EQ(vmo_read(u->stack, *at - U_STACK, f, size), OK);
    return f;
}

static uint64_t fr_reg(const struct exc_frame *f, unsigned i)   /* register i, frame order */
{
    uint64_t r;
    memcpy(&r, (const uint8_t *)f + offsetof(struct exc_frame, r8) + i * 8, 8);
    return r;
}

static uint64_t xcr0_now(void)
{
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (uint64_t)hi << 32 | lo;
}

/* What every entry was given, whatever the fault: the frame where the ABI
 * says, the registers at the entry, the initial vector state, the FS
 * base untouched. `fault_rsp` was RSP at the fault. */
static void entry_ok(struct upc *u, const struct exc_frame *f, uint64_t at, uint64_t fault_rsp)
{
    uint32_t xsize = fpu_frame_size();
    KT_EQ(at % 64, 0);
    KT_EQ(at, (fault_rsp - 128 - EXC_FRAME_XSAVE - xsize) & ~63ull);
    KT_EQ(f->size, EXC_FRAME_XSAVE + xsize);
    KT_EQ(f->xsave_size, xsize);
    KT_EQ(f->kind, EXC_FAULT);
    KT_EQ(f->reserved, 0);
    KT_ASSERT(f->cookie);
    KT_EQ(f->rsp, fault_rsp);
    KT_EQ(get(u, D_EREGS + 15 * 8), at - 8);   /* rsp: as just after a call */
    uint64_t ret = 1;
    KT_EQ(vmo_read(u->stack, at - 8 - U_STACK, &ret, 8), OK);
    KT_EQ(ret, 0);
    KT_EQ(get(u, D_EREGS + 8 * 8), at);   /* rdi */
    uint64_t fl = get(u, D_EFLAGS);
    KT_EQ(fl & (RFLAGS_DF | RFLAGS_TF | RFLAGS_AC), 0);
    KT_ASSERT(fl & RFLAGS_IF);
    KT_EQ(get(u, D_EMXCSR) & 0xffffffff, EXC_MXCSR_INIT);
    KT_EQ(get(u, D_EFCW) & 0xffff, 0x037f);
    KT_EQ(get(u, D_EXMM0), 0);
    KT_EQ(get(u, D_EYMM0), 0);
    KT_EQ(get(u, D_EFS), 0x5eed5eed5eed5eedull);
    if (cpu_features.xsave) {
        const uint8_t *x = f->xsave;
        KT_EQ(*(const uint64_t *)(x + X_BV), xcr0_now());
        for (unsigned i = X_COMP; i < 576; i++)
            KT_EQ(x[i], 0);
    }
}

/* ---- the kill with no entry ------------------------------------------------ */

/* No entry: a fault kills the process with the line and code it always
 * had, byte for byte; int3 is a breakpoint now (its gate is DPL 3). */
KTEST(upcall_none_kills_as_before)
{
    struct upc u;
    upc_new(&u, "upc-none");
    set(&u, D_NOENTRY, 1);
    set(&u, D_FAULTER, AT(f_ud2));
    upc_start(&u, upc_generic);
    fault_is(killed(&u, "invalid opcode", AT(f_ud2_i), 0, ""), 6, OK);
    KT_EQ(get(&u, D_COUNT), 0);
    upc_end(&u);

    upc_new(&u, "upc-none-pf");
    set(&u, D_NOENTRY, 1);
    set(&u, D_FAULTER, AT(f_pfmap));
    upc_start(&u, upc_generic);
    fault_is(killed(&u, "page fault", AT(f_pfmap_i), U_HOLE, ""), 14, ERR_NOT_FOUND);
    upc_end(&u);

    upc_new(&u, "upc-none-bp");
    set(&u, D_NOENTRY, 1);
    set(&u, D_FAULTER, AT(f_int3));
    upc_start(&u, upc_generic);
    fault_is(killed(&u, "breakpoint", AT(f_int3_end), 0, ""), 3, OK);
    upc_end(&u);

    /* The kernel's own threads have no process: no entry to set. */
    KT_EQ(sysc_exception_entry_set(U_CODE), ERR_BAD_STATE);
    KT_EQ(sysc_exception_done(0, EXC_LEAVE), ERR_BAD_STATE);
}

/* ---- every register through a fault and a resume ------------------------- */

static void regs_patterns(struct upc *u)
{
    for (unsigned i = 0; i < 16; i++)
        for (unsigned w = 0; w < 4; w++)
            set(u, D_VPAT + i * 32 + w * 8, 0x0f0e0d0c0b0a0900ull + i * 0x100000000ull + w);
}

static uint64_t vpat(unsigned i, unsigned w)
{
    return 0x0f0e0d0c0b0a0900ull + i * 0x100000000ull + w;
}

/* A fault reaches the entry with every register, the vector state and
 * the flags in the frame; the entry gets what the ABI says; RESUME past
 * the instruction, with rbx and xmm2 edited in the frame, continues
 * with every register as the frame says. */
KTEST(upcall_regs_and_resume)
{
    struct upc u;
    upc_new(&u, "upc-regs");
    regs_patterns(&u);
    set(&u, D_ACTION, AT(act_edit));
    set(&u, D_SKIP, 2);
    upc_start(&u, upc_regs);
    exited(&u, 0);
    KT_EQ(get(&u, D_COUNT), 1);

    uint64_t at;
    struct exc_frame *f = frame_of(&u, &at);
    entry_ok(&u, f, at, U_STACK_TOP);
    KT_EQ(f->vector, 6);
    KT_EQ(f->error, 0);
    KT_EQ(f->addr, 0);
    KT_EQ(f->status, 0);
    KT_EQ(get(&u, D_ERIP), AT(upc_regs_fault));
    KT_EQ(f->rip, AT(upc_regs_after));   /* moved on by the entry */
    KT_EQ(f->rflags & ~(1ull << 16), FLAGS_PAT);   /* RF is the CPU's business */
    for (unsigned i = 0; i < 15; i++)
        KT_EQ(fr_reg(f, i), i == 11 ? 0x0123456789abcdefull : pat(i));   /* rbx: edited */
    /* The entry ran with the thread's registers (but rdi and rsp). */
    for (unsigned i = 0; i < 15; i++)
        if (i != 8)
            KT_EQ(get(&u, D_EREGS + i * 8), pat(i));
    /* The vector state as the program left it (xmm2's edit included). */
    const uint8_t *x = f->xsave;
    KT_EQ(*(const uint32_t *)(x + X_MXCSR), 0x3f80);
    uint32_t ao = avx_off();
    for (unsigned i = 0; i < 16; i++) {
        const uint64_t *xmm = (const uint64_t *)(x + X_XMM + i * 16);
        KT_EQ(xmm[0], i == 2 ? 0x0123456789abcdefull : vpat(i, 0));
        KT_EQ(xmm[1], i == 2 ? ~0x0123456789abcdefull : vpat(i, 1));
        if (ao) {
            const uint64_t *hi = (const uint64_t *)(x + ao + i * 16);
            KT_EQ(hi[0], i == 2 ? ~0x0123456789abcdefull : vpat(i, 2));
            KT_EQ(hi[1], vpat(i, 3));
        }
    }
    kfree(f);

    /* After the resume: what the frame said. */
    for (unsigned i = 0; i < 15; i++)
        KT_EQ(get(&u, D_AREGS + i * 8), i == 11 ? 0x0123456789abcdefull : pat(i));
    KT_EQ(get(&u, D_AREGS + 15 * 8), U_STACK_TOP);
    KT_EQ(get(&u, D_AFLAGS), FLAGS_PAT);
    KT_EQ(get(&u, D_ACS) & 0xffff, GDT_USER_CODE | 3);
    KT_EQ(get(&u, D_ASS) & 0xffff, GDT_USER_DATA | 3);
    KT_EQ(get(&u, D_AMXCSR) & 0xffffffff, 0x3f80);
    for (unsigned i = 0; i < 16; i++) {
        KT_EQ(get(&u, D_AVEC + i * 32), i == 2 ? 0x0123456789abcdefull : vpat(i, 0));
        KT_EQ(get(&u, D_AVEC + i * 32 + 8), i == 2 ? ~0x0123456789abcdefull : vpat(i, 1));
        if (ao) {
            KT_EQ(get(&u, D_AVEC + i * 32 + 16), i == 2 ? ~0x0123456789abcdefull : vpat(i, 2));
            KT_EQ(get(&u, D_AVEC + i * 32 + 24), vpat(i, 3));
        }
    }
    upc_end(&u);
}

/* RFLAGS' privileged bits in a frame are dropped and IF comes back on; CS
 * and SS are the user's whatever the frame (it has neither). */
KTEST(upcall_resume_strips_flags)
{
    struct upc u;
    upc_new(&u, "upc-flags");
    regs_patterns(&u);
    set(&u, D_ACTION, AT(act_flags));
    set(&u, D_SKIP, 2);
    upc_start(&u, upc_regs);
    exited(&u, 0);
    KT_EQ(get(&u, D_AFLAGS), FLAGS_PAT);   /* IOPL 0, NT and VIF/VIP off, IF on */
    KT_EQ(get(&u, D_ACS) & 0xffff, GDT_USER_CODE | 3);
    KT_EQ(get(&u, D_ASS) & 0xffff, GDT_USER_DATA | 3);
    upc_end(&u);
}

/* ---- every vector ----------------------------------------------------------- */

struct vcase {
    const char    *name;
    const uint8_t *code, *insn, *end;   /* f_x, f_x_i, f_x_end */
    uint32_t       vector;
    uint64_t       error;
    bool           trap;      /* RIP is past the instruction */
    uint16_t       clobbers;  /* registers the code sets itself (frame order bits) */
};

#define VC(n, f, vec, err, trap, clob) { n, f, f##_i, f##_end, vec, err, trap, clob }
#define R_RBP (1u << 10)
#define R_RAX (1u << 13)
#define R_RCX (1u << 14)

static const struct vcase vcases[] = {
    VC("de", f_div, 0, 0, false, R_RCX),
    VC("db-int1", f_int1, 1, 0, true, 0),
    VC("bp-int3", f_int3, 3, 0, true, 0),
    VC("bp-int-3", f_int3b, 3, 0, true, 0),
    VC("ud", f_ud2, 6, 0, false, 0),
    VC("ss", f_ss, 12, 0, false, R_RBP),
    VC("gp-hlt", f_hlt, 13, 0, false, 0),
    VC("gp-noncanonical", f_gpnc, 13, 0, false, R_RAX),
    VC("gp-int-80", f_gpint, 13, 0x80 << 3 | 2, false, 0),   /* the IDT, gate 0x80 */
    VC("mf", f_mf, 16, 0, false, 0),
    VC("xm", f_xm, 19, 0, false, 0),
};

/* One fault on its way: the entry (act_skip past it) and the frame. */
static void vector_case(const struct vcase *c)
{
    struct upc u;
    upc_new(&u, "upc-vector");
    set(&u, D_FAULTER, AT(c->code));
    set(&u, D_SKIP, c->trap ? 0 : (uint64_t)(c->end - c->insn));
    upc_start(&u, upc_generic);
    exited(&u, 0);
    KT_EQ(get(&u, D_COUNT), 1);
    KT_EQ(get(&u, D_DONE), DONE);
    uint64_t at;
    struct exc_frame *f = frame_of(&u, &at);
    if (f->vector != c->vector || f->error != c->error)
        ktest_fail("%s: vector %u error %lx, want %u %lx", c->name, f->vector, f->error,
                   c->vector, c->error);
    entry_ok(&u, f, at, U_STACK_TOP);
    KT_EQ(f->addr, 0);
    KT_EQ(f->status, 0);
    KT_EQ(get(&u, D_ERIP), c->trap ? AT(c->end) : AT(c->insn));
    for (unsigned i = 0; i < 15; i++)
        if (!(c->clobbers >> i & 1) && fr_reg(f, i) != pat(i))
            ktest_fail("%s: register %u is %lx", c->name, i, fr_reg(f, i));
    if (c->vector == 16)   /* the zero divide is pending in the x87 status */
        KT_EQ(*(const uint16_t *)(f->xsave + 2) & 0x84, 0x84);
    if (c->vector == 19)   /* the zero divide flag, the program's mask */
        KT_EQ(*(const uint32_t *)(f->xsave + X_MXCSR), 0x1d80 | 4);
    kfree(f);
    upc_end(&u);
}

KTEST(upcall_each_vector)
{
    for (size_t i = 0; i < sizeof(vcases) / sizeof(vcases[0]); i++)
        vector_case(&vcases[i]);
}

/* Single-stepping (TF set by popfq): vector 1 after the next instruction,
 * the frame's RFLAGS with TF, the entry's without; the entry clears it
 * and resumes. */
KTEST(upcall_single_step)
{
    struct upc u;
    upc_new(&u, "upc-step");
    set(&u, D_FAULTER, AT(f_tf));
    set(&u, D_ACTION, AT(act_untf));
    upc_start(&u, upc_generic);
    exited(&u, 0);
    KT_EQ(get(&u, D_COUNT), 1);
    uint64_t at;
    struct exc_frame *f = frame_of(&u, &at);
    entry_ok(&u, f, at, U_STACK_TOP);
    KT_EQ(f->vector, 1);
    KT_EQ(get(&u, D_ERIP), AT(f_tf_end));
    KT_ASSERT(get(&u, D_ERFL) & RFLAGS_TF);   /* (the entry cleared it in the frame) */
    kfree(f);
    upc_end(&u);
}

/* Page faults: the address, the error code and aspace_fault's status;
 * one on a kernel address always says "present". */
static void pf_case(const char *name, const uint8_t *code, const uint8_t *insn,
                    const uint8_t *end, uint64_t arg, uint64_t addr, uint64_t error,
                    status_t status, void (*more)(struct upc *))
{
    struct upc u;
    upc_new(&u, name);
    set(&u, D_FAULTER, AT(code));
    set(&u, D_ARG, arg);
    set(&u, D_SKIP, (uint64_t)(end - insn));
    if (more)
        more(&u);
    upc_start(&u, upc_generic);
    exited(&u, 0);
    KT_EQ(get(&u, D_DONE), DONE);
    uint64_t at;
    struct exc_frame *f = frame_of(&u, &at);
    entry_ok(&u, f, at, U_STACK_TOP);
    KT_EQ(f->vector, 14);
    if (f->addr != addr || f->error != error || f->status != status)
        ktest_fail("%s: address %lx error %lx status %d, want %lx %lx %d", name, f->addr,
                   f->error, f->status, addr, error, status);
    KT_EQ(get(&u, D_ERIP), AT(insn));
    kfree(f);
    upc_end(&u);
}

static struct vmo *more_vmo;   /* the test's VMO at U_MORE, for pf_more_* */

static void pf_more_ro(struct upc *u)
{
    map(u, more_vmo, U_MORE, PG, ASPACE_READ);
}

static void pf_more_shrunk(struct upc *u)
{
    map(u, more_vmo, U_MORE, 2 * PG, ASPACE_READ);
    KT_EQ(vmo_set_size(more_vmo, PG), OK);
}

static uint8_t kernel_word;

KTEST(upcall_page_faults)
{
    pf_case("upc-pf-map", f_pfmap, f_pfmap_i, f_pfmap_end, 0, U_HOLE, 4, ERR_NOT_FOUND, NULL);
    KT_EQ(vmo_create(2 * PG, 0, &more_vmo), OK);
    pf_case("upc-pf-ro", f_pfro, f_pfro_i, f_pfro_end, 0, U_MORE, 7, ERR_ACCESS_DENIED,
            pf_more_ro);
    pf_case("upc-pf-shrunk", f_load, f_load_i, f_load_end, U_MORE + PG, U_MORE + PG, 4,
            ERR_OUT_OF_RANGE, pf_more_shrunk);
    put(more_vmo);
    more_vmo = NULL;
    /* Kernel addresses: one mapped (the CPU says present), one not (it
     * doesn't); the frame says present for both. */
    uint64_t k = (uint64_t)&kernel_word;
    pf_case("upc-pf-kernel", f_load, f_load_i, f_load_end, k, k, 5, ERR_NOT_FOUND, NULL);
    uint64_t hole = VMAP_END + 0x10000000ull;
    if (vmm_translate(vmm_kernel_pml4(), hole) == UINT64_MAX)
        pf_case("upc-pf-khole", f_load, f_load_i, f_load_end, hole, hole, 5, ERR_NOT_FOUND,
                NULL);
    else
        kprintf("ktest upcall_page_faults: %lx is mapped: its case skipped\n", hole);

    /* Executing the data page (no execute): RIP is the page's; the entry
     * moves it on. */
    struct upc u;
    upc_new(&u, "upc-pf-nx");
    set(&u, D_FAULTER, AT(f_pfnx));
    set(&u, D_ACTION, AT(act_poke));
    set(&u, D_ARGOFF, F_RIP);
    set(&u, D_ARG, AT(upc_generic_after));
    upc_start(&u, upc_generic);
    exited(&u, 0);
    uint64_t at;
    struct exc_frame *f = frame_of(&u, &at);
    KT_EQ(f->vector, 14);
    KT_EQ(get(&u, D_ERIP), U_DATA + 0xf00);
    KT_EQ(f->addr, U_DATA + 0xf00);
    KT_EQ(f->error, 0x15);   /* present, user, fetch */
    KT_EQ(f->status, ERR_ACCESS_DENIED);
    kfree(f);
    upc_end(&u);
}

/* A pager that fails the page: SIGBUS's status (ERR_IO) reaches the entry. */
KTEST(upcall_pager_failure)
{
    struct upc u;
    upc_new(&u, "upc-pf-io");
    struct pager *pg;
    KT_EQ(pager_create(u.j, 16, 5 * NS_PER_S, &pg), OK);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 7, PG, &v), OK);
    map(&u, v, U_MORE, PG, ASPACE_READ);
    set(&u, D_FAULTER, AT(f_load));
    set(&u, D_ARG, U_MORE + 8);
    set(&u, D_SKIP, (uint64_t)(f_load_end - f_load_i));
    upc_start(&u, upc_generic);
    struct pager_request q;
    kt_pager_next(pg, &q);
    KT_EQ(q.key, 7);
    KT_EQ(pager_op(pg, 7, PAGER_OP_FAIL, 0, PG, ERR_IO), OK);
    exited(&u, 0);
    uint64_t at;
    struct exc_frame *f = frame_of(&u, &at);
    KT_EQ(f->vector, 14);
    KT_EQ(f->addr, U_MORE + 8);
    KT_EQ(f->status, ERR_IO);
    kfree(f);
    put(v);
    kt_pager_close(pg);
    upc_end(&u);
}

/* ---- refused frames ---------------------------------------------------------- */

struct badcase {
    uint64_t    off, val;
    bool        xor;
    const char *why;
    bool        xsave;   /* only on a CPU with XSAVE */
};

static const struct badcase bad_frames[] = {
    { F_COOKIE, 1, true, "exception_done: a frame that isn't this thread's", false },
    { F_SIZE, 64, true, "exception_done: a frame of the wrong size", false },
    { F_XSIZE, 64, true, "exception_done: a frame of the wrong size", false },
    { F_RIP, USER_TOP, false, "exception_done: a frame with a bad address", false },
    { F_RIP, 0x0000800000000000ull, false, "exception_done: a frame with a bad address", false },
    { F_RIP, 0x8000000000000000ull, false, "exception_done: a frame with a bad address", false },
    { F_RIP, 0xffffffff80000000ull, false, "exception_done: a frame with a bad address", false },
    { F_X + X_MXCSR, 1ull << 31, true, "exception_done: a frame with a bad vector state", false },
    { F_X + X_BV, 1ull << 9, true, "exception_done: a frame with a bad vector state", true },
    { F_X + X_COMP, 1ull << 63, true, "exception_done: a frame with a bad vector state", true },
    { F_X + X_COMP + 8, 1, true, "exception_done: a frame with a bad vector state", true },
    { F_X + 568, 1ull << 56, true, "exception_done: a frame with a bad vector state", true },
};

/* A frame that fails a check kills its process with the reason, the
 * frame's RIP and its address; never the kernel. */
KTEST(upcall_bad_frames_kill)
{
    for (size_t i = 0; i < sizeof(bad_frames) / sizeof(bad_frames[0]); i++) {
        const struct badcase *c = &bad_frames[i];
        if (c->xsave && !cpu_features.xsave)
            continue;
        struct upc u;
        upc_new(&u, "upc-bad");
        set(&u, D_FAULTER, AT(f_ud2));
        set(&u, D_ACTION, AT(c->xor ? act_xor : act_poke));
        set(&u, D_ARGOFF, c->off);
        set(&u, D_ARG, c->val);
        set(&u, D_SKIP, 2);
        upc_start(&u, upc_generic);
        uint64_t rip = c->off != F_RIP ? AT(f_ud2_i) + (c->xor ? 2 : 0) : c->val;
        uint64_t at = (U_STACK_TOP - 128 - EXC_FRAME_XSAVE - fpu_frame_size()) & ~63ull;
        fault_is(killed(&u, c->why, rip, at, ""), 13, OK);   /* Linux's SIGSEGV */
        KT_EQ(get(&u, D_COUNT), 1);
        KT_EQ(get(&u, D_DONE), 0);
        upc_end(&u);
    }

    /* A thread that was never given a frame has nothing to resume. */
    struct upc u;
    upc_new(&u, "upc-forge");
    struct vmo *v;
    KT_EQ(vmo_create(PG, 0, &v), OK);
    struct exc_frame fr = { .size = EXC_FRAME_XSAVE + fpu_frame_size(),
                            .xsave_size = fpu_frame_size(), .rip = AT(upc_generic_after) };
    KT_EQ(vmo_write(v, 0, &fr, sizeof(fr)), OK);
    uint32_t mx = EXC_MXCSR_INIT;
    KT_EQ(vmo_write(v, EXC_FRAME_XSAVE + X_MXCSR, &mx, 4), OK);
    map(&u, v, U_MORE, PG, ASPACE_READ);
    upc_start(&u, upc_forge);
    killed(&u, "exception_done: a frame that isn't this thread's", AT(upc_generic_after), U_MORE,
           "");
    put(v);
    upc_end(&u);
}

/* A resumed RSP is the thread's business, wherever it points: iretq takes
 * it, and the next fault, its frame unwritable there, is the old kill. */
KTEST(upcall_resume_wild_rsp)
{
    static const uint64_t rsps[] = { 0x8000000000000000ull, 0xffff800000000000ull, 0 };
    for (size_t i = 0; i < sizeof(rsps) / sizeof(rsps[0]); i++) {
        struct upc u;
        upc_new(&u, "upc-wildrsp");
        set(&u, D_FAULTER, AT(f_ud2));
        set(&u, D_ACTION, AT(act_poke));
        set(&u, D_ARGOFF, F_RSP);
        set(&u, D_ARG, rsps[i]);
        upc_start(&u, upc_generic);
        killed(&u, "invalid opcode", AT(f_ud2_i), 0, "");
        KT_EQ(get(&u, D_COUNT), 1);
        upc_end(&u);
    }
}

/* System calls with bad pointers return their errors, with an entry set
 * too: a user copy's fault is never an upcall. */
KTEST(upcall_bad_pointers_return)
{
    struct upc u;
    upc_new(&u, "upc-badptr");
    upc_start(&u, upc_badptr);
    exited(&u, 0);
    KT_EQ((int64_t)get(&u, D_RET), ERR_INVALID_ARGS);
    KT_EQ((int64_t)get(&u, D_RET2), ERR_INVALID_ARGS);
    KT_EQ(get(&u, D_COUNT), 0);
    upc_end(&u);
}

/* ---- the window, its end, UNHANDLED --------------------------------------- */

/* A fault inside the entry's window kills with the first fault's line. */
KTEST(upcall_fault_in_window_kills)
{
    struct upc u;
    upc_new(&u, "upc-again");
    set(&u, D_FAULTER, AT(f_pfmap));
    set(&u, D_ACTION, AT(act_again));
    upc_start(&u, upc_generic);
    /* The first fault's line, and its vector (not the ud2's). */
    fault_is(killed(&u, "page fault", AT(f_pfmap_i), U_HOLE, ", again in its exception entry"),
             14, ERR_NOT_FOUND);
    KT_EQ(get(&u, D_COUNT), 1);
    upc_end(&u);
}

/* UNHANDLED: the kill the fault would have been with no entry. */
KTEST(upcall_unhandled_kills_as_before)
{
    struct upc u;
    upc_new(&u, "upc-unhandled");
    set(&u, D_FAULTER, AT(f_pfmap));
    set(&u, D_ACTION, AT(act_unhandled));
    upc_start(&u, upc_generic);
    killed(&u, "page fault", AT(f_pfmap_i), U_HOLE, "");
    KT_EQ(get(&u, D_COUNT), 1);
    KT_EQ(get(&u, D_RET), 0);   /* never came back */
    upc_end(&u);

    upc_new(&u, "upc-unhandled");
    set(&u, D_FAULTER, AT(f_div));
    set(&u, D_ACTION, AT(act_unhandled));
    upc_start(&u, upc_generic);
    fault_is(killed(&u, "divide error", AT(f_div_i), 0, ""), 0, OK);
    upc_end(&u);
}

/* LEAVE ends the window (once; UNHANDLED needs it too); a fault after it
 * is a new upcall, not a kill. */
KTEST(upcall_leave)
{
    struct upc u;
    upc_new(&u, "upc-leave");
    set(&u, D_FAULTER, AT(f_ud2));
    set(&u, D_ACTION, AT(act_leave));
    upc_start(&u, upc_generic);
    exited(&u, 44);
    KT_EQ(get(&u, D_COUNT), 2);
    KT_EQ((int64_t)get(&u, D_RET), OK);
    KT_EQ((int64_t)get(&u, D_RET2), ERR_BAD_STATE);
    KT_EQ((int64_t)get(&u, D_RET3), ERR_BAD_STATE);
    KT_EQ((int64_t)get(&u, D_RET4), ERR_INVALID_ARGS);
    upc_end(&u);
}

/* exception_entry_set: past the user range refused; 0 removes the entry
 * (then a fault is the old kill). */
KTEST(upcall_entry_set)
{
    struct upc u;
    upc_new(&u, "upc-setargs");
    upc_start(&u, upc_setargs);
    killed(&u, "invalid opcode", AT(upc_setargs_i), 0, "");
    KT_EQ((int64_t)get(&u, D_RET), ERR_INVALID_ARGS);
    KT_EQ((int64_t)get(&u, D_RET2), ERR_INVALID_ARGS);
    KT_EQ((int64_t)get(&u, D_RET3), OK);
    KT_EQ((int64_t)get(&u, D_RET4), OK);
    KT_EQ(get(&u, D_COUNT), 0);
    upc_end(&u);
}

/* ---- what stays a kill ------------------------------------------------------- */

/* A frame that can't be written (RSP in nothing writable, or nowhere)
 * is the old kill, line for line. */
KTEST(upcall_unwritable_frame_kills)
{
    static const uint64_t rsps[] = { U_HOLE + PG, U_MORE + PG, 0x2000, 0x8000000000000000ull };
    for (size_t i = 0; i < sizeof(rsps) / sizeof(rsps[0]); i++) {
        struct upc u;
        upc_new(&u, "upc-nostack");
        struct vmo *v;
        KT_EQ(vmo_create(PG, 0, &v), OK);
        map(&u, v, U_MORE, PG, ASPACE_READ);
        set(&u, D_ARG, rsps[i]);
        upc_start(&u, upc_nostack);
        killed(&u, "invalid opcode", AT(upc_nostack_i), 0, "");
        KT_EQ(get(&u, D_COUNT), 0);
        put(v);
        upc_end(&u);
    }
}

/* Out of memory (the job's page limit) stays a kill with an entry. */
KTEST(upcall_out_of_memory_kills)
{
    struct upc u;
    upc_new(&u, "upc-oom");
    struct vmo *v;
    KT_EQ(vmo_create_for(u.j, PG, 0, &v), OK);
    map(&u, v, U_MORE, PG, ASPACE_READ);
    upc_start(&u, upc_oom);
    uint64_t deadline = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    while (!get(&u, D_READY) && uptime_ns() < deadline)
        thread_sleep_ms(1);
    KT_EQ(get(&u, D_READY), 1);
    KT_EQ(job_set_limit(u.j, JOB_LIMIT_PAGES, job_used(u.j, JOB_LIMIT_PAGES)), OK);
    set(&u, D_GO, 1);
    fault_is(killed(&u, "page fault: out of memory (or the job's page limit)", AT(upc_oom_i),
                    U_MORE, ""),
             14, ERR_NO_MEMORY);
    KT_EQ(get(&u, D_COUNT), 0);
    KT_EQ(job_set_limit(u.j, JOB_LIMIT_PAGES, JOB_NO_LIMIT), OK);
    put(v);
    upc_end(&u);
}

static handle_t insert(struct handle_table *t, struct kobject *obj, rights_t rights)
{
    struct khandle kh = khandle_from_new(obj, rights);
    handle_t h;
    KT_EQ(handle_insert(t, &kh, &h), OK);
    return h;
}

/* A page that doesn't match the approved program (the tamper check)
 * stays a kill with an entry: no handler may look at it or step over it. */
KTEST(upcall_tampered_page_kills)
{
    struct upc u;
    upc_new(&u, "upc-tamper");
    struct pager *pg;
    KT_EQ(pager_create(u.j, 16, 5 * NS_PER_S, &pg), OK);
    struct vmo *v;
    KT_EQ(pager_create_vmo(pg, 9, PG, &v), OK);
    uint8_t *page = kzalloc(PG);
    KT_ASSERT(page);
    memset(page, 0x5a, PG);
    struct vmo *tv;
    KT_EQ(vmo_create(VMO_VERIFIED_HASH_BYTES, 0, &tv), OK);
    uint8_t d[SHA256_BYTES];
    sha256(page, PG, d);
    KT_EQ(vmo_write(tv, 0, d, sizeof(d)), OK);
    struct handle_table t;
    handle_table_init(&t);
    t.job = u.j;
    kobject_ref(vmo_kobject(v));
    handle_t h = insert(&t, vmo_kobject(v), PAGER_VMO_RIGHTS);
    struct kobject *root = resource_root();
    kobject_ref(root);
    handle_t hr = insert(&t, root, RIGHTS_BASIC | RIGHT_ROOT_VMEX);
    handle_t ht = insert(&t, vmo_kobject(tv), RIGHTS_BASIC | RIGHT_READ);
    handle_t x;
    KT_EQ(sys_vmo_make_exec_verified(&t, h, hr, ht, 0, &x), OK);
    map(&u, v, U_MORE, PG, ASPACE_READ);
    set(&u, D_FAULTER, AT(f_load));
    set(&u, D_ARG, U_MORE);
    upc_start(&u, upc_generic);
    struct pager_request q;
    kt_pager_next(pg, &q);
    page[100] ^= 1;   /* not what was approved */
    struct pager_src src = { .addr = (uint64_t)(uintptr_t)page, .user = false };
    KT_EQ(pager_supply(pg, 9, 0, &src, PG), ERR_IO);
    fault_is(killed(&u, "page fault: a page doesn't match the approved program", AT(f_load_i),
                    U_MORE, ""),
             14, ERR_BAD_STATE);
    KT_EQ(get(&u, D_COUNT), 0);
    handle_table_destroy(&t);
    put(v);
    kfree(page);
    kt_pager_close(pg);
    upc_end(&u);
}

/* ---- threads ------------------------------------------------------------------ */

#define SPIN_THREADS 8
#define SPIN_ROUNDS  (100000 / SPIN_THREADS)

/* 100,000 faults and resumes on 8 threads, two to a CPU where there are
 * CPUs to spare: every frame holds its own thread's registers, vector
 * state and cookie (each thread's its own), every resume gives them back;
 * nothing leaks (the job comes back empty, the harness counts pages). */
KTEST(upcall_threads_stress)
{
    struct upc u;
    upc_new(&u, "upc-spin");
    set(&u, D_ROUNDS, SPIN_ROUNDS);
    set(&u, D_THREADS, SPIN_THREADS);
    uint32_t spare = cpu_count > 1 ? cpu_count - 1 : 0;
    upc_start_on(&u, upc_spin, 0, spare ? 1 : -1);
    for (unsigned i = 1; i < SPIN_THREADS; i++) {
        char name[8];
        ksnprintf(name, sizeof(name), "t%u", i);
        KT_EQ(uthread_create(u.p, name, &u.u[i]), OK);
        u.nu++;
        cpumask_t m;
        if (spare)
            cpumask_one(&m, 1 + (i / 2) % spare);
        KT_EQ(uthread_start(u.u[i], AT(upc_spin), U_STACK_TOP - i * 2 * PG, i, 0, 0,
                            spare ? &m : NULL),
              OK);
    }
    uint64_t t0 = uptime_ns();
    exited(&u, 0);
    kprintf("ktest upcall_threads_stress: %u faults resumed in %lu ms\n",
            SPIN_THREADS * SPIN_ROUNDS, (unsigned long)((uptime_ns() - t0) / NS_PER_MS));
    KT_EQ(get(&u, D_BAD), 0);
    KT_EQ(get(&u, D_FINISHED), SPIN_THREADS);
    for (unsigned i = 0; i < SPIN_THREADS; i++) {
        uint64_t c = get(&u, D_COOKIES + i * 8);
        KT_ASSERT(c);
        for (unsigned k = 0; k < i; k++)
            KT_ASSERT(get(&u, D_COOKIES + k * 8) != c);
    }
    upc_end(&u);
}

/* ---- exception stacks (SG2) ------------------------------------------------- */

#define ES_PAGES 4
#define ES_TOP   (U_MORE + ES_PAGES * PG)

static uint64_t fsize(void)
{
    return EXC_FRAME_XSAVE + fpu_frame_size();
}

/* A VMO mapped at U_MORE, and the program's exception stack [U_MORE,
 * U_MORE + size) (upc_setup sets it). */
static struct vmo *estack_new(struct upc *u, uint64_t size)
{
    struct vmo *v;
    KT_EQ(vmo_create(ES_PAGES * PG, 0, &v), OK);
    map(u, v, U_MORE, ES_PAGES * PG, ASPACE_READ | ASPACE_WRITE);
    set(u, D_ESTACK, U_MORE);
    set(u, D_ESIZE, size);
    return v;
}

/* The frame at `at`, in VMO v mapped at vbase. The caller kfrees it. */
static struct exc_frame *frame_in(struct vmo *v, uint64_t vbase, uint64_t at)
{
    struct exc_frame *f = kzalloc(fsize());
    KT_ASSERT(f);
    KT_EQ(vmo_read(v, at - vbase, f, fsize()), OK);
    return f;
}

/* A stack overflow into nothing mapped (a recursion off the stack's end)
 * reaches the entry on the exception stack, at its top; without one it is
 * the old kill, and process_info names the page fault. */
KTEST(upcall_estack_overflow)
{
    struct upc u;
    upc_new(&u, "upc-overflow");
    struct vmo *es = estack_new(&u, ES_PAGES * PG);
    set(&u, D_ACTION, AT(act_exit));
    upc_start(&u, upc_recurse);
    exited(&u, 42);
    KT_EQ((int64_t)get(&u, D_ESRET), OK);
    KT_EQ(get(&u, D_COUNT), 1);
    uint64_t at = get(&u, D_FRAME);
    KT_EQ(at, (ES_TOP - fsize()) & ~63ull);
    KT_EQ(get(&u, D_EREGS + 15 * 8), at - 8);   /* rsp: as just after a call */
    struct exc_frame *f = frame_in(es, U_MORE, at);
    KT_EQ(f->kind, EXC_FAULT);
    KT_EQ(f->vector, 14);
    KT_EQ(f->status, ERR_NOT_FOUND);
    KT_EQ(f->addr, U_STACK - 8);
    KT_EQ(f->rsp, U_STACK);
    KT_EQ(f->rip, AT(upc_recurse_i));
    kfree(f);
    put(es);
    upc_end(&u);

    upc_new(&u, "upc-overflow");
    upc_start(&u, upc_recurse);
    fault_is(killed(&u, "page fault", AT(upc_recurse_i), U_STACK - 8, ""), 14, ERR_NOT_FOUND);
    KT_EQ(get(&u, D_COUNT), 0);
    upc_end(&u);
}

/* With RSP outside the exception stack the frame goes to its top; a fault
 * in a handler running there (after LEAVE) nests below RSP on it. */
KTEST(upcall_estack_nested)
{
    struct upc u;
    upc_new(&u, "upc-estack");
    struct vmo *es = estack_new(&u, ES_PAGES * PG);
    set(&u, D_FAULTER, AT(f_ud2));
    set(&u, D_ACTION, AT(act_leave));
    upc_start(&u, upc_generic);
    exited(&u, 44);
    KT_EQ(get(&u, D_COUNT), 2);
    uint64_t first = (ES_TOP - fsize()) & ~63ull;
    struct exc_frame *f = frame_in(es, U_MORE, first);
    KT_EQ(f->vector, 6);
    KT_EQ(f->rip, AT(f_ud2_i));
    KT_EQ(f->rsp, U_STACK_TOP);
    kfree(f);
    uint64_t at = get(&u, D_FRAME);
    KT_EQ(at, (first - 8 - 128 - fsize()) & ~63ull);
    f = frame_in(es, U_MORE, at);
    KT_EQ(f->vector, 6);
    KT_EQ(f->rsp, first - 8);   /* the handler's, on the exception stack */
    kfree(f);
    put(es);
    upc_end(&u);
}

/* What doesn't fit is the old kill, never a write below the exception
 * stack's base: RSP inside it with no room left, a stack smaller than a
 * frame. RSP in nothing writable outside it is taken there.
 * thread_exception_stack's refusals. */
KTEST(upcall_estack_limits)
{
    struct upc u;
    upc_new(&u, "upc-nostack");
    struct vmo *es = estack_new(&u, ES_PAGES * PG);
    set(&u, D_ARG, U_MORE + 256);
    upc_start(&u, upc_nostack);
    fault_is(killed(&u, "invalid opcode", AT(upc_nostack_i), 0, ""), 6, OK);
    KT_EQ(get(&u, D_COUNT), 0);
    put(es);
    upc_end(&u);

    upc_new(&u, "upc-nostack");
    es = estack_new(&u, 512);
    set(&u, D_ARG, U_HOLE + PG);
    upc_start(&u, upc_nostack);
    killed(&u, "invalid opcode", AT(upc_nostack_i), 0, "");
    KT_EQ(get(&u, D_COUNT), 0);
    put(es);
    upc_end(&u);

    upc_new(&u, "upc-nostack");
    es = estack_new(&u, ES_PAGES * PG);
    set(&u, D_ARG, U_HOLE + PG);
    set(&u, D_ACTION, AT(act_exit));
    upc_start(&u, upc_nostack);
    exited(&u, 42);
    KT_EQ(get(&u, D_COUNT), 1);
    KT_EQ(get(&u, D_FRAME), (ES_TOP - fsize()) & ~63ull);
    put(es);
    upc_end(&u);

    upc_new(&u, "upc-esargs");
    upc_start(&u, upc_esargs);
    exited(&u, 0);
    KT_EQ((int64_t)get(&u, D_RET), ERR_INVALID_ARGS);    /* past the user range */
    KT_EQ((int64_t)get(&u, D_RET2), ERR_INVALID_ARGS);   /* size 0, base not 0 */
    KT_EQ((int64_t)get(&u, D_RET3), ERR_INVALID_ARGS);   /* below it */
    KT_EQ((int64_t)get(&u, D_RET4), OK);                 /* none */
    upc_end(&u);

    KT_EQ(sysc_thread_exception_stack(0, 0), ERR_BAD_STATE);   /* a kernel thread */
}

/* ---- raises (SG2) ------------------------------------------------------------- */

/* Wait until the data word at off is at least want: false past a bound
 * (there so a broken kernel fails, not a timing check). */
static bool wait_word(struct upc *u, uint64_t off, uint64_t want)
{
    uint64_t deadline = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
    while (get(u, off) < want) {
        if (uptime_ns() > deadline)
            return false;
        thread_sleep_ms(1);
    }
    return true;
}

/* Handles to thread t0 in its own process: D_HSELF with RIGHT_SIGNAL,
 * D_HNOSIG without. */
static void self_handles(struct upc *u)
{
    struct handle_table *t = process_handles(u->p);
    struct kobject *o = uthread_kobject(u->u[0]);
    kobject_ref(o);
    set(u, D_HSELF, insert(t, o, RIGHTS_BASIC | RIGHT_SIGNAL));
    kobject_ref(o);
    set(u, D_HNOSIG, insert(t, o, RIGHTS_BASIC));
}

static bool rip_in(struct upc *u, const uint8_t *from, const uint8_t *to)
{
    uint64_t rip = get(u, D_ERIP);
    return rip >= AT(from) && rip < AT(to);
}

/* A raise to a thread spinning in its own code on another CPU reaches its
 * entry at once (that CPU is interrupted: within 10 ms on an idle
 * machine), with RIP inside the loop and a frame of kind EXC_RAISED below
 * the red zone, one interrupt per latch; after RESUME it goes on, and the
 * next raise comes the same way. */
KTEST(upcall_raise_running)
{
    struct upc u;
    upc_new(&u, "upc-raise");
    set(&u, D_ACTION, AT(act_raised));
    int cpu = cpu_count > 1 ? (int)cpu_count - 1 : -1;
    kt_pin_self(0);
    upc_start_on(&u, upc_rspin, 0, cpu);
    bool ok = wait_word(&u, D_LOOPS, 1000);
    uint64_t latched0 = __atomic_load_n(&raise_latched, __ATOMIC_RELAXED);
    uint64_t kicked0 = __atomic_load_n(&raise_kicked, __ATOMIC_RELAXED);
    uint64_t worst = 0;
    bool kinds = true, rips = true;
    for (unsigned round = 1; ok && round <= 2; round++) {
        uint64_t t0 = rdtsc();
        ok = uthread_raise(u.u[0]) == OK && wait_word(&u, D_RCOUNT, round);
        int64_t d = (int64_t)(get(&u, D_RTSC) - t0);
        uint64_t ns = tsc_to_ns(d > 0 ? (uint64_t)d : 0);
        kprintf("ktest upcall_raise_running: raise %u taken after %lu us\n", round,
                (unsigned long)(ns / 1000));
        worst = ns > worst ? ns : worst;
        kinds &= get(&u, D_RKIND) == EXC_RAISED;
        rips &= rip_in(&u, upc_rspin_loop, upc_rspin_end);
    }
    kt_unpin_self();
    uint64_t latched = __atomic_load_n(&raise_latched, __ATOMIC_RELAXED) - latched0;
    uint64_t kicked = __atomic_load_n(&raise_kicked, __ATOMIC_RELAXED) - kicked0;
    set(&u, D_GO, 1);
    exited(&u, 0);
    KT_ASSERT(ok);
    KT_ASSERT(kinds);
    KT_ASSERT(rips);
    if (cpu >= 0)
        KT_IDLE_ASSERT(worst < 10 * NS_PER_MS);
    uint64_t at;
    struct exc_frame *f = frame_of(&u, &at);
    KT_GLOBAL_EQ(latched, 2);
    if (cpu >= 0)
        KT_IDLE_EQ(kicked, 2);
    KT_EQ(get(&u, D_COUNT), 2);
    KT_EQ(at, (U_STACK_TOP - 128 - fsize()) & ~63ull);
    KT_EQ(f->kind, EXC_RAISED);
    KT_EQ(f->vector, 0);
    KT_EQ(f->status, 0);
    KT_EQ(f->error, 0);
    KT_EQ(f->addr, 0);
    KT_EQ(f->rsp, U_STACK_TOP);
    KT_ASSERT(f->cookie);
    kfree(f);
    upc_end(&u);
}

/* An interrupt that comes in a call's last microseconds (interrupts off
 * from the dispatch to sysretq) is taken at the first instruction after
 * the call, and takes a latched raise there: the right thing (a later
 * interrupt, in the thread's own code), but what a test can't tell from a
 * raise taken on the call's return. A raise taken anywhere in the few
 * instructions between the call and upc_rwait ([from, to)) tries again: a
 * kernel that took raises on the call's return would land there on every
 * try, such an interrupt hardly ever twice (soak 2026-10-16, 8 CPUs under
 * load: upcall_raise_in_call once; `ktest=upcall_raise load=16` at 8 CPUs:
 * upcall_raise_in_window in its third loop). */
static bool taken_after_call(struct upc *u, const uint8_t *from, const uint8_t *to,
                             const char *test, unsigned tries)
{
    if (!rip_in(u, from, to) || tries >= 5)
        return false;
    kprintf("ktest %s: an interrupt at the call's return took the raise (try %u): again\n", test,
            tries);
    return true;
}

/* A raise to a thread blocked in a call (sleep_until, which
 * thread_interrupt would end) doesn't end it, and isn't taken on its
 * return: it comes at a later interrupt, in the thread's own code. */
KTEST(upcall_raise_in_call)
{
    for (unsigned tries = 1;; tries++) {
        struct upc u;
        upc_new(&u, "upc-rcall");
        set(&u, D_ACTION, AT(act_raised));
        uint64_t deadline = uptime_ns() + 200 * NS_PER_MS;
        set(&u, D_ARG, deadline);
        upc_start(&u, upc_rcall);
        KT_ASSERT(wait_word(&u, D_READY, 1));
        thread_sleep_ms(20);   /* it is in the call by now */
        KT_EQ(uthread_raise(u.u[0]), OK);
        KT_ASSERT(wait_word(&u, D_RCOUNT, 1));
        exited(&u, 0);
        if ((rip_in(&u, upc_rcall, upc_rcall_ret) && tries < 5) ||
            taken_after_call(&u, upc_rcall_ret, upc_rcall_end, "upcall_raise_in_call", tries)) {
            upc_end(&u);   /* a slow start (taken before the call), or the above: again */
            continue;
        }
        KT_EQ((int64_t)get(&u, D_RET), OK);   /* not ended */
        KT_EQ(get(&u, D_RET2), 0);            /* not taken on its return */
        KT_ASSERT(rip_in(&u, upc_rwait, upc_rwait_end));
        KT_ASSERT(uptime_ns() >= deadline);
        upc_end(&u);
        break;
    }
}

/* A raise while the thread is in its entry's window waits there, however
 * many interrupts come (latched once); after LEAVE it comes at the next. */
KTEST(upcall_raise_in_window)
{
    struct upc u;
    uint64_t latched0;
    for (unsigned tries = 1;; tries++) {
        upc_new(&u, "upc-rwin");
        set(&u, D_FAULTER, AT(f_ud2));
        set(&u, D_ACTION, AT(act_winspin));
        upc_start(&u, upc_generic);
        KT_ASSERT(wait_word(&u, D_READY, 1));
        latched0 = __atomic_load_n(&raise_latched, __ATOMIC_RELAXED);
        KT_EQ(uthread_raise(u.u[0]), OK);
        KT_EQ(uthread_raise(u.u[0]), OK);
        thread_sleep_ms(50);   /* ticks come meanwhile */
        KT_EQ(get(&u, D_RCOUNT), 0);
        KT_EQ(get(&u, D_COUNT), 1);
        set(&u, D_GO, 1);
        exited(&u, 0);
        if (!taken_after_call(&u, upc_rleave_ret, upc_rleave_end, "upcall_raise_in_window",
                              tries))
            break;
        upc_end(&u);
    }
    KT_GLOBAL_EQ(__atomic_load_n(&raise_latched, __ATOMIC_RELAXED) - latched0, 1);
    KT_EQ((int64_t)get(&u, D_RET), OK);
    KT_EQ(get(&u, D_RET2), 0);
    KT_EQ(get(&u, D_RCOUNT), 1);
    KT_EQ(get(&u, D_COUNT), 2);
    KT_EQ(get(&u, D_RKIND), EXC_RAISED);
    KT_ASSERT(rip_in(&u, upc_rwait, upc_rwait_end));
    upc_end(&u);
}

/* thread_raise needs RIGHT_SIGNAL. A thread may raise itself: it comes at
 * its next interrupt, not on the call's return. A thread gone:
 * ERR_BAD_STATE. */
KTEST(upcall_raise_rights)
{
    struct upc u;
    for (unsigned tries = 1;; tries++) {
        upc_new(&u, "upc-rself");
        set(&u, D_ACTION, AT(act_raised));
        upc_start(&u, upc_rself);
        self_handles(&u);
        set(&u, D_GO, 1);
        exited(&u, 0);
        if (!taken_after_call(&u, upc_rself_ret, upc_rself_end, "upcall_raise_rights", tries))
            break;
        upc_end(&u);
    }
    KT_EQ((int64_t)get(&u, D_RET), ERR_ACCESS_DENIED);
    KT_EQ((int64_t)get(&u, D_RET3), OK);
    KT_EQ(get(&u, D_RET2), 0);
    KT_EQ(get(&u, D_RCOUNT), 1);
    KT_ASSERT(rip_in(&u, upc_rwait, upc_rwait_end));
    KT_EQ(uthread_raise(u.u[0]), ERR_BAD_STATE);
    upc_end(&u);
}

/* A raise is dropped with no entry, or when its frame can't be written
 * (RSP in nothing writable): the thread goes on as if none came. */
KTEST(upcall_raise_dropped)
{
    struct upc u;
    upc_new(&u, "upc-rdrop");
    set(&u, D_NOENTRY, 1);
    upc_start(&u, upc_rspin);
    KT_ASSERT(wait_word(&u, D_LOOPS, 1000));
    KT_EQ(uthread_raise(u.u[0]), OK);
    thread_sleep_ms(30);
    KT_ASSERT(wait_word(&u, D_LOOPS, get(&u, D_LOOPS) + 1000));   /* still going */
    set(&u, D_GO, 1);
    exited(&u, 0);
    KT_EQ(get(&u, D_COUNT), 0);
    upc_end(&u);

    upc_new(&u, "upc-rdrop");
    set(&u, D_ARG, U_HOLE + PG);
    set(&u, D_ACTION, AT(act_raised));
    upc_start(&u, upc_rbadsp);
    KT_ASSERT(wait_word(&u, D_READY, 1));
    KT_EQ(uthread_raise(u.u[0]), OK);
    thread_sleep_ms(30);
    set(&u, D_GO, 1);
    exited(&u, 0);
    KT_EQ(get(&u, D_COUNT), 0);
    upc_end(&u);
}

/* A fault in a raise's window kills with its own line and ", in its
 * exception entry"; EXC_UNHANDLED there is ERR_BAD_STATE (a raise has no
 * kill to fall back on). */
KTEST(upcall_raise_window_ends)
{
    struct upc u;
    upc_new(&u, "upc-ragain");
    set(&u, D_ACTION, AT(act_again));
    upc_start(&u, upc_rspin);
    KT_ASSERT(wait_word(&u, D_LOOPS, 1000));
    KT_EQ(uthread_raise(u.u[0]), OK);
    fault_is(killed(&u, "invalid opcode", AT(act_again), 0, ", in its exception entry"), 6, OK);
    KT_EQ(get(&u, D_COUNT), 1);
    upc_end(&u);

    upc_new(&u, "upc-runhandled");
    set(&u, D_ACTION, AT(act_unhandled));
    upc_start(&u, upc_rspin);
    KT_ASSERT(wait_word(&u, D_LOOPS, 1000));
    KT_EQ(uthread_raise(u.u[0]), OK);
    exited(&u, 98);
    KT_EQ((int64_t)get(&u, D_RET), ERR_BAD_STATE);
    upc_end(&u);
}

#define STORM_THREADS 4
#define STORM_RAISERS 4
#define STORM_BURSTS  25
#define STORM_PER     25   /* each raiser raises each thread this often a burst: 10,000 */

struct storm {
    struct upc *u;
    uint32_t    go, done;   /* bursts started; raisers' bursts done (atomic) */
    uint32_t    refused;    /* raises that weren't OK (atomic) */
};

static void storm_raiser(void *arg)
{
    struct storm *s = arg;
    for (uint32_t b = 1; b <= STORM_BURSTS; b++) {
        while (__atomic_load_n(&s->go, __ATOMIC_ACQUIRE) < b)
            thread_sleep_ms(1);
        for (unsigned i = 0; i < STORM_PER; i++)
            for (unsigned k = 0; k < STORM_THREADS; k++)
                if (uthread_raise(s->u->u[k]) != OK)
                    __atomic_add_fetch(&s->refused, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s->done, 1, __ATOMIC_RELEASE);
    }
}

/* 10,000 raises from 4 kernel threads to 4 threads spinning in their own
 * code, in 25 bursts: every thread takes at least one upcall a burst,
 * every frame a raise's, no more upcalls than latches and no more CPU
 * interrupts than latches (a latched raise sends none), no panic. */
KTEST(upcall_raise_storm)
{
    struct upc u;
    upc_new(&u, "upc-rstorm");
    set(&u, D_THREADS, STORM_THREADS);
    uint32_t spare = cpu_count > 1 ? cpu_count - 1 : 0;
    upc_start_on(&u, upc_rstorm, 0, spare ? 1 : -1);
    for (unsigned i = 1; i < STORM_THREADS; i++) {
        char name[8];
        ksnprintf(name, sizeof(name), "t%u", i);
        KT_EQ(uthread_create(u.p, name, &u.u[i]), OK);
        u.nu++;
        cpumask_t m;
        if (spare)
            cpumask_one(&m, 1 + i % spare);
        KT_EQ(uthread_start(u.u[i], AT(upc_rstorm), U_STACK_TOP - i * 2 * PG, i, 0, 0,
                            spare ? &m : NULL),
              OK);
    }
    bool ok = wait_word(&u, D_READY, STORM_THREADS);
    uint64_t latched0 = __atomic_load_n(&raise_latched, __ATOMIC_RELAXED);
    uint64_t kicked0 = __atomic_load_n(&raise_kicked, __ATOMIC_RELAXED);
    struct storm s = { .u = &u };
    struct thread *raisers[STORM_RAISERS];
    for (unsigned r = 0; r < STORM_RAISERS; r++)
        raisers[r] = thread_create("upc-raiser", storm_raiser, &s, PRIO_DEFAULT + 2);
    for (uint32_t b = 1; ok && b <= STORM_BURSTS; b++) {
        uint64_t before[STORM_THREADS];
        for (unsigned k = 0; k < STORM_THREADS; k++)
            before[k] = get(&u, D_RCOUNTS + k * 8);
        __atomic_store_n(&s.go, b, __ATOMIC_RELEASE);
        uint64_t deadline = uptime_ns() + kt_patience_ms(5000) * NS_PER_MS;
        while (ok && __atomic_load_n(&s.done, __ATOMIC_ACQUIRE) < b * STORM_RAISERS) {
            ok = uptime_ns() < deadline;
            thread_sleep_ms(1);
        }
        for (unsigned k = 0; ok && k < STORM_THREADS; k++)
            ok = wait_word(&u, D_RCOUNTS + k * 8, before[k] + 1);
        if (!ok)
            kprintf("ktest upcall_raise_storm: burst %u: a thread took no raise\n", b);
    }
    __atomic_store_n(&s.go, STORM_BURSTS, __ATOMIC_RELEASE);   /* the raisers finish */
    for (unsigned r = 0; r < STORM_RAISERS; r++)
        thread_join(raisers[r]);
    set(&u, D_STOP, 1);
    exited(&u, 0);
    uint64_t latched = __atomic_load_n(&raise_latched, __ATOMIC_RELAXED) - latched0;
    uint64_t kicked = __atomic_load_n(&raise_kicked, __ATOMIC_RELAXED) - kicked0;
    uint64_t taken = 0;
    for (unsigned k = 0; k < STORM_THREADS; k++)
        taken += get(&u, D_RCOUNTS + k * 8);
    kprintf("ktest upcall_raise_storm: %u raises, %lu latched, %lu CPU interrupts, %lu taken\n",
            STORM_RAISERS * STORM_BURSTS * STORM_PER * STORM_THREADS, (unsigned long)latched,
            (unsigned long)kicked, (unsigned long)taken);
    KT_ASSERT(ok);
    KT_EQ(s.refused, 0);
    KT_EQ(get(&u, D_BAD), 0);
    KT_ASSERT(taken >= STORM_BURSTS * STORM_THREADS);
    KT_GLOBAL_ASSERT(taken <= latched);
    KT_GLOBAL_ASSERT(kicked <= latched);
    upc_end(&u);
}

/* ---- hostile (track SGR: docs/SG-REVIEW.md) ---------------------------------------- */

/* Start n threads of prog in u, thread i with RDI = i on its own two
 * pages of stack, two to a CPU past CPU 0 where there are CPUs to spare. */
static void start_threads(struct upc *u, const uint8_t *prog, unsigned n)
{
    uint32_t spare = cpu_count > 1 ? cpu_count - 1 : 0;
    upc_start_on(u, prog, 0, spare ? 1 : -1);
    for (unsigned i = 1; i < n; i++) {
        char name[8];
        ksnprintf(name, sizeof(name), "t%u", i);
        KT_EQ(uthread_create(u->p, name, &u->u[i]), OK);
        u->nu++;
        cpumask_t m;
        if (spare)
            cpumask_one(&m, 1 + (i / 2) % spare);
        KT_EQ(uthread_start(u->u[i], AT(prog), U_STACK_TOP - i * 2 * PG, i, 0, 0,
                            spare ? &m : NULL),
              OK);
    }
}

/* Kernel threads raising a process's threads at random until told to stop. */
struct crossing {
    struct upc *u;
    uint32_t    stop;   /* atomic */
    uint64_t    sent;   /* raises that were OK (atomic) */
};

static void cross_raiser(void *arg)
{
    struct crossing *c = arg;
    uint64_t x = rdtsc() | 1;
    for (unsigned n = 1; !__atomic_load_n(&c->stop, __ATOMIC_ACQUIRE); n++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        if (uthread_raise(c->u->u[x % c->u->nu]) == OK)
            __atomic_add_fetch(&c->sent, 1, __ATOMIC_RELAXED);
        if (!(n % 16))
            thread_sleep_ms(1);
    }
}

#define VSPIN_THREADS 8
#define VSPIN_ROUNDS  20000  /* 12 faults a round: about a second, so raises cross them */
#define CROSS_RAISERS 4

/* Faults on every vector user code can raise here (0; 1 by int1 and by
 * TF; 3; 6; 12; 13 by hlt and by a non-canonical address; 14 on nothing
 * and on read-only code; 16; 19 where the CPU raises it) from 8 threads at
 * once, two to a CPU, while 4 kernel threads raise them at random: every
 * fault's frame has the vector its thread expected, every frame (a
 * fault's or a raise's, which may come between an x87 error and the wait
 * that takes it, or with TF just set) has its own thread's registers and
 * cookie, and every thread finishes. */
KTEST(upcall_hostile_vectors)
{
    struct upc u;
    upc_new(&u, "upc-vspin");
    set(&u, D_ROUNDS, VSPIN_ROUNDS);
    set(&u, D_THREADS, VSPIN_THREADS);
    start_threads(&u, upc_vspin, VSPIN_THREADS);
    KT_ASSERT(wait_word(&u, D_READY, VSPIN_THREADS));
    struct crossing c = { .u = &u };
    struct thread *raisers[CROSS_RAISERS];
    for (unsigned r = 0; r < CROSS_RAISERS; r++)
        raisers[r] = thread_create("upc-crosser", cross_raiser, &c, PRIO_DEFAULT);
    uint64_t t0 = uptime_ns();
    exited(&u, 0);
    __atomic_store_n(&c.stop, 1, __ATOMIC_RELEASE);
    for (unsigned r = 0; r < CROSS_RAISERS; r++)
        thread_join(raisers[r]);
    kprintf("ktest upcall_hostile_vectors: %lu faults, %lu raises taken of %lu sent, in %lu ms\n",
            (unsigned long)get(&u, D_VFAULTS), (unsigned long)get(&u, D_RCOUNT),
            (unsigned long)c.sent, (unsigned long)((uptime_ns() - t0) / NS_PER_MS));
    KT_EQ(get(&u, D_BAD), 0);
    KT_EQ(get(&u, D_FINISHED), VSPIN_THREADS);
    KT_ASSERT(get(&u, D_RCOUNT) > 0);
    /* 10 a round at least (#MF and #XM may not come under TCG) */
    KT_ASSERT(get(&u, D_VFAULTS) >= 10ull * VSPIN_THREADS * VSPIN_ROUNDS);
    for (unsigned i = 0; i < VSPIN_THREADS; i++)
        for (unsigned k = 0; k < i; k++)
            KT_ASSERT(get(&u, D_COOKIES + k * 8) != get(&u, D_COOKIES + i * 8));
    upc_end(&u);
}

#define HOSTILE_FRAMES  10000
#define WITNESS_THREADS 4

/* 10,000 random frames given to RESUME (SIGNALS-PLAN section 5, SGR). A
 * program edits each frame it is given at random and resumes it: most
 * get random registers, RFLAGS and vector state (some TF, some RSP
 * anywhere) and RIP at a landing that checks it got exactly the frame's
 * registers, RFLAGS as USER_RFLAGS_OK lets them through, and xmm0; one in
 * 23 gets a bad cookie, size, XSAVE header or MXCSR, a RIP anywhere or
 * about the user range's top, or is resumed from a wrong address. Each
 * process dies with the kernel's line or runs on (a refused address comes
 * back and the frame goes on); the next one carries on the count.
 * Meanwhile a witness process's 4 threads fault on every vector with
 * their own patterns: neither ever sees the other's in a frame or a
 * register, and the kernel never panics. */
KTEST(upcall_hostile_frames)
{
    struct upc w;
    upc_new(&w, "upc-witness");
    set(&w, D_ROUNDS, 1ull << 40);
    set(&w, D_THREADS, WITNESS_THREADS);
    start_threads(&w, upc_vspin, WITNESS_THREADS);
    KT_ASSERT(wait_word(&w, D_READY, WITNESS_THREADS));
    uint64_t frames = 0, leaks = 0, bad = 0, refused = 0, landed = 0, procs = 0, kills = 0;
    uint64_t seed = rdtsc();
    while (frames < HOSTILE_FRAMES) {
        struct upc r;
        upc_new(&r, "upc-hrand");
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        set(&r, D_RNG, seed | 1);
        set(&r, D_HWANT, HOSTILE_FRAMES - frames);
        upc_start_on(&r, upc_hrand, 0, cpu_count > 1 ? 1 : -1);   /* beside the witness */
        struct process_info info = upc_wait(&r);
        uint64_t f = get(&r, D_HFRAMES);
        if (!info.killed)   /* the end: it gave all it was asked for */
            KT_ASSERT(info.exit_code == 0 && frames + f >= HOSTILE_FRAMES);
        kills += info.killed;
        frames += f;
        leaks += get(&r, D_HLEAK);
        bad += get(&r, D_HBAD);
        refused += get(&r, D_HREFUSED);
        landed += get(&r, D_HLANDED);
        procs++;
        upc_end(&r);
    }
    set(&w, D_STOP, 1);
    struct process_info wi = upc_wait(&w);
    kprintf("ktest upcall_hostile_frames: %lu frames in %lu processes (%lu killed), %lu landed, "
            "%lu refused; the witness took %lu faults\n",
            (unsigned long)frames, (unsigned long)procs, (unsigned long)kills,
            (unsigned long)landed, (unsigned long)refused, (unsigned long)get(&w, D_VFAULTS));
    KT_ASSERT(!wi.killed && wi.exit_code == 0);
    KT_EQ(get(&w, D_BAD), 0);
    KT_EQ(leaks, 0);
    KT_EQ(bad, 0);
    KT_ASSERT(landed > frames / 2);
    upc_end(&w);
}

#define FLOOD_RAISERS 4
#define FLOOD_EACH    10000

struct flood {
    struct upc *u;
    uint32_t    refused;   /* atomic */
};

static void flood_raiser(void *arg)
{
    struct flood *f = arg;
    for (unsigned i = 0; i < FLOOD_EACH; i++)
        if (uthread_raise(f->u->u[0]) != OK)
            __atomic_add_fetch(&f->refused, 1, __ATOMIC_RELAXED);
}

/* A raise flood at a thread in its window: 40,000 raises from 4 kernel
 * threads while it spins in its entry are one latch and at most one CPU
 * interrupt, and none enters there; after EXC_LEAVE exactly one comes,
 * in the thread's own code after the call. */
KTEST(upcall_hostile_window_flood)
{
    struct upc u;
    upc_new(&u, "upc-wflood");
    set(&u, D_FAULTER, AT(f_ud2));
    set(&u, D_ACTION, AT(act_winspin));
    upc_start(&u, upc_generic);
    KT_ASSERT(wait_word(&u, D_READY, 1));
    uint64_t latched0 = __atomic_load_n(&raise_latched, __ATOMIC_RELAXED);
    uint64_t kicked0 = __atomic_load_n(&raise_kicked, __ATOMIC_RELAXED);
    struct flood fl = { .u = &u };
    struct thread *raisers[FLOOD_RAISERS];
    for (unsigned r = 0; r < FLOOD_RAISERS; r++)
        raisers[r] = thread_create("upc-flooder", flood_raiser, &fl, PRIO_DEFAULT + 2);
    for (unsigned r = 0; r < FLOOD_RAISERS; r++)
        thread_join(raisers[r]);
    thread_sleep_ms(20);   /* ticks come meanwhile */
    uint64_t latched = __atomic_load_n(&raise_latched, __ATOMIC_RELAXED) - latched0;
    uint64_t kicked = __atomic_load_n(&raise_kicked, __ATOMIC_RELAXED) - kicked0;
    KT_EQ(get(&u, D_RCOUNT), 0);
    KT_EQ(get(&u, D_COUNT), 1);
    set(&u, D_GO, 1);
    exited(&u, 0);
    KT_EQ(fl.refused, 0);
    KT_GLOBAL_EQ(latched, 1);
    KT_GLOBAL_ASSERT(kicked <= 1);
    KT_EQ((int64_t)get(&u, D_RET), OK);
    KT_EQ(get(&u, D_RCOUNT), 1);
    KT_EQ(get(&u, D_COUNT), 2);
    KT_EQ(get(&u, D_RKIND), EXC_RAISED);
    KT_ASSERT(rip_in(&u, upc_rleave_ret, upc_rwait_end));
    upc_end(&u);
}

/* ---- the benchmark's line ------------------------------------------------------ */

/* bench.c's "a fault caught and resumed": n faults (a ud2) of a process
 * pinned to cpu, each caught by its exception entry and resumed past the
 * instruction, timed in ring 3 around the ud2 (lfence; rdtsc; lfence,
 * as bench's stamps) after 2000 untimed; their TSC cycles into out.
 * Returns how many (n, at most SAMPLES_MAX), 0 if the program didn't
 * end well. */
unsigned upcall_bench_cycles(uint32_t cpu, uint64_t *out, unsigned n);
unsigned upcall_bench_cycles(uint32_t cpu, uint64_t *out, unsigned n)
{
    if (n > SAMPLES_MAX)
        n = SAMPLES_MAX;
    struct upc u;
    upc_new(&u, "upc-bench");
    set(&u, D_ROUNDS, n);
    set(&u, D_WARM, 2000);
    upc_start_on(&u, upc_bench, 0, (int)cpu);
    struct process_info info = upc_wait(&u);
    if (info.killed || info.exit_code)
        n = 0;
    else
        KT_EQ(vmo_read(u.data, D_SAMPLES, out, n * 8ull), OK);
    upc_end(&u);
    return n;
}

/* The benchmark's program runs and times something. */
KTEST(upcall_bench_program)
{
    uint64_t c[64];
    KT_EQ(upcall_bench_cycles(kt_cur_cpu(), c, 64), 64);
    for (unsigned i = 0; i < 64; i++)
        KT_ASSERT(c[i] > 0 && c[i] < 1000000000ull);
}

#define RB_WARM 2000

/* bench.c's "a raise to its upcall": thread t0 spins pinned to cpu with
 * an exception entry; t1, pinned to `from`, raises it n times
 * (thread_raise), each once the last was taken, after RB_WARM untimed.
 * Each sample is the TSC cycles from just before t1's call to t0's entry.
 * Returns how many (n, at most SAMPLES_MAX - RB_WARM), 0 if the program
 * didn't end well. */
unsigned upcall_raise_bench_cycles(uint32_t cpu, uint32_t from, uint64_t *out, unsigned n);
unsigned upcall_raise_bench_cycles(uint32_t cpu, uint32_t from, uint64_t *out, unsigned n)
{
    if (n > SAMPLES_MAX - RB_WARM)
        n = SAMPLES_MAX - RB_WARM;
    struct upc u;
    upc_new(&u, "upc-rbench");
    set(&u, D_ROUNDS, RB_WARM + n);
    upc_start_on(&u, upc_rbspin, 0, (int)cpu);
    self_handles(&u);
    KT_EQ(uthread_create(u.p, "t1", &u.u[1]), OK);
    u.nu = 2;
    cpumask_t m;
    cpumask_one(&m, from);
    KT_EQ(uthread_start(u.u[1], AT(upc_rbench), U_STACK_TOP - 4 * PG, 0, 0, 0, &m), OK);
    if (object_wait_one(process_kobject(u.p), SIG_TERMINATED,
                        uptime_ns() + kt_patience_ms(30000) * NS_PER_MS, NULL) != OK)
        process_kill(u.p, PROCESS_KILLED_CODE, true);   /* a raise lost: don't hang */
    struct process_info info = upc_wait(&u);
    if (info.killed || info.exit_code)
        n = 0;
    else
        KT_EQ(vmo_read(u.data, D_SAMPLES + RB_WARM * 8, out, n * 8ull), OK);
    upc_end(&u);
    return n;
}

/* The raise's benchmark program runs and times something. */
KTEST(upcall_raise_bench_program)
{
    if (cpu_count < 2)
        return;   /* it needs two CPUs */
    uint64_t c[64];
    KT_EQ(upcall_raise_bench_cycles(cpu_count - 1, 0, c, 64), 64);
    for (unsigned i = 0; i < 64; i++)
        KT_ASSERT(c[i] > 0 && c[i] < 1000000000ull);
}
