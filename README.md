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
make syscalls   # regenerate the system call glue after editing abi/syscalls.def
```

User programs (`user/`) are built with the same cross compiler as static
ELFs at 0x400000 and packed with `boot/init.cfg` into `build/bootfs.img`,
which Limine loads as a module (`module_path` on every boot menu entry).
The system call numbers, the kernel dispatch table and the user wrappers
are generated from `abi/syscalls.def` by `tools/gensyscalls.py`; the output
is committed, and every build fails if it doesn't match the `.def`.

The plain **Jam OS** entry (empty command line) and **Jam OS (init + utest)**
(`init`) start user space: the kernel's userboot loads `bin/init` from bootfs,
init runs each program in `boot/init.cfg` as a child process (today
`bin/utest`, the M5 test suite in ring 3), and the RESULTS box shows
`utest: N passed`, init's exit code and whether its root job ended with
nothing charged. The boot menu also has **All tests** (`ktest`: every
in-kernel test), a
**Benchmark** (`bench`: latency and throughput of the kernel's basic operations,
then the same measured from ring 3 (`user:` lines: syscalls, page faults,
process-to-process calls); median and p99, method explained in
`kernel/test/bench.c`), a **2-minute stress test** (after each fix) and a **10-minute** one (milestone sign-off) (kernel threads plus user
processes started and killed at random moments)
and a **Tests** folder with the self-test, the
timer fallback (`nodeadline`), the memory map (`memmap`) and deliberate
crashes that must panic (`testpf`, `testro`, `testrohhdm`, `teststack`, `testsmap`, `testsmep`,
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
                    checker, self-tests, stress test, bootfs, ELF parser,
                    userboot (starts init)
kernel/object/      kernel objects, handles, channels, ports, events, timers,
                    VMOs, dma_cap, vmars, processes + threads, jobs
kernel/abi/         handle-level sys_ API, the syscalls (sysc_*.c), the
                    generated syscall table
kernel/test/        in-kernel tests (KTEST) and race regression tests
kernel/mm/          physical pages, page tables, heap
kernel/dev/         framebuffer console, serial, font
kernel/lib/         string, kprintf
kernel/include/jam/ kernel headers
user/               user programs: libos (crt0, syscall wrappers, printf,
                    heap, startup message, bootfs reader, ELF loader
                    spawn(), threads), init, utest (the M5 suite; its
                    child and benchmark modes are in user/utest/)
abi/syscalls.def    the system call table
boot/limine.conf    boot menu
boot/init.cfg       what init starts (packed into bootfs)
tools/              image, bootfs and syscall generators, font converter,
                    USB writer
third_party/        Limine (BSD-2), limine.h (0BSD), Spleen font (BSD-2)
```
