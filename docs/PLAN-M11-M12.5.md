# Plan: M11 to M13 in five waves

The order of work after M9.5 (the owner, 2026-10-02; G1 moved before
M12 and the code check added after it, 2026-10-05). Each milestone keeps
its row in [ROADMAP.md](ROADMAP.md#later) (goal and done-when) and gets its
own plan, `docs/M<n>-PLAN.md`, written with the owner before its agents
start. This file only says what runs beside what, and why. M10 and M10.5
come after M13 ([ROADMAP.md](ROADMAP.md#status)).

The milestones touch different parts of the system, so most of them can
run at once. What can't is anything that changes the interfaces M12
reviews while M12 reviews them, and anything built on what another one
replaces.

## Wave 1: M11, M11.5 and M11.6 together

| Milestone | What it touches | Meets the others at |
|---|---|---|
| **M11** IOMMU and interrupt remapping | the kernel's DMA (`dma_cap`, pinning), the VT-d tables from the PC's DMAR, interrupt remapping, devmgr's driver start | the syscall table and `abi.h` (new ordinals only) |
| **M11.5** performance pass | channel calls (reply-and-wait, direct hand-off, one copy), the scheduler's switch path (the context switch measured at 30 ns at M5.5 and 92 ns at M8.6: [BENCH.md](BENCH.md#m86-pc-2026-10-02)), the FPU on voluntary switches, the file and block calls' deadlines | the syscall table; channel semantics, which M11.6 builds on |
| **M11.6** services that outlive their process | services and their protocols: queues and state kept in VMOs and kernel objects across a restart, reconnecting, a warm spare; first fat and the mixer | M11.5's call path (below) |

- **M11.6 needs M11.5's call path.** It starts with its plan and the part
  of the work that doesn't depend on the call path (where a service's
  state lives, how a restarted service is handed it back, how clients
  reconnect), and moves onto M11.5's fast path once that is merged. Its
  plan says which parts wait. Of the three it needs the most design with
  the owner, so its plan comes first.
- **M11.5's final numbers are measured after M11 is merged** too: the
  IOMMU adds work to every DMA mapping, and the BENCH.md column should be
  the system as it will stay.
- **M11 is proven on the PC, not in QEMU.** QEMU's emulated IOMMU (intel-iommu)
  tests the code paths; only the board's own DMAR shows the real units,
  their quirks and that every device still works (USB, storage, audio,
  the network).
- Merge order: whichever finishes first, then the others rebased onto it;
  M11.6's call-path part last.
- Sign-off: one PC session for the wave (All tests + `soak 10`, the
  IOMMU's checks, `bench` and the Linux column), or one per milestone if
  a wave runs long.

## Wave 2: G1 alone

The owner's order (2026-10-05): G1 comes before M12, so that M12 reviews
the compositor's protocol with every other interface, instead of M14
reviewing it later. G1 is user space: the compositor, its client protocol
over channels (generated from Wayland's XML as the IDL is), `wl_shm` pools
as VMOs, input focus, the console as a client. It builds on wave 1's
channels and call path as they are; anything it needs from the kernel is
named in its plan and reviewed by M12. Its sign-off is on the screen: windows
from several programs on the PC, the shell in one of them.

## Wave 3: M12 alone

M12 reviews every system call and service protocol, G1's included, and
reshapes them while that is still cheap. It comes after waves 1 and 2, which
change them, and nothing else may change them while it runs. The review
itself runs as parallel tracks:

| Track | What |
|---|---|
| Kernel | the system calls: names, arguments, rights, errors, what M13's POSIX layer will need |
| Protocols | the IDL protocols (netdev, net, dns, audio, initctl, ...) and G1's compositor protocol, variable-length IDL arrays, the hand-written protocols still left (devmgr's) moved to IDL |
| Storage | devmgr's disks, filesystem services and mounts split into a service of their own |

Findings first, then fixes, as every review; a join; the PC sign-off.

## Wave 4: M12.1, the code check

Once M12 has reshaped the interfaces, fresh agents read the whole code base
as M8.6's check did, in five tracks (kernel core, the rest of the kernel,
drivers, services, libraries and tools): findings first, then fixes with a
test each, then All tests and `soak 10` on the PC. It is the clean base
POSIX is built on.

## Wave 5: M12.5 and M13 together

| Milestone | What it touches | Meets the other at |
|---|---|---|
| **M12.5** user-space pagers | the kernel's virtual memory (a VMO whose pages a process supplies), fat as a pager, programs from `/data` loaded on demand | nothing in user space but fat and the program loader |
| **M13** POSIX on musl | user space: musl, file descriptors over handles, `posix_spawn`, paths through the namespace, signals, `poll`/`select` on M9.5's wait sets, the terminal layer, then tinyssh as the first port | file-backed `mmap`, which waits for M12.5's pagers; M13's other stages don't |

Both build on M12's reviewed interfaces and add new ones (the pager calls,
POSIX's needs), which M14 reviews before it freezes anything
([ROADMAP.md](ROADMAP.md#later), M14's row). Separate sign-offs: M12.5's on
the stick (pulled under a mapping), M13's with tinyssh from the Mac.

## Working rules for every wave

- Each milestone's plan names the files each track owns; an edit outside
  them is small and named in the report.
- Agents run the quick tests (the build, `make check`, the init run at 2
  CPUs, their own tests); the heavy suites run once on the merged wave,
  and the PC signs it off.
- An independent review-and-fix agent after each milestone's merge, as
  always ([CODING-GUIDE.md](../CODING-GUIDE.md#8-contributing)).
