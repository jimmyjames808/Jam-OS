# M13 plan: POSIX on musl

Status (2026-10-09): a plan, nothing built. Written against main ae859bee,
with M12's waves C1 and C2 merged, S2a merged and P6a running. **Waiting
for the owner's answers to the questions below before any agent starts.**
M13 is part of wave 5 of [PLAN-M11-M12.5.md](PLAN-M11-M12.5.md#wave-5-m125-m127-and-m13-together),
beside M12.5 (pagers) and M12.7 (NVMe). It becomes **release 0.2.0**
(AGENTS: "0.2.0 at POSIX").

Goal ([roadmap](ROADMAP.md#later)): **POSIX on musl: file descriptors over
handles, `posix_spawn` (no `fork`), paths through the namespace,
`poll`/`select` on M9.5's wait sets, a terminal layer (pseudo-terminals: a
shell that talks to a connection instead of the screen), then ported
programs. The first test port is tinyssh**: an SSH server, key login only
(no passwords), a host key made on first boot, `authorized_keys` in
`/data/etc`, the listen permission, one process per connection (its `fork`
replaced by `posix_spawn`).

**Done when** (the roadmap's row made concrete):
- **unmodified POSIX programs build and run**: the ported utilities
  (question 9: sbase) and a small C program (Lua 5.4, and the plan's own
  `posixtest`), built on the Mac by one compiler wrapper against one
  sysroot, run from the Jam OS shell in a terminal window and over SSH,
  with pipes, redirections and Ctrl+C;
- **`ssh` from the Mac opens a Jam OS shell on the PC**: `ssh jam@<pc>`
  with the owner's key, the native Jam OS shell on a pseudo-terminal,
  window resizes followed, Ctrl+C reaching the program, `exit` ending the
  session; a key not in `/data/etc/authorized_keys` refused, passwords
  never asked; the host key made on the first boot and the same on every
  boot after;
- a program built on the Mac, copied to `/data` and approved with `allow`,
  runs (the same rule as a native program's);
- musl's own tests (libc-test's functional and regression sets) pass but
  for a written list of expected failures, each with its reason;
- every kernel addition has its ktests, and the call path is no slower
  (the reply-and-wait line against M12's);
- and, as every milestone: All tests and `soak 10` on the PC.

## What M13 is not

- **No `fork`**, ever ([ARCHITECTURE.md](../ARCHITECTURE.md#goals)): a
  ported program that forks is patched (an override file) or not ported.
  `vfork` neither (question 4).
- **No dynamic linking** (M14.5): static ELF only, musl's dynamic loader
  left out of the build. M13 must not make M14.5 harder (below, "What M13
  must not block").
- **No POSIX shell port.** The Jam OS shell is the shell, in a window and
  over SSH. Every POSIX shell (dash, mksh, ash) forks for subshells and
  `$(...)`; that is M15's problem (question 4's option B is its answer).
- **No job control** (Ctrl+Z, `fg`, `bg`): the kernel has no stopped
  state for a process. SIGTSTP is ignored by default; an addition later.
- **No users**: one user, no accounts ([ARCHITECTURE.md](../ARCHITECTURE.md#goals)'s
  "Users"; question 7 picks its name and ids).
- **No `/proc`, no `ptrace`, no IPv6, no `sftp`/`scp`** (tinyssh's
  subsystems stay off), no TLS.
- **No freezing.** M14 freezes, after reviewing what M13 adds (the list
  in "The new and changed interfaces" below is written for it).
- **No change to fat or the `file` protocol** if question 5 takes its
  recommendation (offsets stay in libc).

## Where M13 starts

What M12 built for it, and what is still missing. "Have it" means on main
or in an M12 track already merged; the K review's numbers are
[M12-REVIEW-KERNEL.md](history/M12-REVIEW-KERNEL.md)'s findings.

| Need | State | Where |
|---|---|---|
| Thread-local storage | have it: `thread_set_fs_base`, `thread_start`'s FS base | `abi/syscalls.def` (K4) |
| Futexes | have it: `futex_wait`, `futex_wake`, `futex_requeue` (private) | `kernel/sched/futex.c` |
| Yield | have it: `thread_yield` | K4 |
| Error codes for files | have it: `ERR_NOT_DIR`, `ERR_IS_DIR`, `ERR_NOT_EMPTY`, `ERR_NAME_TOO_LONG`, `ERR_BUSY` | `kernel/include/jam/status.h` (K3) |
| Handles always moved on a send (Q1) | have it | `abi/syscalls.def` "channels" |
| `handle_info` (Q11) | have it: type, rights, koids, a channel end's queue counts | `kernel/include/jam/abi.h` |
| The file protocol for POSIX (Q8) | have it: paths as `str[<=4095]`, batched `readdir`, `FS_EXCL`, `is_dir`, `id` (`st_ino`), unlink's `UNLINK_DIR`, rename's `RENAME_REPLACE` | `abi/idl/fs.idl`, `abi/idl/file.idl` (P5) |
| A pipe is a channel of byte messages (Q17) | decided, and `SR_STDOUT` already is one | `user/lib/stdio.c` |
| The heap reserves 1 GiB (Q16) | P6a, running | `user/lib/heap.c` |
| Waiting on many sockets and handles | have it: wait sets, level-triggered, O(ready) | `user/include/netwait.h` |
| Sockets, TCP, the listen permission | have it: `net`, `net_socket`, `net_stream`, `net_listener`; `/svc/net-listen`, `/svc/net-low` | `abi/idl/net.idl` |
| Keys from a terminal's bytes | have it: VT100 sequences to HID usages | `user/include/termkeys.h` |
| Time zones as POSIX rules | have it: named zones with their daylight rules | `user/include/wallclock.h` |
| Ed25519 keys | have it: Monocypher, vendored | `third_party/monocypher/src/optional/monocypher-ed25519.c` |
| A peer that forwards the Mac's TCP into QEMU | have it: `--tcp-forward` (OpenSSH can reach a guest) | `tools/tcprelay.py` |
| Small process and thread ids (K 33) | missing | M13 track K1 |
| A thread's exit clearing its id word (K 38) | missing | K1 |
| Interrupting a blocked call, `ERR_INTERRUPTED` (K 35) | missing | K1 |
| `uname`/`sysconf` without a power (K 32) | missing | K1 |
| CPU time per process and thread (K 34) | missing | K1 |
| Clocks without a system call (Q19: "the time page at M13") | missing | K2 |
| `MAP_FIXED` over a mapping (K 36) | missing | K2 |
| Mapped files, copy-on-write (K 37) | M12.5 | track F1, after M12.5 |
| A socket's options (no-delay, keep-alive), a listener's address reuse (protocols review 32) | missing | N1 |

## The design

### 1. How musl meets Jam OS

musl's system call layer assumes Linux: every wrapper ends in
`__syscall(SYS_x, ...)` (`src/internal/syscall.h`), which
`arch/x86_64/syscall_arch.h` turns into the `syscall` instruction with
Linux's numbers. A handful of files issue the instruction in assembly
(`src/thread/x86_64/`: `clone.s`, `__set_thread_area.s`, `__unmapself.s`,
`syscall_cp.s`; `src/process/x86_64/vfork.s`; `src/signal/x86_64/restore.s`).

| | (a) A Linux call layer behind `arch/x86_64/syscall_arch.h` | (b) musl's wrappers replaced file by file | (c) Another libc |
|---|---|---|---|
| What changes in musl | nothing: musl is vendored unmodified, and about 12 files are **overridden** by the build (our file compiled in place of upstream's), `arch/x86_64/syscall_arch.h` first among them | several hundred wrappers (`open`, `read`, `poll`, ...) rewritten | none (mlibc: a "sysdeps" interface built for hobby OSes, C++ and meson; relibc: Rust; newlib, picolibc: thin POSIX, no threads worth the name) |
| How a call runs | musl's wrapper builds Linux's arguments, our `__jam_syscall(n, a..f)` switches on the Linux number into C code over libjam; an unknown number is `-ENOSYS` | musl's public function is ours directly | the libc's own |
| Updating musl | drop in the new release; a check fails the build if an overridden upstream file changed (its hash is recorded), so each is looked at again | a three-way merge of hundreds of files | the other libc's cadence |
| Upstream-mergeable | not upstream's to take (musl is Linux-only by policy), but **rebaseable in minutes** | no | n/a |
| Cost | one switch per call (nanoseconds against a channel call's ~800 ns); Linux's argument shapes (`struct stat`, `iovec`, `sigaction`'s kernel layout) translated in one place | the most code, and the most chances to diverge from musl's semantics | a new language or build system (mlibc), or weaker POSIX |
| Programs that call `syscall(SYS_x, ...)` directly | work (the same switch) | don't | depends |

**Recommendation: (a)** (question 1). It is how Emscripten and Unikraft
carry musl, and it keeps musl's code (its stdio, locale, malloc, pthreads,
the parts that are hard to get right) exactly upstream's. The layer lives
in **user/posix/** (new), in three parts:

- **The overrides** (user/posix/musl/, each file named as upstream's and
  listed with upstream's hash in user/posix/musl/OVERRIDES): `arch/x86_64/syscall_arch.h`
  (every `__syscallN` a call to `__jam_syscall`; no vDSO), crt1 (below),
  `__set_thread_area` (`thread_set_fs_base`), `clone` (a thread from
  `thread_create` and `thread_start`, its TLS as the FS base; only thread
  flags: anything else `-ENOSYS`), `__unmapself` (unmaps its own stack with
  `vmar_unmap` and ends with `thread_exit`, registers only),
  `syscall_cp` and `src/thread/pthread_cancel.c` (cancellation through section 6's
  interrupt instead of a signal and an instruction-pointer check), `vfork`
  (`-ENOSYS`), the signal restorer (unused: section 6), `src/process/posix_spawn.c`
  (section 5: musl's own clones a vfork child), and `src/network/lookup_name.c`
  (names through `/svc/dns`, section 8). Nothing else of musl's is touched.
- **The Linux call layer** (user/posix/sys/): `__jam_syscall`'s switch and
  one file per area (fd.c, file.c, dir.c, pipe.c, proc.c, signal.c,
  poll.c, socket.c, mem.c, thread.c, time.c, tty.c, misc.c), plus the
  errno table (`ERR_*` to `-E*`, review K's mapping, `ERR_INTERRUPTED` to
  `EINTR`).
- **The extensions** a ported program may call where POSIX has no word
  (user/posix/include/jamos/: `jamos_session_open`, the spawn attribute
  that narrows a child's namespace), and the libc-side readers of Jam OS's
  formats (the startup message, the hand-over message of section 5).

**Startup.** A Jam OS process starts with the startup channel in rdi, not
Linux's stack of argc, argv, envp and auxv. Our crt1 (user/posix/crt/)
reads the startup message with libjam's parser, then the POSIX hand-over
message if one follows it (section 5), builds Linux's block on its stack
(argc, argv, envp, and an auxv with `AT_PAGESZ`, `AT_RANDOM` from
`random_get`, `AT_HWCAP`, `AT_PHDR`/`AT_PHNUM` from `__ehdr_start`, so
musl's `__init_tls` finds `PT_TLS`, `AT_SECURE` 0), and calls musl's
unmodified `__libc_start_main`. `TZ` is set from the wall clock's zone
(`<wallclock.h>`'s rules written as a POSIX TZ string) when the
environment has none; `PWD`, `HOME`, `PATH`, `USER`, `LOGNAME`, `SHELL`
and `TERM` come from the starter (the shell sets them).

**The register rule** holds by itself: every call goes through C
functions, and the only assembly that issues `syscall` (the three thread
files above) lists the vector registers as clobbered.

### 2. libjam: libos's core under musl

A ported program links musl and needs libos's namespace, file calls,
`spawn`, sockets, wait sets, IDL stubs and startup parser, but not libos's
own `malloc`, `printf`, string functions, `environ` and crt0, which would
clash with musl's. **libjam.a** is libos built without those (the
Makefile's LIBOS_SRCS minus `user/lib/heap.c`, `user/lib/string.c`,
printf's formatting, `user/lib/crt0.S`, and start.c's `environ` and
`main` call, which split off into a file of their own); libos.a stays as
it is for native programs (libos = libjam + those parts). libjam's code
calls `malloc` and `snprintf` by their standard names, so under musl it
gets musl's. A link test (musl + libjam, `--fatal-warnings`, no symbol
defined twice) runs in `make check`.

Native programs are not touched: they keep libos, its `printf` to
`SR_STDOUT` or `debug_write`, and their 16-handle startup message. A
POSIX program is marked in its list (`<wants.h>`): the word **`posix`**,
which the compiler wrapper adds; the shell reads it to give the program
file descriptors and an 8 MiB stack (section 5), and
`tools/checkwants.py` accepts it.

### 3. File descriptors over handles

**The table** is libc's (user/posix/sys/fd.c): an array of slots (1024 to
start, `RLIMIT_NOFILE`'s answer), each pointing to an **open file
description** (refcounted: `dup` shares one, as POSIX says) holding a
kind, its handles, an offset, the status flags (`O_APPEND`, `O_NONBLOCK`,
the access mode) and the kind's operations (read, write, seek, stat,
ready, arm, close, hand over). `FD_CLOEXEC` is per slot. One lock for the
table; each description has its own for its offset.

| Kind | Handles | Read / write | Ready for poll |
|---|---|---|---|
| file | the `file` channel and its buffer VMO (`<os.h>`'s `struct jfile`) | `file.read`/`write` at the description's offset; `O_APPEND` writes at the end (`FS_APPEND`) | always |
| directory | none: the absolute path (and its mount) | `getdents64` from `fs.readdir`'s batches, the cookie as `d_off` | always |
| pipe end | a channel end, read-only or write-only rights | section 4 | the channel's `SIG_READABLE` / `SIG_WRITABLE`, `SIG_PEER_CLOSED` |
| socket | a socket's channel, ring VMO and two events (`<sockring.h>`) | the rings (libjam's `net_*`) | the wait set's socket entry |
| listener | a `net_listener` channel | `accept` | its channel readable |
| terminal | the console's `tty` channel and two pipes (section 9) | the input pipe / the output pipe | the input pipe readable, the output pipe writable |
| pseudo-terminal master | the session's master channel and its control channel (section 9) | bytes both ways | the master readable / writable |
| `/dev/null`, `/dev/zero`, `/dev/full`, `/dev/urandom`, `/dev/random` | none | as Linux's | always |
| `/etc` file | none: bytes made by libc (below) | read only | always |
| memfd, `shm_open` | a kept VMO (`VMO_KEEP_PAGES`), grown with the file (the Wayland review's W15) | `vmo_read`/`vmo_write` | always |
| eventfd | an event and a counter in memory | 8-byte reads and writes | the event's user bit |
| epoll | a wait set (section 7) | none | its port |

- **Paths through the namespace.** libc keeps the current directory as an
  absolute path; a relative path is joined to it, cleaned (`.` and `..`;
  `..` stops at the mount's root, as libos's lookup does), and libjam's
  namespace finds the mount by its first name. `/` lists the mounts.
  `chdir` checks the path is a directory; `fchdir` takes a directory
  descriptor's path. **Directory descriptors are paths** (`openat`,
  `fstatat`, `unlinkat`, `renameat`, `mkdirat`, `fdopendir` join to them):
  no change to the `fs` protocol (its own directory handles stay an
  addition for later, as M12's Q8 left them). A directory renamed under an
  open descriptor is not followed (FAT has no inode to follow anyway).
- **`stat`** is made up from `fs.stat`: `S_IFDIR` or `S_IFREG`, mode 0755
  for directories and programs under `/boot`, 0644 otherwise, 0444 on a
  read-only view; `st_ino` the file's `id`, `st_dev` the mount's place in
  the namespace, `st_nlink` 1, the owner the one user (question 7), the
  three times the modification time, `st_blksize` 64 KiB (the transfer
  buffer). `chmod`, `chown`, `utimensat` succeed and change nothing on
  FAT, but for a read-only view (`EROFS`). `link`, `symlink`, `mknod`,
  `mkfifo`: `EPERM`/`ENOSYS` (FAT has none). `readlink`: `EINVAL`.
  `access` answers from the view's flags. `umask` is kept and ignored.
- **`/dev`** is libc's (no service): the nodes in the table, `/dev/tty`
  (the process's terminal, if it has one), `/dev/stdin`, `/dev/stdout`,
  `/dev/stderr`, `/dev/fd/N` (a `dup`), `/dev/ptmx` (section 9). **`/etc`**
  is libc's too when the namespace has no `/etc` mount: `passwd` and
  `group` (the one user), `hosts` (localhost), `services` (a short list:
  ssh, http, domain, ntp), `/etc/resolv.conf` (empty: names go to `/svc/dns`),
  `localtime` (none: `TZ` is set), `shells`. `/tmp` is not given: `TMPDIR`
  is honoured, and a RAM `/tmp` (the tests' `user/tests/ramfs` made a
  service) is a follow-up.
- **errno**: one table, from review K's mapping; `ERR_PEER_CLOSED` is end
  of file on a read and `EPIPE` on a write; `ERR_SHOULD_WAIT` `EAGAIN`;
  `ERR_NO_RESOURCES` `EMFILE` for descriptors, `EAGAIN` otherwise.
- **Inheritance** across `posix_spawn` and `close-on-exec`: section 5.
- **Offsets** of a description given to a child: question 5. The
  recommendation keeps them in libc: the child starts at the parent's
  offset at the moment of the spawn, then each has its own; `O_APPEND` is
  exact (the server appends). A file handed to a child gets **its own
  channel and buffer** (a second `fs.open` of the same path with the same
  flags, done by the parent before the spawn), because two processes
  sharing one buffer VMO would overwrite each other's transfers; the
  write-sharing rules are fat's (it already serves several opens of one
  file).

### 4. Pipes

M12's Q17: **a pipe is a channel of byte messages, closed at the end.**
`pipe2` makes a channel pair, narrows one end to read (`RIGHT_READ`,
`RIGHT_WAIT`, `RIGHT_TRANSFER`, `RIGHT_DUPLICATE`) and the other to
write, with `handle_replace`.

- **Write**: a message per `write` up to 64 KiB (larger ones split);
  `PIPE_BUF` is 4096, and any write up to 64 KiB is atomic. A full queue
  (1024 messages) is `ERR_SHOULD_WAIT`: `EAGAIN` with `O_NONBLOCK`, else a
  wait for `SIG_WRITABLE`. The reader gone: `EPIPE` and SIGPIPE.
- **Read**: the next message into the caller's buffer; what doesn't fit
  stays in the description's leftover buffer (libc's, per process), read
  first next time. `ERR_PEER_CLOSED` with nothing queued is end of file.
- **Who pays**: the writer's job, until the reader reads or closes, also
  after the writer has closed (M12's Q2 as the owner changed it on
  2026-10-09). A pipe is full at 1024 messages or 1 MiB of unread bytes
  (its ends are marked `CHANNEL_BYTE_BOUND`), against Linux's 64 KiB; a
  writer of single bytes fills it at 1024 bytes. (Bounded by the
  message count alone, a pipe of 64 KiB writes held up to 64 MiB, the
  root job's whole message budget: a writer far ahead of a slow reader
  got ENOMEM instead of waiting, and every other writer with it.) Measured in BENCH.md (a new line: bytes a
  second through a pipe between two processes, 4 KiB and 64 KiB writes,
  beside Linux's on the same PC); a byte stream object stays an addition
  if the numbers ask for it.
- **Several readers or writers** (a pipe end in two processes) work: the
  kernel end stays open while any handle to it does, so end of file comes
  when the last writer closes. Two readers each take whole messages; a
  leftover is the reader's own (Linux would split bytes between them
  anyway).
- `socketpair(AF_UNIX, SOCK_STREAM)` is two pipes' worth: one channel,
  both ends read and write. No descriptor passing over it (`SCM_RIGHTS`)
  in M13.

### 5. Processes: `posix_spawn` and what a child gets

`posix_spawn` and `posix_spawnp` (our override of `src/process/posix_spawn.c`) start
the program with libjam's `spawn` (user/lib/spawn.c), so the loader, the
job and the namespace grants are the native ones:

- **The program**: a path through the namespace (`posix_spawnp` searches
  `PATH`). A program in `/boot` runs from the boot image; one on `/data`
  needs `vmo_make_exec` (`RIGHT_ROOT_VMEX`: only init and the shell) until
  M12.5's pager loads it the clean way, so from a POSIX program it is
  `EACCES` until then (M12.5's loader, then track F1, open it, checked
  against `allow`'s hash).
- **Its job**: the parent's own (`SR_JOB`), so the shell's kill of a
  program's job takes everything it started, as today.
- **Its namespace**: everything the parent has (`NS_ALL`), as a Unix child
  sees the parent's files; an extension attribute,
  `posix_spawnattr_setgrants_np`, narrows it to a grants list (`<os.h>`
  "grants"), which tinyssh's listener uses.
- **Its stack**: 8 MiB of address space, committed as touched (a new
  `stack_size` in `struct spawn_args`; 0 keeps the 128 KiB default).
- **Its descriptors, and the rest of what `exec` keeps**: the **POSIX
  hand-over message**, a hand-written format (user/include/posixfds.h,
  magic "JPSX", a line in `abi/idl/ids.txt`'s registry as the startup
  message has), written by the parent on the startup channel right after
  the startup message: the descriptors (number, kind, status flags,
  offset, how many handles; the handles in order: a duplicate of each
  handle, or for a file a fresh open, section 3), the current directory,
  `umask`, the signal mask and the ignored signals (POSIX keeps both
  across `exec`), the parent's pid, the process group and session. As many
  messages as 64 handles each need, the last one marked. Descriptors with
  `FD_CLOEXEC` are not sent; `posix_spawn_file_actions` (`adddup2`,
  `addopen`, `addclose`, `addchdir_np`) are applied to a copy of the table
  first. The child's crt reads them before `main`; a malformed message is
  dropped with its handles and the child starts with no descriptors (and
  says so on `debug_write`). libjam has the writer (fdmsg.c in user/lib/)
  so native starters (the shell, init) speak it too.
- **Its signal channel**: a channel whose server end goes to the child
  (startup role `SR_SIGNALS`, new) and whose client end the parent keeps
  (section 6).
- **Without a hand-over message** (a POSIX program started by a native
  starter that gave none): with an `SR_CONSOLE` channel, descriptors 0, 1
  and 2 are that terminal (section 9); with only `SR_STDOUT`, 1 and 2 are
  it and 0 is `/dev/null`; with neither, 0 is `/dev/null` and 1 and 2 write
  to `debug_write`, as a native program's `printf` does.

**Ids.** `getpid` is the kernel's new small pid (K1: unique among living
processes, at most 2^22, reused only after a full lap), `getppid` the
parent's from the hand-over message (1 for init's children). `gettid` is
the kernel's tid (also small, below 2^30, musl's mutex owner field).

**Waiting.** libc keeps a table of its children: pid, process handle,
signal channel, process group. `waitpid(pid)` waits on that process's
`SIG_TERMINATED`; `waitpid(-1)` and process groups wait on a port with
every child bound; `WNOHANG` is a deadline of 0; the status comes from
`process_get_info` (an exit code, or killed by signal N: section 6). A
child is reaped (its handle closed) by the wait that reports it. Children
not waited for are just handles, closed at the parent's exit.

**`exec` without `fork`** (question 4): `execve` is spawn, wait, exit: the
new program starts as a child with the caller's descriptors, the caller
forwards every signal to it, waits, and exits with its status. Its pid
changes, which `exec`-in-place programs (`env`, `nohup`, `nice`, `time`)
don't notice. `fork`, `vfork`, `clone` without `CLONE_VM`: `ENOSYS`.
`system` and `popen` are musl's, on `posix_spawn`: they work.

**Process groups and sessions** are libc bookkeeping over its children
(`setpgid`, `getpgid`, `setsid`, `kill(-pgid)`), since the only
processes a program can signal are its own children (section 6). The
terminal's foreground is section 9's focus, not a process group.

### 6. Signals

The kernel has no asynchronous signal, and nothing in the design needs a
kernel signal to reach a thread running user code. The recommendation
(question 3, option A): **signals are libc's, delivered at the call
boundary, with one small kernel call to end a blocked call.**

- **Sending.** A process can signal only what it holds: its children (the
  signal channel `posix_spawn` made), itself, its own threads. The
  `signal` protocol (new IDL, id 93) has one event, `send (u8 signo, i32
  value)`. `kill(child, SIGKILL)` is `process_kill`; any other signal is a
  `send` on the child's signal channel. `kill` of a pid that isn't a
  child: `ESRCH` (a pid names nothing a program can reach otherwise). The
  native shell holds the signal channel of each POSIX program it runs.
- **Receiving.** Each POSIX process has a **signal thread** (made by crt1,
  64 KiB of stack, never runs user handlers) waiting on one port: the
  signal channel, the children's `SIG_TERMINATED` (SIGCHLD), the terminal's
  size events (SIGWINCH), and the alarm timer (`alarm`, `setitimer`,
  `timer_create` with `SIGEV_SIGNAL`: SIGALRM). For each signal it either
  acts at once (the default action of a signal with no handler:
  terminate, or ignore; SIGKILL never reaches it) or marks it pending and
  picks a thread that doesn't block it (the main thread first).
- **Interrupting.** A picked thread blocked in a wait is woken by the new
  `thread_interrupt(thread)` (K1): its interruptible wait (`object_wait_one`,
  `port_wait`, `futex_wait`, `sleep_until`) ends with `ERR_INTERRUPTED`,
  and an interrupt that comes while the thread is not waiting is latched
  and ends its next interruptible wait at once, so a signal sent just
  before a thread blocks is never lost. `channel_call` is never
  interrupted (a request can't be taken back, and M12's Q1 consumes its
  handles): file calls to fat finish, as disk I/O does on Linux.
- **Running a handler.** A handler runs on the picked thread, in libc, at
  the next call boundary: when an interruptible wait returns, or when the
  layer's dispatch returns from any call but memory management and
  `futex_wake`. Then the blocked call returns `EINTR`, or is restarted
  (`SA_RESTART`). `sigsuspend`, `pause`, `sigwait`, `sigtimedwait` and
  `sigwaitinfo` wait on the pending set. A handler gets a `siginfo_t`
  (signo, code, sender pid) and an empty `ucontext_t`.
- **What that gives up**, against Linux: a thread spinning in user code
  with no calls runs its handler only at its next call (its default action
  still happens at once, from the signal thread: Ctrl+C on a busy loop
  ends it); faults (SIGSEGV, SIGFPE, SIGILL, SIGBUS) are not delivered as
  signals: the process is killed as today; no `sigaltstack` (accepted,
  unused). Option B (a kernel upcall) removes the first two at the cost of
  a kernel path M14 then freezes.
- **The ones that matter for M13**: SIGINT, SIGQUIT, SIGTERM, SIGHUP
  (Ctrl+C, Ctrl+\, a stop, a closed session), SIGCHLD (tinyssh's self
  pipe), SIGALRM (tinyssh's timeouts), SIGPIPE (raised by the write that
  got `EPIPE`, on the writing thread, synchronously), SIGWINCH (a resize),
  SIGUSR1/2, `raise`, `abort` (SIGABRT, then killed), `pthread_kill`,
  `pthread_cancel` (cancellation points are interruptible waits: the
  override of `syscall_cp` checks the cancel flag before and after).
- **Exit status**: a process ended by a signal's default action calls
  `process_exit` with a code outside 0 to 255 (a libc constant plus N),
  which libc's `waitpid` reports as `WIFSIGNALED` with `WTERMSIG` N, so a
  program's own `exit(130)` stays an exit; the native shell shows either
  as `$?` 128 + N. A process killed by `process_kill` is `WTERMSIG`
  SIGKILL.

### 7. `poll`, `select` and `epoll`

On M9.5's wait sets (`<netwait.h>`), which are already level-triggered and
O(ready):

- **Each descriptor kind** gives the set either a socket entry (its rings,
  events and channel) or a handle entry (which signals mean read, write
  and hang-up), or says "always ready" (files, `/dev`, `/etc`, memfds).
- **`poll` and `select`** use a wait set kept per thread, with an entry
  cache: a descriptor polled again with the same interest keeps its
  bindings, so a loop polling the same few descriptors (tinyssh's six)
  costs the ready ones, not a bind and an unbind of each every time.
  Entries not asked for in 64 calls are removed. `ppoll` and `pselect`
  set the mask for the wait. Timeouts become deadlines.
- **`epoll`** is a wait set of its own (`epoll_create1` makes one,
  `epoll_ctl` adds, changes and removes entries, `epoll_wait` waits);
  `EPOLLET` and `EPOLLONESHOT` are kept in libc over the level-triggered
  set. An epoll descriptor in another epoll set: `EINVAL` (ports can't be
  bound to ports).
- **The signal thread's interrupt** ends a `poll` with `EINTR` (the set's
  `port_wait` is an interruptible wait).
- **A thread's wait set** is made on its first `poll` and removed at its
  exit; 256 entries (`NETWAIT_MAX`), so `poll` on more descriptors at
  once is `EINVAL` in M13. `select`'s `FD_SETSIZE` stays 1024.

### 8. Sockets and names

`AF_INET` only, over libjam's `net` calls and the socket rings:
`socket`, `bind` (ports below 49152 need the listen permission, below
1024 the low one, as today), `connect` (blocking, or `EINPROGRESS` with
`O_NONBLOCK` and writability when it opens), `listen` (needs
`/svc/net-listen`, else `EACCES`), `accept`/`accept4`, `send`/`recv`/
`sendto`/`recvfrom`/`sendmsg`/`recvmsg` (no ancillary data),
`shutdown` (`SHUT_WR` is the tx ring's end, `SHUT_RD` libc's own: further
reads see end of file), `getsockname`/`getpeername` (`net_stream.state`),
`setsockopt`/`getsockopt` (`TCP_NODELAY`, `SO_KEEPALIVE` and
`SO_REUSEADDR` through N1's additions; `SO_RCVBUF`/`SO_SNDBUF` choose the
rings' sizes when given before `connect` or `listen`; the rest accepted
and ignored, a list in the code). Names: `getaddrinfo`,
`gethostbyname` and friends go to `/svc/dns` through the override of
`src/network/lookup_name.c` (one resolver for the system, as today; a program's list
asks for `svc dns`); `/etc/hosts` is libc's. `AF_UNIX`: `socketpair`
only. `AF_INET6`: `EAFNOSUPPORT`.

### 9. The terminal layer

Today a terminal is a console (`user/services/console/`): a window, the
`console` protocol (`abi/idl/console.idl`) with its three levels, the
keys as events (`open_keys`), Ctrl+C always reaching the shell below a
program. The native shell and native programs speak that protocol. M13
needs two more things: **a terminal whose screen and keyboard are a
connection** (SSH), and **a POSIX view of a terminal** (bytes through a
line discipline, `termios`, the window's size).

The recommendation (question 2, option A): **the console is the terminal
in both cases, and the line discipline lives in it.**

- **Two back ends.** The console splits into its front end (the clients
  and their levels, the focus, the keys, the kernel log's lines, the line
  discipline) and a back end: **window** (today's: the text model, the
  escapes, drawing, the mouse) or **connection** (new: the argument
  "conn"; program output goes out as bytes on a master channel, `\n` as
  `\r\n`, as it was written, the remote terminal interpreting the escapes;
  bytes coming in are keys through `<termkeys.h>` for native clients and
  bytes as they came for POSIX ones; the size comes from the session's
  control channel). A connection console holds no screen, no Wayland and
  no serial port; init's (the sessions, below) gets the root with
  `RIGHT_ROOT_KLOG` alone, for the log's lines its shell asks for, and a
  program's own (a pty) gets nothing.
- **The native shell runs on either, unchanged.** It speaks the `console`
  protocol, and the connection console serves it the same way, the kernel
  log's lines for a program it runs included (`show_log`). That is how
  "ssh opens a Jam OS shell": the shell doesn't know.
- **POSIX clients** get a new method, `console.open_tty () -> (handle
  tty, handle input, handle output)`, on a client channel of any level:
  `input` is a pipe the console writes the terminal's input into (after
  the line discipline), `output` a pipe it reads and draws (or sends),
  `tty` a channel of the new **`tty`** protocol (as built: id 43, and one
  data channel in place of the two pipes; "T2 notes"): `get_attr`,
  `set_attr` (now, after drain, after flush), `drain`, `flush`,
  `get_size`, and the event `size (u16 cols, u16 rows)`. libc's terminal
  descriptor is the three together: `read` reads the input pipe, `write`
  writes the output pipe, `tcgetattr`/`tcsetattr`/`TIOCGWINSZ` go to
  `tty`, and the size event becomes SIGWINCH. `isatty` is true on it.
- **The focus**, as today: the most recent `open_keys` or `open_tty` gets
  the input; closing gives it back. A program the shell runs in the
  foreground opens its terminal and has the focus until it ends; a
  program in the background gets no terminal (as today). This is the
  foreground process group, without process groups in the console.
- **The line discipline** (user/lib/ldisc.c, new, libos, with
  `<ldisc.h>`): one per terminal, POSIX's `termios`: `ICANON` (line
  editing: erase, kill, word erase, end of file, `MAX_CANON` 4096),
  `ECHO`, `ECHOE`, `ECHOK`, `ECHONL`, `ECHOCTL`, `ISIG` (Ctrl+C, Ctrl+\
  and Ctrl+Z: Ctrl+Z ignored in M13), `ICRNL`, `INLCR`, `IGNCR`, `OPOST`
  with `ONLCR`, `VMIN`/`VTIME` for non-canonical reads, `IEXTEN`'s `VLNEXT`.
  Type-ahead stays in the terminal across programs; `termios` is the
  terminal's, so `stty` in one program holds for the next, as on Unix. A
  pure module (bytes and keys in, bytes out, no system calls), so its
  tests are utests.
- **Keys into bytes** (user/lib/keybytes.c, new): the window's key events
  as the bytes xterm sends (arrows, Home, End, Page Up and Down, Delete,
  Insert, F1 to F12, Alt as an ESC prefix, Ctrl+letter as a control
  byte), `<termkeys.h>`'s inverse; a round trip through both is a utest.
- **Ctrl+C with `ISIG` on**: the line discipline flushes the input, and
  the console sends the key to the SHELL client below, as today; the
  shell sends SIGINT on the program's signal channel (a native program it
  still kills). With `ISIG` off (an editor in raw mode): question 6.
- **`TERM`**: over SSH the client's (`xterm-256color` from the Mac),
  passed through; in a window the subset of xterm's escapes the console
  draws (T2 lists them and names the `TERM` value: `vt100` unless it does
  more). No terminfo database in M13 (no curses port).
- **Sessions: how SSH gets a shell.** Only init can start a Jam OS shell
  with the shell's authority (its namespace, init's channel, the root
  powers). So init serves a new service, **`/svc/session`** (protocol
  `session`, id 91, a channel per opener, granted only by a list's `svc
  session`, which `allow` refuses for a program on `/data` as it refuses
  `svc init`): `open (u16 cols, u16 rows, str[<=63] term, str[<=4095]
  command) -> (handle master, handle ctl)`. init starts a connection
  console and a shell on it, as it does a terminal window (`user/services/init/terms.c`),
  and hands back the **master** (a channel of byte messages both ways:
  what the terminal shows, read; what is typed, written) and **ctl**
  (protocol `session_ctl`, id 92: `resize (u16 cols, u16 rows)`,
  `hangup ()`, and the event `ended (i32 code)` when the shell exits).
  `command` empty is an interactive shell; otherwise the shell runs that
  line and exits (`ssh pc 'ls /data'`). At most 8 sessions; the 9th is
  `ERR_NO_RESOURCES`. Closing the master hangs the session up: the shell
  gets its console closed (codes 2 and 3, as today), what it runs goes
  with its job.
- **Pseudo-terminals for ported programs** (`posix_openpt`, `openpty`,
  `/dev/ptmx`): libc starts a connection console of its own (bin/console
  is in `/boot`, needs no grant), keeps the master side as a master
  descriptor and opens the slave as a terminal descriptor through
  `open_tty`. No shell, no authority: a pty is a console process in the
  program's job. `ptsname` names it "/dev/pts/N", which only that process
  can open.

### 10. Time and clocks

- `CLOCK_MONOTONIC` and `CLOCK_BOOTTIME`: the uptime; `CLOCK_REALTIME`:
  the wall clock (`wallclock_get`; with no clock set, the uptime counted
  from 1970, as review K proposed). Both **without a system call** from
  K2's **time page**: a read-only VMO every process may map (a call,
  `clock_page`, gives a handle; no right needed), holding the TSC's
  conversion, the boot's TSC and the wall clock's offset under a sequence
  counter the kernel bumps on `wallclock_set`; libc falls back to the
  calls if the TSC is not invariant (the page says so). Resolution 1 ns.
- `CLOCK_PROCESS_CPUTIME_ID`, `CLOCK_THREAD_CPUTIME_ID`, `times`,
  `getrusage`, `clock`: K1's `cpu_ns`.
- `nanosleep`, `clock_nanosleep` (relative and `TIMER_ABSTIME`):
  `sleep_until`, interruptible. Timers: a kernel timer bound to the signal
  thread's port. `clock_settime`: `EPERM` (`RIGHT_ROOT_CLOCK` is init's and
  sntp's).
- `uname`, `sysconf(_SC_NPROCESSORS_ONLN)`, `_SC_PHYS_PAGES`: K1's
  `sys_public_info`. `gethostname`: the setting `host.name`, else
  "jamos".

### 11. Users and ids

Single user ([ARCHITECTURE.md](../ARCHITECTURE.md#goals)). The
recommendation (question 7, option A): one user named **jam**, uid and
gid 1000 (not root: programs that drop or refuse privileges behave as for
an ordinary user, and nothing in Jam OS reads a uid), home `/data`, shell
the Jam OS shell. `getuid` and friends answer 1000; `setuid(1000)`
succeeds and any other is `EPERM`; `getpwnam("jam")`, `getpwuid(1000)`
and `getgrgid(1000)` from libc's `/etc`. Authority stays in handles.

### 12. Memory: `mmap`

- **Anonymous** (`MAP_PRIVATE` or `MAP_SHARED`): a VMO per mapping,
  mapped with `vmar_map`, its handle kept in libc's mapping table (so
  `munmap` of part of it, `mprotect` and `madvise` find it). `munmap`:
  `vmar_unmap` (the kernel splits); the VMO goes when nothing maps it.
  `mprotect`: `vmar_protect`; write with execute is refused (`EACCES`), and
  so is execute on anonymous memory (no `RIGHT_EXEC`: no JITs in M13).
  `madvise(MADV_DONTNEED)` and `MADV_FREE`: `vmo_decommit` of the range
  through the table. `mremap`: `ENOSYS` (musl's `realloc` copies).
  `MAP_FIXED` over a mapping: K2's `VMAR_REPLACE`.
- **`brk`**: a 1 GiB reservation made at start, committed as touched
  (musl's malloc asks `brk` for its metadata and `mmap` for the rest).
- **Files, `MAP_PRIVATE`**: in M13's first waves, a copy: an anonymous
  mapping filled by reading the file (correct, and costs the copy). After
  M12.5 (track F1): a read-only private mapping maps the file's pager VMO
  directly, and a writable one uses M12.5's copy-on-write child if M12.5
  builds it (review K's finding 37), else keeps the copy.
- **Files, `MAP_SHARED`**: after M12.5 (F1), the pager VMO; writable only
  if M12.5's pager writes dirty pages back, else `ENODEV` for
  `PROT_WRITE`. `msync` asks the pager to write. Before M12.5: `ENODEV`.
- **memfd and `shm_open`**: a kept VMO (`VMO_KEEP_PAGES`), the Wayland
  review's W15, so a G2 port's `wl_shm` pool is accepted by the
  compositor.

### 13. Threads

musl's pthreads, unmodified, on the overrides of section 1: `clone`
makes a thread (`thread_create`, `thread_start` with its TLS as FS base)
in the caller's process; the new thread first calls K1's
`thread_set_tid_word` (Linux's `set_tid_address`: the kernel stores 0 at
that word and wakes its futex once the thread is off its stack, which
musl's `pthread_join` and thread list lock rely on); mutexes, condition
variables and barriers on the futexes M12 built; `pthread_setschedparam`
on `thread_set_priority` (up to 24); affinity: `ENOSYS`. Robust mutexes
and priority inheritance: not planned (review K's table).

### 14. How ported programs are built and approved

- **musl** is vendored unmodified in third_party/musl (release 1.2.6, its
  tarball's hash in `third_party/VERSIONS.md`), built by our Makefile (not
  musl's configure): its sources less `ldso/`, `src/ldso` and the overridden
  files, with musl's two generated headers (`bits/alltypes.h`, `bits/syscall.h`) made
  by the same `sed` steps musl's Makefile uses. Compiled once per clean
  build.
- **The sysroot**, build/sysroot: musl's headers, Jam OS's extension
  headers (`jamos/`), `libc.a` (musl + the call layer + libjam), crt1.o,
  crti.o, crtn.o, and the POSIX linker script (user/posix/posix.ld:
  `user/linker.ld`'s three segments at 0x400000, plus `PT_TLS`, the
  program headers inside the first segment so `AT_PHDR` points at them,
  and the list's `PT_NOTE`).
- **The compiler**: **tools/jamos-cc** (new), a wrapper around the cross
  compiler we already build (x86_64-elf-gcc 15.2, the kernel's), with
  `-nostdinc -isystem build/sysroot/include -static -nostdlib`, crt1 and
  libc and libgcc, and the program's list (a `wants` file beside the
  port, or the default "posix" list) linked in as the note. `CC=jamos-cc
  ./configure --host=x86_64-linux-musl` works for an autotools package
  (its tests run on the Mac, cross). Question 10 weighs a real
  `x86_64-jamos` toolchain instead.
- **Ports** live as the rest of outside code: the source unmodified in
  `third_party/<name>/` (each with its licence and a `third_party/VERSIONS.md`
  line; permissive licences only, CODING-GUIDE's rule), and Jam OS's side
  in user/ports/<name>/: a make fragment, the program's list, and override
  files where a program forks (each override says why, and the build
  checks upstream's file is still the one it replaced, as for musl).
- **Where they go**: bootfs `usr/bin/` (`/boot/usr/bin/ls`); the shell
  looks up `PATH` (`/boot/bin:/boot/usr/bin`) after its builtins, so the
  builtin `ls` still wins and `run ls` or `/boot/usr/bin/ls` gets sbase's
  (question 9).
- **Approval**: a ported program in the boot image is approved by the
  build (`tools/checkwants.py` checks its list as any program's); one
  built on the Mac and copied to `/data` runs only after `allow`, its hash
  and list checked as a native program's. Nothing new.

### 15. tinyssh

tinyssh (release 20261001, CC0/0BSD/MIT-0/MIT) is made for inetd: one
`tinysshd` per connection, the connection on descriptors 0 and 1, the key
directory as its argument, a `poll` loop over the connection, the
channel's pipes and a self-pipe for SIGCHLD, `alarm` for timeouts. It
forks in three places, each replaced by an override (the owner's "fork
replaced by `posix_spawn`"):

- subprocess_sign.c (signs with the host key in a child, so the process
  that parses the network never holds the secret key): `posix_spawn` of
  `tinysshd` itself in a helper mode (the data on a pipe in, the signature
  on a pipe out), keeping that separation.
- subprocess_auth.c (checks the offered key in a child): the same
  helper mode, its answer as the exit code; the key list is
  **`/data/etc/authorized_keys`** (the owner's place), OpenSSH's format,
  `ssh-ed25519` lines only.
- channel_forkpty.c and channel_fork.c (the session's shell):
  `jamos_session_open` (section 9) instead of `openpty`, `fork`,
  `login_tty` and `execve` of the user's shell; the master is the
  channel's descriptor, `channel_ptyresize`'s `TIOCSWINSZ` goes to `ctl`,
  and libc reports the session as a child to `waitpid` (its `ended` code).
  An `exec` request (`ssh pc 'cmd'`) opens a session with the command.

Around it:

- **The listener, bin/sshd** (ours, a POSIX program in user/ports/tinyssh/,
  about 200 lines: a test of `accept`, `posix_spawn`, `waitpid` and
  SIGCHLD itself): listens on port 22 (its list asks `svc net listen
  low`), sets `TCP_NODELAY` and `SO_KEEPALIVE` on each connection, and
  spawns one `tinysshd` per connection with the socket as descriptors 0
  and 1 and a namespace narrowed to `/data:r` and `/svc/session`; at most
  8 at once (a 9th connection is closed at once), each killed after an
  hour without a byte (tinyssh's own `alarm`).
- **init** starts bin/sshd once `/data` is mounted and the network is up,
  when question 8 says to, and supervises it as any service. Before the
  first start it makes the **host key** if `/data/etc/ssh/` has none:
  `ed25519.pk` and `.ed25519.sk` in tinyssh's format (32 and 64 bytes),
  made by init itself with Monocypher's Ed25519 and `os_random`, written
  under the guarded `etc`, so no ported program ever holds `/data/etc`
  writable. The key's fingerprint goes into the boot log, and the shell's
  new `ssh` command shows it (and the server's state and its
  connections), so the owner can check what the Mac's first connection
  prints.
- **Crypto**: tinyssh's own (ed25519, curve25519 and sntrup761x25519 key
  exchange, chacha20-poly1305), which OpenSSH on the Mac speaks; its
  random bytes from `getrandom` (`random_get`).

### 16. What M13 must not block

- **M14 (the stable ABI).** A ported program is static: it carries musl,
  the call layer and libjam, so it speaks the `fs`, `file`, `net`,
  `dns`, `tty`, `session` and `signal` protocols and the hand-over
  message itself. "Old binaries keep running" therefore means **those
  protocols are frozen with the system calls**, or carry a version a newer
  server answers. M14's review gets that list (below), and the hand-over
  message and every new protocol carry a version word from the start.
- **M14.5 (dynamic linking).** Nothing in the call layer or crt1 assumes
  static linking beyond crt1's own startup: the layer is ordinary code in
  libc and would go into a `libc.so` unchanged; musl's `ldso/` is left
  out of the build, not deleted or patched.
- **M15 (self-hosting).** GNU make (4.4) and GCC's driver (libiberty's
  `pex`) can start programs with `posix_spawn`, so they need no `fork`.
  What M15 will need that M13 leaves out: a POSIX shell for `configure`
  scripts (question 4's B, emulated `vfork`, is its likely road), shared
  offsets for inherited files (question 5's B), `/tmp`, and a real
  toolchain target (question 10's B). Each is an addition to what M13
  builds, none a change of it.
- **G2 (libwayland and ported Wayland programs)**: memfds as kept VMOs
  (W15) and write-only pipe ends the compositor accepts (W16, M12's) are
  both in M13; `SCM_RIGHTS` over `AF_UNIX` is not (G2's shim carries
  handles itself).

## The new and changed interfaces (for M14's review)

**System calls** (`abi/syscalls.def`, the "processes, threads, jobs" and
"threads" groups and two new lines; numbers chosen by K1 and K2 from the
free ones, none reused):

| Call | Shape | Track |
|---|---|---|
| `thread_set_tid_word` | `(uint32_t *addr) -> i64 tid`: at the thread's end, after its last user access, the kernel stores 0 at addr and `futex_wake`s it (all). 0 clears. `ERR_INVALID_ARGS` for an unaligned or non-user address | K1 |
| `thread_get_info` | `(handle_t thread, struct thread_info *out)`: tid, its process's koid and pid, state, `cpu_ns`, priority | K1 |
| `struct process_info` | gains `pid` (the u32 `reserved`, no size change) and `cpu_ns` (a new u64: the struct grows; callers rebuild) | K1 |
| `thread_interrupt` | `(handle_t thread)`: needs `RIGHT_SIGNAL` on the thread; its current or next interruptible wait (`object_wait_one`, `port_wait`, `futex_wait`, `sleep_until`) ends with `ERR_INTERRUPTED` (-26, new); latched once, not counted | K1 |
| `sys_public_info` | `(struct sys_public_info *out)`, no handle: version, machine ("x86_64"), CPUs online, total pages, page size | K1 |
| `clock_page` | `(handle_t *out)`: the time page, a read-only VMO (`struct time_page` in `<jam/abi.h>`: a sequence word, the TSC's mult and shift, the boot's TSC, the wall clock's offset and flags, an "invariant TSC" bit) | K2 |
| `vmar_map`'s `VMAR_REPLACE` | with `VMAR_FIXED`: what is mapped there is unmapped and the new mapping made in one step | K2 |

**IDL** (`abi/idl/ids.txt`: **90 to 99 are M13's**):

| Id | Protocol | Where | Methods |
|---|---|---|---|
| 12 | `console` (changed: one method added) | `abi/idl/console.idl` | `12 open_tty () -> (handle tty, handle input, handle output)`, every level |
| 90 | `tty` (new) | console.idl | `get_attr`, `set_attr (u8 when, ...)`, `drain`, `flush (u8 which)`, `get_size`; event `size (u16 cols, u16 rows)` |
| 91 | `session` (new) | session.idl (new) | `open (u16 cols, u16 rows, str[<=63] term, str[<=4095] command) -> (handle master, handle ctl)` |
| 92 | `session_ctl` (new) | session.idl | `resize`, `hangup`; event `ended (i32 code)` |
| 93 | `signal` (new) | signal.idl (new) | event `send (u8 signo, i32 value)` |
| 61 | `net_stream` (an addition) | `abi/idl/net.idl` | `set_options (u32 flags, u32 keep_idle, u32 keep_intvl, u32 keep_cnt) -> ()`: no-delay, keep-alive and its three numbers |
| 62 | `net_listener` (an addition) | net.idl | `tcp_listener`'s request has a `u32 flags` (`NET_LISTEN_REUSE`) |

**Hand-written formats**: the POSIX hand-over message ("JPSX", a version
word, user/include/posixfds.h, in ids.txt's registry); the master
channel's bytes (as a pipe's).

**Startup roles** (`user/include/roles.h`): `SR_SIGNALS` (19), a POSIX
program's signal channel (server end).

**A program's list** (`<wants.h>`): the word `posix`; the want `svc
session` (refused for a program on `/data`, as `svc init` is).

**libos/libjam** (new headers): `<ldisc.h>`, `<keybytes.h>`,
`<posixfds.h>`; `struct spawn_args` gains `stack_size` and the hand-over
message's bytes and handles.

**libc extensions** (user/posix/include/jamos/): `jamos_session_open`,
`posix_spawnattr_setgrants_np`.

**Settings**: `ssh` (question 8). **Files**: `/data/etc/authorized_keys`,
`/data/etc/ssh/` (the host key).

**Not changed**: `fs`, `file` (if question 5 takes A), fat, the namespace's
messages, the startup message and its 16 handles.

## Questions for the owner

Each stands alone and ends with a recommendation; the plan assumes the
recommendation until you say otherwise.

### Q1. How musl meets Jam OS

- **(A)** musl vendored unmodified; about 12 of its files overridden by
  ours (the system call entry, thread start and exit, `posix_spawn`, the
  DNS lookup); every Linux call it makes goes to a Jam OS "Linux call
  layer" in libc.
- **(B)** musl's wrappers (`open`, `read`, `poll`, ...) replaced by ours,
  file by file: hundreds of files, a fork of musl.
- **(C)** Another libc: mlibc (built for hobby OSes, but C++ and meson),
  or newlib/picolibc (much less POSIX).

*Recommendation: (A).* musl stays upstream's (a new release is a drop-in
and a recorded-hash check on the overrides), its hard parts (stdio,
malloc, pthreads, locale) stay its own, and one switch translates Linux's
calls, nanoseconds against any real call's cost.

### Q2. Where the terminal's line discipline lives

- **(A)** In the terminal: the console, which gains a "connection" back end
  for SSH beside its window and a POSIX view (`open_tty`: bytes through
  the line discipline, `termios`, the size). The native shell runs on
  either unchanged; `termios` and type-ahead belong to the terminal, as on
  Unix.
- **(B)** In libc: each POSIX program edits its own lines from the
  console's keys. Less console work, but `stty` doesn't outlast the
  program, the keys are turned back into bytes (lossy over SSH), and a
  connection console is still needed for SSH.
- **(C)** A separate pty service in front of the console.

*Recommendation: (A).* One line discipline (a pure libos module with its
own tests), one terminal model in both places, and the Jam OS shell over
SSH with no change to the shell.

### Q3. Signals

- **(A)** In libc, delivered at the call boundary: a signal thread per
  POSIX process receives them (from the parent, the children's ends, the
  timer, the terminal), default actions happen at once, handlers run on
  the target thread at its next call, and one new kernel call
  (`thread_interrupt`) ends a blocked wait with `EINTR`. A program can
  signal only its children and itself. Faults kill as today.
- **(B)** A kernel upcall: true asynchronous delivery (a handler
  interrupts a busy loop; SIGSEGV can be caught), at the cost of new
  kernel code in the return-to-user and switch paths that M14 then
  freezes.
- **(C)** None: `kill` is `process_kill`, no handlers, no `EINTR`.
  tinyssh's SIGCHLD and SIGALRM would need patches.

*Recommendation: (A).* It covers what ports use (SIGINT, SIGTERM,
SIGCHLD, SIGALRM, SIGPIPE, SIGWINCH, `EINTR`) with one small kernel call;
(B) stays an addition if a port ever needs a handler in a busy loop.

### Q4. `fork`, `vfork` and `exec`

- **(A)** `posix_spawn` only: `fork` and `vfork` answer `ENOSYS`; `execve`
  is "spawn, wait, exit with its status"; a ported program that forks gets
  an override file (tinyssh's three, sbase's `xargs` and `find -exec`).
- **(B)** (A), plus an emulated `vfork`: libc records what the "child"
  does (descriptor changes) until its `execve` or `_exit`, then spawns and
  returns to the parent. Most `vfork`-then-`exec` code runs unpatched;
  code that does more in the child breaks in odd ways.

*Recommendation: (A)* for M13: every fork is replaced visibly; (B) is the
likely road to a POSIX shell at M15, and an addition then.

### Q5. The offset of a file a child inherits

- **(A)** In libc, per process: the child starts at the parent's offset at
  the spawn, then each moves its own; `O_APPEND` is exact. No change to
  fat or the `file` protocol.
- **(B)** In the server: `file` gains a "clone" (a second channel onto the
  same open file) and reads and writes at a shared cursor, so `{ a; b; }
  > f` in a POSIX shell appends as on Unix. fat's state layout and
  bootfs's server change.

*Recommendation: (A)* now: nothing in M13 shares a written file between a
parent and a child but through `O_APPEND`; (B) with M15's shell.

### Q6. Ctrl+C in a program that turns `ISIG` off (raw mode)

Today a program can't trap the keys: Ctrl+C always reaches the shell.

- **(A)** POSIX: with `ISIG` off Ctrl+C is the byte 3 to the program;
  closing the window or the SSH connection stays the way to end it.
- **(B)** As today: Ctrl+C always reaches the shell (SIGINT); editors lose
  Ctrl+C as a key.
- **(C)** (A), and in a window a chord the program never gets
  (Ctrl+Alt+C) always reaches the shell, which kills the program's job.

*Recommendation: (C).* Ported editors work as on Unix, and the desktop
keeps a key no program can take; over SSH, OpenSSH's `~.` is that key.

### Q7. The one user

- **(A)** Named `jam`, uid and gid 1000, home `/data`; SSH logs in as
  `jam` only (`ssh jam@pc`), any other name refused.
- **(B)** Root (uid 0): some programs then refuse to run or try to drop
  privileges.
- **(C)** (A), but SSH accepts any name (the key is the only check).

*Recommendation: (A).* An ordinary user is what ported programs expect;
one accepted name makes a typo fail clearly. The name is a constant, a
setting later if wanted.

### Q8. When the SSH server runs

- **(A)** On port 22 whenever `/data/etc/authorized_keys` holds a key;
  `ssh = off` in the settings stops it. A public build (no key file)
  listens on nothing.
- **(B)** Only with `ssh = on` in the settings **and** a key: off until
  turned on.
- **(C)** (A) on port 2222 (no low-port permission needed).

*Recommendation: (A).* Putting a key on the stick is already an explicit
act, and a second switch would be one more thing to forget; port 22 so
`ssh jam@pc` needs no `-p`. Every frame stays on the build's VLAN.

### Q9. The ported utilities

- **(A)** sbase (suckless; MIT; small, plain POSIX C, a program per
  utility; `xargs`, `find -exec` and `time` fork and get overrides).
- **(B)** toybox (0BSD; more complete, but written for Linux: `/proc`,
  Linux `ioctl`s, its own `fork` paths).
- **(C)** busybox: **excluded** by CODING-GUIDE's licence rule (GPLv2).

Where: in `/boot/usr/bin`, found through `PATH` after the shell's builtins
(the builtin `ls` still wins; `run ls` gets sbase's).

*Recommendation: (A)*, with Lua 5.4 as "a small C program", and builtins
first so nothing a user types today changes.

### Q10. The compiler for ported programs

- **(A)** A wrapper, tools/jamos-cc, around the cross compiler the kernel
  already uses (x86_64-elf-gcc 15.2), static, with the sysroot; autotools
  packages configure as `--host=x86_64-linux-musl`.
- **(B)** A real `x86_64-jamos` target in binutils and GCC (patches to
  both, a toolchain build of about an hour on the Mac and the PC).

*Recommendation: (A)* for M13: nothing new to build or keep, and the PC
host (which builds images) needs nothing either; (B) belongs to M15, which
must build GCC for Jam OS anyway.

## Tracks

Each track is about one agent-hour, starts from main, owns the files
listed (an edit outside them stays minimal and is named in its report),
commits refactors and behaviour changes separately, fixes with a test that
fails without the fix, and hands back at each usable stage. **QEMU tests
run on the PC only** (`tools/remote-test.sh`, AGENTS' rule): the quick
tier (the build, `make check`, the init run) plus the track's own area
scripts, run in parallel; kernel tracks also the full ktest at 4 and 8
CPUs and the soak. No track pushes, touches a USB disk or starts agents.
Main is frozen while a wave's tracks run.

The model follows the owner's split: **Opus** for the kernel, the musl
port core, the descriptor and process core, the terminal's semantics, the
SSH path and every review; **Sonnet** for the console's refactor, the
network additions, the shell, the ported utilities and the join.

### Wave A: the foundations (after M12's P6a and P6b merge)

Runs beside M12.5 and M12.7 (wave 5). Five tracks, independent of each
other but for A1 taking A2's first hand-back.

| Track | Model, why | What | Files it owns (new ones plain) | Gate and tests |
|---|---|---|---|---|
| **K1. Kernel for POSIX** | Opus: kernel, the thread exit path and every wait | Small pids and tids (allocated at create, unique among the living, a full lap before reuse; `process_info.pid`, `thread_get_info`), `thread_set_tid_word` (store and wake after the last user access), `thread_interrupt` and `ERR_INTERRUPTED` (latched; the four interruptible waits), `cpu_ns` for processes and threads, `sys_public_info`. Bench before and after: the reply-and-wait and switch lines | `kernel/object/process.c`, `kernel/sched/thread.c`, `kernel/sched/wait.c`, `kernel/sched/futex.c` (the wake at exit), `kernel/abi/sysc_proc.c`, `kernel/abi/sysc_sysinfo.c`, `kernel/include/jam/status.h`, its group's lines of `abi/syscalls.def` and `<jam/abi.h>`, `kernel/test/test_process.c`, `kernel/test/test_futex.c`, a new kernel/test/test_interrupt_wait.c, libos's `status_str` | ktests: pids unique under 10,000 creates and kills on 8 CPUs; the tid word cleared and woken only after the thread left user space (a joiner unmapping the stack at once never faults); an interrupt before, during and after a wait (none lost, none counted twice); a `channel_call` never interrupted; `cpu_ns` grows only while running. Full ktest at 4 and 8, soak at 4; bench lines no slower |
| **A2. libjam and the hand-over** | Opus: it decides what every ported program links | libjam.a (libos minus its libc parts; start.c split), the link test against a stub libc in `make check`; the POSIX hand-over message (user/include/posixfds.h, user/lib/fdmsg.c: write, read, hostile checks), `SR_SIGNALS`, `struct spawn_args`' `stack_size` and hand-over fields in `user/lib/spawn.c`, the `posix` word and `svc session` in `<wants.h>` and `tools/checkwants.py` | the Makefile's libos/libjam lines, `user/lib/start.c` (split), `user/lib/spawn.c`, `user/include/os.h` (spawn_args), `user/include/roles.h`, `user/include/wants.h`, `user/lib/wants.c`, `tools/checkwants.py`, the new files | utests: the hand-over message round trip, every malformed form refused with its handles closed (a fuzz loop of 10,000 random messages), more than 64 handles across messages; a native child started with `stack_size` 8 MiB touches 7 MiB. First hand-back: libjam.a builds (A1 needs it) |
| **A1. musl, the sysroot and the call layer's core** | Opus: the port's core | third_party/musl (1.2.6), the overrides (section 1) and their hash check, the musl build and the sysroot, user/posix/posix.ld, crt1 (startup and hand-over messages to Linux's block), `__jam_syscall` and the errno table, `mmap`/`munmap`/`mprotect`/`madvise`/`brk` (anonymous), threads (`clone`, `set_tid_address`, `arch_prctl`, `exit`, `exit_group`, futex), clocks (with the calls; the page is K2's), `getrandom`, `uname`, provisional descriptors 0 to 2 (section 5's "without a hand-over message"), tools/jamos-cc, and posixtest (user/posix/test/: one program, a section per area, the later tracks add theirs) | third_party/musl, user/posix/ (but what B tracks own), tools/jamos-cc, the Makefile's musl and sysroot lines, `third_party/VERSIONS.md` | posixtest's start, memory and thread sections in QEMU: argv, environment and auxv as given; TLS in 64 threads; `pthread_join` of 1,000 threads with their stacks unmapped; malloc of 1 GiB in pieces and back; `errno` per thread. `make check` refuses a changed upstream file behind an override. Needs K1's `thread_set_tid_word` for the join test (merge after K1) |
| **N1. Network additions** | Sonnet: protocol work in netstack | `net_stream.set_options` (`TCP_NODELAY`, `SO_KEEPALIVE` in lwIP), a listener's address reuse (a new listener on a port whose old connections are in TIME_WAIT), `net_stream.state`'s local and peer addresses after `accept` (checked), libjam's wrappers | `abi/idl/net.idl`, `user/services/netstack/tcpsock.c`, `user/services/netstack/listen.c`, libjam's `user/lib/nettcp.c` | `tools/tcp-test.sh`, `tools/netstack-test.sh`; tcptest gains: no-delay seen on the wire (the peer counts small segments sent without waiting for an ACK), a listener re-made on its port at once after a close |
| **T1. The console's connection back end** | Sonnet: a refactor plus a back end beside it | The console split into front end and back ends (a pure refactor commit, every terminal test unchanged), then "conn": output as bytes on a master channel, input bytes through `<termkeys.h>`, the size from a control channel, the kernel log's lines for `show_log` clients, no screen or serial | `user/services/console/` | Every terminal test unchanged (`tools/shell-tests/terms.txt`, `terms-windows.txt`, `typeahead.txt`, `console-restart.txt`, `tools/desk-test.sh`, `tools/clip-test.sh`); a new utest starts a connection console with a native shell on it and drives `echo`, `ls /boot` and Ctrl+C through the master |

### Wave B: descriptors, processes, terminals (after A1 and A2 merge)

| Track | Model, why | What | Files it owns | Gate and tests |
|---|---|---|---|---|
| **B1. Files and directories** | Opus: the descriptor core | The descriptor table and open file descriptions (section 3), `open`/`openat`/`close`/`read`/`write`/`pread`/`pwrite`/`readv`/`writev`/`lseek`, `stat` family, `getdents64`, `mkdir`, `unlink`, `rmdir`, `rename` (replace), `truncate`, `fsync`, `statfs`, `access`, `chdir`/`getcwd`/`fchdir`, directory descriptors and the `*at` calls, `MAP_PRIVATE` of a file by copy, a file handed to a child (its own open) | user/posix/sys/fd.c, file.c, dir.c | posixtest's files section on `/data` (fat) and `/boot`: every call's errors against the expected errno (a table, written on the Mac against Linux's answers for the same steps), 10,000 files in a directory listed, a rename over a file, `O_EXCL`, `O_APPEND` from two descriptors, a read-only view's `EROFS` |
| **B2. Pipes, dup, `/dev` and `/etc`** | Opus: the descriptor core | `pipe2`, `dup`/`dup2`/`dup3`, `fcntl` (`F_DUPFD`, `F_DUPFD_CLOEXEC`, `F_GETFD`/`F_SETFD`, `F_GETFL`/`F_SETFL`; locks accepted, not enforced), `O_NONBLOCK`, `/dev` and `/etc` (section 3), memfd and `shm_open` as kept VMOs, eventfd, `socketpair`, the one user's calls | user/posix/sys/pipe.c, dev.c, etc.c, user.c | posixtest's pipe section: 1 GiB through a pipe between two threads checked, `PIPE_BUF` atomicity with four writers, end of file only after the last writer closes, `EPIPE`; a BENCH.md line for pipe throughput |
| **B3. Processes** | Opus: the process core | `posix_spawn` (the override; file actions; attributes: mask, defaults, group, `setsid`, grants), the hand-over (A2's writer), the child table, `waitpid`/`wait4`/`waitid`, `getpid`/`getppid`, `execve` as spawn-wait-exit, process groups, `system`/`popen` checked | user/posix/sys/proc.c, spawn.c, the override of `src/process/posix_spawn.c` | posixtest's process section: 200 children spawned and reaped (any order, `WNOHANG`, by group); descriptors inherited and `FD_CLOEXEC` ones not; a child's `getppid`; `popen("ls /boot")` read whole; a hand-over cut short (the parent killed mid-spawn) leaves a child with no descriptors, not a hang |
| **B5. `poll`, `select`, `epoll`** | Opus: readiness is where wakes get lost | Section 7 over `<netwait.h>`: the per-thread set and its cache, `poll`/`ppoll`/`select`/`pselect`, `epoll_*`, each descriptor kind's ready and arm, `O_NONBLOCK` reads and writes against them | user/posix/sys/poll.c, epoll.c; the kinds' ready and arm hooks in B1's and B2's files (small, named) | posixtest's poll section: 200 pipes, each written once at random, every write seen exactly once; a poll woken by a write from another process; an epoll with 256 entries; a `poll` with no descriptors as a sleep |
| **T2. The terminal's POSIX side** | Opus: `termios` semantics and the focus | The line discipline (user/lib/ldisc.c) and keys into bytes (user/lib/keybytes.c), both pure; `console.open_tty` and the `tty` protocol (id 43) in the console's front end; the focus over keys and tty clients; Ctrl+C under `ISIG` (and question 6's answer); libc's terminal descriptor (`tcgetattr`, `tcsetattr`, `TIOCGWINSZ`, `isatty`, `ttyname`, SIGWINCH's source) | user/lib/ldisc.c, user/lib/keybytes.c, `abi/idl/console.idl`, the console's front end (after T1), user/posix/sys/tty.c | utests: the line discipline against a table of byte sequences and `termios` settings (erase, kill, `VMIN`/`VTIME`, echo, `ONLCR`), keys into bytes into keys round trip; posixtest's tty section on a connection console: canonical read of a typed line, raw mode, a resize as SIGWINCH (after B4) |
| **K2. Time page and `VMAR_REPLACE`** | Opus: kernel | Section 10's time page (`clock_page`, the sequence counter bumped by `wallclock_set`), `VMAR_REPLACE`; libc's clocks move onto the page | `kernel/dev/wallclock.c`, `kernel/arch/x86_64/tsc.c` (the conversion's export only), `kernel/abi/sysc_wallclock.c`, `kernel/mm/aspace.c` (the replace), their syscall lines, user/posix/sys/time.c | ktests: the page against `clock_get` over 10 s on 8 CPUs (never behind, within 1 µs), a `wallclock_set` seen by a reader mid-read (retry, no torn value); `VMAR_REPLACE` with another thread faulting on the range; a bench line: `clock_gettime` with and without the page |

### Wave C: signals, sockets, the shell, sessions, ports

| Track | Model, why | What | Files it owns | Needs | Gate and tests |
|---|---|---|---|---|---|
| **B4. Signals** | Opus: the subtlest part of the layer | Section 6: the signal thread, `sigaction`, masks, pending sets, delivery at the call boundary, `thread_interrupt`, `EINTR` and `SA_RESTART`, `sigsuspend`/`sigwait*`, `alarm`/`setitimer`/`timer_create`, `raise`/`abort`/`kill`/`pthread_kill`, SIGCHLD, SIGPIPE, SIGWINCH from T2, `pthread_cancel` (the `syscall_cp` override), exit status 128 + N, the `signal` protocol (id 93) | user/posix/sys/signal.c, sigthread.c, abi/idl/signal.idl, the overrides of `syscall_cp` and `src/thread/pthread_cancel.c` | K1, B3 | posixtest's signal section: a handler run, a blocked `read` and `poll` ended with `EINTR`, restarted with `SA_RESTART`; 10,000 signals between two processes none lost (counted); SIGCHLD for 200 children; a busy loop ended by SIGINT's default; `pthread_cancel` of a thread in `read` |
| **B6. Sockets and names** | Sonnet: mostly libjam's existing calls behind POSIX's names | Section 8: the socket kinds over libjam's net calls, non-blocking `connect`, the options (N1), `getaddrinfo` through `/svc/dns` (the override of `src/network/lookup_name.c`) | user/posix/sys/socket.c, the override of `src/network/lookup_name.c` | B5, N1 | posixtest's socket section against the network peer (`tools/netpeer.py`): a TCP client and server, 1 GiB checked, `poll` on 32 connections, UDP both ways, `getaddrinfo` of a name the peer's DNS answers; a new tools/posix-net-test.sh |
| **S1. The shell gives programs descriptors** | Sonnet: shell plumbing on A2's formats | A `posix` program gets descriptors 0 to 2 on its terminal (`open_tty`'s three, or the pipe or file a redirection names), its signal channel, an 8 MiB stack and the environment (`PWD`, `HOME`, `PATH`, `TERM`, `TZ`, `USER`, `LOGNAME`, `SHELL`); redirections `<`, `>`, `>>`, `2>`, `2>&1` for programs; `PATH` lookup after builtins | `user/services/shell/sh_program.c`, `user/services/shell/sh_parse.c`, `user/services/shell/sh_exec.c`, `user/services/shell/sh_vars.c` | A2, T2 | a new tools/shell-tests/posix.txt: posixtest run with each redirection, its output checked; native programs and every existing shell script unchanged |
| **X1. Sessions and pseudo-terminals** | Opus: who gets a shell is the security question of M13 | `/svc/session` in init (`session` and `session_ctl`, ids 91 and 92): a connection console and a shell per session, at most 8, supervised and ended as terminals are; `svc session`'s refusal for `/data` programs in `user/services/shell/sh_allow.c`; libc's `jamos_session_open`, the master descriptor, a session as a child for `waitpid`; `posix_openpt`/`openpty`/`/dev/ptmx` on a console of the program's own | new abi/idl/session.idl, init's new session file and its lines in `user/services/init/terms.c` and `user/services/init/services.c`, user/posix/sys/pty.c | T1, T2, B3 | a POSIX test program opens a session, types `echo hi` and reads it back, resizes, hangs up (the shell's job is gone after); 9 sessions (the 9th refused); a program without `svc session` refused; `allow` refuses a list with it; hostile: garbage on the master, a ctl closed first, 1,000 opens and closes |
| **P1. sbase** | Sonnet: a mechanical port | third_party/sbase (pinned, MIT), user/ports/sbase/ (the make fragment, each utility's list, overrides for `xargs`, `find -exec`, `time`), into `/boot/usr/bin` | those, the Makefile's include of the fragment, bootfs's list | B1, B2, B3 | a new tools/shell-tests/sbase.txt: about 40 utilities against expected output made on the Mac by the same sbase build for macOS (`ls`, `cat`, `sort`, `uniq`, `wc`, `head`, `tail`, `grep`, `sed`, `tr`, `cut`, `find`, `xargs`, `date`, `printf`, `od`, `cmp`), in pipes and with redirections |
| **P2. Lua and libc-test** | Sonnet: ports and a test runner | third_party/lua (5.4, MIT) as `/boot/usr/bin/lua` with its portable test set; third_party/libc-test (MIT) built as programs, run by our runner (its own forks), with an expected-failures list, each line with its reason | user/ports/lua/, user/ports/libc-test/, a new tools/libc-test.sh | B1 to B4 | Lua's tests in "portable" mode pass; libc-test's functional and regression sets: every failure on the list, none off it |

### Wave D: SSH, the shell's pipelines, mapped files

| Track | Model, why | What | Files it owns | Needs | Gate and tests |
|---|---|---|---|---|---|
| **X2. tinyssh** | Opus: the network-facing parser and the key handling | third_party/tinyssh (20261001), user/ports/tinyssh/ (the make fragment with a fixed feature header in place of its configure probes, the overrides of section 15, bin/sshd), init's host key and sshd's start (question 8), the shell's `ssh` command, a new tools/ssh-test.sh | those, init's ssh lines (a new init file plus its call in `user/services/init/services.c`), a new shell cmd file | X1, B4, B5, B6 | tools/ssh-test.sh in QEMU with OpenSSH's client on the test host through the peer's `--tcp-forward`: a key login and a scripted session (`echo`, an sbase pipeline, a resize, Ctrl+C on `sleep 100`, `exit` with its code); `ssh pc 'cmd'`; a wrong key and a password both refused; 8 sessions at once and a 9th refused; the host key the same on a second boot of the same image; tinysshd killed mid-session leaves no process (the job checked) |
| **S2. Pipelines and Ctrl+C in the shell** | Sonnet: shell work | Programs in a pipeline run at once, joined by pipes (builtins in a pipeline keep today's turns); Ctrl+C sends SIGINT to a POSIX program, then kills its job if it hasn't ended in 2 s; `$?` 128 + N; `jobs` and `kill %n` for POSIX programs (SIGTERM, then the job) | `user/services/shell/sh_io.c`, `user/services/shell/sh_exec.c`, `user/services/shell/sh_jobs.c`, `user/services/shell/sh_program.c` (after S1) | S1, B4 | tools/shell-tests/posix.txt grows: `yes \| head -3` ends, a three-program pipeline of 100 MiB, Ctrl+C on a pipeline, a program that ignores SIGINT killed after 2 s |
| **F1. Mapped files on M12.5's pagers** | Opus: it builds on M12.5's new kernel interface | Section 12's file mappings after M12.5: read-only and shared mappings of the pager VMO, the copy-on-write child if M12.5 has it, `msync`; `posix_spawn` of a program on `/data` through M12.5's loader, checked against `allow` | user/posix/sys/mem.c's file part, user/posix/sys/spawn.c's `/data` path | M12.5 merged, B1, B3 | posixtest's mmap section: a 100 MiB file mapped and summed without a copy (its pages counted), a shared write seen by `read`, the stick pulled under a mapping ends the access cleanly (M12.5's own test, from POSIX) |

### Reviews and the join

| Track | Model | What | When |
|---|---|---|---|
| **R3. Kernel review** | Opus | K1 and K2: findings first, then High and Medium fixed with a test each (the standing rule); the interrupt's races and the tid word's timing first | once K1 and K2 are merged (in wave C) |
| **R1. POSIX layer review** | Opus | A1, A2, B1 to B6, F1 if merged: findings first, then fixes; reads the call layer against musl's expectations (errno values, partial reads and writes, every handle closed on every path), the hand-over message as hostile input | after waves B and C merge |
| **R2. Terminal, sessions, SSH and shell review** | Opus | T1, T2, X1, X2, S1, S2: who can get a shell (`svc session`, its refusal, sshd's grants), the host key's handling, the line discipline's bounds, the console's two modes; then fixes | after wave D merges |
| **J. The join** | Sonnet | ARCHITECTURE (a "POSIX" section; the terminal, init's session service, the program's list, the Userland table), a new docs/PORTING.md (how to port a program: the sysroot, jamos-cc, a port's directory, its list, overrides), USING (SSH from the Mac: the key, the fingerprint, `ssh jam@pc`), TESTING (the new scripts), NETWORK (port 22), `third_party/VERSIONS.md`, CODING-GUIDE's tree table, ROADMAP and HISTORY, JAMOS_VERSION 0.2.0 (set at the release, after the PC sign-off), this plan to docs/history | last |

### Order and parallel work

1. **Wave A** as soon as M12's P6a and P6b are merged (the namespace and
   heap they change are what libjam splits): K1, A2, N1, T1 at once; A1
   when A2 hands back libjam (about half an hour in). Merge order: K1, A2,
   A1; N1 and T1 when they report.
2. **Wave B** once A1 and A2 are merged: B1, B2, B3, B5, T2, K2 (six
   agents; the PC takes about 8 QEMUs).
3. **Wave C**: B4 (after B3, K1), B6 (after B5, N1), S1 (after T2), X1
   (after T1, T2, B3), P1 (after B1 to B3), P2 (after B4), R3 (after K2).
4. **Wave D**: X2 (after X1, B4 to B6), S2 (after S1, B4), F1 (whenever
   M12.5 is merged; it may run in any wave after B3).
5. R1 after waves B and C; R2 after wave D; J; the main session's full
   regression on the PC; the PC sign-off.

**Beside M12.5 and M12.7**: every M13 track but F1 runs in parallel with
both. Where they meet:
- `abi/syscalls.def` and `<jam/abi.h>`: K1 and K2 edit only their groups'
  lines and take free numbers, merged one at a time with M12.5's pager
  calls; generated files are regenerated after each merge (`make
  syscalls`, `make idl`), never merged by hand.
- `user/lib/spawn.c`: A2 adds `stack_size` and the hand-over message;
  M12.5 changes the loader (programs loaded on demand). Whichever is
  merged second rebases; the two edits are in different functions.
- `kernel/mm/aspace.c`: K2's `VMAR_REPLACE` and M12.5's pager faults.
  K2 waits for M12.5's kernel track to merge if both are in flight.
- init: X1 and X2 add files of their own and a line each in `user/services/init/services.c`
  and `user/services/init/terms.c`; M12.7's installer, if it touches
  init, names its sections. fat and volumes: M13 doesn't touch them
  (question 5's A).
- The Makefile and bootfs's list: append only.

## Tests and the sign-off

| What | Where |
|---|---|
| Each fix's own test | ktest, utest or posixtest, named in the track's report |
| The kernel tracks | full ktest at 4 and 8 CPUs, the soak at 4, the bench lines (reply-and-wait, switch, `clock_gettime`) |
| The line discipline, keys into bytes, the hand-over message | utests (pure modules, table-driven, the hand-over fuzzed) |
| The POSIX layer | posixtest (user/posix/test/), one section per area, run by tools/shell-tests/posix.txt and a new tools/posix-test.sh; its expected answers written down from Linux on the same steps |
| musl's own tests | libc-test's functional and regression sets, tools/libc-test.sh, against the expected-failures list |
| Ported programs | tools/shell-tests/sbase.txt; Lua's portable tests |
| Sockets | tools/posix-net-test.sh (the network peer); `tools/tcp-test.sh` for N1 |
| Terminals | every existing terminal test unchanged (T1), the connection console utest, posixtest's tty section |
| Sessions and SSH | X1's session tests; tools/ssh-test.sh (OpenSSH's client through `--tcp-forward`) |
| Hostile | the hand-over message fuzzed; garbage and floods on the master, `tty`, `session_ctl` and `signal` channels; 1,000 session opens; an SSH client that stalls mid-handshake and one that sends garbage (tinyssh's own limits, then sshd's 8) |
| The bench | BENCH.md gains: pipe throughput (4 KiB and 64 KiB writes), `posix_spawn` + `waitpid` of a small program, `clock_gettime`, SSH throughput (`ssh pc 'cat /data/big' > /dev/null` on the Mac), each with Linux's number on the same PC where one exists |
| The regression | once, after J, on the PC: ktest at 4 and 8, the init run twice, every area script, the shell scripts, the soak at 4 and 8 |

**The PC sign-off**, in one session after the regression and a flash
(`make flash`; the owner's key put in `/data/etc/authorized_keys` first):

1. The everyday boot (desktop, sound, network, both sticks), then the
   host key's fingerprint from the shell's `ssh` command.
2. From the Mac: `ssh jam@<pc>`: the fingerprint matches; the Jam OS
   shell; `ls /boot/usr/bin`; `run ls -l /data | sort -k5 -n | tail -3`
   (sbase's); `lua -e 'print(_VERSION)'` and a Lua script from `/data`;
   the Mac's window resized while a program shows its size; Ctrl+C on
   `run sleep 100`; a second session at once; `exit` (the Mac sees the
   code). `ssh jam@<pc> 'ls /data'` without a terminal. A key not in the
   file refused; `ssh other@<pc>` refused.
3. The same programs in a terminal window on the desktop, with a pipe and
   a redirection into `/data`.
4. A program built on the Mac with tools/jamos-cc, copied to `/data`,
   `allow`ed, run.
5. Robustness: the Mac's Wi-Fi dropped mid-session (the session ends
   within tinyssh's timeout, nothing left running), `kill sshd` (init
   starts it again; the open sessions go on), `reboot` with a session open.
6. `bench` (the new lines, the reply-and-wait line against M12's).
7. **All tests and `soak 10`** with the second stick mounted read-write
   and pulled and replugged (the sign-off tier), and an SSH session open
   from the Mac running `run ls -l /data` in a loop through it.

Then the release, by the owner: 0.2.0 (the join's version bump, the tag,
the public image).

## Size

- **Agents**: 20 build tracks (K1, K2, A1, A2, N1, T1, T2, B1 to B6, S1, S2,
  X1, X2, P1, P2, F1), 3 reviews, 1 join: **24**, about 27 to 31
  agent-hours. Eight are Sonnet (N1, T1, B6, S1, S2, P1, P2, J; the
  regression's fix-up agents too), sixteen Opus.
- **Time**: wave A about 1.5 hours; B about 2; C about 2; D about 1.5
  (F1 whenever M12.5 lands); the reviews about 2 (R3 overlaps C); the join
  and the regression about 2.5 on the PC; one PC session. About three
  working days, the largest milestone yet, and it shares the PC's QEMU
  slots with M12.5 and M12.7.
- **Lines**: about 9,000 of our own (the call layer about 5,000, the
  console's back end and the terminal about 1,500, init's sessions and
  sshd about 800, kernel about 900, the shell about 600, ports' glue and
  tests the rest), beside well over 100,000 vendored lines (musl most of
  them; sbase, Lua, tinyssh, libc-test).

## Risks

- **musl's Linux assumptions** past the system calls: `/proc` (musl reads
  `/proc/self/fd` for `fexecve` and `ttyname`; both answered by the layer
  instead), Linux `ioctl` numbers (the layer decodes the terminal and
  socket ones, the rest `ENOTTY`), `AT_*` auxv entries. libc-test is the
  net under them; R1 reads musl's `src/` for every `__syscall` the layer
  answers `-ENOSYS` and lists which public functions that breaks.
- **Signals that look like Linux's but aren't**: a handler deferred to the
  next call, no fault signals. Written in PORTING.md as the first thing a
  porter reads; question 3's B stays open as an addition.
- **The console carries every terminal.** T1 is a pure refactor before
  the back end lands, with every terminal test unchanged; T2's focus
  change is the risky part (keys going to the wrong client): its tests
  open and close tty and key clients in every order. A console crash is
  as today (init starts it again).
- **Who gets a shell.** `/svc/session` is a full shell for whoever holds
  it. Only a list in the boot image may ask for it (the build's check),
  `allow` refuses it for `/data`, sshd hands it to tinysshd only, and
  tinyssh gives it only after a key in `/data/etc/authorized_keys` signed
  the session. R2 reads exactly this path. Anyone with the stick can add
  a key, as they could replace the kernel (ARCHITECTURE's threat list).
- **tinyssh parses what the network sends**, in a process that holds
  `/data` read-only (the host key's file among it) and the session
  service: its own record is good (small, no known remote hole), and the
  signing helper keeps the secret key out of the parsing process's
  memory. It holds no `dma_cap`, no root powers and no writable mount.
- **Ported binaries freeze protocols** (section 16): if M14 doesn't
  freeze `fs`, `file`, `net`, `dns`, `tty`, `session`, `signal` and the
  hand-over message with the system calls, an old ported binary breaks on
  a new build. Versions from the start; M14's list.
- **Pipes as messages are slow** for byte-at-a-time writers: measured;
  stdio buffers in musl make it rare; a byte stream object is the
  fallback, an addition.
- **M12.5 slips**: only F1 waits; file mappings work by copy meanwhile,
  and programs on `/data` still run from the shell.
- **The PC's QEMU slots**, shared with M12.5 and M12.7: at most 8 QEMUs;
  wave B is six tracks, so the waves of the three milestones are staggered
  by the main session.
- **Build time**: musl adds about 1,500 files to a clean build (once;
  make's dependencies keep it out of incremental builds), on the Mac and
  on the PC host.
- **Licences**: musl, sbase, Lua, libc-test MIT; tinyssh CC0/0BSD/MIT;
  all inside CODING-GUIDE's rule. busybox is out for that reason.

## The owner's answers (2026-10-09)

The owner: "pick your recommendation for everything unless unsure"; all as
recommended. **Q1** A: unmodified musl, the build's overrides, the Linux
call layer. **Q2** A: the line discipline in the terminal, with a `tty`
protocol. **Q3** A: signals in libc at the call boundary plus
`thread_interrupt`. **Q4** A: `posix_spawn` only; `execve` as
spawn-wait-exit; forks replaced by overrides. **Q5** A: offsets per
process in libc. **Q6** C: with ISIG off the program gets the byte;
Ctrl+Alt+C always reaches the shell on the desktop. **Q7** A: one user
`jam`, uid/gid 1000, home /data. **Q8** A: sshd on port 22 whenever
authorized_keys holds a key, `ssh = off` stops it. **Q9** A: sbase in
/boot/usr/bin after the shell's builtins. **Q10** A: the `jamos-cc`
wrapper.

## K1 notes

Choices K1 made where the plan left them open (the safest reasonable
option each time):

- **Numbers.** `thread_set_tid_word` 84, `thread_interrupt` 88,
  `thread_get_info` 89 (the threads group's free numbers) and
  `sys_public_info` 136 (the system information's range), all four in one
  block at the end of the threads group of `abi/syscalls.def`, so a merge
  beside another track's lines is a plain append. `ERR_INTERRUPTED` is -26.
- **Ids.** One space for pids and one for tids, each 1 .. 2^22 - 1
  (`IDS_MAX`; a tid is below 2^30 as musl needs). An id is taken when the
  object is made and given back when it is destroyed, not when it dies:
  a dead child whose handle its parent holds (a zombie, to POSIX) keeps
  its pid. Ids go out in rising order from a cursor; one given back
  behind the cursor is reached only after the wrap, and one given back
  ahead of it is held until the wrap, so no value is handed out again in
  the lap in which it was given back. The bitmaps are 2 MiB of the
  kernel's .bss (two spaces, used and held, 512 KiB each); a full space
  is `ERR_NO_RESOURCES` from `process_create` or `thread_create`.
- **The tid word** is cleared only while the process runs: a thread
  leaving a dying process leaves it alone (nobody in the process is left
  to join it, and futexes are private). A store that faults is skipped
  with no wake. The address is checked for alignment and the user range
  when it is set, not for being mapped.
- **thread_interrupt** needs `RIGHT_SIGNAL` on the thread, so thread
  handles gain it (`THREAD_RIGHTS`); a thread not started or already gone
  is `ERR_BAD_STATE`. A wait whose condition is met (a futex wake that
  picked the thread, a packet queued, the signals matched, a deadline
  passed: `ERR_TIMED_OUT` before `ERR_INTERRUPTED`) returns its own result
  and leaves the interrupt latched for the next interruptible wait, so a
  futex wake is never lost to an interrupt. The interruptible waits are
  only the system calls' (`sys_object_wait_one`, `sys_port_wait`,
  `futex_wait`, `sleep_until`); `channel_reply_wait`'s port wait, the
  kernel's own waits and the fault path are never interrupted. libc may
  see a spurious `ERR_INTERRUPTED` (an interrupt latched after it already
  ran the signal's handler at a call boundary): it restarts the call when
  nothing is pending. Two interrupts that meet in one wait end it once,
  so B4's rule is: the signal thread marks a signal pending before it
  calls `thread_interrupt`, and the woken thread looks at the pending set
  after its call returns; then no signal is lost however the interrupts
  fall.
- **cpu_ns** counts kernel time in the thread's own calls too (Linux's
  user + system); `process_info.cpu_ns` is the threads that left plus the
  live ones, as `proc_list`'s.
- **sys_public_info**: version, machine, total pages, page size, CPUs
  online; not the free pages (they stay behind `RIGHT_ROOT_SYSINFO`, and
  counting them drains the heap's caches).
- **Not done, for PK1's `vmo_read` wait.** Making a pager VMO's read
  interruptible would take: a `vmo_read_interruptible` for the system call
  only (the kernel's own readers keep the plain wait), its wait on the
  page through `thread_block_interruptible`/`waitqueue_wait_interruptible`
  returning `ERR_INTERRUPTED` only while the page is still absent and
  through `thread_interrupt_consume`, and a rule for a read cut short:
  `vmo_read` returns no byte count, so the simplest is "nothing promised
  in the buffer, read again" (a read has no side effects). The fault path
  (PK2) must stay uninterruptible: a fault has no `EINTR`.

## A2 notes

Track A2 (libjam and the hand-over) as built, with the choices the plan
left open:

- **libjam.a** (`build/uobj/libjam.a`, built by `make`) is libos's objects
  less `user/lib/heap.c`, `user/lib/string.c`, `user/lib/stdio.c` (the
  printf family, moved out of the old printf.c) and
  `user/lib/startmain.c` (environ and `libos_start`, which calls main). It
  still holds dr_mp3, Monocypher and the driver API: a program that never
  calls them doesn't link them. What it needs from a C library is exactly
  malloc, calloc, free, memcpy, memmove, memset, memcmp, strlen, strnlen,
  strcmp, strncmp, strchr, strrchr, strstr, printf, snprintf and
  vsnprintf; under musl its own log lines therefore go to descriptor 1.
  `user/lib/status.c` (once printf.c) holds `status_str` alone.
- **Startup**: `startup_read(ch, &argc, &argv)` (`<os.h>`) is the parser
  both starts call: the environment follows `argv[argc]`. It uses no
  heap, printf or thread-local storage, so the POSIX crt calls it before
  musl runs; its two log lines are written with `debug_write` itself
  (the same bytes as before: neither case has an `SR_STDOUT`).
- **The link test** in `make check`: every object of libjam linked with
  `tools/libjam-stub.c` (`--whole-archive`, `--fatal-warnings`: a C
  library function libjam needs and the stub lacks, or one both define,
  fails), and `tools/checklibjam.py`, which refuses a C library name
  defined by libjam: from its own list of the C standard's and POSIX's
  names, or, given musl's `libc.a` as a third argument, from musl's own.
- **The hand-over** (`<posixfds.h>`, `user/lib/fdmsg.c`; "JPSX", 0x5853 in
  `abi/idl/ids.txt`): a 64-byte header (version, `POSIXFDS_LAST`, a
  sequence number, counts), the first message alone carrying the state
  (umask, ppid, pgid, sid, the blocked and ignored signals) and the
  current directory; then 24-byte records, each with its data padded to
  8 bytes. Each kind has an exact number of handles of fixed object types
  and a data shape (a path, a name, or the layer's own bytes): the table
  is in fdmsg.c, for the B tracks to change before M14 freezes it. An
  epoll descriptor is not a kind: B5 decides whether a child gets one.
  Status flags are Linux's `O_*` values, from a fixed set; only a file or
  a directory has an offset; SIGKILL and SIGSTOP in a mask are refused.
  The starter writes the whole hand-over before `process_start` (spawn
  does), so the reader never waits: nothing after the startup message is
  "no hand-over" (`ERR_NOT_FOUND`), and anything queued after the last
  message makes it malformed. The reader gives descriptors to a sink
  only after their whole message passed its checks (the handles' object
  types included), and on any fault resets the sink, closes the
  message's handles and drains what is queued. One static 64 KiB buffer
  under a lock serves both sides, so no message is too big to read and
  drop.
- **`spawn_args`**: `stack_size` (0: 128 KiB; rounded up to pages; at
  most `SPAWN_STACK_MAX`, 1 GiB; committed as touched and charged to the
  caller's job, as every VMO spawn makes) and `handover` (consumed
  whatever happens, as the extras are).
- **The list**: `posix` grants nothing and may appear once; it is not in
  the text `allow` shows. `svc session` (`SVC_SESSION`) is accepted by
  the build for programs under `user/services/`, user/ports/ and
  `user/tests/` only; `allow`'s refusal for `/data` is X1's
  (`user/services/shell/sh_allow.c`). Found, not changed: libos's list
  parser takes a line with a trailing space ("posix ", "svc net ") that
  `tools/checkwants.py` refuses.
- **For A1**: `<os.h>` declares the C library's names itself and defines
  `PAGE_SIZE`, which musl's `<limits.h>` also defines: a file that
  includes both needs `#undef PAGE_SIZE` or a header of its own.

## A1 notes

Choices A1 made where the plan left them open, and what the B tracks
need to know.

**musl and the build**
- musl 1.2.6 is the whole release tarball, unmodified (third_party/VERSIONS.md
  has its hash, its signature check and the two advisories against it:
  the iconv GB18030 one, a slow-down, is not patched, since nothing uses
  iconv yet).
- `user/posix/posix.mk` (included at the end of the Makefile) builds it:
  src/ldso is left out, as the plan says, so `dlopen` and its kin are
  link errors, not run-time failures. A clean build of musl, the layer
  and the sysroot takes about 6 s at `make -j10` on the Mac (some 1,300
  files); after that it is never rebuilt unless its sources change.
- `build/sysroot/lib/libc.a` holds musl, the overrides, the call layer
  and libjam's objects in one archive, so `-lc` is the whole C library and
  a port's link line is crt1.o, crti.o, its objects, libc.a, libgcc,
  crtn.o (`tools/jamos-cc` does this). `build/musl/libmusl.a` (musl and
  the overrides alone) is what `tools/checklibjam.py` compares libjam
  with in `make check`.
- The overrides are listed in `user/posix/musl/OVERRIDES` with the
  upstream file's sha256; `tools/checkoverrides.py` checks them before
  musl is compiled and again in `make check`, with a self-test that a
  changed, missing, malformed or doubled line is refused. The overrides
  are thin: each calls the layer through `user/posix/sys/glue.h`
  (plain C, because they are compiled with musl's flags and headers).
  `src/process/posix_spawn.c`, `src/thread/pthread_cancel.c` and
  `src/network/lookup_name.c` are not overridden yet (B3, B4, B6):
  musl's `posix_spawn` fails cleanly today (its `clone` without
  `CLONE_THREAD` is `ENOSYS`), and `syscall_cp`'s override only makes the
  call (cancellation by signal comes with B4's).
- `tools/jamos-cc` defines `__jamos__` and not `__linux__`, puts musl's
  headers before the compiler's own, and links the one-word list
  `posix` unless `--jamos-wants=FILE` names another. posixtest is built
  with it, so the build tests the wrapper.

**The call layer's shape (for the B tracks)**
- Every musl system call ends in `__jam_syscall` (`user/posix/sys/calls.c`),
  a table indexed by Linux's number: `[SYS_x] = lx_x`. A number with no
  entry is `-ENOSYS`. Every handler has one signature (`lx_call` in
  `user/posix/sys/calls.h`): six `long`s in, a value or `-errno` out.
  To add a call: write `lx_<name>` in your area's file, declare it in
  your area's block of calls.h, and add `[SYS_<name>] = lx_<name>` to
  your area's block of the table. A number given twice fails the build
  (`-Woverride-init`). Only calls.c may include `<sys/syscall.h>`: its
  `SYS_*` names clash with Jam OS's own (`<jam/syscall_nums.h>`).
- Area files include `layer.h` (libjam through `<jamos/libjam.h>`,
  which settles `PAGE_SIZE` with musl's `<limits.h>`): `lx_err(st)` is a
  status's default errno; `lx_self_process()`, `lx_self_vmar()`,
  `lx_self_thread()`; `lx_handover(&cwd)`, the hand-over's state
  (umask, ppid, pgid, sid, the signal masks) or NULL.
- Where each area's code goes: files B1, B2, B3, B5, B4, B6, T2 and X1
  own as the tracks' table says. Two of A1's files are stand-ins to be
  replaced: stdfd.c (descriptors 0 to 2: the null device, the log, or
  a pipe; B1's descriptor table replaced it and took its handlers'
  table lines, and B2's kinds its pipe, null and log) and nosig.c
  (`rt_sigprocmask` kept a per-thread mask and nothing else; B4's
  `user/posix/sys/signal.c` replaced it).
- The start (`user/posix/crt/start.c`) reads the startup message, then
  the hand-over with `posixfds_read`; without one, stdfd's defaults. A
  hand-over's pipes and null devices at 0 to 2 are taken; every other
  descriptor's handles are closed (until B1). A malformed hand-over
  leaves no descriptors and a line in the log. B1 takes the sink over.

**Choices**
- The kernel's POSIX calls (thread_set_tid_word, thread_get_info, the
  CPU times, sys_public_info) are made only in
  `user/posix/sys/kposix.c`: a change to their shapes is a change to that
  file. Without sys_public_info's figures (a call that fails), `uname`
  says "unknown" and `sysconf` one CPU.
- Thread handles: `clone` keeps each new thread's handle in a table by
  its id (8,192 threads alive at once at most: `EAGAIN` past it), and the
  thread closes its own on its way out (`exit`, or `__unmapself` for a
  detached one). The first thread's is the startup message's. B4's
  `pthread_kill` and cancellation look threads up there.
- Futexes: private and shared are the same (no memory is shared between
  processes); `FUTEX_CMP_REQUEUE`, `WAKE_OP`, the PI and bitset
  operations and `FUTEX_CLOCK_REALTIME` are `ENOSYS` (musl uses none of
  them; robust and PI mutexes are not planned).
- mmap: libc's table of its mappings (`user/posix/sys/maptable.c`) is a
  VMO of its own, `VMAR_MAX_MAPPINGS` entries, so it is never the limit.
  `munmap` and `madvise` act only on the table's mappings: the program,
  its stack and libjam's mappings are never unmapped by libc, and an
  unmap of a range libc didn't map succeeds and does nothing. `MAP_FIXED`
  over a table mapping unmaps that part, then maps, under the layer's
  lock (not atomic against a vmar_map outside the layer: K2's
  `VMAR_REPLACE` makes it one step); over anything else it is `ENOMEM`,
  and `MAP_FIXED_NOREPLACE` over anything is `EEXIST`. Flags the layer
  can't honour (`MAP_GROWSDOWN`, `MAP_HUGETLB`, `MAP_32BIT`, `MAP_SYNC`,
  unknown bits) are `EINVAL`, stricter than Linux, which ignores unknown
  bits. `PROT_EXEC` is refused by `mmap` and by `mprotect`, also on the
  program's own code.
- brk: the 1 GiB reservation is made at the first `brk` call, not at the
  start (the same address-space cost, none for a program that never asks);
  it is a table mapping, so musl's malloc may put its guard page over it
  with `MAP_FIXED`, and brk keeps a VMO handle of its own for the
  decommit of pages above a lowered break.
- Descriptors without a hand-over: a terminal (`SR_CONSOLE`) does not
  make 0 to 2 a terminal yet (T2's open_tty); such a program writes to
  the log, as a native program in the foreground does, and reads the
  null device.
- The start: the auxiliary vector also has `AT_ENTRY` and the one user's
  ids (1000, Q7) as `AT_UID`, `AT_EUID`, `AT_GID` and `AT_EGID`;
  `AT_HWCAP` is CPUID leaf 1's EDX, as Linux's. `TZ` is added (from the
  wall clock's zone, as a POSIX rule) only when the starter gave none.
- `uname`: `sysname` is `JamOS` (one word, for scripts), `nodename`
  `jamos` until the `host.name` setting is read (B2's `/etc`),
  `release` and `version` the kernel's version from sys_public_info
  ("unknown" without it). `sched_getaffinity` (musl's
  `sysconf(_SC_NPROCESSORS_ONLN)`) and `sysinfo` (`_SC_PHYS_PAGES`) come
  from the same figures.
- posixtest is `bin/posixtest` (a test program, beside utest), last in
  `boot/init.cfg`. Its start section starts itself twice with libjam's
  `spawn`: once with a hard argv and environment, once with a
  hand-over (a pipe at 1, the null device at 0).

## T1 notes

What track T1 (the console's connection back end) built, and where the plan
left a choice.

- **Split.** `struct backend` in `user/services/console/console.h`: the
  front end (clients, focus, keys, kernel log lines, the loop) calls a back
  end for output, log lines, size, clear, drawing and its own events. The
  window back end is `winback.c` over the old files; the connection back
  end is `conn.c` and `connin.c`. A pure refactor commit came first; every
  terminal test ran unchanged on it.
- **Starting one.** `bin/console` with the argument `conn` (and `cols=<n>`,
  `rows=<n>`, default 80x24, for the size until the control channel says
  another). Startup handles: `SR_USER + 0..7` the ADMIN client channels as
  for any console; `CONN_MASTER_ROLE` (`SR_USER + 11`) the master, required;
  `CONN_CTL_ROLE` (`SR_USER + 12`) the control channel, optional;
  `SR_RESOURCE` optional, `RIGHT_ROOT_KLOG` alone. `<termconn.h>` has the
  roles and limits.
- **The control channel is its own protocol, `conn_ctl` (id 42,
  `abi/idl/conn_ctl.idl`)**, one method, `resize (u16 cols, u16 rows) ->
  ()`, served by the console. It is the console's link to its owner, not
  the plan's `session_ctl` (id 92): that one is the SSH side's, and init
  relays between the two (`ended` is init's to say: the console doesn't
  know the shell's exit code). Ids 90 to 99 stay free for M13's other
  protocols.
- **The master** is a pipe of byte messages both ways, no framing and no
  handles (a handle sent on it is closed). Out: messages of at most 4096
  bytes, in order; a bare `\n` becomes `\r\n`; the escape sequences are the
  remote terminal's to read. In: any size up to 64 KiB, as xterm sends it.
- **Output is never dropped for a slow reader, and the clients wait.** The
  output goes into a 64 KiB ring; while less than 4 KiB is free the console
  serves no client request (their writes wait, as into a full pipe) and
  starts again as the master drains; the master's own queue holds 1,024
  messages. Kernel log lines are the one exception: a line that doesn't fit
  is dropped and a notice says how many bytes. The input path never waits
  on output, so Ctrl+C is read while output is stalled; a shell blocked in
  a write cannot read it until the master drains (sshd's job: keep reading,
  and drop a client that stops).
- **Hang-up.** The master closed (by the owner, or found closed by a write)
  ends the console with code 0: its clients see their channels close (a
  shell exits 2). The console ending closes the owner's ends. The control
  channel closed changes nothing but that the size stays.
- **Size limits.** A resize past 480x180 (the window console's grid limits)
  is reported as 480x180, not refused, so a client's buffers sized for the
  window hold; zero is `ERR_INVALID_ARGS`. The size changes `console.size`
  only: nothing is told when it changes (the shell asks); T2's `tty` size
  event belongs in `conn_ctl_resize`'s handler in `conn.c`.
- **Keys.** `<termkeys.h>`'s keys go to the focus as a serial terminal's do
  (usage 0 and the byte for text and control characters). A lone ESC that
  waits 50 ms alone is the Escape key. A bracketed paste's markers, which
  termkeys drops, are typed to the client that asked for bracketed paste
  (the console reads `ESC [ ? 2004 h` and `l` in the output to know), as
  the window's paste does, and dropped for another. A key the focus's
  channel can't take is dropped, as in the window; a paste of more than
  its 1,024 keys in one burst loses the tail (pacing it would hold Ctrl+C
  behind it).
- **The log** starts at the end of the kernel's ring (no replay of the
  boot's), stays off unless a client asks (`console.show_log`), and each
  line goes out as text with any control character shown as `?`.
- **Not in T1**: `open_tty`, the line discipline and the `tty` protocol
  (T2); `/svc/session` and who starts a connection console (X1);
  `TERM`. A connection console has no alternate screen of its own: the
  remote terminal's is used, since the escapes pass through.

## B2 notes

Track B2 (pipes, dup, /dev and /etc) as built, with the choices the plan
left open. The files: `user/posix/sys/kinds.h` (the kinds' header),
pipe.c, dup.c, dev.c, etc.c, memfd.c (and memfd.h), eventfd.c and user.c.

- **The stand-ins are gone.** B1's stdfd.c (its pipe, null and log
  kinds and fcntl's four commands) is replaced: fd.c's `fd_adopters`
  point at `pipe_adopt`, `dev_adopt`, `etc_adopt`, `memfd_adopt` and
  `eventfd_adopt`, and the log kind and `stdfd_default` (descriptors 0 to
  2 without a hand-over) are dev.c's.
- **Paths.** /dev and /etc are reached through B1's path calls, which ask
  kinds.h first: `dev_path`/`dev_open`/`dev_stat`/`dev_unlink`,
  `etc_path`/`etc_open`/`etc_stat` (each open installs its descriptor
  itself). Until B1's open calls them, posixtest checks those paths only
  through a hand-over and says so. Listing /dev and /etc is not built
  (they stat as directories). /dev/tty is ENXIO (no terminal yet: T2's)
  and /dev/ptmx ENOENT (section 9).
- **A pipe's leftover moves to a child** with the descriptor (A2's data
  for the kind): the bytes are the description's, and the child is the
  next reader in the usual case (a shell handing its input on). A
  leftover bigger than a hand-over's data (`POSIXFDS_DATA_MAX`, 4 KiB)
  refuses that descriptor's hand-over (ENOBUFS) rather than lose or
  reorder bytes. A spawn that fails after asking loses them.
- **Pipes**: `pipe2` takes `O_CLOEXEC` and `O_NONBLOCK` only (`O_DIRECT`,
  packet mode, is EINVAL); `F_GETPIPE_SZ` is 64 KiB (a message), and
  `F_SETPIPE_SZ` changes nothing and says 64 KiB. A message carrying
  handles (none of ours do) has them closed. SIGPIPE is not sent until
  signals exist (B4): EPIPE alone.
- **socketpair** is `AF_UNIX`, `SOCK_STREAM` only (datagrams and
  `SOCK_SEQPACKET`: EOPNOTSUPP); a handed-over end is the pipe kind with
  `O_RDWR`, which the child takes as a socketpair end. `send`/`recv` on
  it come with the socket calls (B6), which must route a socketpair end
  to its read and write.
- **fcntl**: `F_SETFL` changes `O_APPEND` and `O_NONBLOCK`; `O_ASYNC` is
  EINVAL (no SIGIO is ever sent), `O_DIRECT` and `O_NOATIME` are ignored.
  Record locks are checked (type, whence, the access mode) and always
  granted, `F_GETLK` says `F_UNLCK`. `F_SETOWN` keeps nothing. Anything
  unknown is EINVAL.
- **memfd and shm_open** are kept VMOs (`VMO_KEEP_PAGES`). A kept VMO
  never shrinks, so a smaller `ftruncate` keeps the pages and shrinks
  the file in libc; growing over them again zeroes them. The size and
  the seals are the process's own: a child starts with the parent's, and
  each keeps its own after (they share the pages). Seals hold in this
  process only; a process that must trust a size checks the VMO. A
  read-only descriptor's VMO goes out without write or resize rights.
  `shm_open`'s names are the process's own (no service holds them; at
  most 64): another process sees an object only through a descriptor.
  `MAP_SHARED` of a memfd: B1's mmap calls `memfd_vmo`.
- **eventfd**: the counter is in the description (the plan's "a counter
  in memory"), the event's user bits 24 (readable) and 25 (writable)
  follow it, for the waits and for poll. A child gets a copy of the
  counter and an event of its own, not the parent's: the hand-over's
  kind has no shared memory for the counter, and a shared event over two
  counters would say ready wrongly. Sharing it needs a VMO in the kind.
- **The one user**: the set calls succeed for 1000 (and -1) and are
  EPERM otherwise; `setgroups` is EPERM; `getgroups` is {1000}. musl's
  set calls go through `__synccall`, which needs signals once the process
  has a second thread (B4).
- **/etc**: passwd, group, hosts (localhost, and 127.0.1.1 for `jamos`,
  uname's nodename), services, resolv.conf (empty), shells. There is no
  `host.name` setting yet (<settings.h>), so the name stays `jamos`.
- **For poll (B5)**: each `fd_ops` table has a commented `.ready, .arm`
  line. Pipe ends are ready to read with a leftover or the channel's
  `SIG_READABLE`/`SIG_PEER_CLOSED`, to write on `SIG_WRITABLE`. Eventfd
  uses its event's bits. /dev, /etc and memfd have none (always ready).
- **For the review**: the layer's global names (`dev_open`,
  `pipe_adopt`, `fd_new`, ...) share the program's namespace in libc.a:
  a port that defines one of them fails to link. Hiding them (a prefix
  reserved for the layer, or localizing them when libc.a is made) is a
  question for the whole layer.

## B5 notes

Track B5 (`poll`, `select`, `epoll`) as built, with the choices the plan
left open. The model and its argument are in `user/posix/sys/ready.h`;
the cache in `user/posix/sys/poll.c`'s header; epoll's rules in
`user/posix/sys/epoll.c`'s.

**What changed outside B5's files**
- `<netwait.h>` gained two things. `NETWAIT_EDGE` (handles only): an
  entry reported ready is armed instead of staying pending, and its read
  and write signals are bound apart, so a read edge is seen while the
  write side stays up (epoll's `EPOLLET` on a socketpair). And
  `netwait_block`: sleep until a packet is queued, taking none, which is
  how a waiter looks at libc's own state (a pipe's leftover) between the
  set's look and the sleep without losing a wake. utest `netwait_edge`.
- fd.h and fd.c (B1's): `struct fd_desc` has a `serial` (never 0, never
  given twice), set in `fd_new`; `fd_put` calls `epoll_forget(d)` before
  the kind's close (declared in fd.h).
- pipe.c and eventfd.c (B2's): the `.ready`/`.arm` lines and their two
  small functions.
- calls.c and calls.h: B5's block (poll, ppoll, select, pselect6, the
  five epoll calls); posixtest's main.c and pt.h: the poll section and
  the `--child-poll` mode.

**Choices**
- **The cache** is keyed by the description's serial, not its address
  or handle: a description freed and another made at the same address
  with a handle of the same value is never taken for the old one (whose
  binding the kernel ended when its handle closed). A place keeps what
  the kind's arm said and the interest; any difference binds again.
  Places not asked for in 64 calls go, and so does one the set reports
  ready when the call didn't ask for it. The set is freed by a
  thread-specific key's destructor (pthread_exit runs it), so thread.c
  is not touched.
- **The order of a look**: the set (a past deadline), then each kind's
  `ready`, then `netwait_block`. A kind's `ready` takes no lock and makes
  no system call (it runs under epoll's lock); pipe.c's reads the
  leftover's two counters with relaxed atomic loads, while pipe.c writes
  them under the description's lock with plain stores (a stale answer is
  corrected by the next look, since a leftover only comes off the
  channel, whose rise queues a packet).
- **Hang-ups**: the set's HUP is POLLHUP, but POLLERR for a handle with
  no read signals (a pipe's write end whose reader is gone, as Linux).
- **select**: a descriptor only in the exception set is checked (EBADF)
  and left out, since POLLPRI never comes; a hang-up counts as writable
  as well as readable, so a writer finds out (EPIPE). select is its own
  file (select.c, poll.c's core through pollcore.h). The table's call
  that clashed with `<sys/select.h>`'s `fd_set` type is `fd_place` since
  B1, so no file needs a rename around fd.h.
- **epoll**: an item is (number, description) and holds no reference:
  `epoll_forget` takes it out of every set when the description goes
  (a global list of sets, skipped while no set has an item). A kind with
  no arm (a file, /dev) is EPERM, an epoll descriptor in a set or in
  poll/select EINVAL. Unknown event bits are EINVAL (Linux ignores them);
  `EPOLLEXCLUSIVE` and `EPOLLWAKEUP` are accepted and do nothing.
  `EPOLLET` on a socket is EOPNOTSUPP until a socket entry can be an edge
  one (B6). A full set is ENOSPC. A child is never handed an epoll
  descriptor (`give` is ENOTSUP). A look asks the set for no more events
  than the caller's array holds, so turns of a few reach every ready item.
- **Edge-triggered** = a rise of the kind's signals (or of its own
  `ready`, for a leftover another thread read into the buffer). A second
  message into a pipe that wasn't drained is no edge (Linux before 5.5
  behaves so too); a program that drains to EAGAIN, as `epoll(7)` asks,
  sees every write.
- **Masks**: ppoll, pselect6 and epoll_pwait set nosig.c's mask for the
  wait and put it back after; B4's signal.c makes it one step with the
  wait. **EINTR**: `netwait_block` is the system call wait K1 made
  interruptible; ERR_INTERRUPTED is returned as EINTR and B4 decides
  restarts.

**For later tracks**
- **B6 (sockets)**: a socket's `arm` gives `FD_WAIT_SOCK`. poll does not
  cache a socket's place: it goes at the end of the call, because the
  set reads the rings at every look and a socket's close can't reach
  another thread's set to take it out first. Keeping them (tinyssh's loop
  would bind and unbind its socket each call) needs the rings to outlive
  every place, e.g. the description keeping them mapped until its places
  are gone. epoll is safe as it is (`epoll_forget` runs before the
  kind's close). Edge entries for sockets are B6's too.
- **T2 (terminals)**: `struct fd_wait` has one handle; a terminal is read
  through its input pipe and written through its output pipe. The arm
  gets the interest, so a poll for one side works; for both at once,
  fd_wait needs a second part (poll.c's place and epoll.c's item would
  then hold two entries).
- **B3 (spawn)**: an epoll descriptor's `give` is ENOTSUP: leave it out
  of a child's hand-over.

## B3 notes

Track B3 (processes) as built, with the choices the plan left open (the
safest reasonable option each time):

- **Files.** `user/posix/sys/proc.c` (ids, groups, sessions, the child
  table, `wait4`/`waitid`), `user/posix/sys/spawn.c` (`posix_spawn`'s
  request, the program, the state, `execve`), `user/posix/sys/spawnfd.c`
  (the copy of the descriptor table and the file actions; split from
  spawn.c to keep each file one job), `user/posix/sys/proc.h` and
  `user/posix/sys/spawn.h`; the override of `src/process/posix_spawn.c`
  (thin: it reads musl's attributes and file actions into
  `struct jam_spawn_req`, `user/posix/sys/glue.h`); the extension
  `posix_spawnattr_setgrants_np` in `user/posix/include/jamos/spawn.h`
  (the list kept in `posix_spawnattr_t`'s spare bytes; NULL, the default,
  is everything the parent has).
- **The table copy** holds a reference to each description (B1's
  `fd_next`), not descriptor numbers: another thread's close mid-spawn
  changes nothing. File actions apply to the copy; an open action opens
  in the parent with `O_CLOEXEC` and keeps only the reference; nothing is
  exported (`ops->give`) until every action worked. A description in two
  slots (a `dup2`) becomes two descriptions in the child (the hand-over
  can't say "the same"). A kind without `give` (the log, epoll) is left
  out, as `FD_CLOEXEC` would.
- **The program.** `/boot/<x>` is the boot image's `<x>`; `/bin/<x>` and
  `/usr/bin/<x>` are the boot image's `bin/<x>` and `usr/bin/<x>` (there
  is no other `/bin`; musl's `system`, `popen` and default `PATH` name
  `/bin/sh` and `/bin`). Any other path: `ENOENT` if missing, else
  `EACCES` (no program from a file system until M12.5's loader), checked
  with `fs_stat` before spawn reads anything. `posix_spawnp` searches the
  caller's `PATH` (musl's default without one).
- **Ids.** In the hand-over, `pgid` and `sid` 0 mean "the child's own
  pid" (nobody knows it before `process_create`). Without a hand-over a
  program's parent is 1 and it leads a group and a session of its own
  (so its `setsid` is `EPERM`, as a session leader's). `setpgid` accepts
  a group of our session that exists (ours, or a child's), or the
  target's own pid; a session leader can't change group. A child is told
  its group and session once, at the start: a later `setpgid(child, g)`
  changes our table, not what the child's `getpgid(0)` says.
- **Waits.** One port with every child bound (`SIG_TERMINATED`,
  `PORT_BIND_ONCE`, keyed by a serial number, never a pid). One waiting
  thread at a time blocks on the port; the others sleep on a futex word
  bumped whenever an end is noticed or the reader leaves; a non-reader
  that notices an end (a `WNOHANG` drain, the signal thread) queues a
  key-0 packet so the reader looks again. At most 4,096 children unreaped
  at once (`PORT_MAX_BINDINGS`): `posix_spawn` is `EAGAIN` past it (the
  child is killed). `WUNTRACED`/`WCONTINUED`/`WSTOPPED` are accepted and
  never report (no stops). `rusage` gives the child's CPU time as user
  time.
- **Exit status.** A code `LX_EXIT_SIGNAL` (0x10000) plus N is
  `WIFSIGNALED` with `WTERMSIG` N (B4 exits that way for a signal's
  default action); killed by the kernel is SIGKILL.
- **For B4.** `lx_sigchld_hook(pid)` in proc.c is the one place a
  child's end is noticed (called once per child, no lock held): today it
  does nothing, and an end is only noticed by a wait. B4's signal thread
  should wait on `lx_child_port()` too and hand each packet to
  `lx_child_packet()`, which notices the end, wakes the waits (and the
  blocked reader) and calls the hook. `lx_exec_child()` is the pid
  `execve` is waiting for, to forward every signal to. The child's
  signal channel (`SR_SIGNALS`) is not made yet: spawn.c's `start` is
  where B4 adds it (an extra handle) and the table (`lx_child_add`) where
  it keeps the parent's end.
- **execve** closes all of our descriptors after the spawn (a pipe end
  held by the waiting caller would hide its end of file), then waits and
  exits with the child's status; other threads of the caller keep
  running until then. The child gets our parent's pid, group and session.
  `execve` returns `-1` and `errno` (and nothing changes) if the program
  can't be started.
- **Scheduling attributes** (`POSIX_SPAWN_SETSCHEDPARAM`,
  `SETSCHEDULER`): `ENOSYS` (musl's setters are `ENOSYS` already).
  `POSIX_SPAWN_RESETIDS` and `USEVFORK` do nothing (one user; no vfork).
- **Not done here, waiting for B1:** the umask (`spawnfd_umask` gives the
  hand-over's, or 022) and the current directory (`getcwd`, else the
  hand-over's, else `/`) until B1's file.c and dir.c keep them;
  `addchdir` checks its directory with libjam's `fs_stat`.
- **/bin/sh until a POSIX shell** (`user/posix/progs/sh/`, bootfs
  `bin/sh`): `sh -c` of one simple command only (words, quotes,
  backslashes; anything else, `|`, `;`, `$`, globs, ..., is refused with
  exit 2), run with `execvp`; 127 not found, 126 not runnable. It exists
  so `system` and `popen` work; the native shell's builtins still win
  over it. posixtest's `ls` is a tiny one at bootfs `test/ls`
  (`user/posix/progs/ls/`, libjam's `fs_readdir`), not on any `PATH` but
  the test's own, so sbase's `ls` is never shadowed.
- **posixtest's list** gains `mount /boot r`: its children list `/boot`.
- **A hand-over cut short.** posix_spawn writes all of it before
  `process_start`, so a parent killed mid-spawn leaves no child, or one
  with everything; the test kills a spawning parent at random times in
  its own job and checks every process in that job ends, and feeds the
  reader a first message without its last to check it gives up at once
  with every handle closed.
- **Found, not changed:** B1's `fd.h` declares `fd_set`, which is also
  `<sys/select.h>`'s type (included by `<sys/wait.h>` and
  `<sys/time.h>`): a layer file can't include both (spawn.c avoids
  `<sys/wait.h>` for that).
- **Found, not changed (kernel):** killing a process while it loops
  libjam's `spawn` of short-lived children panics the kernel about 60 ms
  later at 2 CPUs (3 runs of 3; 0 of 4 at 4 and 8 CPUs): a page fault
  in `thread_reap` (`kernel/sched/thread.c`), in its inlined `stack_put`
  writing below a `stack_top` of 0, so a thread reaped twice or after it
  was freed and reused. No POSIX code is needed for it (the native
  `spawn` loop alone does it; the same loop without the kill, or killing
  a loop that only creates processes and threads it never starts, does
  not). posixtest's hand-over check (the parent killed mid-spawn) is what
  reaches it: `QEMU_SMP=2 tools/qemu-test.sh build/test init init`.

## T2 notes

Track T2 (the terminal's POSIX side) as built, with the choices the plan
left open (the safest each time):

- **Ids.** The `tty` protocol is **43** (in `abi/idl/console.idl`): 90 to
  92 are devmgr's (M12's D). Section 9's `session` (91) and `session_ctl`
  (92) collide with devmgrctl and crasher, and section 6's `signal` (93)
  sits in devmgr's block: X1 and B4 take free ids (44 onwards).
- **One data channel, not two pipes** (B5's note above). `console.open_tty
  () -> (handle tty, handle data)`: `data` is a pipe both ways (as a
  socketpair's end): the console writes what is typed, the client writes
  its output. `struct fd_wait` holds one handle, and this way the client's
  one end says readable, writable and hung up (the console gone) with no
  user bits to keep right; fd.h is unchanged. `POSIXFDS_TTY` is two
  handles (the tty channel, the data channel: `user/lib/fdmsg.c`). The
  console keeps a duplicate of the client's end (read rights) to take
  back the input it left unread.
- **open_tty** works on a client of any level; 8 at once; it takes the
  focus as `open_keys` does (a PROGRAM client still can't push the shell's
  focus out). The focus stack holds key channels and ttys; a tty leaves
  it when its `tty` channel closes, or when it breaks the protocol (an
  output message over `TTY_OUT_MAX`, 2048 bytes: console.write's limit,
  so the connection back end's room rule holds).
- **The line discipline** (`user/lib/ldisc.c`) is one per console, with
  Linux's bits and c_cc indices, its defaults Linux's but `IXON` off (no
  flow control is built) and `IUTF8` on (erase takes a UTF-8 character).
  A bit it has no meaning for is kept and changes nothing, as on a Linux
  pseudo-terminal (refusing them would break every port that copies a
  termios about). Ctrl+Z (`VSUSP`) under `ISIG` is swallowed (no job
  control in M13). `ISIG`'s keys echo as `^C` and `^\` by `ECHOCTL`, as
  on Linux.
- **Ctrl+C.** With `ISIG` on (Q6 C): the input is flushed (the
  discipline's and the tty's unread messages), and the key goes to the
  newest SHELL or ADMIN key channel below the tty, as Ctrl+C reaches the
  shell today (Ctrl+\ as the key with code point 0x1c); with no shell's
  keys below (a pty, a program alone on a terminal) the tty gets the
  `signal` event (`TTY_SIG_INT`, `TTY_SIG_QUIT`). With `ISIG` off it is
  the byte 3. In a window, Ctrl+Alt+C never reaches a program: it is
  Ctrl+C to the newest SHELL or ADMIN key channel (keys.c), whatever has
  the focus.
- **Type-ahead.** What the discipline makes ready goes to the newest tty
  at once, one message a record. When a tty goes, the messages it left
  are read back and put at the front of the discipline's queue (16 KiB and
  256 records at most: what doesn't fit is dropped); when no tty is left,
  the queue and the line half typed are typed into the keys focus as keys
  (through `<termkeys.h>`), so a line typed under a POSIX program reaches
  the shell after it. Keys typed before a program opened its tty are the
  shell's (the shell keeps them as type-ahead today).
- **drain** and `set_attr` after a drain are answered once the client's
  output has been read off the data channel and handed to the back end
  (for a connection, into its ring, not necessarily onto the master yet).
- **Output** goes through the discipline's `OPOST` (`ONLCR`, `OCRNL`) and
  a new back-end hook, `write_raw`: the connection back end adds no CR of
  its own; the window's text model reads CR and LF as it always has, so
  with `OPOST` off a bare LF still starts the line at column 0 there (the
  window's main screen has no "down one row"). The application cursor
  mode (`ESC [ ? 1 h`) is scanned in each tty's output and changes the
  bytes of its cursor keys.
- **A paste** into a tty (Super+V) goes to its discipline at once as
  bytes, bracketed if it asked (`ESC [ ? 2004 h`): no pacing, so a raw
  reader's queue bounds it (`LD_READY_MAX`, then the channel's 1024
  messages of up to 4 KiB).
- **TERM.** The window console draws: SGR colours (30-37, 40-47, 90-97,
  100-107, 0, 1, 22, 39, 49), `ESC [ K`, `C`, `D`, `H` (column 0 only),
  `ESC [ 2 J`, and on the alternate screen (`ESC [ ? 1049 h`) `H`/`f`
  with a row and column, `A`, `B`, `J`, `K`; DEC modes 25, 2004, 2026
  and 1. That is less than a vt100 on the main screen (no cursor
  addressing, scroll regions, reverse video, `ESC [ 0/1 J`, `ESC [ 1/2
  K`, insert or delete line): **`TERM=vt100`** is still the name to give
  a program in a window (the plan's default), and the gaps are a
  follow-up before any curses port. Over a connection the remote
  terminal's `TERM` passes through. Nothing sets `TERM` yet: S1 (a
  window's) and X1 (a session's, from `open`'s `term`).
- **libc's descriptor** (`user/posix/sys/tty.c`): each read asks the
  console for the settings (another process on the terminal may have
  changed them), then reads by `ld_read_deadline`; `FIONREAD` is the
  bytes the next read gets without waiting; `TIOCSWINSZ` is `EINVAL`
  (the size is the terminal's); `TCXONC` is accepted and does nothing;
  the process-group calls (`TIOCGPGRP`, `TIOCSPGRP`, `TIOCSCTTY`) are
  `ENOTTY` until B3/X1; a write to a hung-up terminal is `EIO`, as on
  Linux. `ttyname` is `/dev/tty` (musl's `ttyname_r` is overridden: no
  `/proc`). The process's terminal is its first terminal description,
  held for its life; B2's `/dev/tty` opens it again (the description
  shared, as a dup).
- **Without a hand-over**, a starter's `SR_CONSOLE` makes descriptors 0
  and 2 the terminal, and 1 too unless it gave `SR_STDOUT` (a program in
  a shell's pipe writes its output there and its errors to the terminal,
  as on Unix). The console channel itself stays the program's.
- **Not done**: SIGWINCH and the `signal` event's SIGINT (B4: below). The
  window's keys through `<keybytes.h>` and Ctrl+Alt+C are checked by the
  `keybytes` utest and by reading, not by a window test (no utest drives
  a window console's keys).
- **Found and fixed**: the front/back split left the window back end's
  `replay_log` unset, so a window console (started or restarted) drew none
  of the boot log; `console-restart.txt` failed on every run, not only
  under load.
- **Edits outside T2's files** (small): `user/posix/sys/fd.c` (the
  `POSIXFDS_TTY` adopter line), `user/posix/sys/dev.c` (`stdfd_default`
  asks `tty_stdfds` first; `/dev/tty` opens and stats the process's
  terminal), `user/posix/sys/kinds.h` (that comment), `user/lib/fdmsg.c`
  and `user/include/posixfds.h` (the tty kind's two handles),
  `user/posix/sys/glue.h` and `user/posix/musl/OVERRIDES` (`ttyname_r`),
  and utest's connection-console rig moved out of `conconn.c` into
  `conrig.c` (a refactor commit) for `contty.c` to share.

**For B4 (signals).** `tty.h`: the signal thread waits on
`tty_event_handle(d)` (`SIG_READABLE`) for the process's terminal
(`tty_controlling()`) and calls `tty_take_signals(d)`, which reads every
queued event and returns the signals they raise as bits (SIGWINCH for a
resize, SIGINT or SIGQUIT for the `signal` event). Until then the events
wait on the channel (the console drops one for a full channel). A read
blocked on the data channel ends with `EINTR` on `ERR_INTERRUPTED`; the
`tty` calls are channel calls (never interrupted).

**For S1 (the shell gives programs descriptors).** Giving a POSIX
program `SR_CONSOLE` (a PROGRAM client, as today) is enough: libc opens
its terminal. To share one terminal between a pipeline's programs, open
it once (`console.open_tty`) and hand each child duplicates as
`POSIXFDS_TTY`. The shell's Ctrl+C while a POSIX program runs arrives as
today (a key on its own key channel); with B4 it should send SIGINT on
the program's signal channel instead of killing its job, and print no
`^C` of its own (the line discipline echoed it). Ctrl+\ arrives as the
key with code point 0x1c (SIGQUIT). The settings outlast a program: a
raw-mode program that crashes leaves the terminal raw for the next POSIX
program, as on Unix; the shell may `set_attr` the defaults after one ends
if it wants a clean terminal. Set `TERM=vt100` for a window.

**For X1 (sessions).** Ids above. The size already reaches every tty
client as the `size` event through `conn_ctl.resize`. A pty's slave is
`console.open_tty` on a connection console's client; its name for
`ttyname` (`/dev/pts/N`) needs a name field in tty.c's description
(today every terminal is `TTY_NAME`, `/dev/tty`). The `signal` event is
how a program alone on a pty gets SIGINT.

## B1 notes

Track B1 (files and directories) as built, with the choices the plan
left open. The files: `user/posix/sys/fd.h` and fd.c (the table),
files.h (what the file side shares), path.c (paths, the current
directory, a mount's answers as errno), fobj.c (a file's one open in a
process), file.c (the file kind, open, truncate, mmap of a
descriptor), dir.c (the directory kind, getdents64, mkdir, unlink,
rmdir, rename, chdir, getcwd), meta.c (the stat family, statfs, access,
chmod and the rest FAT has no answer for); posixtest's files.c,
files2.c, fileskid.c and ptfiles.h; `tools/shell-tests/posix-files.txt`.

- **The kind interface** (fd.h): `struct fd_ops` per kind (read and
  write take an iovec and a position, FD_AT_OFFSET for the description's
  own; `write_at` for pwritev2's RWF_NOAPPEND; seek, stat, ioctl, sync,
  truncate, getdents, setfl, path; ready and arm for poll; give for
  posix_spawn; close), `struct fd_desc` (refcounted; a lock for the
  offset and the kind's per-call state; status flags read and set
  atomically). Slots: `fd_get`/`fd_put`, `fd_alloc` + `fd_place` (a
  reserved slot, so EMFILE comes before an open's side effects),
  `fd_install`, `fd_replace` (EBUSY onto a reserved slot, as Linux's
  dup2), `fd_next`, `fd_give`. A NULL hook answers what Linux answers a
  kind without the call (ESPIPE, ENOTTY, ENOTDIR, EINVAL).
- **The hand-over is staged.** start.c reads it before musl runs, so
  fd_stage only records (no malloc): records in a static array, their
  data in a VMO mapped for the purpose (committed as touched). A
  constructor at priority 101 (before any of the program's) makes the
  descriptors through `fd_adopters`, one line per POSIXFDS kind; a kind
  without an adopter is left out with a line in the log.
- **One open per file per process** (fobj.c). fat lets a file have one
  writable open and nothing else beside it but reads (ERR_BUSY), which
  the plan's "fat already serves several opens of one file" missed for
  writers. So every description of a file in a process shares one open,
  found by the mount and fs.stat's id; it is opened again to write when
  a description first needs it. The process then holds the file's only
  writable open, so it knows the size, and **O_APPEND is the layer's**
  (a write at that size under the open's lock: exact for every
  description in the process). No FS_APPEND, and no FS_GATHER (writes
  go through as POSIX programs expect of the page cache's readers).
- **A writable file can't be handed to a child** while the parent holds
  it: the child's own open (section 3) is ERR_BUSY, so `fd_give` is
  EBUSY. A read-only file is given (its own open, at the parent's
  offset, M13 Q5). `cmd > file` therefore needs the open done in the
  child (posix_spawn's `addopen`, B3) or a change to fat's rule (several
  writers of one file, or the file protocol's "clone" of Q5's (B)): a
  question for the owner, not decided here.
- **Paths**: up to 4096 bytes with the NUL (the `fs` protocol's), called
  on the mount's channel directly (libos's file calls stop at 256).
  ".." stops at a mount's root as the plan says, so `chdir("/data/..")`
  stays in /data. A trailing slash, "." or ".." at the end means only a
  directory will do (ENOTDIR; EISDIR for O_CREAT). Paths under /dev and
  /etc go to B2's kinds.h first; any other change there is EACCES (/dev)
  or EROFS (/etc).
- **open** refuses unknown flags with EINVAL (openat2's rule; open(2)
  ignores them), access mode 3 too; O_TMPFILE is EOPNOTSUPP, O_PATH
  works for files (stat, fchdir and the *at calls on directories;
  read and write EBADF), O_SYNC and O_DSYNC sync after each write.
  O_CREAT on a read-only mount opens a file that is there (Linux's
  order), O_RDONLY|O_TRUNC truncates (Linux's).
- **Errors in Linux's order** where FAT allows: mkdir and O_CREAT look
  for the name before write access (EEXIST before EROFS); unlink, rmdir
  and rename ask for write access first (EROFS for a missing name on a
  read-only mount). A refusal is EROFS when the mount says it is
  read-only (fs.statfs, remembered a second per mount), else EACCES.
  "/" and /svc are read-only: mkdir there is EROFS, rmdir of a mount
  point EBUSY, rename across mounts EXDEV.
- **stat**: modes as the plan says, but on a read-only mount the write
  bits go (0555 for directories and /boot's programs, 0444 for files:
  the plan's 0444 for everything would make directories unsearchable to
  `test -x`). A program under /boot is a file in a `bin` or `sbin`
  directory. /svc's services stat as sockets (S_IFSOCK 0666). st_dev is
  the mount's place in the namespace (1 on), st_nlink 1, st_blocks from
  the size. statfs: FAT's magic (MSDOS) or ramfs's for the boot image,
  4096-byte blocks, ST_RDONLY from the mount.
- **getdents64** gives "." and ".." first (the services leave them out);
  d_off is a position (2 + the batch's cookie << 16 | the entry in it),
  which seekdir takes back. A listing started again (from ".", or after
  any lseek: rewinddir, seekdir) asks the service afresh.
- **pwrite**: musl 1.2.6's pwrite is pwritev2 with RWF_NOAPPEND, so
  preadv2 and pwritev2 are here: it writes where it is told even with
  O_APPEND (POSIX's rule; Linux's pwrite(2) appends). RWF_DSYNC and
  RWF_SYNC sync; RWF_NOWAIT and RWF_APPEND are EOPNOTSUPP.
- **unlink of an open file** is EBUSY (fat's rule); musl's tmpfile
  ignores its unlink's failure, so the name stays until removed.
- **mmap** of a descriptor (mem.c, A1's file: the call to file.c's
  `file_map` and the source VMO in `map_new`): MAP_PRIVATE is a copy of
  the file, or of a memfd, zero past the end; MAP_SHARED of a memfd is
  its own VMO (B2's `memfd_vmo`); of a file ENODEV until M12.5. EACCES
  for a descriptor not open for reading, ENODEV for a kind with no
  mapping.
- **Found and fixed in A1's code**: a detached thread's stack, unmapped
  by `__unmapself` itself, stayed in libc's table of mappings, so a
  later mapping at that address could not be unmapped (ENOENT);
  `__jam_unmapself_prepare` now takes the base and size and the table
  forgets the range (`mem_forget`). posixtest's thread section checks it.
- **fd_set is fd_place** (B5's note): `fd_set` is `<sys/select.h>`'s type.
- **The 10,000 files** take about 2.5 minutes on QEMU's USB stick (8 ms a
  file made, 7 removed: fat writes each entry through), so the init run's
  files section lists 300 (two readdir batches) and the 10,000 are the
  files-many section, run by `tools/shell-tests/posix-files.txt`
  (`QEMU_TIMEOUT=600`). posixtest's list asks for /data (rw) and /boot
  (r) so the shell gives them.
- **The errno table** (posixtest files.c) has each row's man page and
  error; it was written from the man pages and POSIX, not run on Linux.
- **Not done**: `prlimit`/`getrlimit` (RLIMIT_NOFILE's 1024 is FD_MAX but
  no call answers it yet), `statx` (ENOSYS: musl falls back to fstatat),
  listing /dev and /etc (B2's), delete-on-close for an open file
  unlinked.

**B1's question, answered (2026-10-09, by Q5's recommendation):** fat
keeps one writable open per file, and a descriptor open for writing isn't
handed to a child (EBUSY). Redirections are opened by the child itself:
S1 and the shell use `posix_spawn_file_actions_addopen` for `>`, `>>` and
`2>`, never a parent's writable descriptor. A file shared for writing
between a parent and a child waits for Q5's (B), the `file` clone, with
M15's shell.

## S1 notes

Track S1 (the shell gives programs descriptors) as built, with the choices
the plan left open. Files: `user/services/shell/sh_posix.c` (new: the
descriptors and the hand-over), `sh_program.c`, `sh_parse.c`, `sh_exec.c`,
`sh_vars.c`; `pxio` (user/posix/progs/pxio, the test program, also at
`/boot/usr/bin/pxio-usr`); `tools/shell-tests/posix.txt`.

- **Which programs.** One whose list has `posix` (`struct wants.posix`).
  Everything else is started as before; only these get an 8 MiB stack
  (`stack_size`), the POSIX environment, and the hand-over.
- **The hand-over, always, in the foreground.** The plan's "SR_CONSOLE is
  enough" would have worked for the terminal, but a hand-over is the only
  way to give the program the shell's current directory (libc takes its
  cwd from it), its parent's pid and a umask, so every foreground POSIX
  program gets one: the shell makes the PROGRAM client (as for any
  program, and still passes it as SR_CONSOLE), calls `console.open_tty` on
  it itself, and hands the result over as descriptors 0 to 2. The three
  are one terminal description (see POSIXFDS_DUP below), as libc's own
  defaults make them. A program in the background (`prog &`) gets no
  hand-over and no terminal: libc's defaults on its SR_STDOUT pipe, as
  native ones, plus the stack and the environment.
- **Redirections** (`sh_parse.c`, `sh_posix.c`): `<`, `>`, `>>`, `N<`,
  `N>`, `N>>`, `N>&M`, `N<&M` for N and M one of 0 to 2, applied left to
  right (`2>&1 >f` leaves stderr on the terminal), the file name expanded
  as any word. An unquoted `<` or `>` ends a word (`a>b` is `a >b`); quoted
  ones are arguments. `&` after `<` or `>` is not a background `&`. Only a
  foreground POSIX program takes them: a shell command (other than `run`),
  a native program, an alias or a program with `&` is refused, before
  anything runs, with status 2. A redirection alone (`> f`) does nothing.
  An error opening a file says `sh: <path>: <why>` and the status is 1,
  with the program not started (a `>` file already made empty stays so, as
  in any shell).
- **Files are opened by the program, in effect** (B1's answer). The shell
  `file_open`s the file and `file_give`s the open (its file channel and
  buffer) into the hand-over as a `POSIXFDS_FILE` record carrying the path;
  the shell keeps no handle, so the program holds the file's only open and
  fat's one-writer rule is never met. No `posix_spawn` is involved (the
  shell is a native program); the hand-over is the equivalent. `<` is
  `FS_READ`, `>` `FS_WRITE|FS_CREATE|FS_TRUNCATE` (status O_WRONLY), `>>`
  `FS_WRITE|FS_CREATE` (O_WRONLY|O_APPEND, which the layer implements).
- **POSIXFDS_DUP (a new hand-over kind, 12).** `> f 2>&1` needs two
  descriptors on one open file, and the hand-over could only send two
  descriptions: the file protocol's buffer VMO has no `RIGHT_DUPLICATE`
  (fat gives it `READ|WRITE|MAP|TRANSFER`), so the shell could not even
  send a second copy of the open, and fat refuses a second write open. A
  DUP record has no handles and no status; its data is a u32, the number
  of an earlier descriptor of the same hand-over, and it is the same open
  file description (one offset, one set of flags), as `dup2` makes it. The
  reader refuses one that names a descriptor not sent before it, itself,
  a bad length, a status or an offset (utest `posixfds_dup`); the child's
  adopter (`dup_adopt`, fd.c) takes a reference to the earlier
  description. posixtest's fds section sends one. B3's table copy still
  turns a description in two slots into two (its note above); it could
  use DUP now.
- **Pipes.** A POSIX program in a pipeline gets its stdout as a
  `POSIXFDS_PIPE` (the shell keeps the read end and copies it into the
  next stage's buffer as for native ones) and its stdin from the previous
  stage's buffer: all of it written into a channel before the program
  starts (a pipe holds at most 4 MiB here), the shell's end closed, so end
  of file comes after the last byte. Stages still run one after the other
  (S2 makes them concurrent). Stderr stays on the terminal unless `2>&1`.
- **Environment** (`sh_make_env_posix`): the exported variables, and where
  the shell exports none of them `HOME=/`, `PATH`, `TERM=vt100` (T2's
  name), `TZ` (the system's zone), `USER=jam`, `LOGNAME=jam` and
  `SHELL=/boot/bin/shell`; `PWD` is always the shell's current directory.
  A native program's environment is unchanged. A `TERM` the shell
  exports (a session's shell can, for X1) wins over `vt100`.
- **PATH.** The shell's default `PATH` is now `/boot/bin:/boot/usr/bin`
  (it was `/boot/bin`, used by nothing). A bare name that is not a shell
  command is looked for in each directory of `$PATH` in order (unset:
  the default; a prefix assignment `PATH=... cmd` works); a program found
  in `/boot/bin` is started as before (by its bare name), one in another
  directory by its path. A name with a `/` is still not a command (use
  `run`); `run` does not search `PATH`; `type` and Tab know `/boot/bin`
  only (not changed: not S1's files).
- **THE HOOK for B4 (SIGINT).** Today a Ctrl+C reaches the shell as a key
  while a POSIX program holds its terminal (the line discipline sends it
  to the newest shell key channel below the tty), and `wait_program`
  (sh_program.c) calls `interrupt_program(s)`, which says `^C: killing`
  and kills the job. That function is the one place to change: for
  `s->posix` write SIGINT on the program's signal channel instead (make
  the SR_SIGNALS channel in `start_program` beside `program_handles`'
  extras, keep its client end in `struct started`), kill the job only if
  the program has not ended 2 s later (S2), and print nothing of its own.
  The `^C` is currently printed twice, "^C^C" then the killing line:
  `interrupt()` in sh_io.c (S2's file) prints one and the discipline's
  ECHOCTL the other; it must go quiet while a POSIX program holds the
  terminal.
- **Edits outside S1's files**, small: `sh_core.h` and `sh.h`
  (declarations), `user/include/posixfds.h`, `user/lib/fdmsg.c` and
  `user/posix/sys/fd.c` (POSIXFDS_DUP), `user/tests/utest/` (posixfds.c,
  posixfds_bad.c, main.c, utest.h: the DUP checks), `user/posix/test/fds.c`
  (a DUP in the hand-over), `user/posix/posix.mk` (pxio).
- **Limits.** A program with a redirected writable file that starts
  children (`system`, `popen`, `posix_spawn` without `FD_CLOEXEC` on it)
  gets `EBUSY` from `posix_spawn`: B1's `fd_give` of a writable file is
  refused (fat's rule; Q5's (B) is the fix). The terminal's settings
  outlast a program (T2's note); the shell does not reset them. A
  redirection to a file whose `FS_TRUNCATE` open succeeded but whose
  other redirection then failed leaves the file empty.

## B4 notes

Track B4 (signals) as built, with the choices the plan left open. Files:
`user/posix/sys/sig.h` (the model and what the layer shares),
`user/posix/sys/signal.c` (dispositions, masks, pending sets, routing,
delivery, the call boundary), `user/posix/sys/sigcalls.c` (the calls),
`user/posix/sys/sigthread.c` (the signal thread),
`user/posix/sys/sigtimer.c` (setitimer and the POSIX timers),
`abi/idl/signal.idl`, the overrides
`user/posix/musl/src/thread/x86_64/syscall_cp.c` (C now: the assembly's
address window was only for musl's cancel handler) and
`user/posix/musl/src/thread/pthread_cancel.c`; posixtest's signal.c and
sigproc.c. nosig.c is gone.

- **Protocol `signal`, id 44** (the plan's 93 is devmgr's block). One
  event, `send (u8 signo, u8 code, i32 value)`: the plan's shape plus
  `code` (`CODE_USER` for kill, `CODE_QUEUE` for sigqueue), so a handler's
  `si_code` is right. The receiver drops a number out of range, SIGKILL
  and an unknown code. X1 takes ids from 45.
- **The boundary** is `sig_call` (signal.c), which calls.c's
  `__jam_syscall` and syscall_cp's `__jam_syscall_cp` run every call
  through: the handlers due run before the call and after it. calls.c's
  `kind_of` names the exceptions: memory calls, futex wakes and requeues,
  `exit` and `exit_group` see no delivery; poll, select, epoll_wait, the
  sleeps, sigsuspend, sigtimedwait, pause and a timed futex wait are never
  restarted (signal(7)'s list; musl recomputes a timed futex's time
  itself); the write family, and sendto/sendmsg without `MSG_NOSIGNAL`,
  raise SIGPIPE on `EPIPE`. Every other call is restarted after an EINTR
  if every handler that ran had `SA_RESTART`.
- **An EINTR with no handler run is restarted, always.** It is an
  interrupt left latched (a signal delivered at a boundary before its
  sender's interrupt was consumed), which ends the next wait at once, so
  the restart costs nothing; and a bare `thread_interrupt` with no signal
  behind it never shows as EINTR (posixtest's poll section checks both:
  B5's check that a bare interrupt ends a poll became a real signal).
  1,000 in a row give up with EINTR.
- **Routing.** A signal sent to the process is given at once to one
  thread that doesn't block it (the main thread first, one without it
  pending already first), not left in the process's set for whoever
  passes a boundary first: a thread asleep in a timed wait is never woken
  for a signal another thread then takes (its restart would wait the whole
  time again). With no such thread it stays in the process's set; a thread
  that unblocks it takes it at its next boundary. A thread that blocks a
  signal routed to it, or ends, gives it back to be routed again.
- **Interrupting** only a thread inside a call (`calls` > 0, a Dekker pair
  with the pending bit, both sequentially consistent: sig.h): a thread in
  user code looks at its next call anyway, and fewer interrupts mean fewer
  latched leftovers. The sender never interrupts itself.
- **Default actions** happen in the sender (often the signal thread):
  terminate is `process_exit(LX_EXIT_SIGNAL + N)` (B3's code), at once,
  unless every thread blocks it (then pending, and done at the boundary of
  the thread that unblocks it). A thread waiting in sigtimedwait counts as
  blocking for that and for "ignored" (Linux's real_blocked). No job
  control: SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU and SIGCONT are ignored;
  SIGKILL to a child is `process_kill`, to ourselves the exit.
- **Queues.** A standard signal pending twice is one (its first details
  kept); real-time ones (32 to 64) queue, the first in its set's slot and
  the rest on one queue of 1,024 for the process (`sigqueue` past it:
  EAGAIN to ourselves; between processes the receiver drops it).
- **Thread records** are allocated by each thread (`calloc`) and listed;
  a thread finds its own through a thread-local pointer and, when that is
  gone, by its thread pointer (%fs:0) in the list: musl's SIGEV_THREAD
  notification thread resets its thread-local storage after each
  notification (`__reset_tls`). A new thread starts with its creator's
  mask, as on Linux (thread.c passes it through clone).
- **The signal thread** is libjam's `thread_spawn` on a static 64 KiB
  stack, with no thread-local storage: it never calls through musl, and the
  boundary knows it by its stack (`sig_on_signal_thread`); everything that
  reads thread-local storage is behind that test and a function the
  compiler can't move ahead of it (`mine_now`, `me_ptr`: a first version
  read the record's address early and faulted). It starts as a
  constructor at priority 102, after the descriptor table's 101 (so the
  hand-over's terminal is there to listen to), with the hand-over's mask
  on the main thread and its ignored signals; a process that can't make
  the port or the thread exits 127 with a line in the log (it could never
  see a signal or a child's end).
- **Children** (B3's proc.c, changed): every child is bound to the
  signal thread's port, which is the port's only reader; the waits sleep
  on B3's `gen` alone (B3's reader and its wake packet are gone), and a
  WNOHANG wait asks each live child it matches whether it has ended, so an
  end the signal thread hasn't taken yet shows at once. Whoever notices an
  end sends SIGCHLD (`CLD_EXITED` or `CLD_KILLED`, the pid, the code or
  signal); with SIGCHLD `SIG_IGN` or `SA_NOCLDWAIT` the child is reaped as
  it is noticed and no wait sees it (Linux's rule). Each child's signal
  channel is made in spawn.c's `start` (an extra handle, `SR_SIGNALS`) and
  kept by the table (`lx_child_add`'s new argument), closed when the child
  is reaped.
- **kill.** Our pid: ourselves. A pid > 0: that child (one that has ended
  but isn't reaped: 0 and nothing sent, as Linux's zombie). 0 or -pgid:
  our children in the group, then us if it is ours. -1: every child (not
  us). Anything else ESRCH. A send waits up to a second for room on a
  full channel, then EAGAIN. `sigqueue` to another process needs a
  negative `si_code` (EPERM otherwise, Linux's rule). `tkill`/`tgkill` of
  musl's SIGCANCEL (33) only interrupts the thread (`pthread_cancel`).
- **execve** forwards every signal from its channel and its terminal to
  the program it waits for (`lx_exec_child`).
- **The terminal** (T2's tty.h): the signal thread binds the process's
  terminal's `tty` channel when it has one (`sig_tty_attached` from tty.c
  when the first terminal description is made); a resize is SIGWINCH,
  ISIG's keys with no shell below SIGINT and SIGQUIT, all `SI_KERNEL`.
- **ppoll, pselect, epoll_pwait**: the wait's mask through
  `sig_wait_mask`; a wait ended by EINTR runs its handlers with the wait's
  mask, then the old mask is back (`ready_mask_restore` takes the wait's
  result: a one-argument change in poll.c, select.c and epoll.c).
- **sigsuspend and pause** sleep with `sleep_until` until a handler has
  run on the thread (a signal whose action became "ignore" meanwhile
  doesn't end them); sigtimedwait takes its set's signals itself (they are
  unblocked for the wait, but no handler runs) and ends with EINTR for
  another signal's handler.
- **sigaction**: unknown flags are EINVAL (fail closed; Linux ignores
  them); `SA_ONSTACK` is accepted; `sigaltstack` is accepted and says
  disabled (handlers run on the thread's own stack, at a call). A handler
  gets a siginfo_t (signo, code, pid, the one user's uid, value or
  status, a timer's id and overrun) and a zeroed ucontext_t with
  `uc_sigmask` set.
- **Timers**: 32 POSIX timers (EAGAIN past them) on `CLOCK_REALTIME`,
  `CLOCK_MONOTONIC` and `CLOCK_BOOTTIME` (an absolute realtime expiry is
  turned into an uptime one when set); `ITIMER_REAL` only (the CPU-time
  ones and clocks are EINVAL). Each is a kernel timer bound to the signal
  port; a stale packet (the timer re-armed since) is told by the timer's
  signal having been cleared by the arming. Overrun counts expirations
  missed by a late signal thread and those while the signal was still
  pending; `timer_getoverrun` gives the last signal's.
- **pthread_cancel**: the flag and an interrupt; a cancellation point
  checks the flag before its call and after an EINTR (upstream's
  `__syscall_cp_c`). An asynchronous cancel acts at the next cancellation
  point.
- **Found and fixed**: `user/posix/posix.ld` kept `.init_array` but the
  cross compiler puts constructors in `.ctors`, so no constructor of the
  layer's or of any program ever ran (fd.c's start only ran because every
  descriptor call also starts it). posixtest's start section checks a
  constructor ran, after the layer's.
- **Edits outside B4's files**: proc.c and proc.h (the port, the waits,
  `lx_child_add`'s channel, `lx_child_signal`; `lx_sigchld_hook` and
  `lx_child_port` are gone), spawn.c (the channel; the masks from
  `sig_spawn_masks`), thread.c (a thread listed at its start and unlisted
  at its end, its creator's mask), tty.c (`sig_tty_attached`), ready.c,
  pollcore.h, poll.c, select.c, epoll.c (above), calls.c, calls.h,
  glue.h, posix.ld; posixtest's pipe.c (SIGPIPE ignored around its EPIPE
  check: the default would end the test), poll.c (above), tty.c (the
  terminal's signals), main.c, pt.h, start.c; the shell's sh_program.c
  (Ctrl+C is SIGINT to a POSIX program: below), pxio (`pxio sigint`) and
  tools/shell-tests/posix.txt.
- **The shell** (S1's hook, done here at the coordinator's word):
  `start_program` makes each POSIX program's signal channel
  (`SR_SIGNALS`) and keeps its end in `struct started`; Ctrl+C in
  `interrupt_program` writes SIGINT on it and prints nothing (a full
  channel falls back to killing the job). A background program's end is
  closed at once (S2 should keep it in sh_jobs for `kill %n`'s SIGTERM).
  Left for S2: the 2-second kill, `$?` as 128 + N for a code
  `LX_EXIT_SIGNAL` + N (the shell's `ended` says "exited with code 65538"
  today), Ctrl+\ (the key with code point 0x1c) as SIGQUIT, and sh_io.c's
  own `^C` while a POSIX program holds the terminal (S1's note).
- **For X1**: a session's shell is a native program and gets Ctrl+C as
  keys; a POSIX program alone on a pty gets SIGINT and SIGQUIT from the
  `signal` event (the signal thread already listens), and SIGWINCH from
  `conn_ctl.resize`. A session as a child (`waitpid`) needs a signal
  channel only if the session's owner should signal it. SIGHUP on a
  hang-up is X1's to send (a `send` on the session program's channel).
- **For B6**: sendto and sendmsg already raise SIGPIPE on EPIPE unless
  `MSG_NOSIGNAL` (calls.c's `kind_of`); musl's `send` is sendto. A
  socket's blocking waits must return EINTR on ERR_INTERRUPTED (the
  boundary decides restarts); `connect` and a wait with `SO_RCVTIMEO` or
  `SO_SNDTIMEO` are never restarted on Linux: add them to `kind_of`'s
  never-restarted list (connect as a call; the timed ones by the socket's
  option, which the call can't see, so the socket's code returns EINTR
  only when it has no timeout, else EAGAIN as Linux does on a timeout).
- **Found, not fixed**: thread.c keeps `self_tid` in thread-local storage,
  which musl's SIGEV_THREAD notification thread resets after its first
  notification: from then `gettid` gives 0 there, and its handle stays
  open when it ends (`forget_self` finds no entry). The fix is thread.c's
  (find the tid by thread pointer, as signal.c does). And libjam's
  ns.c (`take_locked`'s first wait for the namespace message) gives up
  early on ERR_INTERRUPTED: a signal routed to a thread inside a
  program's first namespace call would leave it without mounts.
- **Size**: signal.c is about 720 lines: one state with one lock, which
  the calls' own steps (its last part) can't reach from another file
  without a header of the state's internals.

## K2 notes

Track K2 (the time page and `VMAR_REPLACE`) as built, with the choices
the plan left open.

**The time page**
- **The call** is `clock_page`, number 137 (the wall clock's group, next
  to `wallclock_get` 134 and `wallclock_set` 135; 136 is K1's
  `sys_public_info`). No handle or right needed; the handle has
  `TIME_PAGE_RIGHTS` (read, map, the basic rights: never write or
  resize), and every call hands out a handle to the one page.
- **The page** is a kept VMO of one page (`VMO_KEEP_PAGES`: no holder can
  decommit or shrink it, and a `VMAR_KEPT_ONLY` mapping of it never
  faults), made at boot by `wallclock_init` and never freed, written by
  the kernel through a mapping of its own. `struct time_page` (48 bytes,
  pinned in abi_check.c): the sequence word, flags, `tsc_hz`, `tsc_boot`,
  `mult`, `shift`, and the wall clock's flags and offset (UTC ns minus
  uptime ns, so UTC is the uptime plus the offset, exactly
  `wallclock_get`'s sum).
- **Exact, not near.** The kernel's uptime is `floor((tsc - tsc_boot) *
  1e9 / tsc_hz)` (tsc.c's `tsc_to_ns`), and a mult/shift product alone
  drifts from it by up to a few µs an hour. So the page also has `tsc_hz`,
  and `time_page_uptime` (an inline function in `<jam/abi.h>`, shared by
  libc and the ktests) takes the mult/shift estimate (shift 64 for any
  TSC from 1 GHz up: at most 2 ns under) and adds back whole nanoseconds
  while the 128-bit remainder allows: the result is `clock_get`'s for the
  same TSC value, to the nanosecond. The plan's "within 1 µs" holds with
  0 ns of difference; the ktest checks it at eight TSC rates and 20,000
  deltas each, from 0 to 2^64 - 1.
- **The sequence word** covers only the wall clock's two fields (the TSC
  fields are fixed at boot). The writer (`time_page_publish`, under the
  wall clock's spinlock) stores the word odd, a release fence, the
  fields, then the word even with a release store; a reader
  (`time_page_begin`, `time_page_retry` in `<jam/abi.h>`) loads it with
  acquire (waiting out an odd word), loads the fields, an acquire fence,
  and loads the word again: equal means not torn. `wallclock_set` and the
  RTC's reading at boot publish.
- **The TSC read** in libc is `lfence; rdtsc`, so a clock read is never
  made before the instructions ahead of it (the kernel's own `rdtsc` in
  `uptime_ns` has no fence; the ktest fences around it too).
- **Invariant TSC: the page is used either way** (a change from the
  plan's "libc falls back to the calls if the TSC is not invariant").
  The kernel's clock is the TSC whether CPUID calls it invariant or not,
  with no other source to switch to, so the page always gives what
  `clock_get` gives and a fallback would buy nothing. QEMU (with KVM on
  the PC) reports `invariant-tsc=0` on every run, so the plan's rule would
  have left the page unused under every test. The page keeps the bit
  (`TIME_PAGE_INVARIANT`, for information) and has another,
  `TIME_PAGE_TSC` ("the TSC fields are the kernel's clock"), set always
  today, which libc checks: a kernel that one day keeps time some other
  way clears it, and libc reads with the calls.
- **libc** (user/posix/sys/time.c) maps the page at the first clock read
  (`clock_page`, then `vmar_map` with `VMAR_READ | VMAR_KEPT_ONLY`), keeps
  it for the process's life, and moves every clock read in the file onto
  it (`clock_gettime` for the monotonic, boot-time and real-time clocks,
  and the sleeps' deadlines). Two threads that map it at once keep one
  mapping (a compare-and-swap; the loser unmaps its own). A process the
  page can't be mapped into (its job can't pay for the page table) reads
  with the calls for good: the same values, slower. The call is made in
  time.c, not kposix.c: it is the clocks' alone.

**`VMAR_REPLACE`**
- **Bit 5 of `vmar_map`'s flags** (`ASPACE_REPLACE`), with `VMAR_FIXED`
  only and never with `VMAR_KEPT_ONLY` (`ERR_INVALID_ARGS`): a kept
  mapping's fill can fail half way (the job refuses a page table), and by
  then what it replaced would be gone, against the call's rule that an
  error changes nothing.
- **One hold of the region lock**: every check that can fail comes first
  (the mapping cap counted after the replace, the charge, a spare struct
  for a split, the new mapping's reverse-map entry, which checks the
  VMO's size), then the range is cut exactly as an unmap cuts it (entries
  cleared and shot down, mappings trimmed, split or freed) and the new
  mapping goes in. A fault on the range waits on the region lock, so it
  finds the old mapping or the new one; the copier ktest shows a copy
  never fails across 5,000 replaces. aspace_unmap's cut became
  `first_in`, `maps_reserve` and `cut_locked`, shared by both (a separate
  refactor commit, no change in behaviour).
- **The answer is written first**: `sysc_vmar_map` undoes a map whose
  address it can't copy out with an unmap, which can't give a replace
  back what it took. So for `VMAR_REPLACE` the address (the one it was
  given) is copied out before the map, and a bad pointer fails the call
  with nothing changed.
- **libc's `MAP_FIXED`** (mem.c) over the table's mappings is now one
  replace. Holes between them in the range may hold mappings libc didn't
  make (the program, a stack, libjam's): first every hole is mapped with
  the new mapping's own pages as plain fixed maps, which fail (`ENOMEM`,
  the holes filled so far emptied again, nothing else changed) on such a
  mapping, so the replace never takes away what libc doesn't own; then
  the replace covers the whole range and the table forgets the old
  entries. Before, an unmap then a map left a moment with nothing there
  (another thread's read killed the program), and a map that then failed
  over a foreign mapping had already lost the table's pages. `MAP_FIXED`
  where the table has nothing is a plain fixed map, as before.

**Tests**
- ktests (kernel/test/test_timepage.c): `timepage_conversion_exact`,
  `timepage_against_clock_get` (10 s on up to 8 CPUs: never behind the
  `clock_get` before it, never ahead of the one after, and the page's UTC
  is `wallclock_get`'s), `timepage_set_mid_read` (a publish between a
  reader's begin and retry makes it read again; the live page's word
  moves by two on a `wallclock_set`), `timepage_no_torn_reads` (a writer
  publishing two pairs as fast as it can against readers on the other
  CPUs: no mixed pair). kernel/test/test_aspace_replace.c:
  `aspace_replace_shapes`, `aspace_replace_under_faults`.
- utest `time_clock_page`: the handle's rights, writes, decommit, resize
  and a writable mapping refused, a kept mapping that reads the uptime and
  wallclock_get's offset, a bad pointer.
- posixtest: a clock section (8 threads for a second between two
  `clock_get` calls, `CLOCK_REALTIME` against `wallclock_get`, getres,
  nanosleep, time(), and the bench lines in BENCH.md), and in the mem
  section `MAP_FIXED` 2,000 times over four pages a thread reads without
  pause, and a `MAP_FIXED` over a foreign mapping refused with nothing
  changed.

## X1 notes

Track X1 (sessions and pseudo-terminals) as built, with the choices the
plan left open (the safest each time).

- **Ids.** `session` is **45** and `session_ctl` **46** (`abi/idl/session.idl`):
  the plan's 91 and 92 are devmgr's.
- **The protocol.** `session.open (u16 cols, u16 rows, str[<=63] term,
  str[<=4095] command) -> (handle master, handle ctl, handle proc)`: the
  plan's, plus `proc`, the shell's process with `RIGHT_WAIT`,
  `RIGHT_INSPECT` and `RIGHT_TRANSFER` only (never `RIGHT_WRITE`, which
  would let the opener into a process with the shell's authority): its end
  and exit code are the session's, which is how libc reports a session to
  `waitpid`. `term` may hold only `[A-Za-z0-9._+-]`, `command` no byte below
  0x20 nor 0x7f (`ERR_INVALID_ARGS`); a command longer than the shell's
  240-byte line is `ERR_OUT_OF_RANGE` (nothing cut). `session_ctl`:
  `resize`, `hangup`, `signal (u8 signo)` (kill(2)'s way: SIGHUP, SIGTERM,
  SIGKILL hang up; 0 checks; any other `ERR_NOT_SUPPORTED`), and the event
  `ended (i32 code)` (-1 when the shell was killed).
- **init** (`user/services/init/sessions.c`, and small hooks: `sessions_init`
  in services.c's `services_init`, `sessions_event`/`sessions_due` in
  shell.c's loop, `sessions_tell` from `tell_mounts`, `terms_shell_powers`
  split out of terms.c's `start_shell`, a refactor commit). A session is
  bin/console "conn" "endidle" (process name `session-console`, in a job of
  its own, the root with `RIGHT_ROOT_KLOG` alone) and bin/shell "session"
  [`TERM=`] [`cmd=`] (`session-shell`, a job of its own, a SHELL client of
  that console, every terminal shell's powers and init's whole namespace,
  kept in step as the terminals' shells' is). init closes its ADMIN channel
  once the shell has its client. Not supervised: a session that ends is
  over. Openers: a channel each through `svc.connect`, at most 32; at most
  8 sessions whose shell runs and nobody has hung up, and as many more
  ending (a 17th slot takes the oldest ending one's place, killing what is
  left of it). An opener or a control channel that leaves more answers
  unread than the protocol's bound is cut off (its channel closed; the
  session goes on): init's loop never waits for a client. A resize is
  answered at once and passed on with `conn_ctl.resize_send`, one in
  flight, the newest size winning.
- **The end.** Closing the master (or `hangup`, or `signal` 1/9/15, which
  kill the console) hangs up: the console ends, the shell (its console
  gone) ends with 2. The shell's end sends `ended` with its code; 2 s later
  (`HUP_GRACE`) init kills the shell's job (what it left in the background;
  the POSIX programs on the terminal have had the time to act on SIGHUP).
  The console is not killed after a normal end: with **"endidle"** (a new
  argument of the connection back end, conn.c) it ends by itself once a
  shell or a program (a client below ADMIN, or a tty) has been on it and
  none is left and its output has gone out, so `ssh pc 'cat big'` loses
  nothing to a slow reader: the master gives every byte, then its peer
  closes. Without "endidle" a connection console behaves as before.
- **The shell** (edits outside X1's files, small): "session" (`exit [code]`
  ends it with that code: `exit` takes an optional code now, 0 to 255; no
  `initctl.shell_ready`), "TERM=<name>" (exported), "cmd=<line>" (that line
  alone, no banner or prompt, then the end with `$?`).
- **SIGHUP on hang-up** is the process's own terminal hanging up, not a
  `send` by init: the signal thread binds its terminal's `tty` channel for
  `SIG_PEER_CLOSED` (ONCE) beside its events and raises SIGHUP (`SI_KERNEL`)
  when the console lets go of it (a session's or a pseudo-terminal's master
  closed, the terminal window closed, the console gone). init can't reach
  the programs a session's shell runs (the shell holds their signal
  channels), and this way every terminal's programs get it, a
  pseudo-terminal's child too, with no channel to keep. A program whose
  default it is ends as Linux's would (exit as by SIGHUP).
- **Pseudo-terminals** (`user/posix/sys/pty.c`, `pty.h`; dev.c's /dev
  routes there): `/dev/ptmx` starts bin/console "conn" "endidle" in a job
  under the program's (`pty-console`) and gives a master descriptor (a kind
  wrapping pipe.c's socketpair end over the master channel: read, write,
  poll, O_NONBLOCK; a read at the end, and a write after it, is EIO as
  Linux's master's); `/dev/pts/N` (N the slot, 0 to 7, `PTY_MAX`; the 9th
  ptmx is ENOSPC, posix_openpt's EAGAIN) is `console.open_tty` on the ADMIN
  channel libc keeps, so only this process can open it (a child gets the
  slave as a descriptor). The master's TIOCSWINSZ and the slave's are
  `conn_ctl.resize` (SIGWINCH on the slave), TIOCGPTN the slot, TIOCSPTLCK
  accepted (nothing to unlock), grantpt and musl's unmodified openpty
  work. Closing the master kills its console (its programs: SIGHUP). The
  slave is named `/dev/pts/N` (tty.c's description has a name: ttyname,
  stat's major 136); a slave opened with O_NOCTTY never becomes the
  process's terminal (it did before: every first terminal description).
- **The terminal's process-group calls** (tty.c): TIOCSCTTY makes a
  terminal the process's when it has none and leads its session (0 if it
  already is; EPERM else); TIOCGPGRP, TIOCSPGRP (a group of our session:
  EPERM else) and TIOCGSID on the process's terminal only (ENOTTY else).
  The foreground group is libc's bookkeeping on the description, our own
  group until set: the console knows no groups (its focus is the
  foreground). TIOCNOTTY stays ENOTTY.
- **jamos_session_open** (`<jamos/session.h>`): `svc_open(SVC_SESSION)`,
  `session.open`, the master as a descriptor of the master kind (opened
  FD_CLOEXEC: its close is the hang-up), TIOCSWINSZ on it is
  `session_ctl.resize`, and the session a child (proc.c's `lx_session_add`):
  `waitpid` gives the shell's code, SIGCHLD comes, `kill` sends
  `session_ctl.signal` (SIGHUP, SIGTERM, SIGKILL hang up; others accepted
  and nothing done). ENOENT without `/svc/session`, EAGAIN at 8, E2BIG for a
  long command.
- **Who may.** `svc session` was already the build's (services, ports,
  tests: tools/checkwants.py); `allow` now refuses it for a program on
  /data (sh_allow.c). bin/wantsession (user/tests/) asks for it alone, for
  the shell test. posixtest's list doesn't ask for it (it would put `/svc`
  in its namespace from the shell and change posix-run.txt's checks):
  utest's session_posix starts posixtest's session section with it.
- **Tests.** utest (session.c, sessbad.c, sessrig.h; skipped where init
  has no /svc/session, as in the regression run): an interactive session
  (prompt, echo, TERM and a resize as a POSIX program on it sees them,
  `exit 7`), commands (output, no banner, the status), hang-ups three ways
  and SIGHUP reaching a POSIX program's handler, the limit of 8, posixtest's
  session section, bad arguments, junk on the master, the control channel
  and an opener's channel, the control channel closed first, answers left
  unread, 1,000 connects and 1,000 opens closed at once, a namespace
  without /svc/session. posixtest's pty section (in the regression run
  too). tools/shell-tests/sessions.txt runs them from the shell, with
  `allow` refusing wantsession on /data.
- **For X2.** Use `jamos_session_open` for both channel_forkpty and
  channel_fork (no pty without a request: `command` is the exec request's
  line, TERM from the pty request or ""); the master is the channel's
  descriptor (poll it; EIO is the end), `TIOCSWINSZ` on it for
  window-change, `waitpid` on the pid for exit-status (SIGCHLD comes; 2
  means the terminal hung up), `close(master)` or `kill(pid, SIGHUP)` to
  end it. The shell's own line is at most 240 bytes. sshd's list needs
  `svc session` (a port: allowed by the build). The output's LF arrives as
  CR LF whatever the request (the console's rule, as a pty's ONLCR).
- **Found, not changed.** The console frees a tty whose channel closed
  without first reading what is left on its data channel (console tty.c's
  `tty_event` → `ttys_hang_up`): a program that writes and exits at once
  can lose its last output if the console sees the close first. The pty
  test waits for the output before closing the slave for that reason.
