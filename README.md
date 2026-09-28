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

The boot menu has **All tests** (`ktest`: every in-kernel test), a
**Benchmark** (`bench`: latency and throughput of the kernel's basic operations,
median and p99, method explained in `kernel/test/bench.c`), a **10-minute stress test**
and a **Tests** folder with the self-test, the
timer fallback (`nodeadline`), the memory map (`memmap`) and deliberate
crashes that must panic (`testpf`, `testro`, `testrohhdm`, `teststack`,
`testlockorder`, `testlocknest`, `testlockirq`, `testmutexorder`,
`testmutexspin`, `teststuck`, `testwatchdog`, `testpanic`; `testbp` must
continue). To run any kernel command line headless and get the log and a
screenshot:

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

## Layout

```
kernel/boot/        loader glue (only place that knows about Limine)
kernel/arch/x86_64/ entry and CPU-specific code
kernel/acpi/        static ACPI tables (MADT, FADT, HPET, MCFG)
kernel/core/        kmain, klog, panic, symbols, scheduler, locks + lock
                    checker, self-tests, stress test
kernel/object/      kernel objects, handles, channels, ports, events, timers,
                    VMOs, dma_cap
kernel/abi/         handle-level sys_ API (the future system calls)
kernel/test/        in-kernel tests (KTEST) and race regression tests
kernel/mm/          physical pages, page tables, heap
kernel/dev/         framebuffer console, serial, font
kernel/lib/         string, kprintf
kernel/include/jam/ kernel headers
boot/limine.conf    boot menu
tools/              image builder, font converter, USB writer
third_party/        Limine (BSD-2), limine.h (0BSD), Spleen font (BSD-2)
```
