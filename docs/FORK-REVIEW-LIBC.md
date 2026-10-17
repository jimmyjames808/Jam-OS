# FR2: the libc review of fork, exec and the notices (FL1a to FL4)

Track FR2 of [FORK-PLAN.md](FORK-PLAN.md) ("5. Tracks"): the C library's
side of fork, vfork, clone and exec, read at main 2ed95cb2 by an agent
that wrote none of it, with what came after the four tracks:

- **FL1a**, libjam's `jam_fork` (`user/lib/fork.c`, `fork_entry.S`,
  `startup_forked` in `user/lib/start.c`): the roles a child gets, the
  child's start on its copy of the stack, libjam's modules made fresh.
- **FL1b**, POSIX's `fork`, `vfork` and `clone` without `CLONE_VM`
  (`user/posix/sys/fork.c`): the seven locks across the clone, the
  child's steps, ids and the table of children.
- **FL2**, every descriptor kind's fork step, the table of mappings
  (`maptable.c`), `VMAR_CLONE_NONE` buffers, the cached service channels.
- **FL3**, exec in place (`spawn_replace` in `user/lib/spawn.c`,
  `exec_replace` and `exec_handover` in `user/posix/sys/spawn.c`): the
  hand-over v2 and what survives.
- **FL4**, the notices and vfork's wait (`user/posix/sys/proc.c`).
- **CCS**'s clone-child stack (`fork.c`'s `on_call_stack`, `calls.c`'s
  test) and **SX**'s exec trust (`user/services/init/exec.c`'s `raised`
  and `child_dir`, `start_sealed`'s image).

SGR ([SG-REVIEW](SG-REVIEW.md)) covered the signal machinery and rated
the layer heap High (its finding 1, track LH); FR1
([FORK-REVIEW-KERNEL](FORK-REVIEW-KERNEL.md)) the kernel. Neither is
repeated here.

