# FORK plan: `fork`, `vfork` and `exec` for Chromium's launcher

The owner (2026-10-16): "Build the fork/vfork emulation layer in libc
first and use it for Chromium's launcher instead of patching it." This
plan replaces [CR-PLAN section 5](CR-PLAN.md#5-launching-other-programs)'s
option (A), a `posix_spawn` branch in Chromium's
`base/process/launch_posix.cc`: there is no launcher patch. Chromium's
`base::LaunchProcess` and `GetAppOutput` run as written: `fork()`, the
child's set-up, `execvp` of `/proc/self/exe`. It is
[ROADMAP](ROADMAP.md)'s M13.5 `fork` and `vfork`, brought forward to
before Chromium's multi-process milestone (CR-c3, track CB5); M13.5's
shell (mksh) stays after Chromium.

**In one paragraph.** libc makes a forked child as a new process of the
same program, whose address space the kernel copies mapping by mapping,
copy-on-write, on F1's child VMOs (one new call, `vmar_clone`). The child's
one thread starts in a libc trampoline that puts back the forking thread's
registers and returns 0 from `fork`. libc hands the child new handles for
what may be shared (descriptors, the namespace, the job, the self-image)
and nothing else. `execve` replaces the process's image in place (a second
new call, `process_replace`), so a forked child that execs keeps its pid,
its parent's handle and its signal channel: Chromium's `kill`, `waitpid`
and `/proc/<pid>/stat` of a renderer reach the renderer. `vfork` is `fork`
with the parent waiting until the child execs or exits.

## What this plan is not

