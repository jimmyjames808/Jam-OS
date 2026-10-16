/* libos's fork (fork.c, fork_entry.S), as the assembly and the modules
 * whose state a forked child makes fresh see it. */
#pragma once

/* The block jam_fork saves on the forking thread's stack (fork_entry.S),
 * which the child finds at the same address in its copy: the
 * callee-saved registers, the stack pointer and the return address the
 * call returns with, MXCSR and the x87 control word; fork.c adds the FS
 * base and the C library's context. */
#define FORK_B_RBX     0
#define FORK_B_RBP     8
#define FORK_B_R12     16
#define FORK_B_R13     24
#define FORK_B_R14     32
#define FORK_B_R15     40
#define FORK_B_RSP     48
#define FORK_B_RIP     56
#define FORK_B_MXCSR   64
#define FORK_B_FPUCW   68
#define FORK_B_FS_BASE 72
#define FORK_B_CTX     80
#define FORK_BLOCK     88   /* 8 more than a multiple of 16: the call below it is aligned */

#ifndef __ASSEMBLER__
#include <os.h>

struct fork_block {
    uint64_t rbx, rbp, r12, r13, r14, r15;
    uint64_t rsp;        /* the caller's, as the call returns */
    uint64_t rip;        /* where the call returns to */
    uint32_t mxcsr;
    uint16_t fpucw;
    uint16_t pad;
    uint64_t fs_base;    /* jam_fork_args.fs_base */
    void    *ctx;        /* jam_fork_args.ctx */
};

/* The forked child's first instruction (fork_entry.S). */
void jam_fork_trampoline(void);
/* What fork_entry.S calls: in the parent with the block it saved, in the
 * child on the trampoline with its startup channel and its copy of it. */
int64_t jam_fork_parent(struct fork_block *b, const struct jam_fork_args *a);
void    jam_fork_child(handle_t ch, struct fork_block *b);

/* The C library's hooks (jam_fork_set_libc, kept by start.c), or NULL. */
const struct jam_fork_libc *jam_fork_hooks(void);

/* In the child, before jam_fork returns 0 (fork.c's order): each module
 * drops what its copy of the parent's memory says it holds, without
 * closing anything (those values were the parent's) and without freeing
 * memory (the C library's allocator may still be locked), and makes its
 * locks fresh. */
/* start.c: the table startup_handle reads becomes these n handles by
 * role, and ch the startup channel. */
void startup_forked(handle_t ch, const handle_t *hs, const uint32_t *roles, unsigned n);
/* ns.c: the namespace empty until the parent's copy of it comes on the
 * new SR_NS; svc_get's cache empty. */
void ns_forked(void);
/* fs.c: fs_readdir's kept batch dropped. */
void fs_forked(void);
/* fdmsg.c: the hand-over's buffer free. */
void fdmsg_forked(void);

/* libos's C library (native programs only: libjam.a leaves these files
 * out), whose fork hooks startmain.c sets. heap.c: its lock taken before
 * the clone and given back after it in the parent, made fresh in the
 * child; stdio.c: printf looks for SR_STDOUT again. */
void heap_fork_prepare(void);
void heap_fork_parent(void);
void heap_fork_child(void);
void stdio_forked(void);
#endif
