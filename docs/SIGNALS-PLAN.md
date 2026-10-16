# SG plan: faults and signals to a thread

Faults are not signals on Jam OS. A user thread that faults where its
address space can't serve it has its whole process killed by the kernel
(`kill_current` in `kernel/arch/x86_64/uentry.c`), so no SIGSEGV, SIGBUS,
SIGILL or SIGFPE handler ever runs. And M13's signals
([M13-PLAN](M13-PLAN.md#6-signals) and its "B4 notes") are libc's,
delivered at a call boundary: a thread busy in its own code runs a handler
only at its next call, and the handler's `ucontext_t` is empty. Chromium
and V8 lean on both: V8's WebAssembly trap handler, its CPU profiler,
Chromium's sampling profiler and its crash stack dumps. CR-PLAN's risk 6
([Risks](CR-PLAN.md#risks)) worked around them: jitless, no profiler,
`--no-wasm-trap-handler`, no crash dumps.

**The owner's rule (2026-10-16, given for `fork`, applied here): build the
correct layer instead of patching or disabling Chromium's features.** This
plan is that layer: a fault reaches a handler on the faulting thread with
Linux's `siginfo_t` and a full `ucontext_t`, and a signal reaches a chosen
thread wherever it is.

It is M13's question 3 option B (a kernel upcall) added under option A,
not instead of it ([M13-PLAN Q3](M13-PLAN.md#q3-signals)). The signal
thread, routing, default actions in the sender, the call boundary,
`thread_interrupt`, `EINTR` and `SA_RESTART` all stay. The upcall is used
only where A can't reach: a fault, and a thread running its own code.

SG1 is built (the kernel's part of section 1: `exception_entry_set` 170,
`exception_done` 171, `struct exc_frame` in `<jam/abi.h>`,
kernel/arch/x86_64/upcall.c, ktests `upcall_*`); SG2 to SG6 and the
review SGR are not. Tracks are in section 5; the questions for the owner
in section 6, each with a recommendation, which the plan follows until he
says otherwise. Names and numbers of calls and structures below are
proposals where SG1 didn't settle them.

What SG1 settled differently from the text below, or added: the frame
keeps the registers in Linux's gregs order (REG_R8 .. REG_EFL) so libc
copies them in one go; the frame's XSAVE area has every component of
XCR0 written out (XSTATE_BV = XCR0) whatever the save left in its
initial state, and FOP, FIP and FDP only with an x87 exception pending;
`EXC_RESUME` also checks the frame's size and xsave_size are the
kernel's, and that the thread was ever given a frame (its cookie is made
at its first upcall); `EXC_LEAVE` and `EXC_UNHANDLED` outside the window
are `ERR_BAD_STATE`; an unreadable frame to `EXC_RESUME` is
`ERR_INVALID_ARGS`. int3's gate is now DPL 3, so `int3` from user code
is vector 3 with RIP after it (it was a general protection fault). The
alignment check (17) can't happen yet: CR0.AM is off, so AC in user code
does nothing.

## Where things stand

- **The kernel.** Every exception from ring 3 lands in `trap_dispatch`
  (`kernel/arch/x86_64/trap.c`). A page fault goes to `trap_page_fault`
  (`uentry.c`), which asks `aspace_fault`: resolved, the access is retried
  (demand paging, a pager's wait); not resolved, `kill_current` with the
  reason from `fault_reason` (nothing mapped or not with that access; past
  the end of a shrunk VMO, "bus error"; the file's pager failed, is gone
  or didn't answer; out of memory or the job's page limit; a page that
  doesn't match the approved program). Every other exception from ring 3
  (divide error, invalid opcode, general protection, `int3`, ...) goes to
  `user_fault_kill`. `kill_current` prints `user: process "x" killed: <why>
  at rip ..., address ... (thread "y")` and kills the process with
  `PROCESS_KILLED_CODE`. Its comment says it: "there are no exception
  channels". A fault in the kernel is a user copy's fixup (the call returns
  an error) or a panic.
- **libc** (M13's B4: `user/posix/sys/signal.c`, `sig.h`, `sigthread.c`,
  `sigcalls.c`). A signal thread per process receives what comes from
  outside and routes each signal to a thread that doesn't block it;
  default actions happen at once in the sender; handlers run on the
  target thread at a call boundary (`sig_call`), a blocked wait ended by
  `thread_interrupt`. A handler gets a `siginfo_t` (signo, code, pid, ...)
  and a zeroed `ucontext_t` with `uc_sigmask` set. `sigaltstack` is
  accepted and says disabled; `SA_ONSTACK` is accepted. `waitpid` reports
  a killed process as `WTERMSIG` SIGKILL (`wait_status` in
  `user/posix/sys/proc.c`), so a crash and a `kill -9` look the same.

What needs more than that:

| Who | What it does | What it needs | Today |
|---|---|---|---|
| V8's WebAssembly trap handler (`src/trap-handler/`) | Wasm loads and stores carry no bounds checks; one out of bounds faults in a guard region, and the SIGSEGV handler turns the fault into a Wasm trap by sending the thread to a landing pad (section 3) | SIGSEGV on the faulting thread with `si_code` > 0 and `si_addr`; the ucontext's RIP and R10 written by the handler and taken on its return | killed; the workaround is `--no-wasm-trap-handler` (bounds checks in the code, slower) |
| V8's CPU profiler (`src/libsampler/sampler.cc`) | a sampler thread sends SIGPROF with `pthread_kill` to the thread running JS (handler flags `SA_RESTART \| SA_SIGINFO \| SA_ONSTACK`); the handler reads `gregs[REG_RIP]`, `REG_RSP` and `REG_RBP` | a signal that stops a thread wherever it is, and its registers at that point | the handler runs at the thread's next call with an empty ucontext: profiler off |
| Chromium's sampling profiler (`base/profiler/stack_copier_signal.cc`) | `syscall(SYS_tgkill, getpid(), tid, SIGURG)` (handler `SA_RESTART \| SA_SIGINFO`); the handler copies `uc_mcontext` and the thread's stack, then wakes the sampler through a futex | the same | the same |
| Chromium's crash stack dump (`base/debug/stack_trace_posix.cc`, `EnableInProcessStackDumping`) | handlers for SIGILL, SIGABRT, SIGFPE, SIGBUS, SIGSEGV (`SA_RESETHAND \| SA_SIGINFO`) print the signal, its `si_code`'s name (`SEGV_MAPERR`, `BUS_ADRERR`, ...), `si_addr`, every register from `gregs` (`REG_TRAPNO`, `REG_ERR` and `REG_CR2` too) and a stack trace, then send the signal to themselves again with its siginfo (`rt_tgsigqueueinfo`, else `raise`) under an `alarm` | fault signals with siginfo and ucontext; a re-raise to itself | SIGABRT's runs (CR-PLAN's "BT notes"); the fault ones never do |
| Chromium's first-chance hook | the stack dump's handler (and Crashpad's, when built) offers a fault first to V8's `TryHandleWebAssemblyTrapPosix`, so V8's own handler isn't installed beside it (*unsure* which file wires it in 155) | as V8's handler | |
| Rust's std (Rust ports; *unsure* any of Chromium's Rust, whose `main` and threads are C++'s) | a SIGSEGV and SIGBUS handler and a `sigaltstack` per thread it starts print "thread 'x' has overflowed its stack" and abort | a guard-page fault on an alternate stack | killed, no message |
| Chromium's `TerminationStatus` | a child ended by SIGSEGV, SIGILL, SIGBUS, SIGFPE or SIGABRT is "crashed", by SIGKILL "killed" (the sad tab and the crash counts say which) | `WTERMSIG` of the fault | every fault reads as SIGKILL |
| Crashpad (out-of-process minidumps) | the crashing process's handler asks `crashpad_handler`, which reads its memory and threads with `ptrace` | a debugger's power over another process | not this plan (Q5) |

## How others do it

- **Linux**: the kernel builds the signal frame (`rt_sigframe`: a
  ucontext with every register, the siginfo, the XSAVE area) on the
  thread's stack or its `sigaltstack`, and enters the handler on the
  faulting thread; `rt_sigreturn` restores whatever the frame then says.
  A thread-directed signal (`tgkill`) is delivered at the thread's next
  return to user mode; an IPI hurries a running thread there. The kernel
  knows the dispositions and the masks.
- **Zircon**: a fault stops the thread in the kernel and sends an
  exception to the thread's, then the process's, then the job's
  exception channel; a handler thread (often in another process: a
  debugger, the crash service) reads and writes the stopped thread's
  registers (`zx_thread_read_state`, `zx_thread_write_state`) and
  resumes it. There are no signals; POSIX layers build them on top.
- **macOS**: Mach exception ports (a handler thread, often another
  process's: the crash reporter, a debugger) first, then BSD signals on
  the faulting thread. V8's trap handler there catches SIGBUS.

Jam OS takes Linux's shape for the delivery and keeps POSIX itself in
libc, as M13 does: the kernel knows nothing of signal numbers, masks or
dispositions.

## The design

### 1. Faults on the faulting thread

**The choice (Q1, recommended A): an upcall into the faulting thread.**
A process registers one entry address. When one of its threads takes a
fault the kernel can't resolve, the kernel writes the thread's whole
register state and what happened (a **frame**) onto the thread's own
stack, or its exception stack, and returns to user mode at the entry
with the frame's address in `rdi`. libc there turns the frame into
Linux's `siginfo_t` and `ucontext_t`, runs the handler on that thread,
and gives the (possibly edited) registers back with one call that
resumes the thread there. One kernel entry and one exit per fault; the
handler runs where V8 needs it (its thread-local state, its stack); it
nests as Linux's frames do; and the same frame serves a signal to a
chosen thread (section 2). The other shapes are in Q1.

**The kernel's part** (SG1; the exception stack and the exit status are
SG2's):

- **The entry**: `exception_entry_set(entry)` sets the calling process's
  entry (0 removes it; an address at or past the user range's top is
  `ERR_INVALID_ARGS`). It acts on the caller's own process only, as
  `thread_set_fs_base` acts on the caller: no handle, no right. One per
  process (libc has one entry; a thread without libc's records is libc's
  case to refuse, below).
- **Which faults reach it**: where `kill_current` is called for a user
  fault today, the kernel first looks at the process's entry; with none,
  nothing changes. Every user exception reaches it except the ones that
  stay kills:

  | The fault | The frame says | libc makes it (SG3) |
  |---|---|---|
  | page fault, nothing mapped there (`ERR_NOT_FOUND`) | vector 14, the CPU's error code, the address (CR2), the status | SIGSEGV, `SEGV_MAPERR`, `si_addr` the address |
  | page fault, mapped without that access (`ERR_ACCESS_DENIED`) | the same | SIGSEGV, `SEGV_ACCERR` |
  | page fault past the end of a shrunk VMO (`ERR_OUT_OF_RANGE`: Linux's truncated file) | the same | SIGBUS, `BUS_ADRERR` |
  | page fault the file's pager failed (`ERR_IO`, `ERR_PEER_CLOSED`, `ERR_TIMED_OUT`) | the same | SIGBUS, `BUS_ADRERR` (Linux's I/O error on a mapped file) |
  | general protection (13), a non-canonical address included | vector, error code | SIGSEGV, `SI_KERNEL`, `si_addr` 0 |
  | stack segment (12), segment not present (11) | vector, error code | SIGBUS, `SI_KERNEL` |
  | invalid opcode (6: `ud2`) | vector | SIGILL, `ILL_ILLOPN`, `si_addr` the instruction |
  | divide error (0) | vector | SIGFPE, `FPE_INTDIV`, `si_addr` the instruction |
  | x87 and SIMD errors (16, 19) | vector (the status words are in the XSAVE area) | SIGFPE, `FPE_FLTDIV` ... from the unmasked exception bits |
  | alignment check (17) | vector | SIGBUS, `BUS_ADRALN` |
  | breakpoint (3: `int3`, RIP after it), debug trap (1: `TF`, `int1`) | vector | SIGTRAP, `SI_KERNEL` or `TRAP_TRACE` |
  | overflow (4), bound range (5), control protection (21, CET is off) | vector | SIGSEGV, `SI_KERNEL` |
  | **stays a kill**: a page that doesn't match the approved program (`ERR_BAD_STATE`), out of memory or the job's page limit (`ERR_NO_MEMORY`) (Q3) | | |

  Unchanged: NMI, double fault and machine check; a thread being killed
  while it waited on a pager (`ERR_CANCELED`: it leaves); a user copy
  inside a system call (its fixup returns an error, never an upcall);
  any fault in kernel mode.
- **The frame** (`struct exc_frame` in `<jam/abi.h>`): its size; its kind
  (`EXC_FAULT`, or `EXC_RAISED` for section 2); the vector, the CPU's
  error code, the address, `aspace_fault`'s status; a per-thread cookie
  (Q4); the sixteen general registers, RIP and RFLAGS; then the XSAVE
  area in its standard (uncompacted) form, 64-byte aligned, of the size
  the CPU gives for the enabled features (832 bytes while XCR0 is x87,
  SSE and AVX, `kernel/arch/x86_64/fpu.c`; FXSAVE's 512 on a CPU without
  XSAVE). Not the FS base: the handler runs with the thread's own
  thread-local storage, untouched. Built in a zeroed buffer, so no kernel
  byte reaches it through padding. For a fault at or above the user
  range's top the error code always says "present" (Linux's
  `sanitize_error_code`), so a frame never tells which kernel pages are
  mapped.
- **Where it goes**: below the interrupted RSP less the 128-byte red zone
  of the x86-64 ABI, 64-byte aligned; or, if the thread has an exception
  stack (SG2: `thread_exception_stack(base, size)`, libc's
  `sigaltstack`) and RSP is outside it, at that stack's top; or, if RSP
  is already inside it (a fault in a handler running there), below RSP
  on it. Written with `copy_to_user` at the bottom of the kernel stack
  with interrupts on (as `kill_current` runs), so a stack page is
  committed as by any touch; a write that fails (RSP in nothing mapped:
  a stack overflow with no exception stack, or one that overflowed that
  too) can't deliver, and the process is killed with today's line.
- **Entering**: RIP the entry, RSP just below the frame (aligned as after
  a call), RDI the frame; RFLAGS with DF, TF, RF and AC cleared (the ABI
  wants DF clear at a function's entry); the vector and x87 registers in
  their initial state with the default MXCSR (Linux's rule: a handler
  doesn't inherit a rounding mode or a full x87 stack). The other
  registers keep the thread's own values.
- **The window**: delivery sets a per-thread flag, cleared when libc says
  it has taken the frame (`LEAVE` below). A fault while it is set means
  libc's own entry failed (a bad entry address, a stack that can't take
  the frame's copy, a corrupt process): the process is killed with the
  first fault's line (the kernel kept its vector, RIP and address in the
  thread) and "again in its exception entry". This is the only nesting
  rule the kernel keeps; Linux's (a fault whose signal is blocked is
  fatal) is libc's, because only libc knows masks. A thread that leaves
  its handler by `siglongjmp` has already left the window, so nothing in
  the kernel counts handlers.
- **Done**: `exception_done(frame, how)`:
  - `RESUME`: the thread continues from the frame (the kernel reads it
    from user memory once): every general register, RIP and RFLAGS, and
    the XSAVE area. Checked: RIP below the user range's top (else the
    process is killed, as for `sysret` today); RFLAGS through
    `USER_RFLAGS_OK` with IF set, as the system call return does; CS and
    SS never from the frame; the XSAVE header's `XCOMP_BV` 0, its
    reserved bytes 0, `XSTATE_BV` within the user features of XCR0, and
    MXCSR within the CPU's mask (a bad one makes `XRSTOR` fault in the
    kernel): a frame that fails is a kill, never a kernel fault. No
    supervisor state (CET, PASID) is ever taken from a frame. Clears the
    window.
  - `LEAVE`: clears the window and returns normally: the handler now
    runs as ordinary code of the thread.
  - `UNHANDLED`: libc won't handle it (the signal is blocked, ignored or
    default): the process is killed with the line it would have had with
    no entry, from what the kernel kept, not from the frame.
- **A fault with no entry**: exactly today's kill, line and code (SG2
  adds the fault to the process's end, below).
- **The exit status** (SG2): `process_info` gains the fault that killed
  the process (its vector and status, 0 for any other end), so libc's
  `waitpid` reports `WTERMSIG` SIGSEGV, SIGBUS, SIGILL, SIGFPE or SIGTRAP
  as Linux would, and Chromium tells a crashed renderer from a killed
  one. `process_kill` and an exit say none.
- **The cost**: zero on every fault that is served (the resolved path in
  `trap_page_fault` returns before any new code) and on every system
  call. The new code runs only where the process would otherwise be
  killed, plus one more per-thread flag tested where
  `return_to_user_work` already tests `cancel_pending`, on the return
  from an interrupt or exception only (section 2). No kernel memory per
  upcall: the frame is on the user's stack, the kernel keeps a few words
  per thread.
- **Hazards for SG1**: `fpu.c`'s XSAVEOPT tracking and owner shortcut
  must stay true after a resume loads a state the area didn't hold; the
  frame is written and checked with interrupts on and `return_to_user_work`
  run again afterwards (a kill or a reschedule may have come meanwhile);
  `user_faults` counts kills only; ring-3 test threads without a process
  keep today's path.

**libc's part** (SG3; `sigaltstack` and the exit status SG4):

- **Registering**: the entry is set while any of SIGSEGV, SIGBUS,
  SIGILL, SIGFPE or SIGTRAP has a handler, or once the process has
  raised one of its own threads (SG5: a raise to a process with no entry
  is dropped); with it set and no handler for a fault, the entry says
  `UNHANDLED` and the kill's line is the same. A program that never asks
  keeps the kernel's own kill, line for line, with libc nowhere on the
  path.
- **The entry** (a new user/posix/sys/sigentry.S calling C in a new
  user/posix/sys/sigfault.c): finds the thread's record through its FS
  base, as `signal.c` does; a thread without one (the signal thread,
  libjam's own threads: no thread-local storage) is `UNHANDLED`. Takes
  no lock the faulting thread may hold: a fault inside the layer while
  it holds `sig_lock` is `UNHANDLED` (sig.h marks the holder), and the
  dispositions are read without the lock (a sequence count beside
  `sigaction`'s writes).
- **The signal**: the table above from the vector and status. `siginfo_t`:
  `si_signo`, `si_code` (always > 0 for a fault: V8 tells a kernel's
  fault from `kill` by it), `si_addr`, `si_errno` 0.
- **The ucontext** (musl's x86-64 `ucontext_t`, Linux's layout):
  `uc_mcontext.gregs[REG_R8 .. REG_CR2]` from the frame (`REG_TRAPNO` the
  vector, `REG_ERR` the error code, `REG_CR2` the address, `REG_CSGSFS`
  the user selectors, `REG_OLDMASK` the mask's first word);
  `uc_mcontext.fpregs` pointing at the frame's XSAVE area, whose first
  512 bytes are `struct _fpstate`, with Linux's `fpx_sw_bytes` (the two
  magic words, the area's size and features) in its reserved tail so
  readers that look for the extended state find it; `uc_sigmask` the
  mask before the handler; `uc_stack` the alternate stack; `uc_link` 0.
- **Dispositions and flags**: a blocked, ignored or default fault signal
  is `UNHANDLED` (Linux forces the default for a synchronous fault).
  Otherwise: the thread's mask becomes the old one, `sa_mask` and the
  signal itself unless `SA_NODEFER`; `SA_RESETHAND` resets the action
  first; `SA_SIGINFO` picks the three-argument call. `SA_ONSTACK` (SG4):
  the kernel always uses the exception stack when there is one; a
  handler without `SA_ONSTACK` gets its siginfo and ucontext moved to the
  interrupted stack below its red zone and runs there, as on Linux. A
  fault during that move is still inside the window: the kernel's kill.
  Then `LEAVE`, then the handler.
- **Returning**: the mask from `uc_sigmask`, the registers from `gregs`
  and the XSAVE area as the handler left them, into the frame, then
  `RESUME`. Returning without edits re-runs the faulting instruction:
  V8's and Chromium's way of passing a fault on is to reset the action
  and return, so the fault comes again to the next handler or to the
  default (`UNHANDLED`, or no entry left: the kernel's kill). A handler
  that `siglongjmp`s out is fine (the window is closed, `siglongjmp`
  restores the mask).
- **Nested faults**: a fault inside a handler is a new upcall; its signal
  is blocked unless `SA_NODEFER`, so `UNHANDLED`, as Linux kills. With
  `SA_NODEFER` the handler runs again, nested below the first frame
  (V8 unblocks SIGSEGV inside its handler on purpose, so a fault there
  dies cleanly).
- **Sending a fault signal to itself**: `rt_tgsigqueueinfo` to our own
  thread with any `si_code` (Linux allows it within a thread group:
  Chromium's stack dump re-raises this way), delivered at that call's
  boundary; `raise` as today.
- **The exit status** (SG4): `wait_status` gives `WTERMSIG` of the fault
  from SG2's fields.

### 2. Signals to a chosen thread

**What needs it**: V8's CPU profiler (`pthread_kill(SIGPROF)` to the
thread running JS), Chromium's sampling profiler (`tgkill` with SIGURG),
and any handler a thread in a busy loop should run now (a SIGINT handler
on a loop that makes no calls; test watchdogs, *unsure* which of
Chromium's need it). Both profilers need two things M13 lacks: the
handler must run while the thread is in its own code, and the ucontext
must hold the registers where it was stopped (a sample taken at a libc
call boundary would point into libc every time).

**The choice (Q2, recommended A): a raise.** `thread_raise(thread)`
(needs `RIGHT_SIGNAL` on the thread, as `thread_interrupt`) latches a
flag on the thread; at the thread's next return to user mode from an
interrupt or exception, the kernel delivers an upcall of kind
`EXC_RAISED` with the interrupted registers, as for a fault (same frame,
stack choice, window and `exception_done`). If the thread is running in
user mode on another CPU the kernel kicks it (`sched_kick`'s reschedule
IPI), so the upcall comes within microseconds. The rules:

- Latched, not counted: a raise while one is latched does nothing more
  (and sends no second IPI), so a storm of raises costs one upcall.
- Never delivered inside the window (it waits for `LEAVE` or `RESUME`)
  nor to a process with no entry (dropped).
- Delivered only on the interrupt and exception return path; the system
  call return never looks (its cost stays zero). A raise that comes while
  the thread is inside a call waits for its next interrupt in user mode
  and then finds nothing to do (the call's boundary already delivered the
  signal), which libc's entry returns from at once. Harmless, and rare.
- The other shape, B, is Zircon's: suspend the thread, read its
  registers, write a frame onto its stack from the sender, set its RIP,
  resume it. Four calls and two waits per signal, a suspend state the
  scheduler must keep through system calls, and register access to
  another thread is a power the kernel would have to confine. A does it
  with one call and the path faults already use.

**libc's part** (SG5): routing stays B4's. When the signal is routed to a
thread that is in its own code (`calls` == 0, the Dekker pair B4 already
keeps with the pending bit), the router (the signal thread, or the sender
for `pthread_kill`, `tkill` and `tgkill` to our own threads) calls
`thread_raise` after marking the signal pending; a thread inside a call
still gets `thread_interrupt`, and its boundary delivers. libc's entry,
for `EXC_RAISED`: if the thread is inside the layer (`calls` > 0) or
every pending signal is blocked, it keeps the interrupted registers in
the thread's record and resumes at once (the boundary delivers, and its
ucontext gets those registers rather than zeros); otherwise it runs each
due handler as section 1 does, the ucontext holding the interrupted
state. Default actions stay in the sender. Faults never pass through the
signal thread. Nothing changes for a program that never sends a signal
to a thread.

**What M13's model gives up after this**: nothing on the list in
[M13-PLAN's section 6](M13-PLAN.md#6-signals) ("a thread spinning in user
code ... runs its handler only at its next call", "faults ... are not
delivered as signals", "no `sigaltstack`") except job control, which is
not this plan's.

### 3. V8's trap handler, W^X and the JIT

What V8's handler does on Linux x64 (`src/trap-handler/`, read
2026-10-16 from V8's main branch; Chromium 155's copy is checked by SG6):

- **Outside** (`handler-outside-posix.cc`): `RegisterDefaultTrapHandler`
  installs `HandleSignal` for SIGSEGV with `SA_SIGINFO | SA_ONSTACK`,
  keeping the old action; `RemoveTrapHandler` puts the old one back.
  Each Wasm code object registers its range and the offsets of its
  **protected instructions** (the loads and stores allowed to fault) in a
  table the handler reads under a lock it can take from a signal
  handler.
- **Inside** (`handler-inside-posix.cc`, `TryHandleSignal`): refuses a
  nested call (a thread-local guard); refuses any signal but SIGSEGV;
  refuses one not made by the kernel (`si_code` > 0 and not `SI_USER`,
  `SI_QUEUE`, `SI_TIMER`, `SI_ASYNCIO`, `SI_MESGQ`); refuses an `si_addr`
  outside the memory it covers (`IsAccessedMemoryCovered`); unblocks
  SIGSEGV for its own run; reads `gregs[REG_RIP]`; refuses a RIP not in
  the table; then writes the landing pad's address into `REG_RIP` and the
  faulting RIP into `REG_R10`, and returns true.
- **When it refuses**, `HandleSignal` removes V8's handler and returns:
  the instruction faults again and reaches the old handler or the
  default; a user-sent SIGSEGV is raised again.

So it needs, exactly: SIGSEGV on the faulting thread (its thread-local
guard and the table's lock are per thread); `si_code` > 0 and the real
`si_addr` (CR2) in the siginfo; RIP and R10 read from `gregs` and taken
on return with every other register unchanged; `pthread_sigmask` and
`sigaction` working inside the handler; the instruction run again when
the handler returns unchanged; and `SA_ONSTACK` honoured when an
alternate stack is set. Section 1 gives each.

**W^X**: nothing new. The landing pad is a builtin, in the program's
approved text (or in V8's code range under `right jit`, if V8 remaps its
builtins there: CJ2's business). Wasm code itself is JIT memory, so the
trap handler matters only with CR-e. `RESUME` changes registers, never a
page's permissions: a RIP into memory that isn't executable faults on
the fetch like any jump. The Wasm memories' guard regions (8 GiB and
more a memory) are reservations in V8's cage, which CL1's stage 2 and
CK1 already make cost nothing until touched.

**Chromium's wiring**: Chromium enables V8's handler in the renderer
(`v8::V8::EnableWebAssemblyTrapHandler`), and where its own fault
handlers are installed (the stack dump, Crashpad) it passes V8's
`TryHandleWebAssemblyTrapPosix` to them as a first-chance hook instead
of letting V8 install its own (*unsure* of the 155 details: SG6 reads
them). Either way it is ordinary `sigaction` code; nothing in Chromium
or V8 is patched for this plan. CR-PLAN's *unsure* (whether the renderer
honours `--no-wasm-trap-handler`) stops mattering: the flag goes.

### 4. Security

- **A handler gains no authority.** The upcall runs the process's own
  code, on the faulting thread, with that thread's handles and rights,
  in its own address space. Anything it can do (set every register, jump
  anywhere executable) the thread could already do. `exception_entry_set`
  needs nothing because it gives nothing.
- **What the kernel trusts from a frame**: nothing it doesn't check
  (section 1, `RESUME`): RIP, RFLAGS, the XSAVE header and MXCSR. CS, SS,
  the FS base, CR3, the kernel stack and any supervisor state never come
  from it. A bad frame kills its own process, never the kernel.
- **`RESUME` as a gadget**: like Linux's `rt_sigreturn`, it sets every
  register from memory in one call, which sigreturn-oriented programming
  uses after a stack overflow bug. Not a new power, but a convenient one
  for an attacker in a renderer; Q4 recommends a per-thread random
  cookie in each frame that `RESUME` checks, so a forged frame needs a
  read of the stack too.
- **What leaves the kernel**: only the faulting thread's own registers,
  into its own stack, through `copy_to_user` (SMAP and SMEP as for any
  copy). No kernel address: the frame is built zeroed, and the error code
  of a fault on a kernel address always says "present".
- **Another process's faults never reach us.** A fault is the faulting
  thread's, in its own address space: a VMO shared between processes
  gives each mapper its own faults. A pager answers page requests and
  never sees a client's registers or signals. A process whose mapping of
  another's VMO shrinks gets SIGBUS on its own thread, if it asked for
  handlers (it is killed today). A frame is never written into another
  address space.
- **Raises**: only with the thread's handle and `RIGHT_SIGNAL`; a process
  normally holds only its own threads'. A raise from another holder only
  makes the thread run its own process's entry, which finds nothing
  pending and resumes: no state crosses, nothing runs that the target
  didn't install. Latched, so a flood is one upcall and one IPI.
- **What stays a kill** (Q3): a page that doesn't match the approved
  program (the verified pages' tamper check: a handler must not be able
  to look at it or step over it) and running out of memory (the handler
  would need memory itself). Kernel faults are unaffected: a fault in
  kernel mode never consults the entry, a user copy's fault is still an
  error code, and NMI, double fault and machine check keep their paths.
- **The JIT power** is unchanged (section 3): `RESUME` never makes memory
  executable.

### 5. Tracks

Each track is about one agent-hour, starts from main, owns the files
listed (an edit outside them stays minimal and is named in its report;
new files are written without a path check), adds a test for each
behaviour (one that fails without it), and updates the docs its change
touches (ARCHITECTURE's Processes paragraph and the call's comments in
`abi/syscalls.def` for the kernel tracks; sig.h's model comment and
M13-PLAN's section 6 pointer for libc; TESTING.md's rows; BENCH.md's
lines). QEMU tests run on the PC through `tools/pc-batch.sh`; the
quick tier plus the track's own cases; kernel tracks the full ktest at 4
and 8 CPUs.

| Track | What | Files | Gates | Tests | Model |
|---|---|---|---|---|---|
| **SG1. Fault upcalls (kernel)** | Section 1's kernel part: `exception_entry_set`, `struct exc_frame`, delivery for every fault in the table in place of `kill_current`, the frame on the current stack, the window, `exception_done` (`RESUME` checked, `LEAVE`, `UNHANDLED`), the cookie (Q4), the kill unchanged with no entry. Commits the ABI first ("SG1 ABI:") so SG3 can start | `kernel/arch/x86_64/uentry.c`, `kernel/arch/x86_64/trap.c`, `kernel/arch/x86_64/fpu.c` (the standard-form save and the checked restore), a new kernel/arch/x86_64/upcall.c, `kernel/object/process.c` (the entry), `kernel/include/jam/sched.h` (the thread's words), `kernel/abi/sysc_proc.c`, their lines of `abi/syscalls.def` and `<jam/abi.h>`, a new kernel/test/test_upcall.c (ring-3 code in `kernel/test/test_uentry.c`'s way) | none (no other kernel track in `uentry.c` or `trap.c` at the same time) | ktests: each vector in the table reaches the entry with its vector, error code, address, status and every register (set to a pattern before the fault); `RESUME` with RIP moved past the instruction continues there with every other register, xmm and ymm included, as the frame says; refused frames (RIP at the user top, a bad XSAVE header, a bad MXCSR, a wrong cookie) kill the process and never the kernel, and IOPL, NT and RFLAGS' other privileged bits are stripped; no other thread's vector state in any frame; a fault in the window kills with the first fault's line; a frame that can't be written kills with today's line; with no entry the line and code are today's, byte for byte; `ERR_BAD_STATE` and `ERR_NO_MEMORY` kill even with an entry; a user copy with a bad pointer still returns its error; 100,000 fault-and-resume rounds on 8 threads leak no page or handle. Full ktest at 4 and 8, the init run; BENCH: a fault caught and resumed | **Opus** |
| **SG2. Stacks, raises, a fault's end (kernel)** | `thread_exception_stack` and section 1's stack choice; section 2's `thread_raise` (latched, kicked, delivered on the interrupt and exception return only, never in the window, dropped with no entry); `process_info`'s fault fields | SG1's kernel files, `kernel/sched/wait.c` or `kernel/sched/thread.c` (the latch beside `interrupt_pending`), `kernel/sched/sched.c` (the kick, if it must be shared), their lines of `abi/syscalls.def` and `<jam/abi.h>`, kernel/test/test_upcall.c | SG1 merged | ktests: the exception stack used when RSP is outside it and nested below RSP inside it; a stack overflow into nothing mapped reaches the entry on the exception stack and kills without one; a raise to a thread spinning on another CPU reaches its entry within 10 ms with RIP inside the loop; a raise to a thread blocked in `channel_call` arrives only at a later interrupt, a raise in the window only after `LEAVE`; `RIGHT_SIGNAL` checked; 10,000 raises from 4 threads to 4 spinning threads: no panic, at least one upcall per thread per burst, one IPI per latch; `process_info` names the fault after a fault's kill and none after `process_kill` or an exit; the reply-and-wait and switch bench lines no slower. Full ktest at 4 and 8, the init run; BENCH: a raise to its upcall | **Opus** |
| **SG3. Fault signals (libc)** | Section 1's libc part: the entry, the signal table, `siginfo_t`, the ucontext (`gregs`, `fpregs`, `fpx_sw_bytes`), dispositions and flags (`SA_SIGINFO`, `SA_NODEFER`, `SA_RESETHAND`, `sa_mask`), the window and `LEAVE`, `RESUME` from the edited ucontext, `UNHANDLED`, no lock the thread may hold, `rt_tgsigqueueinfo` to itself, the entry set only while a fault signal has a handler | new user/posix/sys/sigentry.S and user/posix/sys/sigfault.c, `user/posix/sys/signal.c` and `user/posix/sys/sig.h` (the disposition read, the holder mark, the hooks), `user/posix/sys/sigcalls.c`, `user/posix/posix.mk`, posixtest's new fault section (a new user/posix/test/fault.c, `user/posix/test/main.c`) | SG1's ABI commit (merged after SG1) | posixtest `fault`: SIGSEGV on a `PROT_NONE` guard page with `si_addr` the address and `SEGV_ACCERR` (`SEGV_MAPERR` on an unmapped one; *unsure* which CL1's reservations give: the section checks what Linux gives and says if they differ); a handler that skips the faulting instruction (`REG_RIP` += its length) and returns; one that writes RIP and R10 (V8's shape) lands with R10 the old RIP; `siglongjmp` out of a SIGSEGV handler 1,000 times; SIGFPE on integer division by zero (`FPE_INTDIV`); SIGILL on `ud2`; SIGTRAP on `int3`; SIGBUS on a mapping past a shrunk memfd (`BUS_ADRERR`); SIGSEGV blocked, ignored or default ends a child as SIGSEGV; `SA_RESETHAND` lets the second fault end it; `SA_NODEFER` runs a nested handler; a handler that resets to `SIG_DFL` and returns ends the child (Chromium's and V8's pattern); `raise(SIGSEGV)` gives `si_code` <= 0; `rt_tgsigqueueinfo` to itself with `SEGV_MAPERR`; the handler sees the thread's xmm value through `fpregs`; an `uc_sigmask` edited on return changes the mask; the kernel's kill line for a program with no handler is unchanged. libc-test's signal cases unchanged; the init run | **Opus** |
| **SG4. `sigaltstack` and a fault's end (libc)** | `sigaltstack` for real (`SS_ONSTACK` while on it, `SS_DISABLE`, `EPERM` while on it, `MINSIGSTKSZ`), `SA_ONSTACK` (the move to the interrupted stack for a handler without it), `wait_status`'s `WTERMSIG` from SG2's fields | user/posix/sys/sigfault.c (the move), `user/posix/sys/sigcalls.c`, `user/posix/sys/proc.c` (`wait_status`), posixtest's fault section | SG2, SG3 | posixtest: infinite recursion on a thread with an alternate stack reaches its `SA_ONSTACK` handler there (`siglongjmp` back, the thread goes on); without `SA_ONSTACK` the same overflow ends the child as SIGSEGV; a handler without `SA_ONSTACK` runs on the interrupted stack when an alternate one is set; `sigaltstack` reports `SS_ONSTACK` inside; a child killed by a fault with no handler is `WIFSIGNALED` with `WTERMSIG` SIGSEGV (SIGILL for `ud2`), a `kill -9` still SIGKILL. The init run | Sonnet 5.5 |
| **SG5. Signals to a chosen thread (libc)** | Section 2's libc part: `thread_raise` for a target in its own code, the entry's `EXC_RAISED` path (run due handlers with the interrupted registers, or keep them and defer inside a call), the boundary's ucontext from kept registers, `pthread_kill`, `tkill` and `tgkill` to a busy thread | `user/posix/sys/signal.c` (routing: raise or interrupt), user/posix/sys/sigfault.c (the raised path), `user/posix/sys/sig.h`, posixtest's `user/posix/test/signal.c` | SG2, SG3 (beside SG4: merged after it) | posixtest: `pthread_kill` to a thread in a loop with no calls runs its handler within 10 ms, `REG_RIP` inside the loop's function and `REG_RBP` its frame (V8's profiler); 10,000 SIGPROF from a sampler thread, each sent after the last was handled, all handled; `tgkill(SIGURG)` with the handler copying the stack and waking the sender through a futex (Chromium's `StackCopierSignal`); a SIGINT handler runs on a busy loop; B4's cases unchanged (10,000 signals between two processes none lost, `EINTR` and `SA_RESTART`, `pthread_cancel` in `read`). libc-test's signal cases; the init run; shell scripts basic and cmds | **Opus** |
| **SGR. Review** | Section 4 against SG1 to SG5: findings first (a new docs/SG-REVIEW.md), then High and Medium fixed with a test each | the files its fixes need | SG1 to SG5 merged | its own, and hostile: 10,000 random frames given to `RESUME` (the process dies or runs on, the kernel never panics, no other thread's state ever shows); faults on every vector from 8 threads at once with raises crossing them; a raise flood at a thread in the window. Full ktest at 4 and 8, the init run | **Opus** |
| **SG6. Chromium with them** | V8's trap handler on (`--no-wasm-trap-handler` and the profiler's switch-off gone from the recipe, if CR-e added them), Chromium 155's wiring read (section 3), base_unittests' profiler and stack-dump tests run, a renderer crash printing its stack and reported as crashed | ports/chromium (args, the test list), `tools/cr-test.sh`'s cases, CR-PLAN's item 6 and BT note | SG3, SG4, SG5; CB2's tree (base); CR-e's V8 half (CJ2) for Wasm | cr-test.sh `base` with `StackSamplingProfiler*`, `StackCopierSignal*` and the stack dump's tests unfiltered; V8's `TrapHandler*` unit tests if CB's tree builds `v8_unittests` cheaply (*unsure*); `content_shell` with `chrome://crash` (the trace in the log, the browser sees "crashed"); with CR-e, a Wasm page under the trap handler and the DevTools profiler recording JS frames | Sonnet 5.5 |

**Order**: SG1 at once (no gate but the kernel slot: no other track in
`uentry.c` or `trap.c` meanwhile). SG3 when SG1's ABI commit is there,
merged after SG1. SG2 after SG1 merges. SG4 and SG5 after SG2 and SG3,
side by side (SG4 merged first, SG5 rebasing: both touch sigfault.c).
SGR after SG1 to SG5. SG6 after them and CB2, its Wasm half with CR-e.
After each kernel merge the main session runs `make syscalls` and commits
what it writes.

**Beside CR's tracks**: independent of CB3 and CB4 (neither needs it,
neither blocks it). Needed by CR-e (the trap handler: SG3, and SG4 for
`SA_ONSTACK`), by Chromium's crash stack dumps and crashed-or-killed
status (SG3, SG4), and by the profilers (SG5). Done before CR-e,
`--no-wasm-trap-handler` never ships.

**What changes in CR-PLAN's item 6**: the workarounds become temporary.
Until SG3 and SG5 merge, jitless and no profiler stay as they are (they
are there for the JIT's own reasons too), and Chromium's fault stack
dumps don't run. If CR-e comes first, `--no-wasm-trap-handler` is used
until SG3 and SG4 merge and then removed by SG6. The *unsure* about the
renderer honouring that flag goes with it.

### 6. Questions for the owner

**Q1. How a fault reaches a handler.**
- **(A)** An upcall into the faulting thread (section 1): the kernel
  writes the frame on the thread's stack and enters libc's entry; one
  call resumes. Small kernel code on the kill path and the interrupt
  return; a frame layout M14 then freezes.
- **(B)** An exception channel served by another thread (Zircon's): the
  faulting thread stops in the kernel, libc's signal thread reads its
  registers, builds the frame on its stack, writes its registers and
  resumes it. No kernel writes to user stacks, and the start of a
  debugger. But two switches and four calls a fault, a fault on the
  signal thread or while it is stuck hangs or kills, and register access
  to another thread is a new power to confine.
- **(C)** Keep killing; keep the workarounds (against the owner's rule).

*Recommendation: (A).* It is what Linux does, so Chromium's and V8's
expectations hold without a second thread in the path, and it gives
section 2 for nothing. A debugger's power (B's half) can come later on
its own (Q5).

**Q2. How a signal reaches a chosen thread.**
- **(A)** `thread_raise`: a latched flag the kernel acts on at the
  thread's next return from an interrupt or exception, with an IPI to a
  running thread (section 2).
- **(B)** Suspend the thread, read and write its registers from the
  sender, resume it (Zircon's).
- **(C)** Keep the call boundary only: no profilers.

*Recommendation: (A)*, on the same frame and entry as faults; one call,
no new thread state but a flag.

**Q3. Which faults stay kills, even with a handler.**
- **(A)** Two: a page that doesn't match the approved program (the
  tamper check) and running out of memory. Every other fault Linux makes
  a signal becomes one.
- **(B)** Only out of memory (Linux's line exactly; a tampered page would
  be a SIGBUS a handler could step over).
- **(C)** Only what Chromium needs (page faults, invalid opcode, divide
  error); the rest stay kills.

*Recommendation: (A).* The tamper check is Jam OS's rule, not the
program's business; out of memory leaves a handler nothing to run on.

**Q4. A cookie in each frame.**
- **(A)** A per-thread random value written into each frame and checked
  by `RESUME` (a mismatch kills): a forged frame needs a read of the
  stack as well as a write. One word and one compare.
- **(B)** None, as Linux (`rt_sigreturn` takes any frame).

*Recommendation: (A).* Cheap hardening for the renderer, which is the
attacker's first foothold until CR-f's sandbox.

**Q5. Crash dumps.**
- **(A)** In-process only now: Chromium's own stack dump to the log
  (section 1), Rust's overflow message. Crashpad's minidumps (another
  process reading the crashed one's memory and threads) wait for a plan
  of their own for a debugger's power.
- **(B)** Plan that power now (a right to read another process's memory
  and stop and read its threads), so Crashpad works with CR-d.
- **(C)** Never: logs are enough.

*Recommendation: (A).* The stack in the log is what a developer reads
on Jam OS; a power over another process's memory deserves its own
security plan, and no Chromium milestone needs it.

**Q6. Order against CR-e.**
- **(A)** SG1 to SG5 now, beside CB3 and CB4, so CR-e starts with the
  trap handler on.
- **(B)** CR-e first with `--no-wasm-trap-handler`, this plan after CR-d.

*Recommendation: (A).* About five agent-hours, none on Chromium's critical
path, and the workaround never ships; if the kernel slot is busy when
CR-e is ready, CR-e may go first with the flag for a while.

**Decided by the main session (2026-10-16), under the standing rule that
technical choices follow the recommendation:** Q1 (A) an upcall into the
faulting thread; Q2 (A) `thread_raise`; Q3 (A) the tamper check and out of
memory stay kills; Q4 (A) the per-thread cookie; Q6 (A) SG1 to SG5 now,
beside CB3 and CB4. Q5 (A) for now: crash dumps in-process; Crashpad's
out-of-process minidumps need a debugger power, a capability question
planned with the owner later.

## Size

The kernel: about 400 to 600 lines (the frame, delivery, the checked
resume, the stack choice, raises) and 500 of ktests. libc: about 600 to
900 (the entry, the conversion both ways, the dispositions' path,
`sigaltstack`, the raised path) and 500 of posixtest. Seven tracks of
about an agent-hour each, SGR and SG6 included.
