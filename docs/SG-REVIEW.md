# SGR: the signals review (SG1 to SG5)

Track SGR of [SIGNALS-PLAN.md](SIGNALS-PLAN.md) (section 5): section 4's
security rules, and what the plan says the code does, read against what
SG1 to SG5 built, with what came after them: CBF1's checked copies, the
triage TB, FL1b and FL4 (signals in a fork child, `sig_alt_reapply`, a
vfork parent's signals after its wait, the notices on the signal
channel), and two findings of track CCS (a call boundary's handler gets
zeroed registers; the layer on small stacks). Read at main 401b5f3a by an
agent that wrote none of it.

- **The kernel** (`kernel/arch/x86_64/upcall.c`, the frame half of
  `fpu.c`, `user_trap_return` and `user_kill` in `uentry.c`,
  `thread_raise` in `sched/wait.c`, `uthread_raise`, `process_replace`'s
  clearing): fault upcalls, `exc_resume` (the cookie, the sizes, RIP below
  `USER_TOP`, the XSAVE header and MXCSR, RFLAGS masked, CS and SS forced,
  `iretq`), exception stacks, `thread_raise` and `EXC_RAISED`, the fault
  fields.
- **libc** (`user/posix/sys/sigentry.S`, `sigfault.c`, `signal.c`,
  `ucopy.c`, the signal thread, `fork.c`'s and `proc.c`'s signal parts):
  fault signals, `sigaltstack` and `SA_ONSTACK`, the entry's reasons
  (`SIG_ENTRY_COPY`, `RAISE`, `HANDLERS`), `__jam_sig_resume`, the quiet
  stretches of the entries musl calls directly, `uc_fault` first in
  `sig_fault`, the signal thread.

Severity, as in the other reviews: **High** is a hole (a frame or a raise
that gives a process another's state or the kernel's, or panics the
kernel), or a hang or loss a real program will meet; **Medium** is wrong
behaviour the system or a test tier will meet; **Low** is a wart or a
narrow race that nothing relies on yet.

