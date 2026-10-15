# CR2: the POSIX and IPC review

The independent review of Chromium's POSIX and IPC layers
(docs/CR-PLAN.md, the CR2 row), at main d84583da, by an agent that wrote
none of it. Read in full:

- **CU1**, descriptor passing on socketpairs: `user/posix/sys/sockpair.c`,
  `fdpass.c`, `fdpass.h`, the adopters a record reaches (`pipe.c`'s,
  `memfd.c`'s, `file.c`'s and `fobj.c`'s, `dev.c`'s, `etc.c`'s), and the
  calls around them (`sockio.c`, `sockconn.c`'s shutdown);
- **CU2**, named sockets: the broker (`user/services/unix/main.c`,
  `abi/idl/unix.idl`), init's start of it (`user/services/init/unixsvc.c`),
  libc's side (`user/posix/sys/unixsock.c`, `svcsock.c`, `svcsock.h`);
- **CL2**, the self-image and `/proc`: `user/posix/sys/procfs.c`, the self
  branch of `user/posix/sys/spawn.c`, `user/lib/spawn.c` (the image, the
  path, the layout), `user/include/roles.h`, the hand-over's path
  (`user/include/posixfds.h`, `user/lib/fdmsg.c`);
- **CS1**, launching: `user/lib/start.c` (`take_args`), `user/lib/spawn.c`
  (`make_args`, `plan_layout`), `user/posix/crt/start.c`, `spawnfd.c`'s
  closefrom and the `posix_spawn` override;
- **CL1**, reservations: `user/posix/sys/mem.c`, `maptable.c`;
- **CL3**, raw calls: `prctl.c`, `sched.c`, `enosys.c`, `thread.c`'s futex;
- **U6** only where it meets these (`/proc/self/fd`'s paths, spawn's
  execute bit), and init's `/svc/exec` relay (`user/services/init/exec.c`)
  where the hand-over crosses it.

What it was read against: every record, handle and length that crosses a
process boundary read as hostile input (a socketpair's peer, a named
socket's connector or listener, a starter, a caller of `/svc/exec` or
`/svc/unix`); every handle on every path; and, for the self-image, the
owner's two rules: nothing runs that the owner didn't approve, and no
program gains authority it didn't hold.

Severity: **High** lets one program use another's authority, or corrupts
memory or a handle; **Medium** lets one program stall or deny another, or
a call that gives a wrong answer in a common case; **Low** a narrow case,
a slow path, or a design point.

**1 High, 2 Medium, 9 Low, 2 notes.** The High and one Medium are design
questions for the owner (below): reported, not decided. The last column
says what was done, with the commit and its test.

## What held up

- **A socketpair message read as hostile input** (`sockpair.c`'s
  `take_part`, `records_take`, `fdpass_adopt`): the header's every field,
  each record's place, size, data and handle count, every handle's object
  type, the record's status bits and its kind against the socket's policy;
  a file's path is never believed, a pipe end comes with no leftover bytes;
  a part that breaks the format drops its send's descriptors and sets
  `MSG_CTRUNC`; at most 253 descriptors are ever held; every handle is
  kept, delivered or closed on every path (the 10,000-round fuzz checks the
  job's handle count).
- **The read-only twin.** A memfd given out (a child, `SCM_RIGHTS`, a
  `MAP_SHARED` mapping) from a read-only description, or after a write
  seal, is a VMO without `RIGHT_WRITE` or `RIGHT_RESIZE`; a receiver's
  reopen of `/proc/self/fd/N` asks the VMO's own rights (`memfd_reopen`),
  so `O_RDWR` there is `EACCES` whatever the sender's record claims.
- **The self-image** (CL2). Spawn gives a child `SR_SELF_IMAGE` only for
  the VMO the child itself runs from, at offset 0, and only one with
  `RIGHT_EXEC`, which the kernel gives only to the boot image's VMO, a
  verified VMO (every page checked against the approval it was sealed
  with), a `vmo_make_exec` VMO (`RIGHT_ROOT_VMEX`) or JIT memory
  (`RIGHT_ROOT_JIT`): pages the owner approved, or pages a holder of an
  approved power made. `posix_spawn` of `/proc/self/exe` starts the child
  in our job, with our namespace or less and no root resource, so it holds
  at most what we hold. A JIT holder can start a child from its JIT memory
  through libjam's spawn, but that child runs bytes the parent could run
  itself, with less authority. Keeping `RIGHT_TRANSFER` (decided) lets a
  program hand its image on; a receiver runs it only if it chooses to,
  with its own authority. `from_handover` refuses a path under `/proc`,
  `/dev` or `/etc`, so `self_program` can't recurse through the hand-over.
  The one hole found is not the image: it is the names a less trusted
  starter writes (finding 1).
- **`SR_ARGS`** (CS1). `take_args` copies the strings once into a VMO of
  the child's own before parsing them (no double fetch), and every string
  must end inside the copy; the one gap is the size check's wrap (finding
  4).
- **The broker** (CU2). Keys are 128 bits from the kernel's generator
  (`os_random`), compared in constant time, never answered twice; the
  broker never reads a file and holds no namespace; a listener's queue is
  bounded by its backlog and the unread bound.
- **Reservations** (CL1). `MAP_FIXED` decommits in place only over private
  anonymous memory libc made (`all_anon`), replaces only what the table
  holds after filling the holes with fixed maps (so nothing libc didn't map
  is taken), and the kernel, not libc, decides execute (JIT VMOs only).

## Findings

| # | Sev | Where | What is wrong | The failure | Done |
|---|---|---|---|---|---|
| 1 | High | `user/services/init/exec.c:349-375` (`relay`), `user/posix/sys/file.c:467` (`file_adopt_fd` keeps a record's path), `file.c:396` (`kind_give` reopens it), `dir.c:342-352`, `procfs.c` (`from_handover`), `spawn.c:429` | `/svc/exec` moves the caller's POSIX hand-over to the child message for message, and the child believes its names: a file record's path, a directory record's path, the current directory and the program's path. The child (its own approval's namespace, which may reach what the caller's can't: the owner's CE1 rule) then resolves them with its own authority: a file it gives to a child of its own is opened again by that path with the record's access mode, a directory is opened by its path, the cwd is a name in its namespace, and `/proc/self/exe` reads and opens the named file | A program with `svc exec` starts an approved `/data` program whose namespace is wider and that runs children with its descriptors (a shell, `make`, `xargs`), giving it descriptor 0 as a file record named `/jam/private` with a channel of its own: the grandchild's stdin is a real open of `/jam/private` made with the child's authority, and what it prints comes back on the caller's pipe. `O_WRONLY` the same way writes there; the cwd sends the child's relative writes into a directory the caller can't write. CU1 refused the same belief for `SCM_RIGHTS` ("a path is only a name in ours") | Not fixed: a design question (Q1 below) |
| 2 | Medium | `user/posix/sys/fobj.c:57-78` (`jf_map`'s `file_stat_within`), reached from `sockpair.c:314` (`deliver_fds`, the description's lock held) | A file that arrives over a socket is made by the hand-over's adopter, which asks the file's channel for its size and id and waits up to `FS_CALL_TIMEOUT` (60 s) for the answer; the channel is the peer's | A peer that sends a file record whose channel never answers holds the receiver's `recvmsg` 60 s for each such descriptor (253 a message: over 4 hours), `MSG_DONTWAIT` or not, and every other thread on that socket with it: a renderer could freeze the browser's IO thread. Linux never waits here (CU1's note for CR2) | To fix: a file adopted without its path asks nothing; its size asked of the service when a call needs it, as for a mount with many writers. Test: posixtest scm, a file whose server never answers arrives within a second |
| 3 | Medium | `user/services/unix/main.c:129-158` (`on_bind`), `251-260` (`opener_free`), `227-249` (`on_svc_connect`); `abi/idl/unix.idl` | The broker's limits are per opener channel, but it can't tell programs apart: one program may open all 64 opener channels, and a listener outlives its opener channel counted against nobody, so even one channel at a time binds all 128 listeners (bind 32, close the channel, open another). Abstract names are one table, first come | Any program with `svc unix` can take every opener slot (every other program's `bind` is `EACCES`, `connect` `ECONNREFUSED`) or every listener (`ENOBUFS`), and can take any abstract name first, so a listener it doesn't own is never reached (CU2's note; abstract names are global by the decided one-user model) | Not fixed: a design question (Q2 below) |
| 4 | Low | `user/lib/start.c:197-199` (`take_args`) | `STARTUP_ARGS_MAX - slots` wraps (64-bit) when the header's `argc + envc` is 262,143 or 262,144: `slots` is then past `STARTUP_ARGS_MAX`, and the strings' length is not checked at all | A starter's `SR_ARGS` header can make the child copy up to 4 GiB of strings into its own job's memory, past the 2 MiB limit (the starter is the child's creator, so no authority is gained) | To fix (trivial). Test: posixtest launch, malformed `SR_ARGS` VMOs (this one, a bad magic, version and path, strings that run off the end or past the VMO, an event for a VMO, strings in the message too) each leave the child with argc 0 |
| 5 | Low | `user/posix/sys/unixsock.c:86-91` (`unix_name_out`) | For a path of 108 bytes (no NUL in `sun_path`), `full` is `sizeof(struct sockaddr_un) + 1` and the copy reads one byte past the stack `struct sockaddr_un` | `getsockname` and `accept` of a socket bound to a 108-byte path give a length of 111 and one stray stack byte (Linux gives 110) | To fix (trivial). Test: posixtest unix, a 108-byte path's `getsockname` length |
| 6 | Low | `user/posix/sys/sockpair.c:231-234`, `283` | When `calloc` of the description's `sp_fds` fails, `take_part` returns `PART_LAST` with `p->fds` still NULL, and `fill_locked` reads `p->fds->ready` for a stream's empty last part | A NULL dereference in the receiver, only when the heap is short and the peer sends a part with no bytes | To fix (trivial). No test: needs `calloc` to fail at that call |
| 7 | Low | `user/posix/sys/sockpair.c:676-679` | `send_lock` is held across a blocking send's wait for room | A `MSG_DONTWAIT` (or `O_NONBLOCK`) send on a description waits while another thread's blocking send on it waits for a slow reader; Linux's never does. Mojo sends from one thread, non-blocking | Listed |
| 8 | Low | `user/posix/sys/memfd.c:337-352` (`obj_from_wire`) | A received memfd's seals are the sender's word. Only the write seals are backed by rights, and only for the receiver's handle: the sender still holds its own writable VMO, and a VMO a hostile sender made itself need not be kept (it can shrink) | `F_GET_SEALS` on a received memfd can say `F_SEAL_WRITE` or `F_SEAL_SHRINK` while the sender writes or shrinks it: a receiver that trusts a seal (as libwayland-server's `wl_shm` does to skip its SIGBUS guard) can be shown changing data or fault. Jam OS's compositor maps with `VMAR_KEPT_ONLY`, as memfd.c's header says a receiver must | Listed |
| 9 | Low | `user/posix/sys/memfd.c:277` (`vmo_for`) | `F_SEAL_GROW` is not carried into the rights: a writable memfd given out after it keeps `RIGHT_RESIZE` | A receiver can grow the shared VMO with `vmo_set_size` although the object is grow-sealed (shrinking stays impossible: the VMO is kept) | Listed |
| 10 | Low | `user/posix/sys/fobj.c:66-67`, `memfd.c:354-373` | A received file's transfer buffer and a received memfd's VMO are mapped as they come | A peer's own VMO, shrunk or decommitted after the receiver mapped it, faults the receiver on its next read of that file or touch of that mapping; a VMO whose pager never answers holds the fault (a pager needs `RIGHT_ROOT_PAGER`). For a memfd Linux has the same SIGBUS; for a file it has nothing like it | Listed |
| 11 | Low | `user/posix/sys/procfs.c:553` (`reopen_other`) | `/proc/self/fd/N` of a file opens its path again as the path is now (U6: "paths as written") | After a rename, or a symlink put in its place, the reopen gives another file; Linux's link opens the file the descriptor has | Listed |
| 12 | Low | `user/posix/sys/file.c:363-376` (`lend_locked`), from `fdpass.c` (`give_one`) | A writable file on a one-writer mount (fat) sent with `SCM_RIGHTS` is lent: the sender's own open is closed until the receiver closes its own (M13's model for children) | A receiver that keeps the file denies the sender writes to its own file (`EBUSY`); Linux shares the open | Listed |
| 13 | Low | `user/posix/sys/maptable.c:71-77`, `mem.c:149-156` | `mt_vmo_shared` walks the whole table, and `drop_pages` and `mt_remove` call it for each entry an unmap or a `MAP_FIXED` touches | An `munmap` over k of n entries costs k times n: slow for a reservation cut into thousands of pieces (CK2 allows 65,536) | Listed |
| 14 | Note | `user/posix/sys/prctl.c:106-110`, `user/posix/crt/start.c:148` | `PR_SET_NO_NEW_PRIVS` is accepted and changes nothing, and `AT_SECURE` is always 0, while `/svc/exec` can start a child with more authority than its caller (finding 1's boundary) | A sandbox that sets no-new-privileges expecting an exec never to gain authority is not told; the child can't tell from `AT_SECURE` that its environment came from a less trusted caller | Listed (with Q1) |
| 15 | Note | `user/services/unix/main.c:192-200` | A listener's waiting connections are charged to the broker's job until accept reads them, at most 64 a listener: 8,192 channel ends for 128 listeners nobody accepts on | One program can make the broker hold that much (bounded); with finding 3 it is the same program | Listed (with Q2) |

## Questions for the owner

**Q1 (finding 1): what does a child started through `/svc/exec` keep of
its caller's names?** The hand-over is name-based (a file's path, a
directory's path, the cwd, the program's path), and names are resolved in
the namespace of whoever uses them. Under the CE1 rule the child's
namespace may reach more than the caller's, so every believed name lends
the caller the child's authority. Options:

- (A) **init rewrites the hand-over** it relays (`posixfds_read` into a
  sink, then `posixfds_write`): file records lose their paths (as
  `SCM_RIGHTS` files do: the child can use them but not give them on,
  `ENOTSUP`), directory records are dropped, the program's path is the one
  init sealed, and the cwd becomes `/`. Safe; a POSIX program started this
  way can't hand a redirected file to its own children, and starts at `/`
  (a `make` started through `/svc/exec` must be told its directory).
- (B) **The child's namespace is its approval's and the caller's
  intersection.** Then believing names is harmless, but a low-access parent
  can no longer start a higher-access child: against the CE1 decision.
- (C) **Names become objects**: the hand-over carries a directory channel
  for the cwd and directory records, and file records carry no path (a
  format change, v2), so the child holds only what the caller held. The
  right end state; the most work.

My recommendation: (A) now (small, in init's relay), (C) when a port
needs a `/svc/exec` child to inherit a working directory. Environment
variables naming files (`FONTCONFIG_FILE`, `TMPDIR`) are the same kind of
name, smaller; `AT_SECURE` 1 for such a child would let libraries that
check it ignore them.

**Q2 (finding 3): how should `/svc/unix` count a program?** The broker sees
opener channels, not programs. Options: (A) init gives each program its
own opener channel when it publishes `/svc/unix` in that program's
namespace (one per namespace), and the broker counts listeners against
that channel until each listener closes; (B) listeners are charged to
their creator (a slot table per opener, freed with the opener's last
listener), and openers per program limited by init the same way; (C) keep
the global limits (a program with `svc unix` is trusted not to take them
all). Abstract names per session would need the session in the opener
(as (A) gives); Chromium's singleton uses a socket file, so abstract names
matter only to ports that use them. My recommendation: (A), and abstract
names stay global until a port needs them per session.

## Tests added

A fuzz loop between two processes (posixtest's scm section, the child
`--child-hostile`): a hostile peer over a socketpair and over a named
socket's connection through `/svc/unix` sends random messages, parts with
random descriptor counts, kinds, handles and truncations, and files whose
server never answers; the receiver takes them with random room and flags.
(Filled in with the fixes.)
