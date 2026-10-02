# Jam OS coding guide

The rules for writing and changing Jam OS code.
[ARCHITECTURE.md](ARCHITECTURE.md) says *what* the system is; this
file says *how the code is written*; [docs/TESTING.md](docs/TESTING.md)
says how to test it. Where old code breaks a rule, new code follows the rule.
Fix old code only when you are already changing it for another reason, or
in a separate cleanup commit.

House style comes from the files the reviews called good:
`kernel/object/port.c` (+ `port.h`), `kernel/mm/pmm.c`,
`drivers/hid/keyboard.c`, `user/services/devmgr/` (split by job) and
`user/services/shell/sh.h`. When this guide doesn't cover something, copy them.

Write the code to be read: someone learning OS development should be able
to follow it.

---

## 1. Principles

**Authority only through handles.** A process can do only what its handles
allow. Never decide "who may do this" by process name, pid, or a global
flag. If a feature needs new authority, it gets a new handle type or a new
right.
*Why:* one mechanism is auditable; name checks and flags can be spoofed or
forgotten. (devmgr's query vs control channels are two handles, not one
handle plus an "is admin" check.)

**Least rights.** Hand out the narrowest handle that works:
duplicate with fewer rights (`jam_handle_duplicate(h, rights, &out)`), and
send hardware handles with `channel_write_rights` so they arrive without
`RIGHT_DUPLICATE`/`RIGHT_TRANSFER`.
*Why:* a crashed or compromised process can only misuse what it holds.

**Mechanism in the kernel, policy in processes.** Before adding kernel
code, apply Liedtke's test: does it *have* to be in the kernel for the
system to work? If a process could do it with the right handles, it goes in
a process. Restart backoff (devmgr), keyboard layouts (hid), the driver
match table (devmgr), the command language (shell) are policy.
*Why:* kernel bugs take the machine down; process bugs get restarted.

**What stays in the kernel is enforcement.** Objects and handles,
scheduling, memory, and the hardware checks no process may bypass: the PCI
core (ECAM, BAR sizing, MSI/MSI-X, Bus Master Enable), interrupt objects,
resources, DMA pins and quarantine, the config-write filter.