Counts: **0 High in the code SG1 to SG5 built, 1 High rated (the layer
heap, CCS's, not fixed here), 2 Medium, 5 Low, 5 notes, 2 design
questions.** Every rule of section 4 holds in the kernel, by reading and
by the hostile ktests this track adds: no frame carries another
thread's or the kernel's state, a resumed frame can't set a privileged
flag, a selector, the FS base or a vector state the CPU would refuse,
and a raise needs the thread's `RIGHT_SIGNAL` and only enters the
target's own entry.

## Findings

| # | Sev | Where | What is wrong | How to make it happen | Outcome |
|---|---|---|---|---|---|
| 1 | High (rated, not fixed) | the layer's calls (fd.c's descriptions, procfs's buffers, ~200 allocations in ~30 files) | The layer allocates from the program's `malloc` inside its calls, so a call made by a handler isn't async-signal-safe: a handler that interrupted the program's allocator with its lock held (a crash inside PartitionAlloc, a SIGPROF in malloc) and calls `open`, `read` of `/proc`, `socket` ... waits for that lock for ever. Linux's calls never touch the program's heap. Chromium's crash path opens `/proc/self/status` (`BeingDebugged`) in its SIGSEGV handler, so a renderer crashing in its allocator hangs instead of dying (CCS's stack-trace death test, which allocates in a crashing `malloc`). | A SIGSEGV handler that calls `open()`, installed by a program whose fault comes inside its own `malloc` while the allocator's lock is held. | Not fixed: the layer heap is a track of its own (AGENTS's open items, CCS (a)). |
| 2 | Medium | `user/posix/sys/signal.c:600` (`run_handler`) | A handler run at a call's boundary (a signal sent while the thread was in a call, or by itself: `raise`, `abort`, `kill(getpid())`, a timer's) gets a ucontext whose registers are all 0, unless a raise inside the call kept some. Linux gives the registers of the call: RIP after it, RSP and RBP as the caller left them, the arguments in RDI .. R9 and the result in RAX. Crash reporters (crashpad, breakpad) and base's own handler walk the crashed thread from REG_RIP, REG_RSP and REG_RBP: on an `abort()` (every CHECK of a non-official build) they get nothing to walk. | `LoggingTest.CheckCausesDistinctBreakpoints` (CCS): the handler reads REG_RIP 0. posixtest's new `signal` case: `raise(SIGUSR1)` with an `SA_SIGINFO` handler that checks REG_RIP, REG_RSP, REG_RBP and REG_RDI. | To fix: musl's way into the layer (`__jam_syscall`, `__jam_syscall_cp`) records its caller's RIP, RSP and RBP in the call's `sig_frame`, and the boundary's handlers get those, the call's arguments in Linux's registers, RAX the result (the number before the call), RCX and R11 as `syscall` leaves them. Callee-saved RBX and R12 to R15 stay 0 (they would need the entry in assembly; an unwinder recovers them from the stack). Whether `LoggingTest` then passes is for the next base run: on Linux an `abort()` gives the same RIP (inside `raise`) for each of its three children, so if its CHECKs end in `abort()` here the test differs from Linux earlier than this. |
| 3 | Medium | `user/posix/sys/calls.c:85` (dispatch), CR-PLAN's CCS list item 1 | A handler on its alternate stack runs the layer's calls on that stack, and a call that takes a path needs 16 to 29 KiB of it (CCS's call graph); musl's `SIGSTKSZ` is 8 KiB and `sysconf(_SC_MINSIGSTKSZ)` is 4,968 bytes. Below a `malloc`'d alternate stack is the program's heap, not a guard page, so the overflow corrupts it silently. On Linux the call runs on the kernel's stack, so the same handler is fine. | A SIGSEGV handler with `SA_ONSTACK` on an 8 KiB `sigaltstack` that calls `open("/proc/self/status")` (Chromium's crash path does). | Not fixed: design question 1 (where the calls of a handler on its alternate stack run). |
| 4 | Low | `user/posix/sys/sigentry.S:211` (`__jam_sig_resume`, the resume in user mode of a frame made before the last fork) | It loads RSP from the frame and then RBX from the frame through RBX. A raise between the two writes its frame below the restored RSP's red zone: exactly where the frame being read lies (the kernel put it there the same way, or at the alternate stack's top where the new one goes too). The raise is resumed at once (the way out), but RBX is then read from the new frame: the frame's own address, not the interrupted RBX. | A child forked inside a handler, whose thread is raised (a signal to it while busy) at that one instruction boundary as the handler returns. Too narrow to hit by a test (one instruction). | To fix: RBX is read into a word beside `resume_rip` before RSP moves, and loaded from there last. |
| 5 | Low | `user/posix/sys/procfs.c:401` (`__jam_dladdr`) | An entry musl calls directly (not through a call) that isn't a quiet stretch: `proc_exe` takes `exe_lock` and, the first time, allocates and looks up files under it. A raise there runs a handler with the lock held; one that calls `dladdr` or reads `/proc/self/exe` waits for ever. TB marked the others (`__jam_clone`, `__jam_posix_spawn`, `__jam_resolve`, `__jam_ttyname`, `__jam_clone_process`). | A SIGPROF handler that symbolizes with `dladdr` while the thread's first `dladdr` is finding the program's path. One raise inside one lock's hold: not practical to test. | To fix: `__jam_dladdr` is a quiet stretch. (`__jam_unmapself_prepare` needs none: musl blocks every signal before it, and its first step unlists the thread. `__jam_set_thread_area` runs before the thread is listed.) |
| 6 | Low | `user/posix/sys/calls.c:85`, CR-PLAN's CCS list item 2 | A thread made with a stack under ~32 KiB (`pthread_attr_setstacksize` takes down to musl's 2 KiB) overflows it in the layer's calls that take a path. Chromium's threads have 256 KiB and more. | `pthread_attr_setstacksize(&a, 16384)`, then `open()` in the thread. | Not fixed: design question 1 would serve it (the same switch). |
| 7 | Low | `user/posix/sys/sigfault.c` (`sig_fault`, `sig_fault_run`), CR-PLAN's CCS list item 3 | The fault and raise entries' C costs about 3.4 KiB a level on the stack it runs on, beside the kernel's 1 KiB frame; a clone child's fn on gtest's two pages (8 KiB) fits one level and a small handler. | A raise to a clone child's thread while fn runs on its two pages, with a handler deeper than ~3 KiB. | Not fixed: within what Linux's own frame costs (~1 to 3 KiB with AVX); the plan's `sig_alt_min` already counts it. |
| 8 | Low | `user/posix/sys/spawn.c:631` and the other direct entries, CR-PLAN's CCS list item 4 | The entries musl calls directly run on their caller's stack: a clone child's `posix_spawn` on two pages overflows. | `clone(fn, two pages)` whose fn calls `posix_spawn`. | Not fixed: design question 1 (the same switch, for the direct entries too). |
| 9 | Note | `user/posix/sys/sigentry.S:160` | The resume in user mode (a frame made before the last fork) restores the arithmetic flags and DF only: TF, AC and ID set by the handler are dropped, where the kernel's `RESUME` takes them (`USER_RFLAGS_OK`). | | No change: a forked child single-stepping out of a handler of its parent's thread is nothing anyone does. |
| 10 | Note | `user/posix/sys/signal.c:1086` (`sig_raise_begin`, `kept`) | A raise inside a call keeps the registers it found, which are the layer's (RIP in the layer's code, RSP on its frames, gone by the time the boundary's handler runs). As on Linux for a signal that comes while a thread runs libc's code, but a stack copier given that RSP copies dead frames. | | No change. With finding 2 the boundary's handler gets the call's registers when no raise kept any. |
| 11 | Note | `kernel/arch/x86_64/upcall.c:114` (the cookie) | The cookie is the thread's for its whole life, and a resumed frame stays in the dead stack below RSP until overwritten: one read of the stack gives the cookie for every later forged frame. Q4 asked for "a read of the stack too", which this is. | | No change (Q4's design). |
| 12 | Note | `kernel/arch/x86_64/upcall.c:245` (`exc_resume`) | `RESUME` needs no window: any frame with the thread's cookie and the kernel's sizes is resumed, as `rt_sigreturn` takes any frame. A latched raise waits for the next interrupt after it (up to a tick). | | No change: both documented (SG3, SG5). |
| 13 | Note | `user/posix/sys/proc.c` (lx_child_packet), `user/posix/sys/fork.c:358` (`lx_vfork`) | FL4's notices and vfork's wait, read for signals: a child's notice can only change what the parent knows of that child (the writer's pid checked, junk dropped, an oversized message ends listening to that child: its own loss); the vfork parent waits inside its call (`calls` > 0), so a signal meanwhile interrupts the futex wait, which waits again, and its handler runs at vfork's boundary, as SG5's routing says. A forked child's signal state (sig_fork: dispositions and mask kept, nothing pending, the entry set again, `sig_alt_reapply`, `sig_fault_forked`) is as FORK-PLAN 1.7 says. | | No change. |