- **Not `fork` in the kernel.** The kernel gains two mechanisms: a copy
  of an address space's mappings into a process that hasn't started
  (`vmar_clone`) and a process taking a new image in place
  (`process_replace`). Which handles a child holds, its descriptors,
  signals, ids, groups and children stay libc's
  ([ARCHITECTURE](../ARCHITECTURE.md#goals)'s "no `fork` in the kernel":
  Q1).
- **No zygote.** `--no-zygote` stays. With `fork` a zygote becomes
  possible (fork without exec); nothing plans it: it belongs with Jam OS's
  sandbox (CR-f).
- **No shared file offsets yet** (M13's Q5, option B): Chromium's
  children never use a file they inherit. Track FQ5 does them with M13.5's
  shell.
- **No overcommit**: a fork charges its child's memory up front
  (section 1.3), as the owner set for M13.5.
- **No process that shares memory** (`clone` with `CLONE_VM` but not
  `CLONE_THREAD`) and no namespace flags (`CLONE_NEW*`: `EINVAL`).

## Where it starts

| Piece | Built by | What fork does with it |
|---|---|---|
| Copy-on-write child VMOs (`vmo_create_child`): flat sharing, each table pays for every page in it, charged at creation | F1a ([M13-PLAN: F1a design](M13-PLAN.md#f1a-design)) | every writable VMO the parent maps becomes a child for the new process |
| VMOs to 128 TiB, the mapping tree, the reverse map by page range | CK1, CK2, CK3 | a 1 TiB reservation and thousands of mappings cloned |
| A child of a sparse VMO skips missing leaves | CR1 finding 5 ([CR-REVIEW-KERNEL](CR-REVIEW-KERNEL.md)) | V8's cage forked in milliseconds |
| One VMO per reservation in libc | CL1 | |
| The self-image (`SR_SELF_IMAGE`), `/proc/self/exe` | CL2 | the child's exec of `/proc/self/exe` runs the same pages |
| argv and the environment in a VMO; `addclosefrom_np` | CS1 | exec's arguments of any size |
| `posix_spawn`, the hand-over (`<posixfds.h>`), the table of children, signal channels | M13's B3 and B4 | the forked child's descriptors and signal channel, the parent's table |
| Each descriptor kind's give (what a child gets) | B1, B2, CU1 | what crosses at fork (section 1.6) |
| `/proc/<pid>/stat`, `statm`, `status`, `maps` for a child (`process_get_maps`) | PRC ([CR-PLAN: PRC notes](CR-PLAN.md#prc-notes)) | Chromium's reads of a renderer |
| A job per started program, 16 MiB of messages | MB | the forked child's job |
| JIT VMOs, `right jit`, the review | CJ1, CJ2, CJR ([CR-REVIEW-JIT](CR-REVIEW-JIT.md)) | section 1.10 |

Today `fork`, `vfork` and `clone` without `CLONE_VM` answer `ENOSYS`
(`user/posix/sys/calls.def`, the vfork override
`user/posix/musl/src/process/x86_64/vfork.c`, the clone override
`user/posix/musl/src/thread/x86_64/clone.c`), and `execve` is spawn, wait,
exit (M13's Q4 A; `user/posix/sys/spawn.c`).

## 1. `fork()`

### 1.1 The address space (FK1, FK2)

libc can't build the child's address space with the calls it has:
`maptable.h` knows only what `mmap` and the break mapped; the program's
segments, its stack and libjam's own mappings were mapped by the starter,
whose VMO handles the process never held. And a copy made one VMO at a
time while the parent's other threads run is torn: a thread that writes
VMO A then VMO B can leave the child B's new value with A's old one,
which no Linux program can see. So the kernel copies the whole address
space in one call:

- **`vmar_clone(dst, vmos, children, n, flags)`** (new): `dst` is a
  process the caller made with `process_create` and hasn't started (its
  handle, `RIGHT_WRITE`), its address space still empty (`ERR_BAD_STATE`
  otherwise); the source is
  always the caller's own address space. Every mapping of the source is
  made again in `dst` at the same address, with the same permissions and
  maximum, by its **clone mode**:
  - *copy*: one `SNAPSHOT_AT_LEAST_ON_WRITE` child per source VMO (two
    mappings of one VMO stay two mappings of one child), never
    executable;
  - *share*: the same VMO (the same pages, both ways);
  - *none*: nothing; the range is free in `dst`.

  `vmos` (the caller's handles, up to n) names VMOs whose children the
  caller wants handles to: `children[i]` gets the child made for
  `vmos[i]` (a fresh VMO's rights), so libc's table of mappings in the
  child can decommit and remap as before (section 1.6). A VMO not mapped
  *copy* is `ERR_INVALID_ARGS`.
- **The mode** (FK1) is the mapping's, set at `vmar_map` by two new flags,
  kept by protect, split and merge (neighbours merge only with the same
  mode), and shown in `process_get_maps`' `flags`:
  `VMAR_CLONE_SHARE` (libc's `MAP_SHARED`) and `VMAR_CLONE_NONE`
  (`MADV_DONTFORK`'s meaning: connection buffers, below). Without either:
  *share* when the mapping's maximum has no write (nothing can ever write
  through it, so sharing is the copy, for free: program text, read-only
  data, a file's own pages, the time page, a kept mapping), else *copy*.
  The kernel overrides two cases: a mapping of a JIT VMO whose maximum has
  write is *none* (section 1.10), and a physical or contiguous VMO (device
  memory; no POSIX program maps one) is *share*.
- **One instant.** The clone holds the source's region lock throughout (no
  map, unmap, protect or fault there meanwhile), raises `protecting` on
  each VMO it copies before sharing its first page (from then on every
  other writer of it waits: a write fault, `vmo_write`, a decommit, a
  resize), shares the pages and takes the write bit from their entries
  in batches within CR1's interrupts-off budgets, and does one shootdown
  of them all at the end, before any `protecting` falls: a source thread
  that writes faults and waits for the whole clone, and one still writing
  through a stale entry writes the page the child shares. So no write the
  child misses comes before one it holds, and the child holds the memory
  as it was at one moment, as Linux's fork does (FK2 notes).
- **Refusals**: a pin or a kernel mapping over a page to copy
  (`ERR_BAD_STATE`: drivers don't fork); the charge (1.3); the child's
  mapping records (a page per 16 mappings, charged now). Nothing is made
  on any error.
- **Stale handle values.** The child's handle table is new, but libc's
  memory holds the parent's handle values. A value is `(slot + 1) << 15 |
  generation`; FK2 starts the generations of a cloned process's table at
  a random point, so a value libc forgot to replace is almost always
  `ERR_BAD_HANDLE` in the child instead of some other object of the
  child's.

A `process_create` flag that clones the caller's address space would do
the same with no new number, but has no room for the VMO arrays; the
kernel track may choose it if it finds another way to hand libc its
children's handles.

### 1.2 The calling thread

`process_start` already does what is needed, with a libc trampoline, so
the kernel needs no "start from a register block" call. `fork` saves the
calling thread's callee-saved registers, stack pointer, return address,
FS base, MXCSR and the x87 control word into a block on its own stack (a
`returns_twice` save, as `setjmp`), then clones: the block is in the
child's copy at the same address. The parent starts the child's thread
(`thread_create`, then `process_start`) at the trampoline, a few hundred
bytes below the saved stack pointer, with the startup channel as `arg0`
and the block's address as `arg1`. The trampoline sets the FS base
(`thread_set_fs_base`), runs libjam's and the POSIX layer's child set-up
(1.6) on that stack, then restores the block and returns 0 where `fork`
was called. Caller-saved registers are dead across the call anyway; the
vector registers are a call's to clobber on Jam OS too
([ARCHITECTURE](../ARCHITECTURE.md#the-system-call-abi)). Because the
child has its own memory, the "returns twice" hazard of an emulated
`vfork` ([CR-RESEARCH](CR-RESEARCH.md#2-process-launching)) doesn't arise:
the child changes its copy only.

### 1.3 The charge: no overcommit

The rule is F1a's and needs nothing new: each table pays for every page in
it, shared or not, so a child VMO is charged when it is made for every
page it shares (and its own tables), and no separation afterwards, on
either side, charges anything. The pages "a write would separate" are
therefore paid for at the fork, by the forking process's job, which is
the child's job too (1.9); a job that can't pay gets `ENOMEM` from `fork`
and nothing is made. No RAM is set aside behind a charge (as for every
page): the job tree's limits keep the machine within its memory.

- **Page tables**: a child's faults build its page tables, and a table
  page that can't be paid for kills the faulting process. To keep "no
  surprise kill", the clone also charges the child as many page-table
  pages as the parent has under the copied mappings, and the child's
  tables draw on that before charging again; what it hasn't used goes
  back at exit or `process_replace`. (The kernel track may build the
  tables at the clone instead, as Linux does; lazily is cheaper for a
  child that execs at once, which is every child here.)
- **What it costs Chromium**: the browser's committed pages are charged
  once more while a just-forked child lives (a few milliseconds, until its
  exec drops the copy): a 400 MiB browser needs 400 MiB of room in its job
  per launch in flight. Its launcher thread launches one child at a time.
  F1a measured 140 us per 64 MiB committed (about 1 ms for 400 MiB), and
  about 1.1 us per page either side writes while they share.

### 1.4 Huge sparse reservations

A 1 TiB V8 cage or a PartitionAlloc pool is one VMO (CL1) mapped in
pieces. Its clone is one child of that VMO (copy, or share if libc mapped
it `MAP_SHARED`) and the same pieces in `dst`. Creation walks only the
present leaves (CR1 finding 5's fix: `vmo_big_child_sparse`, 1.5 to 2.5 ms
for a page in every GiB of a TiB), charges only committed pages and the
tables, and the child's mapping count is the parent's (CK2 keeps it flat).
FK2's ktests and posixtest's fork section both fork a process holding a
1 TiB reservation and check the time and the charge.

### 1.5 Threads

Only the calling thread exists in the child, as POSIX says. musl's
`fork` runs the `pthread_atfork` handlers, takes its own locks (malloc's
among them, so `malloc` works in the child of a threaded parent) and calls
`_Fork`, which blocks every signal and makes the system call: our
`lx_fork` runs inside all of that. The POSIX layer and libjam do the same
for their own state: a fixed list of their locks (the descriptor table,
the signal lock, the children's lock, `mem.c`'s, libjam's namespace and
console locks: FL1a and FL1b make the list and each file's comment names
its place in it) is taken in order before the clone and released in the
parent after it; in the child they are made fresh. A lock another thread
held at the clone and that isn't on the list stays held in the child
(the application's own locks: POSIX's rule, async-signal-safe calls only).
The other threads' stacks are copied and never run again. Threads libjam
or the layer started (the signal thread, a module's helper) are not in the
child; each module starts its own again in its child set-up, or at its
next use.

### 1.6 Handles and descriptors

The child holds only what libc hands it, in messages on its startup
channel written before it starts (a new format beside `<posixfds.h>`,
with its line in `abi/idl/ids.txt`). A handle is passed only as a
duplicate, so one **without `RIGHT_DUPLICATE` is not passed** (the owner's
rule), nor one without `RIGHT_TRANSFER` (a message can't carry it). The
child's set-up (the trampoline's) walks the same tables in the same order
the parent did (the parent writes them with the tables' locks still held
from the clone, so both sides see the same entries) and puts each new
handle in place of the parent's value, or removes the entry.

**libjam's** (FL1a): the startup roles by a fixed list:

| Role | The child gets |
|---|---|
| `SR_SELF_PROCESS`, `SR_SELF_VMAR`, `SR_SELF_THREAD` | its own (duplicates of what the parent made) |
| `SR_JOB` | a duplicate (the same job: 1.9) |
| `SR_NS` | the parent's namespace, each entry duplicated (spawn's `NS_ALL`) |
| `SR_STDOUT`, `SR_CONSOLE`, `SR_BOOTFS`, `SR_SELF_IMAGE` | duplicates |
| `SR_SIGNALS` | a new signal channel's server end (1.7) |
| `SR_RESOURCE` | nothing: a program's root has no `RIGHT_TRANSFER`, and posix_spawn gives a child no root either (CL2) |
| the JIT power's own handle (CJR's design question 2, option (b), when built) | a duplicate, kept across an exec of the same program (1.10, Q4) |
| every service role (keep, state, standby, devmgr, audio ...) | nothing |

Every other handle libjam or the layer keeps (a cached `/svc/dns`,
`/svc/unix` or `/svc/exec` channel, a port, a timer) is dropped in the
child and made again at its next use. FL2 lists each one in the file
that holds it, and the tests count the child's handles (the handle
charge of a job that holds only the forking program and its child)
against what was passed: none left over.

**Descriptors** (FL2): every descriptor, close-on-exec ones too (the
child holds them until its exec: Chromium's child `dup2`s close-on-exec
ones into place). Each kind gets a fork step beside its give
(`user/posix/sys/fd.h`): the parent's half makes the handles, the
child's half puts them into its copy of the description. The rule for
every kind: libc-side state that holds data (a pipe's leftover bytes, a
socketpair's descriptors not yet read, a file's buffered bytes) stays
with the parent, and the child's copy starts empty, so no byte is read
twice.

| Kind | At fork the child gets |
|---|---|
| pipe end, socketpair end | a duplicate of the channel end (`pipe_pass`'s way); both read and write the one channel, as on Linux |
| memfd, `shm_open` | a duplicate of the VMO, the description's rights; its seals are the VMO's |
| eventfd | as spawn gives it: a copy (the counter at the fork, an event of its own; the counter is in the process's memory, so one counter for both would need it in shared memory, which a later change of the hand-over's eventfd kind can bring if a program needs it) |
| `/dev` nodes, libc's `/etc` files | the same description (no handle) |
| a file or a directory on a mount | its first use in the child makes the child's own open of the path at the description's offset (spawn's give, done late, so a fork costs no file server a call); a file open for writing gets `EBUSY` then, as spawn's give does today, until FQ5's `file` clone shares one open and one offset (M13's Q5 B) |
| a terminal, a pseudo-terminal | as spawn gives it |
| a netstack socket, an `epoll` set, any kind whose give moves its handle | not passed: the descriptor is absent in the child (the socket's ring VMO can't be duplicated; an `epoll` set is a port, which two processes can't share meaningfully). A program that relies on one (a pre-forking server's listening socket) needs netstack's help later |

Not passing a descriptor is a deviation from Linux, so the child's
`/proc/self/fd` lists only what it got; Chromium's child closes what it
doesn't keep and `dup2`s what it does, and a kept descriptor that wasn't
passed would make its `dup2` fail and the child `_exit(127)` with
Chromium's own message, visibly.

**The table of mappings** (`maptable.h`, FL2): *copy* entries get the
children's handles from `vmar_clone`'s `children`; *share* entries (a
`MAP_SHARED` memfd, a file's own pages) a duplicate; *none* entries are
removed. libc maps with `VMAR_CLONE_SHARE` for `MAP_SHARED`, and libjam
and the layer map their per-connection buffers (a file's transfer buffer,
a socket's ring, an audio stream's ring, a Wayland pool's client side)
with `VMAR_CLONE_NONE`: the child makes its own when it uses the
descriptor, and the server's pages are never write-protected by someone
else's fork.

### 1.7 Signals

In the child: the dispositions and the mask are the parent's (they are in
the copied memory); nothing is pending; the timers, `alarm` and the
interval timers are gone (POSIX); the list of threads is the calling
thread alone, with its new tid and handle. The child gets a new signal
channel: the parent makes the pair, keeps the client end in its table of
children (as `posix_spawn` does) and passes the server end as
`SR_SIGNALS`; the child's set-up starts its signal thread on a new port
(its stack is static, so the copy's is free). The parent's `kill` reaches
the child as a spawned child's; `SIGKILL` is `process_kill` on the handle
the parent holds.

**From the child to its parent** (FL4): the signal channel carries a
few notices the other way: "I have exec'd" (vfork's release, section 3),
"my process group is now G" and "I lead session S" (Chromium's
`new_process_group` calls `setpgid(0, 0)` in the child only; libc's
groups are bookkeeping, so the parent's table must hear of it for
`kill(-pgid)`). The parent's signal thread reads them from each child's
end, at most a few per wake, and ignores anything malformed: a child that
lies about its group only changes which of the parent's group signals
reach it.

### 1.8 Ids, waits, `kill`, `/proc/<pid>`

The child's pid is its kernel pid (`process_get_info`), its parent's pid
the forking process's, its group and session the parent's. The parent puts
the child in its table of children (`lx_child_add`, as posix_spawn does):
`waitpid`, `WNOHANG`, `SIGCHLD`, `kill` and `kill(-pgid)` work as for a
spawned child, and PRC's `/proc/<pid>/stat`, `statm`, `status` and `maps`
read the child through that handle. With exec in place (section 2) the
same process, the same handle and the same signal channel carry on
through the child's `execve`: nothing in the parent changes. The child's
own table of children is empty (its copy is cleared, not closed: those
values were the parent's).

### 1.9 The job

The child goes in the forking process's job (`SR_JOB`), as posix_spawn's
children do: the shell's kill of a program's job takes everything it
forked, and the fork's charge is that job's. MB's per-program job gives a
program 16 MiB of queued messages, so a multi-process Chromium shares one
budget across all its processes (Mojo's socketpairs are
`CHANNEL_BYTE_BOUND` at 1 MiB each); CB5 measures it. If it is short, the
fix is MB's (a larger limit for a program whose list asks), not fork's.
MB's decision that CB5's launcher would give renderers job limits of
their own has no launcher to live in now: renderers share the browser's
job until Jam OS's sandbox (CR-f) puts each in a job of its own (a
sandbox needs Chromium's own code then anyway). `PR_SET_PDEATHSIG` stays
accepted (CS1).

### 1.10 JIT memory, the power, W^X

- **JIT memory in a forking parent.** The kernel never makes a child of a
  JIT VMO (`vmo_create_child`: `ERR_NOT_SUPPORTED`; CR1's hostile test
  `vmo_jit_hostile_spawn`: "no fork-like snapshot of JIT memory"). A
  mapping of one whose maximum has write is *none*: the child has nothing
  there (a touch is `SIGSEGV`), no handle to it, and isn't marked
  `[jit]`. Sharing it instead would let a process that may not hold the
  power write code its parent runs, the class of CJR's finding 1. A
  mapping whose maximum has no write (since CJR's fix, a program spawned
  from a JIT VMO maps its code through a handle that can't write) is
  *share* with that same maximum: the child runs those pages and can never
  make them writable, which keeps finding 1's property; `ps` marks it as
  it marks any process mapping JIT memory (by finding 3's rule, as CJF
  settles it). libc removes `MT_JIT` entries
  from the child's table. A program that forks and goes on running JIT
  code in the child makes new JIT memory there (V8 in a forked child that
  doesn't exec isn't supported).
- **The power.** The root isn't passed (1.6). What a forked child gets of
  the JIT power, and how the browser keeps it for renderers only with an
  unpatched launcher, is Q4: CJR's design question 2 recommends a second
  handle holding the power alone (a root narrowed to `RIGHT_ROOT_JIT` with
  `RIGHT_TRANSFER` and `RIGHT_DUPLICATE`, under a startup role of its
  own, which `MAP_JIT` looks for). Of CJR's three options that is the one
  that fits fork: (a), transfer on the whole root, would let libc pass
  every power the program holds to every forked child; (c), every child
  through `/svc/exec`, is a way fork doesn't take (a forked child is no
  new approval, and its exec of `/proc/self/exe` runs the self-image).
  With (b)'s handle passed at fork like the other duplicates (it has
  duplicate and transfer), the unpatched launcher still can't choose
  which child gets it, so this plan recommends every forked child keeping
  it, across its exec of the same program, and Chromium's non-renderer
  processes closing it as they start (Q4). An exec of any other program
  doesn't pass it, as posix_spawn passes no power today.
- **W^X.** Every mapping keeps its permissions and maximum; *copy*
  mappings are never executable (a child VMO never has `RIGHT_EXEC`); the
  only mapping that could be writable and executable, a JIT VMO's with
  both, is never cloned. The child holds no W+X memory at the fork.

### 1.11 The three ways in

| Call | Who | Where the child goes on |
|---|---|---|
| `fork()` | musl's `fork` and `_Fork` (unmodified) make `SYS_fork` | returns 0 from the call |
| `syscall(SYS_clone, SIGCHLD, 0, ...)` without `CLONE_VM` | base's `ForkWithFlags` | returns 0 from the call; `CLONE_PARENT_SETTID`, `CLONE_CHILD_SETTID` and `CLONE_CHILD_CLEARTID` honoured, other flags `EINVAL` |
| `clone(fn, stack, SIGCHLD, arg)` | gtest's "threadsafe" death tests | calls `fn(arg)` on `stack` and exits with its value (the override in clone.c gains this branch) |

A raw `clone` skips musl's atfork and lock handling (on Linux too), but
not the layer's own, which runs inside the call.

## 2. `execve` in a forked child

Today `execve` is spawn, wait, exit: the new program is a child of the
caller, which forwards every signal to it and exits with its status. For a
forked child that is a third process with another pid. Chromium kills,
waits on and reads `/proc/<pid>/stat` of the pid `fork` returned, so the
renderer must be that process. Three ways:

| | (a) Spawn, wait, forward (today), plus a told pid | (a2) Hand the identity back to the parent, in libc | (b) Replace the image in place (`process_replace`) |
|---|---|---|---|
| How | the forked child P1 spawns the program P2, waits and forwards; P2's hand-over tells it its pid is P1's | P1 spawns P2, then sends the parent's libc P2's process handle and signal channel on its signal channel; the parent swaps them into P1's entry and answers; P1 exits unseen | P1 builds the new program in an unstarted process the usual way (libjam's spawn, all but the start), then asks the kernel to make that its own image: P1's other threads die, its address space and handle table are dropped (all but the startup channel), the new address space becomes P1's, and its calling thread starts at the new entry |
| `kill`, `waitpid` | through P1's forwarding; `SIGKILL` kills P1 only and leaves P2 running (P1 would need a job of its own, then `job_kill`) | reach P2 directly | the same process |
| `/proc/<pid>/stat` | P1's (the waiter): wrong figures | P2's | the program's |
| Memory | P1 keeps its copy of the browser, charged, as long as the renderer lives: a browser's worth per renderer | P1's copy goes when it exits | the copy goes at the replace |
| The kernel's view (`ps`, the shell's `kill`) | two processes | P2's pid isn't the one Chromium knows | one process, one pid |
| Kernel work | none | none | one new call (FK3): about 400 lines and its ktests |
| libc work | a told pid, a job per forked child | a two-way protocol with races (a kill between the spawn and the swap), and the old way kept for a parent that isn't libc (the shell) | build-then-replace in exec; a program from `/data` keeps (a) for now (below) |
| Risk | known, but leaks a browser's memory per renderer | moderate, in libc; identity right only for POSIX parents | kernel: killing a process's other threads and swapping its address space while it runs (Linux's `de_thread` and `exec_mmap`); FR1 reviews it |

**Recommendation: (b)** (Q2). It is the only one where the renderer *is*
the pid Chromium holds, for every observer, and it fixes today's
`execve` for every caller too (`env`, `nohup`, `time` keep their pid, and
a parent's `SIGKILL` no longer misses the exec'd program). It must be in
the ABI before M14 freezes it. The call:

- **`process_replace(proc, entry, stack, arg0, arg1)`** (FK3): `proc` is a
  process the caller made with `process_create` in its own job, not
  started, with no thread (`RIGHT_WRITE`; anything else `ERR_BAD_STATE` or
  `ERR_INVALID_ARGS`). The caller's process kills its other threads and
  waits until they are gone; takes `proc`'s address space and drops its
  own (the fork's copy, and its charges, go here); closes every handle in
  its table but `arg0` (a channel end, moved, as `process_start`'s);
  takes `proc`'s name; and the calling thread goes on at `entry` with
  `stack`, `rdi` = `arg0`'s new value, `rsi` = `arg1`, the FS base 0 and
  a clean FPU state. The process keeps its pid, its kernel object (every
  handle others hold of it, waits and port bindings), its job, its
  creation time and its CPU time. `proc` is consumed. `PROCESS_FLAG_JIT`
  is the new address space's (unmarked).
- **libc's exec** (FL3): the same steps as posix_spawn of that program
  (the program found first, then the hand-over with descriptors without
  `FD_CLOEXEC`, the directory, umask, mask and ignored signals, the
  self-image for `/proc/self/exe`), plus: the startup message's
  `SR_SELF_PROCESS` and `SR_SELF_THREAD` are the caller's own
  (duplicates), `SR_SIGNALS` is the caller's own signal channel's server
  end (moved), and the hand-over says pid = our pid and parent = our
  parent. Then the "I have exec'd" notice to the parent (vfork, 1.7) and
  `process_replace`. Handlers go back to the default, ignored signals
  stay ignored, the mask stays (POSIX). If anything fails before the
  replace, `execve` returns the error and the caller carries on, as
  POSIX says.
- **A program from `/data`** is started by init's `/svc/exec` (the owner's
  rule: init makes the process, the caller never holds its authority), so
  it can't be built by the caller: its `execve` stays (a) for now. init
  building it and handing the caller an unstarted process it may only
  replace itself with (FX, later, with M13.5's shell) would end that.
  Chromium's children are `/proc/self/exe` and never meet this.

## 3. `vfork()`

| | (A) `vfork` is `fork`, and the parent waits until the child execs or exits | (B) The recorder (M13's Q4 B): the child's calls are recorded until `execve`, then one posix_spawn |
|---|---|---|
| Correct for | every program: the child has its own memory | only the calls the recorder knows; any other call changes the parent |
| Cost | a fork's: about 1 ms per 400 MiB committed, the charge for a moment | none |
| Work | a few lines on top of `fork` and FL4's notice | a recorder for `dup2`, `close`, `fcntl`, `sigaction`, `sigprocmask`, `setpgid`, `setrlimit`, `prctl`, `chdir`, `open` and `/proc/self/fd` listings, faithful to each |
| Breaks | a child that writes a variable for its parent to read (undefined by POSIX; some old code reports an exec failure this way): the parent never sees it, sees the child's exit status instead | a child that does anything unrecorded; the "returns twice" compiler hazard |

**Recommendation: (A)** (Q3; the owner's 2026-10-11 roadmap text names
(B), so his to change). The wait keeps `vfork`'s order (the parent runs
again only after the child's exec or exit), which some programs rely on.
musl's `posix_spawn`, `system` and `popen` are ours and never fork, so the
cost lands only on programs that call `vfork` themselves. The vfork
override becomes a call to the fork core with a flag.

## 4. Chromium on it

Chromium's child after `fork` (`base/process/launch_posix.cc`), step by
step, and what serves each:

| Step | Served by |
|---|---|
| clear `FD_CLOEXEC` on the kept descriptors | the child's descriptor table (all passed, 1.6) |
| `/dev/null` as stdin | libc's `/dev` |
| `setpgid(0, 0)` when asked | libc's groups, the notice to the parent (1.7) |
| `getrlimit`, `setrlimit` | libc's rlimit values (not carried across exec today: FL3 adds the soft limits to the hand-over if a test needs them) |
| every handler to `SIG_DFL`, the mask cleared | libc's signals in the child's copy |
| the new environment | `environ`, then exec's envp |
| `dup2`s to the numbers on the command line | the child's table |
| close everything else by listing `/proc/self/fd` | CL2's `/proc/self/fd` over the child's table |
| `PR_SET_NO_NEW_PRIVS`, `PR_SET_PDEATHSIG` | accepted (CL3, CS1) |
| `chdir` | libc's directory, then the hand-over |
| `execvp` of `/proc/self/exe` | exec in place from the self-image (CL2, FL3) |

`GetAppOutput` is the same with a pipe on stdout; its programs (none on
the browser's start path) are found or `_exit(127)`. Crashpad stays off
(it forks, `setsid`s and spawns its handler; CR-PLAN section 5).

**base's tests.** CB2's first run found `fork` `ENOSYS` in
`ForkWithFlagsTest` (raw `clone`), `LoggingTest` (gtest's death tests:
`clone(fn, stack, SIGCHLD)` then an exec of the test program) and
`ProcessGTestOutputTest` (the test launcher's children); with
`ProcessUtilTest`'s `LaunchProcess`, `GetAppOutput`, kill and wait tests
they are FC1's: taken off the recipe's disabled list
(ports/chromium/patches/0007-disable-failing-tests.patch), run by
cr-test.sh's `base` case, PASS.

**What replaces the launcher patch in CB5's row**: nothing in Chromium's
launcher. CB5 runs `--no-zygote --no-sandbox` on Chromium's own fork and
exec, gated on FK1 to FK3 and FL1a to FL4. Q4's answer may add a few lines
in content's child start (closing the JIT power in non-renderers) with
CR-e, not before.

## 5. Tracks

Each track is about one agent-hour where the work can be cut that small;
FK2, FK3 and FL1b are about two (their halves can't be tested apart).

| Track | What | Files | Gate | Tests | Size | Model |
|---|---|---|---|---|---|---|
| **FK1. Clone modes** | `VMAR_CLONE_SHARE`, `VMAR_CLONE_NONE` on `vmar_map`, kept through protect, split, merge and replace; `map_info.flags` shows them; syscalls.def's text for both new calls (the contract FK2 and FK3 fill); built, see "FK1 notes" | `kernel/mm/aspace.c` (the flags), `kernel/include/jam/abi.h`, `abi/syscalls.def`, a new ktest file | nothing | ktests: each flag kept by every mapping operation, merges only with the same mode, refused with `VMAR_KEPT_ONLY` where it means nothing; full ktest 4 and 8 | 1 | **Opus** |
| **FK2. The address-space clone** | `vmar_clone` (1.1): one instant, the three modes, the JIT and device rules, the children's handles, the charges and the page-table reserve (1.3), the refusals, random handle generations in the clone's table; built, see "FK2 notes" | `kernel/mm/aspace.c` (the walk), `kernel/object/vmo.c` (protecting many VMOs, then sharing), `kernel/object/process.c`, `kernel/abi/vmar_sys.c`, `kernel/object/handle.c` (the generations), syscalls.def, abi.h, a new ktest file | FK1 | ktests: each kind of mapping cloned and checked from both sides; 8 writer threads keeping an invariant across two VMOs while the clone runs (the child never sees it broken); a 1 TiB reservation with a page per GiB cloned within a bound; 60,000 mappings; charges exact and refused up front; a pin refused; storms of map, unmap and protect from other threads; full ktest 4 and 8; soak 2 | 2 | **Opus** |
| **FK3. Exec in place** | `process_replace` (section 2) | `kernel/object/process.c`, `kernel/sched/thread.c` (the other threads' end), `kernel/object/handle.c` (all but one closed), `kernel/mm/aspace.c` (the swap), `kernel/abi/sysc_proc.c`, syscalls.def, abi.h, a new ktest file | FK2 (the same files) | ktests: other threads in a wait, a futex, a channel call, a fault waiting for a pager all gone; every handle closed but one; the parent's handle, pid, waits and port bindings unchanged; the old address space's charges back; each refusal; two threads replacing at once; a replace racing a kill; full ktest 4 and 8; soak 2 | 2 | **Opus** |
| **FL1a. libjam's fork** | the save and the trampoline (assembly), `jam_fork` (the clone, the thread, the start, the roles of 1.6), the child's rebind of libos's roles and locks, the hooks the POSIX layer registers; built, see "FL1a notes" | a new user/lib/fork.c and its assembly, `user/lib/start.c` (the roles' rebind), `user/include/os.h`, a new message header in user/include/, `abi/idl/ids.txt`, a utest file | FK2 | utest: a native program forks: both sides return, memory separate, the child's roles its own, its handle table exactly the passed set, the parent's charge back after the child exits | 1 | **Opus** |
| **FL1b. POSIX `fork`** | `lx_fork`, `clone` without `CLONE_VM` (both forms), `vfork` as fork with the parent's wait (Q3's A; the wait itself needs FL4), the layer's locks, signals in the child (1.7), ids and the table of children (1.8), JIT entries removed | a new user/posix/sys/fork.c, `user/posix/sys/calls.def`, the vfork and clone overrides, `user/posix/sys/sigthread.c`, `user/posix/sys/signal.c`, `user/posix/sys/proc.c`, a new posixtest section (fork) | FL1a | posixtest `fork`: returns twice, memory separate (heap, stack, data, `mmap`), `MAP_SHARED` anonymous shared, malloc and atfork in a threaded parent's child, the two-VMO invariant from a threaded parent, a 1 TiB reservation forked within a bound, `ENOMEM` in a job that can't pay, ids, `waitpid`, `SIGCHLD`, `kill`, `SIGKILL`, pending cleared, timers gone, both `clone` forms, `jittest` forking (the child's JIT range faults, it isn't marked) | 2 | **Opus** |
| **FL2. Descriptors and mappings at fork** | each kind's fork step (1.6's table), the table of mappings' handles, `VMAR_CLONE_SHARE` from `mmap`, `VMAR_CLONE_NONE` on every connection buffer, the cached service channels | `user/posix/sys/fd.h` and each kind's file (pipe, sockpair, memfd, eventfd, dev, etc, svcsock, file, dir, tty, pty, socket, epoll), `user/posix/sys/mem.c`, `user/posix/sys/maptable.c`, `user/posix/sys/resolve.c`, `user/posix/sys/unixsock.c`, libjam's buffer mappings, a new posixtest file | FL1b | posixtest `fork` descriptors: every kind used from both sides after a fork, leftovers read once, close-on-exec ones present until exec, absent kinds absent, `/proc/self/fd` exact, Chromium's child sequence (dup2s, close by `/proc/self/fd`) run in a child; no handle left over | 1.5 | **Opus** |
| **FL3. libc's exec in place** | `execve` by build-then-replace (section 2); (a) kept for `/data` programs; rlimits in the hand-over if needed | `user/posix/sys/spawn.c` (execve), `user/lib/spawn.c` (all but the start), `user/include/posixfds.h` if rlimits go in, a new posixtest file | FK3, FL1b | posixtest `exec`: pid, parent's wait, kill and `/proc/<pid>/stat` the same before and after; threads gone; close-on-exec closed; handlers reset, mask kept; the fork's charge back; a failed exec returns; `/proc/self/exe` exec'd from a forked child; utest: a native starter (the shell's way) waits on a POSIX program that execs | 1 | **Opus** |
| **FL4. Notices and `vfork`'s wait** | the child-to-parent notices (exec'd, group, session) on the signal channel, read by the parent's signal thread; `vfork`'s parent waiting for exec or exit | `abi/idl/signal.idl` and its header, `user/posix/sys/sigthread.c`, `user/posix/sys/proc.c`, user/posix/sys/fork.c's wait | FL1b | posixtest: `vfork` then `_exit`, then `execve`, the parent not running until then; a child's `setpgid(0, 0)` seen by the parent's `kill(-pgid)`; junk notices ignored | 1 | **Opus** |
| **FC1. base's fork tests** | section 4's tests off the disabled list, run, PASS; their findings | ports/chromium's test list (0007-disable-failing-tests.patch), cr-test.sh's `base` expectations | FL1a to FL4, CB2 | cr-test.sh `base` | 1 | Sonnet 5.5 |
| **FR1. Kernel review** | FK1 to FK3: findings first ([CR-REVIEW-KERNEL](CR-REVIEW-KERNEL.md)'s way), High and Medium fixed with a test each; hostile tests: a clone storm from 8 threads at the job's limit, replace racing kill and clone | the files its fixes need | FK1 to FK3 merged | its own; full ktest 4 and 8; soak 4 | 1.5 | **Opus** |
| **FR2. libc review** | FL1a to FL4: what a child holds that it shouldn't (every handle and role), what a hostile child can make its parent do (notices), stale values, locks | the files its fixes need | FL1a to FL4 merged | its own; posixtest; utest | 1.5 | **Opus** |
| **FJ. The join** | ARCHITECTURE (Goals' process creation line, Memory's clone, Userland's fork and exec), M13-PLAN's Q4 and Q5 lines, TESTING (the new sections and ktests), ROADMAP, CR-PLAN, HISTORY; this plan deleted when HISTORY covers it | the docs | FR1, FR2 | `make check` | 0.5 | Sonnet 5.5 |
| **FQ5. Shared offsets** (M13.5's shell, not Chromium's path) | the `file` protocol's clone: a second channel onto one open file with one offset and one writable open; fat's, extfs's and bootfs's servers; fork and spawn give a file through it | `abi/idl/file.idl`, the file servers, `user/posix/sys/file.c` | FL2 | posixtest: `{ a; b; } > f` appends; a forked child's writes to an inherited file | 2 | **Opus** |
| **FX. `/svc/exec` replaces** (later, with M13.5) | init builds a `/data` program for a caller's `execve` in an unstarted process and gives the caller a handle to it that allows `process_replace` only (a right of its own, never write), so the caller becomes the program without ever running code with its authority (the owner's rule kept) | init's exec service, `abi/idl/exec.idl`, `user/posix/sys/spawn.c` | FL3 | utest `exec_*`; posixtest | 1.5 | **Opus** |

**Order.** FK1, then FK2 (the kernel's files, one track at a time as
CR's order has it), then FK3 beside FL1a; FL1b after FL1a; FL2 and FL4
after FL1b (different files); FL3 after FK3 and FL1b; FR1 once FK1 to FK3
are merged, FR2 once FL1a to FL4 are; FC1 after FL4 and CB2; CB5 after
FL1a to FL4 (FR1 and FR2 run beside it, as CR1 ran beside the CB tracks).
FJ last. FQ5 and FX with M13.5's shell, after Chromium.

**Against CR's tracks.** CB3 (headless, `--single-process`) and CB4
(`content_shell --single-process` on Wayland) launch no children: they
don't wait for any of this, and run beside it. CB5 (multi-process) is the
first user: its row loses the launcher patch and gains FORK's gates
(above). CJ2's Chromium half (V8's `MAP_JIT`, renderers only) follows
Q4's answer, and builds CJR's option (b) handle (the starters' grant, the
startup role, `MAP_JIT`'s look), which FL1a's list of roles then passes at
fork (a line there, added by CJ2).

**Size**: the Chromium path (FK1 to FK3, FL1a to FL4, FC1, FR1, FR2, FJ)
about 16 agent-hours; FQ5 and FX 3.5 more, later. In lines: about 900 in
the kernel (FK2 500, FK3 400) and 2,000 in libc and libjam, besides tests.

**Risks**
1. *A handle libc forgets* (a module's cached channel used in the child):
   the random generations turn most into `ERR_BAD_HANDLE`; the tests count
   the child's handles against what was passed; FR2 reads every global
   handle.
2. *The clone's one instant* is the kernel's hardest part (many VMOs'
   `protecting` at once, the batches within CR1's budgets): FK2's writer
   test and FR1's storms.
3. *`process_replace` in a live process*: other threads mid-call, the
   address space under a fault. Linux's exec has the same shape; FK3's
   ktests and FR1.
4. *Chromium's browser in a tight job*: a launch charges the browser's
   committed pages again for a moment (1.3). CB5 records the figures; the
   job's limit is the program's to size.
5. *Linux-specific use of `fork` in Chromium* that this plan didn't read
   (the sandbox's `clone` with namespace flags is off; a call the scan
   finds later goes to FL1b's table of flags).

### FK1 notes

What FK2 and FK3 start from:

- **The mode** is `clone` in `struct mapping` (`kernel/mm/aspace.c`):
  `ASPACE_CLONE_SHARE`, `ASPACE_CLONE_NONE` or 0 for a copy, the same
  bits as `VMAR_CLONE_*` (bits 6 and 7; `abi_check.c` holds them equal),
  so `aspace_get_maps` copies it into `map_info.flags` as it is.
  `clone_mode` decides it once in `mapping_new`, from the flags and the
  maximum; `split` copies it, `joinable` (every merge, and the counts
  `merges_for` and `protect_merges` make before a change) compares it, and
  nothing else writes it. `aspace_check` refuses a mode the rules don't
  allow (`clone_ok`): FK2's walk may trust that a copy is of a paged VMO,
  not JIT, through a maximum with write.
- **Decided here**: the clone flags with `VMAR_KEPT_ONLY` are
  `ERR_INVALID_ARGS` (the row's "refused where it means nothing": a kept
  mapping is always shared); a contiguous or physical VMO's mapping keeps
  an explicit `VMAR_CLONE_NONE` (only its copy becomes share: nothing is
  lost by giving a child nothing); `VMAR_CLONE_SHARE` asked on a JIT
  VMO's mapping whose maximum has write is not refused but made none,
  as the plan's "the kernel overrides" says, so `MAP_SHARED` over JIT
  memory keeps working in the parent. `vmar_protect` takes neither flag
  (its mask already refused them). A new `vmo_is_paged` (`vmo.c`) tells
  the device kinds apart.
- **The two calls** are in syscalls.def as 65 `vmar_clone(dst, vmos,
  children, n, flags)` and 139 `process_replace(proc, entry, stack, arg0,
  arg1)`, with the contract text; their kernel halves
  (`sysc_vmar_clone` in `kernel/abi/sysc_vm.c`, `sysc_process_replace` in
  `kernel/abi/sysc_proc.c`) answer `ERR_NOT_SUPPORTED` and touch nothing
  until FK2 and FK3 replace them. (syscalls.def's header said a weak
  default was generated; none has been since the generator dropped its
  file of weak calls, so the header now says so.) Left to FK2: the limit
  on `n` and the rights `vmos` need; to FK3: whether `arg0` leaves the
  table on an error (the text says nothing changes, the stub's behaviour).

### FK2 notes

What FK3 and FL1a start from (`aspace_clone` in `kernel/mm/aspace.c`, its
"Clones"; `vmo_clone_*` in `kernel/object/vmo.c`; ktests `vmar_clone_*`):

- **`dst` is a process handle** (`RIGHT_WRITE`), not a vmar: only the
  process can say it hasn't started, and `process_clone_begin` holds its
  `setup` mutex from that check to the end of the copy, so
  `process_start` waits for it. On success its handle table starts every
  slot at a random generation (`handle_table_randomize`).
- **`vmos`** need `RIGHT_READ | RIGHT_DUPLICATE` (`vmo_create_child`'s
  rule: a child is a new way to reach the bytes); `n` is at most
  `VMAR_CLONE_MAX` (= `VMAR_MAX_MAPPINGS`: each must be mapped as a
  copy); duplicates get the same child. The children's slots are reserved
  and their values written to `children[]` before the copy, so nothing
  fails after it; on an error `children[]` holds values that name nothing.
- **A copy's child** is as large as the VMO and holds only the pages the
  caller's copies map (pages mapped nowhere aren't copied, as Linux's
  fork), with a pager child's window if it is one; a kept VMO's pages are
  copied, not shared.
- **One instant**: the source's region lock for the whole call; each
  copied VMO's `protecting` raised before its first page is shared and
  lowered only after one shootdown of every write-protect at the end.
  `protecting` now freezes the VMO for every other writer: a write fault
  (any page), `vmo_write` and every pin or kernel mapping (`GET_PRIVATE`),
  decommit and a size change wait for it (one wait queue for all VMOs,
  instead of the "vmo resize" mutex, which the clone never takes), and a
  fault meanwhile installs nothing writable. That is what makes a clone of
  many VMOs untorn for writers outside the source too.
- **Charges**, to `dst`'s job (the caller's for a fork; the syscall text
  says so): the children (their struct, every page they share and their
  tables), `dst`'s mapping structs, and the table reserve: as many table
  pages as the source has over the mappings made again (`clone_tables`),
  which `dst`'s `pt_prepare` draws on before charging; what is left goes
  back when the address space dies (so FK3's swap gives it back with the
  old one). Kept twins get their entries at the clone, from the reserve.
- **Two region locks are never held at once** (lockdep has no subclass
  for mutexes): the new mappings are built in a tree of their own under
  the source's lock and moved into `dst` under `dst`'s.
- Measured on the PC (QEMU, 4 CPUs): 1 TiB with a page per GiB, 2,050
  source tables, 2.3 ms; 60,000 mappings of one VMO, 15 ms.
- Not built: a concurrent `process_start` racing the clone is not tested
  (the `setup` mutex is the argument); the `process_replace` side of the
  reserve (FK3).

### FL1a notes

What FL1b, FL2 and FL4 start from (`jam_fork` in `user/include/os.h`,
`user/lib/fork.c` and `fork_entry.S`; the message in `user/include/forkmsg.h`;
utests `fork_*`):

- **The call** is `int64_t jam_fork(const struct jam_fork_args *a)`: the
  child's process handle in the parent, 0 in the child, a status with nothing
  made on a failure. `a` (NULL for none) gives the calling thread's FS base
  (the caller's to say: FSGSBASE is off, so libjam can't read it; libos
  itself uses none), the child's name, the `vmos` and `children` arrays
  passed to `vmar_clone` (the children's handles are the caller's, for its
  give to move; a failed clone leaves them 0), `signals` (NULL, or where the
  client end of the child's new `SR_SIGNALS` goes: lx_fork asks, as
  posix_spawn does) and a context for the hooks.
- **The C library's hooks** (`struct jam_fork_libc`, set once at its start
  with `jam_fork_set_libc`; start.c keeps them, so setting them links no
  fork): `prepare` takes its locks just before the clone, `give` writes its
  messages on the child's startup channel after libjam's (the descriptors
  and the table of mappings: FL2), `parent` gives the locks back with the
  fork's status, and `child` runs in the child after libjam has made the
  roles its own (fresh locks, its messages read, the signal thread and the
  exception entry, which is the process's and so not in the child, started
  again: FL1b and SG3). None of them may allocate: libos's heap lock is in
  libos's `prepare` (startmain.c), and musl's fork holds its malloc lock
  across `_Fork`. FL1b's order to check: a layer lock held by a thread
  that then calls malloc meets musl's fork holding malloc's lock and
  waiting for that layer lock.
- **libjam's own locks are not held across the clone** (section 1.5 listed
  them): each module whose state the child can't use starts again empty
  there instead (`ns_forked`, `fs_forked`, `fdmsg_forked`, start.c's table,
  libos's printf channel), so neither a lock another thread held nor a
  table it was changing matters, and no lock order ties them to the C
  library's. There is no console lock in libjam. The namespace comes to the
  child as a starter's would: `ns_prepare(NS_ALL)` after the clone (the
  child's copy holds none of it) and an `nschan` mount on its new `SR_NS`
  after the start, as spawn sends it; svc_get's cache starts empty.
- **The roles** are fork.c's `dup_roles` and the made ones, each only if
  ours has `RIGHT_DUPLICATE` and `RIGHT_TRANSFER`; CJR's JIT-only handle
  (Q4 (A)) is one line there once CJ2 gives it a startup role. The child
  ends with `FORK_BAD_MESSAGE` (3) if its message is malformed.
- **The child's stack** is its copy of the forking thread's, from 128 bytes
  below the saved block; its set-up runs there.
- **The handle count** (`fork_handles`): the job's `JOB_LIMIT_HANDLES`
  counts objects too (a process while it lives, every VMO's struct, so
  each VMO `vmar_clone` makes), and the parent's heap may be made inside
  the fork (`ns_prepare`'s allocation): the test counts all of them.
- Not built here: anything POSIX (lx_fork, vfork's wait, descriptors,
  signals, ids: FL1b, FL2, FL4); libjam's per-connection buffers mapped
  `VMAR_CLONE_NONE` (FL2); a native program's open files (`struct jfile`)
  keep the parent's values in the child, as any handle the program holds
  itself.

## 6. Questions for the owner

One at a time, in this order; each ends with a recommendation, which the
plan assumes until the owner says otherwise.

### Q1. Does the kernel's part keep "no `fork` in the kernel"?

ARCHITECTURE's Goals: "No `fork` in the kernel, ever; the POSIX layer gets
`posix_spawn` (M13.5 emulates `fork` in libc on copy-on-write child
VMOs)". Libc can't copy a process's address space alone: it holds no
handle to the program's segments or stack (the starter mapped them), and
a copy made one VMO at a time while other threads run is torn.

- **(A)** Two kernel mechanisms, not a fork call: `vmar_clone` (copy the
  caller's mappings, copy-on-write, into a process that hasn't started)
  and `process_replace` (section 2). The process, its thread, its
  handles, descriptors and signals are libc's. Goals' line becomes "No
  `fork` call in the kernel: it copies an address space and replaces an
  image; `fork` and `exec` are libc's".
- **(B)** libc only: every starter keeps a handle to everything it maps
  and libc copies VMO by VMO; correct only in a single-threaded parent.
  Chromium's browser has dozens of threads, so (B) can't serve it.

*Recommendation: (A).* Both are mechanisms in the style of Zircon's
(a VMO child, an address space), and they must enter the ABI before M14.
Whose: the owner's (his architecture rule).

**Answered by the owner (2026-10-16): (A), and Goals' line stays as it
is.** `vmar_clone` and `process_replace` are not a fork call; used
together by libc they make a safer, virtual `fork`, so the rule "no
`fork` in the kernel" already allows them and needs no rewording.

### Q2. `execve`: in place, or forwarded

Section 2's table. **(a)** spawn-wait-forward as today: leaks a copy of
the browser per renderer and a `SIGKILL` misses the program; **(a2)** the
identity handed back to the parent in libc: no kernel work, but the
kernel's pid isn't Chromium's and a native parent keeps the old way;
**(b)** `process_replace`: one kernel call (FK3), the same process
throughout.

*Recommendation: (b).* Chromium holds the pid `fork` returned and must
reach the renderer through it; (b) is the only way every observer agrees,
and it mends `execve` for every program. Whose: technical (decided by
this recommendation unless the owner says otherwise), but it adds a
system call before M14.

**Decided by the main session (2026-10-16, a technical choice): (b)**, in
place through `process_replace`: Chromium's kill, waitpid and
`/proc/<pid>/stat` must reach the renderer by the pid it holds.

### Q3. `vfork`: as `fork`, or the recorder

Section 3. **(A)** `vfork` is `fork` and the parent waits for the child's
exec or exit; **(B)** the recorder ROADMAP's M13.5 row names (the owner,
2026-10-11: "the child's descriptor changes recorded until `execve` or
`_exit`, then `posix_spawn`").

*Recommendation: (A).* With a real `fork` the recorder buys only the cost
of one copy-on-write fork, and it is wrong for any call it doesn't
record; (A) is right for every program and a few lines. Whose: the owner's
(it changes his roadmap text).

**Answered by the owner (2026-10-16): (A)** — `vfork` is the virtual `fork`
(`vmar_clone` + `process_replace` through libc) with the parent waiting
for the child's `exec` or `_exit`. No recorder.


### Q4. The JIT power for renderers only, with Chromium's own launcher

The owner's default is "renderers only". CJR (design question 2)
recommends giving the power as a handle of its own (a root narrowed to
`RIGHT_ROOT_JIT`, with transfer and duplicate, under its own startup role)
that its holder may hand on. With `fork`, Chromium's unpatched launcher
can't choose which child gets it.

- **(A)** Every forked child keeps that handle, and an exec of the same
  program (`/proc/self/exe`) passes it on; Chromium's non-renderer
  process types close it as they start, before reading anything (a few
  lines in content's child start, not the launcher; libc gives a call to
  close it), and run `--jitless` as planned.
- **(B)** Every child keeps it and `--jitless` alone keeps the
  non-renderers from using it: no Chromium patch, but a flag, not a
  capability (CJR's option (c) in effect).
- **(C)** No child gets it at the fork; the browser sends it to each
  renderer over Mojo after the launch (libc makes the power a descriptor
  kind that `SCM_RIGHTS` carries): patches on both sides of a Mojo
  interface.

*Recommendation: (A).* The power stays a capability that only renderers
hold once their start is done, at the cost of a small patch away from the
launcher; it is needed only at CR-e (the JIT), after CB5. Whose: the
owner's (his JIT rule meets his no-launcher-patch decision).

**Answered by the owner (2026-10-16): (A)** — every forked child inherits
CJR's JIT-only handle across an `exec` of the same program, and
Chromium's content child start closes it in every process that isn't a
renderer (a few lines of policy in Chromium, not a missing layer).

