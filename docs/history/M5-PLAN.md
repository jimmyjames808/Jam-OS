# M5 plan: ring 3, processes, init

> **Historical.** This plan is finished and kept as written; paths and status in it are as they were then. What it delivered: [HISTORY.md](../HISTORY.md).

Goal (ARCHITECTURE.md milestone row): **init runs from bootfs; a process
killed mid-`channel_call` cleans up; a runaway process hits its job quota,
not a panic.** Everything else in this file serves those three checks.

## What M5 adds

| Piece | What it is |
|---|---|
| Entry path | `syscall`/`sysret` with `swapgs`, user-mode traps and interrupts, SMEP + SMAP, `stac`/`clac` user copies with an exception fixup table, canonical-RIP check before `sysret`, eager XSAVE per user thread, NMI/#MC/#DB that find GS themselves |
| Address spaces | one flat user address space per process (`struct aspace`), VMOs mapped with R/W/X (W^X, exec needs `RIGHT_EXEC`), demand paging on fault, a per-VMO reverse map so decommit/shrink unmap user pages everywhere, per-address-space CPU mask for TLB shootdown |
| Objects | `vmar` (handle to an address space), `process`, `thread`, `job` |
| Jobs | limits on committed pages, handles and threads, inherited by children; queued channel messages charged to the sender's job; syscall-reachable allocations return `ERR_NO_MEMORY` instead of panicking |
| Syscall ABI | one table (`abi/syscalls.def`) generates the numbers, the user wrappers and the kernel dispatch; every existing `sys_*` gets a user-copy wrapper |
| Userland | `user/` tree: `libos` (crt0, syscall wrappers, printf over `debug_write`, a minimal ELF loader), `init`, `utest` (the M5 test program) |
| Boot | bootfs image (one Limine module) with init and the test programs; `userboot`, a tiny in-kernel ELF loader that starts init with its startup message |
| Scheduler | wake-affine handoff for `channel_call`; user priorities capped at 24 |

## Fixed decisions

- **User address space**: `0x0000000000001000 .. 0x00007fffffffe000`
  (`USER_BASE` .. `USER_TOP`). Page 0 and the last page below the canonical
  hole are never mapped, so a `sysret` to a bad RIP can't happen (the entry
  path still checks). Static ELFs link at `0x400000`; no ASLR yet.
- **Syscall convention**: `syscall` with the number in `rax`, arguments in
  `rdi rsi rdx r10 r8 r9`, result (`status_t`, negative = error) in `rax`.
  At most 6 register arguments; a call with more (`channel_call`) passes a
  pointer to an argument struct. `rcx`/`r11` are clobbered (hardware).
- **User pointers are `uint64_t`, never C pointers**, in kernel code. The
  only way to touch user memory is `copy_from_user` / `copy_to_user` /
  `copy_str_from_user` (jam/usercopy.h), which return `ERR_INVALID_ARGS` on
  a bad range or an unrecoverable fault. Kernel code never dereferences a
  user address, and SMAP makes that a crash if it tries.
- **Page faults**: #PF on a user address (from user mode, or from a user
  copy) calls `aspace_fault()`. If that resolves it (demand commit), the
  instruction retries. Otherwise: from user mode the thread's process is
  killed (M5 has no exception channels yet; the kill is logged); from a user
  copy the copy fails with `ERR_INVALID_ARGS`; anywhere else it is a kernel
  panic as today.
- **Lock order** (adds to the existing ones): `aspace` region lock (a
  sleeping mutex, class "aspace") -> `vmo` (object spinlock) -> `aspace
  page tables` (spinlock). Nothing that allocates page tables runs under
  `vmo` except through the page-table lock.
- **Program output**: libos `printf` writes through the `debug_write`
  syscall (bytes straight to klog, prefixed with the process name). The
  startup message also carries a stdout channel handle for later (M7).
- **Startup message**: one channel message on the handle passed in `rdi` at
  entry: a header (`magic, version, argc, envc, nhandles`), then argv/env
  strings, then handle roles (`SELF_PROCESS, SELF_VMAR, SELF_THREAD,
  STDOUT, BOOTFS, JOB`, ...). Layout in `jam/startup.h` (shared with user).
- **Kill**: `process_kill` cancels every thread (`thread_cancel`), waits
  for them to leave the kernel, then closes the handle table and tears down
  the address space. A thread in user mode is stopped at its next kernel
  entry (syscall, interrupt, fault): the return-to-user path checks the
  cancel flag and exits instead.

## Tracks

Phase 1 runs three agents in parallel, each in its own git worktree, on top
of a foundation commit on main that fixes the interfaces between them
(headers + weak stubs, below). Phase 2 joins them.

### Foundation (on main, before the agents)
- `jam/aspace.h`, `jam/usercopy.h`, `jam/uentry.h`, `jam/syscall.h`,
  `jam/startup.h`, `jam/bootfs.h`, `jam/elf.h`: the interfaces.
- `kernel/core/m5_weak.c`: weak stub for every interface function, so each
  track builds and boots alone. A track replaces the stubs it owns by
  defining the real functions (strong symbols win); phase 2 deletes the file.