## The kernel, rule by rule (section 4)

- **What leaves the kernel.** `frame_write` builds the frame in a zeroed
  buffer from the trap frame and `fpu_frame_save`, which copies only the
  fields the CPU defines from the thread's own area just saved (FOP, FIP
  and FDP only with an x87 exception pending, so an AMD CPU's stale
  pointers of another thread never show). A page fault at or above
  `USER_TOP` always says "present". The frame goes to the thread's own
  stack or its own exception stack by `copy_to_user`. ktest
  `upcall_hostile_frames` (below) looks for another process's patterns in
  every frame and every resumed register: none.
- **What `RESUME` trusts.** Read once into a kernel buffer; the cookie,
  `size` and `xsave_size` the kernel's; RIP below `USER_TOP` (so `iretq`
  never takes a non-canonical RIP in ring 0); RFLAGS through
  `USER_RFLAGS_OK` (no IOPL, NT, RF, VM, VIF, VIP) with IF and bit 1 set;
  CS and SS the user selectors, never the frame's; the FS and GS bases
  untouched; `fpu_frame_ok`: MXCSR within the CPU's mask, XSTATE_BV within
  XCR0 (user components only: no PKRU, no supervisor state), XCOMP_BV and
  all 48 reserved header bytes 0, so `XRSTOR` (from the 64-byte-aligned
  thread area, after a copy) can't fault in ring 0. A pending x87 or SIMD
  exception in a loaded state faults in user mode: the kernel's own x87 use
  is `fnstcw` and `stmxcsr`, which don't wait. RSP is the thread's
  business (`upcall_resume_wild_rsp`). 10,000 random frames
  (`upcall_hostile_frames`) die or run on; no panic.
