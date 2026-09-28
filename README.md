# Jam OS

A capability-based x86_64 operating system in C that boots on real PCs.
Monolithic now, microkernel later. See [ARCHITECTURE.md](ARCHITECTURE.md).

## Build (macOS)

```sh
brew install x86_64-elf-gcc qemu mtools
make image      # build/jamos.img: FAT32 USB image with Limine (UEFI)
make run        # boot it in QEMU: q35 + OVMF + USB boot over xHCI
make debug      # same, paused for gdb on :1234
```

The boot menu's **Tests** folder runs the memory self-test and deliberate
crashes (NULL write, write to kernel code, stack overflow, panic). To run one
headless and get the log and a screenshot:

```sh
tools/qemu-test.sh build/test selftest selftest
```

## Boot on a real PC

```sh
diskutil list external
make usb DEV=/dev/diskN
```

`tools/write-usb.sh` refuses internal disks and asks before erasing. Then boot
the PC from the stick in UEFI mode with Secure Boot off.

## Layout

```
kernel/boot/        loader glue (only place that knows about Limine)
kernel/arch/x86_64/ entry and CPU-specific code
kernel/core/        kmain, klog, panic, symbols, self-tests
kernel/mm/          physical pages, page tables, heap
kernel/dev/         framebuffer console, serial, font
kernel/lib/         string, kprintf
kernel/include/jam/ kernel headers
boot/limine.conf    boot menu
tools/              image builder, font converter, USB writer
third_party/        Limine (BSD-2), limine.h (0BSD), Spleen font (BSD-2)
```
