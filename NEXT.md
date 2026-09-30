# Next: handoff

The page for whoever picks up Jam OS next, human or agent. Read this, then
[docs/ROADMAP.md](docs/ROADMAP.md). Keep it short: history goes to
[docs/HISTORY.md](docs/HISTORY.md), status to the roadmap.

## Current state (2026-09-30)

- M0 to M7 are done and confirmed on the PC. The stick has 0.0.24-m7
  (ab4e106).
- The M7.5 cleanup is done in QEMU, with no behaviour change (the version
  string is still 0.0.24-m7): the five tracks, an independent review, two
  CODING-GUIDE.md style passes (kernel; drivers and user), a docs overhaul
  and two bug-fix rounds. 219 ktests at 4 and 8 CPUs, utest 30, usbtest
  12, every shell and area script, 60 s stress at 4 and 8 CPUs.
- Public on GitHub: https://github.com/jimmyjames808/Jam-OS, BSD-2-Clause.
  `origin` is set; `main` and `learn` are pushed.
- The owner reads and experiments in `~/jamos-learn` (a worktree of the
  `learn` branch; `git merge main` to catch up).

## Next step

1. The PC round on the cleanup build: flash, All tests, then the 2-minute
   stress. The scheduler's `volatile` -> atomics change and the usb-bus
   splits need real hardware (TCG has no PCIDs or TSC-deadline, and not
   the Intel xHCI's timing).
2. M8, storage: write the plan as docs/M8-PLAN.md first (like the
   [earlier plans](docs/history/)); notes for it are in the
   [roadmap](docs/ROADMAP.md#next-m8-storage). The review's design items
   go in: a system-wide file namespace (a new fs protocol, not the shell's mount
   table), bulk data in IDL (usb.idl has no bulk transfers), and
   debug_command's `kill <name>` moving to init. Then the audio track.
3. Smaller deferred items: [roadmap](docs/ROADMAP.md#smaller-follow-ups).

## Open questions for the owner

- On the last PC round the apps got real pixel graphics: how did they run?
  (the fractal's `b` benchmark, life's generations per second, how tetris
  feels)
- A `panic_reboot=<s>` boot option now (reboot N seconds after a panic),
  or wait for M8.5's crash kernel?
- When to start M8.
- At M9: is the switch port a trunk (Jam OS tags VLAN 21) or an access
  port on VLAN 21 (the switch tags)?

## Standing rules

- **Commits**: no `Co-Authored-By` trailer on this repository. The
  author is James Graham with the GitHub no-reply address, set in the
  repository's own git config (worktrees share it).
- **Agents** work in their own worktree and branch, and never push, never
  touch main or `~/jamos`, never write to a USB disk (no `make usb`, no
  `dd`). Big agent-written code is merged as planned, then reviewed by a
  separate agent. **Agents never spawn agents of their own**: say so in
  every prompt. Full rules: [CODING-GUIDE.md](CODING-GUIDE.md#8-agent-workflow).
- **Flash as soon as the stick is in**: when the owner says it's plugged
  in and a fix is ready, build and copy it at once
  ([HARDWARE.md](docs/HARDWARE.md#flash-and-boot-the-stick)); wait for the
  mount if `/Volumes/NO NAME` isn't there yet. No long QEMU checks first:
  the PC is the fast loop.
- **Stress tiers on the PC**: the 2-minute stress after each fix round;
  the 10-minute only as a milestone sign-off (`stress 600` from the shell
  counts; skip the 2-minute right before it). Details:
  [TESTING.md](docs/TESTING.md#the-tiers).
- **Working with the owner**: tell them exactly which boot entry to pick,
  and ask only for the lines you need from the RESULTS box. Photos arrive
  as HEIC in `~/Downloads`; convert with `sips -s format png`. Ignore the
  old blueprint artifact.
- Finished agent worktrees under `.claude/worktrees` are all merged; they
  are safe to leave.
