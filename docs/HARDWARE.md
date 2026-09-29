# The real PC

Jam OS is developed against one machine, and every milestone is checked on
it. This page is the one place its facts are written down; other docs link
here. The facts come from Jam OS's own boot log and device listings on that
PC (the Devices run of 2026-09-29 and the usb-bus run of the same day).

## The machine

| Part | What it is |
|---|---|
| Board | ASUS TUF GAMING B760-PLUS WIFI |
| CPU | Intel Core i7-14700 (not F): 8 P-cores with Hyper-Threading + 12 E-cores = **28 CPUs** in x2APIC mode (xAPIC mode only reached 20); TSC 2112 MHz, TSC-deadline timer, PCIDs + INVPCID, SMEP/SMAP/UMIP; XSAVE with xcr0 = 7 (832-byte state) |
| Memory | 32 GB |
| Display | NVIDIA RTX 4080 SUPER (01:00.0, 10de:2702), monitor on it. UEFI GOP framebuffer 2560x1440 at physical 0x4000000000 (256 GiB: Resizable BAR on). No iGPU PCI function: the UHD 770 is disabled in firmware |
| PCI | 24 functions on 6 buses |
| Serial | a working COM1 UART, but no cable; logs come off the PC by photo today, later from the stick and over the network |

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
| 9.1 | 058f:6387 HS | **the boot stick** (mass storage), behind the hub |
| 10 | 0c45:652f FS | **the keyboard** (Microdia): if0 boot keyboard, if1 boot mouse |
| 11 | 258a:0033 FS | **the mouse** (Sino Wealth "Wired Gaming Mouse"): if0 mouse, if1 keyboard |
| 24 | 174c:3074 SS | ASMedia hub, USB 3 half |

So the keyboard is on a root port (it needs no Transaction Translator), and
storage talks to the stick through the high-speed hub. The shell's hid
process for the keyboard is `hid-10:0`.

## Other devices

| Function | Device | Notes |
|---|---|---|
| 05:00.0 | Realtek RTL8125 2.5 GbE, 10ec:8125 rev 05 | MSI (1, maskable) and MSI-X (32; table at BAR4+0x0, PBA at BAR4+0x800). BAR0 I/O 0x3000, BAR2 mem64 64 KiB (the registers), BAR4 mem64 16 KiB (MSI-X only), so the registers never share a page with the MSI-X table. An Ethernet cable is ready. Whether the switch port is a trunk (Jam OS tags VLAN 21) or an access port on 21 (the switch tags) is not known yet: ask before the network milestone |
| 00:1f.3 | Intel HD Audio, 8086:7a50 (MSI) | front-panel headphone jack via a Realtek codec (ALC897 class; confirm its id on the PC) |
| 01:00.1 | NVIDIA HD Audio, 10de:22bb | HDMI/DP audio on the RTX |
| 00:14.3 | Intel Wi-Fi (the board lists an AX201), 8086:7a70 (MSI-X 16) | not planned |
| 00:0e.0 | Intel VMD/RAID, 8086:a77f | its 64-bit BAR has a hard-wired-zero upper half (see [HISTORY.md](HISTORY.md#m6-pci-msi-devmgr-drivers-through-handles)) |
| 02:00.0 | Crucial NVMe, c0a9:5421 | no driver planned |
| | SATA, SMBus, I2C/SPI functions, 6 bridges | |

## Flash and boot the stick

Only the owner does this: agents never write to a USB disk.

**A fresh stick** (erases the whole disk):

```sh
diskutil list external           # find N
make usb DEV=/dev/diskN
```

`tools/write-usb.sh` refuses internal and non-removable disks, shows the
disk, and asks you to type YES before it erases it. Check the disk number
twice anyway.

**Updating a stick that already boots Jam OS** (the usual way, nothing is
erased): with the stick mounted as `NO NAME`,

```sh
make image
cp build/jamos.elf build/bootfs.img "/Volumes/NO NAME/boot/"
cp boot/limine.conf "/Volumes/NO NAME/boot/limine/"
cmp build/jamos.elf "/Volumes/NO NAME/boot/jamos.elf"   # and the other two
diskutil eject "/Volumes/NO NAME"
```

Then boot the PC from the stick in UEFI mode with Secure Boot off, and pick
an entry from the boot menu ([TESTING.md](TESTING.md#the-boot-menu)). The
kernel ends every run with a RESULTS box; the owner reads lines from it or
sends a photo.

## If something goes wrong on the PC

- A hang during boot: the last log line names the step (`pci:` lines name
  the function being sized).
- Anything that looks like memory corruption: boot with `nopcid` first
  (QEMU's TCG has no PCIDs, so the PC is the only place they run).
- The safe mode entry (`nousb`) starts no USB drivers; input then comes only
  from the serial port.
