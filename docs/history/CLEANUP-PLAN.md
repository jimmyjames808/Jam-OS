# Cleanup (M7.5): readability and structure, no behaviour change

**Status: DONE in QEMU (2026-09-30).** Tracks A-E, a round of bug fixes and
the after-the-merge step are merged; the PC round (All tests, 2-minute
stress) and the independent review of the after-the-merge step are next.
Results are at the end of this file. The track descriptions below are the
plan as written; paths in them are the old ones.

Two read-only reviews (architecture; readability) found the core design
sound and the overgrowth mostly milestone scaffolding. This milestone does
their groups 1 (delete what's dead) and 2 (reorganise). Group 3 (a system
file namespace `fs.idl`, bulk data in IDL, shrinking `debug_command`) is
design work and starts M8's plan instead.

**The rule for every track: behaviour does not change.** Same commands,
same output, same boot menu, same ktest names (`ktest starvation` must
still work), same syscall numbers, same IDL. The only removals allowed are
the ones listed. If a track finds a real bug, it notes it in its report
and does not fix it here.

## Tracks (parallel, one agent each, each in its own worktree)

Each track owns the files listed. Touch nothing else unless the build
forces it (then keep the edit minimal and say so in the report). The
Makefile is shared: keep Makefile edits small and inside your own section.

### A. Delete the kernel build of drivers and dead scaffolding
Owns: `kernel/drivers/` (delete), `kernel/core/m6_weak.c` (delete),
`drivers/xhci-noop/` (delete), `user/init/xhcitest.c` (delete), the kernel
tests that exist only for the kernel build (test_driver.c, test_edu.c,
test_hid.c, test_xhci.c, and the kernel-build parts of test_drvrights.c:
keep every test that exercises process drivers or rights), `tools/gensyscalls.py`
(stop generating `kernel/abi/syscall_weak.c`: a missing sysc_* becomes a
link error), `drivers/include/jam/driver.h` + `user/lib/driver_user.c`,
the Makefile's driver section, `boot/limine.conf`, `tools/checkdriver*`.
- Remove boot words and init modes that only served the deleted code:
  `drivers=kernel`, `xhcitest`, and the `demo:<framebuffer>` boot path
  (kernel/core/main.c's demo block + fbcon_mute use for it, init's
  `run_demo`). The shell's `demo` command stays (Track E keeps bin/demo
  working when started from the shell). Edit kernel/core/main.c and
  user/init/main.c only for these removals.
- driver.h: one implementation (process build). Rewrite its header
  comment accordingly. Add `drv_snprintf` / `drv_vsnprintf` (libos
  vsnprintf underneath) and let checkdriver.py allow them.
- Move the test drivers (null, drvtest, crasher, edu) to `drivers/test/`;
  their bootfs names (drv/null ...) stay the same.
- Delete the dead code in user/shell/main.c? NO: that is Track B's.

### B. Shell: one command table, one file per command
Owns: `user/shell/` (all of it), `tools/shell-tests/`, and the Makefile's
user-program rule only as far as needed to compile subdirectories
(`user/<prog>/*/*.c`).
- Every command goes through the table in sh_exec.c: turn main.c's strcmp
  chain (demo, kill, mem, clear, panic, reboot, ...) into SH_CMD functions
  and delete run_command, split, sh_main_command, cmd_help, cmd_run and
  any other dead code. argv reaches every command intact (no join/re-split).
- **One file per command** in `user/shell/cmd/<name>.c` (the owner's
  choice). Code shared by several commands moves to named helper files
  (e.g. `sh_time.c` for the calendar/time-zone code now in cmds_info.c,
  one for sysinfo/cpu helpers, one for path helpers), each with a short
  header or a section in sh.h.
- Split sh_exec.c by job: output/capture (sh_io.c), variables and aliases
  (sh_vars.c), the command table and help (sh_table.c), parsing
  (sh_parse.c), execution (sh_exec.c), tab completion (sh_complete.c).
  main.c keeps only console I/O, the line editor and history.
