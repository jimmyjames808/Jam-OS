# FR1: the kernel review of fork (FK1, FK2, FK3)

Track FR1 of [FORK-PLAN.md](FORK-PLAN.md) ("5. Tracks"): the kernel side
of fork and exec, read in full context at main 401b5f3a by an agent that
wrote none of it.

- **FK1**, clone modes (efdfb783): `VMAR_CLONE_SHARE`, `VMAR_CLONE_NONE`,
  `clone_mode` and `clone_ok` in `kernel/mm/aspace.c`, kept by split,
  merge, protect and replace.
- **FK2**, `vmar_clone` (c49326a2, fe1938c5): `aspace_clone` and its walk,
  the staging tree, the table reserve, `vmo_clone_begin`, `_share` and
  `_end` in `kernel/object/vmo.c`, the freeze (`protecting`: every writer
  waits), `process_clone_begin`, random handle generations.
- **FK3**, `process_replace` (c00fd5a0, d78c3d8a): the claim, the other
  threads' end, the swap, `handle_table_close_all`, what of the thread is
  cleared (SG1/SG2's exception state).

And where they meet what was merged since: WK1 to WK3's writable pager
VMOs (the clone-mode override, dirty pages across a clone, the dirty
limit's store wait, which drops the region lock), CBF2a's channel writer
pid and CBF4's `process_info.faults` across a replace, FL3's exec in place
(libjam's `spawn_replace`, whose kernel half is FK3's).

Looked at first: every way out of `aspace_clone` (charges, children,
twins, `protecting` lowered once per raise), every writer of a VMO page
against the freeze (faults, `vmo_write`, decommit, resize, pins, kernel
mappings, separations), the lock order of `setup`, the region locks, the
VMO lock and the pager waits, every reader of `struct process`'s `as`,
and every wait a thread of a replaced or killed process can be in.

Severity as in [CR-REVIEW-KERNEL](CR-REVIEW-KERNEL.md): **High** is a
crash, a hole, or a hang or breakage that a program can cause for others
with hostile arguments; **Medium** is wrong behaviour the system or a test
tier will meet, or a cost a program can push onto every other one; **Low**
is a wart, a mismatch with the documented contract that nothing relies on
yet, or a cost only a holder of a power pays.

Counts: **0 High, 3 Medium, 3 Low, 4 notes.**

## Findings

| # | Sev | Where | What is wrong | How to make it happen | Outcome |
|---|---|---|---|---|---|
| 1 | Medium | `kernel/object/vmo.c:1220` (write_bytes: get_page, then the copy with no lock) against vmo_clone_begin (`kernel/object/vmo.c:2198`) and share_pages (`kernel/object/vmo.c:2070`) | A `vmo_write` takes its page (GET_PRIVATE: not shared, not frozen, under the VMO lock) and copies its bytes into it after the lock is dropped. A clone (or a copy-on-write child's creation) that freezes the VMO in between shares that page with the child and returns; the bytes land afterwards, in the page the child now holds. So a forked child's private memory changes after `fork` returned, with nobody writing to the child: FORK-PLAN 1.1's "the child holds the memory as it was at one moment" is broken, and a `VMO_CHILD_SNAPSHOT` child likewise (that half predates FK2). The stale-entry writes the freeze allows end at the clone's shootdown; this one has no bound. | Thread A: `vmo_write(V, 0, new)`, stopped between its page and its copy (`DBG_VMO_WRITE_COPY`, or a preemption). Thread B: `vmar_clone` of an address space mapping V as a copy. The child reads the old bytes, then A goes on and the child reads A's bytes. | Fixed (fe25aaee): `vmo_write` takes its pages as GET_WRITE, counted in the VMO's `inflight` (in the same lock hold that saw it not frozen) until its copy is done; `vmo_clone_begin` and a creation's `share_pages` wait for the count to reach 0 right after raising `protecting` (freeze_drain_locked: the copies are from a kernel buffer and hold nothing, so the wait is short and waits for nothing else). ktest `vmar_clone_write_in_flight`, a clone and a creation (without the fix: the child 1 then 2). |
| 2 | Medium | `kernel/object/vmo.c:475` (cow_wait) and `kernel/object/vmo.c:1592` (vmo_set_size) | The waits for a freeze are not cancellable. FK3's replace ends the other threads as a kill does and waits for them all; a thread waiting for someone else's clone or child creation (a write fault on a frozen VMO, `vmo_write`, decommit, resize) ignores the cancel until that freeze ends. A freeze lasts as long as its clone's walk from that VMO on (the source's memory: one program's choice); two processes that map one VMO as copies and fork in loops whose clones overlap keep its count above 0 for as long as they like, and the writer (any process sharing a writable buffer with them) can't be killed and can't exec meanwhile. | A VMO frozen (`vmo_clone_begin`, held); a thread of another process writes it (a fault or `vmo_write`) and waits; kill that process: it stays DYING, its thread in `cow_wait`, until the freeze ends. | Fixed (da557e41): both waits are cancellable; `ERR_CANCELED` comes back from `vmo_write`, `vmo_decommit` and `vmo_set_size` with nothing changed, and from a fault (the fault path already ends the thread quietly for it, as for a pager's wait). ktest `vmar_clone_frozen_cancel`: each of the four waits on a VMO held frozen, then a cancel (without the fix: "went on waiting"). The starvation itself: design question 1. |
| 3 | Medium | `kernel/object/process.c:352` (process_get_info, CBF4's `faults`) | `faults` is read from `p->as` after the process lock is dropped. A replace (FK3 swaps `as` and drops the old one) or a teardown (`as` to NULL, then dropped) in between leaves it reading the `faults` word of a freed `struct aspace`: a use-after-free read of kernel heap, returned to user space (whatever reuses the slot). Today's code loads `p->as` once (checked in the object code); a compiler that loads it twice would hand `aspace_fault_count` a NULL (a kernel page fault). The comment says "as aspace_maps_jit's", but that one is read under the lock. Also the snapshot is torn: a process reported RUNNING with its threads can come back with the faults of no address space or of the next image. | A parent calls `process_get_info` on its child in a loop while the child execs or exits (any holder of a process handle). | Fixed (193bcdef): read under the process lock with the rest of the snapshot. ktest `replace_info_one_snapshot`: a new hook, `DBG_PROCESS_INFO`, kills the process right after the snapshot's lock (it only widens the window the code had); without the fix the snapshot says RUNNING with 0 faults ("faults 2 before, 0 in the snapshot"). The use-after-free itself is a read of memory nobody poisons, so no test sees it fail; `replace_racing_clone` reads the info of a process replacing itself and dying, millions of times. |
| 4 | Low | `kernel/abi/sysc_vm.c:214` and `kernel/abi/vmar_sys.c:126` (vmar_clone's arrays) | Two `kmalloc`s sized by `n`, up to `VMAR_CLONE_MAX` (65,536): 512 KiB and 1.5 MiB, each one physically contiguous buddy block (orders 7 and 9), not charged to any job. Transient (freed before the call returns), but any program can take them from every thread at once, and they are the blocks drivers' contiguous buffers need. | Eight threads calling `vmar_clone` with `n` = 65,536 (the call fails at the first VMO not mapped as a copy, after both allocations). | Not fixed (not trivial): take `vmos` in chunks of a page, or bound `n` by the VMOs mapped as copies before allocating. |
| 5 | Low | `kernel/object/vmo.c:2238` (vmo_clone_share's copy of a kept VMO) | A writable mapping of a kept VMO is copied page by page (a kept VMO is never shared), but nobody's writable entries over those pages lose their write bit, so its writers go on writing during the copy without faulting or waiting: the child's copy is torn. A thread that writes kept page 1 (just copied) and then kept page 5 (not yet copied) leaves the child 5's new value with 1's old one; the same against another VMO's stale-entry write, which the child does see. | Two threads: one copying a counter into kept pages 1 then 5 in a loop, one forking. | Not fixed (not trivial): take the write bit off the kept range's writable mappings and shoot them down before the copy (a kept-only mapping is read-only, untouched), so its writers fault and wait as any other's. |
| 6 | Low | `kernel/object/process.c:352` with `kernel/mm/aspace.c:2262` | `process_info.faults` is the address space's count, so it starts again at the exec (FK3's swap): `/proc/<pid>/stat`'s minflt goes back to a few after `execve`, where Linux keeps counting across it. | `fork`, `execve`, read minflt. | Not changed: `kernel/include/jam/abi.h` says so (the address space's count, 0 once torn down). Design question 3. |
| 7 | Note | `kernel/object/process.c:1071` (process_replace, what of the thread is cleared) | The replacing thread keeps its kernel names (the uthread's and the scheduler thread's, from the old image's `thread_create`): the log and panic screens show the old image's thread name in the new one. Nothing user-visible reads them. | | No change. |
| 8 | Note | `kernel/object/process.c:959` (process_replace) and `kernel/abi/vmar_sys.c:123` (sys_vmar_clone) | Every handle others hold of the process survives a replace (FK3's point: waits, ports, kill). One with `RIGHT_WRITE` can also make and start a thread in the new image (`thread_create`, `thread_start`) or map into it through a vmar it was given. Fine today: the new image has only what its caller gave it. For FX (an image `/svc/exec` builds with init's authority), a holder of the caller's process handle would get that authority; and FX's replace-only right must not allow `vmar_clone` into the process either (which takes `RIGHT_WRITE` and charges the destination's job). | | Design question 2. |
| 9 | Note | `kernel/object/vmo.c:469` (protect_wq) | One wait queue for every VMO's freeze, woken whole at every freeze's end: with many writers waiting on different VMOs each end wakes them all. A cost only (FK2 chose it). | | No change. |
| 10 | Note | `kernel/object/vmo.c:2198` (vmo_clone_begin of a pager VMO's child) | A copy of a pager VMO's child gets the whole window again (charged, its tables made: a leaf table per 2 MiB, up to 64 GiB), whatever the mappings cover: a fork of a process with a big private file mapping costs the window again, as the parent's child did. As designed (WK1's pager children); written down for CB5's figures. | | No change. |

## Design questions

1. **A writer starved by freezes (finding 2).** Cancellable waits (the
   fix) let a kill or an exec end the wait; the writer still waits for
   as long as freezes overlap. Choices: (a) leave it: only a holder of a
   writable mapping of that VMO can freeze it, and such a peer can
   already scribble on the buffer; (b) a freeze that begins while a
   writer of that VMO waits waits itself for that writer first (fair, but
   a clone holding other VMOs frozen while it waits can deadlock against
   a writer waiting on one of them: needs an order, say a clone that
   would wait gives up and starts again); (c) bound one clone's freeze of
   a VMO by ending it right after that VMO's pages (a shootdown per VMO,
   not one per clone: costs the "one instant" argument, which needs every
   freeze held until the single shootdown).
2. **Handles to a replaced process (note 8).** For FX: drop or narrow
   every other process handle's `RIGHT_WRITE` at a replace into an image
   with authority its caller lacks (a generation on the process's
   write-power), or make FX's images refuse thread creation from outside
   (a flag on the process). Not needed until FX.
3. **minflt across exec (finding 6).** Keep `process_info.faults` per
   address space (today's contract), or add the old image's count to a
   per-process sum at the replace (and at the teardown), as Linux's
   minflt keeps counting across `execve`. An ABI wording change, small
   code; nothing in Chromium is known to care.

## Tests

**The fixes' tests** (each in the commit with its fix):
`vmar_clone_write_in_flight`, `vmar_clone_frozen_cancel`
(`kernel/test/test_vmar_clone.c`) and `replace_info_one_snapshot`
(`kernel/test/test_process_replace.c`, with the new hook
`DBG_PROCESS_INFO`).

**The hostile tests** (FORK-PLAN's FR1 row):

- `vmar_clone_hostile_storm`: eight threads, one per CPU, fork one
  address space (two VMOs mapped as copies, 17 pages touched) at once,
  150 times each, in a job with room for a clone and a half, while a
  writer stores a counter to VMO A and then to VMO B through it. Every
  clone is whole (`aspace_check`) with the child's B never ahead of its
  A, or refused with `ERR_NO_MEMORY` and nothing left; afterwards the job
  is back to the page and a write and a resize go straight through
  (nothing frozen). On the PC: 758 to 962 clones and 238 to 442 refusals
  of the 1,200 tries, about 400 clones caught the writer between A and B.
- `replace_racing_clone`: twenty rounds of a process with a thread
  spinning on another CPU and one in a futex replacing itself, while
  kernel threads clone its address space (old or new) into fresh ones of
  its job, take `vmar_clone`'s claim on its image (`process_clone_begin`)
  and try to fill it, and read its `process_get_info`; every other round
  a kill 0.5 to 9.5 ms in. Every clone whole, every try at the image
  refused (`ERR_BAD_STATE`: mapped, or claimed, or dying), the image
  consumed or dying, and the job empty after each round. On the PC
  8,000 to 145,000 clones, 0.9 to 16 million tries at the image and as
  many info reads per run.

**Without the fixes** (the tests on 3709a488 plus the hook, the PC, 4
CPUs, batch 18083929): `vmar_clone_write_in_flight` failed ("a clone
passed the write, the child 1 then 2"), `vmar_clone_frozen_cancel` failed
("vmo_write went on waiting at the cancel"), `replace_info_one_snapshot`
failed ("faults 2 before, 0 in the snapshot"). The two hostile tests
passed there (at 4 and 8 CPUs): they found nothing the findings above
don't list.

**On the branch's head** (the PC, `JAMOS_TIER=kernel`, batch 18084439):
the full ktest at 4 and at 8 CPUs, 638 passed each; `ktest=vmar_clone
loops=3` at 4 CPUs and `ktest=replace loops=3` at 8, PASS; the init run
(`tools/init-test.sh`), PASS; the shell scripts basic, cmds, commands,
files, posix-execinfo and posix-fork, PASS; `tools/kdump-test.sh save`,
PASS; `make check` on the Mac, PASS. The soak at 4 CPUs: see the report
of the track (two tries failed on things this change doesn't touch,
under a full PC: `intr_deadline_comes_first`'s timing, and a watchdog
panic in the USB bus thread in ring 3 at 12.8 s of boot, as seen
before on a loaded PC).