Looked at first: every handle and role a forked child ends with (the
fork message, the descriptor and mapping stream, the namespace, the
address space's mappings), every handle an exec'd image starts with, what
a child can write that its parent acts on (the notices, the vfork wait,
`/svc/exec`'s image), every value of the parent's a child's copy keeps
(ids, caches, the directory, locks held by other threads), and the
clone child's stack switch.

Severity, as in the other reviews: **High** is a hole (a child holding
authority its program shouldn't, a parent made to act for a child), or a
hang or loss a real program will meet; **Medium** is wrong behaviour the
system or a test tier will meet; **Low** is a wart or a narrow race that
nothing relies on yet.

Counts: **0 High, 3 Medium (2 fixed, 1 a design question), 8 Low, 6
notes, 4 design questions.** No way was found for a forked or exec'd
child to hold a handle its program wasn't given, nor for a child to make
its parent do more than FL4 says (change which group signals and waits
reach it). The one claim of more than a caller has at `/svc/exec` is the
narrowed spawn child, already TS's (no image for it). Finding 1 widens
SGR's layer heap: that heap's lock must be held across vfork and clone
too, not only fork.

## Findings

| # | Sev | Where | What is wrong | How to make it happen | Outcome |
|---|---|---|---|---|---|
| 1 | Medium | `user/posix/sys/fork.c:358` (`lx_vfork`), `:376` (`lx_clone`), `:508` (`__jam_clone_process`) | musl's `fork()` holds its allocator's lock across the clone (`__malloc_atfork`), so a child of a threaded parent finds malloc whole and free. vfork, a raw `SYS_clone` and musl's `clone()` take none of musl's locks (upstream's way: on Linux their child only calls `execve`, which allocates nothing in the process). Here the child's `execve` is the layer's, which allocates at once (`spawn_child`'s `malloc(PATH_MAX)`, the descriptor table's copy, the hand-over), as do `open`, `dup2`, `close` and `chdir` on paths. A child cloned while another thread held malloc's lock waits for it for ever; a vfork parent then waits for that child for ever too. gtest's threadsafe death tests (base's default since CCS) are exactly `clone()` then `execve` in a threaded parent. | A thread looping on `malloc` and `free` while another vforks (or clones) a child that calls `execve`: the child hangs in its first `malloc`, the parent in vfork. | Fixed (musl's allocator): vfork and both clones take its lock first, as musl's `fork()` does (`__malloc_atfork`: given back in the parent, made fresh in the child); `fork()` itself is musl's to lock, and a raw `SYS_fork` or `_Fork` can't be told from it (they stay as on Linux). Test: posixtest fork `alloc` (two threads in malloc while 40 vfork and 40 clone children each allocate: every one ends with 0); without the fix the case ran out of its 60 s (batch 18101355), with it the 80 end (18101726). A program whose malloc is its own (Chromium's PartitionAlloc) needs the layer heap (LH): the layer's allocations then come from musl's internal allocator, whose lock this already covers. |
| 2 | Medium | `user/posix/sys/spawnfd.c:59` (`our_cwd`) | The directory a spawned or exec'd child starts in, and that a relative program path is found in, was `getcwd()`'s, and else the hand-over's (the image's first directory), else `/`. Since CBF6 `getcwd` says `ENOENT` for a directory removed under us, so after an `rmdir` of the current directory the child starts in that other directory and `./prog` names a program there. Linux's child inherits the removed directory (its relative paths fail with `ENOENT`). A program that cleans a directory it was handed (or runs `make` in it) would act on another one. | `mkdir d; chdir d; rmdir d`, then `execve("./x")` or one whose program uses relative paths. | Fixed: `our_cwd` takes the layer's directory as text (`path_cwd`), removed or not, so the child finds it gone. Test: posixtest exec `gone` (a forked child removes the directory it is in and execs: the new image's `getcwd` is `ENOENT` and an `O_CREAT` open there `ENOENT`); without the fix the new image was in `/` (batch 18101726: `exec.c line 250`). |
| 3 | Medium (design) | `user/posix/sys/spawn.c:664` (`lx_execve` of a `/data` program), `user/posix/sys/proc.c:788` (`lx_child_wait`) | A program from `/data` is still spawn, wait, exit (FORK-PLAN section 2's (a), until FX): the parent's `SIGKILL` of the pid it forked kills only the waiter, and the program runs on in the job, unseen (the parent's `waitpid` already reported the kill); the program's `setpgid` and `setsid` notices reach the waiter, never the parent, so `kill(-pgid)` and group waits miss it; the waiter's other threads (an `execve` from a threaded program) run on; signals pending at the `execve` aren't handed on (only an exec in place hands them on). base's launch tests (`KillSlowChild`, every test timeout) kill such programs. | A forked child `execve`s a `/data` program that sleeps; the parent `kill(pid, SIGKILL)`s and reaps the child: the program is still running. | Not fixed: design question 1 (a job per forked child now, or FX). |
| 4 | Low (design) | `user/posix/sys/spawn.c:502` (`exec_handover`) | An exec in place hands over no `AT_SECURE`: a raised program (SX: its list asks for more than its caller's) that execs itself, as Chromium makes every child through `/proc/self/exe`, runs the new image as not raised, so musl's secure behaviours (a `TZ` file path, `secure_getenv`, `NLSPATH`) end at that exec. Linux keeps `AT_SECURE` while the raised credentials last. The variables the crt drops are gone already. | A program started raised execs `/proc/self/exe` with `TZ=/some/file` in its environment. | Not fixed: design question 2. |
| 5 | Low (design) | `user/posix/sys/spawn.c:282` (`child_state`), FL3's notes | rlimits are handed on neither by exec nor by `posix_spawn` (FL3's call): a lowered `RLIMIT_NOFILE`, `RLIMIT_CORE` or `RLIMIT_NPROC` (`ulimit -n 64; exec prog`) is back at the default in the new image. The limits are libc's own, so nothing is escaped; it is fidelity. | `setrlimit(RLIMIT_NOFILE, 64)`, then `execve`: the new image's `getrlimit` says the default. | Not fixed: design question 3. |
| 6 | Low | `user/posix/sys/sigthread.c:69` (`sig_channel_send`) | `kill` of a child whose signal channel is full waits up to a second for room, per child: `kill(-pgid)` over N such children takes N seconds, and the signal thread of a process waiting for its `/data` program (`arrived`) stalls the same way. Linux's `kill` never waits (a signal pending already isn't queued twice). A child whose signal thread can't keep up (or a hostile one that keeps its end and never reads) is enough. | A child that blocks its own signal thread (or a stub that holds `SR_SIGNALS` and never reads), then a flood of `kill`s to it. | Not fixed: design question 4 (drop when full: what is lost then). |
| 7 | Low | `user/posix/sys/fork.c:348` (`fork_core`) | When the table of children is full, `fork` says `EAGAIN` after the child has started: it has returned 0 and may write to a shared pipe or file before it is killed. Linux's `EAGAIN` means no child ran. | 2,048 children unreaped, then a fork whose child writes at once. | Not fixed: reserving a place in proc.c's table before `jam_fork` would close it. |
| 8 | Low | `user/posix/sys/proc.c:76` (`CHILD_MAX`) | `CHILD_MAX` is half the signal thread port's bindings (a child's end and its notices), but the same port binds the starter's channel, the terminal (two) and each timer: at 2,048 children a `timer_create` or the terminal's binding fails, or the last child is `EAGAIN` after it has run (finding 7). | 2,048 children, then `timer_create`. | Not fixed: `CHILD_MAX` a few below half. |
| 9 | Low | `user/posix/sys/fork.c:299`, `:387`, `:519` | `CLONE_PARENT_SETTID` and `CLONE_CHILD_SETTID` store to the program's pointers directly: a bad one faults inside the layer (`SIGSEGV`), where Linux ignores the failed store and the clone succeeds. | `syscall(SYS_clone, SIGCHLD \| CLONE_PARENT_SETTID, 0, (void *)8, 0, 0)`. | Not fixed: `uc_put` would do; left beside TS's `CLONE_FS` change in the same functions. |
| 10 | Low (for TS) | `user/posix/musl/src/thread/x86_64/clone.c`, `fork.c`'s `__jam_clone_process` | A `clone()` child's musl thread record keeps the parent thread's tid (musl's `clone()` sets none, as upstream), so `raise` and `abort` there aim at a tid the child doesn't have: `abort` then ends in `a_crash` (`SIGSEGV`), not `SIGABRT`. On Linux the same stale tid signals the parent's thread. | `clone(fn, ...)` whose fn calls `abort()`: `WTERMSIG` is `SIGSEGV`. | Not fixed: TS's (the main thread's tid becomes the pid there). |
| 11 | Low | `user/posix/sys/spawn.c:502` (`exec_handover`), `tty.c`'s `ctty` | The controlling terminal is handed on only as a descriptor: a program that closed 0 to 2 but keeps its terminal (a session leader's) loses it at an exec in place, so the new image gets no `SIGINT`, `SIGWINCH` or `SIGHUP` from it. Linux keeps the controlling terminal across exec. | `setsid`, open the terminal, close every descriptor, `execve`; Ctrl+C does nothing. | Not fixed. |
| 12 | Note | `user/posix/sys/fork.c:162` (`flush`), `maptable.c` | The child's descriptors and mappings come in messages of 64 handles on its startup channel, written before it starts: `CHANNEL_MAX_QUEUED` (1,024) bounds them to 65,536 handles. A process with some 60,000 distinct VMOs mapped can't fork (`EAGAIN`, nothing made). | | No change. |
| 13 | Note | `user/posix/sys/proc.c:535` (`lx_vfork_wait`) | A vfork child that never execs or exits holds its parent's vfork for ever, as on Linux; the parent stays killable and its other threads run. A child may also send its exec notice without exec'ing: it only lets the parent go on. | | No change. |
| 14 | Note | `user/posix/sys/proc.c:431` (`hear_locked`) | The notices, read for what a hostile child can do: the writer's pid is the child's or the message is dropped (an end handed on writes nothing that counts), the format is the generated dispatch's, handles are refused by the end's mark (and closed if one came), an oversized message ends listening to that child only, a group is the child's pid or one of its session we know; a flood costs the parent 16 reads a turn and 16 per child per look at the groups. A lie changes only which of our group signals and waits reach the liar (FL4's claim holds). | | No change. |
| 15 | Note | `user/services/init/exec.c:395` (`raised`), `user/lib/fork.c:72` (`dup_roles`) | SX's trust, read for a child claiming more than it has: a forked child holds its parent's image and a copy of its namespace (it is the same program); an exec in place gives the new image an image only for `/proc/self/exe` (`image_rights`: the VMO it runs from); init never hands a caller a sealed VMO (`op_open` answers with a channel) and a child VMO never has `RIGHT_EXEC`. The narrowed spawn child (`posix_spawnattr_setgrants_np`) keeping its image is the one way, TS's. | | No change. |
| 16 | Note | `user/posix/sys/fork.c:420` (`on_call_stack`), `calls.c:89` | CCS's stack, read: one global test per call, false in every other process; a clone or a fork inside a clone child runs on the stack for calls and its child resumes there (the mapping and the thread's top copied); a handler run at a boundary, or on its sigaltstack, calls in place; the signal thread is told by its stack before any thread-local is read; a `siglongjmp` out leaves nothing to undo. What is left of small stacks is SGR's findings 3, 6 to 8 (TS). | | No change. |
| 17 | Note | `user/lib/fork.c`, `user/posix/sys/fork.c`, each kind's fork step | What a child holds, read handle by handle: libjam's roles (its own process, address space, thread, `SR_NS` and `SR_SIGNALS`; duplicates of `SR_JOB`, `SR_STDOUT`, `SR_CONSOLE`, `SR_BOOTFS`, `SR_SELF_IMAGE`, each only with `RIGHT_DUPLICATE` and `RIGHT_TRANSFER`; never the root or a service's role); the stream's descriptors and mappings by key (anything else closed, a handle without both rights never passed); the namespace by `ns_prepare_in(NS_ALL)`; no JIT VMO handle (the kernel leaves the writable view out, mem.c the entries); the connection buffers `VMAR_CLONE_NONE` and their pointers dropped (fobj's `jf`, sockets, Wayland, the mixer); the cached `/svc` channels made again; the clock page shared (a kept mapping). The seven locks are taken in one order everywhere they nest (proc, cwd, umask, table, mem, limits, sig), none held across malloc or a wait, and every other lock a child may find held is made fresh by its module's step before use. | | No change. |

## Design questions

1. **A `/data` program exec'd from a forked child** (finding 3): give each
   forked child a job of its own (inside ours), so `SIGKILL` of it is a
   `job_kill` that takes the program it started with it (init starts that
   program in a job inside its caller's), at the cost of a job per fork;
   or leave it for FX (init builds the program for a replace, so the
   process is the program). The group notices and pending signals would
   still need FX.
2. **`AT_SECURE` across an exec in place** (finding 4): should a raised
   image's exec (of itself, or of anything) start the new image raised,
   as Linux keeps `AT_SECURE` while the credentials last? It would be the
   hand-over's `secure` bit set from our own.
3. **rlimits across exec and spawn** (finding 5): hand the soft and hard
   limits over (a hand-over v3), as Linux does, or keep FL3's call.
4. **`kill` of a child whose queue is full** (finding 6): wait up to a
   second (today), or drop the signal (it can't be told to the child
   then; Linux's pending set would have it already), or a pending mask in
   the protocol that coalesces.
