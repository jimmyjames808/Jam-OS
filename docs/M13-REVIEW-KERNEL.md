# M13 code check: the kernel for POSIX (K1, K2)

M13's track R3 ([M13-PLAN.md](M13-PLAN.md), "Reviews and the join"): every
line K1 and K2 changed, read in full context at main e30d7fec. K1
(merge d60772d6: ed7d1c68, 0ce6d3fc, 2825c1da): small pids and tids
(`kernel/lib/idspace.c`), `thread_set_tid_word` (84), `thread_interrupt`
(88) and `ERR_INTERRUPTED` (-26) on the four system call waits,
`thread_get_info` (89), `cpu_ns`, `sys_public_info` (136); K2 (merge
4642b85a: 13c6c29d, 9faf64fd, a076f3e4, 059c1bb1, 264fc44f): the time page
(`clock_page`, 137) and `VMAR_REPLACE` (bit 5 of `vmar_map`'s flags), with
libc's `time.c` and `mem.c` sides. Read against the plan's K1 and K2 rows
and its "K1 notes" and "K2 notes", and against what M12.5's PK1-PK4
(merged since) changed in the same paths: the fault path's pager wait
(`aspace_fault`, `fault_wait`), the copies' statuses, `vmo_read`'s wait.

Looked at first, as the plan asks: the interrupt's races (a thread
interrupted entering, inside and leaving each kind of wait; interrupt
against wake, timeout, kill and exit), the tid word's timing, the time
page's consistency for readers on other CPUs, `VMAR_REPLACE`'s atomicity
against faults, pins and other mappers, and each new call's handle,
rights and bounds checks.

Severity, as in the M12.1 checks: **High** is a crash, a hole, or a hang
or breakage that a program can cause for others with hostile arguments;
**Medium** is wrong behaviour the system or a test tier will meet;
**Low** is a wart, a guide violation, a mismatch with the documented
contract that nothing relies on yet, or a hole only a holder of a power
can use on itself.

Counts: **0 High, 1 Medium, 7 Low.** The Medium and two Lows are fixed,
one commit each with a test that failed before it; the other five Lows
are reported (design notes, or cosmetic).

Before any change, on e30d7fec: the full ktest at 4 CPUs on the PC, 505
passed.

## Findings

