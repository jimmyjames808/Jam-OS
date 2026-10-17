# INOTIFY plan: file watching for Chromium

`inotify_init` and `inotify_init1` are `ENOSYS` today
(`user/posix/sys/enosysnames.c`), so Chromium's `base::FilePathWatcher`
can't start, and all 165 `FilePathWatcher*` tests of `base_unittests` are
on the skip list (`tools/cr/base_unittests.skip`; [CR-PLAN's CB-RM
count](CR-PLAN.md#cb-rm-2026-10-17)). They are the biggest group left
before the CB3 gate, whose wording includes `inotify`. Chromium watches
files through one reader in base (base/files/file_path_watcher_inotify.cc,
Chromium 155.0.8059.39, the version ports/chromium builds) and through
`FilePathWatcher` users above it: net's DNS configuration watcher
(`/etc/resolv.conf`, `/etc/hosts`, `/etc/nsswitch.conf`), policy and
profile files. This plan is that layer, built the owner's way (the layer,
not a Chromium patch). Built so far: the protocol and the library (track
IP, [its notes](#ip-notes)); no service makes events yet.

**In one paragraph.** The file services make the events, because only
they see every change: through every view, by every process. A program's
libc asks a mount's service for a watcher channel (a new `fs` method),
adds watches on it by path, and the service keeps a small queue per
watcher channel and rings a bell (a kernel event's user signal that libc
gave it) when the queue stops being empty. The inotify descriptor is libc's:
its readiness is the bell, so `poll`, `epoll` and `select` work as for
eventfd; `read` and `FIONREAD` take the queued events from each service
and hand out Linux's `struct inotify_event` records. Watches are on
inodes, so a change made through `/jam` reaches a watch made through
`/tmp`'s view of the same directory. Views keep their rights: a watch is
judged where its path resolves, hidden names never show up in an event,
and a watch whose inode leaves the watcher's view ends. Watches outlive a
service restart (the keeper holds their channels, the state VMO their
table) and the queue starts with `IN_Q_OVERFLOW`. The kernel changes
nothing.

## What this plan is not

- **No fanotify, no dnotify** (`fcntl(F_NOTIFY)`): Chromium uses neither.
- **No recursion in the services.** A watch is on one directory or file;
  Chromium's recursive watches add one watch per directory in user space,
  as on Linux.
- **No events from mapped stores** or write-back (WE1, WE2): Linux has
  none either.
- **No directory opens:** Jam OS has no directory handles yet, so there
  is no `IN_OPEN`, `IN_ACCESS` or `IN_CLOSE_NOWRITE` for a directory.
- **No events in libc's own trees** (`/`, `/svc`, `/dev`, `/etc`,
  `/proc`, `/sys`) and on read-only mounts that can't change (the boot
  image): a watch there is accepted and never fires ("quiet", 3.2).
- **No sharing across `fork` or `exec`** (Q3).
- **FAT later** (Q1, answered (B)): fat refuses watches; track IF waits
  until a FAT `/data` needs them.

## Where it starts

| Piece | Built by | What this plan does with it |
|---|---|---|
| The `fs` and `file` protocols, one request at a time per service, a transaction per change | extfs (M13.6), fat | a new `fs` method, a new protocol, an event at each change |
| Views and subviews, hidden names judged where a path resolves (`user/lib/fsview.c`, extfs's `user/services/extfs/walk.c`) | M13.6 | watches judged the same way; one service serves `/data`, `/jam` and init's views on a drive boot (DB1) |
| The keeper and the state VMO: a restart the clients don't see | M12, M13.6 (`user/services/extfs/adopt.c`) | watcher channels kept, the watch table in the state |
| Kernel events with user signals | the kernel; eventfd (`user/posix/sys/eventfd.c`) | the bell |
| Descriptor kinds, readiness through a kind's arm (`user/posix/sys/ready.h`) | M13 | a new kind |
| A kind's fork step, the exec hand-over | FL2, FL3 ([FORK-PLAN](FORK-PLAN.md#16-handles-and-descriptors)) | absent in a forked child, not given at exec (Q3) |
| libc's `/proc` (`user/posix/sys/procfs.c`) | CBF4 | `/proc/sys/fs/inotify` |
| A channel message's writer pid (`CHANNEL_READ_PID`) | CBF2a | a per-process cap on watcher channels |
| `base_unittests` on Jam OS, the skip list | CB2, CB-RM | the 165 tests run |

## 1. What Chromium uses

### 1.1 The calls

| Call | Chromium's use |
|---|---|
| `inotify_init()` | once per process, lazily: one descriptor for every watcher of the process (the `InotifyReader` singleton). Not `inotify_init1`, so no `IN_CLOEXEC` and no `IN_NONBLOCK`. musl's `inotify_init` calls `inotify_init1(0)` (294) first and `inotify_init` (253) after `ENOSYS`: both names are in CB2's `ENOSYS` log |
| `inotify_add_watch(fd, dir, IN_ATTRIB \| IN_CREATE \| IN_DELETE \| IN_CLOSE_WRITE \| IN_MOVE \| IN_ONLYDIR)` | every watch, always a directory (`IN_ONLYDIR`); `-1` from any error is "this path component isn't there (yet)" |
| `inotify_rm_watch(fd, wd)` | when its last watcher of that `wd` goes |
| `poll({fd, POLLIN}, 1, -1)` | the reader thread (`inotify_reader`) waits |
| `ioctl(fd, FIONREAD, &int)` | then sizes its buffer to the bytes queued |
| `read(fd, buf, that size)` | then walks the records: `sizeof(struct inotify_event) + len` each |
| `/proc/sys/fs/inotify/max_user_watches` | read once (`std::ifstream`); divided by 16 is the most watches one `FilePathWatcher` may hold (missing: an `ERROR` line in the log, then 8192 / 16) |

The reader ends for good (a warning, then the thread returns) if `poll`
fails, `FIONREAD` fails or is negative, or `read` fails. So **a read of
the size `FIONREAD` gave must never fail**: with a size of 0 it must not
wait and must not be `EINVAL` (3.3).

What it reads from an event: `wd`; `IN_IGNORED` (dropped at once);
`name` (`len` 0: the watched directory itself); `IN_ISDIR` (a file or a
directory); the kind, most specific first (`IN_MOVED_FROM` or
`IN_MOVED_TO` "moved", `IN_CREATE` "created", `IN_DELETE` "deleted",
anything else "modified"); `cookie` (non-zero: reported, so a move's two
halves can be paired). An event whose `wd` it doesn't know is dropped,
which is all that happens to `IN_Q_OVERFLOW` (`wd` -1): **Chromium ignores
overflow**, and never adds a watch again after an `IN_IGNORED`.

### 1.2 What a watch is to Chromium

`FilePathWatcher::Watch(target)` adds a watch on **every directory on
the target's path** ("/", "/data", "/data/cr", ..., the target's parent,
and the target itself if it is a directory), and learns that the target
changed from its parent's events naming it. A component that isn't there
stops the walk (its parent's `IN_CREATE` later adds the rest). A
component that is a symbolic link also has its link's target watched.
So:

- **"/" must be watchable**, and every mount point: a refused "/" stops
  the walk at once and the watcher never fires (Watch still returns
  true). On Jam OS "/" is libc's: quiet watches (3.2).
- A change to a file is seen as `IN_CLOSE_WRITE` or `IN_ATTRIB` **in its
  parent directory's watch, with its name**. `IN_MODIFY` is not asked.
- A recursive watch (`Type::kRecursive`) adds a watch per subdirectory,
  and more when `IN_CREATE | IN_ISDIR` comes.
- The `wd` must not come back soon after a watch ends by itself (its
  directory removed: `IN_IGNORED`, which Chromium drops without
  forgetting the `wd`): a new watch given that `wd` would have its events
  sent to the old watcher. Linux hands `wd`s out cyclically; so does libc
  (3.1).

### 1.3 The 165 tests

`base/files/file_path_watcher_unittest.cc`: 41 `FilePathWatcherTest`
tests that build on Linux, and 31 `FilePathWatcherWithChangeInfoTest`
tests, each run 4 times (non-recursive and recursive, with and without
the modified path reported): 41 + 124 = 165. What they check:

- **Exact event counts.** Most `FilePathWatcherTest` tests compare the
  callbacks with an expected list (`ContainerEq`), so one event too many
  fails the next step as surely as one too few. Linux's numbers are in
  them: a new file written is 2 (`IN_CREATE`, `IN_CLOSE_WRITE`:
  `kExpectedEventsForNewFileWrite`), an existing one 1, a directory's
  `TouchFile` 2 (`IN_ATTRIB` on the directory's own watch and on its
  parent's, by name), a rename inside a recursive watch 2.
- **Order and types** (the change-info tests): a new file is "created"
  then "modified", in that order; a move is two "moved" events with **one
  cookie** (`MatchCookies`); `IN_ISDIR` right on every event.
- **What arrives before the watch doesn't count**: `ModifiedFile` writes
  the file, then watches, then writes again and expects exactly one
  event. An `IN_CLOSE_WRITE` from the first write that came late would
  fail it (2.3).
- Symbolic links (`CreateLink`, `DeleteLink`, `LinkedDirectoryPart1` to
  3, ...): the link and its target watched; on ext `/data`, which has
  links.
- `RacyRecursiveWatch`: 100 rounds of 20 directories made, written and
  removed while a recursive watch comes and goes, and no watch left
  over. *unsure*: its time on Jam OS (6,000 changes, each a transaction).
- `InotifyLimitIn*`: the limit is overridden by the test, not the system.

### 1.4 Other users

net's DNS watcher (`net/dns/dns_config_service_linux.cc`) watches three
`/etc` files and logs an error for each that fails; libc's `/etc` never
changes, so quiet watches are the truthful answer. Policy, profile and
storage watchers watch real files: ext on a drive boot. No other
`inotify` caller in what CB3 builds (headless_shell) is known (*unsure*).

## 2. The services

### 2.1 Why the services make the events

There is no kernel file system: a change happens in the service that
serves the volume, whichever process asked and through whichever view
(`/data` and `/jam` on one volume, `/tmp`, `/usr`, `/home` and `/pkg` as
views of it: DB1). So the service is where every change is seen once,
and the only place a change by another process is seen at all. extfs
keeps a table of watched inodes; each request that changes, opens,
reads or closes something looks its inode and its directory up there
(one lookup, nothing when the volume has no watch) and queues an event
on each matching watch.

### 2.2 Which change makes which event

Linux's rules (fsnotify's), so that the counts of 1.3 come out the same.
"The directory" is the one the request's walk ended in (for an open
file's later events: the directory and name it was opened by, which the
service keeps in memory and moves with renames of that name).

| Request | The directory's watches (with the name) | The inode's own watches |
|---|---|---|
| a new name: `fs.open` with `FS_CREATE` making the file, `mkdir`, `symlink`, `link` | `IN_CREATE` | `link`: `IN_ATTRIB` (its link count) |
| `unlink` of a file | `IN_DELETE` (after the inode's `IN_ATTRIB`) | `IN_ATTRIB`; when the inode goes (its last name and its last open): `IN_DELETE_SELF`, `IN_IGNORED` |
| `unlink` with `UNLINK_DIR` | `IN_DELETE` | `IN_DELETE_SELF`, `IN_IGNORED` |
| `rename` | `IN_MOVED_FROM` in the old directory, `IN_MOVED_TO` in the new, one cookie | a replaced target's inode `IN_ATTRIB` (its link count, a directory's too), then the moved one's `IN_MOVE_SELF`; the target's `IN_DELETE_SELF` when it goes |
| `rename` with `RENAME_EXCHANGE` | two pairs, a cookie each (vfs_rename's second fsnotify_move) | each pair followed by its inode's `IN_MOVE_SELF` |
| `file.write`, `file.truncate`, `fs.open` with `FS_TRUNCATE` | `IN_MODIFY` | `IN_MODIFY` |
| `setattr` (`fs.` or `file.`): mode, owner, or both times | `IN_ATTRIB` | `IN_ATTRIB` |
| `setattr` of one time alone | `IN_ACCESS` (atime) or `IN_MODIFY` (mtime) | the same |
| `fs.open` of a file | `IN_OPEN` | `IN_OPEN` |
| `file.read` | `IN_ACCESS` | `IN_ACCESS` |
| the file's channel closed | `IN_CLOSE_WRITE` if opened `FS_WRITE`, else `IN_CLOSE_NOWRITE` | the same |
| `readdir`, `getattr`, `stat`, `readlink`, `resolve`, `file.map`, stores to a mapping | nothing | nothing |

`IN_ISDIR` is set whenever the subject is a directory, but never on
`IN_DELETE_SELF` and `IN_MOVE_SELF` (inotify masks it off those). The
directory's event comes first, then the inode's own (Linux's order),
except a new link's and an unlink's `IN_ATTRIB`, which come before the
name's `IN_CREATE` or `IN_DELETE` (fsnotify_link, vfs_unlink). A directory's
own times changing because of a new or removed name make no event (Linux
makes none). An event goes only to watches whose mask has its bit;
`IN_IGNORED`, `IN_Q_OVERFLOW` and `IN_UNMOUNT` always go. `IN_ONESHOT`
ends a watch after its first event; `IN_EXCL_UNLINK` keeps an unlinked
open file's events out of its old directory's watches. An event equal to
the last one still queued on that channel (same watch, mask, cookie and
name) is dropped, as Linux merges them.

libc shares one open of a file among a process's descriptors (fobj,
`user/posix/sys/fobj.c`), so `IN_CLOSE_WRITE` comes when the process's
last descriptor of the file closes, not at each close (3.6).

### 2.3 Order: a close before the next request

Linux queues a close's `IN_CLOSE_WRITE` in `close` itself, before
anything the thread does next. Here a close is a channel closing, which
the service sees as a peer-closed packet on its port, possibly after a
request the same thread sent later on another channel (a persistent
binding already queued keeps its earlier place in the port's queue,
`kernel/object/port.c`). `ModifiedFile` (1.3) fails if that happens.

**The rule: a service handles every close made before a request was
sent before it serves that request.** The peer-closed packet is queued in
the close's system call, so it is on the port by the time any later
request can be read. When the volume has a watch, or the request is on a
watcher channel, the service first takes every packet already on its
port (waits with no time), handles the closes among them, keeps the
rest in order, then serves the request. One extra port call per request,
only while something is watched. utest `port_order` pins the kernel
behaviour this needs (a peer-closed packet, then a message written after
the close on another channel bound to the same port: both there, the
close's first or coalesced earlier) without a kernel change.

### 2.4 The protocol

Built (IP): `fs` gains method 17, and the watcher channel has a file of
its own, abi/idl/fswatch.idl (the real text is there):

```
# fs.idl
17 watch  () -> (handle watcher)

# fswatch.idl
protocol fswatch 49 kept
const BELL 1 << 24             # the bell's user signal
1  add     (str[<=4095] path, u32 mask) -> (u32 id, u8 existed)
2  remove  (u32 id) -> ()
3  take    (u32 room) -> (u8 more, u8[<=8192] events)

protocol fswatch_bell 52
1  give  (handle<event> bell) event
```

**The bell is not an argument of `fs.watch`** (this plan's first sketch
had `watch (handle bell)`): `fs` is `kept`, and genidl refuses a handle in
a kept protocol's request, since a successor runs the request in progress
again from its slot and the dead instance's handles died with it. Nor can
the service make the bell and hand it back: libc's descriptor waits on one
handle (a kind's arm, `user/posix/sys/ready.h`) over every mount it
watches. So libc sends its bell as the watcher channel's first message,
an event of a protocol of its own (`fswatch_bell.give`, not kept: it is
taken once); until the service holds it, anything else first closes the
channel. A service that died after reading it and before its keeper held
it has no bell: its successor closes the channel at its next message, and
libc asks `fs.watch` again. The rest of the IDL is the sketch's: `IN_*`
bits as constants (`FSWATCH_IN_*`), the records' layout
(`FSWATCH_RECORD_FIXED`), `FSWATCH_TAKE_BYTES` and `FSWATCH_QUEUE_BYTES`;
`take` answers `ERR_BUFFER_TOO_SMALL` when its first record doesn't fit
`room`; the client clears the bell before it takes, never after, and
takes until `more` is 0.

An `id` carries a generation (its slot and a count), so an id is never
given again while libc might still hold the old one's events. Watch
requests change nothing on the disk; a successor runs one again after a
crash (an `add` finds its watch, a `remove` finds none and answers OK, a
`take` answers the overflow).

### 2.5 Watches, keys and views

A watch is on an inode, keyed by its number and generation (ext). Every
view of the volume is served by the one process, so a change through any
view meets every watch on the inode. A watcher channel keeps its view's
flags and root (a copy: the view's own channel may close first), and
judges its adds and its events by them.

Limits (per volume, *unsure* of the state's room): 4096 watches, at most
1024 on one channel; 32 watcher channels, at most 8 from one process
(the `fs.watch` request's writer pid, CBF2a); a queue of at most 16 KiB
per channel, allocated as events come. Past the queue's limit one
`IN_Q_OVERFLOW` is queued and later events are dropped until a `take`
makes room.

### 2.6 Rights

- A watcher channel is never wider than the view it came from, and its
  adds walk as that view's requests do (`user/services/extfs/walk.c`):
  hidden paths (`FS_VIEW_HIDE_*`) are `ERR_ACCESS_DENIED`, nothing above
  a subview's root can be named. A watch needs the read permission bit
  (Linux's rule), as a listing does.
- **Names**: an event names an entry of a watched directory, which the
  watcher may list. The hidden entries are filtered per channel: `etc`
  in the volume's root with all three hide flags, `ssh` in `etc` with
  `FS_VIEW_HIDE_KEYS`, `authorized_keys` with `FS_VIEW_HIDE_AUTH`. The
  service knows those two directories' inodes, so the filter is a compare.
- **Reach** (Q2): when a rename moves a directory, or a watched file, to
  another directory, every watch through a narrower view (a subview, or
  hide flags) is judged again: its inode's place from the volume's root
  (`..` for a directory; a file's kept directory), and one now out of
  its view ends (`IN_MOVE_SELF` if asked, then `IN_IGNORED`).
- **The bell**: the service holds only `RIGHT_SIGNAL` on the watcher's
  event: it can ring or silence that watcher, nothing more. A service is
  trusted for its own files' events as for their contents.
- **Memory**: a watcher that never reads costs the service its 16 KiB;
  the one `take` reply in flight is charged to the service as any reply.

### 2.7 Moves and cookies

A rename's two events carry one cookie, from a counter each service
instance starts at a random non-zero value (a successor draws again), so
cookies of two services (`/data` and an in-memory `/tmp`) don't collide
in practice and a move between two views of one volume pairs (one
service). A rename between mounts is `EXDEV`, so a move never spans two
services.

### 2.8 Restarts and the mount going

| What happens | The watches | What the watcher reads |
|---|---|---|
| the service dies and a successor carries on (the keeper's restart) | kept: the table is in the state VMO (watcher records: view flags and root, the bell's keeper slot; watches: inode, generation, mask, id); the keeper holds each watcher channel and its bell | `IN_Q_OVERFLOW` first (the dead queue and any change finished by the successor from its slot without running again went unreported), then events as before |
| the successor starts fresh (the state given up) | gone: the watcher channels close, as open files do | `IN_UNMOUNT`, then `IN_IGNORED` per watch (libc makes them on the channel's peer-closed) |
| the mount goes (a stick pulled, `mount -w` / `-r`, volumes gives up) | gone | the same |

The open files' directories and names are kept in memory only: after a
restart a file opened before it makes no events in its directory's watch
(its own watch still does). The overflow says events were lost.

### 2.9 fat and the boot image

The boot image never changes, and its server answers `fs.watch` with the
generated `ERR_NOT_SUPPORTED`; libc makes a quiet watch for a read-only
mount that says so (`statfs`), which is truthful. fat (Q1): until track
IF, `ERR_NOT_SUPPORTED` too, and libc refuses a watch on a writable
mount (`EOPNOTSUPP`, *unsure*: inotify(7) lists no errno for it). With
IF: directories keyed by their first cluster (a rename moves the entry,
not the cluster), files by their entry's place (fat's `id`), moved with
renames; no links and no modes, so no `IN_ATTRIB` from `setattr`.

## 3. libc

### 3.1 The descriptor

A new kind (a new file user/posix/sys/inotify.c, its ops in
`user/posix/sys/kinds.h`):

- the **bell**: an event made at `inotify_init`, its `BELL` bit the
  kind's arm (readable), as eventfd's bits are (`user/posix/sys/ready.h`);
- the **watcher channels**, one per mount view it watches through, keyed
  by the mount's `fs` channel's koid (`handle_info`), so a mount replaced
  meanwhile gets a new one; each got its own duplicate of the bell;
- the **wd table**: `wd` to (channel, service id) or to a quiet watch;
  `wd`s from a counter that only grows (wrapping at `INT_MAX`), so a `wd`
  isn't given again soon (1.2); the same (channel, id) is the same `wd`;
- the **buffer**: records taken from the services and not yet read, as
  Linux's records (64 KiB at most).

`inotify_init1` takes `IN_NONBLOCK` and `IN_CLOEXEC` (`EINVAL` for any
other bit). `close` closes the bell and the channels (the services drop
their watches). The description's lock guards the table and the buffer
and is held across a service call (as a file's is), never across the
bell's wait.

### 3.2 `inotify_add_watch`

The path is resolved as `stat` resolves it (`path_get`, then
`path_info_follow` for links that lead out of a mount; `IN_DONT_FOLLOW`
sets `nofollow`), so views, symbolic links and mounts work as they do
for every call (`user/posix/sys/files.h`). Then:

- **inside a mount**: `add` on that mount's watcher channel (made with
  `fs.watch` the first time); `ERR_LINK_OUT` follows the link and adds on
  the mount it leads to. The service's answers map to `ENOENT`,
  `ENOTDIR`, `EACCES`, `EEXIST`, `ENOSPC`, `ELOOP`, `ENAMETOOLONG`.
- **quiet**: "/", `/svc`, libc's `/dev`, `/etc`, `/proc` and `/sys`
  trees, and a read-only mount whose service answers `ERR_NOT_SUPPORTED`:
  the path's existence is checked (`ENOENT`, and `ENOTDIR` for
  `IN_ONLYDIR` on a file), and the watch never fires. These never
  change, or change only as Linux's `/proc` does, without events.

`inotify_rm_watch`: `remove` on its channel (the service queues the
`IN_IGNORED`; the `wd` is forgotten when that is read), or for a quiet
watch an `IN_IGNORED` added to the buffer. An unknown `wd`: `EINVAL`.

### 3.3 `read`, `FIONREAD`, readiness

- **Taking**: clear the bell, then `take` from each channel until no
  `more` or the buffer is full, converting each record (`id` to `wd`,
  `id` 0 to `wd` -1, the name NUL-padded to a multiple of 16 bytes as
  Linux pads it). A channel that still has `more` rings the bell again
  (libc's own signal), so the next `poll` sees it. A channel that closed
  gives `IN_UNMOUNT` and `IN_IGNORED` for each of its `wd`s (2.8).
- **No lost wake**: the bell is cleared before the takes, and a service
  rings when its queue goes from empty to not empty, so an event queued
  after a take always rings again.
- **`FIONREAD`**: take, then the buffer's bytes (an `int`).
- **`read`**: take if the buffer is empty; whole records only, as many
  as fit; a count smaller than the first record: `EINVAL` (Linux). Empty
  after the take: `EAGAIN` with `O_NONBLOCK`, else wait on the bell
  (`EINTR` and `SA_RESTART` as other waits). **A count of 0 with nothing
  queued returns 0 at once** (Linux would wait): a bell rung for an event
  an earlier take already took makes `poll` say readable and `FIONREAD`
  say 0, and Chromium's reader then reads 0 bytes and must go back to
  `poll` (1.1). `pread`: `ESPIPE`; `write`: `EBADF` (the descriptor is
  `O_RDONLY`, as Linux's); `lseek`: `ESPIPE`.
- **`poll`, `epoll`, `select`**: the arm is the bell; the kind's `ready`
  is "the buffer isn't empty". No change to `user/posix/sys/epoll.c` or
  poll.
- `fstat` and `/proc/self/fd/N`: Linux's `anon_inode:inotify`.

### 3.4 fork, spawn and exec (Q3)

As an `epoll` set ([FORK-PLAN 1.6](FORK-PLAN.md#16-handles-and-descriptors)):
absent in a forked child, and not given to a spawned child or across
`execve` (give: `-ENOTSUP`, as `FD_CLOEXEC` would). Chromium's children
close it or never had it.

### 3.5 Limits

`/proc/sys/fs/inotify/max_user_watches` 4096, `max_user_instances` 128,
`max_queued_events` 512 (a 16 KiB queue of short records; *unsure*: no
program read is known but Chromium's of the first). Chromium's
per-watcher limit is then 256. At most 128 inotify descriptors per
process (`EMFILE` past that, as Linux's per-user limit answers).

### 3.6 Where it differs from Linux

- `IN_CLOSE_WRITE` at a process's last close of a file, not at each
  (2.2).
- No directory open, read or close events; no `IN_ACCESS` on `readdir`.
- A watch ends when its inode leaves the watcher's view (Q2).
- `read` of 0 bytes doesn't wait (3.3).
- Absent after `fork`, closed at `exec` (Q3).
- The same inode watched through two different mounts of one volume
  (`/data/x` and `/jam/x` on a drive boot) gets two `wd`s: libc can't
  tell the two views are one volume.
- Quiet watches in libc's trees and on the boot image (Linux's `/proc`
  is quiet too).
- A restart costs an `IN_Q_OVERFLOW` (2.8).
- A read of 0 bytes is 0 even with records held (Linux: `EINVAL`).
- Two mounts' records come mount by mount in a take, not interleaved in
  time; a mount going is seen at the next take, by a blocked `read`
  within a second, not by `poll` or `epoll` alone ([IL1 notes](#il1-notes)).

## 4. The kernel

Nothing changes:

- **Where events are made**: in the services; the kernel has no file
  system and can't name a file.
- **How they travel**: `fs.watch`'s channel carries requests and
  replies, which exist; readiness is an event's user signal, which
  eventfd already waits on through `poll` and `epoll`.
- **Memory**: queues are the service's memory, bounded per channel;
  nothing is written to a watcher unasked, so no message piles up
  charged to the service.
- **Restarts**: the keeper and the state VMO, as for open files.
- **Order**: the one kernel behaviour 2.3 leans on (a close's packet is
  queued in the close itself) is the port's today; utest `port_order`
  pins it.

Considered and dropped: a kernel notification object (one queue many
services write into): it would hold user memory in the kernel for a
program that doesn't read, and duplicate what a channel and an event do.

## 5. Tracks

Each about an agent-hour. Gates name the tracks running now: **WE1**
(extfs `pager.c`, `files.c`, `fsops.c`, `extfs.h`, `adopt.c`), **WE2**
after it (`pager.c`, `main.c`, `fsops.c`, `test.c`, `extfs.h`),
**DRO2** (extfs's and fat's `disk.c`), **WK3** (the kernel: no overlap),
**CBF6** (libc `path.c`, `file.c`, `procfs.c`), **FL4** (libc
`fork.c`), **CCS** (the clone child's stack; the only one running
`base_unittests` now).

| Track | What | Files | Gate | Tests | Size | Model |
|---|---|---|---|---|---|---|
| **IP. The protocol and the library** (built: [IP notes](#ip-notes)) | 2.4 and the service-independent half of 2.2, 2.5 and 2.7: `fs.watch`, the `fswatch` protocol, and a library every service uses: the watcher channels, the inode index, the queues with the merge and overflow rules, ids with generations, the bell, the per-process cap, the `take` encoding, the state records' layout; utest `port_order` | `abi/idl/fs.idl` (method 17 and its comment), a new abi/idl/fswatch.idl and its line in `abi/idl/ids.txt`, the generated headers, a new user/lib/fswatch.c and user/include/fswatch.h, new utest files user/tests/utest/fswatch.c (a fake service in-process) and user/tests/utest/port_order.c | none: new files, and `fs.idl` has no other editor (a missing method is `ERR_NOT_SUPPORTED` in every other `fs` server, so fat, bootfs and ramfs build unchanged) | utest `fswatch_` (merge, one overflow then drops, `take`'s room and `more`, ids not given again, the bell rung once per empty to not empty, a removed watch's events dropped and `IN_IGNORED` last, the caps), `port_order`; `make check` (genidl's check); init run; shell scripts basic, cmds, commands, files and kdump save (an IDL changed) | 1 | **Opus** |
| **IE1. extfs: events and order** (built: [IE1 notes](#ie1-notes)) | 2.1 to 2.3 on extfs: `fs.watch`, `add` walked in the view (`IN_ONLYDIR`, `IN_DONT_FOLLOW`, the mask flags, the read bit), every row of 2.2's table at its request, the open files' directory and name in memory, `IN_DELETE_SELF` when an orphan goes, cookies, the close-first rule; a restart ends every watch in this track | a new user/services/extfs/watch.c, `user/services/extfs/fsops.c`, `user/services/extfs/files.c`, `user/services/extfs/main.c` (the rule), `user/services/extfs/request.c`, `user/services/extfs/extfs.h`, a new utest file user/tests/utest/extfs_watch.c, `tools/shell-tests/extfs-svc.txt` (its line) | IP and WE1 merged; never beside WE2 (same files): before or after it | utest `extfs_watch`: each row of 2.2 with its exact events and order; a change through one view seen through another view of the volume; a move across directories with one cookie; the order case (write and close, add, write and close: exactly one `IN_CLOSE_WRITE`); `IN_ONESHOT`, `IN_EXCL_UNLINK`, `IN_MASK_CREATE`; `tools/extfs-svc-test.sh`; init run | 1 | **Opus** |
| **IE2. extfs: rights and restarts** | 2.6 and 2.8: the hidden-name filter, the reach check at a rename (Q2), watches and watcher records in the state VMO, keeper slots for each channel and its bell, the successor's adopt (watches back, `IN_Q_OVERFLOW` queued and the bell rung on each), a fresh start closing them, test powers that kill extfs between a change and its event; volumes' handle limits for extfs's job | user/services/extfs/watch.c, `user/services/extfs/views.c`, `user/services/extfs/walk.c` (the reach), `user/services/extfs/state.c`, `user/services/extfs/keeper.c`, `user/services/extfs/adopt.c`, `user/services/extfs/test.c`, `user/services/extfs/extfs.h`, volumes' extfs limits (a line), a new utest file user/tests/utest/extfs_restart_watch.c | IE1 merged | utest `extfs_watch`'s view cases (a hidden name never in an event, an add on one refused, a subview's watch ending when its directory moves above the root, a `HIDE_KEYS` watcher's watch ending when its directory moves into `etc/ssh`), `extfs_restart_watch` (killed at each point: the watches back, the overflow first, new events after; a fresh start: the channel closed); `tools/extfs-svc-test.sh`; `tools/layout-test.sh` drive; init run | 1 | **Opus** |
| **IL1. libc: the descriptor** (built: [IL1 notes](#il1-notes)) | 3.1 to 3.4 and 3.6 | a new user/posix/sys/inotify.c, `user/posix/sys/kinds.h` (its ops), `user/posix/sys/calls.def` (the four calls), `user/posix/sys/enosysnames.c` (the two names out), a new posixtest file user/posix/test/inotify.c (section `inotify`) and tools/shell-tests' new posix-inotify.txt | IP merged to start, CBF6 merged (the call tables); IE1 merged before its last run (events on ext); beside IE2 | posixtest `inotify` on ext `/data` and on `/tmp`: the records (padding, `len`, alignment), `FIONREAD`, a short read `EINVAL`, a 0-byte read, `O_NONBLOCK`'s `EAGAIN`, a blocking read woken, `poll`, `epoll` and `select` with another thread changing files, Chromium's reader loop copied (poll, `FIONREAD`, read) over 1,000 changes, quiet watches on "/", `/etc`, `/proc` and the boot image, `IN_ONLYDIR`, `IN_MASK_ADD`, `rm_watch`'s `IN_IGNORED`, `wd`s not reused; shell scripts basic, cmds, commands, files; init run | 1 | **Opus** |
| **IL2. libc: limits, fork, exec** | 3.4's tests, 3.5, `/proc/self/fd`'s name | `user/posix/sys/procfs.c` (`/proc/sys/fs/inotify`), user/posix/sys/inotify.c (the fork and give steps if IL1 left them), posixtest's `user/posix/test/procfs.c` and `user/posix/test/forkfd.c` (an inotify case each), the TESTING rows | IL1 and CBF6 merged | posixtest `procfs` (the three files), `fork` (absent in the child), `exec` (gone after `execve`); `tools/shell-tests/posix-fork.txt`; init run | 0.5 | Sonnet 5.5 |
| **IF. fat** (only with Q1's (A)) | 2.9 on fat with the library: keys, its rows of 2.2, the order rule in its loop, its state and keeper | `user/services/fat/` (a new watch.c, fsops.c, its loop, state, keeper, adopt), a new utest file user/tests/utest/fat_watch.c | IE2 merged (the pattern); DRO2 merged (fat's restart paths) | utest `fat_watch`, `fat_restart` unchanged; posixtest `inotify` with `JAMOS_DATA_FS=fat` | 1 | **Opus** |
| **IC. Chromium's tests** | the 165 run: `FilePathWatcher*` off `tools/cr/base_unittests.skip` (what still fails listed with its reason), the stage relinked (`tools/cr/build.sh start base_unittests test_child_process`) | `tools/cr/base_unittests.skip`, `docs/CR-PLAN.md`'s rows | IE2, IL1 and IL2 merged; no other agent running `base` | `CR_BASE_ONLY='FilePathWatcher*' tools/cr-test.sh <out> base` with `TMPDIR` on ext `/data`, and once on `/tmp` (`CR_BASE_ENV=`) | 0.5 | Sonnet 5.5 |
| **IR. Independent review and fix** | everything above, findings first (a new docs/INOTIFY-REVIEW.md), then High and Medium fixed with a test each: can an event reach a watcher who can't list its name, can a watch outlive its reach, can a service be made to hold unbounded memory, is a wake ever lost, is a close ever reported after the next request's events, can a `wd` be reused while Chromium holds it | the files its fixes need | IC | its own; hostile: a watcher that never reads (one overflow, the service's memory flat), 8 channels from one process and the ninth refused, renames moving directories in and out of a subview while it watches, a rename storm with extfs killed every 200 ms, 8 threads closing and renaming while another reads; full init run; the extfs and posix scripts | 1.5 | **Opus** |
| **IJ. The join** | ARCHITECTURE (Storage: watches; the POSIX layer: the kind), TESTING's rows, CR-PLAN, ROADMAP, HISTORY; this plan deleted when HISTORY covers it | the docs | IR | `make check` | 0.5 | Sonnet 5.5 |

**Order.** IP first (it gates nothing running). Then IE1 (after WE1) and
IL1 (after CBF6) side by side; then IE2 and IL2 side by side; then IC,
IR, IJ. IF, if chosen, after IE2. **Total: 7 agent-hours** (8 with IF).
IE1 and IE2 can't run beside WE2; the main session picks the order of
the three (recommended: whichever of WE2 and IE1 is ready first, the
other after).

### IP notes

Built (2026-10-18): `fs.watch` (method 17), abi/idl/fswatch.idl
(`fswatch` 49, `fswatch_bell` 52), `<fswatch.h>` and user/lib/fswatch.c
(in libos, so any service links it), utest `fswatch_*` and `port_order`
([TESTING](TESTING.md#file-watching-inotify-plan)). No service serves
`fs.watch` yet: every `fs` server answers the generated
`ERR_NOT_SUPPORTED`.

**What changed from the plan.** The bell is not `fs.watch`'s argument
(2.4: a kept protocol's request carries no handle) but the watcher
channel's first message, `fswatch_bell.give`. `fswatch.add`'s mask is
checked as Linux checks it (`ERR_INVALID_ARGS` only for no `IN_*` bit at
all, or `IN_MASK_ADD` with `IN_MASK_CREATE`; other bits ignored; a mask
of flags alone is a watch that never fires). The overflow's own record
counts in the queue's bytes (it may pass `FSWATCH_QUEUE_BYTES` by its 14).
`IN_IGNORED` obeys the queue's room like any event (Linux's way): after an
overflow a client may miss one, and an id it still holds can then come
back from an `add` with `existed` 0, meaning the old watch is gone.

**For IE1 (extfs).**
- *Channels.* `fs.watch`: `jam_channel_create`, `fsw_chan_new(w, pid,
  view flags, &root key, server end, &c)`, the client end as the reply's
  handle; bind the server end to the port. A packet for a channel
  without its bell (`!fsw_belled`): `fsw_chan_bell`; `ERR_SHOULD_WAIT`:
  wait on; anything else but OK: `fsw_chan_close`. With the bell, its
  requests go through the kept slot as `file`'s do: `add` walks the path
  in the channel's view (`IN_DONT_FOLLOW` as `FS_NOFOLLOW`; call
  `fsw_mask_check` before the walk, as Linux checks the mask before the
  path), checks the read bit, then `fsw_add`; `remove` and `take` are
  `fsw_remove` and `fsw_take` as they are. Peer closed: `fsw_chan_close`.
- *The pid* for the per-process cap: fs requests are read through
  `svcstate_take` (user/lib/svcstate.c), which doesn't ask the kernel for
  the writer's pid (`CHANNEL_READ_PID`). Until a slot read keeps it, pass
  0: no process's share is counted, the volume's 32 still hold.
- *Events*, 2.2's rows: `fsw_event(w, &dir, name, len, &self, mask,
  flags)` where both sides get the same bit (`IN_MODIFY`, `IN_ATTRIB`,
  `IN_OPEN`, `IN_ACCESS`, the closes); `fsw_dir_event` for a new or
  removed name (`IN_CREATE`, `IN_DELETE`) and a move's pair with one
  `fsw_cookie(w)`; `fsw_self_event` for the inode's side alone (`link`'s
  and `unlink`'s `IN_ATTRIB`, `IN_MOVE_SELF`); `fsw_gone(w, &key)` when the
  inode goes (`IN_DELETE_SELF`, `IN_IGNORED`). `FSW_EV_DIR` for a
  directory (the library adds `IN_ISDIR`), `FSW_EV_UNLINKED` for an open
  file whose name is gone (`IN_EXCL_UNLINK`). Each is one test while
  `fsw_watching(w)` is false: call them freely.
- *Order* (2.3): a `struct fsw_order` with room for every binding the
  port can hold; `fsw_order_next` in place of the port wait; before
  serving a request while `fsw_watching` (or one on a watcher channel),
  `fsw_order_drain`, then `fsw_order_pick` each kept packet of a file
  channel that is closed, judged by the channel's signals
  (`jam_object_wait_one(ch, SIG_PEER_CLOSED, 0, NULL)`), not only the
  packet's: a close folded into an earlier readable packet keeps that
  packet's signals (`port_order`'s third case). Serve that file's
  remaining requests and its close, then the request.

**For IE2.** `struct fsw_table` (about 100 KiB: 4,096 watches of 24 bytes,
32 channel records of 40) is what goes in the state VMO; `keep[2]` in a
channel's record is the service's (its keeper's places for the channel
and the bell). A successor: `fsw_adopt` (false: refused and set up
empty: close every watcher channel kept), then per live channel
`fsw_chan_handles(w, c, ch, bell)` from the keeper, or `fsw_chan_close`
when the keeper has no bell for a channel whose record says `belled`
(the dead instance read the give and died before keeping it; a channel
whose record says 0 reads its give next). The hidden-name filter is the
`hidden` hook in `struct fsw` (asked for directory events on channels
whose view has flags); Q2's reach check walks the watches itself
(`fsw_each`, or `t->w` for the descendants of a moved directory) and ends
the ones out of reach with `fsw_end(w, slot, true)`.

**For IL1 (libc).** One event per inotify descriptor is the bell of
every watcher channel it opens (one per mount it watches): after
`fs.watch`, `fswatch_bell_give(watcher, a duplicate with RIGHT_SIGNAL |
RIGHT_TRANSFER)`, then the adds. Clear `FSWATCH_BELL` before taking,
never after, and take each channel until `more` is 0 (or set the bell
again). A record's `id` with its channel is libc's `wd`; id 0 is
`IN_Q_OVERFLOW` (`wd` -1); `IN_UNMOUNT` and `IN_IGNORED` for every watch
of a channel that closed are libc's own. `remove` of an id that isn't a
watch answers OK: `EINVAL` for a `wd` libc doesn't hold is libc's.
`FSWATCH_IN_*` equal Linux's `IN_*` (static asserts against musl's
header are cheap).

### IL1 notes

Built (2026-10-18): user/posix/sys/inotify.c (the kind, `inotify_ops` in
kinds.h), the four calls in calls.def and calls.h, posixtest's `inotify`
section (user/posix/test/inotify.c, and its `--child-inotify` mode) and
tools/shell-tests/posix-inotify.txt
([TESTING](TESTING.md#file-watching-inotify-plan)). The section runs
against fake services posixtest mounts in its own namespace (one keeping
watches with `<fswatch.h>`, one read-only and one writable without them),
since no real service serves `fs.watch` before IE1.

**What changed from the plan.**
- The bell has two bits: the services' `FSWATCH_BELL`, and libc's own
  `INO_HELD` (bit 25), up while the buffer holds records no read took.
  Both are the arm's readable bits: a `poll` in one thread can't sleep
  through records another thread's take left in the buffer, and a read
  that empties the buffer clears `INO_HELD` without clearing a ring of
  the services'. The kind's `ready` says the same as `INO_HELD`.
- A read of 0 bytes is 0 at once even with records held (Linux:
  `EINVAL`): an event coming between Chromium's `FIONREAD` of 0 and its
  read of 0 bytes must not end its reader. `write` is `EBADF` (3.3).
- enosysnames.c is unchanged: it is musl's whole table of names, for the
  log (eventfd's and epoll's are there too), not the calls left `ENOSYS`.
- A channel that closed (the mount went, or a successor closed it) is
  seen at the next take (a read, `FIONREAD`), by an add or a removal on
  it, and by a blocked read, which looks every second; `poll` and `epoll`
  wait on the bell alone and see it only at their next wake. Its wds get
  `IN_UNMOUNT` and `IN_IGNORED`, and the next add on the mount asks
  `fs.watch` again (posixtest checks both).
- A take goes channel by channel, so two mounts' records are not
  interleaved in time (no service knows another's order).
- A removed wd waits for its `IN_IGNORED`; one lost to an overflow keeps
  the wd held until its id comes back from an add with `existed` 0 (then
  libc makes the `IN_IGNORED` and gives a new wd).
- Limits for now: 8,192 wds per descriptor and 32 mounts' channels
  (one no watch uses is reused), `ENOSPC` past them; `fs.watch`'s
  `ERR_NO_RESOURCES` is `ENOSPC` too.

**For IL2.** fork's and exec's steps are done (`fd_fork_absent`, no
`give`), and posixtest checks a forked child and a spawned one without
`FD_CLOEXEC`; IL2's cases go in forkfd.c and exec.c as planned.
`/proc/self/fd/N` still names the kind as procfs names any. The 3.5
limits are IL2's (`INO_WATCHES` is the per-descriptor cap now).

**For IE1 and the run on ext.** posixtest's `real_events` runs on `/data`
and `/tmp` as soon as their service answers `fs.watch`: Chromium's mask
on a new directory, then a create, a write and close, a rename, a
`chmod`, an `unlink` and the directory's removal, expected in that order
(other records between them allowed), the rename's two halves with one
non-zero cookie. Until then the section prints `no watches from its
service yet` for each and goes on. After IE1 merges: `posix-inotify.txt`
and the init run on ext.

### IE1 notes

Built (2026-10-18): extfs serves `fs.watch` (user/services/extfs/watch.c),
2.2's rows at their requests (fsops.c, files.c), `IN_DELETE_SELF` when an
orphan goes (request.c's defer_run), and the close rule (main.c's
order_closes); utest `extfs_watch_*`
([TESTING](TESTING.md#file-watching-inotify-plan)). Also svcstate's slot
keeps its request's writer pid (`svcstate_read`, `svcstate_pid`:
`CHANNEL_READ_PID`; `SVCSTATE_VERSION` 3), which extfs's fs requests are
read with, so `FSW_PER_PROCESS` counts real processes.

**What held differently from the plan, or from Linux.**
- *Linux's events, checked against its source* (fsnotify.h, namei.c,
  inotify_fsnotify.c): a replaced rename target gets `IN_ATTRIB` (a
  directory too); `RENAME_EXCHANGE` is two fsnotify_move calls, so two
  pairs with a cookie each, each followed by its inode's `IN_MOVE_SELF`; a
  link's `IN_ATTRIB` and an unlink's come before the name's event;
  `IN_ISDIR` is never on `IN_DELETE_SELF` or `IN_MOVE_SELF` (inotify masks
  it), which the library's end_watch set: fixed there (fswatch_rules
  expects it). 2.2's text says so now.
- *Open then truncate*: `fs.open` with `FS_TRUNCATE` of a file that was
  there makes `IN_OPEN` then `IN_MODIFY`, also for an empty file (Linux
  since 6.10's vfs_open; before it the other order).
- *Not made, where Linux makes them*: Linux's getdents makes `IN_ACCESS`
  (with `IN_ISDIR`) on the directory, and opendir/closedir `IN_OPEN` and
  `IN_CLOSE_NOWRITE`; extfs has no directory handles (readdir is by path),
  so the table's "readdir: nothing" stands until DIRHANDLE-PLAN's handles
  give it an open and a close. A path that doesn't end in a name (`.`,
  `..`, a view's root) gives the inode's own event only, no directory
  event (Linux's dentry has a parent there).
- *A removed watch's queued events* were dropped by the library's
  `fsw_remove` (IP's choice); Linux leaves them in the queue before
  `IN_IGNORED`. The main session's call: Linux's way; IE2 changed the
  library (its events stay, `IN_IGNORED` after them).
- *The close rule's own order*: before a file's request the rule runs only
  when that channel has a message queued; a packet that is the file's own
  close is handled in the port's order (running the rule there handled a
  later close first: `extfs_watch_order` caught it). A watcher channel's
  close is not in the rule; `fs.watch` refused for the process's share
  first ends every watcher channel whose client has closed it, then tries
  again.
- *Events wait for the commit*: a request's events are held (at most 8,
  an exchange makes 6) until its transaction commits and dropped if it is
  given up; a close's and an orphan's going come outside requests, at
  once. The open files' renamed and unlinked names are moved at the
  request, not at its commit (a failed commit is a volume going read-only).
- *Restarts*: the table is the instance's memory; the watcher channels die
  with it (libc's `IN_UNMOUNT`, `IN_IGNORED`). An `fs.watch` the successor
  finds committed and not answered is answered from its slot with no
  handle (adopt.c's reply_again knows no `XF_MADE_*` for it): IE2's.

**For IE2.** watch.c's `table` goes into the state VMO (`XF_STATE_LAYOUT`
bump) and `wc[]`'s view copy with it (flags, root inode and generation,
the root's place: today memory only, 32 x 1 KiB); the channels and bells to
the keeper; `XF_CHAN_WATCH(slot, gen)` ids are in the slots already, so
adopt.c's chan_of needs a case for them, and `fs.watch` an `XF_MADE_*`
kind so reply_again makes the client's end again. The hidden-name hook
(`w.hidden`) is unset. files.c's `at[]` (each open file's directory and
name) stays memory only, as 2.8 says.

**For IL1.** extfs answers `fs.watch` on every view and subview, a
read-only volume's too; `add` with no `IN_*` bit is `ERR_INVALID_ARGS`, a
link out `ERR_LINK_OUT` (resolve and add on the other mount, as other
calls), no read bit `ERR_ACCESS_DENIED`, `IN_ONLYDIR` or a trailing `/` on
a file `ERR_NOT_DIR`; a ninth channel of one process `ERR_NO_RESOURCES`.
A close and an inode's going are queued before any later request of
yours is served, so a take after a `close` sees its event.

## 6. Tests

| What | Where |
|---|---|
| Queues, merging, overflow, ids, the bell, the caps | utest `fswatch_` (IP) |
| The port's order the close rule needs | utest `port_order` (IP) |
| extfs's events, order, views, rights, reach | utest `extfs_watch`; `tools/extfs-svc-test.sh` |
| extfs restarts | utest `extfs_restart_watch` |
| Views of one volume on a drive boot | `tools/layout-test.sh` drive |
| The Linux API through libc | posixtest `inotify` (tools/shell-tests' posix-inotify.txt), `procfs`, `fork` |
| FAT (with IF) | utest `fat_watch`; posixtest `inotify` with `JAMOS_DATA_FS=fat` |
| Chromium | `tools/cr-test.sh` `base` with `CR_BASE_ONLY='FilePathWatcher*'` |

## 7. Questions for the owner

**Answered (owner, 2026-10-17), all three as recommended:** Q1 (B), FAT
watches later: no track IF, fat answers `fs.watch` `ERR_NOT_SUPPORTED`,
and libc refuses a watch on a writable FAT mount (`EOPNOTSUPP`). Q2 (A),
a watch whose inode leaves the watcher's view ends (`IN_MOVE_SELF` if
asked, then `IN_IGNORED`). Q3 (A), the descriptor as `epoll`'s: absent in
a forked child, closed at `exec`, not given to a spawned child.

Decided here as technical (by the reasons above unless the owner says
otherwise): events made by the services, one watcher channel per view,
watches on inodes, the bell, libc's cyclic `wd`s, quiet watches where
nothing changes, the close-first rule, random cookie starts, Linux's
merge rule, the limits of 2.5 and 3.5, watches kept across a restart
with an overflow, a 0-byte `read` that doesn't wait, no kernel change.

### Q1. Watches on FAT volumes

- **(A)** Build them now (track IF, an hour): a stick's FAT `/data` gets
  events like ext.
- **(B)** Later: fat answers `ERR_NOT_SUPPORTED`, and a watch on a
  writable FAT mount is refused (`EOPNOTSUPP`). Chromium's watcher then
  stops at the first FAT component and never fires there (its `Watch`
  still says yes).

*Recommendation: (B).* Drive boot first and ports by need: Chromium's
tests run on ext, and a drive boot's `/home`, `/tmp` and `/jam` are ext.
A FAT `/data` matters only for Chromium run from a stick with its profile
there; IF is ready to run when that comes. **Answered: (B).**

### Q2. A watch whose inode leaves the watcher's view

Someone with a wider view renames a watched directory (or a directory
holding a watched one) to a place the watcher's view can't reach: above
its subview's root, or into a hidden `etc/ssh`.

- **(A)** The watch ends there (`IN_MOVE_SELF` if asked, then
  `IN_IGNORED`): a view never shows what it can't reach.
- **(B)** Linux's way: the watch follows the inode wherever it goes. A
  watched directory then keeps naming its entries to a watcher that can
  no longer list it (a watched file: what changes and when, no names).
  No check at renames.

*Recommendation: (A).* The check runs only at a rename that moves a
directory or a watched file to another directory, and only for watches
through narrower views; Chromium adds its watches again after any move
on the path anyway. Whose: the owner's (what a view promises).
**Answered: (A).**

### Q3. The descriptor across `fork` and `exec`

- **(A)** As `epoll`: absent in a forked child, not given to a spawned
  child or across `execve`.
- **(B)** Linux's way: the child shares the queue (duplicates of the bell
  and the channels). Each process's `wd` table is its own memory, so after
  the fork a watch added in one is unknown to the other: its events,
  taken by the wrong process, are dropped. Keeping it across `execve`
  needs a new hand-over kind with the table.

*Recommendation: (A).* Chromium's children close it, and (B)'s split
tables are a worse difference than a missing descriptor, which fails
visibly (`EBADF`). **Answered: (A).**
