# CJR: the JIT review (CJ1, CJ2's first half)

Track CJR of [CR-PLAN.md](CR-PLAN.md) ("Tracks"): JIT memory read
against the owner's rule (2026-10-11, [section 12](CR-PLAN.md#12-the-jit-by-capability)):
JIT by capability, delegable handles; every other program keeps "never
writable and executable". Read at main 9d540249 (CR1F merged; line numbers of the unfixed findings are of main 82acedcf) by an agent
that wrote none of it.

- **CJ1**, the kernel half (9baa78cd): `RIGHT_ROOT_JIT` (bit 27),
  `vmo_make_jit` (syscall 64), W+X only on a JIT VMO's mapping within its
  handle's rights, `PROCESS_FLAG_JIT`. CR1 read it first
  ([CR-REVIEW-KERNEL.md](CR-REVIEW-KERNEL.md): no escalation, W^X holds)
  and left design question 2 (pins) here.
- **CJ2**, the program half: `right jit` (`user/include/wants.h`,
  `user/lib/wants.c`, `tools/checkwants.py`, `allow`'s line in
  `user/services/shell/cmd/allow.c`), the starters' grant through
  `dp_root_rights` (`user/lib/dataprog.c`: the shell, `/svc/exec`, init's
  apps; init's test run through `test_root`), `ps`'s mark, libc's
  `MAP_JIT` (`user/posix/sys/mem.c` and the musl overrides).

The questions, each path tried from a program without the power: can it
get memory both writable and executable (protect, map, replace, a
duplicate, a copy-on-write child, a self-image, `/svc/exec` with CR2F's
rewritten hand-over, CR1F's `VMAR_MAX_OVERLAP`)? Can a JIT VMO become
approved code (sealed, verified, paged, mapped as a program image, handed
to spawn)? Can a handed-on handle widen its rights? Does the power reach a
program only through the owner's approval (an allow line, a boot-image
list the build checked, init's test run, a package's tagged line from
PO5)?

Severity, as in the kernel reviews: **High** is a hole (a program without
the power gets W+X, or approved code becomes writable), **Medium** is
wrong behaviour the system or a test tier will meet, **Low** is a wart or
a mismatch with the documented rule that nothing relies on yet.

Counts: **0 High, 0 Medium, 3 Low, 4 notes, 1 design question.** No
program without the power gets W+X memory, and no JIT memory becomes
approved code, by any path tried; a holder of the power can hand on more
than it may mean to (finding 1).

## Findings

| # | Sev | Where | What is wrong | How to make it happen | Outcome |
|---|---|---|---|---|---|
| 1 | Low | `user/lib/spawn.c:143` (load_segment), the handle from find_image (`user/lib/spawn.c:362`) | A child started from a VMO of its caller maps its code and read-only data straight from the caller's handle, so each mapping's maximum is that handle's rights. A JIT VMO's handle may write and execute: the child's code is mapped read and execute, but the child (no power, no handle that writes) may `vmar_protect` it writable and executable, and its writes land in the caller's JIT memory. Its `SR_SELF_IMAGE` is narrowed (IMAGE_RIGHTS: "run the program again, never change it"); its mappings aren't. Every other image handle (the boot image's, a sealed VMO's) has no write right, so only JIT memory is affected. | A holder of the power copies a program into a JIT VMO and `spawn`s it with `.vmo` that handle; the child protects a page of its own code to `VMAR_READ \| VMAR_WRITE \| VMAR_EXEC`: OK, where its own handles allow no write. | Fixed: spawn maps a caller's VMO through a duplicate without `RIGHT_WRITE` and `RIGHT_RESIZE` (a handle without them is used as it is). utest `jit_spawn` (utest copied into a JIT VMO and started from it, mode `jit-text`: marked, its image run-only, its code page's protect to W+X `ERR_ACCESS_DENIED`; without the fix the protect succeeds). |
| 2 | Low | `user/services/init/apps.c:73` (start) | A desktop app's root is a duplicate made with `dp_root_rights` (never `RIGHT_TRANSFER`) and sent `RIGHT_SAME` in the startup message, whose write needs `RIGHT_TRANSFER` on every handle: spawn fails (`ERR_ACCESS_DENIED`) and the app never starts, for any list with a `right` word (`jit` or another). Section 12 names init's apps among the starters that give the power. Latent: neither desktop app (terminal, jamjar) asks for a right. The shell and `/svc/exec` duplicate their root as it is and narrow it in the send (`extra_rights`), which works. | An app in `<deskapps.h>` whose list says `right jit` (or `right clock`), launched from the search box: init's log says it could not start. | Fixed (CJF): apps.c duplicates its root as it is and gives `dp_root_rights` as the extra's rights, as `/svc/exec` does. The shell's channel may now launch init's test programs (apps.c's `test_apps`: bin/jittest, `right jit`), the compositor's never; `jit.txt`'s `launch jittest` (without the fix: `init: launch jittest: ERR_ACCESS_DENIED`). |
| 3 | Low | `kernel/mm/aspace.c:1502` (aspace_map; CR1's note 7) | `PROCESS_FLAG_JIT` is set for any mapping of a JIT VMO, also one without execute through a handle without `RIGHT_EXEC`. So a holder of the power can put `[jit]` on another process: it hands on its JIT VMO in place of shared memory and the receiver maps it to read or write. `ps` then says the receiver "writes and runs its own code". CR1 left the mark erring on the side of marking; this is the hostile side of that choice. | A JIT program sends a JIT VMO (narrowed to read, write and map) as a memfd over `SCM_RIGHTS` (or, in Chromium, as a shared-memory region over Mojo) to the browser; the browser's `mmap(PROT_READ \| PROT_WRITE, MAP_SHARED)` marks it. | Fixed (CJF): the mark comes only with JIT memory mapped executable (aspace_map with execute, or a protect that adds it). ktest `aspace_jit_mark_needs_exec` (mapped read-write, through a handle with or without execute, protected short of execute or refused it: not marked; protected to execute, or mapped so: marked; without the fix the read-write mapping marks it). |
| 4 | Note | `user/lib/dataprog.c:379` (dp_root_rights), `user/posix/sys/spawn.c:373` (start), `user/services/init/exec.c:497` | The power itself can't be handed on, only JIT VMOs: a program's root has no `RIGHT_TRANSFER`, `posix_spawn` gives a child no root, and `/svc/exec` gives a child its own approval, so every process of one binary gets the power alike. Section 12's "the browser hands it to renderer children only" (CB5) has no way to do it yet. | | For CB5: see "Design questions" 2. |
| 5 | Note | `user/services/init/pkgapprove.c:306` | PO5's `pkgctl.approve` approves a package's `right jit` on the ports key's signature alone (init has no terminal to ask on); section 12 puts the owner's question in bin/pkg (PO6), which doesn't exist yet. Today only utest calls `approve`. | | For PO6's review: bin/pkg shows `allow`'s JIT line and asks before it calls `approve`. |
| 6 | Note | `user/services/init/main.c:190` (run) | init's test run gives a program `RIGHTS_BASIC` (with `RIGHT_TRANSFER`) on its root, so utest could hand the power to anything it starts; and bin/utest and bin/jittest, both JIT holders by their lists, are in every boot image, given the power by any shell that starts them. Both test-only by design. | | No change. |
| 7 | Note | `user/services/init/exec.c:480` (start_child) | A caller of `/svc/exec` without the power starts an approved JIT program with arguments, environment and descriptors of its choosing (for Chromium: `--js-flags`, a page to load). The power stays the child's own approval's (the owner's rule), and the child is `AT_SECURE` (CR2F). A deputy's question for CR-f's sandbox, not a way to the power. | | No change. |

## Design questions

1. **Pins of JIT memory (CR1's note 9).** `vmo_pin_flags`
   (`kernel/object/vmo.c:2549`) pins a JIT VMO as any other: a pin a
   device writes through needs `RIGHT_WRITE` on the handle, a read-only one
   (where the IOMMU enforces it) `RIGHT_READ`. The options:
   - (a) **Leave it to the handle's rights** (as now). Nothing new: a
     driver given a JIT handle that writes could `vmo_write` the code
     itself. Against: a device becomes a writer of executable memory that
     the stricter modes section 12 plans (a per-thread write switch with
     protection keys, a write view and an execute view) can't see or
     switch (a device's write ignores PKRU and any view); and a pinned page
     stays committed, so V8's decommit of code pages (a `MAP_FIXED` over
     them: libc's decommit is advice) would keep the old code silently.
   - (b) **Refuse every pin of a JIT VMO** (`ERR_NOT_SUPPORTED`, a pager
     VMO's answer; the test under the VMO lock in range_add, where
     `vmo_make_jit` looks for pins, so a pin and the marking can't pass each
     other). JIT memory is then written only by threads, through mappings
     and `vmo_write`. No use is lost: no driver in the tree, and nothing in
     Chromium's plan, gives a device code memory. One check and a ktest.
   - (c) **Read-only pins only.** A device may read code, never write it.
     No use for that either.

   **Recommended: (b)**: the smallest rule to reason about once the
   stricter modes come, at no cost today, and the same answer
   `vmo_make_jit` gives a VMO that is pinned already. The main session
   decides. **Decided: (b)**, done by CJF: range_add refuses a pin of a
   JIT VMO under the VMO lock; ktests `vmo_jit_pin_refused` (a pinned VMO
   isn't marked; a JIT VMO's pins refused, writable or read only, nothing
   committed or kept; without the fix the pin succeeds) and
   `vmo_jit_sys_make_races_pin` (a pin made at the marking's check makes
   it refuse).

2. **Handing the power to renderers only (note 4, for CB5).** The options:
   - (a) **`RIGHT_TRANSFER` on the root of a program whose list says
     `right jit`**: it can pass a duplicate narrowed to `RIGHT_ROOT_JIT`
     in spawn's extras to the children it picks. But it could pass its
     other powers on too (klog, sysinfo, clock).
   - (b) **A second handle for the power alone**: a starter that gives
     the power also gives a root narrowed to `RIGHT_ROOT_JIT` with
     `RIGHT_TRANSFER` (a startup role of its own, which libc's `MAP_JIT`
     looks for besides `SR_RESOURCE`); the program hands that on, and
     nothing else of its root.
   - (c) **`/svc/exec` for every child**: each gets its own approval, so
     every Chromium process type holds the power, and "renderers only" is
     `--jitless` on the others: a flag, not a capability.

   **Recommended: (b)**: the power is delegable by its holder, as the JIT
   VMOs are, and nothing else is. CB5's to decide with the launcher.
   **Built in CR-e: (b)**, as `SR_JIT` (Fixes, below).

## Checked, no finding

- **W^X in the kernel** (CR1's argument at the top of
  `kernel/mm/aspace.c`, read again): `perms_ok` and `protect_span` allow
  W+X only over JIT VMOs, and only within each mapping's maximum, which
  comes from the handle's rights; a merge needs equal maxima, so an
  execute-only duplicate's mapping never joins a writable one's. Every
  `RIGHT_EXEC` in the kernel: the boot image's handles (no write),
  `vmo_make_exec`'s and `_verified`'s (write taken away, sealed), and
  `vmo_make_jit`'s. userboot maps nothing writable and executable. No call
  writes another process's memory (no debugger write).
- **A duplicate, a replace, a send**: `handle_duplicate`,
  `handle_replace` and `channel_write_rights` only narrow (a set bit 31
  with others is refused too); every send and `process_start`'s `arg0`
  need `RIGHT_TRANSFER`. A handle in flight counts as a handle, so
  `vmo_make_jit` can't take a VMO another holder still reaches.
- **A copy-on-write child**: a JIT VMO has none (both kinds refused under
  "vmo resize"); a child of approved code gets a fresh VMO's rights (no
  execute); a child that shares pages has them in `committed`, so it can't
  become JIT memory holding someone else's bytes.
- **Approved code**: a JIT VMO is never sealed (`vmo_seal`,
  `vmo_seal_verified`), never a pager VMO or given a pager;
  `pager_supply` copies from a buffer, so no JIT page ever backs a
  verified VMO. `/svc/exec` seals only a pager VMO from the caller's
  `file.map` (a forged file server answering with a JIT VMO is
  `ERR_NOT_SUPPORTED`), and the shell runs only boot-image and verified
  VMOs. spawn takes a JIT VMO as an image (finding 1): the child runs code
  its caller wrote, with no more than the caller's grants, and is marked.
- **The self-image**: `SR_SELF_IMAGE` is a duplicate with IMAGE_RIGHTS
  (no write) of a sealed, verified or boot-image VMO; a `/proc/self/exe`
  child gets the same and no root.
- **`/svc/exec` and CR2F**: the child's root is `dp_root_rights` of the
  list read from the sealed VMO, with `dp_wants_refused` applied; the
  rewritten hand-over carries the caller's descriptors' handles as they
  are (no widening), and a root can't be in it (no `RIGHT_TRANSFER`).
- **CR1F's `VMAR_MAX_OVERLAP`**: checked before anything changes in a map
  or a replace; JIT patterns are far under it (V8 maps each page of a
  reservation once, two views would be 2), and the cap is per address
  space, so a holder of a duplicate can't use it against the creator.
- **The grant paths**: one parser for the shell, init and `/svc/exec`
  (`wants_read`), and `tools/checkwants.py` reads the same PT_NOTE notes
  with the same refusals (two lists, a byte that isn't printable ASCII);
  `right jit` builds only under ports/, user/ports/ and user/tests/; a
  program on `/data` gets it only from its allow line (the bytes hashed,
  every page checked, the list read from them), after `allow`'s own line
  about it; `/data`'s `etc` is in no program's namespace but the shell's.
  libc's `MAP_JIT` is a courtesy over the kernel's rule (`EPERM`
  without the power, `EACCES` for execute elsewhere); the kernel refuses
  what libc lets through.
- **The mark**: no way to run JIT code unmarked (a mapping that executes
  is made by `aspace_map` in that address space, which marks it).

## Tests

**The hostile tests** (from a program without the power):

- posixtest's jit section (the init run, where init's run gives it
  `RIGHT_ROOT_VMEX`, and `posixtest jit` from the shell, where it has no
  root): its own code and read-only data (W+X `ERR_INVALID_ARGS`, write
  `ERR_ACCESS_DENIED`, `mprotect` `EACCES`), its stack and heap (W+X
  `ERR_INVALID_ARGS`, execute `ERR_ACCESS_DENIED`; mapped over in place by
  `VMAR_REPLACE` and `MAP_FIXED`: refused, the page unchanged), a VMO
  handle widened to execute by a duplicate, a replace and a send, a
  copy-on-write child, the root widened to the power, `vmo_make_jit` with
  its root, no root or a VMO as the root, sealed code it wrote (never
  writable, never JIT memory), and no mark. All held before any fix.
- utest `jit_spawn` (finding 1): a program started from JIT memory,
  without the power: marked, its image run-only, its code never writable.
  Without the fix (the PC, 2026-10-16): `jit_spawn: FAILED ...
  info.exit_code == 0: 1 vs 0` ("our code is writable and executable").
- CR1's kernel hostile tests (`vmo_jit_hostile_alias`, `_handoff`,
  `_spawn`) cover a second mapping, a hand-off narrowed or not, and a
  parent mapping into its child.

**On the branch's head** (fecfa8dc, main 82acedcf merged; the PC, one
batch): the init run (`tools/init-test.sh`, `jit_spawn` and posixtest's
jit section in it), ktest at 4 CPUs (555 passed), `jit.txt`, `allow.txt`,
`basic.txt`, `cmds.txt`, `commands.txt`, `files.txt`, kdump `save`,
`tools/pager-test.sh` case `exec` (both boots), `tools/posix-mmap-test.sh`
case `spawn`: all PASS. `make check` on the Mac, PASS. No kernel change.

## Fixes

- Finding 1 (dce73960): `user/lib/spawn.c`'s find_image.
- Design question 1 (CJF, the main session's choice (b)):
  `kernel/object/vmo.c`'s range_add refuses every pin of a JIT VMO.
- Finding 2 (CJF): `user/services/init/apps.c`'s start duplicates its
  root as it is and gives `dp_root_rights` as the extra's rights
  (`extra_rights`), as `/svc/exec` does. No desktop app asks for a right
  yet, so the test is a test program the shell's channel may launch
  (bin/jittest, in `jit.txt`).
- Finding 3 (CJF): `kernel/mm/aspace.c`'s aspace_map and aspace_protect
  set the mark only for JIT memory that may execute.
- Design question 2 (CR-e, option (b)): the power alone as `SR_JIT`
  (`user/include/roles.h`; every starter gives it for `right jit`, never on
  the root), handed to a forked child (`user/lib/fork.c`) and across an exec
  of `/proc/self/exe` (`user/posix/sys/spawn.c`), closed by
  `prctl(PR_SET_MDWE)` (`user/posix/sys/prctl.c`, `mem.c`); Chromium's
  non-renderers close it at their start (ports/chromium patch 0914;
  [CR-PLAN](CR-PLAN.md#cr-e-notes)).

# CJR2: the JIT's second half (CR-e, SwiftShader)

Track CJR2 (2026-10-19): the code CJR never saw, read by an agent that
wrote none of it, at main d3ee42b9 (line numbers are of that commit): the
JIT merge 5d3d728a (7820f3d0 `SR_JIT` and `startup_handle_drop`; ee610670
the power as `SR_JIT` alone, handed on to forked children and an exec of
`/proc/self/exe`, given up with `prctl(PR_SET_MDWE)`, and `MAP_FIXED` over
JIT memory decommitting in place; e280e16d ports/chromium's patches 0913
and 0914; 9730bcb4 cr-test's jit case), `ps`'s `[jit]`, and SWS's merge
d3ee42b9 (0913 for memory asked executable from the start, 0914's GPU
process under SwiftShader, 0915's Reactor in `MAP_JIT` memory, cr-test's
webgl case).

The questions: can any process but a renderer (or the GPU process under
SwiftShader) end up able to map memory writable and executable, by
posix_spawn, a fork then an exec of another program, an exec of
`/proc/self/exe` with other arguments, a crash and restart, the browser
itself, or a child that never calls `prctl`? Is `PR_SET_MDWE` irrevocable
and inherited as Linux's is, or can a process get the power back (a
handle left in its table, a duplicate, a startup handle not dropped)? Can
a web page, or a flag a page can influence, turn on SwiftShader's JIT in
the GPU process? Does the decommit in place leave stale code, a writable
alias, a leaked VMO, or a race with a thread running the code? Does the
kernel, not only libc, still keep "never writable and executable" for
every program without the power? Is `[jit]` exact? Severity as in CJR.

Counts: **0 High, 1 Medium, 4 Low, 4 notes; SWS's question 1 answered.**
No process without the power gets W+X memory: the kernel is unchanged
since CJR and stays the rule's one enforcer, and every path the power
takes is a handle its holder chose to pass. The Medium is a renderer told
`--jitless` keeping the power; the Lows are the browser's own use of the
power it hands on, `/svc/exec` ignoring `PR_SET_MDWE`, `PR_SET_MDWE`
missing a second handle, and what `[jit]` says.

## Findings

| # | Sev | Where | What is wrong | How to make it happen | Outcome |
|---|---|---|---|---|---|
| 1 | Medium | ports/chromium/patches/0914-jit-for-renderers-only-on-jamos.patch:63 (`ContentMainRunnerImpl::Initialize`'s test) | A renderer keeps `SR_JIT` whatever its V8 is told. One whose `--js-flags` say `--jitless` makes no code, yet can map W+X memory: the browser's way to run a site's renderer without a JIT (`RenderProcessHostImpl::IsJitDisabled` adds `--js-flags=--jitless`; on Windows such a renderer also loses the right to make code), or the owner's own `--js-flags=--jitless`. | cr-test's jit case's second load and the multi case (`--js-flags=--jitless`): every renderer holds the power it never uses; one that is compromised calls `mmap(MAP_JIT)` and has W+X pages. | Fixed (cb370c82): a renderer whose `--js-flags` end in `--jitless` (the last of `--jitless`, `--no-jitless`, `--nojitless`, as V8 reads them) gives the power up with every non-renderer; gin logs at `VLOG(1)` whether V8 compiles code and whether the process holds the power. cr-test jit (`--vmodule=v8_initializer=1`) checks every renderer of the `--jitless` load runs `--jitless` without the power. Needs a Chromium build. |
| 2 | Low | user/lib/fork.c:78, user/posix/sys/mem.c:308, patch 0914:63 (an empty `--type` keeps it) | The browser holds the power it hands on and can use it: `MAP_JIT` works there. V8 runs `--jitless` in the browser, so nothing in it makes code today, but W^X no longer guards the most powerful process: one that is compromised has W+X pages for the asking. Section 12 says so ("keeps the power to hand on but never runs compiled code"); this is the cost of that choice. No user-space change can split "may hand on" from "may use": a duplicate can only narrow, and the browser picks which child gets it, so it can always pick itself. | Code running in the browser calls `mmap(..., PROT_READ \| PROT_WRITE \| PROT_EXEC, MAP_PRIVATE \| MAP_ANONYMOUS \| MAP_JIT, -1, 0)`: it succeeds. | Open, for CR-f: a launcher outside the browser (section 13's per-type table at spawn) gives `SR_JIT` to renderers only and the browser gives the power up at its start; or Chromium's zygote holds it (forked before any input, the browser then giving it up). Design question 1. |
| 3 | Low | user/services/init/exec.c:602, user/posix/sys/prctl.c:21 | `PR_SET_MDWE` isn't inherited across `/svc/exec`: a program on a file system started through it gets its own approval whatever its caller holds (CE1, the owner's rule), so one whose list says `right jit` gets the power from init although its caller gave the power up. Linux's MDWE is inherited across every `execve`. prctl.c and section 12 promised Linux's rule. Not reachable from Chromium: content_shell's list has no `svc exec` (a child exec of a `/data` path is `EACCES`), and its children are `/proc/self/exe` execs. | A program whose list says `svc exec`, after `PR_SET_MDWE`, `posix_spawn`s an approved `/data` program whose list says `right jit`: the child holds `SR_JIT`. | Documented (prctl.c, section 12): the exception named. Making it Linux's would need the kernel to know the process gave the power up (a process flag `vmo_make_jit` refuses and `/svc/exec` reads from the caller's pid): design question 2. |
| 4 | Low | user/lib/start.c:114 (`startup_handle_drop`) | `PR_SET_MDWE` closed only the first `SR_JIT` handle: a process started with two (a native starter's choice; a warm spare's promotion appends to the table too) kept the power with the second while `prctl` returned 0 (Chromium's `PCHECK` passes). And the drop closed the table up by moving the later entries down while another thread (libc's signal thread) could be reading it: a lookup of another role could find its neighbour's handle in the moment between the two stores. | utest spawns bin/jittest with two `SR_JIT` handles and `jittest giveup`: after `PR_SET_MDWE`, `MAP_JIT` still works and `PR_GET_MDWE` is 0. | Fixed (bdad478d): every entry with the role is emptied where it is (its role `SR_NONE` first, then its handle closed); utest `jit_mdwe` (two handles: `jittest power` 0, `jittest giveup` 1; without the fix, the PC, batch 19222522: `jit_mdwe: FAILED ... jittest_two_powers(jit, "giveup") == 1: 0 vs 1`). |
| 5 | Low | kernel/mm/aspace.c:1618, 1754, 2021; user/services/shell/cmd/ps.c:66 | `[jit]` is "has mapped JIT memory to run since this image started", not "may write and run its own code": it stays after the JIT memory is unmapped; a program spawned from a JIT VMO (CJR finding 1) is marked though it can't write its code; a holder that hasn't used the power (the browser, a SwiftShader GPU process before its first shader) isn't. And `ps` can't name process types, so cr-test counts marks (one more with SwiftShader) rather than checking which process holds them. | `ps` after a renderer's JIT memory is gone; `ps` in the webgl case. | Left (CJR's choice of a mark for use, section 12); the footnote's present tense is loose. gin's new line (finding 1) now names the holders by type in the jit case. |

**Notes** (no change):

- **6.** A `MAP_FIXED` of private anonymous memory over JIT memory keeps it
  JIT memory (user/posix/sys/mem.c:763), which is what Linux's decommit by
  remap needs; but a program that reuses a former code range for data and
  then forks loses that data in the child (mem_fork drops `MT_JIT`
  entries, mem.c:249, and the kernel leaves a writable JIT mapping out of
  a clone), where Linux copies it. No Jam OS program does; Chromium's
  renderers don't fork. The `-EACCES` fallback to replace_locked there
  can't happen for libc's own JIT memory (always mapped through a handle
  that writes and executes).
- **7.** The decommit itself is sound: the protect comes first and is the
  only step that can fail; `vmo_decommit` of a JIT VMO can't fail (no
  pins since CJF, no children), and it is the kernel's ordinary decommit
  (each mapping's entries gone, the TLB shot down, before a page is
  freed); the range keeps its one VMO, so nothing leaks and no second
  mapping (an alias) exists in the process; another thread running the
  code faults once the protect drops execute, as on Linux, or runs zeroes
  after the decommit if the new protection keeps it.
- **8.** `exec_replace` (user/posix/sys/spawn.c:592) passes a duplicate of
  `SR_JIT` only to `/proc/self/exe`, through the self-image (the same
  sealed or boot-image pages) or, without one, a boot-image program by our
  own name (a file program goes to `/svc/exec` instead); the duplicate is
  in the startup message, which dies with the channel if the replace
  fails, so no copy is left in the table to take back after `PR_SET_MDWE`.
  A forked child's duplicate is the fork message's, closed if it is never
  written. `PR_SET_MDWE` and a fork or an exec in another thread at the
  same moment are not ordered (the reads of `SR_JIT` there take no lock):
  the call is for a process's start (Chromium's, one thread).
- **9.** `SR_JIT`'s handle, a root narrowed to `RIGHT_ROOT_JIT`,
  `RIGHT_DUPLICATE` and `RIGHT_TRANSFER`, is the first root that can be
  sent; every other call on a root (sysinfo_check_root's, the hardware
  calls' `RIGHT_SLICE`, `RIGHT_INSPECT`, `RIGHT_WRITE`, `RIGHT_MAP`,
  `RIGHT_MANAGE`) needs a right it lacks, so it carries the JIT power and
  nothing else.

## SwiftShader in the GPU process (SWS's question 1)

The GPU process keeps the power exactly when its own command line says
`--type=gpu-process` and `--use-angle` is `swiftshader` or
`swiftshader-webgl`, compared as strings in content's child start (patch
0914:59, before anything is read). Only the browser writes that command
line: the switches of its own it copies to the GPU process and the ones
its GPU mode adds (Chromium's `GpuDataManagerImplPrivate::
AppendGpuCommandLine`, `--use-angle=swiftshader-webgl` in its SwiftShader
mode); a renderer or a page writes no part of it.

- **A page.** It can't add a flag; it can only make the GPU process
  crash, and the browser starts it again in the next mode of its fallback
  list. SwiftShader is in that list only when the browser allows it
  (Chromium's `--enable-unsafe-swiftshader`, or a feature the owner turns
  on with `--enable-features`), so a crash moves the GPU process to
  SwiftShader only where the owner has opted in already. Checked on Jam OS
  by cr-test's webgl case: `--disable-gpu` alone gives no WebGL and no
  extra `[jit]`, and (new, 8783eee9) neither do no GPU flags at all, the
  hardware's GL then failing to start and the browser falling back. Not
  read in Chromium 155's source (this track may not touch `~/cr`): the
  main session can confirm `IsSwiftShaderAllowed`'s rule and its feature's
  default there with one grep.
- **A restart after a crash** is a fresh fork and exec of `/proc/self/exe`
  from the browser, which still holds the power: content's child start
  decides again from the new command line, so a GPU process started again
  in another mode gives it up.
- **Every other case gives it up**: `--disable-gpu` alone (its display
  compositor mode adds no `--use-angle`), any other `--use-angle` value
  (with SwiftShader ANGLE's only Vulkan, `--use-angle=vulkan` would run
  SwiftShader without the power: Reactor's `MAP_JIT` is `EPERM` and it
  fails closed). One owner flag keeps it without
  `--enable-unsafe-swiftshader`: `--use-angle=swiftshader` itself, which
  the browser passes on and which asks for SwiftShader by name. And an
  in-process GPU (`--in-process-gpu`, `--single-process`) runs SwiftShader
  in the browser, which holds the power anyway (finding 2).
- **The main session's call** (SwiftShader opt-in only until CR-f, never
  a default for `chrome` in CR-d): agreed; nothing found that turns it on
  without the owner's flag. If `--enable-unsafe-swiftshader` alone is to
  be the opt-in, 0914 would also need that switch on the GPU process's
  command line; whether the browser passes it there is a question for the
  source.

## Checked, no finding

- **posix_spawn**: no `SR_JIT` (spawn.c's start passes none); a
  `/proc/self/exe` child of `posix_spawn` holds none (jittest).
- **A fork, then an exec of another program**: in place, `p->self` is
  false and no duplicate goes; a file program goes to `/svc/exec` (finding
  3). The forked child holds the power between its fork and its exec, as
  its parent does.
- **An exec of `/proc/self/exe` with other arguments**: keeps it, and
  content's child start gives it up for every process type but the
  renderer and the SwiftShader GPU process (crashpad's handler, a zygote,
  a utility of any sub-type, an unknown type: all of them pass through
  `ContentMainRunnerImpl::Initialize`, content_shell's `main` being
  `ContentMain`); a process that gave it up has nothing to pass on.
- **A child that never calls `prctl`**: a forked child before its exec
  (holds it, as its parent), a death-test child of base_unittests (a test
  program whose list says `right jit`); headless_shell's list has no
  `right jit`, so none of its processes holds it.
- **Approval**: the four starters give `SR_JIT` from the list alone, as
  CJR checked; init's test run narrows its duplicate to `DP_JIT_RIGHTS`
  itself, the others in the send; every error path closes it.
- **0913 and 0915**: `MAP_JIT` for memory PartitionAlloc is asked for
  executable, and for Reactor's code buffers (`need_exec`), in a process
  without the power is `EPERM` where the old path failed at `mprotect`:
  the same refusal, sooner.
- **The kernel's W^X** (unchanged since CJR): `perms_ok` and
  `protect_span` allow write with execute only over a JIT VMO and within
  the handle's rights; libc's `MAP_JIT`, `PR_SET_MDWE` and decommit are
  courtesy above it, and a process without the power gets `EACCES` or
  `EPERM` from libc and the kernel's refusal underneath.

## Design questions

1. **The browser's power (finding 2).** (a) Keep it: the browser picks
   the renderers, as on every other platform (Linux's and macOS's
   browsers may JIT too). (b) CR-f's launcher holds `SR_JIT` and gives it
   to renderers by the per-type table, the browser giving it up at its
   start. (c) Chromium's zygote: forked before the browser reads
   anything, holding the power, the browser giving it up after. **Recommended:
   (b) with CR-f** (the table is planned there anyway, and it takes the
   power out of the browser without a zygote); (a) until then.
2. **`PR_SET_MDWE` across `/svc/exec` (finding 3).** (a) Leave it named
   as the one exception (only programs with `svc exec` can meet it, and
   Chromium's can't). (b) A process flag in the kernel, set by a new call
   (`PR_SET_MDWE` makes it), never cleared, copied to a process its
   holder creates, refused by `vmo_make_jit`, and read by `/svc/exec` from
   the caller's pid (CBF2a's writer pid) to withhold `SR_JIT`. That is
   Linux's rule exactly, and also covers a JIT root a process receives
   later. **Recommended: (a) now, (b) with CR-f's sandbox**, when
   processes that give the power up may also reach `/svc/exec`.

## Tests (CJR2)

- utest `jit_mdwe` (finding 4), in every init run.
- cr-test jit's power checks (finding 1): a renderer compiling code and
  holding the power, every renderer of the `--jitless` load without it,
  no other process type holding it, the browser never compiling code.
  They need patch 0914 as changed here built into `out/jamos`; on the
  build before it they fail for want of gin's lines.
- cr-test webgl's third load (SwiftShader's question): no GPU flags, no
  WebGL, as many `[jit]` children as with `--disable-gpu` alone.

**On the branch's head** (main d3ee42b9 merged; the PC): batch 19220853,
the init run (`jit_mdwe` in it), ktest at 4 CPUs, `basic.txt`,
`cmds.txt`, `files.txt`, `jit.txt`, kdump `save`, cr-test `multi` and
`webgl` (its third load: no WebGL, 2 children marked as with
`--disable-gpu` alone, 3 with SwiftShader): PASS; `commands.txt` failed
on utest `extfs_watch_unread`'s message-bytes check (known under load,
track SQ's) and passed on its rerun (19221757); cr-test `jit` passed every
check but the two new ones, which found no gin line in a Chromium built
before this patch, as they should. `make check` on the PC: PASS. No
kernel change.

## Fixes (CJR2)

- Finding 1 (cb370c82): patch 0914's child start and gin's line;
  tools/cr/jit.txt and jit-check.py.
- Finding 4 (bdad478d): libos's `startup_handle_drop` (user/lib/start.c;
  every program linked against libos takes it at its next link, Chromium's
  at its next relink).
- Finding 3: prctl.c's comment and section 12 name the exception.