| # | Sev | Where | What is wrong | Failure scenario | Fix | Outcome |
|---|---|---|---|---|---|---|
| 1 | Medium | `kernel/sched/sched.c:461` (thread_cpu_tsc), used by `process.c` (process_cpu_tsc, uthread_get_info, uthread_exit_current) | A thread's CPU time is `run_tsc` plus, while it runs, `now - switch_tsc` of its CPU, read with no lock and no recheck: `run_tsc` first, then `t->cpu`, `on_cpu`, `current` and `switch_tsc`. A switch of t between the loads mixes two moments: t switched out (and maybe in again) after `run_tsc` was read gives the old `run_tsc` plus nothing (it no longer looks current on the CPU read) or plus the new run only, losing the run just ended; `run_tsc` read after the switch-out's add but `switch_tsc` before its store counts that run twice. | `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` (and the process clock, `thread_get_info.cpu_ns`, `process_info.cpu_ns`, `proc_list`) goes backwards by up to the run that was in progress (a 20 ms slice, more for a thread that ran long without a switch): a thread preempted between the two loads of its own read, or another thread's read racing its switch. A program timing work with its CPU clock sees a negative interval (a huge unsigned one). Rare per call, certain over millions of calls under load. | Read under the run queue lock of the thread's CPU, rechecking the CPU once locked (thread_wake's "task_rq_lock" loop): `schedule()` holds that lock across `account_switch` and the `current` change, and a thread's CPU only changes under the lock of the CPU it moves to while it isn't running, so no switch of t falls between the loads. | Fixed 1cfd9f6b (ktest `sysinfo_cpu_time_never_backwards`) |
| 2 | Low | `kernel/sched/wait.c:187` (block_prepared's return), so object_wait_one, port_wait, futex_wait, sleep_until | When the deadline passes while the thread is blocked and an interrupt comes before it runs again, the wait returns `ERR_INTERRUPTED` and takes the interrupt. K1's notes (and `abi/syscalls.def`: "A wait whose condition is met returns its result and leaves the interrupt for the next") put a passed deadline among the conditions: `ERR_TIMED_OUT` (OK for `sleep_until`) before `ERR_INTERRUPTED`. The loops check the deadline only before they block. | A `poll` or `nanosleep` whose time is up at the moment a signal arrives reports `EINTR` instead of its timeout; POSIX allows either, and B4's rule (look at the pending set after every call) loses no signal, so nothing breaks today, but the documented order doesn't hold. | In block_prepared: an interrupt is not reported once the wait's deadline has passed (the caller's loop then sees the deadline and returns its timeout, the interrupt left latched). | Fixed 174bfd94 (ktest `intr_deadline_comes_first`) |
| 3 | Low | `kernel/abi/sysc_vm.c:144` (sysc_vmar_map) | For `VMAR_REPLACE` the answer is written to `*addr` before the call is checked, also when `VMAR_FIXED` is missing (then `a` is 0, never read from `*addr`) and the call is refused: a refused call writes 0 over the caller's word. | `jam_vmar_map(..., VMAR_REPLACE, &addr)` without `VMAR_FIXED` returns `ERR_INVALID_ARGS` with `addr` zeroed, against the rule that an error changes nothing. The caller's own word only. | Write the answer early only with `VMAR_FIXED` (without it the call is refused before anything changes, so nothing needs undoing). | Fixed 3e11212b (utest `vmar_replace_calls`) |
| 4 | Low | `kernel/object/process.c:653` (clear_tid_word before thread_left) | The tid word is cleared and its waiters woken before the thread gives its job back its thread and its `UTHREAD_KMEM_PAGES` (thread_left, after the address space is left). | A program at its job's thread limit that joins a thread and at once starts another (a pool keeping N workers with N the limit) can get `ERR_NO_RESOURCES` (`EAGAIN` from `pthread_create`) for a moment. Only the program itself. | Design note: credit the job before the clear (the thread's last moments would then run uncharged), or document it next to the tid word in `abi/syscalls.def`. Not changed here. | Reported |
| 5 | Low | `kernel/sched/futex.c:130` (futex_wait) | A failed read of the word becomes `ERR_INVALID_ARGS` whatever the copy said: since PK2 a copy can also fail with `ERR_IO` (the word's pager failed it, a verified page didn't match) or `ERR_CANCELED` (the thread was killed while it waited for the pager). | A futex word on a file mapping whose pager fails reports `EINVAL` rather than `EIO`/`EFAULT`; a killed thread's call reports `ERR_INVALID_ARGS` on its way out (nobody sees it). | Pass `ERR_IO` and `ERR_CANCELED` through. | Reported (cosmetic; R1 may want EFAULT for the layer) |
| 6 | Low | `kernel/include/jam/process.h:50` (THREAD_RIGHTS), `kernel/sched/wait.c:263` (thread_interrupt) | Every thread handle now carries `RIGHT_SIGNAL`, so whoever holds one (the spawner of a native program, who keeps the child's thread handle until the start) can end that program's `object_wait_one`/`port_wait`/`futex_wait`/`sleep_until` with `ERR_INTERRUPTED`, which native libos code doesn't expect (B4 found libjam's `ns.c` take_locked giving up on it). Each interrupt also wakes a non-interruptible waiter (`channel_call`, a fault's pager wait) for nothing. | A holder of a thread handle can make a native program's waits fail or spin it with wake-ups. The holder could kill it anyway (RIGHT_WRITE on the process); no authority is gained. | Note for R1/B4: native programs' handles could be made without `RIGHT_SIGNAL` (libos spawn closes its copy today). | Reported |
| 7 | Low (pre-existing, now visible) | `kernel/arch/x86_64/smp.c:333`, `<jam/abi.h>` time_page_uptime | The kernel's clock assumes one TSC across CPUs; an AP whose `IA32_TSC_ADJUST` differs is only reported. With the time page user code now computes the uptime from whichever CPU it runs on, without a call. | On such a machine a reader that moves between CPUs can see the uptime step back by the TSCs' skew (or wrap, if an AP's TSC is behind `tsc_boot`), exactly as the kernel's own `uptime_ns` would there. Not seen on the PC (the boot log says when it happens). | Note: if the boot ever sees a skew, clear `TIME_PAGE_TSC` (libc then reads with the calls) or write the APs' TSC_ADJUST. | Reported |
| 8 | Low | `kernel/lib/idspace.c:48-75` (find_free, wrap) | A search and a wrap scan the bitmaps (up to 65,536 words each) under the space's spinlock with interrupts off. | Bounded (about 0.5 MiB read, tens of microseconds) and only when ids are dense or at a wrap; a latency wart, not a hang. | None needed now. | Reported |

Not findings (checked and correct):

- **The interrupt (K1).** `thread_interrupt` stores the flag (seq_cst),
  fences, then reads the state in `thread_wake`; block_prepared stores
  `T_BLOCKED`, fences, then reads the flag: one side always sees the
  other, so an interrupt is never slept through, and the skip path only
  moves `BLOCKED -> RUNNING` by CAS, as thread_cancel's. Each interruptible
  wait checks its own condition under its lock after the block and
  returns it in preference to the interrupt: the object wait's `hit` (set
  by the observer under the object lock), the port's queue (a wake_one
  that picked an interrupted waiter is never wasted: it takes the packet),
  the futex's `woken` (waiter_leave under the bucket lock, after a requeue
  too). `thread_interrupt_consume` runs only on a returned
  `ERR_INTERRUPTED`; a second interrupt merged into the first is the
  documented "latched, not counted", which B4's pending set makes safe.
  Every other wait a user thread can be in (`channel_call`'s and
  `channel_read`'s waits, `pager_req_wait` in the fault path, mutexes,
  `thread_join`) loops on its condition, so the wake is spurious there,
  as thread_cancel's always was; none of the kernel threads' plain waits
  (timer service, DMA quarantine, VT-d log) can be reached by it.
  Interrupt against kill: `ERR_CANCELED` first. Against exit:
  `uthread_interrupt` checks `UT_DEAD` under the process lock that the
  exit sets it under, and the join reference keeps the thread. Against
  the start window: an interrupt between `u->t`'s publication and
  start_thread's wake only starts the thread a moment early (as a kill
  there always did).
