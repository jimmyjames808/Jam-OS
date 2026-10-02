<h1 align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/logo/jamos-lockup-dark.png">
    <img alt="Jam OS" src="docs/logo/jamos-lockup-light.png" width="420">
  </picture>
</h1>

Jam OS is a from-scratch operating system for x86_64 PCs, written in C. It
is capability-based: a program can do only what the handles it holds
allow, and every driver and service runs as a separate user process that
the kernel supervises through those handles. Until the IOMMU work
(planned), that keeps a crashed driver from taking the system down, not a
faulty driver's device from writing memory. It boots from a USB stick on
a real desktop PC, which is where every milestone is tested.

![The Jam OS shell in QEMU: uname, date, free, the services under /svc, mount and ps](docs/images/shell-m86.png)

## What works today

- UEFI boot from a USB stick (via Limine) on a real PC with 28 CPUs, and in
  QEMU.
- A preemptive SMP kernel: per-CPU scheduling for hybrid P/E-core CPUs,
  virtual memory, a lock-order checker, a watchdog and a panic screen with
  a symbolised backtrace.
- Capability handles, channels and ports; processes, threads and jobs with
  quotas on every kernel resource a process can use.
- Drivers as user processes: a PCI core with MSI/MSI-X and DMA
  capabilities, a device manager that restarts crashed drivers, and USB
  (xHCI controller, hubs, keyboard and mouse).
- Storage: USB sticks through a usb-storage driver and a FAT32 service
  (FatFs) per volume. The stick Jam OS boots from has its boot partition
  at `/esp`, read-only, and a data partition at `/data`; any other stick
  shows up read-only at `/usb0`, `/usb1`, ... and `mount -w` makes it
  writable. Each boot's kernel log is saved to `/data/logs/` and can be
  read on another computer.
- A console and a shell with about 80 commands, pipes, variables, Tab
  completion and file commands (`ls cat cp mv rm mkdir write df mount`),
  plus a few apps: a Mandelbrot explorer, life, tetris, snake, minesweeper
  played with the mouse, and a graphical system monitor.
- Kernel and user-space test suites, a stress test, a soak test (the
  kernel tests repeated in shuffled order under load, with sticks pulled
  and plugged) and a benchmark, runnable from the boot menu or the shell.
- Sound: `beep`, and `play /data/song.wav` plays a PCM WAV
  file (8- to 32-bit, mono or stereo, any common rate) in the headphones;
  `play /data/song.mp3` plays an MP3 (CBR or VBR, ID3 tags skipped, decoded
  by dr_mp3) copied straight from the Mac; a mixer service plays several
  programs at once, `vol` sets their volumes; `music start` plays a folder
  of MP3s and WAVs in shuffle in the background while the shell goes on,
  and `jamjar` is the same player in a window: the library by artist and
  album, search, the controls with keys and the mouse, a sleep timer, and
  the albums' own covers (read from the MP3s' tags), stereo spectrum bars
  across the bottom of the screen (left up, right down) and a sunburst in
  the full-screen view.
- The real date and time: the PC's real-time clock read at boot, kept in
  UTC by the kernel and shown in the owner's time zone (Sydney, with
  daylight time; `date -z` changes it); files on the sticks and the boot
  logs are dated. Settings that survive a reboot live in
  `/data/etc/settings` (the zone, the volumes, the music folder).
- `kernel load` loads a freshly flashed kernel from the stick while Jam OS
  runs, so the next `reboot` is instant.
- A boot splash: the logo animation with its sound while Jam OS starts
  (it plays to the end, and what is typed meanwhile reaches the shell; the
  `verbose` boot entry shows the text log instead).
  After it the shell's screen holds the shell alone: the kernel log stays
  in `log` and `dmesg`, and only a few notices reach the screen (a stick
  plugged in or pulled out, a service that crashed, `/data` full).
- Each program gets only what it asks for: a list in its own file (the
  services under `/svc` and the mounts it wants, read-only or writable).
  A program copied to `/data` runs once the owner has said yes to its list
  with `allow`.