- **Raises.** `thread_raise` needs `RIGHT_SIGNAL` on the thread's handle
  (`upcall_raise_rights`), latches one flag (a flood is one upcall and at
  most one IPI: `upcall_raise_storm`, and `upcall_hostile_window_flood`
  inside the window), is taken only on the interrupt and exception return
  to ring 3, never inside the window, and with no entry or no room is
  dropped. `process_replace` clears the window, the cookie, the exception
  stack and the latch, so a new image never gets the old one's frames.
- **What stays a kill.** `ERR_BAD_STATE` and `ERR_NO_MEMORY` page faults
  never reach the entry (`status_delivers`); NMI never reaches
  `user_trap_return`; #DB from ring 3 is moved off its IST stack before
  `user_fault` runs; double fault and machine check panic as before.

## libc, briefly

The window holds: from the entry to `EXC_LEAVE` only the frame, the stack
below it and the thread's own record are touched (`uc_fault` touches the
frame and the table only, and runs for threads with no thread-local
storage too; the signal thread is told before any thread-local read). The
dispositions are read under `sig_lock` with the holder check, so a fault
while holding it is `UNHANDLED`, not a deadlock. TB's way in and out
(`struct sig_tail`) closes the window between `EXC_LEAVE` and the handler
and between the handler and the resume; `__jam_sig_resume` looks at
`again` last, and a raise after the look raises the thread again. The
routing's Dekker pair (pending bit, then `calls`) has no gap: a thread
that entered a call after the sender looked sees the pending bit in
`enter()`. A handler that `siglongjmp`s out of a boundary leaves `calls`
right (it was decremented around the delivery).

## Design questions

1. **Where a handler's calls run when it is on a small stack** (findings 3,
   6 and 8). On Linux a call never uses the caller's stack beyond the
   return address; here a call that takes a path needs up to ~29 KiB.
   - (a) **A layer stack per thread that needs one** (recommended): mapped
     by `sigaltstack` when it is given a stack under 64 KiB, and by thread
     creation under 64 KiB, as CCS maps one for a clone child; calls.c's
     boundary (the one test CCS added) moves a call onto it when RSP is on
     the small stack, and below the current RSP if the thread is on it
     already (a fault in a call, then a handler's call). 64 KiB of address
     space per such thread, committed only as used; nothing for threads
     with ordinary stacks.
   - (b) **Smaller frames**: move the 4 KiB path buffers off the stack
     (CBF6's files) so a path call fits ~8 KiB. Helps every case, but a
     handler on a 4,968-byte stack still can't make one.
   - (c) **Leave it**: Chromium's own threads have big stacks; its crash
     handler's alternate stack is what matters, and that is (a)'s case.
2. **The boundary's callee-saved registers** (finding 2). RBX and R12 to
   R15 stay 0 in a boundary handler's ucontext. Giving them means entering
   the layer through a few instructions of assembly that store them in the
   call's frame (seven stores a call). Recommended: not until a crash dump
   needs them (an unwinder recovers them from the stack by the CFI).
