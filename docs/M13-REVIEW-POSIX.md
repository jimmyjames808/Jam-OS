# M13 R1: the POSIX layer's code check

The independent review of M13's POSIX layer (docs/M13-PLAN.md, the R1
row), at main 05618a11, by an agent that wrote none of it. Read in full:

- **A1**: the call layer's core (`user/posix/sys/calls.c`, `calls.h`,
  `layer.h`, `errno.c`, `mem.c`, `maptable.c`, `thread.c`, `time.c`,
  `misc.c`, `kposix.c`), the start (`user/posix/crt/`), the overrides
  (`user/posix/musl/`) and the build (`user/posix/posix.mk`,
  `tools/jamos-cc`, `user/posix/posix.ld`);
- **A2**: libjam's hand-over (`user/include/posixfds.h`, `user/lib/fdmsg.c`)
  read as hostile input, and `user/lib/ns.c` and `user/lib/spawn.c` where
  the layer meets them;
- **B1 to B6**: the descriptor table and the files (`fd.c`, `file.c`,
  `fobj.c`, `path.c`, `dir.c`, `meta.c`), the kinds without a server
  (`pipe.c`, `dup.c`, `dev.c`, `etc.c`, `memfd.c`, `eventfd.c`, `user.c`),
  the processes (`proc.c`, `spawn.c`, `spawnfd.c`, the `posix_spawn`
  override), readiness (`ready.c`, `poll.c`, `select.c`, `epoll.c`), the
  signals (`signal.c`, `sigcalls.c`, `sigthread.c`, `sigtimer.c`, the
  `syscall_cp` and `pthread_cancel` overrides), the sockets (`socket.c`,
  `sockconn.c`, `sockio.c`, `sockwait.c`, `sockgive.c`, `sockopt.c`,
  `resolve.c`, the `lookup_name` override); P2's `rlimit.c`.

T2's `tty.c` and X1's `pty.c` are R2's; they were read only where the
layer's other files call them.

What the call layer was read against: musl's own expectations of each call
(its wrappers in `third_party/musl/src`: which calls are cancellation
points, which results it checks, what it does with `-ENOSYS`), errno values
against Linux's man pages, partial reads and writes, and every handle on
every path (made, given, moved, closed). The hand-over message was read as
hostile input: every record, its data and its handles, each kind's
adopter.

Severity: **High** corrupts memory or a handle, or lets one program break
another; **Medium** a call that gives a wrong answer or loses data in a
common case, or a POSIX interface that can't be used as written; **Low** a
narrow case, a slow path, or a design point.

**0 High, 7 Medium, 14 Low, 4 notes** (items 22 to 25 are notes and
design questions, not bugs). The last column says what was done, with the
commit and its test.

## What held up