- `struct thread` gets `aspace`, `process`, `ustate` (XSAVE area) and the
  saved user entry state; `schedule()` calls `arch_thread_switch(prev,
  next)` before `switch_context`; `trap_dispatch` hands #PF to
  `trap_page_fault()` before panicking.

### Track A: entry path (agent 1)
Files: `kernel/arch/x86_64/` (new `syscall.S`, `usercopy.S`, `fpu.c`,
edits to `isr.S`, `trap.c`, `gdt.c`, `cpu.c`), `kernel/linker.ld`
(`__ex_table`).
- MSRs: `STAR`, `LSTAR`, `SFMASK` (clear IF, DF, TF, AC); `EFER.SCE`.
- `syscall` entry: `swapgs`, save user RSP in the per-CPU block, load the
  thread's kernel stack, build a `struct syscall_frame`, call
  `syscall_dispatch(frame)`, check `need_resched` / cancel, restore,
  canonical-RIP check, `swapgs`, `sysretq` (or `iretq` if the check fails
  or the frame was changed to need it).
- Interrupts and exceptions from ring 3: `swapgs` on entry and exit when
  the saved CS is a user selector; TSS `rsp0` = the current thread's kernel
  stack, updated in `arch_thread_switch`.
- NMI, #MC, #DB (IST): decide whether GS is the kernel's by reading the GS
  base MSR, never by trusting CS (they can land in the `swapgs` window).
- SMEP + SMAP on every CPU when supported; `stac`/`clac` only inside the
  user-copy routines; `copy_*_user` with an exception table so a fault
  inside them returns an error instead of panicking.
- XSAVE: enable `CR4.OSXSAVE`, XCR0 = x87|SSE|AVX (what CPUID offers); each
  user thread has an XSAVE area; save/restore eagerly on every switch
  between threads that have one. The kernel stays built with no SSE.
- `arch_enter_user(entry, stack, arg0, arg1)`: first entry to ring 3 via
  `iretq` with clean registers and the default FPU state.
