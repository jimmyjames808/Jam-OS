# The real PC

Jam OS is developed against one machine, and every milestone is checked on
it. This page is the one place its facts are written down; other docs link
here. The facts come from Jam OS's own boot log and device listings on that
PC (the Devices and usb-bus runs of 2026-09-29, and the boot logs saved on
the stick on 2026-10-01).

## The machine

| Part | What it is |
|---|---|
| Board | ASUS TUF GAMING B760-PLUS WIFI |
| CPU | Intel Core i7-14700 (not F), Raptor Lake: family 6, model 0xB7, stepping 1, microcode 0x11F. 8 P-cores with Hyper-Threading + 12 E-cores = **28 CPUs** in x2APIC mode (xAPIC mode only reached 20). TSC 2112 MHz (invariant), TSC-deadline timer, PCIDs + INVPCID, SMEP/SMAP/UMIP, protection keys (PKU and PKS), WAITPKG, CET; no user interrupts. XSAVE with xcr0 = 7 (832-byte state) |
| Topology | cpus 0-15: the P-cores, two threads each (cpu 0 and 1 are core 0, 2 and 3 are core 4, ... 14 and 15 are core 28); cpus 16-27: the E-cores, one thread each (cores 32-43) |
| Memory | 32 GB (Jam OS manages 32049 MiB) |
| Display | NVIDIA RTX 4080 SUPER (01:00.0, 10de:2702), monitor on it. UEFI GOP framebuffer 2560x1440, 32 bpp, at physical 0x4000000000 (256 GiB: Resizable BAR on). No iGPU PCI function: the UHD 770 is disabled in firmware |
| PCI | 24 functions on 6 buses |
| Serial | a working COM1 UART, but no cable; logs come off the PC as files on the stick (each boot's log in `/data/logs/`) and as photos of the screen |

**PCIDs.** Alder Lake and Raptor Lake have an erratum (Intel ADL063,
RPL042): with PCIDs on, INVLPG may leave global TLB entries behind. Intel's
microcode fixes it from revision 0x118 on this model, and the PC has
0x11F, so PCIDs stay on here. The boot log's `cpu id:`, `cpu bits:` and
`pcid:` lines say so on every boot
([ARCHITECTURE.md](../ARCHITECTURE.md#memory) has the rule).

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
| 05:00.0 | Realtek RTL8125 2.5 GbE, 10ec:8125 rev 05 | MSI (1, maskable) and MSI-X (32; table at BAR4+0x0, PBA at BAR4+0x800). BAR0 I/O 0x3000, BAR2 mem64 64 KiB (the registers), BAR4 mem64 16 KiB (MSI-X only), so the registers never share a page with the MSI-X table. An Ethernet cable is ready: this is M9's NIC. Whether the switch port is a trunk (Jam OS tags VLAN 21) or an access port on 21 (the switch tags) is not known yet: ask before the network milestone |
| 00:1f.3 | Intel Raptor Lake PCH HD Audio, 8086:7a50 rev 11, class 04 03 00 (HDA mode, not the audio DSP's). MSI (1, 64-bit), no MSI-X. BAR0 mem64 16 KiB (the HDA registers), BAR4 mem64 1 MiB (the DSP's, unused) | drv/hda. Codec 0 is a Realtek ALC897 (10ec:0897, subsystem 1043:8841); the front-panel headphone jack is its pin 1b, fed by DAC 02 through mixer 0c. Codec address 2 is reported but never answers (likely the disabled iGPU's HDMI codec). The whole graph: `hda` in the shell, `[hda]` lines in the boot log, and [A1-PLAN.md](A1-PLAN.md#the-hardware) |
| 01:00.1 | NVIDIA HD Audio, 10de:22bb | HDMI/DP audio on the RTX: no driver, not planned |
| 00:14.3 | Intel Wi-Fi (the board lists an AX201), 8086:7a70 (MSI-X 16) | not planned |
| 00:0e.0 | Intel VMD/RAID, 8086:a77f | its 64-bit BAR has a hard-wired-zero upper half (see [HISTORY.md](HISTORY.md#m6-pci-msi-devmgr-drivers-through-handles)) |
| 02:00.0 | Crucial NVMe, c0a9:5421 (MSI 8, MSI-X 9) | not used: storage is the USB stick; no driver planned |
| | SATA, SMBus, I2C/SPI functions, 6 bridges | |

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

`tools/flash-usb.sh` finds the one external disk with Jam OS's two
partitions, mounts its ESP, copies the kernel, the bootfs and `limine.conf`,
compares all three, and ejects the stick. It asks for your password: macOS
does not mount an MBR partition of type 0xEF by itself, so the script
mounts it by hand with `sudo`. It refuses a disk whose first partition
holds no Jam OS kernel. The files are copied in place, so pulling the
stick in the middle of a flash can leave it unbootable: run `make flash`
again. (A stick made before M8 has one partition that macOS mounts as
`NO NAME`: copy the three files there by hand, or remake it with
`make usb`.)

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

- **A panic.** No panic screen: the screen turns the splash background,
  the PC boots again without the firmware (the splash, then the shell),
  and the shell's first line says what the panic was and where its log
  went (`the last boot panicked: ... (saved as
  /data/logs/boot-NNNN-crash.txt)`), or why it was not saved. Read that
  file on the Mac: the last 64 KiB of the log, the panic with its
  registers, backtrace and note line (a kernel test's loop, seed and test,
  when tests were running; [TESTING.md](TESTING.md#soak) says how to
  replay it). The boot's own `boot-NNNN.txt` stops up to a quarter of a
  second before the panic. The red panic screen only stays up (photograph
  it) when there is no stored kernel to start, or for a second panic
  within 30 s of the boot that followed a panic (a crash loop).
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
  of it: boot `Jam OS (text log, no splash)` (`verbose`) to see the log
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
- The safe mode entry (`nousb`) starts no USB drivers; input then comes only
  from the serial port.