- **The tid word (K1).** Set only by the thread for itself, checked
  4-aligned and in the user range; cleared in `uthread_exit_current`
  after the last user-mode instruction and before the address space is
  left, by `copy_to_user` (a fault, or since PK2 a pager failure, skips
  it with no wake) and `futex_wake` on the same key (`t->aspace`). Nothing
  of the thread touches user memory after the clear, so musl's joiner (it
  waits on `__thread_list_lock`, which the clear releases) may unmap the
  stack at once. Skipped for a dying process (nobody left to join).
- **Ids (K1).** Taken at create and given back at destroy (a zombie keeps
  its pid); the hold for the lap is right for both sides of the cursor;
  every failure path of process_create and uthread_create gives the id
  back exactly once.
- **The new calls' checks.** 84: no handle, the address checked. 88:
  `OBJ_THREAD` with `RIGHT_SIGNAL`; ERR_BAD_STATE before the start and
  after the exit. 89: `RIGHT_INSPECT`, the struct zeroed, no padding
  (pinned in abi_check.c). 136: no handle, nothing secret (free pages stay
  behind `RIGHT_ROOT_SYSINFO`). 137: no handle; the handle's
  `TIME_PAGE_RIGHTS` exclude write and resize, so no holder can write,
  decommit, resize, pin for writing or map it writable; `vmo_make_exec` of
  it is refused (the kernel's writable mapping is a mapping).
  `sys_object_wait_one` copies `observed` out for `ERR_INTERRUPTED` too;
  `port_wait` copies a packet only on OK.
- **The time page (K2).** The writer (under the wall clock's spinlock)
  stores the word odd, a release fence, the fields, the word even with
  release; the reader loads the word with acquire (waiting out odd), the
  fields, an acquire fence, the word again: the standard seqlock, correct
  on any CPU. The TSC fields are written once before anyone can ask for
  the page and never change (the kernel never recalibrates). The
  conversion is exact: `time_page_uptime` is `tsc_to_ns`'s floor for
  every delta (mult rounds down, the remainder loop adds back at most 2).
  libc reads the offset under the word and the TSC after it, a
  consistent moment.
- **VMAR_REPLACE (K2).** Every check that can fail precedes any change,
  including the reverse-map entry's size check; the cut is aspace_unmap's
  own (`cut_locked`), entries shot down before the reverse map lets go;
  the cap is counted after the replace. The region lock is held from the
  first look to the new mapping's insertion, and every installer of user
  entries (`aspace_fault`, `fill_kept`) holds it too; a fault waiting for
  a pager (PK2) dropped it and starts again from `find`, so it meets the
  old mapping or the new one. Decommit and shrink of either VMO only
  clear entries and find the mapping through the reverse map as for any
  unmap. Pins are on the VMO's pages, not on the mapping, and are
  untouched by it.
- **M12.5 in K1's paths.** The fault path's pager wait is cancellable and
  never interruptible (`pager_fault_interrupt`), the interrupt left
  latched; `vmo_read`'s wait the same (K1's notes, "Not done"). PK4's
  `ERR_BAD_STATE` from a fault becomes `ERR_IO` for a copy, so the tid
  word's clear and `futex_wait`'s read treat it as a failed copy.
