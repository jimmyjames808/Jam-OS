# GPU plan: the Intel iGPU

Status (2026-10-20): a plan, nothing built. Written against main bcc76f8c
by track GP0 (docs only). **Order: after CR-f (the sandbox) and together
with "run well"** (the owner, 2026-10-20; both have plans of their own
being written, SANDBOX-PLAN.md and RUNWELL-PLAN.md). **Waiting for the
owner's answers to the questions below.** Only P0, the owner's own check
of the hardware, can happen earlier: the sooner it is done, the fewer of
this plan's unknowns are left when the tracks start.

Goal (roadmap [G3 and G4](ROADMAP.md#later), made concrete for
Chromium): **the PC's integrated GPU, the UHD 770 in the i7-14700, draws
the desktop tear-free on vblank and renders Chromium's pages (WebGL,
canvas, rasterisation and compositing) instead of the CPU.** G3's first
wording ("mode setting and vsync, only through the Intel iGPU; needs the
monitor on the board's output") is the main path again: the owner said on
2026-10-20 that he is not against moving the monitor to the board. Render
offload (the iGPU renders, the monitor stays on the RTX) is kept as the
fallback and compared below.

**How to read the facts.** Each fact that matters is marked:
**[checked]** read online or in this tree on 2026-10-20, with its source
in [Sources](#sources) (S1, S2, ...); **[measured]** counted from the
sources' own trees (method in [How big](#how-big-measured)); **[assumed]**
from memory of the code or by reasoning, with how it gets checked;
**[unknown]** the P0 check answers it.

The owner's decisions this plan builds on:

- **Licence** (2026-10-11): Jam OS stays BSD-2; GPL and LGPL only in
  separate programs (a driver, a service, a package: its own process, its
  source shipped beside it).
- **Linux code** (2026-10-20): rebuilding Linux source code for Jam OS is
  fine (a driver over a translation layer, Mesa, libdrm); running Linux
  binaries natively is not (no blob user-space driver, no layer that runs
  Linux ELF files as they are). GPU firmware, which the driver loads into
  the GPU and Jam OS never runs, is [Q5](#q5-gpu-firmware-blobs).
- **The monitor** (2026-10-20): may go on the board's output.
- **Order** (2026-10-20): the sandbox (CR-f) first.

**Done when:**

1. **Stage 1 (G3a, the display).** On the PC, monitor on the board's
   output, `iommu=on`: Jam OS's iGPU driver takes over the mode the
   firmware set, and the compositor draws by flipping between buffers in
   RAM on vblank: no tearing, frames paced to the monitor's refresh, the
   panic screen still shows, the driver killed comes back, and a kexec
   reboot still shows its boot log.
2. **Stage 2 (G4a, commands).** A test program with no Mesa runs batches
   on the render and copy engines in its own GPU address space; another
   client can't reach it; a hanging batch is found and the engine reset;
   the driver killed mid-batch comes back.
3. **Stage 3 (G4b, Vulkan).** Mesa's Vulkan driver for Intel (ANV) runs a
   triangle, a texture and a compute shader on the UHD 770 and the
   results match.
4. **Stage 4 (G4c, Chromium).** `chrome` on the PC renders through ANGLE
   on ANV: WebGL and scrolling at the monitor's rate, a full-screen canvas
   scanned out directly, a page's runaway shader costs only its own
   context, and with no iGPU everything falls back to today's software
   path.
5. As for every milestone: an independent review-and-fix before each PC
   check, then All tests and `soak 10` on the PC with the GPU bound.

## What this plan is not

- **Not the RTX.** The NVIDIA card needs NVIDIA's GSP firmware and either
  NVIDIA's closed user-space libraries (Linux binaries: out) or nouveau
  plus NVK, the biggest driver and the biggest blob on the machine for a
  card Chromium doesn't need. It stays a firmware framebuffer (render
  offload) or unused (the main path). Not planned.
- **No Linux binaries**, as the owner decided: every piece is source built
  for Jam OS.
- **No GL driver at first.** Chromium's GL ES is ANGLE's, on Vulkan; Mesa's
  GL driver (iris), EGL and GBM are not needed ([Mesa](#the-user-space-driver-mesa)).
- **No video decode or encode** before stage 5, which gets a plan of its
  own.
- **No display features beyond the firmware's mode in stage 1**: no other
  modes or refresh rates, no hotplug, no DP MST, PSR, HDCP, DSC, display
  audio or overlay planes. Each comes with real mode setting (stage 1b) or
  i915's display (option 3), if the owner wants them ([Q4](#q4-the-monitor-and-more-than-the-firmwares-mode)).
- **No GPU composition in the compositor.** The CPU still composites; the
  display engine scans out (stage 1) and client buffers go straight to a
  plane when they cover the screen (stage 4).
- **No dynamic linking** (M14.5): Mesa's Vulkan driver is linked into the
  program, as SwiftShader is (patch 0915).

## The hardware

| What | Fact |
|---|---|
| The iGPU | Intel UHD Graphics 770 in the i7-14700 (Raptor Lake-S Refresh). i915 treats Raptor Lake-S as a sub-platform of Alder Lake-S: the RPL-S PCI ids 0xA780-0xA783 and 0xA788-0xA78B map to `adl_s_info` **[checked: S3, S5]**. Which id this chip has **[unknown: P0]**: today the firmware hides the function ([HARDWARE](HARDWARE.md#the-machine)) |
| Graphics | Gen12 (Xe-LP): engines RCS0 (render), BCS0 (copy), VCS0 and VCS2 (video), VECS0; full 48-bit per-process GPU page tables (PPGTT); a shared last-level cache (LLC); 39-bit DMA addresses **[checked: S5]** |
| Display | display version 12 ("XE_D", as Tiger Lake and Rocket Lake), ports A and TC1-TC4 **[checked: S8]**; on Alder Lake-S these are all combo PHYs, no Type-C **[assumed: P0's VBT dump shows]** |
| The board's outputs | ASUS TUF GAMING B760-PLUS WIFI: one DisplayPort 1.4 and one HDMI 2.1, both rated 4K at 60 Hz **[checked: S27]**. Which i915 port each is wired to: the VBT (in the firmware's OpRegion) **[unknown: P0]** |
| Intel's documents | no Programmer's Reference Manual is published for Alder Lake or Raptor Lake; Tiger Lake's (same Gen12 graphics, same display version) has 18 volumes, among them 2a-2d the command and register reference, 8 command streams, 12 the display engine, 14 the workarounds **[checked: S19]**. i915's own source (MIT) is the other document |
| The monitor | 2560x1440 (the mode the RTX's firmware sets today). Model, top refresh rate and inputs **[unknown: Q4]** |
| The firmware's mode | with the iGPU as primary display the firmware lights the board's output itself. Microsoft's UEFI requirements say a desktop's firmware must try the display's native resolution and timing from its EDID **[checked: S26]**, which today's 2560x1440 agrees with; the refresh rate it picks **[unknown: P0 reads the transcoder's timings]** |
| The IOMMU | today one remapping unit with INCLUDE_PCI_ALL and no RMRR ([HARDWARE](HARDWARE.md#the-iommu-vt-d)). With the iGPU on, Intel client boards usually add a unit of its own for graphics and often an RMRR for its stolen memory **[assumed: P0 reads the DMAR table]** |
| Stolen memory | the firmware reserves RAM for the iGPU ("DVMT pre-allocated", its base in PCI config register BDSM, its size in GGC); the firmware's framebuffer lives there **[assumed: P0]** |
| QEMU | no Intel GPU model; passing the real iGPU to a guest needs a Linux host with VFIO, and the PC runs Windows with WSL2 **[assumed]**. So every stage's real test is on the PC ([the test loop](#the-test-loop), Q6) |

## The two paths: monitor on the iGPU, or render offload

| | **Monitor on the iGPU** (main path) | **Render offload** (monitor stays on the RTX) |
|---|---|---|
| Display work in the driver | stage 1: take over the firmware's pipe and plane (no mode setting) | none |
| First visible result | stage 1, with no command submission, no Mesa and no firmware: a tear-free desktop | only at stage 4, when Chromium renders |
| How a frame reaches the screen | the compositor draws into buffers in RAM that the display engine scans out, flipped on vblank; in stage 4 a full-screen client buffer is scanned out directly | the compositor composites every changed area into the RTX's firmware framebuffer over PCIe, as today |
| vsync, tearing | vsync; no tearing | no vsync; tears as today |
| Modes and refresh rates | the firmware's (native resolution; rate as the firmware chose); more needs real mode setting (stage 1b or option 3) | the RTX's firmware mode, never more |
| GPU buffers to the screen | linear ones composited by the CPU; tiled and compressed ones only on a plane | linear only (the CPU must read them): the GPU writes uncompressed, linear buffers, which costs it bandwidth |
| CPU cost per frame | the damage, plus flushing the written lines from the caches | the damage written over PCIe to the RTX (fbbench measured 10.8 GB/s for that framebuffer: `user/services/compositor/paint.h`) |
| Windows and games | the cable choice of [Q1](#q1-the-cables-and-the-monitors-inputs) | unchanged |
| New risks | the takeover, the IOMMU and stolen memory at boot, the panic screen on the iGPU | none on the display side |
| Stage 1 | **easier than offload's first step**: no engine, no context, no workaround list; a few display registers and the vblank interrupt | offload's first step is stage 2 (a working engine) with nothing on screen to show for it |

The firmware framebuffer keeps working on either path: it is what the
kernel and the compositor use until the driver binds, in QEMU, on a PC
without the iGPU, and after the driver dies.

## What Chromium needs, end to end

### 1. The kernel side

- **The device.** devmgr binds the iGPU (00:02.0, vendor 8086, class 03)
  to the new driver by its PCI id list, as it binds the others
  ([ARCHITECTURE: Drivers and services](../ARCHITECTURE.md#drivers-and-services)).
  The driver gets BAR0 (16 MiB: the registers in its lower half, the
  global GTT's entries in its upper half **[assumed: the PRM's Vol 2c]**),
  never BAR2 (the aperture, through which the firmware framebuffer is
  reached), and one MSI vector.
- **PCI configuration words.** The driver needs three read-only words a
  driver can't read today (no raw ECAM access): GGC (the GTT and stolen
  sizes), BDSM (stolen memory's base) and ASLS (the OpRegion's address).
  devmgr reads them for it (an allow-list of offsets per driver).
- **The OpRegion.** 8 KiB of firmware memory holding the VBT (the board's
  ports, their AUX and DDC pins). Stage 1 doesn't need it (it reads the
  live state from the registers); real mode setting does. The kernel would
  give a read-only VMO of it after checking the address lies in firmware
  memory and the signature.
- **Memory and the GTT.** The GPU reaches memory through two kinds of page
  table: the **global GTT** (one, written by the driver alone through BAR0:
  scanout buffers, rings, context images) and **per-process PPGTTs** (one
  per client, four levels, in RAM). Both hold device addresses. Every page
  they point at is a VMO page pinned through the driver's `dma_cap`, so it
  is in the iGPU's IOMMU domain; with the IOMMU, Jam OS's device addresses
  are the physical ones ([ARCHITECTURE: The IOMMU](../ARCHITECTURE.md#the-iommu)),
  so the driver's numbers don't change. Buffers are the clients' own VMOs
  (charged to their jobs); the driver pins one when it is bound into a
  GPU address space and unpins it at unbind. A GPU makes and drops buffers
  all the time (Mesa caches them, so not per frame): pins are no longer
  "made at start, dropped at stop", and an unpin costs an IOTLB
  invalidation (microseconds).
- **The IOMMU's limits for a GPU.** A domain holds at most 2048 table
  pages (8 MiB: up to 4 GiB of pins dense in 2 MiB blocks). Enough for
  Chromium, but a GPU's buffers are scattered: the iGPU's cap may need a
  higher limit (a small kernel change, decided when stage 2 measures).
  i915 aligns scanout buffers to 256 KiB and keeps 64 valid entries after
  each when VT-d is on, because the display engine reads past a surface's
  end **[checked: S10]**: the driver points every unused global-GTT entry
  at a scratch page, as i915 does.
- **Stolen memory and the boot.** When the IOMMU turns on at boot, a
  function nobody drives is sent to the blocking domain unless an RMRR
  names its region. If the board gives no RMRR for the iGPU's stolen
  memory, the display engine's reads of the firmware framebuffer are
  blocked from that moment: a black screen on every `iommu=on` boot once
  the iGPU is primary **[unknown: P0 boots both ways]**. The fix if so:
  the kernel treats the stolen region (from BDSM and GGC) as the iGPU's
  boot region, as it would an RMRR, in its home domain and in every
  domain its driver gets.
- **Bus mastering.** Making a `dma_cap` turns Bus Master Enable off until
  the driver turns it on, and kexec turns it off on every function. On an
  iGPU that also stops the display engine's reads **[assumed]**: a blink
  when the driver binds (it turns bus mastering on as soon as it has seen
  the engines idle; the firmware leaves none running) and a black screen
  after a kexec until the driver binds again, unless the kernel leaves the
  iGPU's bus mastering on across a kexec while it scans out the firmware
  framebuffer. Decided in GPD3 after P1 shows what the hardware does.
- **Interrupts.** One MSI. Gen11+ has a master interrupt register and,
  under it, the display's per-pipe registers: `GEN8_DE_PIPE_IIR(pipe)` at
  0x44408 + 0x10 x pipe, bit 0 vblank, bit 3 plane 1's flip done, the
  display's master enable in `GEN11_DISPLAY_INT_CTL` (0x44200) **[checked:
  S7]**; the GT's (engine completion, context switches) in the GT's own
  registers.
- **The panic screen and kexec.** Today the kernel draws its panic log on
  the firmware framebuffer, which the RTX scans out by itself. On the
  iGPU, once the driver flips to its own buffers, the firmware framebuffer
  is no longer on screen. The driver registers with the kernel a short
  list of register writes (the plane's surface address, stride and
  control, back to the firmware framebuffer's values) that the panic path
  makes before drawing, with the iGPU's bus mastering left on and the
  stolen region mapped. The driver never rewrites the global-GTT entries
  of the firmware framebuffer, so the kernel's mapping of it stays right.
  At an orderly stop (update, reboot) the driver flips back to it.
- **Security.** Gen12 enforces the separation of client batches in
  hardware: a batch in a PPGTT runs unprivileged **[assumed: the PRM's
  Vol 8]**, and i915 runs no software command parser on Gen12 (only on
  Gen7, and Gen9's copy engine) **[checked: S9]**. A client reaches only what its PPGTT maps; the
  global GTT is never a client's.

### 2. The GPU driver (one program: drv/igpu, new)

One driver program for the whole iGPU, supervised and restarted by
devmgr like the others, its hardware handles non-transferable. Its parts,
whichever option builds them ([below](#the-kernel-driver-the-options)):

- **Display** (stage 1): the takeover, flips on vblank, the panic list;
  later (stage 1b or option 3) mode setting, hotplug and more planes.
- **GT** (stage 2): forcewake, the engines' rings, submission (execlists:
  no firmware), context images, per-client PPGTTs, interrupts, hang
  detection and engine reset, the GT's frequency, Intel's workaround list
  for this chip.
- **The client interface** (stage 2): what Mesa and Chromium's GBM call
  ([below](#the-client-interface-i915s-uapi-or-a-mesa-back-end)).

A driver restart loses every client's GPU contexts and mappings but not
their buffers (they are the clients' VMOs): clients see "device lost"
and set up again (Chromium restarts its GPU process; the compositor falls
back to the firmware framebuffer until the driver is back).

### 3. The user-space driver

Mesa's ANV (Vulkan), built for Jam OS, linked into the programs that use
it ([Mesa](#the-user-space-driver-mesa)).

### 4. ANGLE and Chromium

- **What Chromium's Wayland back end does with a GPU** **[checked: S25]**:
  for a hardware GL it uses `GLSurfaceWayland` (EGL window surfaces) or
  `GbmSurfacelessWayland` (only built with Wayland GBM, and only when a
  GBM device opens); the shared-memory read-back surface is for software
  GL only; and Chromium's own Vulkan (Skia on Vulkan) is refused on
  Wayland except for SwiftShader ("'--ozone-platform=wayland' is not
  compatible with Vulkan"). So the path is **GL ES through ANGLE, whose
  back end is Vulkan on ANV**, with buffers from GBM sent to the
  compositor as dmabufs: `use_wayland_gbm=true`.
- **GBM**: Chromium's bundled minigbm (`use_system_minigbm=false` is
  already in args.gn). minigbm has i915 and xe back ends **[checked: S24]**;
  that Chromium 155's bundled copy has them **[assumed: GPC1 checks
  ~/cr]**.
- **Two Vulkan drivers in one program.** Patch 0915 makes SwiftShader the
  program's Vulkan (ANGLE calls `vk*` directly). With ANV beside it, both
  would define `vk*`. A small dispatch of ours (each driver's
  `vk_icdGetInstanceProcAddr`, SwiftShader's names renamed) picks ANV when
  the GPU answers, SwiftShader otherwise. A change to 0915's group.
- **GPU info and the blocklist.** args.gn has `use_libpci=false`, so
  Chromium's Linux collector may see no device **[assumed: GPC1 checks]**;
  ANGLE can name the device from Vulkan. The blocklist must not refuse it
  (`--ignore-gpu-blocklist` while testing).
- **No JIT for the GPU process.** ANV compiles shaders to GPU code in
  ordinary memory; the GPU process gives up the JIT power (patch 0914's
  rule: only SwiftShader keeps it). A step forward from CR-e's
  SwiftShader, whose shaders are CPU code.
- WebGPU (Dawn on Vulkan) on ANV is a later step **[assumed possible]**.

### 5. The compositor: flips, direct scanout and dmabufs

- **Stage 1: an output that flips.** Beside today's firmware-framebuffer
  output, a second one used when the driver's display service is there:
  three buffers (VMOs, linear XRGB8888 at the firmware mode's size, a
  stride the plane accepts), made by the compositor and handed to the
  driver, which pins them, puts them in the global GTT and flips by
  writing the plane's surface register, latched at the next vblank
  **[assumed: the plane registers are double-buffered; `PLANE_SURF` for
  pipe A's plane 1 is at 0x7019C, `PLANE_STRIDE` 0x70188, `PLANE_CTL`
  0x70180: checked S6]**. The driver reports each vblank and each finished
  flip as an event with the frame count (`PIPE_FRMCOUNT`, 0x70040 for pipe
  A: checked S7) and a time. The compositor paints only damage into the
  back buffer (each buffer's age says what it missed), flushes the lines
  it wrote from the CPU's caches (`clflushopt`, a user-mode instruction:
  the display engine doesn't snoop the caches **[assumed: i915 maps
  scanout buffers uncached or write-through]**), flips, and sends frame
  callbacks at vblank. If flushing costs too much (a BENCH line decides),
  the kernel gets a write-combining mode for VMOs (the framebuffer already
  uses PAT's write-combining entry). Today's painting was tuned for a
  write-only framebuffer; RAM buffers may also be read, which only helps.
- **Without the driver** (QEMU, the RTX, the driver dead) the
  firmware-framebuffer output, as today; the switch both ways is the
  compositor's.
- **Stage 4: dmabufs.** `zwp_linux_dmabuf_v1` with Jam OS's handles: a
  dmabuf is a VMO (libc's descriptor for it), an offset, a stride, a
  format and a modifier (linear, X-tiled, Y-tiled, Y-tiled with
  compression **[assumed list: the plane's capabilities]**). Linear
  buffers are composited by the CPU like `wl_shm` pools (mapped
  read-only); a tiled buffer only goes on a plane. **Direct scanout:** when
  one surface covers the whole output (a full-screen video, game or
  canvas), its buffer goes on the primary plane as it is: no composition,
  no copy. Otherwise the compositor asks clients for linear buffers
  (dmabuf feedback). A buffer is read only when the GPU has finished
  writing it: the driver gives the compositor an event per committed
  buffer (the buffer's last write done), so the compositor still never
  waits on anyone.
- **What direct scanout costs the compositor**: the surface's buffer must
  match the plane (format, modifier, stride, the full output), stay alive
  until the next flip, and nothing of the compositor's may be drawn over
  it while it is scanned out. More planes at once (overlays, the cursor
  plane) need the display's watermarks and buffer allocation reprogrammed
  (i915's `skl_watermark` is about 3,700 lines **[measured]**): stage 1b or
  option 3, not stage 4.

### 6. What the sandbox must allow the GPU process

For SANDBOX-PLAN's per-type table (CR-f), when the GPU lands:

- **One new grant: `svc gpu`**, a client channel to drv/igpu (Linux's
  "render node"): its own PPGTT, its own contexts. Only the GPU process
  gets it; renderers reach the GPU only through it, as on Linux; a test
  renderer that tries to open `svc gpu` fails.
- **No JIT power** with ANV (SwiftShader's fallback keeps CR-e's rule).
- **Memory**: buffers are the GPU process's own VMOs, charged to its job
  and its limits; the driver's page-table pages for a client are capped
  per client.
- **Hangs**: a page's shader that never ends is found by the driver's
  hang check, the engine reset, the guilty context banned after a few
  hangs (as i915 does); the GPU process sees its context lost, Chromium
  recreates it, the driver and other clients go on.
- **Disk**: Mesa's own shader cache off (Chromium keeps its own in the
  profile); nothing else on disk.

## The kernel driver: the options

### Option 1: our own display takeover (no mode setting)

Take over the pipe and plane the firmware set up: read the mode from the
transcoder's timing registers (`TRANS_HTOTAL` 0x60000, `TRANS_VTOTAL`
0x6000C, `PIPESRC` 0x6001C for A: checked S7) and the plane's state; point
the primary plane's surface at our buffers (`PLANE_SURF`, `PLANE_STRIDE`,
same format, linear); flip on vblank with the pipe's vblank and flip-done
interrupts. No mode set, no PLLs, no link training, no watermarks (the
plane's format and size don't change).

- **Size.** The register work is a few hundred lines, as the owner
  guessed. Around it, in Jam OS's style: binding, the global GTT with its
  scratch entries, the interrupt, the display protocol for the
  compositor, restart, the panic list, logging: about 1,500-2,000 lines in
  the driver, 800-1,200 in the compositor, 300-500 in the kernel and
  devmgr, 500 of tests **[estimate]**.
- **What it gives:** vsync, no tearing, direct scanout later, at the
  firmware's mode (usually native).
- **What it can't do:** change the mode or the refresh rate, follow a
  monitor plugged in later, retrain a DisplayPort link the monitor dropped
  (a DP monitor switched off and on may stay black until a reboot; HDMI at
  this mode needs no link training **[assumed]**: so HDMI, [Q1](#q1-the-cables-and-the-monitors-inputs)),
  the display's low-power states (they need DMC firmware), more planes.
- **References**: Haiku's intel_extreme (below), Fuchsia's intel-display,
  i915's plane code, Tiger Lake's Vol 12.

### Option 2: our own render driver for Gen12 only, from i915, native

A render-only driver for this one GPU generation, written on Jam OS's
driver library, with i915 (MIT) as the source of facts and tables and no
Linux layer: the render and copy engines; execlists submission (no
firmware); context images (made by running one "golden" context and
saving it, as i915 and Fuchsia do); the global GTT and per-client PPGTTs;
the GT's interrupts; forcewake (register access needs the GT awake); the
GT frequency (a fixed request at first; RC6 power-saving later);
engine reset on a hang; and **this chip's workaround list carried over**
from i915's tables (registers and values: MIT, kept with its notice).

- **Submission.** i915's default on Raptor Lake-S is GuC submission:
  Tiger Lake and Rocket Lake get no GuC, Alder Lake-S "HuC authentication
  only", Raptor Lake-S falls through to "HuC authentication and GuC
  submission" **[checked: S1]**. The execlists path is still in i915 for
  all three (the `enable_guc=0` parameter) and is what Alder Lake-S, the
  same silicon family, uses by default; but it is not what Intel's tests
  run on this chip since 2022. P0 boots Linux with `i915.enable_guc=0` once
  and runs a Vulkan program: if that works, execlists works on this chip.
- **Size.** Fuchsia's render driver for Intel (msd-intel-gen: Gen 9.5 and
  Gen 12, render and video engines, no GuC files) is about 10,800 lines of
  C++ **[measured: S18]**. Ours, Gen12 render and copy only: about 6,000-9,000
  lines, plus 3,000-4,000 for the client interface and 600 in libc
  **[estimate]**. Of i915's code that this chip still needs after whole
  files are deleted (about 91,000 lines outside display: below), that is
  about 12-15%; of all i915, about 3%.
- **The cost of owning it**: Intel's later fixes for Gen12 come only by
  hand. A small check against each new Linux LTS (i915's workaround tables
  for Alder Lake-S and Raptor Lake-S, diffed against ours) keeps it honest.
  And the bugs are ours to find on real hardware: a missed workaround shows
  as a rare hang or corruption. The oracle: Linux's own list of what it
  applied on this chip (debugfs, read in P0), compared register by
  register.

### Option 3: i915 itself, over a LinuxKPI-style layer

Linux's i915 (render and display) built as a Jam OS driver program over a
layer that gives it Linux's kernel interfaces on Jam OS's driver library,
as FreeBSD's LinuxKPI and drm-kmod do. Since 2026-10-20 the owner accepts
this kind of port.

#### How big (measured)

Counted from the projects' own trees (S13, S14, S15, S17, S18): bytes of
.c and .h files divided by 30, the bytes per line measured on three i915
files (29.6-30.4). Linux pinned at **6.18**, the longterm release the
linux-uapi port already builds from (`ports/linux-uapi/recipe`: 6.18.56),
so its tarball is already fetched.

| i915 at 6.18 | Lines |
|---|---|
| display/ | 193,700 |
| gt/ (with its selftest files) | 61,000 |
| gt/uc/ (GuC, HuC, GSC) | 23,100 |
| gem/ | 26,400 |
| gvt/ (virtual GPUs) | 22,800 |
| selftests/ | 11,300 |
| pxp/ (protected content) | 2,300 |
| soc/ | 1,000 |
| the top level (driver, irq, perf, registers, ...) | 48,800 |
| **i915 in all** | **390,400** |

What it also uses, measured at 6.12: Linux's DRM core
(drivers/gpu/drm's top level: 76,000 lines, of which i915 uses perhaps
half **[assumed]**), the DRM display helpers (17,400: DP and HDMI ones
needed), dma-buf (7,300: fences, reservations, sync files). xe, Intel's
newer driver, is 110,900 lines at 6.18 plus i915's display code, which it
compiles.

The layer, from the two BSDs that run i915:

| Layer | Lines |
|---|---|
| OpenBSD: drm_linux.c 2,800 + 279 linux/*.h headers 9,900 + sorts and radix tree 700 | **13,400** |
| FreeBSD's LinuxKPI (sys/compat/linuxkpi: 313 linux/*.h, headers 40,500, C 28,300) | **68,800** |
| FreeBSD drm-kmod's own extra layer | 3,100 |

OpenBSD's is small because its kernel already has what i915 leans on
(pages with a page structure, task queues, timers, interrupt context,
bus_dma, firmware loading). Jam OS's layer sits in a user-space driver
and has to build those on libos and the driver library: **about 20,000-35,000
lines [estimate]**, between the two.

#### How far it can be cut for one iGPU

Whole files that this chip (Raptor Lake-S, desktop, execlists) never
uses, by name, at 6.18 **[measured]**:

| Deletable | Lines |
|---|---|
| display: older platforms and outputs (i9xx, g4x, vlv/chv, ilk/pch, hsw, bxt/glk, LVDS, TV, SDVO, DVO, CRT, FDI, overlay, sprites) | 34,600 |
| display: laptop panels (PSR, ALPM, backlight, PPS, DRRS) | 12,100 |
| display: newer platforms (Type-C, C10/C20 and SNPS PHYs, DPT, pmdemand) | 10,800 |
| display: HDCP 5,100, DSI 4,100, MST/tunnels/DP tests 4,100, debugfs/CRC 3,300, FBC 2,300, DSC 1,900, audio 1,800 | 22,600 |
| selftests (all) | 32,700 |
| gvt | 22,800 |
| GuC (with execlists) | 16,800 |
| perf PMU, hwmon, debugfs, sysfs, traces, error capture | 8,900 |
| older GT (gen2-7, ring submission, command parser, render state) | 6,900 |
| OA/perf | 6,000 |
| discrete and newer (TTM, local memory, migrate, GSC, SR-IOV) | 5,400 |
| HuC and GSC loading 2,900, PXP 2,300, userptr 500 | 5,700 |
| **Deletable in all** | **about 185,000** |

**About 205,000 lines remain** (113,500 of display, 91,300 of GT, GEM and
the core). Cutting other platforms' branches inside the kept files (the
PLL manager, CDCLK, power wells, colour, the DDI tables all carry every
generation) might bring it to 130,000-150,000 **[estimate]**, but that is
hand surgery: a fork, never merged again. Pinning 6.18 and never merging
upstream is workable for one mature chip (Raptor Lake-S's Gen12 code is
years old in i915 **[assumed]**); the cost is that later fixes come by
hand, as for option 2.

**The layer shrinks much less than the code.** GT and GEM use memory
(pages, scatterlists, shmem-backed objects, io-mapping), locking (mutexes,
ww_mutex, RCU), work queues, timers, fences and interrupts as much as
display does. Dropping display drops i2c, backlight, ACPI video, the audio
component, fbdev and hotplug: perhaps a quarter of the layer **[estimate]**.
i915 has no switch to build without display; it can skip it at run time
**[assumed]**, so a render-only i915 is a fork as well.

#### Big but mechanical, or hard?

**Big and mostly mechanical, with two hard parts.** Mechanical: each Linux
interface i915 calls has a known shape on another kernel (OpenBSD's shim
is the map; FreeBSD's the fuller version), and Jam OS already runs drivers
in user space behind the IOMMU with device address = physical (NVMe, HDA,
xHCI, the network cards), so `dma_map_*` is `vmo_pin`, MSI is the kernel's,
firmware files come from `/boot`, and devmgr restarts a dead driver. The
hard parts:

1. **The memory model.** i915's buffers live on shmem pages with a page
   structure and scatterlists, and it maps them for clients through its
   own fault handler. On Jam OS a buffer can simply be a VMO: the layer's
   "shmem page" is a committed, pinned VMO page, and a client's mmap is the
   VMO itself mapped, so the fault path isn't needed on Gen12 with LLC
   (Mesa maps buffers write-back) **[assumed]**. M12.5's pagers could back
   faults if some path needs them. Writing that layer is design work, not
   copying.
2. **Execution context.** i915 assumes interrupt context, softirqs and
   tasklets, per-CPU data, preemption control and RCU. In a user-space
   process each becomes a thread or a lock, and ordering bugs show only on
   the hardware, without Linux's ftrace and debugfs.

What it buys: everything Intel ships for this chip at once: real mode
setting (other refresh rates, hotplug, DP link training, more planes),
GuC and HuC if wanted, power management, every workaround, hang recovery,
the video engines (stage 5). And its client interface is i915's own, so
Mesa and minigbm run on it unchanged.

#### The BSDs and Haiku

- **FreeBSD** (drm-kmod), **OpenBSD** (inteldrm) and **NetBSD** all run
  Linux's i915 over an adapter layer. FreeBSD's port moved to Linux 6.12
  LTS in 2026 and stays a trailing target **[checked: S16]**; its i915 copy
  has FreeBSD `#ifdef`s in 63 files and OpenBSD's (851 files, about
  391,000 lines) Linux-only blocks in 77 **[measured: S14, S15, GitHub code
  search]**: adaptation, not features of their own **[assumed from those
  counts]**. NetBSD's port **[assumed, not checked]**. Porting a BSD's copy
  would be Linux to BSD to Jam OS, a double translation for nothing.
  **As references** they are good: OpenBSD's shim (ISC licence
  **[checked: S15]**) is the shortest list of what i915 needs and how a
  non-Linux kernel answers each call; FreeBSD's LinuxKPI (BSD-2-Clause
  **[checked: S14]**) the fuller one. Both licences let us borrow code into
  a Jam OS program with their notices.
- **Haiku's intel_extreme** (its own display driver, mode setting only,
  no 3D): about 11,800 lines (accelerant 8,800, kernel driver 3,000)
  **[measured: S17]**. Its device list has Tiger Lake (mobile), Alder Lake-P
  and Alder Lake-N, **not Alder Lake-S or Raptor Lake-S**; Tiger Lake's
  PLL set-up came in 2024 **[checked: S17]**. A useful reference for option
  1 and stage 1b (a small native Intel display driver that works on Gen12
  laptops), not a source for this desktop chip.
- **Fuchsia**: the closest precedent for options 1 and 2. msd-intel-gen
  (render, above) and intel-display, a native mode-setting driver for
  Skylake, Kaby Lake and Tiger Lake (Tiger Lake's DDI PHY registers are in
  its tree), about 40,000 lines plus 22,000 of tests **[measured: S18]**: what
  a full native display driver costs for about two generations.

### Others considered

- **xe** (Intel's newer driver): on Alder Lake-S and Raptor Lake-S it
  needs `force_probe` in 6.18 (`require_force_probe = true` **[checked:
  S11]**): not Intel's supported driver for this chip. It submits only
  through the GuC (its execlists back end was "mostly for testing" and
  "not fully functional" when it was posted **[checked: S12]**), so it needs
  the firmware blob. Rejected for now; Mesa's xe paths stay available if
  that changes.
- **Porting a BSD's copy**: above.
- **The RTX**: above ([What this plan is not](#what-this-plan-is-not)).

### Comparison

| | Option 1: display takeover | Option 2: native render driver | Option 3: i915 over a layer |
|---|---|---|---|
| Gives | vsync, no tearing, scanout of our buffers at the firmware's mode | commands, contexts, isolation, hang recovery: what Mesa needs | all of 1 and 2, plus real mode setting, hotplug, planes, power management, video engines |
| New code of ours | ~3,000 lines (all of stage 1) | ~10,000-13,000 | the layer ~20,000-35,000 |
| Foreign code carried | none (facts from i915 and the PRM) | i915's tables (MIT) | ~205,000 lines of i915 kept (or a ~140,000-line fork), plus ~50,000 of DRM core, helpers and dma-buf |
| Firmware | none | none (execlists) | none needed with `enable_guc=0`; GuC by default on this chip |
| Agent-hours **[estimate]** | 5-7 | 16-24, plus hardware iteration | 45-75, plus hardware iteration |
| Risk | the takeover at boot (IOMMU, stolen memory) | missed workarounds, execlists off Intel's default path on this chip | the layer's memory and context model; bring-up blind |
| Upkeep | none (no upstream) | fixes by hand | pinned at 6.18; fixes by hand |
| Fits Jam OS's driver model | wholly | wholly | through a layer |

### Recommendation

**The main session's leaning, confirmed with three changes.**

1. **Option 1 first (stage 1)**: small, visible, and needs no engine, no
   Mesa and no firmware.
2. **Then option 2 for render (stage 2), speaking i915's interface to
   clients** (the part Mesa, libdrm and minigbm use on Gen12:
   [below](#the-client-interface-i915s-uapi-or-a-mesa-back-end)), so that
   option 3 can later replace it with nothing above it changing.
3. **Option 3 by need, not only by quirks**: if stage 2's first half
   (2a) has not shown a batch completing in a client's own PPGTT within
   about 12 agent-hours of hardware work, or Linux's applied workarounds
   disagree with ours in ways we can't explain, switch then, not after
   Mesa. And if the owner wants more than the firmware's mode (high
   refresh rates, hotplug, planes) or video decode, option 3 brings all
   of it at once, where the native road would rewrite i915 piece by
   piece.

So the order is stage 1 (option 1), stage 2 (option 2), stages 3 and 4
on top; option 3 replaces 1 and 2 under the same user space whenever one
of those triggers fires.

## The client interface: i915's uAPI, or a Mesa back end

| | **Answer i915's uAPI** (the Gen12 subset) | **A Jam OS back end in Mesa** |
|---|---|---|
| What changes upstream | nothing in Mesa or minigbm; libdrm gets a small Jam OS branch for finding the device (Linux reads sysfs) | a back end in Mesa's ANV (its kernel-driver back-end layer exists since Mesa 23.1 for i915 and xe **[checked: S23]**), one in minigbm, and libdrm's device finding |
| In the driver | about 30 calls: parameters and queries (engines, topology), buffer create, map offset, close, wait, busy, contexts and VMs, execbuffer (soft-pinned addresses, no relocations, fence arrays), reset stats, timestamp read; DRM's sync objects and dmabuf export/import; implicit fences on shared buffers | our own smaller calls (VMs, bind a VMO range, submit, events as fences) |
| In libc | a device file `/dev/dri/renderD128` whose ioctls are channel calls, and `mmap` of a buffer's offset maps its VMO | nothing |
| When Mesa updates | nothing to redo: Linux's uAPI never breaks | our back ends rebased each time: Mesa's internal back-end interface changes between releases |
| Switching to option 3 | invisible: i915 answers the same calls | Mesa goes back to its own i915 back end |
| Tested by others | the paths Intel's own CI runs on this chip | only by us |

**Recommendation: answer i915's uAPI.** It is more driver work (DRM's
sync objects and implicit fences included), but it is the stable
interface, it keeps Mesa and minigbm unchanged, and it is the hedge that
makes option 3 a drop-in. It is source-level compatibility, not a Linux
binary layer.

## The user-space driver: Mesa

- **ANV (Vulkan), not iris (GL).** ANGLE on Vulkan covers Chromium's GL
  ES. iris would add EGL, GBM, Mesa's GLSL compiler and the DRI loader
  (which loads drivers with `dlopen`: G2-PLAN's static-Mesa doubt). iris
  waits until a native GL program needs it.
- **The release**: Mesa 26.2.5, a 65 MB tarball **[checked: S21]**, built
  with meson against `ports/jamos.meson`, with `toolchain clang` (Intel's
  shader compiler is C++ **[assumed]**) and libc++ (`ports/llvm-runtimes`).
  Options: Vulkan driver `intel`, no Gallium drivers, no GLX, EGL or GBM,
  Wayland only, no XML config (no expat), no Valgrind or libunwind.
- **No LLVM at run time, but at build time.** Since Mesa 24.1 the Intel
  drivers need host tools (mesa_clc, the precompiler) built with LLVM,
  clang, libclc and SPIRV-LLVM-Translator; Mesa can take them from the
  build host (`-Dmesa-clc=system`, `-Dprecomp-compiler=system`), which is
  how Yocto cross-builds it **[checked: S22]**. On the PC's WSL: the
  distribution's packages, or built once by the recipe.
- **Static.** Mesa builds its Vulkan driver as a loadable module; the
  recipe patches it to a static archive **[assumed: one patch; if it fails,
  this waits for M14.5]**.
- **libdrm** 2.4.134 (427 KB **[checked: S21]**): the core only (no
  libdrm_intel, which ANV doesn't use), with the Jam OS branch for
  finding the device.
- **Licences**: Mesa is MIT for the most part, libdrm MIT **[assumed:
  the recipes list every licence file, as the ports do]**.

## Staging

As M12.7 did for NVMe: each stage usable on its own, a review before each
PC check.

### Stage 0: the hardware's facts (P0, the owner)

The BIOS settings and the cable of Q1 and Q2; then Jam OS once with
`iommu=on` and once with `iommu=off` (does the desktop show? the boot
logs: the iGPU's PCI id and BARs, the DMAR table's units and RMRRs); then
a Linux live stick once, saving: `lspci -vvnn` for 00:02.0, i915's boot
messages, the display state, the VBT, the list of workarounds i915 applied
(debugfs), the monitor's EDID, the DMAR table; and a Vulkan test program
run once with `i915.enable_guc=0`. **Done when** HARDWARE.md has the
facts (track GPH).

### Stage 1 (G3a): the display, taken over (option 1)

**Done when** (P1 on the PC, `iommu=on`): drv/igpu binds and logs the
firmware's mode (width, height, refresh from the timings); the compositor
draws by flips: a sweeping-bar test shows no tear (the owner looks; the
driver's frame and flip counters show one flip per vblank and none missed
in 10 minutes of animation); frame callbacks come at the refresh rate; the
pipe's CRC of a known pattern matches **[assumed: the CRC registers work
as IGT uses them]**; `kill igpu` brings the screen back within 2 s; the
kernel debugger's panic shows its text; `update` reboots show the boot
log. **Tests**: in QEMU (no iGPU) the init run and the compositor's flip
output against a fake display service (a test program serving the
display protocol with VMOs and timed vblank events), so the compositor's
half is tested on every run; host tests of the driver's register
encodings in `make check`; the rest on the PC.

### Stage 1b (later, by need): real mode setting

Other modes and refresh rates, hotplug, DP link training, more planes.
Either option 3's display or our own from the PRM, i915, Haiku and
Fuchsia ([Q4](#q4-the-monitor-and-more-than-the-firmwares-mode)).

### Stage 2 (G4a): commands (option 2; option 3 replaces it)

- **2a**: forcewake, the global GTT with its scratch page, the GT's
  interrupts, the render and copy engines' rings, execlists, a golden
  context, a batch in the global GTT that stores a value.
- **2b**: per-client PPGTTs and contexts, the workaround list (checked
  against P0's dump), hang check and engine reset, the GT frequency, the
  client interface (i915's uAPI subset), libc's device file.

**Done when** (P2 on the PC): a test program (bin/gputest, no Mesa)
stores values from batches on both engines and fills 64 MiB with the copy
engine, checked by the CPU; two clients' address spaces are separate (a
batch aimed at the other's address faults, its context is banned, the
other runs on); a batch that never ends is found within 2 s and the engine
reset, the next batch runs; 10,000 batches leak nothing (handles, pins);
the driver killed mid-batch comes back; every workaround register reads
back what Linux wrote. **Tests**: host tests in `make check` of the
encodings (page-table entries, the execlist port and status buffer, the
batches), against a register-file fake; gputest on the PC.

### Stage 3 (G4b): Vulkan (Mesa)

**Done when** (P3): a Vulkan test program (an offscreen triangle, a
sampled texture, a compute shader filling a buffer, 1,000 frames) gives
the expected checksums; Vulkan names the device "Intel(R) UHD Graphics
770"; a hanging shader gives `VK_ERROR_DEVICE_LOST` and the program ends
cleanly. Khronos's conformance suite only as a smoke subset, if the owner
OKs its download.

### Stage 4 (G4c): Chromium on the GPU

**Done when** (P4): with ANGLE on Vulkan, WebGL's renderer names ANV on
the UHD 770 (cr-test `webgl` on the PC); a WebGL demo and a long page
scrolled run at the monitor's rate; a full-screen canvas is scanned out
directly (the compositor logs it); a page whose shader never ends loses
only its own context; a renderer can't open `svc gpu`; without the iGPU
(QEMU, or the iGPU off) cr-test `webgl` passes on SwiftShader as today;
BENCH has WebGL and MotionMark against SwiftShader and the CPU used while
scrolling.

### Stage 5 (later): video

Decode through ANV's Vulkan Video or VA-API with Intel's media driver, the
open codecs only; HuC questions there. A plan of its own.

## Firmware blobs (GuC, HuC, DMC)

What i915 loads on this chip (Raptor Lake-S uses Alder Lake-S's entries)
**[checked: S2, S4, sizes S20]**:

| File (linux-firmware) | What | Size | Needed |
|---|---|---|---|
| i915/tgl_guc_70.bin | GuC: the GPU's scheduling microcontroller | 335,360 B | only for GuC submission (i915's default here; not ours) |
| i915/tgl_huc.bin | HuC: media microcontroller (encode bit-rate control, protected content) | 589,888 B | not for stages 1-4 |
| i915/adls_dmc_ver2_01.bin | DMC: the display's power states | 18,704 B | no: without it i915 logs "Disabling runtime power management" and keeps the display out of its low-power states **[checked: S4]** |

All three are under Intel's binary licence, LICENSES/LICENSE.i915 in
linux-firmware **[checked: S20]**: redistribution in binary form without
modification, the notice reproduced, no endorsement, **no reverse
engineering, decompilation or disassembly**, and a patent licence only for
use with an operating system under an OSI-approved licence (Jam OS's
BSD-2 qualifies). They run on the GPU's own microcontrollers, never on
Jam OS's CPUs. Stages 1-4 need none ([Q5](#q5-gpu-firmware-blobs)).

## The test loop

QEMU can't show any of this, and the PC is also the build machine: while
it runs Jam OS it builds nothing. Three ways (Q6):

- **(A) The owner's sessions**: each hardware step waits for him at the
  PC (as M12.7's P checks did). Fine for stage 1 (a few runs); slow for
  stage 2, where bring-up is many short tries, each possibly a hang.
- **(B) Automated one-shot boots on the PC**: Windows sets the next boot
  to Jam OS once (its boot manager can do that **[assumed]**), Jam OS runs
  a hardware test list, sends the results by netlog and reboots into
  Windows. No new hardware, but the PC builds nothing meanwhile, and a hang
  that wedges it needs the owner (or a smart plug and "power on after AC
  loss" in the BIOS).
- **(C) A second machine**: a used office mini PC with a 12th- or 13th-gen
  desktop CPU (Alder Lake-S or Raptor Lake-S: UHD 730 or 770, the same GT
  and display version), a smart plug, network boot or Jam OS's own update
  for new builds, and the display engine's CRCs (or a cheap HDMI capture
  stick) for what's on screen. Agents run hardware tests alone and the PC
  stays the build machine. The owner's money and desk space.

Either way, `make check` gets host tests of the driver's encodings
against a register-file fake, so most logic is tested before any boot.

## Questions for the owner

**Answered (the owner, 2026-10-20): the recommendations taken** (two cables, the board's HDMI for Jam OS; the BIOS settings when the time comes; route B, our own drivers first and i915 by need; no firmware blobs for now, DMC with run well; the stage-3 downloads approved when stage 3 comes). Still the owner's to answer: Q4 (the monitor's model and top refresh rate) and Q6 (a test box: his call, not required).

Each ends with a recommendation; the plan assumes it until you say
otherwise. Q1, Q2 and Q3 shape the work; the rest are smaller.

### Q1. The cables and the monitor's inputs

- **(A) One cable to the board.** Jam OS and Windows both on the iGPU's
  output; Windows renders games on the RTX and copies each frame to the
  iGPU (Windows' hybrid graphics): some cost in frame rate and latency, and
  the board's outputs are rated 4K at 60 Hz only.
- **(B) Two cables, two inputs**: the board's output to one input of the
  monitor (Jam OS, and the firmware's screens), the RTX to another
  (Windows). Windows sees two displays on one monitor: set "Show only on"
  the RTX's once, and it remembers. The boot menu appears on the board's
  input; you switch the monitor's input after picking Windows (or the
  monitor follows the active signal).
- **(C) One cable, moved by hand** between the RTX and the board.

*Recommendation: (B), with the board's **HDMI** output if the monitor has
an HDMI input:* stage 1 can't retrain a DisplayPort link, so a DP monitor
switched off and on could stay black until a reboot, while HDMI at this
mode needs no training **[assumed]**. Windows games keep the RTX's output.

### Q2. The BIOS settings

When the time comes (P0): **iGPU Multi-Monitor: Enabled** (the RTX stays
on for Windows); **Primary Display: IGFX** (the firmware lights the
board's output itself, so stage 1 only takes that mode over; with the RTX
primary the iGPU's output would start dark and stage 1 would need full
mode setting); **DVMT pre-allocated memory**: the default. Jam OS then
manages a little less RAM (the stolen memory), and the HD Audio's codec
2 (the iGPU's display audio, silent today) may start answering: the
audio driver must leave it alone, as it uses only codec 0's headphone
path **[assumed: P0's boot log shows]**.

*Recommendation: as listed.*

### Q3. The driver route

- **(A) Native only**: stage 1 (option 1), stage 2 (option 2), Mesa and
  Chromium on top; the firmware's mode for good unless we write mode
  setting ourselves.
- **(B) Native first, i915 by need** (the [recommendation](#recommendation)
  above): as (A), with option 3 replacing options 1 and 2 when stage 2a
  stalls, or when you want what only full display support gives.
- **(C) i915 from the start**: stage 1 as the quick win anyway, then the
  layer and i915 instead of option 2: the most foreign code and the longest
  first step, but everything Intel ships.

*Recommendation: (B).* Cost [estimate]: stages 1-4 about 40-55 agent-hours
on (A) and (B); about 70-105 on (C).

### Q4. The monitor, and more than the firmware's mode

Which monitor is it (model, top refresh rate at 2560x1440, inputs)? Do you
want Jam OS above 60 Hz, or hotplug? Stage 1 shows the firmware's mode
only; P0 tells us its refresh rate.

*Recommendation: tell us the model; if a higher refresh rate matters,
plan option 3's display (or stage 1b) right after stage 4.*

### Q5. GPU firmware blobs

GuC (335 KB), HuC (590 KB) and DMC (19 KB), Intel's binary licence
([above](#firmware-blobs-guc-huc-dmc)). **(A)** none until something needs
one; **(B)** DMC only, for the display's idle power; **(C)** all three in
a separate `firmware-i915` package, with their licence.

*Recommendation: (A) now; (B) when "run well" measures idle power; HuC
with video (stage 5) if ever.*

### Q6. The test loop

(A), (B) or (C) of [the test loop](#the-test-loop).

*Recommendation: your call. (C) if you want stages 2 and 3 to move at
agent speed without you at the PC; otherwise (A) for stage 1 and (B) to
try.*

### Q7. The downloads

[Below](#downloads-for-the-owners-ok): fetch Mesa, libdrm and the build
host's LLVM tools when stage 3 starts?

*Recommendation: yes, at stage 3, not before.*

## The tracks

Each track starts from main, owns the files listed (an edit outside them
stays minimal and is named in its report), commits refactors and behaviour
changes separately, adds a test that fails without each behaviour, and
never pushes or starts agents of its own. Models: **Opus** for the driver,
the kernel, the compositor's output and every review; **Sonnet 5.5** for
facts, tests, tools, ports that follow a pattern, and the join. New files
in plain text.

### Stage 0

| Track | What | Files it owns | Waits for | Tests | Model |
|---|---|---|---|---|---|
| **GPH. The facts** | P0's logs and dumps into HARDWARE.md (the iGPU's row, the outputs and their ports, the DMAR table's units and RMRRs, the firmware's mode and refresh, the GT's workaround list kept as a test input); this plan's [unknown] marks answered | docs/HARDWARE.md, this plan, a workaround-list file for GPK2's test (new, under tools/) | P0 | `make check` | Sonnet 5.5 |

### Stage 1: the display (option 1)

| Track | What | Files it owns | Waits for | Tests | Model |
|---|---|---|---|---|---|
| **GPD1. The driver's display half** | drv/igpu: binding, BAR0, the global GTT (scratch entries, the firmware framebuffer's entries never touched), the MSI and the display's interrupts, the takeover (mode, plane), the display protocol (display.idl, new: mode, buffers, flip, vblank events), the panic list; devmgr's match row | drivers/igpu/ (new), abi/idl/display.idl (new), devmgr's match table | CR-f done; P0; GPH | host tests of the encodings (tools/igpucheck.c, new, in `make check`); init run (no device in QEMU: nothing binds); P1 | **Opus** |
| **GPD2. The compositor's flip output** | the output beside the firmware framebuffer: three buffers, buffer age, `clflushopt` of written lines, flips, frame callbacks at vblank, the switch back and forth when the driver comes and goes; the fake display service for QEMU | `user/services/compositor/` (a new output file and its hook), the fake service (new, a test program) | GPD1's protocol merged (in parallel after that) | the compositor's area scripts and the shell scripts; a new script running the fake service in QEMU (flips, a dead driver, the switch back) | **Opus** |
| **GPD3. The kernel's part** | devmgr's read of GGC, BDSM and ASLS; stolen memory as the iGPU's boot region in its domains (if P0 shows the black screen); the panic path's register list and the iGPU's bus mastering at panic and kexec | `kernel/dev/` (the IOMMU's boot regions, the panic list), devmgr's config read | P0 | ktest at 4 and 8; kd-save; the kexec scripts; P1 | **Opus** |
| **GPR1. Review and fix** | GPD1-3: can a client or the compositor make the driver write a register or a GTT entry it shouldn't; the panic and kexec paths; restarts | whatever its fixes need | GPD1-3 merged; **before P1** | the area scripts; ktest at 4 and 8 | **Opus** |

### Stage 2: commands (option 2)

| Track | What | Files it owns | Waits for | Tests | Model |
|---|---|---|---|---|---|
| **GPK1. GT bring-up (2a)** | forcewake, GT reset, the render and copy rings, execlists (the submit port and the status buffer), the golden context, a stored value from a global-GTT batch | drivers/igpu/ (gt files, new) | P1 confirmed | host tests against the register fake; gputest's first case on the PC | **Opus** |
| **GPK2. Contexts, isolation, hangs (2b)** | per-client PPGTTs and contexts, the workaround list (i915's for Alder Lake-S and Raptor Lake-S), MOCS and PAT tables, hang check and engine reset, banning, the GT frequency | drivers/igpu/ | GPK1 merged | host tests (page tables, workaround list vs P0's); gputest's isolation and hang cases | **Opus** |
| **GPK3. The client interface** | the i915 uAPI subset as an IDL served per client (gpu.idl, new), DRM's sync objects and dmabuf export and import, implicit fences on shared buffers; libc's `/dev/dri/renderD128` device file (ioctl as calls, `mmap` of a buffer offset as its VMO); `svc gpu` in init's namespace words | drivers/igpu/ (client files), abi/idl/gpu.idl (new), libc's device-file files (new, `user/posix/sys/`), init's grant word | GPK1 merged (beside GPK2) | posixtest cases for the device file; gputest through libc | **Opus** |
| **GPK4. gputest and the scripts** | bin/gputest (stores, fills, two clients, a hang, 10,000 batches, a killed driver), the PC run list for stage 2 | gputest's directory (new), its run list | GPK1 merged; final run after GPK2 and GPK3 | itself, on the PC | Sonnet 5.5 |
| **GPR2. Review and fix** | GPK1-4: isolation between clients, what a batch can reach, the pins, hang recovery, the uAPI's checks | whatever its fixes need | GPK1-4 merged; **before P2** | the stage 1 and 2 tests | **Opus** |

**If option 3 is chosen instead** (Q3 (C), or a trigger in stage 2):
GPL1 the layer (memory on VMOs, locks, RCU, work queues, timers,
interrupts, PCI and firmware, dma-buf from Linux: Opus, the largest
track, split by area), GPL2 i915 trimmed and built against it at 6.18 (Opus),
GPL3 bring-up on the PC (Opus), GPR2 as above. GPK3's libc device file and
GPK4's gputest stay the same: they speak i915's uAPI either way.

### Stage 3: Vulkan

| Track | What | Files it owns | Waits for | Tests | Model |
|---|---|---|---|---|---|
| **GPM1. libdrm and Mesa** | ports/libdrm and ports/mesa (new): recipes, the device-finding branch, the static Vulkan driver, the host tools on the PC | ports/libdrm/, ports/mesa/ (new) | P2 confirmed; Q7 | `tools/ports.py` builds; the selftest | **Opus** |
| **GPM2. The Vulkan tests** | a Vulkan test program (triangle, texture, compute, 1,000 frames, a hang), its PC run list | its directory (new) | GPM1 merged | itself on the PC | Sonnet 5.5 |
| **GPR3. Review and fix** | GPM1-2 and the driver under Mesa's load: leaks, fences, what Mesa's paths reach that gputest didn't | whatever its fixes need | GPM1-2 merged; **before P3** | stage 2 and 3 tests | **Opus** |

### Stage 4: Chromium

| Track | What | Files it owns | Waits for | Tests | Model |
|---|---|---|---|---|---|
| **GPC1. Chromium on ANV** | args (`use_wayland_gbm=true`), minigbm on our uAPI, the two-driver Vulkan dispatch (patch 0915's group), GPU info and the blocklist, cr-test cases | ports/chromium's patches and args, `tools/cr/` cases | P3 confirmed; GPC2 for the window | cr-test on the PC; cr-test `webgl` in QEMU (SwiftShader unchanged) | **Opus** |
| **GPC2. dmabufs and direct scanout** | `zwp_linux_dmabuf_v1` with VMOs in the compositor and the Wayland shim, formats and modifiers from the plane, readiness events from the driver, direct scanout of a full-screen buffer | `user/services/compositor/`, the shim's dmabuf part, drivers/igpu/ (the readiness event) | P3 confirmed | wlprobe's dmabuf cases; the compositor's scripts; the fake display service | **Opus** |
| **GPC3. The GPU process's sandbox** | `svc gpu` in CR-f's per-type table for the GPU process only; a test renderer that tries it fails; the JIT power off with ANV | CR-f's table (by then its own files) | CR-f done; GPC1 | the sandbox's hostile tests | Sonnet 5.5 |
| **GPR4. Review and fix** | GPC1-3: what a web page can do to the GPU, the driver and the compositor | whatever its fixes need | GPC1-3 merged; **before P4** | everything above | **Opus** |
| **GPJ. The join** | ARCHITECTURE (drivers table, Graphics, the IOMMU's GPU notes), HARDWARE, TESTING, USING, ROADMAP, HISTORY; this plan to docs/history | the docs | GPR4 merged | `make check` | Sonnet 5.5 |

## Order and parallel work

1. **Now**: the owner's answers; P0 at his next hardware session (it
   needs nothing built).
2. **After CR-f**: GPH, then GPD1 and GPD3 together, GPD2 once GPD1's
   protocol is in; GPR1; **P1**.
3. **After P1**: GPK1, then GPK2 and GPK3 together, GPK4 beside them;
   GPR2; **P2**. The decision point for option 3 is during GPK1 and GPK2.
4. **After P2**: GPM1, GPM2; GPR3; **P3**.
5. **After P3**: GPC1 and GPC2 together, GPC3; GPR4; **P4**; GPJ; All
   tests and `soak 10` on the PC with the GPU bound.

"Run well" goes beside it (the owner's pairing): its measurements of the
compositor and of Chromium are the BENCH rows this plan adds. The driver
tracks touch new files and the compositor's output; the kernel track
(GPD3) touches the IOMMU's boot regions and the panic path, so it waits
for any running kernel track in those files.

## Tests

| What | Where |
|---|---|
| Register and page-table encodings, the execlist state machine, the workaround list against P0's | host tests in `make check` (a register-file fake; no QEMU, no PC) |
| The compositor's flip output | the fake display service in QEMU, every run |
| The kernel's part | ktest at 4 and 8, kd-save, the kexec scripts |
| The client interface in libc | posixtest cases (the device file, `mmap` of a buffer) |
| Stage 1 on the hardware | P1: flips, tears, CRC, kill, panic, kexec |
| Stage 2 | gputest on the PC (P2) |
| Stage 3 | the Vulkan test program on the PC (P3) |
| Stage 4 | cr-test on the PC (P4); cr-test `webgl` in QEMU (the fallback) |
| The bench | BENCH rows: a flip's latency, the compositor's frame time at 2560x1440, WebGL and MotionMark against SwiftShader, CPU while scrolling |
| The sign-off | All tests and `soak 10` on the PC with drv/igpu bound |

## The PC checks

- **P0** (before any track; about an hour): the BIOS (Q2), the cable
  (Q1), Jam OS with `iommu=on` and `iommu=off`, a Linux live stick's
  dumps and a Vulkan program with `i915.enable_guc=0` ([Stage 0](#stage-0-the-hardwares-facts-p0-the-owner)).
- **P1** (stage 1; half an hour): the sweeping bar, an animation for 10
  minutes, `kill igpu`, a panic from the debugger, `update` and a reboot.
- **P2** (stage 2): gputest's cases, if (C) of Q6 isn't there to run them.
- **P3** (stage 3): the Vulkan test program.
- **P4** (stage 4): `chrome` with WebGL, scrolling, a full-screen canvas,
  a bad shader; the iGPU off in the BIOS once, to see the fallback.

## Size

- **Agents** [estimate]: stage 1 three tracks and a review (5-7
  agent-hours); stage 2 four and a review (16-24, plus hardware
  iteration); stage 3 two and a review (8-12); stage 4 three, a review and
  the join (10-14). **About 40-55 agent-hours** on route (B) of Q3 if
  option 3 isn't triggered; option 3 instead of option 2 adds about 30-50.
- **Time**: stage 1 a day or two of tracks, then P1; stage 2 is the
  unknown: its length is the number of hardware tries, which Q6 decides.

## Risks

- **A black screen at boot** once the iGPU is primary with `iommu=on`, if
  the board names no RMRR for stolen memory. P0 boots both ways; GPD3 has
  the fix; `iommu=off` is the way out meanwhile.
- **Bus mastering and scanout**: a blink at bind, and after a kexec a dark
  screen until the driver binds, unless GPD3 keeps the display's reads
  going ([the kernel side](#1-the-kernel-side)).
- **A dropped DisplayPort link** in stage 1: HDMI (Q1).
- **Execlists on Raptor Lake-S** is not Intel's default path since 2022:
  P0's `enable_guc=0` run shows whether it works on this chip before any
  code is written.
- **A missed workaround**: rare hangs or corruption. Linux's applied list
  from P0 is the oracle, checked register by register in GPK2.
- **A hang that wedges the PC**: the owner's power button, or (C)'s smart
  plug.
- **Two Vulkan drivers in one static program**: symbol clashes, handled
  by GPC1's dispatch.
- **A static Mesa**: if the recipe's patch fails, stage 3 waits for
  M14.5's dynamic linking.
- **Chromium refusing the GPU**: GPU info without libpci, the blocklist
  (GPC1).
- **The compositor's cost** of flushing caches for scanout (the bench
  decides; write-combining VMOs are the fallback).
- **No iGPU** (the BIOS off, another PC, QEMU): drv/igpu doesn't bind,
  the compositor stays on the firmware framebuffer, Chromium stays on
  SwiftShader (opt-in, or the default once CR-f has made it safe). Every
  stage keeps that path tested in QEMU.
- **Licences**: Intel's firmware licence only if Q5 says so; option 3's
  i915 is MIT for the most part and a separate program either way.

## Downloads for the owner's OK

None is fetched by this plan; each needs the owner's OK naming it.

| What | From | Size | When |
|---|---|---|---|
| Linux 6.18.56 (for option 3, and i915's tables for option 2) | already fetched by `ports/linux-uapi/recipe` | 154.8 MB | nothing new |
| Mesa 26.2.5 | archive.mesa3d.org | 65 MB **[checked: S21]** | stage 3 |
| libdrm 2.4.134 | dri.freedesktop.org/libdrm | 427 KB **[checked: S21]** | stage 3 |
| LLVM, clang, libclc, SPIRV-LLVM-Translator for the build host | the PC's WSL distribution's packages | a few hundred MB **[unknown]** | stage 3 |
| GuC, HuC, DMC firmware and LICENSE.i915 | linux-firmware (gitlab.com/kernel-firmware) | 335 KB, 590 KB, 19 KB **[checked: S20]** | only if Q5 says so |
| Tiger Lake PRM volumes, if a kept copy helps (read online otherwise): 2c registers (11.5 + 13.5 MB), 8 command streams (0.9 MB), 12 display (5.9 MB), 14 workarounds (0.7 MB) | intel.com | as listed **[checked: S19]** | stage 1 or 2 |
| Khronos's Vulkan conformance suite (a smoke subset) | github.com/KhronosGroup | **[unknown]** | stage 3, optional |
| minigbm | in Chromium's tree **[assumed]** | none | stage 4 |

## Later

- Stage 1b, real mode setting (or option 3's display): refresh rates,
  hotplug, overlay and cursor planes.
- Stage 5, video decode.
- iris (GL) and EGL for native GL programs; WebGPU on ANV.
- DMC for idle power (with "run well").
- The compositor composing on the GPU, if the CPU's share ever matters.

## Pointers this plan needs elsewhere

Not edited by GP0 (other tracks own the files): ROADMAP's G3 and G4 rows
(the main path, the options, this plan's link); CR-PLAN's "Later" row and
section 9 (`use_wayland_gbm`, linux-dmabuf "until the GPU" now has a
plan), section 13 (the GPU process's `svc gpu`); TESTING.md (the new
scripts, when built).

## Sources

All read on 2026-10-20. "master" means the tree's head that day.

- **S1** Linux master, drivers/gpu/drm/i915/gt/uc/intel_uc.c, `uc_expand_default_options`: <https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/gt/uc/intel_uc.c>
- **S2** Linux master, drivers/gpu/drm/i915/gt/uc/intel_uc_fw.c, the GuC and HuC firmware lists (Alder Lake-S: tgl_guc_70, tgl_huc): <https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/gt/uc/intel_uc_fw.c>
- **S3** Linux master, include/drm/intel/pciids.h, `INTEL_RPLS_IDS`: <https://github.com/torvalds/linux/blob/master/include/drm/intel/pciids.h>
- **S4** Linux master, drivers/gpu/drm/i915/display/intel_dmc.c, `ADLS_DMC_PATH` and the failure message: <https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/display/intel_dmc.c>
- **S5** Linux master, drivers/gpu/drm/i915/i915_pci.c, `adl_s_info`, `GEN12_FEATURES`, the RPL-S ids' entry: <https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/i915_pci.c>
- **S6** Linux master, drivers/gpu/drm/i915/display/skl_universal_plane_regs.h, the plane registers: <https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/display/skl_universal_plane_regs.h>
- **S7** Linux master, drivers/gpu/drm/i915/display/intel_display_regs.h, the display's interrupt, frame counter and timing registers: <https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/i915/display/intel_display_regs.h>
- **S8** Linux 6.18, drivers/gpu/drm/i915/display/intel_display_device.c, `adl_s_desc`: <https://github.com/torvalds/linux/blob/v6.18/drivers/gpu/drm/i915/display/intel_display_device.c>
- **S9** Linux 6.12, drivers/gpu/drm/i915/i915_cmd_parser.c, `intel_engine_init_cmd_parser`: <https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/i915_cmd_parser.c>
- **S10** Linux 6.12, drivers/gpu/drm/i915/display/intel_fb_pin.c (the VT-d alignment and padding) and intel_display.c (`intel_scanout_needs_vtd_wa`): <https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/i915/display/intel_fb_pin.c>
- **S11** Linux 6.18, drivers/gpu/drm/xe/xe_pci.c, `adl_s_desc`: <https://github.com/torvalds/linux/blob/v6.18/drivers/gpu/drm/xe/xe_pci.c>
- **S12** LWN, the xe driver's first posting (2022), on its two submission back ends: <https://lwn.net/Articles/918468/>
- **S13** Sizes: GitHub's tree API on torvalds/linux at v6.12 and v6.18 (drivers/gpu/drm/i915, xe, display, the DRM top level, drivers/dma-buf); bytes / 30, calibrated on intel_display.c, intel_workarounds.c and intel_execlists_submission.c at 6.12
- **S14** FreeBSD: sys/compat/linuxkpi on main (GitHub freebsd/freebsd-src; licence header of linux_compat.c), freebsd/drm-kmod master (its linuxkpi directory; `__FreeBSD__` in its i915 by GitHub code search)
- **S15** OpenBSD: sys/dev/pci/drm on the openbsd/src mirror (drm_linux.c and its ISC header, include/linux, i915; `__linux__` in its i915 by GitHub code search)
- **S16** Phoronix, "FreeBSD Updates Its Graphics Driver Port From Linux 6.12 LTS": <https://phoronix.com/news/FreeBSD-drm-kmod-Linux-6.12>
- **S17** Haiku: src/add-ons/kernel/drivers/graphics/intel_extreme/driver.cpp (the device list), the accelerant's and the driver's sizes and commit log (GitHub haiku/haiku)
- **S18** Fuchsia: src/graphics/drivers/msd-intel-gen (README, src listing) and src/graphics/display/drivers/intel-display (listing, sizes): <https://fuchsia.googlesource.com/fuchsia/+/main/src/graphics/drivers/msd-intel-gen/>
- **S19** Intel, Graphics for Linux developer reference (the platform list; Tiger Lake's 18 volumes and sizes): <https://www.intel.com/content/www/us/en/docs/graphics-for-linux/developer-reference/1-0/tiger-lake.html>
- **S20** linux-firmware on gitlab.com/kernel-firmware: LICENSES/LICENSE.i915 (read in full) and the three files' sizes from the repository's API
- **S21** <https://archive.mesa3d.org/> (mesa-26.2.5.tar.xz, 65M, 2026-10-14); <https://dri.freedesktop.org/libdrm/> (libdrm-2.4.134.tar.xz, 427K)
- **S22** Mesa's Intel drivers need libclc and LLVM at build time since 24.1, and the `mesa-clc` and `precomp-compiler` system options: Yocto oe-core patch threads (patchwork.yoctoproject.org, 2025-2026) and Mesa's commit "intel: Allow using intel_clc from the system"
- **S23** Phoronix, "Intel's Mesa Drivers Begin Preparing For The New Xe Kernel Driver" and "Mesa 24.1 Enables Intel Xe Kernel Driver Support By Default"; Mesa merge request 20418
- **S24** ChromiumOS minigbm, the repository's file list (i915.c, xe.c): <https://chromium.googlesource.com/chromiumos/platform/minigbm/+/refs/heads/main>
- **S25** Chromium main, ui/ozone/platform/wayland/gpu/ (listing) and wayland_surface_factory.cc (`CreateViewGLSurface`, `CreateSurfacelessViewGLSurface`, `CreateVulkanImplementation`)
- **S26** Microsoft, Windows Hardware Lab Kit, the UEFI display requirement (a desktop's firmware sets the display's native resolution and timing from EDID): <https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/6afc8979-df62-4d86-8f6a-99f05bbdc7f3>
- **S27** ASUS, TUF GAMING B760-PLUS WIFI tech specs (one DisplayPort 1.4 and one HDMI 2.1, 4K at 60 Hz): <https://www.asus.com/motherboards-components/motherboards/tuf-gaming/tuf-gaming-b760-plus-wifi/techspec/>
