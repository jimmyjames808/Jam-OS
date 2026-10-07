# M12 review K: the system calls

Stage A's review of the system call interface, for
[M12-PLAN.md](M12-PLAN.md#stage-a-the-reviews-findings-first). Docs only:
no code changed, no QEMU run. Read at main 56aa8904.

Read in full: `abi/syscalls.def`; the kernel headers user code sees
(`kernel/include/jam/abi.h`, `kernel/include/jam/status.h`,
`kernel/include/jam/startup.h`, the generated `user/include/jam_syscalls.h`);
every file in `kernel/abi/`; `<jam/driver.h>`; the object code behind the
calls where a finding depends on it (`kernel/object/channel.c`,
`kernel/object/channel_send.c`, `kernel/object/handle.c`,
`kernel/object/port.c`, `kernel/object/object.c`,
`kernel/object/process.c`, `kernel/mm/aspace.c`'s map, unmap and protect,
`kernel/arch/x86_64/uentry.c`); and the callers each finding names.

The owner's answers of 2026-10-07 are taken as decided: handles are always
moved on send (Q1), the reader pays once the writer has closed (Q2), the
"receives no handles" mark (D2), TLS and a futex in M12 (Q10),
`handle_info` (Q11), read-only pins (Q13), one rule for time (Q19). This
review gives each its shape and the callers it touches; the design
questions at the end are only what those answers leave open.

Not reviewed here: the IDL and hand-written protocols (review P), the
compositor's protocol (review W), the storage split (review S), and speed
(the message in registers that M11.5 left "for M12 to weigh" stays with the
speed pass: it is an addition).

**In short:** 38 findings: 6 bugs today (four small enough to fix at once),
11 inconsistencies, 10 cleanups and 11 needs of M13. Nothing found lets a
program gain authority or corrupt the kernel. Six design questions.

## Findings

Kinds: **bug today** (wrong now, whatever M12 decides), **inconsistency**
(two calls that do one thing two ways, or a rule with exceptions),
**cleanup** (names, headers, dead calls), **M13 need** (what POSIX will
miss). "Breaks" means callers must change in the same merge; "adds" means
nothing that builds today stops building or behaving as it does.

| # | Kind | Where | What | Proposed change | Track | Breaks or adds | Callers |
|---|---|---|---|---|---|---|---|
| 1 | bug today (latent) | `kernel/object/channel_send.c:14-28` | One global 32-bit txid counter. After it wraps (about 72 minutes at a million calls a second) a late reply queued on an end (its caller gave up) can be taken by a new call that drew the same txid. Decided as D1 | A counter in `struct chan_pair`, shared by the pair's two ends. Not one per end: two ends calling each other at once would draw equal txids, and one end's request would be taken as the other's reply (the comment at line 19 gives exactly this as the reason for a global counter). Never 0 | K1 | adds (kernel only) | 0 programs; the txid ktests |
| 2 | bug today | `kernel/object/channel.c:487-488`, `kernel/object/channel_send.c:347-349`, `abi/syscalls.def:74-76` | A wait whose own channel end is closed by another thread ends with `ERR_BAD_STATE` in `channel_read_wait` (so in `channel_reply_wait`), but with `ERR_CANCELED` in `channel_call`. The def's error list for `channel_reply_wait` has neither | `ERR_CANCELED` for every wait ended by the closing of its own handle's object (a killed thread never sees the status, so `ERR_CANCELED` means only this), and the def lists it. Small: fix now | K1 | breaks nothing found (no caller tests `ERR_BAD_STATE` after a wait) | 0 |
| 3 | bug today | `kernel/abi/sysc_channel.c:107-131`, `:143-174`, `:211-236` | A handle buffer address of 0 with a non-zero `handles_cap` (or reply `rh` with `rhcap`) is accepted: the message is taken, its handles go into the table, the copy out faults, they are closed and the message is lost. For `channel_reply_wait` the reply has already gone out by then, against the def's rule that argument errors "come before anything is sent" (`abi/syscalls.def:68-69`) | `ERR_INVALID_ARGS` before anything happens when a buffer's size is non-zero and its address 0 (handles and bytes, all three calls). Small: fix now | K1 | adds a check | 0 |
| 4 | inconsistency | `kernel/abi/channel_sys.c:96-114`, `:148-169`, `:244-258`, `:336-355`; `kernel/include/jam/sys.h:20-24`, `:40-45`; `kernel/include/jam/channel.h:76-77`, `:94-96` | Q1: a send that fails before its message is queued gives the handles back; one that fails after (a call's deadline) has moved them; the status doesn't say which | Every handle named in `channel_write`, `channel_write_rights`, `channel_call` and `channel_reply_wait`'s reply is consumed once the kernel has read the handle list, whatever the status (edges: question C). The `untake_all` paths become closes. Docs in the def, `sys.h`, `channel.h`, `<jam/driver.h>` | K1 | breaks | 1 must change: libjwl's `write_batch` keeps a batch and its handles after `ERR_SHOULD_WAIT` or `ERR_NO_MEMORY` and sends the same handles again later (`user/lib/jwl_transport.c:281-290`): it must wait for `SIG_WRITABLE` first or keep duplicates (P1's file; lands with K1's change). About 20 sites close the handles after a failed write ("not sent: still ours": `drivers/include/idl/common.h:168`, `user/lib/svcserve.c:50`, `user/lib/fsview.c:149`, `user/lib/ns.c:701`, `user/lib/keep.c:77`, `user/lib/keep.c:416`, `user/lib/svcstate.c:444`, devmgr, the mixer, netstack, the console, the shell's `serve`, usb-bus, hda): harmless after the change (a closed value fails with `ERR_BAD_HANDLE`, generations keep it from naming anything new), dead code to drop; the give-back ktests and utests |
| 5 | inconsistency | `kernel/abi/sysc_proc.c:94-106`, `kernel/object/handle.c:386-399`, `kernel/abi/vmo_sys.c:122-145` | Other calls that take a handle away follow other rules: `process_start` gives `arg0` back on failure; `handle_replace` keeps `h` when the rights are refused; `vmo_make_exec` keeps `vmo` on a rights or type error but loses it on `ERR_BAD_STATE` | Q1's rule for them too: the handle is gone whatever the status (question C) | K1 (`process_start`), K3 (the other two) | breaks | `process_start` 1 (`user/lib/spawn.c`), `handle_replace` 10, `vmo_make_exec` 1 (`user/services/shell/sh_allow.c:202`) |
| 6 | inconsistency | `kernel/object/channel_send.c:58-87` | D2: no way to keep handles out of a queue. A kept server end whose queue holds a message carrying a channel end can't be sent to a successor (the cycle check refuses it) | A one-way mark on an end: a write carrying handles to a marked end fails `ERR_NOT_SUPPORTED` (the cycle check's code: "this end can't take that"). Set by a new call (question B). Only for ends whose protocol takes no handles in requests | K1 | adds | 2 setters (init's and devmgr's, later volumes', supervision); the protocols on those ends (P and S say which) |
| 7 | inconsistency | `kernel/object/channel.c:44-49`, `channel_close` at `kernel/object/channel.c:289` | Q2: a queued message stays charged to its writer's job until read, even after the writer closed: the compositor keeps a "lingering" slot per disconnected client, the clipboard's owner pays for a reader that never reads | When an end closes, the messages it wrote that wait on its peer move their charges to the reader's job (question A says which job); a message that job can't pay is dropped with every later one, so what is left is a prefix of the stream, then `ERR_PEER_CLOSED` | K1 | adds (behaviour) | the compositor's lingering slots go (`user/services/compositor/conn.c`, P1); ARCHITECTURE's "IPC" and clipboard text |
| 8 | cleanup | `kernel/include/jam/abi.h:216-280` | Three structs, three naming schemes: `channel_read_args` has `bytes_cap` then `bytes`, `channel_reply_wait_args` `bytes` then `bytes_cap` and `rn`/`rhn`, `channel_call_args` `wn`, `rcap`, `rhcap` | One scheme and one order in all three: the sent side `wr_bytes`, `wr_len`, `wr_handles`, `wr_nhandles`; the received side `rd_bytes`, `rd_cap`, `rd_handles`, `rd_hcap`, `rd_actual`, `rd_hactual`; then `deadline_ns`, `flags`, `reserved`. The out-pointers stay (the adjacent-words copy already makes it one copy); a pure rename first, in its own commit | K1 | breaks (mechanical) | about 130 sites fill these by hand (75 `channel_read`, 35 `channel_reply_wait`, 20 `channel_call`), plus the generated `drivers/include/idl/common.h` and `user/lib/driver_user.c` |
| 9 | cleanup | `kernel/include/jam/channel.h:39-43`, `kernel/include/jam/process.h:70`; copies in `user/tests/utest/utest.h:19`, `user/lib/keep.c:19` | A message's 64 KiB and 64 handles, a queue's 1024 messages and the 1 KiB charged per carried handle are in kernel headers only; user code copies them by hand | `CHANNEL_MAX_BYTES`, `CHANNEL_MAX_HANDLES`, `CHANNEL_MAX_QUEUED`, `JOB_OBJECT_BYTES` move to `<jam/abi.h>` (the kernel's headers include it); the two copies go | K1 | adds | 2 copies |
| 10 | inconsistency | `kernel/abi/sysc_hw.c:295-307` | D3: `dma_cap_create` charges the caller's job (devmgr's) for the cap and, through `dma_cap_create_for`, the IOMMU domain's table pages | `dma_cap_create(dev, job, out)`: the job to charge, needing `RIGHT_MANAGE` (the creator's right; a program can't charge a job it doesn't manage); devmgr makes the driver's job first, as D3 says | K2 | breaks | 2 (`user/services/devmgr/bind.c`, a utest) |
| 11 | inconsistency | `abi/syscalls.def:101`, `kernel/abi/sysc_hw.c:312-347` | Q13: every pin is read-write for the device and needs `RIGHT_WRITE`, even for a buffer the device only reads; `vmo_pin` already has six arguments, so no room for a flag | A read-only pin flag (question D for the argument's shape). With the IOMMU translating, a read-only pin needs only `RIGHT_READ` and maps read-only; with `iommu=off` it promises nothing, so it still needs `RIGHT_WRITE` (a device could write through it). The drivers' transmit rings and sound buffers use it | K2 | breaks if the struct form | 13 pin sites (the `drv_vmo_pin` wrapper can keep its six arguments); the read-only users: `drivers/rtl8125/ring.c`, `drivers/e1000e/ring.c`, `drivers/hda/stream.c` |
| 12 | inconsistency | `kernel/mm/aspace.c:648-655` | `VMAR_KEPT_ONLY` on a VMO that isn't kept answers `ERR_WRONG_TYPE`, though the handle is a VMO: the type is right, the kind isn't (G1 K1's note) | `ERR_NOT_SUPPORTED` | K2 | breaks tests only | 0 programs; `user/tests/utest/keptvmo.c`, the kept-VMO ktests |
| 13 | cleanup | `kernel/abi/sysc_hw.c:218-236`, `abi/syscalls.def:168-172` | `pci_bus_master(dev, enable)` refuses `enable` 1: it is an off switch with an argument that can only be 0 | `pci_bus_master_off(dev)` (a new number; 96 retired) | K2 | breaks | 2 (`user/services/devmgr/bind.c:354`, `user/tests/utest/supervise.c:344`) |
| 14 | inconsistency | `kernel/abi/vmo_sys.c:31-43` | `vmo_create` takes a `dma_cap` that only gates the drivers' two flags, and any handle to any dma_cap passes: unbound, superseded, with no rights | `vmo_create(size, flags, out)` for everyone; `dma_vmo_create(dma, size, flags, out)` for contiguous or below-4-GiB memory, needing a cap bound to a function | K2 | breaks (mechanical) | 84 `jam_vmo_create` sites drop the 0; `drv_vmo_create`'s 19 callers keep the wrapper |
| 15 | cleanup | `kernel/include/jam/vmo.h:41-43`, `kernel/include/jam/resource_impl.h:175`, `kernel/mm/aspace.c:156`, `drivers/include/jam/driver.h:138-139` | Limits user code can't see: a VMO's 64 GiB, a pin's 4096 pages, an address space's 16384 mappings, contiguous memory's 4 MiB; the contiguous and below-4-GiB flags have kernel names (`VMO_CONTIGUOUS`) and driver names (`DRV_VMO_CONTIGUOUS`) for the same bits | Into `<jam/abi.h>`, the two flags under one public name (`<jam/driver.h>` keeps its names as aliases or drops them) | K2 | adds | 0 |
| 16 | cleanup | `user/lib/keep.c:244-268`, `user/services/compositor/data.c:398-412` | Q11: nothing names a handle's type. The keeper probes with up to five calls; the compositor reads zero bytes, and so takes a channel end whose peer has closed (`ERR_PEER_CLOSED`) for "not a channel" | `handle_info(h, struct handle_info *out)`: the object's type (the `OBJ_*` numbers made public in `<jam/abi.h>`), the handle's rights, the object's koid and a related koid (a channel end's peer, a thread's process, a process's job). No right needed: it says only what the holder could find out by probing | K3 | adds | 2 |
| 17 | M13 need | `kernel/include/jam/status.h:7-27` | Q9: no not-a-directory, is-a-directory, not-empty, name-too-long or busy | `ERR_NOT_DIR` -21, `ERR_IS_DIR` -22, `ERR_NOT_EMPTY` -23, `ERR_NAME_TOO_LONG` -24, `ERR_BUSY` -25, with `status_str` in the kernel and in libos (`user/lib/printf.c:305`); `ERR_INTERRUPTED` waits for M13 (finding 35) | K3 | adds | the filesystem servers (P5) |
| 18 | inconsistency | `abi/syscalls.def:39`, `kernel/abi/sysc_basic.c:69-75` | Q19: `nanosleep` takes a deadline under a sleep's name; drivers already call it `drv_sleep_until` | Renamed `sleep_until`, number 5 kept (same arguments and meaning). Every other call already follows Q19's rule (deadlines; the timeout flag only in the two calls that have a flags word) | K3 | breaks (mechanical) | 152 `jam_nanosleep` sites; `drv_sleep_until`'s 23 unchanged |
| 19 | cleanup | `kernel/include/jam/abi.h:22-55` | Generic rights and root powers share one word out of order: `RIGHT_RESIZE` is bit 19, between the powers at 11-18 and 20-21 | Generic rights in bits 0-11 (`RIGHT_RESIZE` becomes 11), powers from bit 16 in today's order, `RIGHT_SAME` stays bit 31 | K3 | breaks nothing by name | 0 (no rights value outlives a boot: state VMOs don't survive kexec) |
| 20 | inconsistency | `kernel/abi/sysc_proc.c:22-30`, `kernel/abi/sysc_console.c:419-420`, `kernel/abi/sysc_console.c:465-466`, `kernel/abi/sysc_kexec.c:33-34`, `kernel/abi/sysc_random.c:15-16`, `kernel/abi/sysc_basic.c:24-25`, `kernel/abi/sysc_console.c:529-530` | Too long, four ways: process and thread names are cut silently; a log name, a debug command and a kexec command line are refused with `ERR_INVALID_ARGS`, as is `random_get` over 256 bytes; `debug_write` cuts at 4096 bytes silently; `serial_write`, `channel_write` and `vmo_pin` refuse with `ERR_OUT_OF_RANGE` | One rule: a length past a call's limit is `ERR_OUT_OF_RANGE`, nothing cut and nothing done; the limits in `<jam/abi.h>` (finding 23); libos cuts a process or thread name itself before the call | K3 | breaks | 3 name sites in `user/lib/spawn.c`; `debug_write`'s 4 direct callers write at most 512 bytes (`user/lib/printf.c:13`) |
| 21 | cleanup | `kernel/abi/port_sys.c:142-153`, `abi/syscalls.def:51` | Two signal calls: `event_signal` (an event's `SIG_SIGNALED` and user bits) and `object_signal` (user bits on any object). `object_signal` has no caller outside tests, and on a channel end it sets bits only that same end's holders see | `object_signal` goes (number 17 retired); `event_signal` stays | K3 | breaks tests only | 0 programs; `user/tests/utest/netsrv.c`, `user/tests/utest/main.c`, `kernel/test/test_object.c` |
| 22 | cleanup | `kernel/include/jam/abi.h:338-409`, `kernel/include/jam/startup.h:12-54`, `:69-97` | Service protocols in kernel headers: the console's `open_keys` messages (`input_key_event` and friends) and the startup roles the kernel never uses (`SR_DEVMGR` ... `SR_STANDBY`), the promotion message | Move to user headers. The kernel keeps the roles userboot gives (`SR_SELF_*`, `SR_JOB`, `SR_BOOTFS`, `SR_RESOURCE`), `SR_CRASHLOG` and the startup message itself. Q14 removes `open_keys` anyway (P1) | K3 | breaks includes only | the console, the compositor's input path, hid, serialin (P1 rewrites them), libos's start and spawn |
| 23 | cleanup | `kernel/include/jam/handle.h:52`, `kernel/include/jam/port.h:40-43`, `kernel/include/jam/process.h:48`, `kernel/abi/sysc_console.c:40`, `kernel/abi/sysc_basic.c:16` | The other limits user code can't see: 65,536 handles, 4096 user packets and 4096 bindings a port, 32-byte names, 64-byte debug commands, 4096-byte serial and debug writes | Into `<jam/abi.h>` | K3 | adds | 0 |
| 24 | bug today | `kernel/object/process.c:647-668`, `kernel/arch/x86_64/uentry.c:356-357` | `thread_start` and `process_start` accept an entry outside user space; the thread then starts, and its first entry to ring 3 kills the whole process ("entry at a bad address"): an argument error kills the caller instead of failing the call | `ERR_INVALID_ARGS` from `start_thread` for `entry >= USER_TOP` (and for K4's FS base, finding 29). Small: fix now | K4 | adds a check | 0 |
| 25 | inconsistency | `kernel/object/object.c:213-240`, `kernel/object/port.c:353` | A wait on a handle another thread closes doesn't end (`object_wait_one`, `port_wait`: the wait holds the object), while a channel wait does (finding 2) | Document it now ("a wait holds the object; closing the handle ends only a channel end's own waits"). M13 decides if `close()` from another thread must wake a blocked `read()` (per-handle waiters, as Zircon) | K3 (doc) | adds | 0 |
| 26 | bug today | `kernel/abi/port_sys.c:92-105`, `kernel/include/jam/port.h:22-25`, `drivers/include/jam/driver.h:119-121` | A port binding outlives every handle to its object: unbinding needs the object's handle, so a program that closes the handle first can never unbind, and the binding keeps the object and its charge until the port dies. A whole class of leaks: the 2026-10-07 sweep fixed it in the compositor, libjwl, dns, music, netstack, the mixer, hda, the network drivers and usb (105 binds against 72 unbinds before) | A binding ends when its object has no handles left (question E); its packet, if queued, stays | K3 | adds (frees what nobody could reach) | 0 programs rely on it; the port ktests |
| 27 | cleanup | `kernel/object/port.c:338`, `kernel/include/jam/abi.h:98` | `port_queue` keeps the caller's `status`, which `<jam/abi.h>` reserves (`ERR_CANCELED` for a binding's teardown) | User packets carry status `OK` (the kernel sets it); the data is in `user.data` | K3 | breaks if a caller uses `status` | 12 queue sites to check |
| 28 | bug today (docs) | `drivers/include/jam/driver.h:35`, `user/include/os.h:128-131` | Stale words: `DR_INPUT` "an input channel to the console" (the compositor's now); `struct spawn_args` refuses programs from elsewhere "until the kernel can make a VMO executable" (`vmo_make_exec` has since M8.6). Plan items 28 | Reword. Small: fix now | any | adds | 0 |
| 29 | M13 need | `kernel/arch/x86_64/uentry.c:363-366`, `kernel/arch/x86_64/cpu.c:142-145` | Q10: no thread-local storage: every thread's FS base is 0 | `thread_start` gains a sixth argument, the new thread's FS base; `thread_set_fs_base(base)` sets the calling thread's (the first thread's at crt0: `process_start` has no room, and musl's `__set_thread_area` is this call). A base at or above `USER_TOP` is `ERR_INVALID_ARGS` (a non-canonical one would fault `wrmsr` in the kernel). Kept in `struct thread`, written on a switch only when it differs from the CPU's last; never read back, since FSGSBASE stays off (user code can't change it); GS stays 0. A bench line for the switch | K4 | breaks | `thread_start` 1 (libos `thread_spawn`, which drivers' threads use too) |
| 30 | M13 need | `user/lib/lock.c:11-18` | Q10: no wait on an address: libos's lock pauses, then sleeps 20 us between tries | `futex_wait(addr, expected, deadline, flags)`: `ERR_BAD_STATE` if `*addr != expected` when checked, `ERR_TIMED_OUT`, `ERR_INVALID_ARGS` for an unaligned or unmapped address; `futex_wake(addr, count)` returns how many woke; `futex_requeue` per question F; spurious wakes allowed; libos's lock on them. Keyed per question F | K4 | adds | `user/lib/lock.c` (its API unchanged) |
| 31 | M13 need | `kernel/abi/sysc_basic.c:69-75` | No yield: `sched_yield` has nothing to map to (a sleep with a past deadline returns without giving up the CPU) | `thread_yield()`: K4 if it is cheap there (it is in the scheduler anyway), else M13 | K4 or M13 | adds | 0 |
| 32 | M13 need | `kernel/abi/sysc_sysinfo.c:87-96` | `uname` and `sysconf` (CPUs online, memory pages) need `sys_info`, which needs `RIGHT_ROOT_SYSINFO`: an ordinary program can't ask. Plan item 15 left the decision here | Yes, they need a call without a power: `sys_public_info(out)` (version, machine, CPUs online, total pages, page size), no handle, as `wallclock_get`. An addition: M13 adds it (K3 may, if cheap) | M13 | adds | 0 |
| 33 | M13 need | `kernel/object/object.c:17`, `:97`, `kernel/include/jam/abi.h:196-203` | A koid is not a pid: every kernel object (each event, VMO, channel end) draws one, so on a busy system that runs long enough koids pass 2^31, but musl's `pid_t` is an `int` and its mutex word keeps the owner's thread id in 30 bits | M13 adds a process id and a thread id, small and unique among the living (`process_info.reserved` can carry the pid without a layout change; `handle_info` or a thread info call the tid) | M13 | adds | 0 |
| 34 | M13 need | `kernel/abi/sysc_sysinfo.c:114-136` | CPU time is visible only through `proc_list` (a power): no `times`, `getrusage`, `clock()` or the CPU-time clocks | `process_info` gains `cpu_ns` (the kernel has it: `process_cpu_tsc` in `kernel/object/process.c`), and a thread info call gives a thread's; M13 (or K3 while it is in `<jam/abi.h>`: a layout change, cheaper now) | M13 | breaks the struct's size if done | `process_get_info`'s 20 callers rebuild only |
| 35 | M13 need | every blocking call | No signals and no way to interrupt a blocked call (`EINTR`) | M13 adds: `thread_interrupt(thread)` ends the thread's blocking call with `ERR_INTERRUPTED` (a new code) through the cancellable waits every blocking call already uses, never losing what a call already took; for a thread running user code, an upcall at its next return to ring 3 (`thread_set_upcall(entry, stack)`, the registers and the full vector state saved on that stack, `upcall_return`); faults as signals the same way, or the process killed as today. `channel_call` is not interrupted once its request is out (it can't be sent twice) | M13 | adds | 0 |
| 36 | M13 need | `kernel/mm/aspace.c:737-745` | `mmap(MAP_FIXED)` over an existing mapping replaces it; `vmar_map` refuses (`ERR_ALREADY_BOUND`). `madvise(MADV_DONTNEED)` names an address, `vmo_decommit` a VMO | M13 adds `VMAR_REPLACE` (with `VMAR_FIXED`: unmap what is there, then map, in one step) and a decommit by address range | M13 | adds | 0 |
| 37 | M13 need | `abi/syscalls.def:105` | No copy-on-write child VMO: `MAP_PRIVATE` of a file needs one. Plan item 16 | M12.5 with the pagers, or M13: `vmo_create_child(vmo, offset, size, COPY_ON_WRITE, out)` | M12.5 / M13 | adds | 0 |
| 38 | M13 need | `kernel/abi/sysc_basic.c:56-60` | musl's `pthread_join` and its thread-list lock count on the kernel clearing a word and waking it once an exiting thread is off its stack (Linux's `CLONE_CHILD_CLEARTID`) | M13 adds `thread_exit_wake(addr)`: store 0, `futex_wake` all, after the last user access. Until then a joiner can wait for the thread handle's `SIG_TERMINATED` (have it) | M13 | adds | 0 |

**Bugs today, for the main session:** 2, 3, 24 and 28 are small (a status,
two checks, two comments), each with an obvious test. 1 is D1 (K1). 26 needs
question E first.

## The POSIX table

Every system call musl's x86-64 port makes (from its `arch/x86_64` system
call list and the calls its `src/` uses), grouped by the need it serves,
with the needs of M13's row ([ROADMAP.md](ROADMAP.md#later)). Marks:
**have it** (which call), **M12 adds** (the shape), **M12.5 / M13 adds**
(the shape), **user space** (libc and a service, no system call), **not
planned** (musl gets `ENOSYS` and copes). M13 replaces musl's system call
layer, so a Linux call maps to whatever does the job, not one to one.

| Need | Linux calls musl makes | Mark | How, or the shape |
|---|---|---|---|
| Exit | `exit`, `exit_group` | have it | `thread_exit`, `process_exit` |
| Threads | `clone` (thread flags), `set_tid_address`, `gettid` | have it, M13 adds | `thread_create`, `thread_start`; join by `SIG_TERMINATED` on the thread handle; a detached thread frees its own stack with `vmar_unmap` then `thread_exit` from registers (musl's `__unmapself` pattern). M13 adds the tid (finding 33) and `thread_exit_wake` (finding 38) |
| Thread-local storage, `errno` | `arch_prctl(ARCH_SET_FS)`, `clone`'s `CLONE_SETTLS` | M12 adds | finding 29 (K4). `errno` is thread-local in musl, so it needs this too |
| Futexes | `futex` (`WAIT`, `WAKE`, `REQUEUE`, `PRIVATE`; `LOCK_PI` only for priority-inheritance mutexes) | M12 adds | finding 30 and question F (K4). musl's waits pass a relative timeout: its layer turns it into a deadline. PI: not planned |
| Robust mutexes | `set_robust_list`, `get_robust_list` | not planned | a robust mutex then never reports its owner's death |
| Yield, priority, affinity | `sched_yield`, `sched_setparam`, `sched_getscheduler` and friends, `setpriority`, `sched_getaffinity`, `sched_setaffinity` | have it in part | priority: `thread_set_priority` (up to 24). Yield: finding 31. Affinity: not planned (`sched_getaffinity` only for the CPU count: finding 32) |
| Starting programs | `clone(CLONE_VM\|CLONE_VFORK)` then `execve` (`posix_spawn`), `fork`, `vfork`, `execve` | have it (`posix_spawn` only) | `process_create`, `vmar_map` of the ELF (libos's loader), `process_start` with the startup message; a pipe or file as a startup handle (the startup message carries 16 handles: review P, item 27). A program from `/data` needs `vmo_make_exec`, which needs `RIGHT_ROOT_VMEX` (only the shell has it) until M12.5's pager runs programs from `/data` the clean way. `fork`, `vfork`, `execve`: not planned |
| Waiting for a child | `wait4`, `waitid` | have it | `object_wait_one(proc, SIG_TERMINATED)` (deadline 0 for `WNOHANG`), `process_get_info` for the code; any child: one port, a binding per child. Killed-by-signal status: M13 (today only `killed` and `PROCESS_KILLED_CODE`) |
| Process ids | `getpid`, `getppid`, `setsid`, `getsid`, `setpgid`, `getpgid` | have it in part | `process_get_info`'s koid works but isn't a `pid_t` (finding 33: M13 adds a pid); `getppid`: M13 (the parent's pid in the startup environment, or a field); sessions and process groups: user space (the terminal layer) |
| Signals | `rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn`, `rt_sigpending`, `rt_sigsuspend`, `rt_sigtimedwait`, `rt_sigqueueinfo`, `sigaltstack`, `kill`, `tkill`, `tgkill` | M13 adds | finding 35: `thread_interrupt`, an upcall, `ERR_INTERRUPTED`. `kill` by pid needs a handle: libc keeps its children's; others through a service |
| Interrupting a blocked call | (`EINTR` from any blocking call) | M13 adds | finding 35. Every blocking call already waits through `thread_block_cancellable`, so the interruption is one more reason to wake, not new waiting code |
| Memory | `mmap` (anonymous), `munmap`, `mprotect` | have it | `vmo_create` + `vmar_map`, `vmar_unmap` (splits), `vmar_protect` (splits; `PROT_NONE` allowed; write and execute together refused: `EACCES`) |
| Memory, the rest | `mmap(MAP_FIXED)` over a mapping, `madvise`, `mremap`, `brk`, `msync`, `mlock`, `mincore`, `memfd_create`, `membarrier` | M13 adds, or not planned | `MAP_FIXED` replace and `madvise` by address: finding 36. `mremap`, `brk`: not planned (musl's malloc falls back to `mmap`). `msync`, `mlock`, `mincore`: no-ops. `memfd_create`: a VMO (have it). `membarrier`: not planned |
| Mapped files | `mmap` of a file (`MAP_SHARED`, `MAP_PRIVATE`) | M12.5 adds | the pagers; `MAP_PRIVATE` needs a copy-on-write child (finding 37) |
| Clocks | `clock_gettime` (vDSO), `clock_getres`, `gettimeofday`, `time` | have it | `CLOCK_MONOTONIC`, `CLOCK_BOOTTIME`: `clock_get`; `CLOCK_REALTIME`: `wallclock_get` (`ERR_NOT_FOUND` when nobody set the clock: libc gives the uptime from 1970); resolution 1 ns. The read-only time page (clocks without a system call): M13, as Q19 says |
| CPU-time clocks, usage | `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)`, `CLOCK_THREAD_CPUTIME_ID`, `times`, `getrusage` | M13 adds | finding 34 |
| Setting the clock | `clock_settime`, `settimeofday`, `adjtimex` | have it (init, sntp) | `wallclock_set` needs `RIGHT_ROOT_CLOCK`: `EPERM` for ported programs; `adjtimex`: not planned |
| Sleeping | `nanosleep`, `clock_nanosleep` (relative and `TIMER_ABSTIME`) | have it | `nanosleep`, renamed `sleep_until` (finding 18); relative: the clock plus the time |
| Timers | `timer_create`, `timer_settime`, `timer_gettime`, `timer_delete`, `setitimer`, `alarm`, `timerfd_*` | have it, M13 adds | timer objects (`timer_create`, `timer_set`, `timer_cancel`; periodic: libc sets it again); delivery as a signal: finding 35 |
| Files | `open`, `openat`, `read`, `write`, `readv`, `writev`, `pread64`, `pwrite64`, `lseek`, `fstat`, `stat`, `lstat`, `newfstatat`, `statx`, `getdents64`, `mkdir`, `mkdirat`, `rmdir`, `unlink`, `unlinkat`, `rename`, `renameat`, `ftruncate`, `truncate`, `fsync`, `fdatasync`, `sync`, `statfs`, `fstatfs`, `access`, `faccessat`, `utimensat`, `readlink`, `symlink`, `link`, `chmod`, `chown`, `umask`, `mknod`, `flock`, `fallocate`, `fadvise64`, `sendfile`, `copy_file_range` | user space | libc over the `fs` and `file` protocols (Q8, review P); `close`: `handle_close`; errors: Q9's codes (finding 17). FAT has no links, owners or modes: those calls answer what libc decides |
| Descriptors | `dup`, `dup2`, `dup3`, `fcntl` (`F_DUPFD`, `FD_CLOEXEC`, `O_NONBLOCK`, locks), `close_range` | user space | libc's descriptor table over handles; `handle_duplicate` (have it) |
| Directories | `chdir`, `fchdir`, `getcwd`, `chroot` | user space | libos's namespace already resolves paths; the current directory is libc state |
| Pipes | `pipe`, `pipe2` | have it | Q17: a channel of byte messages (`channel_create`); a closed reader is `ERR_PEER_CLOSED` (`EPIPE`; `SIGPIPE` with finding 35); non-blocking is `ERR_SHOULD_WAIT` (`EAGAIN`). Once Q2 is built, data a writer left behind is the reader's to pay for |
| Waiting on many | `poll`, `ppoll`, `select`, `pselect6`, `epoll_*`, `eventfd2` | have it | ports: `port_bind` once per descriptor, `port_wait`, `port_unbind` (M9.5's `netwait` already does this); `eventfd`: an event's user bits. M13 may add `object_wait_many(items, n, deadline)` if a small `poll` costs too many calls: an addition |
| Sockets | `socket`, `socketpair`, `bind`, `connect`, `listen`, `accept`, `accept4`, `getsockname`, `getpeername`, `sendto`, `recvfrom`, `sendmsg`, `recvmsg`, `sendmmsg`, `recvmmsg`, `shutdown`, `setsockopt`, `getsockopt` | user space | M9.5's socket rings and the `net` protocol; `socketpair`: a channel |
| Terminals | `ioctl` (`TCGETS`, `TCSETS`, `TIOCGWINSZ`, `FIONBIO`, ...) | user space | M13's terminal layer (pseudo-terminals) as a service |
| System information | `uname`, `sysinfo`, `sethostname`, `getcpu`, `personality` | M13 adds | finding 32 (`sys_public_info`); the host name is a setting, not the kernel's; `getcpu`, `personality`: not planned |
| Random numbers | `getrandom` | have it | `random_get` (256 bytes a call: exactly `getentropy`'s limit; libc loops for more) |
| Limits | `getrlimit`, `setrlimit`, `prlimit64` | have it (reading) | `job_get_info` on `SR_JOB` (pages, handles, threads); setting: `EPERM` (a program's own job handle lacks `RIGHT_MANAGE`, by design) |
| Users and groups | `getuid`, `geteuid`, `getgid`, `getegid`, `getgroups`, `setuid` and friends, `getresuid` | user space | one user: libc constants |
| Mounts, reboot, log | `mount`, `umount2`, `reboot`, `syslog` | not planned for ports | the volumes service, `reboot` and the `klog_*` calls stay Jam OS programs' (they need powers) |
| Debugging | `ptrace`, `process_vm_readv` | not planned | |
| `errno` values | (every call) | M12 adds, M13 adds | the mapping, one to one where it can be: `ERR_NO_MEMORY` `ENOMEM`; `ERR_INVALID_ARGS` `EINVAL` (also for a bad pointer: no `EFAULT`, which programs don't test for); `ERR_BAD_HANDLE` `EBADF`; `ERR_ACCESS_DENIED` `EACCES` or `EPERM`; `ERR_SHOULD_WAIT` `EAGAIN`; `ERR_TIMED_OUT` `ETIMEDOUT`; `ERR_PEER_CLOSED` `EPIPE` on a write, end of file on a read; `ERR_NOT_FOUND` `ENOENT`; `ERR_ALREADY_EXISTS` `EEXIST`; `ERR_NO_SPACE` `ENOSPC`; `ERR_IO` `EIO`; `ERR_NO_RESOURCES` `EMFILE` or `EAGAIN`; `ERR_NOT_SUPPORTED` `ENOSYS` or `ENOTSUP`; Q9's five (finding 17) `ENOTDIR`, `EISDIR`, `ENOTEMPTY`, `ENAMETOOLONG`, `EBUSY`; `ERR_INTERRUPTED` `EINTR` with M13's signals |

## Design questions

Only what the owner's answers leave open. Each has a recommendation.

**A. Q2: whose job is "the reader's"?** An end has no owner today: a
handle to it can sit in several tables, or in transit.
- (a) The job of the table a handle to the reading end was last put into
  (one store per insert of a channel end; in transit, the last holder).
- (b) The job that made the channel pair.
- (c) Whoever reads it, charged at the read (nothing charged meanwhile).

*Recommendation: (a).* It is who will read it. (b) is wrong for pairs a
server makes and hands out (audio streams, open files), (c) leaves the
memory unbounded until a read that may never come.

**B. D2: how an end gets its "receives no handles" mark.**
- (a) A one-way call, `channel_set_flags(h, CHANNEL_NO_HANDLES)`, on an end
  the caller holds; `ERR_BAD_STATE` if a message carrying handles is queued
  there already.
- (b) A flags argument on `channel_create` (200 callers change).

*Recommendation: (a).* Two supervisors set it; nothing else should pay.

**C. Q1's edges: which calls, and argument errors.**
- (a) Zircon's: once the kernel has read the handle list, every valid
  handle in it is consumed whatever the status, a refused list included (a
  bad value, a handle without `RIGHT_TRANSFER`, the channel's own end); and
  the same for every call that takes a handle away (`process_start`'s
  `arg0`, `handle_replace`, `vmo_make_exec`: finding 5).
- (b) Consumed only once the call is past its argument checks (a bad list
  gives everything back), and for the channel calls only.

*Recommendation: (a).* One rule with no exceptions to remember; argument
errors are bugs anyway, and a caller never has to ask which case it is in.

**D. Read-only pins: the argument (Q13, finding 11).** `vmo_pin` has six
arguments already.
- (a) `vmo_pin` takes a struct (`@vmo_pin_args` with a `flags` word).
- (b) A second call, `vmo_pin_read_only`, with the same six.
- (c) The flag in `len`'s low bits (it is page-aligned).

*Recommendation: (a).* Every pin is made at setup, never per request (all
13 sites), so one more copy costs nothing, and a flags word leaves room.

**E. Port bindings that outlive their handles (finding 26).**
- (a) A binding ends when its object has no handles left anywhere (a
  closed channel end, an event nobody holds: such a binding can never
  fire usefully); a packet already queued stays.
- (b) Zircon's: a binding ends when the handle it was made through closes
  (the binding remembers the table and value; `handle_close` finds it).
- (c) As today, plus `port_unbind(port, key)` by key alone, so a program
  can always undo what it did.

*Recommendation: (a).* It removes the leak class in the kernel, cheaply
(the object's zero-handles hook already exists), and changes nothing a
working program does. A binding on an object other processes still hold
stays the binder's to unbind.

**F. The futex's shape (Q10, finding 30).**
- (a) Private futexes only (keyed by address space and address), a
  `flags` argument reserved now so a shared kind (keyed by VMO and offset,
  for process-shared mutexes and semaphores) is an addition; and
  `futex_requeue(addr, wake, addr2, requeue)` in K4 too, since musl's
  condition variables move waiters with it.
- (b) Wait and wake only; requeue and shared at M13.
- (c) Keyed by VMO and offset from the start: shared works at once, but
  every call looks up the mapping.

*Recommendation: (a).* Requeue is a few lines in the same file and musl's
condition variables need it; shared waits for a program that needs it.

## The fixes, by track

Each track starts with its renames and moves (refactors, one commit), then
its behaviour changes, one commit each with a test that fails without it.

**K1. Channels.**
1. Finding 8: the three structs renamed (a pure rename; every filler of
   the structs changes mechanically) and finding 9: the channel limits into
   `<jam/abi.h>`.
2. Finding 2: `ERR_CANCELED` for a wait ended by its own end's close;
   finding 3: the zero-address checks.
3. Finding 1 (D1): the txid counter per pair, with a ktest of two ends
   calling each other at once.
4. Findings 4 and 5 (Q1, question C): handles consumed on every outcome,
   for the channel calls and `process_start`; libjwl's `write_batch`
   changes in the same merge (named in the report: it is P1's file);
   genidl's "still ours" closes become dead code (G drops them).
5. Finding 6 (D2, question B): the mark and its setter; init and devmgr
   set it on their kept server ends.
6. Finding 7 (Q2, question A): charges move at the writer's close; the
   compositor's lingering slots can go after (P1).
   Bench before and after: the reply-and-wait line.

**K2. Memory and DMA.**
1. Finding 15: the VMO, pin and mapping limits and the two DMA flags into
   `<jam/abi.h>`.
2. Finding 14: `vmo_create` without its `dma_cap`, and `dma_vmo_create`.
3. Finding 13: `pci_bus_master_off`.
4. Finding 10 (D3): `dma_cap_create` takes the job; devmgr's bind makes
   the job first.
5. Finding 11 (Q13, question D): read-only pins; the transmit rings and
   sound buffers use them.
6. Finding 12: `VMAR_KEPT_ONLY`'s error.

**K3. Handles, names, errors, headers** (after K1: both edit
`<jam/abi.h>` and `abi/syscalls.def`).
1. Findings 22 and 23: the service definitions out of the kernel's
   headers, the remaining limits in.
2. Finding 19: the rights renumbered. Finding 18: `sleep_until`.
3. Finding 21: `object_signal` removed.
4. Finding 17 (Q9): the five codes.
5. Finding 16 (Q11): `handle_info`; the keeper's probes and the
   compositor's check use it.
6. Finding 20: one rule for lengths; libos cuts names itself.
7. Finding 5's other two calls (`handle_replace`, `vmo_make_exec`).
8. Finding 26 (question E): bindings end with their object's last handle.
9. Findings 25 and 27: the wait rule written down; user packets' status.
   (Finding 32's `sys_public_info` and finding 34's `cpu_ns` only if the
   owner wants them in M12.)

**K4. Threads.**
1. Finding 24: the entry check in `start_thread`.
2. Finding 29 (Q10): the FS base per thread (`thread_start`'s new argument,
   `thread_set_fs_base`, the switch), with ktests: a thread's FS base
   survives switches and migration, a bad base is refused; the switch's
   bench line.
3. Finding 30 (Q10, question F): `futex_wait`, `futex_wake`,
   `futex_requeue`, then libos's lock on them; ktests: no lost wake, a
   deadline, a bad address, a value that changed.
4. Finding 31: `thread_yield`, if it is a few lines there.

**Now, before the tracks:** findings 2, 3, 24 and 28 are small bugs the
main session can fix at once (or leave to K1, K4 and the join).
