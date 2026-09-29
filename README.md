# Jam OS

A capability-based x86_64 operating system in C that boots on real PCs.
Monolithic now, microkernel later. See [ARCHITECTURE.md](ARCHITECTURE.md).

## Build (macOS)

```sh
brew install x86_64-elf-gcc qemu mtools
make            # the kernel, the user programs and build/bootfs.img
make image      # build/jamos.img: FAT32 USB image with Limine (UEFI)
make run        # boot it in QEMU: q35 + OVMF + USB boot over xHCI
make debug      # same, paused for gdb on :1234
make check      # generated code current, driver isolation check still rejects what it must
make KTESTS=0   # a kernel without the in-kernel tests (build/noktests/)
make syscalls   # regenerate the system call glue after editing abi/syscalls.def
make idl        # regenerate drivers/include/idl/*.h after editing abi/idl/*.idl
make compdb     # compile_commands.json for VS Code / clangd
```

User programs (`user/services/`, `user/apps/`, `user/tests/`: every
directory there is `bin/<name>`) are built with the same cross compiler as static
ELFs at 0x400000 and packed with `boot/init.cfg` into `build/bootfs.img`,
which Limine loads as a module (`module_path` on every boot menu entry).
The system call numbers, the kernel dispatch table and the user wrappers
are generated from `abi/syscalls.def` by `tools/gensyscalls.py`; the output
is committed, and every build fails if it doesn't match the `.def`.

The boot menu has **Jam OS** (a plain boot: init starts the
console, devmgr with the USB drivers and the shell: you type at the `jam>`
prompt with a real USB keyboard; `help` lists the commands), **Jam OS (safe
mode)** (`nousb`: no USB drivers, serial input only) and a **Tests** folder
with what must run without a keyboard: **All tests** (`ktest`: every
in-kernel test), the **2-minute** (after each fix) and **10-minute**
(milestone sign-off) stress tests, the **Benchmark** (`bench`), **init +
utest + usbtest** (the user-space regression run, RESULTS box with `utest: N
passed` and whether init's root job ended clean) and the timer fallback
(`nodeadline selftest`). Everything else is a shell command: `ktest`,
`bench`, `stress`, `utest`, `usbtest`, `devices`/`lspci`, `usb`/`lsusb`,
`pci`, `memmap`, `demo`, `crash <name> yes` (the deliberate panics), `top`,
`ps`, `date`, the text tools with pipes, and the apps `run fractal`,
`run tetris`, `run life`. Hidden boot words for the QEMU tests:
`pcilist`, `keytest`, `memmap`, `selftest`, `test<name>` (a crash test).

```sh
tools/qemu-test.sh build/test kt ktest                  # all ktests
tools/qemu-test.sh build/test chan ktest=chan           # tests starting "chan"
QEMU_SMP=8 QEMU_TIMEOUT=60 tools/qemu-test.sh build/test st ktest stress=30
QEMU_SMP=20 tools/qemu-test.sh build/test smp20 selftest   # like the real PC
```
`QEMU_MEM`, `QEMU_SMP`, `QEMU_CPU` (e.g. `max,-x2apic`) change the machine;
`QEMU_TIMEOUT` (seconds, default 30) is how long to wait for it to finish;
`QEMU_IMAGE` boots another image. `make KTESTS=0 image` builds a kernel
without the in-kernel tests into `build/noktests/`.

## Boot on a real PC

```sh
diskutil list external
make usb DEV=/dev/diskN
```

`tools/write-usb.sh` refuses internal disks and asks before erasing. Then boot
the PC from the stick in UEFI mode with Secure Boot off.

## Where things live

```
kernel/main.c        the boot sequence (kmain), then the tests or user space
kernel/boot/         loader glue (the only place that knows about Limine)
kernel/arch/x86_64/  entry, interrupts, syscall/sysret, user entry, CPUs,
                     LAPIC/IOAPIC, TSC, FPU, PCIDs, IPIs and TLB shootdown
kernel/acpi/         static ACPI tables (MADT, FADT, HPET, MCFG)
kernel/mm/           physical pages, page tables, heap, address spaces
kernel/sched/        scheduler (run queues, placement, the switch), threads
                     and the stack cache, waits, wait queues, mutexes
kernel/object/       kernel objects and handles: channels, ports, events,
                     timers, VMOs, VMARs, processes, jobs, interrupts,
                     resources, dma_cap
kernel/abi/          the handle-level sys_* API and the syscalls (sysc_*.c,
                     the generated dispatch table)
kernel/proc/         starting programs: bootfs, the ELF parser, userboot
                     (starts init)
kernel/dev/          the kernel's own devices: framebuffer console, serial,
                     font, RTC, PCI core (+ MSI, reports), reboot
kernel/debug/        klog, the panic screen, symbols, spinlocks + the lock
                     checker, the RESULTS box, debug_command (shell tests),
                     self-tests, crash tests and the stress test (these ship
                     in every kernel)
kernel/test/         in-kernel tests (KTEST, by subject) and the benchmark;
                     left out by `make KTESTS=0`
kernel/lib/          string, kprintf, the command line
kernel/include/jam/  kernel headers (the ones user code may see are copied
                     for it: UINC_HDRS in the Makefile)
drivers/usb-bus/     xHCI + hubs: serves one channel per USB interface
drivers/hid/         USB HID boot keyboard and mouse -> the console
drivers/test/        test drivers: null, drvtest, edu (QEMU), crasher
drivers/include/     <jam/driver.h> (all a driver may use), generated <idl/*.h>
user/lib/            libos: crt0, syscall wrappers, printf, heap, startup
                     message, bootfs reader, spawn(), threads, driver.h's
                     implementation
user/include/        libos and service headers (<os.h>, <devmgr.h>, ...)
user/services/       init, console, devmgr, serialin, shell (shell/cmd/: one
                     file per command)
user/apps/           fractal, life, tetris, demo, and fun/ (libfun: the
                     apps' screen, drawing, keys and thread pool)
user/tests/          utest (the user-space suite), usbtest, contest
abi/syscalls.def     the system call table
abi/idl/             the protocols (IDL)
boot/limine.conf     boot menu
boot/init.cfg        what init starts in the regression run (packed into bootfs)
tools/               image, bootfs, syscall, IDL and symbol generators, the
                     driver check, QEMU test scripts (qemu-test.sh,
                     shell-tests/, usb-test.sh, ...), font converter, USB writer
third_party/         Limine (BSD-2), limine.h (0BSD), Spleen font (BSD-2)
```

How the code is written: [CODING-GUIDE.md](CODING-GUIDE.md).
