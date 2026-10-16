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
- Design question 2: left to CB5.