- **The hand-over** (`fdmsg.c`): every length is checked before the bytes
  are read (`padded > n - at` before `record_ok` touches the data), every
  string's NUL, every handle's object type, the duplicate and DUP-target
  rules, and a refusal closes the message's handles, resets the sink and
  drains what is queued. The adopters (fd.c's `fd_adopters`) each consume
  their handles on every path, and each checks its own data's size and
  fields (`memfd_wire`, `efd_wire`, `sock_given`, the paths). Nothing a
  starter sends can make the child read or write out of bounds.
- **Handles on every path** in the descriptor table, the kinds, the spawn
  (`posixfds_discard` on every failure before the write, the signal
  channel closed on a failed start, a child the table can't hold killed
  and closed) and the sockets (`sock_adopt` closes all four on any fault).
- **The signal model**: the Dekker pair between a sender and a thread's
  `calls`, the routing, `SA_RESTART`, the spurious-interrupt restart and
  the cancellation flag are consistent with K1's interrupt rules.
- **Readiness**: poll's place cache keyed by the description's serial,
  epoll's `epoll_forget` before a kind's close, the socket places dropped
  at the end of each call.
- **mmap**: `MAP_FIXED` never takes away what libc didn't map, a split
  always has its entry, and `brk` keeps its own VMO handle.

## Findings

| # | Sev | Where | What is wrong | The failure | Done |
|---|---|---|---|---|---|
| 1 | Medium | `user/posix/sys/spawn.c:257-271` | `spawn_child` exports the descriptor table (`spawnfd_export`: sockets move out of the parent, a pipe's leftover bytes are taken) before `launch` looks the program up | `posix_spawn` of a missing program, and every `PATH` directory musl's `execvp` tries before the right one (each try is an `execve`), leaves the parent's inherited sockets `SK_GONE` (ENOTCONN from then on) and drops its pipes' leftover bytes; an `EBUSY` from a file's give fails the whole `execvp` at the first directory | Fixed bb39fdd2: the program is found before anything is exported. posixtest proc: a pipe's leftover survives a spawn of a missing program |
| 2 | Medium | `user/posix/sys/file.c:304-326`, `fobj.c`, `spawnfd.c:135-154` | A file open for writing can't be given to a child: the child's own open meets the parent's, and fat allows one writable open (B1's and S1's limit, queued item 3) | `posix_spawn` and `execve` with an inherited writable file are `EBUSY` (`find -exec cmd > f`, `xargs cmd > f`, `env cmd > f`, `time cmd > f` from the shell); `posix_spawn_file_actions_addopen` with `O_WRONLY` is `EBUSY` always (the action opens in the parent first) | Fixed 6b51c31c: the parent lends its open (closes it) when it gives a writable file, and takes it back at its next use, waiting up to a second for the child's close to reach fat; and (with the commit after, found by ports.txt) a description at the file's end follows the end its child left, as a shared offset would. posixtest files (with and without O_APPEND, two children in a row, an addopen for writing); ports.txt (xargs and env into a file) |
| 3 | Medium | `user/posix/sys/calls.c` (no entry), `time.c` | `times` and `getrusage` have no handler (`-ENOSYS`), though the plan builds them on K1's `cpu_ns`; musl's `times` returns the raw `-ENOSYS` as a `clock_t`, never `-1` | sbase's `time` (in the boot image) reads a `struct tms` nothing filled and prints stack garbage as user and system time; `getrusage` fails | Fixed 29f59670: `times` and `getrusage` (self, the reaped children, the thread) on `cpu_ns`. posixtest proc |
| 4 | Medium | every `user/posix/sys/*.c` | The layer's 369 global names (`fd_get`, `path_get`, `poll_run`, `file_map`, `sig_call`, `lx_read`, ...) are in `libc.a`, in the program's own name space (B2's note) | A port that defines one of them fails to link (a duplicate definition) or has the layer call its function | Fixed 8d672d81: `libc.a` holds the layer's objects with every global name but `__*` and `jamos_*` renamed `__jl_<name>`, by `objcopy` from a list `tools/posixnames.py` makes. make check links `tools/posix-link-test.c` |
| 5 | Medium | `user/posix/sys/calls.c:23-215`, the Makefile's libjam | The handler table names every handler, so every program links the whole layer; libjam's objects are built without per-function sections, so `--gc-sections` can't drop their unused functions (queued item 1) | Every static POSIX program is about 300 KB stripped (sbase's `true` 299,968 bytes, `ls` 343,496); the boot image was near `update`'s 32 MiB limit before P1's multi-call boxes | Fixed dfa6d1a8: per-call references ("Program size" below); make check fails if a program that calls only `write` links `execve`'s, `setsockopt`'s or `timer_create`'s handler |
| 6 | Medium | musl's `src/network/res_msend.c` (not overridden) | `res_query`, `res_send` and `getnameinfo`'s reverse lookup use musl's DNS client toward resolv.conf's servers; libc's names none, so musl asks 127.0.0.1 (queued item 2) | Each such call waits musl's whole timeout (about 10 s) before it fails or `getnameinfo` gives the numeric host | Fixed 9524044c: `res_msend.c` overridden to fail at once (the system resolver knows A records only: nothing to route a raw query to). posixtest socket (fails before only with the network: `tools/posix-net-test.sh`) |
| 7 | Medium | `user/services/netstack/clients.c:458-475`, `sock.c:438-452`, `tcpsock.c:283-297`, `479-494` | `serve_opener`, `sock_serve`, `ts_serve` and `l_serve` print and stop on `ERR_NO_MEMORY` (netstack couldn't allocate the buffer to drop an oversized message): the message stays queued and the PERSISTENT binding never fires again (queued item 5) | One program's oversized message while netstack's heap is short leaves that program's channel unread for good | Fixed c808c704: the channel is held (`progs_behind`) and served at the next look, as one past the unread bound. No test: needs netstack's heap to fail inside the generated server |
| 8 | Low | `user/posix/sys/thread.c:52`, `100-113`, `205` | `self_tid` is thread-local, and musl's `SIGEV_THREAD` thread resets its TLS after each notification (B4's note) | In a timer's notification `gettid` is 0, `CLOCK_THREAD_CPUTIME_ID` reads the main thread's time, and the thread's handle stays open when it ends | Not reproduced: posixtest signal's new check (gettid in three notifications, the same thread's) passed before any change on the PC; thread.c unchanged, the check kept (ea7bf209) |
| 9 | Low | `user/posix/sys/spawnfd.c:56-59` | `spawnfd_umask` returns its fallback (the hand-over's, or 022): a `umask()` the program made is never handed on | A child gets the starter's umask, not ours | Fixed c348a720: meta.c's umask. posixtest proc |
| 10 | Low | `user/posix/sys/spawnfd.c:208-216` | `exported_as` walks every exported record and `find`s each, for every slot: cubic in the descriptors | A spawn from a program with 1,000 open descriptors takes about 10^9 steps | Fixed 2bdcc938: each record keeps its description. No test (a time limit would be flaky) |
| 11 | Low | `user/posix/sys/dup.c:147-151` | `F_DUPFD` with `arg` at or past `RLIMIT_NOFILE` is `EMFILE` | Linux and POSIX say `EINVAL` | Fixed 46cb1d89. posixtest fds |
| 12 | Low | `user/posix/sys/calls.c` (no entry) | `flock` has no handler (`-ENOSYS`), while `fcntl`'s record locks are accepted and granted | A program that locks with `flock` fails where one that uses `fcntl` works | Fixed 35874c5e. posixtest fds |
| 13 | Low | `user/posix/sys/calls.c` (no entry) | `mlock`, `munlock`, `mlockall`, `munlockall` have no handler; Jam OS never pages memory out, so they have nothing to do | A program that keeps keys locked (SSH, crypto) fails or aborts | Fixed 65937d7d. posixtest mem |
| 14 | Low | `user/lib/ns.c:264-268` | The first namespace wait ends at `ERR_INTERRUPTED` (K1's interrupt), and the namespace message comes after `process_start` (`user/lib/spawn.c:387`) | A signal routed to a thread in a program's first path call (microseconds after the start) makes that one call `ENOENT`; later calls see the mounts | Fixed 1f6e82ef. No test: the window is the starter's write right after the start |
| 15 | Low | `user/posix/sys/sigthread.c:99-111` | `take_channel` stops at the first error but `ERR_SHOULD_WAIT`, with no next turn: an `ERR_NO_MEMORY` from the generated server leaves the starter's signal channel queued and unbound for good (the same corner as 7) | No more signals from the parent | Fixed c4f22d83: a stop waits 10 ms and asks for the next turn (`port_again`). No test: needs the heap to fail in the server |
| 16 | Low | `user/posix/sys/sockgive.c:68-90` | `struct sock_given` has implicit padding (`struct sock_opts`' five bools), filled by a compound literal: its padding bytes are whatever was on the stack | Stack bytes cross into the child (the same trust: the parent made it), against CODING-GUIDE's rule for wire formats | Fixed 21ff8aed. No test: the child can't tell padding from ignored fields |
| 17 | Low | `user/posix/sys/meta.c:326-340` | The first `umask` calls of two threads at once: one may set `umask_ready`, the other exchange the default, then the first store the hand-over's over the other's new mask | One thread's `umask()` lost | Fixed with 9 (c348a720): the umask is under a lock |
| 18 | Low | `user/posix/sys/sigthread.c:58-75`, `80-87` | While `execve` waits, the signal thread forwards each signal with `sig_channel_send`, which waits up to a second for room on a full channel | A stuck program behind `execve` delays every later signal by a second each | Listed |
| 19 | Low | `user/posix/sys/memfd.c:135-153` | A child's memfd object knows the VMO's size as it was at the spawn; if the parent grows it after, the child's own grow asks `vmo_set_size` for less than the VMO has, which a kept VMO refuses | A child's write past the end of a memfd its parent grew fails (EFBIG) | Listed |
| 20 | Low | `user/posix/musl/src/thread/x86_64/syscall_cp.c`, `__syscall_cp.c` (upstream) | musl's `__syscall_cp.c` has a fallback (`sccp`) that makes the call with a run-time number | Part of finding 5's fix: an override keeps it from naming every handler | Fixed with 5 (dfa6d1a8) |
| 21 | Low | `user/posix/sys/meta.c:245-251` | `utimensat` and `futimens` succeed and change nothing (queued item 4, confirmed): FAT keeps a modification time, but the `file` protocol has no call to set it | `touch` of an existing file, `cp -p`, `tar` extraction keep the old times | Listed: needs a `file`/`fs` protocol method (not M13's) |
| 22 | Note | `user/posix/sys/sockgive.c` | A socket given to a child moves (its ring VMO can't be duplicated): every `posix_spawn` without `FD_CLOEXEC` on a socket takes it from the parent | A program with an inherited connection that runs `system()` or `popen()` loses the connection | Design question for the owner (below) |
| 23 | Note | `user/posix/sys/calls.c` | The calls musl makes that answer `-ENOSYS` (the plan's risk list asks R1 for them) | See "Calls that answer ENOSYS" below | Listed |
| 24 | Note | libjam in `libc.a` | libjam's own global names (`spawn`, `now`, `lock_take`, `file_open`, `fs_stat`, `svc_get`, ...) share the program's name space too | A port defining `now` or `spawn` clashes | By design (libjam is the Jam OS API ports may call); listed for M14's review of what a ported binary freezes |
| 25 | Note | `user/posix/sys/socket.c:292-324` | `close` of a connection waits up to 10 s for netstack to take what was written (B6's choice: netstack drops the rest when the channel closes) | A program closing a connection to a stalled peer blocks up to 10 s | B6's documented design; listed |

### The queued items

1. **Program size** (finding 5). Confirmed with a link map of a one-line
   `main` (`-Wl,-Map`): `calls.c`'s table pulls in every handler's object
   and from them every kind, the spawn, the sockets, the timers; and
   libjam's objects come in whole (no `-ffunction-sections`), so
   `--gc-sections` drops nothing of `fs.c`, `ns.c`, `net*.c`, `spawn.c`.
   The two measured apart: libjam with per-function sections alone took
   `true` from 242,407 to 220,391 bytes of text; the table alone (a test
   table of the dozen calls `true` makes) to 174,407. The fix is in the
   section below; the numbers are in "Program size".
2. **DNS reverse lookups** (finding 6): `res_msend.c` overridden to fail
   at once. Routing through `/svc/dns` isn't possible: its protocol has
   `resolve` (A records) only, and these calls send raw queries of any
   type (PTR for `getnameinfo`).
3. **EBUSY on children** (finding 2): found and fixed. The cause is the
   one the item guessed: `fobj.c` keeps the file's one writable open (fat's
   rule, `user/services/fat/fileops.c` "The lock"), and `kind_give`'s
   `reopen` for the child meets it. The fix lends the open; what still
   can't be done before Q5's (B) (the `file` protocol's clone, M13.5/M15):
   the parent and the child writing the file at the same time (the
   parent's take-back is then `EBUSY` until the child closes), and a
   shared offset in general: each moves its own, but a description that
   was at the file's end when its child got the file moves to the end the
   child left (what a shared offset gives the usual `cmd > f` that starts
   children: `xargs`, `find -exec`); one that had seeked elsewhere stays.
4. **utimensat and futimens** (finding 21): confirmed no-ops.
5. **netstack's OOM corners** (finding 7): fixed with the hold the loops
   already use for the unread bound rather than `port_again`: `port_again`
   queues the next turn at once, which under a lasting shortage turns the
   loop into a spin that prints a line a turn; the hold looks again after
   `IDL_UNREAD_LOOK_NS`.

### Calls that answer ENOSYS

musl's wrappers that name a call the layer has no handler for (x86-64
numbers, `src/` less `ldso`), and what a program sees:

- **On purpose**: `fork`, `vfork`, `clone` of a process (Q4); `execveat`
  (so `fexecve` fails: no `/proc`); SysV IPC (`shmget` and kin, `semget`,
  `msgget`), POSIX message queues (`mq_open` and kin), `inotify`,
  `fanotify`, `signalfd`, `timerfd` (not built: M13's plan has none of
  them); `mount`, `umount2`, `pivot_root`, `chroot`, `swapon`, `reboot`,
  `init_module`, `acct`, `quotactl`, `settimeofday`, `adjtimex`,
  `clock_adjtime`, `sethostname`, `setdomainname`, `ioperm`, `iopl`,
  `ptrace`, `process_vm_*`, `setns`, `unshare`, the xattr calls.
- **That a port may meet**: `getpriority`/`setpriority` (`nice`),
  `sched_getparam`, `sched_getscheduler`, `sched_setparam`,
  `sched_setscheduler`, `sched_get_priority_min/max`,
  `sched_rr_get_interval`, `sched_setaffinity` (so
  `pthread_getschedparam` fails), `prctl` (`PR_SET_NAME`), `sendfile`,
  `copy_file_range`, `splice`, `tee`, `vmsplice` (callers fall back to
  read and write), `msync` (F1's), `mincore`, `readahead`,
  `sync_file_range`, `get_robust_list`/`set_robust_list` (robust mutexes:
  not planned), `membarrier` (musl falls back), `statx` (musl falls back
  to `fstatat`), `utimes`/`futimesat` (musl's go through `utimensat`:
  fine), `getdents` (musl uses `getdents64`), `fchmodat2` (musl falls
  back), `capget`/`capset`, `personality`, `syslog`, `vhangup`.

`times`, `getrusage`, `flock` and the `mlock` family were on this list
(findings 3, 12, 13) and are fixed.

### Design questions for the owner

- **A socket given to a child moves** (22). Today every `posix_spawn`
  takes each socket the program didn't mark `FD_CLOEXEC`. Options: (A)
  keep it (inetd's way works; `system()` in a program with an open
  connection breaks it); (B) give a socket only when a file action names
  it (`adddup2` onto the child's descriptor), leaving inherited ones in
  the parent (Linux programs that pass a socket by inheritance alone
  break); (C) a ring VMO that can be duplicated, so both keep it (a
  kernel and netstack change: two processes on one ring). Recommendation:
  (B) once a port needs it; nothing in M13 does.
  **Answered 2026-10-11: (B)** (the owner). A socket goes to a child only
  when a file action names it; inherited sockets stay in the parent.
  Queued as a small track after the owner's launch hold.

## Program size

How a program now links only what it calls (finding 5):

- **libjam in `libc.a`** is built again for the sysroot with
  `-ffunction-sections -fdata-sections` (`user/posix/posix.mk`), so
  `--gc-sections` (already in `tools/jamos-cc`) drops what isn't used.
  Native programs and `libjam.a` are unchanged.
- **The table names each handler weakly** (one list of the calls, one line
  each: `LX` or `LX_CP`, read by `calls.c` and by the override), so a handler's object is linked only
  when something names it. Each of musl's system call sites names it: the
  override of `syscall_arch.h` puts a relocation that names the call's
  handler (`R_X86_64_NONE`: no bytes change) beside each inline
  `__syscallN` with a constant number, and one that names
  `__jam_calls_all` (every handler) where the number is only known when
  it runs (musl's public `syscall()`), so nothing can call a handler that
  wasn't linked. The cancellation points (`syscall_cp`, called by name,
  not inline) are `LX_CP`: always linked. musl's `__syscall_cp.c` is
  overridden so its fallback (`sccp`) calls the layer directly instead of
  naming every handler.

Sizes, stripped of debug as the boot image holds them (sbase's `true`
and `ls` linked alone, as one-tool boxes):

| Program | Before | After |
|---|---|---|
| `true` | 299,968 | 248,264 |
| `ls` | 343,496 | 301,592 |
| sbase-plain | 439,696 | 394,320 |
| sbase-ro | 515,832 | 479,760 |
| sbase-rw | 544,560 | 508,496 |
| lua | 614,312 | 577,512 |
| posixtest | 560,120 | 550,528 |
| the boot image (`build/bootfs.img`) | 17,342,464 | 17,186,816 |

What every program still carries (about 200 KB of `true`'s text): the
kinds a hand-over may give it (files and their paths, pipes, terminals,
pseudo-terminals, sockets, memfds, eventfds, /dev, /etc: a child given a
socket as descriptor 0 must read it whatever its own code calls), the
cancellation points' handlers (poll, select, epoll_wait, connect, accept,
the waits), the signal thread, musl's `vfprintf` (libjam's error lines),
and malloc. Making the kinds' code optional too would need the hand-over
to say which kinds a program takes: a question for the porting work, not
a bug.

## Also done

From M13 R3's notes (the kernel review):

- An interrupted `nanosleep`'s time left was the deadline less the
  uptime, negative if the deadline had saturated; it is now the request
  less the time slept (d503f061; posixtest signal, which passes either
  way: a relative deadline saturates only past 2^63 ns of uptime).
- A futex word the kernel can't read in (PK2's `ERR_IO`) is `EFAULT` in
  libc, `ERR_CANCELED` stays `EINTR` (ffda5170). The kernel's futex_wait
  passes both through since 39aec118 (it had turned them into
  `ERR_INVALID_ARGS`, `EINVAL`).