- Tests (ktest + crash tests): a kernel-built ring-3 blob (a few bytes of
  machine code mapped into a test address space by hand-made page tables,
  or through Track B's API once merged) that makes syscalls and returns;
  user copies of good, unmapped, kernel and non-canonical addresses; SMAP
  crash test (`testsmap`: the kernel reads a user page without `stac`);
  SMEP crash test (`testsmep`); an NMI arriving while in user mode.

### Track B: address spaces and VMAR (agent 2)
Files: `kernel/mm/aspace.c` (new), `kernel/object/vmar.c` (new),
`kernel/object/vmo.c` (reverse map + O8), `kernel/abi/vmar_sys.c`,
`kernel/arch/x86_64/ipi.c` (shootdown to a CPU mask).
- `aspace_create/destroy`: a PML4 with the kernel half (256-511) copied.
- `aspace_map(as, vmo, vmo_off, len, flags, &addr)` (fixed address or
  first fit), `aspace_unmap`, `aspace_protect`. Flags R/W/X/USER; W+X is
  refused. Mappings are regions in a sorted list or tree under the region
  lock (mutex); page-table entries are filled on demand by `aspace_fault`,
  which commits the VMO page.
- Reverse map: each VMO keeps its user mappings (aspace, base, offset,
  length). `vmo_decommit` and shrink unmap those pages from every address
  space (shootdown to the CPUs in that address space's mask, free the pages
  only after the flush). Pins still block decommit; user mappings no longer
  do.
- TLB: per-address-space `active_cpus` mask, set/cleared in
  `arch_thread_switch` when CR3 changes; unmaps shoot down only those CPUs.
- `vmar` object + `sys_vmar_map/unmap/protect` (handle layer, kernel
  pointers like the other `sys_*`; phase 2 adds the user copies).
- Fix TODO(O8): decommit/shrink drop the VMO lock between leaf tables.
- Tests (ktest): map/unmap/protect edges, W^X refused, fault-in commits a
  page, decommit of a user-mapped page unmaps it in two address spaces on
  different CPUs (checked through `vmm_translate` on each aspace's PML4),
  shrink under a mapping, stress: 8 threads mapping/faulting/decommitting
  one VMO; no leaked page tables after destroy.

### Track C: userland, ABI generator, bootfs, ELF (agent 3)
Files: `user/` (new), `abi/syscalls.def` + `tools/gensyscalls.py` (new),
`tools/mkbootfs.py` (new), `kernel/core/bootfs.c`, `kernel/core/elf.c`,
Makefile, `boot/limine.conf` (module line).
- `abi/syscalls.def`: one line per syscall (number, name, argument list).
  The generator writes `kernel/include/jam/syscall_nums.h` (shared with
  user code), `user/lib/syscalls.S` (wrappers), and
  `kernel/abi/syscall_table.c` (dispatch table; each entry points at a
  `sysc_<name>` function, with a weak `ERR_NOT_SUPPORTED` default).
  Build-time check that the generated files are up to date.
- `user/`: freestanding build with the same cross gcc (`-mno-red-zone` not
  needed, SSE allowed), `user/linker.ld` at `0x400000`, `crt0.S` (take the
  startup handle from `rdi`, call `main`, then `process_exit`), `libos`
  (syscall wrappers, `printf` over `debug_write`, `memcpy`/`strlen`...,
  a bump `malloc` over a VMO, startup-message parsing), `init/main.c`
  (prints its argv and handles, then runs the tests listed in `init.cfg`),
  `utest/main.c` (stub; phase 2 fills it).
- bootfs: `tools/mkbootfs.py` packs files into one image (header, entry
  table: name, offset, size; 4 KiB-aligned file data). Limine loads it as a
  module; `bootfs.c` finds it in `boot_info`, validates it, and serves
  `bootfs_find(name) -> vmo` (a physical VMO over the module, read-only).
- `elf.c`: parse and validate a static ELF64 (`ET_EXEC`, x86-64, PT_LOAD
  segments inside the user range, no W+X segment, sane alignment, entry
  inside an executable segment) into a load plan. No mapping here: the plan
  is applied by userboot in phase 2.
- Tests: ktests that find `init` in bootfs and parse it; fuzz the ELF
  parser with truncated/corrupted headers (must never read out of bounds or
  panic); `make` builds user programs and the bootfs image into the USB
  image.

### Phase 2: processes, syscalls, userboot, jobs (after the merge)
Status 2026-09-29: built and passing in QEMU (see NEXT.md), except the
three items a parallel agent owns (per-CPU page caches, the stack cache
limit, `channel_call` wake-affine placement). Decisions made on the way:
VMO pages are charged to the job of the process that created the VMO (so
libos's loader pays for a child's data and stack; userboot charges the
child's own job); a process with no threads left exits with code 0; the
bootfs handle carries RIGHT_EXEC (programs map their text from it) but
never RIGHT_WRITE; new calls `debug_report`, `job_get_info`,
`process_get_info`, `thread_set_priority`.
- `process`, `thread`, `job` objects and their syscalls; `process_kill`
  and the return-to-user cancel check; user thread creation on top of
  Track A's `arch_enter_user`.
- `sysc_*` glue for every syscall: user copies in and out, the current
  process's handle table, handle rights. `channel_read`'s user copy must
  still release handles when the user buffer is bad; `channel_call` stamps
  its txid into the kernel copy, never the user buffer.
- userboot: create the root job and init's process, map init's segments
  from bootfs (text RX, data RW, bss, stack with a guard), send the
  startup message, start the thread.
- Jobs: quotas charged at every allocation a syscall can reach; queued
  channel messages charged to the sender's job (closes TODO(M5)/O3c);
  `thread_alloc`, handle-table growth, `kstack_alloc` and the
  `smp_call_others` allocation return errors instead of panicking.
- `channel_call` wake-affine handoff (run the woken server on the caller's
  CPU, or its idle HT sibling: 1.3 us vs 2.0 us round trip on the PC); user
  priority cap at 24.
- Thread stack cache: pages beyond the 256 cached stacks go back to the
  allocator, and `kstack_alloc` failure is an error, not a panic.
- Per-CPU page caches for order-0 pages (a small magazine per CPU in front
  of the buddy lock). The PC benchmark 2026-09-29: page alloc+free is
  53 ns on one CPU but 19 us with all 28 CPUs allocating at once (one
  global lock). Processes faulting pages in on every core will hit this.
  Done when the all-CPU benchmark line is within a few times the one-CPU
  line. The same for kmalloc is M5.5.
- Benchmark lines for the new paths (they become the M5 column of
  BENCH.md): syscall round trip, user page fault, process-to-process
  channel_call (same CPU, HT sibling, other P-core, E-core), address-space
  switch.
- `utest`: runs as a process under init and checks the milestone: syscalls
  and rights, bad pointers get `ERR_INVALID_ARGS`, a NULL dereference kills
  only that process, W^X in user space, channel ping-pong between two
  processes, a process killed mid-`channel_call` cleans up (handles, pages,
  threads all come back), a runaway allocator hits its job quota and gets
  `ERR_NO_MEMORY` (no panic), user FPU/SSE state survives preemption.

## Done when
- QEMU at 4 and 8 CPUs: all ktests, the `utest` suite under init, the
  stress test, and every crash test (old and new) pass.
- The real PC: "All tests" and the 10-minute stress, then init + utest,
  with the RESULTS box showing the utest lines.

## Rules for the agents
- Work only in your worktree and your track's files; the foundation
  headers are the contract. If an interface needs to change, say so in the
  report instead of changing another track's side.
- Every commit leaves the tree building with all existing ktests passing
  (4 and 8 CPUs in QEMU: `tools/qemu-test.sh`). Add tests for everything.
- No `Co-Authored-By` trailer on commits. Don't push, don't touch main,
  never write to a USB disk.
- Don't run tests in long repeated loops; one run at 4 and one at 8 CPUs
  per change is enough unless you are chasing a race.