**Drivers and services are processes from the start**
([the migration rule](ARCHITECTURE.md#the-migration-rule)). A driver includes only `<jam/driver.h>`, the
generated `<idl/*.h>`, `<jam/abi.h>` and `<jam/status.h>`;
`tools/checkdriver.py` fails the build otherwise. A driver keeps no state
across a restart: a restart is a bind from scratch.

**Fail closed.** When unsure, refuse. Unknown flags, non-zero reserved
fields and out-of-range values are `ERR_INVALID_ARGS`, never "ignored".
New defaults are the safe ones: a new `dma_cap` turns bus mastering off;
with no VLAN configured the NIC stays down.
*Why:* an ignored flag becomes a silent ABI promise; a permissive default
is a hole nobody notices.

**Every resource user code can grow is bounded and charged.** Any kernel
memory a process can make the kernel hold (messages, packets, bindings,
page tables, objects) is charged to a job *before* it is allocated, and
every per-object queue or table has a cap (`PORT_MAX_BINDINGS`, 1024
messages per channel endpoint). Over the cap: `ERR_NO_RESOURCES` or
`ERR_SHOULD_WAIT`, never a panic.
*Why:* otherwise one process can exhaust the kernel (see port.c's header).

**No panic on anything user code can cause.** `panic` is for broken
kernel invariants only. Bad input, a full table or out of memory on a
syscall path returns an error.

---

## 2. Structure

### Where code goes

| Code | Place |
|---|---|
| Boot sequence | `kernel/main.c` |
| Kernel, by subsystem | `kernel/<subsystem>/`: `arch/x86_64` (CPU, interrupts, entry), `mm` (memory), `sched` (scheduler, threads, waits, mutexes), `object` (kernel objects and handles), `abi` (syscalls), `proc` (bootfs, ELF, userboot), `dev` (the kernel's own devices, PCI core, reboot), `debug` (klog, panic, symbols, lock checker, RESULTS box, debug commands, self-, crash and stress tests), `kexec` (the stored kernel, reboot and panic by kexec), `acpi`, `boot`, `lib` |
| Kernel headers | `kernel/include/jam/<name>.h`; a subsystem's internal header next to its code (`sched/sched_internal.h`, `dev/pci_internal.h`) |
| Syscall glue (`sysc_*`, `sys_*`) | `kernel/abi/` |
| Kernel tests | `kernel/test/test_<subject>.c` (left out by `make KTESTS=0`; checks that must ship in every kernel go in `kernel/debug/`) |
| Drivers | `drivers/<name>/` (test drivers in `drivers/test/<name>/`); either way `drv/<name>` in bootfs |
| System services (bootfs, console, devmgr, fat, init, logd, mixer, music, serialin, shell) | `user/services/<name>/` |
| Apps (fractal, life, tetris, snake, mines, sysmon, jamjar, jamcover, demo, splash, play) | `user/apps/<name>/`; the apps library (libfun) is `user/apps/fun/` |
| Test programs (utest, usbtest, hdatest, mixtest, contest, ramfs, soakload) | `user/tests/<name>/` |
| Shared user code | `user/lib/` (libos, headers in `user/include/`), libfun (`<fun.h>`) |
| ABI sources | `abi/syscalls.def`, `abi/idl/*.idl` |
| Build tools and test scripts | `tools/` (QEMU shell scripts in `tools/shell-tests/`) |

The Makefile finds programs and drivers by directory: no list to edit.

### Licence and outside code

Jam OS is BSD-2-Clause ([LICENSE](LICENSE)). Everything in the tree must
be compatible with that.

- **Never copy code from GPL or LGPL projects** (Linux, GRUB, glibc, ...),
  not even a few lines. *Why:* it would force the whole project under the
  GPL. Reading them for hardware facts is fine: register names and
  offsets, bit meanings, init order, quirks. Write the code yourself and
  cite the fact's source in a comment ("init order as in Linux r8169").
  Prefer permissive references (FreeBSD, OpenBSD, the vendor datasheet).
- **Vendored code goes in `third_party/<name>/` with its own LICENSE file**
  and an entry in `third_party/VERSIONS.md`. Only permissive licences
  (BSD, MIT, ISC, 0BSD, Zlib, Apache-2.0).

### Files and functions

- **One job per file.** The file header says what that job is. A file you
  can't describe in one sentence should be split (devmgr: `main.c` event
  loop, `bind.c` starting and stopping, `supervise.c` restarts, `usb.c`
  USB interfaces).
- **Files: aim under ~600 lines.** Over 800 needs a reason (a single table,
  a generated file). *Why:* a file you can read in one sitting is a file you
  can review and understand.
- **Functions: aim for 60 lines or less; over 80 needs a reason in the
  review.** *Why:* NASA's Power of Ten uses 60 (one printed page); past that
  functions stop being one idea. Split into named steps, as usb-bus's `enumerate()`
  became a short sequence of step functions. Long data
  tables and test bodies that are a straight list of checks are exempt, but
  prefer several short tests.
- **At most three levels of nesting** inside a function, counted as
  written (a macro's own `do`/`if` doesn't count). Use early returns and
  helpers.
- **At most six parameters.** *Why:* x86-64 passes six in registers, and
  syscalls take six. More means a struct (as `@channel_call_args` does).
  Exempt: IDL-generated signatures, and the `sys_*` handle layer, which
  mirrors its syscall (it may take the syscall's `@struct` instead).
  libfun's drawing calls take a `struct rect` (a `struct picture` to
  scale from); the ones whose shape is no rectangle may take up to eight:
  `line_aa` and `ring_aa` (float end points or a centre, a width, the
  colour and its alpha) and `text2` (a position, a scale, two colours, a
  shadow or not).

### Headers

- The header comment first, then `#pragma once` (as `port.h` does): what
  the module is, its lock order, and who owns what.
- Include order: `<std*.h>` compiler headers first, then the other
  `<...>` headers (`<jam/...>`, `<idl/...>`, `<os.h>`), then the program's
  own `"..."` headers, alphabetical within each group. Don't sort by hand:
  `make includes` does it (`tools/sortincludes.py`), and `make check`
  fails when a run of `#include` lines is out of order. A blank line
  between includes starts a new run, for grouping on purpose.
- **Public** headers (`kernel/include/jam/`, `drivers/include/jam/driver.h`,
  `user/include/`) declare the API, with each function's contract on its
  declaration: what it does, which errors it returns, what context it needs
  ("interrupts on, no spinlock held").
- **Internal** headers live next to the code and are included with quotes:
  `user/services/devmgr/internal.h`, `drivers/hid/hid.h`, `kernel/abi/sysc.h`.
  Use one when several files of one component share declarations. A user
  program's own directory is on its quote include path, so files in a
  subdirectory write `"sh.h"`, not `"../sh.h"`.
- **The user-visible ABI** is only the headers the Makefile copies for user
  code (`UINC_HDRS`: abi.h, bootfs.h, startup.h, status.h, syscall_nums.h).
  User code never includes other kernel headers.
- Includes: `<jam/...>` and other angle includes sorted alphabetically,
  then local `"..."` includes. Include what you use.
- **Never `#include` a `.c` file.** Shared code goes in a library (libos,
  libfun) or a component's own `.c` with an internal header.
- **Shared helpers live once.** Before writing `now()`, a time constant, a
  string builder, a lock or a test helper, look first: kernel `jam/time.h`
  and `ktest.h`; user `os.h` (a lock between a program's threads is
  `lock_take`), libos and `check.h`; drivers `<jam/driver.h>`
  (which has its own `NS_PER_*`, since drivers can't include kernel
  headers). Copy-paste across components is a review failure.

### Formatting

- 4 spaces, no tabs. Lines up to 100 columns.
- Function braces on their own line; control-statement braces on the same
  line. A single-statement body has no braces; if one branch of an
  if/else has braces, all do.
- Declare variables where they are first set, in the smallest scope (C99).
- Align struct field comments in a column (see `struct binding`).

---

## 3. C rules

The build is `-std=gnu17 -Wall -Wextra -Werror -Wvla`, freestanding, and
the kernel adds `-Wframe-larger-than=3072`. A warning is a build failure; never silence one with a cast or a pragma without a
comment saying why it is safe.

### Types

- `uint8_t .. uint64_t` / `int8_t .. int64_t` for anything with a size that
  matters: hardware registers, ABI structs, wire formats, addresses (always
  `uint64_t`), times in ns (always `uint64_t`).
- `size_t` for in-memory lengths; `bool` for yes/no (never an int flag);
  `status_t`, `handle_t`, `rights_t`, `signals_t` for what they name.
- Plain `unsigned`/`int` only for small local counters and indices.
- No typedefs for structs (write `struct port`); typedefs only for opaque
  scalar handles like the ones above, and for small value types used like
  numbers (fractal's double-double `dd`, GCC vector types).
- `enum` for related constants that belong together (`enum bind_kind`).

### Integer overflow and truncation (CERT INT30-C, INT31-C)

- Range checks are written so they can't wrap: `off > size || len > size - off`,
  never `off + len > size` (see `sysc_vm.c`).
- Sizes computed from user values: `__builtin_add_overflow` /
  `__builtin_mul_overflow`, and fail with `ERR_OUT_OF_RANGE` or
  `ERR_INVALID_ARGS`.
- Narrowing casts only after a range check, and always written out:
  `(uint32_t)x`. Syscall arguments narrower than 64 bits are already
  truncated by the dispatcher: check the value as the kernel receives it.
- Shifts: `1ull << n` with `n < 64` checked first.
- Hardware registers read `0xffffffff` when a device is gone: treat it as
  "gone", not as data (`wait_op` in usb-bus does).

### User pointers

- In the kernel a user address is a `uint64_t`, never a C pointer. Only
  `copy_from_user`, `copy_to_user` and `copy_str_from_user` touch it.
- **Copy once, check, then use.** Never read the same user memory twice
  (double fetch): the user can change it between the reads.
- Never copy with a spinlock held or the aspace region lock held: a copy
  can fault and sleep.
- A result that can't be copied out is undone (`sysc_put_handle` closes the
  new handle) and the call fails with `ERR_INVALID_ARGS`, so a bad pointer
  never leaks a handle.

### Uninitialised memory and the ABI (CERT EXP33-C, DCL39-C)

- Kernel objects are allocated with `kzalloc`. Use `kmalloc` only when
  every field is written on the next lines.
- **ABI structs have no implicit padding.** Every hole is an explicit
  `reserved` field (the kernel requires 0 on input and writes 0 on output),
  and the size and key offsets are pinned with `_Static_assert` in
  `kernel/abi/abi_check.c`. *Why:* padding bytes would leak kernel stack or
  heap contents to user space, and an unchecked reserved field can never be
  used later.
- A struct copied to user space is zeroed first (`memset(&s, 0, sizeof(s))`),
  then filled.
- Don't use `__attribute__((packed))` on syscall structs (misaligned
  fields); IDL wire structs are packed by the generator, and that is the
  only place.

### Errors and cleanup

- A function that can fail returns `status_t`: `OK` (0) or a negative
  `ERR_*`. Results go through **out-parameters, last**, named `out` or
  `out_<thing>`, written **only on success** (`port_create` sets `*out` as
  its last step). Callers never read an out-parameter after a failure.
- The status variable is called `st`. Don't reuse the name for anything
  else.
- Check every status. To ignore one on purpose, write `(void)` and a
  comment. Exempt: closing a handle and sleeping, whose failure leaves
  nothing to do (a bare `jam_handle_close(h);` is fine).
- Pick the error that says what happened:

| Error | Use for |
|---|---|
| `ERR_INVALID_ARGS` | bad flags, bad user pointer, non-zero reserved, bad size |
| `ERR_BAD_HANDLE` / `ERR_WRONG_TYPE` / `ERR_ACCESS_DENIED` | handle missing / wrong object type / missing right |
| `ERR_NO_MEMORY` | an allocation or a page/message-byte job limit failed |
| `ERR_NO_RESOURCES` | a table, queue or handle/thread limit is full |
| `ERR_SHOULD_WAIT` | would block (non-blocking call) |
| `ERR_OUT_OF_RANGE` | a value outside what the object supports |
| `ERR_BAD_STATE` | right call, wrong moment (already started, closed) |
| `ERR_PEER_CLOSED` | the other end is gone: reconnect per the protocol |
| `ERR_TIMED_OUT` / `ERR_CANCELED` | deadline passed / thread or wait cancelled |
| `ERR_NOT_SUPPORTED` / `ERR_NOT_FOUND` | not implemented or not allowed by design / no such thing |

- **Cleanup, the house style** (decided for Jam OS; Linux prefers goto
  ladders, but this codebase reads better this way):
  1. **Early return with the undo inline** when there are one or two
     things to undo (`port_bind`, `bar_for_driver`). Undo in reverse order
     of acquisition.
  2. **One `goto out`** when a lock must be released on several exits
     (`send_msg` in channel.c). The label says what it does (`out`,
     `out_unlock`).
  3. In user code, the **`st == OK &&` chain** for a sequence of steps
     that each need the previous (`pci_handles`, `start_driver`), with
     cleanup at the end based on what was acquired.
  More than one goto label means the function should be split.

### const and static

- Pointer parameters a function doesn't write through are `const`
  (`port_queue_user(..., const struct port_packet *pkt)`).
- Lookup tables are `static const` (`row_plain`, `limits[]`, `matches[]`).
- **Everything is `static` unless a header declares it.** File-scope
  variables too. *Why:* the reader knows at once that nothing outside the
  file touches it.
- `static inline` in a header only for tiny helpers (a few lines) or
  generated code.

### Macros

- Constants and function-like macros in `UPPER_CASE`; parenthesise
  arguments; multi-statement macros in `do { } while (0)`.
- Prefer `static inline` functions and `enum` over macros.
- No hidden control flow in new macros. The accepted exceptions are
  `SYSC_TABLE` (which returns) and the test macros that end a test on
  failure: `KT_*` in the kernel, `CHECK`/`STEP` in `user/include/check.h`.

### No floating point in the kernel

The kernel builds with `-mno-sse -mno-80387`; the FPU/SSE/AVX state belongs
to user threads. Use integers: times in ns, fixed-point where you need
fractions. User programs may use floating point and SIMD freely.

### volatile, MMIO and atomics

- **`volatile` is for memory something other than this CPU's code
  writes or reads:** device memory (MMIO, DMA-shared descriptors),
  memory the boot loader fills in (the Limine request structs), and
  accesses whose point is the access itself (crash tests, TLB probes).
  It is not a synchronisation tool: it gives no atomicity and no ordering
  between CPUs.
- Drivers access registers only through `drv_read32/drv_write32` (and the
  8/64 variants) on the mapped BAR. Kernel code uses a small accessor per
  device, not scattered pointer arithmetic (lapic.c's `rd`/`wr`).
- Shared variables between CPUs use the `__atomic_*` builtins with an
  explicit order. `__ATOMIC_RELAXED` for statistics counters; the last
  refcount drop is `__ATOMIC_ACQ_REL` (`binding_put`); a flag that publishes
  data is a `RELEASE` store paired with an `ACQUIRE` load, and the comment
  names the other side. Don't use `__sync_*`.
- Counters: with one writer, `COUNTER_ADD(&c, 1)` / `COUNTER_SUB`
  (`jam/atomic.h`: an atomic load and an atomic store, no `lock` prefix on
  hot paths); with several writers, `__atomic_add_fetch`. Never mix: a
  variable another CPU reads is touched only through atomic accesses, its
  own writer's reads included. Plain stores are fine before an object is
  published to other CPUs (initialisation).
- Prefer a lock to a lock-free scheme unless BENCH.md shows the lock
  costs. Lock-free code comments its whole argument (as pmm.c's stash lock
  does).

### Locks

- Every lock has a class name (`spin_init(&l, "port bindings")`). The
  lock checker (lockdep) panics on an order cycle, a double take, and a
  class used both in and out of interrupt handlers.
- **Document the lock order** in the module's header, as `port.h` does.
  A new lock that nests under another is added to that list.
- Two locks of one class: `spin_lock_nested`. A function that expects a
  lock held ends in `_locked`.
- A lock taken in an interrupt handler is always taken with
  `spin_lock_irqsave` elsewhere.
- **Never block with a spinlock held or interrupts off:** no waits,
  mutexes, `copy_*_user`, channel calls, sleeps, TLB shootdowns
  (`kstack_free`). `schedule()` panics if you try.
- Don't drop an object's last reference under a lock its destructor may
  take. Collect doomed objects on a private list and release them after
  unlocking (port.c's `reap()`).
- Hold locks for as short as possible; never print or log under a hot
  lock when you can format first and print after (`debug_write` does).

### Every hardware wait is bounded

- Every loop that waits for a device or another CPU has a **deadline in
  time** (not an iteration count: CPU speeds differ; the one exception is
  code that runs before the TSC is calibrated, which uses a generous
  iteration bound), returns
  `ERR_TIMED_OUT` when it passes, and logs the last value it saw.
- Drivers sleep between polls (`drv_sleep_until`), as `wait_op` does;
  kernel code uses `udelay` with a deadline check.
- A driver's worst-case sum of waits stays below devmgr's `STOP_WAIT`;
  a driver with many devices caps its own shutdown time instead of
  summing per-device timeouts.
- Loops over lists that user code can grow are bounded by that list's cap;
  loops over hardware-provided chains (PCI capability lists, USB
  descriptors) carry a guard counter (`find_cap`'s `guard < 48`).

### Recursion and the stack

- **No recursion in the kernel.** Use an explicit list or loop
  (object.c's teardown uses a per-CPU pending list, so destroying a channel
  full of channels never recurses).
- A kernel thread has a 64 KiB stack. Keep a function's locals under ~1 KiB;
  bigger buffers come from `kmalloc` or a static/per-CPU buffer. No VLAs
  or `alloca` anywhere. The compiler enforces the outer limits: `-Wvla`
  everywhere, and a kernel frame over 3 KiB fails the build. The accepted
  exception to ~1 KiB is the channel syscalls' message buffers
  (`sys_channel_call`, about 2.4 KiB).

---

## 4. Naming

| Layer | Pattern | Example |
|---|---|---|
| Kernel object layer | `<object>_<verb>` on kernel structs | `port_bind`, `vmo_create` |
| Handle layer (kernel) | `sys_<object>_<verb>(struct handle_table *t, ...)`, kernel pointers | `sys_port_bind` |
| Syscall entry (kernel) | `sysc_<name>`, generated dispatch, user addresses as `uint64_t` | `sysc_port_wait` |
| User syscall wrapper | `jam_<name>` (generated) | `jam_port_wait` |
| Driver API | `drv_<name>` | `drv_port_wait`, `drv_read32` |
| IDL client / server | `<proto>_<method>[_until]`, `<proto>_serve`, `struct <proto>_ops` | `null_ping`, `input_key` |
| Shell | `sh_<helper>`, commands `SH_CMD(name)` → `shc_<name>` | `sh_say`, `shc_ls` |
| Constants | `UPPER_CASE`, prefixed by module | `PORT_MAX_BINDINGS`, `DR_SERVE`, `SUP_WINDOW` |
| Lock classes | lower-case words | `"port bindings"` |

- Lifecycle verbs: `_create`/`_destroy`, `_ref`/`_unref` (or `_put` for a
  private refcount), `_init` for embedded structs, `_locked` = caller holds
  the lock, `_irqsave` = saves and disables interrupts.
- Global names are descriptive; locals are short (`p`, `b`, `st`, `f` for
  saved flags, `i`, `n`). No Hungarian notation.
- Time values carry their unit in the name when it isn't ns:
  `backoff_ms`, `timeout_ms`; a bare `deadline` is absolute ns uptime.

---

## 5. Comments

- **Comments say why, and state invariants.** Not what the next line does.
  Good: `/* Listed before it can fire, so a spent ONCE packet dequeued right
  away always finds it on the list to retire. */`
- **Never history.** No milestone tags (`(M5)`, `M7 Track C`), no audit or
  review ids (`(C3)`, `(O2)`, `review R6`), no "used to", "now", "new",
  "was changed to". Git keeps history. Write the reason itself: not
  `/* (O3a) */` but `/* each binding is walked with interrupts off on every
  signal change, so an unbounded number is a DoS */`. For something not
  built yet, say so plainly ("not built yet: X"); the roadmap says when.
- **Every file starts with a header comment:** what it holds, the model a
  reader needs, and its key invariants (lock order, ownership, what is
  charged to whom). pmm.c and keyboard.c are the model.
- **Every struct field gets a short comment** unless its name says it all:
  what it holds, its unit, and which lock guards it
  (`uint32_t nbindings; /* live bindings; bindings_lock */`). A comment
  heading a block of related fields covers the block, and a spec citation
  covers a spec-defined layout (`/* xHCI 6.2.3 Endpoint Context */`).
- **Public functions** are documented on their declaration in the header:
  behaviour, errors, context. A static helper gets a one-line comment when
  its name isn't enough.
- **Hardware:** name every register and bit with a constant
  (`U_CAPS`, `PCI_BAR_MMIO`); comment the meaning of magic values; cite the
  spec section for non-obvious behaviour ("USB HID 1.11, Appendix B" for the boot keyboard report).
- `/* */` comments only. No `TODO` without a matching line in
  [docs/ROADMAP.md](docs/ROADMAP.md#smaller-follow-ups); prefer the roadmap
  line on its own.
- Plain English, full sentences, short. Readers are learning from this.

---

## 6. Recipes

### Add a syscall

1. Add a line to `abi/syscalls.def` in the right group, with a comment
   saying what it does. Pick an unused number; never reuse or renumber one.
   Pointer arguments become user addresses; more than six arguments: pass
   `@<struct>` and define the struct in `jam/abi.h` (no implicit padding,
   `_Static_assert` in `abi_check.c`).
2. `make syscalls`. Commit the generated files; `make check` fails if
   they are stale.
3. Object layer: the real work, on kernel structs (`kernel/object/`).
4. Handle layer: `sys_<name>` in `kernel/abi/<area>_sys.c`, declared in
   `jam/sys.h`. It calls `handle_get(t, h, OBJ_<TYPE>, RIGHT_<X>, &obj, NULL)`
   with the narrowest right that fits, and `kobject_unref`s after. New
   objects get their rights from a named constant (`PORT_RIGHTS`).
5. Syscall layer: `sysc_<name>` in `kernel/abi/sysc_<area>.c`:
   `SYSC_TABLE(t)`, copy in, validate, call `sys_*`, copy out, undo on a
   failed copy-out. A missing `sysc_*` is a link error.
6. Tests: a ktest for the object layer, and a utest that calls it from user
   space, including a bad pointer, a missing right and a wrong handle type.
7. Mention it in ARCHITECTURE.md if it changes the design.

### Add an IDL protocol or method

1. New protocol: `abi/idl/<name>.idl` with a unique protocol id and a
   header comment saying who serves it, who calls it, and **what a client
   does after `ERR_PEER_CLOSED`** (the reconnect rule).
2. New method: the next ordinal; never renumber or change an existing
   method's arguments (add a new method instead). Comment lines above a
   method are copied into the generated header.
3. Handles travel only as results. Bulk data goes through a shared VMO,
   not large `u8[N]` arrays (max 4096 per array, 8192 per message).
4. `make idl`; commit `drivers/include/idl/<name>.h`. Never edit it.
5. Server: fill a `static const struct <name>_ops`, call `<name>_serve`.
   Client: `<name>_<method>` or `_until` with a deadline.
6. Test both ends (a utest with a mock peer, as `user/tests/utest/hid.c` does).

### Add a shell command

1. `user/services/shell/cmd/<name>.c` with `SH_CMD(name) { ... }`, declared in
   `sh.h`.
2. A row in the command table (`sh_table.c`): category, usage, help.
3. Output with `sh_say` (goes into a pipe when piped); usage and errors
   with `sh_tty`. Read input with `sh_input`/`sh_stdin`.
4. Return 0 for success, 1 for failure, 2 for a usage error.
5. Every loop checks `sh_interrupted()` so Ctrl+C works; sleeps use
   `sh_sleep`.
6. Shared logic used by several commands goes into a named helper file,
   not a copy.
7. If it needs a handle the shell doesn't hold, stop: that is a design
   question, not something to work around.
8. Add checks to a `tools/shell-tests/*.txt` script.

### Add a driver

1. `drivers/<name>/`, `int driver_main(const struct driver_start *s)`,
   including only `<jam/driver.h>` and `<idl/*.h>`. The Makefile picks it up
   and packs `drv/<name>` into bootfs; `checkdriver.py` must pass.
2. Get handles with `drv_handle(s, DR_*)`; check each for
   `HANDLE_INVALID`.
3. Bind it in devmgr: a match-table row (PCI) or the USB interface path.
4. Order of bring-up: map BARs, reset or quiesce the device, *then*
   `drv_dma_bus_master(dma, 1)`. Interrupts through a `PERSISTENT` port
   binding on the interrupt object, `drv_interrupt_ack` after handling.
5. Every register wait bounded (section 3). Treat `0xffffffff` as gone.
6. Exit 0 only when the driver is finished for good; any other exit (or a
   crash) makes devmgr restart it. Keep no state across a restart.
7. Test with a mock (utest) and in QEMU; say in your report what only the
   PC can prove.

### Add a kernel test

1. In `kernel/test/test_<subject>.c`: `KTEST(<subject>_<behaviour>)`.
   The name starts with the subject so `ktest=<subject>` selects it.
   **Names never change**: tests are run by name.
2. `KT_ASSERT`, `KT_EQ` for the test's own objects.
3. System-wide counts (free pages, live channels) use `KT_GLOBAL_EQ` /
   `KT_GLOBAL_ASSERT`, so the test also runs from the shell on a live
   system. A test whose whole point is a global count, or that runs the
   machine out of memory, starts with `KT_SKIP_LIVE("why")`.
4. Clean up everything: a test that leaks more than 2 pages fails. It must
   pass on any run of a boot and in any order (`ktest loops=3 seed=1`):
   set up the state you start from yourself (no static left from the last
   run), and leave no thread, timer, hook or pin behind. A hook point is
   the whole system's: count only what is yours, and act only on what is
   yours. A hook that makes a call fail checks that the call is its own
   test's (its process, its thread) first; otherwise, under load, it fails
   whoever else passes by (a process-start hook once failed the stress
   workers' spawns on the PC). A test that legitimately grows a cache on
   its first run measures a later, identical round instead
   (`KT_OWN_LEAK_CHECK`).
5. It runs under load too (`ktest load`, `soak`). A check on exact
   timing, exact placement or an idle CPU is `KT_IDLE_ASSERT` /
   `KT_IDLE_EQ`; a test that is nothing else starts with
   `KT_NEEDS_IDLE("why")`. A wait that only guards against a hang takes
   `kt_patience_ms`. Never skip a correctness test for being slow.
   Which marker for which check: [TESTING.md](docs/TESTING.md#soak).
   Try it before you commit: `ktest <prefix> loops=5 seed=1 keep`, and the
   same with `load`.
6. **Dead is not yet freed.** A process that is reported dead, or a job
   that `job_kill` has emptied, can still be giving back its address
   space's pages on another CPU a moment later. A check that a job is
   empty after a kill waits for it, bounded by `kt_patience_ms`
   (`proc_job_kill_tree` does). The same goes for any count that a
   thread's last switch away changes.
7. Races: reproduce them deterministically with a `DBG_HOOK` point, not
   by looping and hoping.
8. Shared helpers (pinning, fresh jobs, rng) come from the ktest helpers,
   never a copy.

### Add a user program

1. `user/<services|apps|tests>/<name>/`: the Makefile finds it there and
   it becomes `bin/<name>` in bootfs (an app also links libfun).
2. `int main(int argc, char **argv)` on libos. Take handles from the
   startup message by role (`startup_handle(SR_*)`); the program has no
   other authority.
3. If the regression run (the `init` boot word) should run it, add a line
   to `boot/init.cfg`; the services init starts on a plain boot are
   started in `user/services/init/shell.c`, each with its handles.
4. Big ones split by job from the start, with an internal header.
5. An app that draws borrows the screen through the console
   (`console.lend_screen`) and uses the apps library.

---

## 7. Testing

**A bug fix comes with a test that fails without the fix.** Run it before
the fix to see it fail. If a test is impossible (PC-only hardware), say so
in the commit and the report.

Run the tests for the area you touched, cheapest tier first: the tiers,
the exact commands and each script's QEMU setup are in
[docs/TESTING.md](docs/TESTING.md).

- QEMU passing is necessary, not sufficient: say what still needs the PC.
- Don't loosen a test to make it pass. If an expectation was wrong, say so
  and why in the commit.

---

## 8. Contributing

- **Plan first.** Larger work starts from a written plan
  (docs/M<n>-PLAN.md; finished ones move to [docs/history/](docs/history/))
  that splits it into tracks, each owning its files.
- **Refactors and behaviour changes are separate commits.** A refactor
  keeps behaviour identical: same output, commands, ktest names, syscall
  numbers, IDL. Found a bug during a refactor? Fix it in its own commit.
- **Independent review.** A big change is reviewed by someone who did not
  write it, and the review's fixes land after it.
- **Commit messages:** `area: what changed`, present tense, one line (a
  body if the why is not obvious): `usb-bus: refuse hub interfaces`,
  `ktest: relax global counts on a live system`.
- **The networking rule** (hard requirement,
  [ARCHITECTURE.md](ARCHITECTURE.md#networking)): every frame Jam OS sends
  is tagged VLAN 21, nothing else ever leaves. A change that could
  transmit needs a test that proves an untagged frame can't leave.
- **Docs:** each fact has one home, and other docs link to it instead of
  copying it:

  | Fact | Home |
  |---|---|
  | the design and its rules | [ARCHITECTURE.md](ARCHITECTURE.md) |
  | milestone status: done, next, later | [docs/ROADMAP.md](docs/ROADMAP.md) |
  | what happened: milestones, bugs, lessons, dated decisions | [docs/HISTORY.md](docs/HISTORY.md) |
  | the real PC and flashing the stick | [docs/HARDWARE.md](docs/HARDWARE.md) |
  | test commands, boot menu, boot words | [docs/TESTING.md](docs/TESTING.md) |
  | benchmark numbers | [docs/BENCH.md](docs/BENCH.md) |
  | commands, build targets, where code lives | [README.md](README.md) |
  | the version string | `kernel/main.c` |

  Docs are written like the comments: plain English, short sentences,
  facts rather than praise, and what is not built yet said plainly. A
  number from the PC names its date and build.
  Update the doc in the same commit as the code that changes the fact.
  History goes to HISTORY.md, never into comments or status docs. Don't
  copy counts (tests, lines) into docs that aren't dated. `make check` runs
  `tools/checkdocs.py`, which fails on a link to a missing file or anchor,
  a missing repo path, file name or header in backticks, or an unknown
  `make` target.

---

## 9. Before you commit

- [ ] `make`, `make KTESTS=0` and `make check` pass (no warnings); run
      `make includes` if the check lists files.
- [ ] Generated code regenerated and committed (`make syscalls`, `make idl`).
- [ ] `make -s image` before the QEMU tests (plain `make` does not rebuild
      the image they boot).
- [ ] The tests for my area pass in QEMU, at 4 and 8 CPUs for kernel code;
      a new or changed kernel test also passes repeated, shuffled and under
      load (`tools/soak-test.sh`).
- [ ] A bug fix has a test that failed before the fix.
- [ ] Every new user pointer goes through `copy_*_user`, copied once.
- [ ] Every new handle check uses the narrowest right; new handles get
      named rights constants.
- [ ] Every new allocation reachable from user code is charged and capped,
      and fails with an error instead of panicking.
- [ ] Every new hardware wait has a time deadline.
- [ ] No code copied from GPL/LGPL projects; outside code only in
      `third_party/` with its licence (BSD-2-Clause compatible).
- [ ] New locks have a class name and are in the documented lock order;
      nothing blocks under a spinlock.
- [ ] Functions ≤ ~60 lines, files ≤ ~600, nothing copied from elsewhere.
- [ ] New files have a header comment; new struct fields have comments.
- [ ] The docs that hold a fact I changed are updated in the same commit.
- [ ] No milestone tags, audit ids or history in comments.
- [ ] Refactor and behaviour change are in separate commits.
- [ ] Commit message `area: what changed`.

---

## Sources

- Linux kernel, *Linux kernel coding style* (Documentation/process/coding-style.rst):
  short functions, few locals, centralised cleanup, comments on why.
- OpenBSD, *style(9)*: include order, `static`, clarity over cleverness.
- Fuchsia, *Zircon kernel concepts* and C/C++ style guide: handles with
  rights, least privilege, drivers and filesystems in user space.
- J. Liedtke, *On µ-kernel construction* (1995), via G. Heiser's *seL4
  design principles* (2020): the minimality test, policy freedom.
- SerenityOS, *CodingStyle.md*: comments explain why; `#pragma once`.
- G. J. Holzmann, *The Power of Ten: Rules for Developing Safety-Critical
  Code* (NASA/JPL, 2006): bounded loops, no recursion, 60-line functions,
  check every return value, smallest scope.
- SEI CERT C Coding Standard: INT30-C (unsigned wrap), INT31-C
  (conversions), EXP33-C (uninitialised reads), DCL39-C (padding across a
  trust boundary).
- OSDev wiki, *Volatile (keyword)* and *Memory mapped registers in C/C++*:
  what `volatile` does and does not guarantee.
