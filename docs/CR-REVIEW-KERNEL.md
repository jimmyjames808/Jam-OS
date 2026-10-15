# CR1: the kernel review (CK1, CK2, CK3, CP1's kernel line, CJ1)

Track CR1 of [CR-PLAN.md](CR-PLAN.md) ("Tracks"): every line the kernel
tracks of the Chromium plan changed, read in full context at main
f3255c61 by an agent that wrote none of it.

- **CK1**, big sparse VMOs (128ee0a8, 857adbdf): the top and upper tables
  past 64 GiB, `VMO_MAX_SIZE` 128 TiB, the walks that skip missing tables,
  shrink's freeing as it goes, the charges.
- **CK2**, mappings at scale (5be596c5, 801d12b9, 211773f8; merge
  fca7b571): the AVL tree with gaps (`kernel/lib/avl.c`), first fit,
  merging after map, replace and protect, the cap of 65,536, the mapping
  count in `process_get_info`.
- **CK3**, the reverse map at scale (e18bec61, edd9c7c4, 0047a438): the
  interval tree (`kernel/lib/itree.c`) and its users in
  `kernel/object/vmo.c` and `kernel/mm/aspace.c`.
- **CP1's kernel line** (623d8737): `VMO_VERIFIED_MAX` 1 GiB and
  `kernel/object/vmo_verify.c`.
- **CJ1**, JIT memory (9baa78cd): `RIGHT_ROOT_JIT` (bit 27),
  `vmo_make_jit` (64), W+X only on a JIT VMO's mapping within its
  handle's rights, `PROCESS_FLAG_JIT`. Which programs get the power is
  user space's (`dp_root_rights` in `user/lib/dataprog.c`): CJR's.

Looked at first: the W^X argument at the top of `kernel/mm/aspace.c`
(every writer of user leaves, every place a mapping's flags or maximum
are set, every handle that carries `RIGHT_EXEC`), the lock order and the
races between the region lock's holders and the reverse map's (decommit,
shrink, a copy-on-write separation or creation), the charges on every
error path, and the work one call can do with interrupts off.

Severity, as in the earlier kernel checks: **High** is a crash, a hole,
or a hang or breakage that a program can cause for others with hostile
arguments; **Medium** is wrong behaviour the system or a test tier will
meet, or a cost a program can push onto every other one; **Low** is a
wart, a mismatch with the documented contract that nothing relies on
yet, or a cost only a holder of a power pays.

Counts: **0 High, 3 Medium, 3 Low, 4 notes** (finding 10 came from the
hostile tests, after the rest).