Not yet: networking, power management. Status and plans:
[docs/ROADMAP.md](docs/ROADMAP.md).

## Build and run in QEMU

On macOS, with [Homebrew](https://brew.sh):

```sh
brew install x86_64-elf-gcc qemu mtools   # the OVMF UEFI firmware comes with qemu
make run                                  # build, then boot it in QEMU
```

`make run` boots the image in QEMU (q35, OVMF, the stick on a USB xHCI
controller, a USB keyboard) with the serial console on your terminal. Type
at the `jam>` prompt; `help` lists the commands. Python 3 is needed for
the build tools (and Pillow for test screenshots).

| Command | What it does |
|---|---|
| `make` | the kernel, the user programs and the boot filesystem image (not the disk image) |
| `make image` | `build/jamos.img`: the USB image, a FAT32 boot partition with Limine (UEFI) and a FAT32 data partition |
| `make run` | build the image and boot it in QEMU |
| `make debug` | the same, stopped for gdb on :1234 |
| `make usb DEV=/dev/diskN` | write the image to a USB stick, erasing it ([below](#boot-a-real-pc)) |
| `make flash` | update a stick that already has Jam OS: kernel, boot image and boot menu only |
| `make check` | generated code current, the driver isolation check, the docs check, the include order |
| `make includes` | put `#include` lines in the order the check wants |
| `make KTESTS=0` | a kernel without the in-kernel tests (into `build/noktests/`) |
| `make syscalls` | regenerate the syscall glue after editing `abi/syscalls.def` |
| `make idl` | regenerate `drivers/include/idl/` after editing `abi/idl/` |
| `make compdb` | `compile_commands.json` for editors |
| `make clean` | remove `build/` |

Testing (the tiers, `tools/qemu-test.sh`, the shell scripts, the boot menu)
is in [docs/TESTING.md](docs/TESTING.md).

## Boot a real PC

> **Warning:** `make usb` erases the whole disk you give it. It refuses
> internal disks and asks before writing, but check the disk number twice.

Write the image with `make usb DEV=/dev/diskN` (find N with
`diskutil list external`), then boot the PC from the stick in UEFI mode
with Secure Boot off. After that, `make flash` updates the stick in place
and leaves `/data` alone; it asks for your password, because macOS does
not mount the stick's boot partition by itself. The details, and the PC
Jam OS is built for, are in [docs/HARDWARE.md](docs/HARDWARE.md).

## Where things live

| Path | What |
|---|---|
| `kernel/main.c` | the boot sequence, then the tests or user space |
| `kernel/boot/` | loader glue: Limine's (the only code that knows about it), and a kexec'd kernel's handoff |
| `kernel/kexec/` | kexec: the reserved region, the stored kernel loaded into it, the jump after a reboot or a panic, the next boot's side (the crash record, the panicked boot's log) |
| `kernel/arch/x86_64/` | entry, interrupts, syscalls, CPUs, APIC, TSC, FPU, PCIDs, IPIs |
| `kernel/acpi/` | static ACPI tables (MADT, FADT, HPET, MCFG) |
| `kernel/mm/` | physical pages, page tables, heap, address spaces |
| `kernel/sched/` | scheduler, threads, waits, mutexes |
| `kernel/object/` | kernel objects and handles |
| `kernel/abi/` | the handle-level API and the syscalls |
| `kernel/proc/` | bootfs, the ELF parser, userboot (starts init) |
| `kernel/dev/` | the kernel's own devices: framebuffer console, serial, RTC and the wall clock, PCI core, reboot |
| `kernel/debug/` | klog, panic, symbols, lock checker, RESULTS box, self-, crash and stress tests |
| `kernel/test/` | in-kernel tests and the benchmark |
| `kernel/include/jam/` | kernel headers |
| `drivers/` | `usb-bus/` (xHCI + hubs), `hid/` (keyboard, mouse), `usb-storage/` (USB sticks: partitions as `block` channels), `hda/` (Intel HD Audio: codec path, one output stream, `beep`), `e1000e/` (QEMU's Intel 82574L network card, for the network tests), `lib/` (code several drivers link: the netdev server, `netserver.c`), `test/` (test drivers), `include/` (`<jam/driver.h>`, `<jam/task.h>`, generated IDL headers) |
| `user/lib/` | libos: startup, syscall wrappers, printf, heap, spawn, the file namespace and `/svc`, a program's list (`<wants.h>`), the driver API, cooperative tasks (`<jam/task.h>`), sound output (`<audio.h>`), WAV headers (`<wav.h>`) and MP3 decoding (`<mp3.h>`, on dr_mp3), settings (`<settings.h>`), the calendar and time zones (`<wallclock.h>`), UTF-8, SHA-256 and IPv4 addresses as text (`<ipv4.h>`) |
| `user/services/` | init, console, devmgr, serialin, shell, bootfs (the boot image as `/boot`), fat (the FAT filesystem, on FatFs), logd (the boot log files), mixer (every program's sound into the one output), music (the background music player), netstack (the network stack, on lwIP, on the network card's rings) |
| `user/apps/` | fractal, life, tetris, snake, mines, sysmon, jamjar (the music player's window), demo, splash (the boot splash), play (the shell's `play`: one file decoded and played), jamcover (jamjar's cover decoder), and `fun/` (the apps library) |
| `user/tests/` | utest, usbtest, hdatest (the HD Audio stream's checks), mixtest (the mixer's checks), nettest (a network driver as a hostile netstack sees it), contest, ramfs (a RAM filesystem for the file tests), soakload (the soak test's user-space load), wantdebug (a list asking for `right debug`, for the allow test) |
| `abi/` | `syscalls.def` (the syscall table) and `idl/` (the protocols) |
| `boot/` | `limine.conf` (the boot menu), `init.cfg` (the regression run) |
| `tools/` | image, bootfs, syscall, IDL and symbol generators; checks; QEMU test scripts; the USB writer and `make flash`'s updater |
| `third_party/` | Limine and `limine.h`, the Spleen font, FatFs, dr_mp3, pl_mpeg (the splash's MPEG-1 decoder), stb_image (jamjar's album covers), lwIP (netstack's IPv4, ARP, ICMP and UDP) |
| `docs/` | the documentation below; `docs/logo/`, the logo |

## Documentation

| Doc | For |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | how the system is designed, and why |
| [CODING-GUIDE.md](CODING-GUIDE.md) | how the code is written and changed |
| [CONTRIBUTING.md](CONTRIBUTING.md) | issues, pull requests and what a change needs; [SECURITY.md](SECURITY.md) for reporting a security problem |
| [docs/ROADMAP.md](docs/ROADMAP.md) | milestones: done, next, later |
| [docs/M9-PLAN.md](docs/M9-PLAN.md) | the plan of the milestone under way (networking) |
| [docs/HISTORY.md](docs/HISTORY.md) | what each milestone delivered, bugs and lessons, decisions |
| [docs/TESTING.md](docs/TESTING.md) | test tiers and exact commands |
| [docs/HARDWARE.md](docs/HARDWARE.md) | the real PC, and flashing the stick |
| [docs/BENCH.md](docs/BENCH.md) | benchmark numbers from the PC |
| [docs/logo/README.md](docs/logo/README.md) | the logo's files and colours |

## Contributing

Jam OS is one person's project, written with the help of AI. The rules for
changing the code are in [CODING-GUIDE.md](CODING-GUIDE.md).

## Licence

Jam OS is released under the [BSD 2-Clause License](LICENSE). The
third-party code in `third_party/` keeps its own licences (Limine: BSD-2-Clause; `limine.h`: 0BSD; Spleen: BSD-2-Clause; FatFs: its own one-clause BSD-style licence; dr_mp3: public domain or MIT-0; pl_mpeg: MIT; stb_image: MIT or public domain; lwIP: BSD-3-Clause).
