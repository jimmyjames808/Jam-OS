# The real PC

Jam OS is developed against one machine, and every milestone is checked on
it. This page is the one place its facts are written down; other docs link
here. The facts come from Jam OS's own boot log and device listings on that
PC (the Devices and usb-bus runs of 2026-09-29, the boot logs saved on
the stick on 2026-10-01, and the network runs of 2026-10-02).

## The machine

| Part | What it is |
|---|---|
| Board | ASUS TUF GAMING B760-PLUS WIFI |
| CPU | Intel Core i7-14700 (not F), Raptor Lake: family 6, model 0xB7, stepping 1, microcode 0x11F. 8 P-cores with Hyper-Threading + 12 E-cores = **28 CPUs** in x2APIC mode (xAPIC mode only reached 20). TSC 2112 MHz (invariant), TSC-deadline timer, PCIDs + INVPCID, SMEP/SMAP/UMIP, protection keys (PKU and PKS), WAITPKG, CET; no user interrupts. XSAVE with xcr0 = 7 (832-byte state) |
| Topology | cpus 0-15: the P-cores, two threads each (cpu 0 and 1 are core 0, 2 and 3 are core 4, ... 14 and 15 are core 28); cpus 16-27: the E-cores, one thread each (cores 32-43) |
| Memory | 32 GB (Jam OS manages 32049 MiB) |
| Display | NVIDIA RTX 4080 SUPER (01:00.0, 10de:2702), monitor on it. UEFI GOP framebuffer 2560x1440, 32 bpp, at physical 0x4000000000 (256 GiB: Resizable BAR on). No iGPU PCI function: the UHD 770 is disabled in firmware |
| PCI | 24 functions on 6 buses |
| Serial | a working COM1 UART, but no cable; logs come off the PC as files on the stick (each boot's log in `/data/logs/`), over the network to the Mac (netlog, on the "Jam OS (network)" boot: [The network](#the-network)) and as photos of the screen |

**PCIDs.** Alder Lake and Raptor Lake have an erratum (Intel ADL063,
RPL042): with PCIDs on, INVLPG may leave global TLB entries behind. Intel's
microcode fixes it from revision 0x118 on this model, and the PC has
0x11F, so PCIDs stay on here. The boot log's `cpu id:`, `cpu bits:` and
`pcid:` lines say so on every boot
([ARCHITECTURE.md](../ARCHITECTURE.md#memory) has the rule).

**Random numbers.** The i7-14700 has RDRAND and RDSEED: `cpu bits:` ends
`rdrand=1 rdseed=1`, a few lines later the log should say `random:
seeded from RDSEED, ...`, and the RESULTS box has no `random:` line (one there
means the hardware failed its check, or is missing:
[ARCHITECTURE.md](../ARCHITECTURE.md#random-numbers)).

## USB

One controller carries the keyboard, the mouse and the boot stick:

- **xHCI** 00:14.0, Intel 8086:7a60 rev 0x11 (B760, xHCI 1.2). **MSI only**
  (8 vectors, 64-bit, not maskable), no MSI-X. BAR0 64 KiB. 25 ports
  (USB 2: 1-16, USB 3: 17-25), 48 slots, 32-byte contexts, 34 scratchpad
  buffers. The BIOS hand-off works (not BIOS-owned at boot).

What usb-bus finds on it (root port numbers):

| Port | Device | What |
|---|---|---|
| 2 | 0b05:19af FS | ASUS AURA LED controller (vendor + HID 03/00/00) |
| 7 | 2516:01c9 FS | Cooler Master "ARGB GEN-2" (3 HID 03/00/00) |
| 8 | 2516:01c1 FS | Cooler Master "HAF700" (HID + vendor ff/42/01) |
| 9 | 174c:2074 HS | ASMedia USB 3 hub, USB 2 half (4 ports) |
| 9.1 | 058f:6387 HS | **the boot stick** (Alcor "Generic Flash Disk", 2 GB, Bulk-Only mass storage), behind the hub: `usb-storage-9.1:0` |
| 10 | 0c45:652f FS | **the keyboard** (Microdia): if0 boot keyboard, if1 boot mouse |
| 11 | 258a:0033 FS | **the mouse** (Sino Wealth "Wired Gaming Mouse"): if0 mouse, if1 keyboard |
| 24 | 174c:3074 SS | ASMedia hub, USB 3 half |

So the keyboard is on a root port (it needs no Transaction Translator), and
storage talks to the stick through the high-speed hub. The shell's hid
process for the keyboard is `hid-10:0`. Each hid of a boot mouse interface
logs that interface's report descriptor in hex, the layout it found and
the protocol it chose (report protocol for a mouse with a wheel).

Seen in the boot logs, all harmless so far:
- The mouse on port 11 (258a:0033) often fails its first attempt: in the
  boot logs of 2026-09-30 and 2026-10-01 its Address Device ended in a USB
  Transaction Error twice, or its first GET_DESCRIPTOR did, or (boot 11)
  the command got no completion for 3 s and the try right after the abort
  worked at once. Each time it attached about 0.2 s after the first try
  (after a connect change, or at the next try). Most likely the mouse
  doesn't answer for a few hundred ms after its first bus reset (busy, or
  dropping off the bus and coming back as the HAF700 does), and the xHC
  retries a NAKed SET_ADDRESS for as long as software lets it. usb-bus
  gives Address Device 250 ms a try (USB 2.0 allows a device 50 ms),
  tries a failed port again after 100 ms, doubling, and treats a
  reconnect during the attempt as a fresh start; since every root port is
  attached in its own task (M8.6), the mouse can only delay itself. Each
  failed try is logged with the PORTSC it left (connected, enabled, a
  connect change), which tells the two causes apart.
- The HAF700 on port 8 detaches and comes back once during enumeration,
  and after a warm reboot devmgr can log that its hid "did not end
  cleanly" (a follow-up in [ROADMAP.md](ROADMAP.md#smaller-follow-ups)).

On the boot stick Jam OS mounts partition 1 (the ESP, 63 MiB) read-only at
`/esp` and partition 2 (FAT32, the rest of the stick) at `/data`. A second
USB stick, plugged in at any time, is mounted read-only at `/usb0` if it
holds a FAT volume (in an MBR partition, or over the whole stick with no
partition table; not GPT): `mount` lists it, `mount -w /usb0` makes it
writable, `mount -r /usb0` read-only again. It is never formatted. Tried
on the PC on 2026-09-30 with a SanDisk 15.4 GB stick (MBR, one FAT32
partition): read, made writable, written, read-only again, and macOS's
check found it clean afterwards. Anything plugged in while Windows or
macOS runs gets their files (`System Volume Information`, `.Spotlight-V100`,
`.fseventsd`): `/data` is not private to Jam OS.

## Other devices

| Function | Device | Notes |
|---|---|---|
| 05:00.0 | Realtek RTL8125B 2.5 GbE, 10ec:8125 rev 05 | drv/rtl8125: [The network](#the-network) |
| 00:1f.3 | Intel Raptor Lake PCH HD Audio, 8086:7a50 rev 11, class 04 03 00 (HDA mode, not the audio DSP's). MSI (1, 64-bit), no MSI-X. BAR0 mem64 16 KiB (the HDA registers), BAR4 mem64 1 MiB (the DSP's, unused) | drv/hda. Codec 0 is a Realtek ALC897 (10ec:0897, subsystem 1043:8841); the front-panel headphone jack is its pin 1b, fed by DAC 02 through mixer 0c. Codec address 2 is reported but never answers (likely the disabled iGPU's HDMI codec). The whole graph: `hda` in the shell, `[hda]` lines in the boot log, and [A1-PLAN.md](history/A1-PLAN.md#the-hardware) |
| 01:00.1 | NVIDIA HD Audio, 10de:22bb | HDMI/DP audio on the RTX: no driver, not planned |
| 00:14.3 | Intel Wi-Fi (the board lists an AX201), 8086:7a70 (MSI-X 16) | not planned |
| 00:0e.0 | Intel VMD/RAID, 8086:a77f | its 64-bit BAR has a hard-wired-zero upper half (see [HISTORY.md](HISTORY.md#m6-pci-msi-devmgr-drivers-through-handles)) |
| 02:00.0 | Crucial NVMe, c0a9:5421 (MSI 8, MSI-X 9) | not used: storage is the USB stick; no driver planned |
| | SATA, SMBus, I2C/SPI functions, 6 bridges | |

## The IOMMU (VT-d)

From the read-only probe at a cold boot (2026-10-04, build b7713bd,
boot-2026-10-04_21-\*; `dmesg | grep -E 'vtd:|acpi:'`), the facts M11
([docs/M11-PLAN.md](M11-PLAN.md)) is built against:

- **One remapping unit**, registers at **0xfed91000** (one page), segment
  0, with INCLUDE_PCI_ALL: it covers every PCI function (the iGPU is
  disabled in the firmware, so there is no second unit for it). Its device
  scopes name the PCH's **I/O APIC (id 2, requester 00:1e.7)** and **HPET
  (0, 00:1e.6)**. **No RMRR**, ATSR, SATC or ANDD: no device keeps a
  reserved region across the handover.
- **CAP d2008c40660462, ECAP f050da**: 256 domain ids, 4-level page tables
  (48-bit), 39-bit guest addresses (MGAW), caching mode **off** (CM 0),
  write-buffer flush **off** (RWBF 0), page-selective invalidation (PSI 1,
  MAMV 18), interrupt-entry mask MHMV 15, page walks **not coherent** with
  the CPU's caches (so the kernel flushes every table line it writes),
  queued invalidation, interrupt remapping with x2APIC ids (EIM),
  pass-through and snoop control all present; one fault-recording register
  at offset 0x400.
- At a cold boot the firmware leaves **translation, interrupt remapping
  and queued invalidation all off**, no fault recorded, the fault event
  masked; the highest APIC id is 86 (under 255, so the fault event's own
  interrupt needs no remapping). So Jam OS builds its own tables from
  scratch on an `iommu=on` boot.

QEMU's emulated unit differs (registers at 0xfed90000, caching mode on, an
explicit endpoint scope per function instead of INCLUDE_PCI_ALL, no RMRR):
what only the PC proves is in [M11-PLAN.md](M11-PLAN.md#what-only-the-pc-can-show).
As of 2026-10-05 the PC has run the probe only, never `iommu=on`: its
first boots with the IOMMU on are the "Developer > IOMMU checks" and
"Developer > Jam OS (IOMMU)" entries ([TESTING.md](TESTING.md#the-iommu)), and the IOMMU
stays off by default until they pass.

## The network

**The chip** (05:00.0, 10ec:8125 rev 05): a Realtek RTL8125B, by its own
id (`xid 641` in the transmit configuration register). MSI (1, maskable)
and MSI-X (32 vectors; table at BAR4+0x0, PBA at BAR4+0x800). BAR0 is I/O
0x3000, BAR2 mem64 64 KiB (the registers), BAR4 mem64 16 KiB (MSI-X only),
so the registers never share a page with the MSI-X table. From the
listen-only probe on 2026-10-02 (boot-0065, build f6c57bf;
[the results](M9-PLAN.md#stage-0-on-the-pc-2026-10-02-boot-0065-the-results)):
PCIe gen 2 x1 with ASPM off; the PHY's id 001cc840 with no firmware patch
loaded (the board's firmware loads none), the MAC's ROM code with no break
points; with no firmware tables at all it resets in 1 ms and links at
**1000 full in 2.2 s** (it advertises 2.5G too: the switch port is
gigabit); MSI-X works, and so does 64-bit DMA (its rings and buffers above
4 GiB, no DMA32 needed). The firmware leaves the receiver and transmitter
off and **wake-on-LAN armed** (magic packet); the driver turns
wake-on-LAN off while Jam OS runs, and the firmware arms it again at its
next start. The chip's count of frames sent survives reboots and
power-off on standby power, so the driver compares counts from its own
start.

**Its transmit descriptors are 32 bytes**, as OpenBSD's `rge` writes them:
the driver turns on the chip's format bit for them (MAC OCP 0xeb58 bit 0,
part of rge's start-up), and with it the chip steps through the ring 32
bytes at a time. The driver's first PC runs (2026-10-02, build 8d98b62)
wrote 16-byte ones, so the chip read every other descriptor and transmit
stalled; since 41ccd54 the descriptors are 32 bytes and the driver leaves
the transmitter off unless the chip's bit agrees (the log's `transmit
descriptors: 32 bytes; ... they agree`). The receive descriptor is 32
bytes too. ([The result and the fix](M9-PLAN.md#r1-the-pc-result-and-the-transmit-fix).)
On 41ccd54 (boot-0073, the send test): 20 of 20 ARP probes to 10.2.21.1
answered, each descriptor back from the chip 0.04 ms after its doorbell,
and the chip's count of frames sent equal to the driver's.
Receiving stopped about 75 s into the first full boot (boot-0075): the
chip clears each receive descriptor's buffer address (to 0) when it hands
a frame back, so on the ring's second lap it had nowhere valid to write.
The driver now writes the address with every hand-back, as rge does. On
8ef3fac (the streamed logs of 2026-10-02 18:41): 7609 frames received in
148 s, 0 missed, every descriptor's address cleared by the chip and set
again; `ping 1.1.1.1`, `ping google.com`, `host`; `update` twice.

**The switch port** is a trunk. Untagged it carries the home network
(10.2.0.0/24); tagged it carries VLANs 10, 11, 20 and 21 (the probe saw
frames on each, and the switch's LLDP untagged). Jam OS uses VLAN 21 only
(10.2.21.0/24, "Home Devices VLAN": its DHCP server, router and DNS server
are all 10.2.21.1, and it reaches the internet) and its driver drops every
other frame. Narrowing the port on the switch to VLAN 21 tagged alone
would make the switch enforce the rule too; it isn't needed.
**The owner's builds are VLAN 21 through `local.mk`**: his tree on the
Mac has a git-ignored `local.mk` at the top with `JAMOS_VLAN := 21`
(`local.mk.example`), so every `make` there says `network default: VLAN
21 (local.mk)` and the build tags every frame with 21 on a boot with no
`vlan=` word. A build without it (a fresh clone, a worktree) is untagged:
on this port it would land on the home network, so `update` refuses one
(`update -f` forces it) and `make flash` asks before it writes one
([ARCHITECTURE.md](../ARCHITECTURE.md#networking)). The PC's
cable goes to a one-port bridge, so the Mac can't watch the PC's port; a
capture needs a cable from the PC straight to the Mac's USB Ethernet
adapter ([M9-PLAN.md](M9-PLAN.md#r1-the-pc-result-and-the-transmit-fix)).

**The Mac** is on VLAN 21 too, by Wi-Fi at **10.2.21.174** (kept by a
router reservation and a fixed "Private Wi-Fi address"): the PC's
`net.host`, where netlog sends the log and `update` fetches a build. Both
stay inside VLAN 21 and cross no firewall. The PC's own address is
`net.address` in its settings, or a lease from 10.2.21.1 without it.

Which boot entry uses the chip: every one but "Jam OS (no network)"
(`vlan=off`): the everyday entries as the netdev service with netstack on
it, "Jam OS (network: send test)" and "Jam OS (network: listen only)" as
their tests
([TESTING.md](TESTING.md#the-boot-menu)).

## Flash and boot the stick

Flashing writes to a USB disk: check which disk before every write.

**A fresh stick** (erases the whole disk):

```sh
diskutil list external           # find N
make usb DEV=/dev/diskN
```

`tools/write-usb.sh` refuses internal and non-removable disks, shows the
disk, and asks you to type YES before it erases it; writing needs your
password (`sudo dd`). Check the disk number twice anyway. The stick gets
two MBR partitions: the ESP (type 0xEF, volume `JAMOS`: Limine, the
kernel, bootfs), which Jam OS never writes, and the data partition (type
0x0C, `JAMOS-DATA`, mounted at `/data`), which `tools/mbr-grow.py` grows to
the end of the stick and leaves blank; Jam OS formats it on the first boot
that finds it blank. Everything on `/data` is lost with the rest of the
stick, so copy the logs off first.

**Updating a stick that already boots Jam OS** (the usual way; nothing is
erased and `/data` is not touched):

```sh
make flash                       # or: make flash DEV=/dev/diskN
```

`tools/flash-usb.sh` first says the build's network default (from its
boot image's `build.txt`: `network default of this build: VLAN 21`) and,
for an untagged build (made without `local.mk`) or an old one that
doesn't say, asks before it goes on; then it finds the one external disk
with Jam OS's two partitions, mounts its ESP, keeps the stick's kernel and bootfs as the
previous build (copied to `prev-jamos.elf` and `prev-bootfs.img`, the
boot menu's "Jam OS (previous build)", replacing an older one; the PC's
`update -w` renames instead, [M9-PLAN](M9-PLAN.md#update--w-built)), copies the kernel, the bootfs and `limine.conf`
under new names (`jamos.elf.new`, ...), compares all three, renames them
over the old ones, compares again, and ejects the stick. It asks for your
password: macOS does not mount an MBR partition of type 0xEF by itself,
so the script mounts it by hand with `sudo`. It refuses a disk whose first
partition holds no Jam OS kernel. Pulling the stick during the copies
leaves the old files whole (it boots as before; run `make flash` again);
only the renames at the end, a few milliseconds, could leave the new
kernel with the old boot image for one boot. It also removes
`/esp/boot/limine.conf` once the new menu is in place: the spare an `update -w`
cut short may have left ([ARCHITECTURE.md](../ARCHITECTURE.md#storage)).

The PC's `update -w` brings the boot menu as well (the server's
`boot/limine.conf`), so new boot entries need no `make flash`. A build
older than that (the stick's, the first time) skips the menu and updates
the build alone; the shell then reboots into the new build, and a second
`update -w` (the build is there already: a second or so) writes the menu.
`make flash` stays the way for a new update key, a new Limine, or a stick
whose build has no key. (A stick made before M8 has one partition that macOS mounts as
`NO NAME`: copy the three files there by hand, or remake it with
`make usb`.)

Which disk is the boot disk, with two Jam OS sticks in: the one the PC
booted from, by the MBR disk id Limine reports
([ARCHITECTURE.md](../ARCHITECTURE.md#storage)). `make usb` gives a stick
a random id; a stick made before 2026-10-01 has id 0 (the boot log's
`boot disk: no MBR disk id` line), and on it the first Jam OS disk found
is the boot disk, as before, until it is made again with `make usb`.

If Jam OS is running when the stick comes back to it with a new build on
it, `kernel load` in the shell reads the new kernel and boot image into
the stored copy at once, and the next `reboot` starts them without
reading the stick (a panic then comes back in the new build too).

macOS does mount the data partition by itself (it may show it as
`NO NAME`): the boot logs are in its `logs/` folder, `boot-0001.txt`,
`boot-0002.txt`, ..., one per boot, the newest with the highest number;
each starts with the date and time the kernel started. The settings
(`etc/settings`: the time zone, whether the PC's real-time clock keeps
local time, the volumes, the music folder) can be edited there too
([ARCHITECTURE.md](../ARCHITECTURE.md#time-and-settings)). Files Jam OS
writes are dated in local time, as FAT keeps them. Eject it before
pulling it.

Then boot the PC from the stick in UEFI mode with Secure Boot off, and pick
an entry from the boot menu ([TESTING.md](TESTING.md#the-boot-menu)). A
test run at boot (All tests, the stress test, the benchmark) ends with a
RESULTS box on the screen that sums it up; the `soak` command ends with a
SOAK RESULTS box. To end a run on the PC cleanly, `reboot` from the shell:
it syncs `/data` and has logd write the log's last lines first, then
starts a fresh copy of the system without the firmware (kexec): the
screen turns the splash background at once and the next thing on it is
the splash. It is the copy the kernel stored at boot, unless the stick's
kernel or boot image changed since (then it reads them first, slowly
until M8.6); `reboot -f` goes through the firmware and the boot menu.

## If something goes wrong on the PC

- **A panic.** The screen turns dark with the busy ring: "Jam OS hit a
  problem and is restarting" and a code (`JAM-PF-0008`) for 1.5 s, then
  the PC boots again without the firmware (the splash, then the desktop),
  and the shell's first line says what the panic was, its code and where
  its log went (`the last boot panicked: ... (saved as
  /data/logs/boot-NNNN-crash.txt). Code ...`), or why it was not saved;
  on the desktop a notice says so too, whose Details runs `crashlog` in a
  terminal (the report's summary: the code, what and where, the
  backtrace, the log lines before it, the boot and the build; `crashlog
  list` for older ones). Read the whole file on the Mac: the last 4 MiB
  of the log, the panic with its registers, backtrace and note line (a
  kernel test's loop, seed and test, when tests were running;
  [TESTING.md](TESTING.md#soak) says how to replay it). The boot's own
  `boot-NNNN.txt` stops up to a quarter of a second before the panic.
  When there is no stored kernel to start, or for a second panic within
  30 s of the boot that followed a panic, or for the third panic in a row
  (a crash loop), the screen says "Jam OS hit a problem it can't recover
  from" and counts down 15 s to a firmware reset; its details panel
  comes after 5 s (photograph it). If the reset doesn't work it says to
  hold the power button.
- **Logs.** Every boot with user space (the everyday entries, the Soak
  entry) writes its log to `/data/logs/boot-NNNN.txt`; read it on the Mac
  after a `reboot` or after pulling the plug (the last quarter second may
  be missing). The tests at boot run before user space, so All tests, the
  stress test and the benchmark are not logged: their RESULTS box on the
  screen, photographed, is the record. Nothing is logged while the boot
  stick is out.
- A hang during boot: the last line on the screen names the step (`pci:`
  lines name the function being sized). The plain entry shows the boot
  splash instead of the log, and the shell after it shows only notices
  of it: boot Developer > `Jam OS (text log, no splash)` (`verbose`) to see the log
  as it comes; `log 40` shows its last lines from the shell.
- Fewer than 28 CPUs, or a hang right after the `lapic: timer` line: the
  kernel starts the other CPUs itself (INIT-SIPI-SIPI). The boot log's
  `smp:` lines name each CPU that did not start. To compare, have Limine
  start them instead: in Limine's menu press E on `Jam OS`, add
  `cmdline: smp=loader` (or append `smp=loader` to the entry's
  `cmdline`), then F10 to boot it.
- Anything that looks like memory corruption: boot with `nopcid` first
  (QEMU's TCG has no PCIDs, so the PC is the only place they run). The boot
  log's `cpu id:` and `pcid:` lines give the microcode revision and whether
  PCIDs are on ([PCIDs](#the-machine)).
- The safe mode entry (Developer, `nousb`) starts no USB drivers; input then comes only
  from the serial port.
- **`reboot -f` hangs.** It ends with the kernel's lines on the screen
  (it takes the screen back from the console and redraws the log, init's
  last lines included), one per reset method, each a second after the
  last: `reboot: trying the ACPI reset register (io 0xcf9 = 0x6)` (this
  PC's FADT: the PCH's hard reset), `reboot: trying 0xCF9's full reset`
  (a power cycle), `reboot: trying the 8042 ...`, `reboot: trying a triple
  fault ...`. What the screen shows tells where it stopped. Still the
  shell's `rebooting through the firmware...` and nothing more: init is
  stuck (its sync, the log or devmgr's stop); within 60 s the shell gives
  up and resets by itself, and the kernel's redrawn log then shows init's
  last line. A `trying` line that stays: that method was written and the
  CPU stopped, but the board never came back (the firmware hung after the
  reset); photograph it. Black or the board's logo and no boot menu: the
  same, after the screen was reset. The last line `trying a triple fault
  ... hold the power button`: no method reset the board. To try the full
  reset first, add the boot word `reset=cf9` (E in Limine's menu, as for
  `smp=loader` above; a `reboot` keeps it).