Host checks first, on f3255c61: CK2's model of `kernel/mm/aspace.c`
(the real file, built for the Mac's x86-64 emulation) ported to CK3's
interval tree: 9 runs of 100,000 random maps, replaces, protects, unmaps
and faults against a page-by-page model (mappings, merges, gaps, the
reverse map, charges): all pass. CK1's model of the table code: the
functions it copied from `kernel/object/vmo.c` are unchanged since.

## Findings

| # | Sev | Where | What is wrong | How to make it happen | Outcome |
|---|---|---|---|---|---|
| 1 | Medium | `kernel/object/vmo.c:1211` (zap_umaps_locked) under take_batch_locked (`kernel/object/vmo.c:1263`); also wrprotect_umaps_locked (`kernel/object/vmo.c:1735`) and a separation's cow_replace_locked (`kernel/object/vmo.c:716`) | One decommit or shrink batch (up to 512 pages) clears the entries of every user mapping over those pages under the VMO lock, with interrupts off. CK3 made mappings elsewhere in the VMO free, but mappings over the *same* pages still all cost, and CK2 raised the cap from 16,384 to 65,536 per address space (any number of address spaces may map one VMO). The file headers call it a known limit; the numbers say it is one any program can reach. | One program, no power: map pages 0 to 511 of one VMO 65,535 times (first fit, each a mapping of its own: merging needs contiguous offsets), touch them, then `vmo_decommit` the 2 MiB in a loop from a thread per CPU. Each call holds one CPU with interrupts off for the whole walk, and every other CPU's shootdown waits for it. Measured (ktest `aspace_hostile_overlap_decommit`, the PC): one batch over 65,535 mappings of one page 2.1 to 2.7 ms; over 1,024 mappings of 512 pages, all touched, 0.4 to 0.5 ms, so about 26 ms for 65,535 such (a job that can pay for 256 MiB of page tables). | Fixed (CR1F, design question 1's (a) and (b)): an address space holds at most `VMAR_MAX_OVERLAP` (64) mappings of one VMO over any page (`ERR_NO_RESOURCES`, mmap's `ENOMEM`; counted exactly per page in a second tree of the mappings by VMO page, under the region lock only), and a batch is cut short so its walk costs at most `VMO_BATCH_BUDGET` (65,536: a mapping 128, each page of the batch it maps 1 to clear or 4 to write-protect), down to one page. Measured on the PC (ktest `aspace_hostile_overlap_decommit`, 4 and 8 CPUs, the PC idle). Before, on main: one batch over 65,535 mappings of one page 2.4 to 2.9 ms; over 1,024 mappings of 512 pages 0.54 to 1.03 ms. After: one address space at the cap, 64 mappings of one page 2 to 3 us; 64 of 512 pages, all written, a child's write-protect batches 12 to 33 us and the decommit 17 us; 64 address spaces, 256 mappings of 512 pages: batches of 36 to 61 us (a child) and 30 to 31 us (decommit). Left: one page under 64 mappings from each of many address spaces is one batch, 0.23 to 0.45 ms for 64 address spaces (4,096 mappings); per address space is the bound (the owner, CR1F). |
| 2 | Medium | `kernel/mm/aspace.c:506` (walk_leaves: gather_note always), used by protect_entries (`kernel/mm/aspace.c:1299`) | Every `vmar_protect` that changes a mapping's permissions shoots the range down on every CPU running the process (and flushes the caller's own TLB, the whole of it past 64 pages: `flush_local` toggles CR4.PGE, global entries included), even when it changed no entry: a reservation never touched, or touched and decommitted. That is V8's and PartitionAlloc's commit (`mprotect` to read-write inside a reservation), thousands of times per renderer, and CR-PLAN's section 2 wants commit and decommit cheap. | A process with threads on two CPUs protects an untouched range from no access to read-write: the other CPU takes an IPI (`tlb_mask_flush_count`), every time. | Fixed (57c5244e): a protect asks for a shootdown only when it found an entry present or let a table go; the reverse map's walks still always ask (the file header says why both are safe). ktest `aspace_protect_quiet` (without the fix: 3 IPIs where it wants none). |
| 3 | Low | `kernel/object/vmo_verify.c:67` (vmo_verify_new) | The list of hash pages for a 1 GiB program is a 16 KiB array in one `kzalloc`: a 32 KiB buddy block (physically contiguous, order 3), not charged to the caller's job though every hash page is. On a fragmented machine with plenty free the block can be missing, and the program fails to start with `ERR_NO_MEMORY`. Needs `RIGHT_ROOT_VMEX` (the shell, `/svc/exec`). | `vmo_make_exec_verified` of a 1 GiB pager VMO: one order-3 allocation. | Fixed (9858d54a): the list is 4 pages of 512 addresses, allocated and charged like the hash pages. ktest `pager_verified_max` counts them and marks pages on either side of a list page's edge (without the fix: 2048 pages charged, not 2052). |
| 4 | Low | `docs/CR-PLAN.md:959` | Section 12 says the JIT handle has no `RIGHT_TRANSFER` and "the creator adds it to a duplicate if it hands one on"; a duplicate can only narrow rights (`handle_duplicate`), so that handle could never be handed on. The code keeps transfer (`abi/syscalls.def` 64 says so, ktest `vmo_jit_sys_make` checks it), which is the delegable design the owner chose. | Read the plan, then `abi/syscalls.def`. | Fixed (380abc93): the plan says what the code does. No test: words only. |
| 5 | Low | `kernel/object/vmo.c:1802` (share_pages) | A copy-on-write child's creation skips missing mid tables but not missing leaves: under a mid table that exists it takes both VMO locks once per 2 MiB, so a child of a sparse reservation costs 512 lock holds for every GiB touched at all (V8's 1 TiB cage, one page per GiB: 512 K holds). Decommit and shrink skip both (`leaf_find_locked`). | `vmo_create_child` of a 1 TiB VMO with a page written in each GiB. | Fixed (45bda877, c76f860c): it finds the next leaf with `leaf_find_locked`. ktest `vmo_big_child_sparse`: 1.5 to 2.5 ms on the PC, 17.8 ms without the fix; bound 8 ms, on an idle machine. |
| 6 | Note | `kernel/object/vmo.c:1483` and take_batch_locked's skip (`kernel/object/vmo.c:1229`) | A decommit re-checks for a pin made since it started only over the leaves it works on; a pin made meanwhile over a part with no pages doesn't fail it (CK1 noted the risk). Harmless: nothing there was decommitted, and the pin's pages come after. | | No change. |
| 7 | Note | `kernel/mm/aspace.c:1220` (aspace_map) | `PROCESS_FLAG_JIT` is set for any mapping of a JIT VMO, also through a handle without `RIGHT_EXEC` (or without write), and a moment after the mapping exists (after the region lock). Errs on the side of marking. | | No change. |
| 8 | Note | `kernel/mm/aspace.c:1250` (protect_span) against `kernel/include/jam/aspace.h:81` | For a W+X protect over a gap followed by a mapping that isn't JIT memory the answer is `ERR_NOT_FOUND`; the header lists `ERR_INVALID_ARGS` first. Either refuses. | | No change. |
| 9 | Note | `kernel/object/vmo.c:2450` (vmo_pin_flags) | A JIT VMO may be pinned for a device (a driver holding a JIT handle with the pin rights can let a device write code into it). Not a new power: that handle writes the memory anyway. For CJR to judge. | | Design question 2. |
| 10 | Medium | `kernel/mm/aspace.c:431` (pt_prepare's refusal) with walk_one (`kernel/mm/aspace.c:477`) | Found by the hostile TiB test at 8 CPUs, after the list above. When the job refuses a page table, the PDPT and PD pt_prepare made for it on the way stay, empty and charged ("freed by unmap/destroy", says the comment), but no walk ever frees them: walk_one lets a PD go only after a PT under it went, and a PDPT only after a PD. So they stay charged to the job until the process dies, two pages for every 1 GiB region a fault was refused in: a program near its limit (a renderer under memory pressure) loses them for good. | A job one page short: fault a fresh GiB, `ERR_NO_MEMORY`, then unmap everything: `aspace_pt_pages` is still 2. | Fixed (8db1fe61): pt_prepare takes back the tables it made when the next one is refused, through a shootdown (a CPU running the address space may have cached a link to one), as table_drop's go. ktest `aspace_fault_refused_tables` (without the fix: a PDPT left, then a PDPT and a PD). |

## Design questions

1. **Mappings over the same pages (finding 1).** The choices, roughly
   from cheapest: (a) cap how many user mappings one VMO may have over a
   page, per address space or in all (Linux has none, but its reverse
   map's lock sleeps); (b) shrink a batch so that pages times mappings
   over them stays under a budget (bounds the 512 times factor, not the
   65,536); (c) a walk that drops the VMO lock between mappings and
   starts again when the tree changed (the pages are out of the table
   and freed only after the shootdown, so stale entries are harmless
   meanwhile, but a merge or a move can hide a mapping from a walk that
   passed it, so it needs a generation count and a restart, which a
   hostile mapper can keep restarting). **Decided: (a) per address space
   and (b), not (c)** (CR1F). In-tree, no program maps one VMO over the
   same page more than a few times (an ELF's two segments sharing a page,
   a JIT's two views, a shared region mapped in a few places, as
   Chromium's read-only twin of a memfd), so the cap is 64.
2. **Pins of JIT memory (note 9).** Refuse them (as for pager VMOs), or
   leave them to the handle's rights? CJR's call.

## Tests

**The hostile tests** (`kernel/test/test_mm_hostile.c`, and three in
`kernel/test/test_vmo_jit.c`), with `aspace_check` (a test helper in
`kernel/mm/aspace.c`: the tree, gaps, merging, every reverse-map entry,
the charges and every entry present, W^X entry by entry):

- `aspace_hostile_tib_reservations`: a job allowed 16 MiB (4,096 pages)
  reserves 100 TiB in a hundred 1 TiB VMOs mapped with no access (8
  pages charged: the PML4 and the mapping structs); four threads commit
  pages a GiB apart by protect and touch until the job refuses: about
  780 pages, never past 4,096 pages charged, every refusal
  `ERR_NO_MEMORY`, the machine paying no more than the job and the
  records; each 1 TiB decommit 23 to 134 us; protect back and a shrink
  return the job to its 8 pages. At 8 CPUs it found finding 10.
- `aspace_hostile_protect_storm`: 60,000 mappings of one 512 MiB
  reservation; eight threads protect pieces at random (and touch some)
  while one unmaps and replaces pieces and one decommits and snapshots
  the VMO, for 2 s (at 8 CPUs 165,797 protects, 17,709 unmaps, 9 million
  decommits; at 4 CPUs the never-sleeping decommitter keeps preempting
  the region lock's holder: 2,233 protects); afterwards `aspace_check`,
  the reverse map's count and the job's charge exact.
- `aspace_hostile_overlap_decommit`: finding 1's fix: the cap (first
  fit, fixed, a range, a replace in place of one), one address space at
  the cap over 512 pages (a child's write-protects in batches within the
  budget, the decommit in one batch), and 64 address spaces mapping one
  VMO (one page under 4,096 mappings; 256 mappings of 512 pages, a
  child's write-protects and a decommit in batches within the budget);
  the longest batch of each printed.
- `vmo_jit_hostile_alias`: a program with every power but JIT tries each
  way to a writable and executable alias of one VMO (a second mapping, the
  JIT call without the power or with a VMO as its root, the old way to
  code, which seals first, a copy-on-write child of approved code): all
  refused, the approved page unchanged, the address space never marked.
- `vmo_jit_hostile_handoff`: JIT memory handed by a holder of the power to
  a table without it, narrowed: W+X there and marked, but no passing it
  on (a duplicate has no transfer either), no JIT memory of its own, no
  plain copy to keep or seal, W^X on its own VMOs; an execute-only
  duplicate never writes by any route.
- `vmo_jit_hostile_spawn`: a parent holding its child's vmar maps into
  it: plain memory never W+X there, JIT memory marks the child and stays
  the parent's to change; no fork-like snapshot of JIT memory, and one of
  plain memory never executes.

**Without the fixes** (each fix undone for one run, uncommitted, the PC, 4
CPUs): `aspace_protect_quiet` failed (3 IPIs for untouched protects),
`pager_verified_max` failed (2048 pages charged, not 2052),
`aspace_fault_refused_tables` failed (a PDPT left charged), and
`vmo_big_child_sparse` took 17.8 ms (its bound was then raised from 25
to 8 ms, c76f860c).

**On the branch's head** (the PC, `JAMOS_TIER=kernel`): the full ktest at
4 and at 8 CPUs, 555 passed each; the init run (`tools/init-test.sh`),
PASS; `jit.txt`, `tools/pager-test.sh` and `tools/posix-mmap-test.sh`
(every case), PASS; one soak at 4 CPUs, PASS (5 loops, 2,661 kernel tests,
0 FAILED; the job tree +674 pages and +4,800 message bytes, against
main's +635 and +4,800); `make check` on the Mac, PASS (`avlcheck`,
`itreecheck`, checkdocs). `tools/bigprog-test.sh` FAILED twice, the same
way as on main (the pre-push tester's run on main shows it too): the
boot's timer check prints its lines at about 1.07 s, in the middle of the
first "checking /data/big (300 MiB): 10% 20% ..." line, which the script
then counts once instead of twice. The script's, not the kernel's: not
changed here (CP1's file).