- One printing API for commands (sh_say / sh_tty); remove the say/sh_say
  duplication.

### C. usb-bus: split enum.c, shorten the long functions
Owns: `drivers/usb-bus/`, `drivers/hid/`.
- Split `enum.c` (1834 lines) along its own section banners, e.g.
  devices.c, control.c, intr.c, config.c, report.c, attach.c, hub.c,
  rootport.c, with usbbus.h (or a small internal header) as the glue.
- `enumerate()` (211 lines, 12x attach_failed) becomes a short sequence of
  step functions. Also shorten `hub_port` and `dev_line`.
- Keep the private string builder (sb_*) for now, in report.c: Track A
  adds drv_snprintf and the swap happens after the merge.

### D. Kernel: split the big files, tests by subject
Owns: `kernel/` except what Track A owns (kernel/drivers/, m6_weak.c,
the tests A deletes, and main.c / dbgcmd.c / userboot.c, which D must
not edit or move).
- Split `sched.c` (1593): thread lifecycle to thread.c, wait queues and
  mutexes to wait.c (names are suggestions). Split `pci.c` (1039): MSI to
  pci_msi.c, names/logging/report to pci_report.c. Same directories for
  now (the directory moves happen after the merge).
- Kernel tests named by subject, not milestone: redistribute test_m4.c,
  test_m45.c, test_m55.c, test_m5perf.c, test_m6_review.c,
  test_m6p2_review.c into test_sched.c, test_timer.c, test_heap.c,
  test_serial.c, test_pcid.c, ... Test names (ktest names) do not change.
  Shared helpers (pin_self, free_now, fresh_job, rng, ...) go into one
  place (ktest.h / ktest_util.c) instead of copies.
- `NS_PER_US / NS_PER_MS / NS_PER_S` in jam/time.h; use them instead of
  per-file S/MS/US/SECOND in the files D owns.
- Rewrite milestone tags "(M5)", "(M7 Track C)" and audit ids "(C3)",
  "(O2)", "(R6)" in kernel/ comments into the reason itself; fix stale
  comments. Git keeps the history.

### E. User programs: apps library, console split, shared helpers
Owns: `user/` except user/shell (B), user/init/xhcitest.c and init's
run_demo (A); `user/lib/` except driver_user.c (A); the Makefile's
libfun/font bits.
- `libfun.a` built like libos.a; delete the one-line `#include
  "../fun/fun.c"` files. One `fun_check()` instead of three copies. Each
  app's selftest in its own selftest.c. Break up fractal `play()` (a
  handle_key), tetris `draw()`, life `play()`.
- Rebuild `user/demo` on user/fun (its own log2d/exp2d/sind, map, screen
  borrowing, arg parsing and worker pool go). It only has to work when
  the shell starts it (the argv the shell's `demo` command passes today),
  borrowing the screen through the console like the other apps.
- One font source for user space (not the console's `make font` copy
  plus includes of the kernel's .c file).
- Split `user/console/main.c` (1146): text model, terminal/CSI, framebuffer,
  keys/focus/input sources, clients.
- Shared `now()` and time constants in os.h / libos; delete the six
  copies (init/shell.c, devmgr/supervise.c, usbtest, utest main/hid/bench).
- Milestone tags in the files E owns, as in D.

## After the merge (one agent, sequential)
Directory moves that would collide if done in parallel:
`user/{services,apps,tests}/`, `kernel/core` into sched/, proc/, debug/;
the sb_* -> drv_snprintf swap in usb-bus and hid; a last sweep for
milestone tags; NEXT.md / ARCHITECTURE.md / README.md paths. Then the
independent review, fixes, a 2-minute stress (QEMU now, PC when the
owner is back).

## Every track, before reporting
- `make` (with -Werror) and `make KTESTS=0` build; `make check` passes.
- QEMU: all ktests pass at 4 and 8 CPUs (tools/qemu-test.sh), the init
  regression run (utest, usbtest) is clean, and the scripts that cover
  your area pass (tools/shell-tests/*.txt, fun-test.sh, usb-test.sh,
  usbkeys-test.sh, crash-test.sh). Timing flakes: rerun once.
- Follow CODING-GUIDE.md if it exists by then; otherwise match the house
  style of kernel/object/port.c, drivers/hid/keyboard.c, user/devmgr/ and
  user/shell/sh.h.
- Commit on your own branch (no Co-Authored-By trailer). Never push, never
  touch ~/jamos (main), never write to any USB disk.
- Report: what moved where, line counts before/after, anything you could
  not do, any bugs noticed (not fixed).

## Results

**Tracks.** A: the kernel build of drivers removed (kernel/drivers,
`drivers=kernel`, kdevmgr, the kernel-process machinery, xhci-noop and
`xhcitest`, the `demo:<framebuffer>` boot path and fbcon_mute, the weak
stubs m6_weak.c and syscall_weak.c: a missing `sysc_*` is a link error);
driver.h has one implementation plus `drv_snprintf`; test drivers in
drivers/test/. B: the shell has one command table (sh_table.c) and one
file per command (58 in cmd/), split by job into sh_parse / sh_vars /
sh_exec / sh_io / sh_complete / sh_vfs / ...; main.c 812 -> 312 lines,
sh_exec.c 1218 -> 225. C: usb-bus's enum.c (1834) split into attach,
config, control, devices, hub, intr, report, rootport and work;
`enumerate()` is a list of steps; log lines through `drv_snprintf`. D:
sched.c (1593) -> sched.c 937 + thread.c 327 + wait.c 365; pci.c (1039) ->
pci.c 636 + pci_msi.c 166 + pci_report.c 187; the milestone test files
(test_m4, test_m45, test_m55, test_m5perf, test_m6_review, test_m6p2_review,
test_repro, test_audit_obj) redistributed by subject, helpers in
ktest_util.c, NS_PER_* in jam/time.h. E: libfun.a, one user font object,
`now()` and NS_PER_* in libos, demo rebuilt on libfun, the console split by
job (main.c 1146 -> 137), fractal/life/tetris split into engine, drawing and
selftest files.

**After the merge.** kernel/core split: `kernel/main.c`; `kernel/sched/`
(sched, thread, wait); `kernel/proc/` (bootfs, elf, userboot);
`kernel/debug/` (klog, panic, ksyms, lockdep, report, dbgcmd, and
selftest/stress, which ship in every kernel, KTESTS=0 too, so they are not
in kernel/test/); reboot.c to `kernel/dev/`. User programs by role:
`user/services/`, `user/apps/` (with libfun in apps/fun), `user/tests/`;
the Makefile finds them by directory, a program's own directory is on its
quote include path (no `"../"` includes left); edu_check.h moved next to
utest, its only user. Milestone tags and audit ids swept from comments;
`-Wvla` everywhere and `-Wframe-larger-than=3072` in the kernel (largest
frame: sys_channel_call, 2464 bytes). The sb_* -> drv_snprintf swap had
already landed with Track C's fixes.

**Line counts** (hand-written .c/.h/.S under kernel/, drivers/, user/;
generated code excluded): 66,065 lines in 237 files before, 64,094 in 326
after. Files over 800 lines: 20 -> 13 (what is left: utest/main.c,
bench.c, vmo.c, test files, usb-bus hc.c/serve.c, aspace.c, sched.c).
ktests: 227 -> 219 (the tests of the kernel build went with it; new ones
for devmgr with a real driver process and the quarantine counters).

**Bugs fixed in the round after the tracks** (each its own commit): the
shell wrote one past its segment array for 32 segments ending in an
unclosed quote (6fe292b); usb-bus left a slot enabled when Enable Slot
handed out an out-of-range id (85c6abf), parsed a non-zero alternate
setting without its SuperSpeed Endpoint Companion (c17d078), and now
clamps a companion's bMaxBurst to 15 (350bd79); dma_cap's quarantine
counters could be read between a batch leaving one count and entering the
other (94e8dd7, an 8-CPU ktest failure); Tab completion ignored the
shell's screen-width line cap (350bd79).
