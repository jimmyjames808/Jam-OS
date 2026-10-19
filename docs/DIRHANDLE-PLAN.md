# DIRHANDLE plan: directories as channels in the file protocol

A directory description in the POSIX layer is its absolute path as text
(`user/posix/sys/files.h`, `struct path`, `PATH_BYTES` 4096), and so is
the current directory (`user/posix/sys/path.c`). The `fs` protocol takes
paths of at most 4,095 bytes from the view's root. So Chromium's
`FileUtilTest.DeleteDeep` fails: it makes 26 levels of 255-byte names
through `mkdirat` and `openat` on each level's descriptor (about 6.6 KB
deep), then deletes them through `fstatat`, `openat`, `fdopendir` and
`unlinkat` on descriptors only; the 16th level is `ENAMETOOLONG` here
([CR-PLAN's CBF6 notes](CR-PLAN.md#cbf6-notes-2026-10-17)). Text costs
elsewhere too: `chdir("..")` out of a removed directory fails (Linux's
works), a directory made again at the same path counts as the one that
was removed, a renamed open directory's descriptor follows the old name,
and `struct path` (4,120 bytes) sits on stacks beside a second 4 KiB
buffer (CCS's clone-child finding). The owner: "fix everything". This
plan is the design. Built so far: the protocol and the keeper's room
(track DP, [its notes](#dp-notes)), and extfs's checks on a region
instead of a place's text (track DE0, [its notes](#de0-notes)); no
service serves a directory channel yet.

**In one paragraph.** A directory becomes a channel of the `fs` protocol
whose paths start at that directory: a new `fs.opendir` gives one, every
`fs` method works on it, and the walk in the service starts at the
directory's inode, so a call sends one relative path (at most 4,095
bytes, as Linux's `PATH_MAX`) however deep the directory lies. `..`
climbs from it up to the view's root and never above; the channel keeps
the view it came from (flags and root), so a directory handle never sees
or changes more than a path through its mount could. Paths that start
with `/` still start at the view's root, so the mount's own channel is
the root's directory channel and nothing that sends whole paths today
changes. extfs pins a directory while a channel holds it: removed, it
stays an orphan whose `..` is the parent it had, renamed, its channels
follow it. The keeper and the state VMO carry the channels across a
restart as they carry views. In libc a directory description and the
current directory hold the channel; the *at calls send the program's
relative path on it, `getcwd` asks the service where the directory is,
`fork` duplicates the channel and `exec` hands it over (a new descriptor
kind and, recommended, `POSIXFDS_VERSION` 3 for the current directory).
FAT, the boot image and libc's own trees keep text descriptions. The
kernel changes nothing.

## What this plan is not

- **No new protocol id.** A directory channel speaks `fs` (16); the plan
  adds `fs` methods 18 to 20 after IP's 17 (`fs.watch`).
- **No directory channels on FAT yet** (Q3, recommended: later), on the
  boot image, on the tests' ramfs, or in libc's own trees (`/`, `/svc`,
  `/dev`, `/etc`, `/proc`, `/sys`): those keep today's text descriptions.
- **No `openat2`** (`RESOLVE_BENEATH` and the rest stay `ENOSYS`);
  `fs.subview` stays the way to confine a program to a subtree.
- **No inotify events for directory opens** (`IN_OPEN` with `IN_ISDIR`
  and the rest): Chromium's watcher asks none.
- **No `file.clone`.** A file opened relative to a directory is reopened
  by name on that directory's channel (4.5), not shared.
- **Not the layer heap.** `struct path`'s own 4 KiB buffer (the
  program's string, copied in) moves with the layer heap (track LH, the
  owner's D11); this plan removes the second buffer beside it (4.8).

## Where it starts

| Piece | Built by | What this plan does with it |
|---|---|---|
| The `fs` protocol, `kept`: a successor runs the request in progress again from its slot, so no request carries a handle (`tools/genidl.py`) | M12, M13.6 | three methods; a second directory named by its id, not its handle (2.4) |
| Views and subviews, judged where a path resolves (`user/lib/fsview.c`, `user/services/extfs/walk.c`) | M13.6 | a directory channel keeps its view |
| extfs's walk: `..` physically, links inside the view followed, `ERR_LINK_OUT` and `fs.resolve` for the rest | M13.6 | starts at a directory; the view checks on a region, not the text (3.2) |
| The keeper and the state VMO (`user/services/extfs/adopt.c`, `user/lib/keep.c`) | M12, M13.6 | directory channels kept; the keeper's handle count raised |
| Open unlinked files held as orphans until their last close | M13.6 | directories held the same way |
| libc's directory kind (`user/posix/sys/dir.c`) and current directory (`user/posix/sys/path.c`), both text | M13, CBF6 | the channel, the text kept as a hint |
| A kind's fork hook, the exec hand-over (`user/lib/fdmsg.c`, `user/include/posixfds.h`) | FL2, FL3 | the channel duplicated; a new kind; the current directory (Q4) |
| A message's writer pid (`CHANNEL_READ_PID`) | CBF2a | a per-process share of directory channels |
| posixtest `files2.c` `deep_names` and `current_dir` | CBF6 | grown into the new section's cases |
| `fs.watch` (method 17) and fswatch's ids 49 and 52; IE2's reach check at a rename (Q2 of the inotify plan) | IP (running), IE2 (planned) | numbers after them; the same rule for a directory that leaves its view |

## 1. What POSIX and Chromium need

### 1.1 The calls

| Call | Linux | Jam OS today | With this plan |
|---|---|---|---|
| `openat`, `mkdirat`, `unlinkat`, `fstatat`, `statx`, `faccessat`, `fchmodat`, `fchownat`, `utimensat`, `readlinkat`, `symlinkat` with a directory descriptor | the relative path walked from the directory's inode; at most 4,095 bytes per call, no limit on depth | the descriptor's text and the path joined, then sent from the root: 4,095 bytes in all (`ENAMETOOLONG` at DeleteDeep's 16th level) | the program's relative path sent on the directory's channel, walked from its inode |
| `AT_EMPTY_PATH` on a directory | the directory itself | its text | `""` on its channel |
| `renameat2`, `linkat` with two descriptors | two inodes | two joined texts, one call | one call: `fs.rename_at` / `fs.link_at`, the second directory by its id (2.4) |
| `fdopendir`, `getdents64` | the descriptor | `fs.readdir` of its text | `fs.readdir("")` on its channel |
| `fchdir` | the current directory becomes the inode | ... becomes the text | ... becomes a duplicate of the channel |
| `getcwd` | the dentries walked up; `ENOENT` if removed, `ERANGE` for a short buffer, `ENAMETOOLONG` past a page | the text, and one call asking whether it is still there (CBF6) | one call: `fs.resolve("")` on the channel (4.4) |
| `..` from a removed directory | the parent it had | `ENOENT` (the text walks through the removed name) | the parent remembered at the removal |
| `..` from a renamed directory | its new parent | the old text's | ext's own `..` entry: the new parent |
| a directory removed and made again at the same path | the descriptor stays on the removed one (`ENOENT` to create in it, `getdents` `ENOENT`) | the new one counts | the removed one, as Linux |
| a rename of an open directory | its descriptors follow | not followed | followed |
| `O_PATH` | names without opening: `fchdir`, `fstat`, *at, no `getdents` (`EBADF`) | a text description that lists nothing | `fs.opendir` without `FS_READ`: no read bit asked; `getdents` `EBADF` |
| depth | none for descriptor walks; `getcwd` and `/proc/self/fd` names `ENAMETOOLONG` past a page | 4,095 bytes from the mount | as Linux |

### 1.2 What Chromium uses

Chromium 155.0.8059.39, fetched from chromium.googlesource.com for this
plan:

- **Every recursive delete.** `DoDeleteFile`
  (base/files/file_util_posix.cc:308-372), behind `DeletePathRecursively`
  and so behind every `ScopedTempDir`'s end, does `fstatat(at_fd, name,
  AT_SYMLINK_NOFOLLOW)`, then `unlinkat(at_fd, name, 0)` for anything but
  a directory, or `openat(at_fd, name, O_RDONLY | O_DIRECTORY |
  O_NOFOLLOW | O_CLOEXEC)`, `fdopendir`, `readdir` and a recursive call
  on each entry with the new descriptor, then `unlinkat(at_fd, name,
  AT_REMOVEDIR)`. One descriptor per level, all open at once: 27 for
  DeleteDeep. It also removes entries while it lists them (the listing
  must not skip any: ext's cookies are offsets that a removal doesn't
  move, *unsure* for a hashed directory).
- **The current directory.** `GetCurrentDirectory` (`getcwd`, and its
  `ENOENT`, file_util_posix.cc:1344) and `SetCurrentDirectory` (`chdir`).
- **The tests.** `FileUtilTest.DeleteDeep`
  (base/files/file_util_unittest.cc:1766): `mkdirat` and `openat`
  (`O_DIRECTORY | O_CLOEXEC`) 26 times with `NAME_MAX` names, a
  `symlinkat("..", fd, "up")` at the bottom, `openat(O_WRONLY | O_CREAT
  | O_EXCL)` of a file there and a write, then `DeletePathRecursively`
  and `PathExists`. `MakeAbsoluteFilePathNoResolveSymbolicLinks` (:349)
  removes its working directory and wants `getcwd` to fail: it passes
  since CBF6 and stays right.
- **Not used by base:** `O_PATH`, `fchdir`, `renameat` with two
  descriptors (Chromium's Linux sandbox uses some; it isn't built here,
  *unsure* of other components).

## 2. The protocol

### 2.1 A directory is an `fs` channel

- A path that starts with `/` is walked from the view's root, as today;
  any other from the channel's directory. On a mount's channel and on a
  view's the directory is the view's root, so every path any client
  sends today means what it meant.
- `""` names the directory itself (`getattr`, `readdir`, `resolve`; `.`
  as before). `..` climbs to the directory's parent, up to the view's
  root; a `..` above that root is `ERR_LINK_OUT` with
  `FS_RESOLVE_ABOVE`, as today, and libc goes on from the directory the
  mount point is in.
- Every `fs` method works on a directory channel, but `view`, `subview`
  and `watch` (`ERR_NOT_SUPPORTED`: libc asks them on the mount's
  channel; a later need can define them).
- **Why not a new protocol** (a dir.idl): the directory needs exactly
  the `fs` methods, libc's call sites already take a channel and a path,
  and every `fs` server answers a method it lacks with
  `ERR_NOT_SUPPORTED`, so fat, bootfs and ramfs build unchanged. A
  second protocol would repeat 16 methods, their stubs and their rules.
- **Why not numbers on the mount's channel** (`opendir` giving a token
  that later calls name): a service learns that a channel ended, never
  that a token's holder did, so tokens leak with a process that dies;
  `fork` and spawn share handles, not numbers.

### 2.2 The IDL

```
# fs.idl, the header's path rule (replacing "libos's namespace sends a path
# here with the mount point stripped" with):
# A path that starts with '/' is walked from the view's root (libos sends
# the path after the mount point: "/logs/boot-0001.txt" for
# /data/logs/boot-0001.txt); any other from the channel's directory: an
# fs.opendir channel's, the view's root on every other channel. "" is
# that directory itself.
const FEAT_DIRS 32     # statfs: fs.opendir works (FEAT_MASK becomes 63)

# A directory channel for `path` (ERR_NOT_DIR: not a directory): an fs
# channel whose paths start at that directory (the header's rule), its
# `..` climbing to the view's root. It keeps this channel's view (flags and
# root, a copy: this channel may close first). flags: FS_READ 1 (the read
# bit is asked, as an open to read; without it only the walk's search bits:
# O_PATH, chdir), FS_NOFOLLOW 128 (a link as the last name is an error,
# ERR_INVALID_ARGS). `id` is getattr's; `volume` names the volume, the
# same through every view of it and another for any other volume (ext's
# UUID folded to 64 bits), so libc can tell whether two directories can
# meet in one rename_at or link_at. The directory stays while the
# channel lives: removed, its channel answers "" and ".." and nothing else
# (ERR_NOT_FOUND); renamed, the channel follows it; moved where this view
# can't reach, ERR_NOT_FOUND to everything. view, subview and watch on a
# directory channel: ERR_NOT_SUPPORTED. ERR_NO_RESOURCES: the volume's
# directory channels are all in use, or the caller's process has its
# share. fat and the boot image: ERR_NOT_SUPPORTED.
18 opendir   (str[<=4095] path, u32 flags) -> (handle dir, u64 id, u64 volume)
# rename and link with `to` walked from another directory: `to_dir`, its
# id, a directory of this volume that this channel's view reaches (judged
# as if walked from the view's root; ERR_NOT_FOUND if it doesn't, or it
# was removed, or it isn't a directory). flags and errors as rename's and
# link's. fat and the boot image: ERR_NOT_SUPPORTED.
19 rename_at (str[<=4095] from, u64 to_dir, str[<=4095] to, u32 flags) -> ()
20 link_at   (str[<=4095] from, u64 to_dir, str[<=4095] to) -> ()
```

The restart paragraph adds: a directory channel's restart is not seen,
as a view's. The ABI stamp (`tools/portslib/pkgformat.py`, every .idl and
`user/include/posixfds.h`) changes with these, as it does with IP's
method 17: packages are made again, as after any protocol change.

### 2.3 Rights: the view it came from

A directory channel reaches what its view reaches (Q1): `..` climbs to
the view's root and stops there; the view's flags (read-only, the
guards, the hidden names) are judged where each path resolves, as for a
whole path; a channel opened from another keeps that one's view, so it is
never wider. Giving the descriptor to another process (`fork`, spawn,
`SCM_RIGHTS`) gives that view's reach, as a Linux descriptor gives the
whole tree through `..`. A directory moved out of its channel's view by
someone with a wider one (above a subview's root, into a hidden
`etc/ssh`) cuts the channel off: every request `ERR_NOT_FOUND` (Q2, as
the inotify plan's watches). The read bit: `opendir` with `FS_READ` asks
it, and `readdir` asks it again at each batch, as today.

The shared view code (`fs_view_dispatch`) checks paths lexically from
the view's root before the service walks them; on a directory channel a
relative `etc` is not the volume's, so a directory channel's dispatch
gets only `FS_VIEW_READ_ONLY` (as `views_lexical` already gives every
subview), and extfs's own checks, which judge the place a path reaches,
are the ones that count (3.2).

### 2.4 Two directories in one request

`renameat2(a, "x", b, "y")` with two directory channels needs the second
directory in the request, and a `kept` protocol's request carries no
handle. So `rename_at` and `link_at` name it by its id (the inode number
`opendir` and `getattr` give). The service places it by climbing its
`..` entries: a climb that meets the view's root gives the directory's
region (3.2) and `to` is walked from it; one that reaches the volume's
root first, or a removed or non-directory id, is `ERR_NOT_FOUND`. This
grants nothing: any directory the view reaches can be named by a path
from the view's root; the id only replaces a path that may be too long to
write. An id means something only on its own volume, so libc sends
`rename_at` only when both directories are on the same mount (by name,
today's rule) and their `volume`s are the same (else `EXDEV`, Linux's
answer across mounts), and turns its `ERR_NOT_FOUND` into `EXDEV` when
the second directory is still there through its own channel (a
descriptor from a process whose view is another). A path from
`/` beside a directory descriptor is opened first as a directory channel
(its parent, through the caller's own mount), so both sides are channels
and each keeps its own view; a `/` path is never sent on a channel that
came from elsewhere, whose view's root may not be the caller's mount.

### 2.5 Restart

As views: each directory channel is put with the keeper in a slot of its
own before the request that made it commits; its record (the directory's
inode and generation, the view's flags and root, the parent remembered at
a removal, the opener's pid) is in the state VMO; a successor takes the
channels back, and `XF_MADE_*` makes a reply that never went out again.
Where each directory lies (its region and reach, 3.2) is not in the
state: a successor works it out again at each channel's first request. A
removed directory still held stays an orphan across the restart, as an
open unlinked file does. A fresh start (the state given up) closes the
channels: libc sees `ERR_PEER_CLOSED` and opens the directory again from
its last known text, once, if it has one (4.1); else `ENOENT`.

### 2.6 Cost and limits

- **The client:** one handle per directory description (a `dup` shares
  it; a forked child holds a duplicate); one more per file description
  opened relative to a directory channel (its reopen, 4.5); the
  description's record grows by a handle, the mount's name and the id.
- **The service, per channel:** one channel handle, one keeper handle, a
  record of about 48 bytes in the state VMO (*unsure*), and its place
  worked out in memory. Per volume `XF_DIRS` 256 (the channel ids' low
  byte holds a slot, as files' and views'), at most 64 from one process
  (the `opendir` request's writer pid). 256 records are about 12 KiB of
  the 1 MiB state.
- **The keeper:** extfs's tables need at most 640 slots and 896 handles
  of it today (`keeper.c`'s asserts); IE2 adds its watcher channels and
  bells (32 slots, 64 handles), this plan 256 and 256: 928 slots, 1,216
  handles. `keepch`'s `MAX_SLOTS` (1024) holds them; its `MAX_HANDLES`
  goes from 1024 to 2048. The keeper's table doesn't grow (each slot
  already has room for 4 handles); the keepers' jobs must hold the extra
  handles (*unsure* of their limits today: track DP checks). extfs's own
  job (`ext_limits`, 2,048 handles) holds its about 1,250.
- **Calls:** one per operation, as today; a relative path is sent on the
  description's channel under its reference, so the `ns_channel`
  duplicate and close of today's calls go away for it. Opening a
  directory without `O_DIRECTORY` costs one more (`fs.open` says
  `ERR_IS_DIR`, then `fs.opendir`); closing one is a handle close. The
  walk is shorter: DeleteDeep's calls walk one name each, not 26.
- **When the service is full** (`ERR_NO_RESOURCES`): libc makes a text
  description instead, as today, and logs it once per process (decided
  here: a busy volume falls back to today's behaviour, not to `EMFILE`
  at 256 directories for the whole system).

### 2.7 Whole paths stay

Nothing is duplicated: a whole path is a path that starts with `/`, on
any channel. libc keeps sending absolute paths on the mount's channel
(nothing to hold), and every call on FAT, the boot image and a text
description is today's.

## 3. The services

### 3.1 extfs: walking from a directory

`walk()` starts at the channel's directory unless the path starts with
`/`. Its stack of directories walked into stays as it is; at its start
(depth 0) a `..` is the current directory's parent: the view's root
answers `ERR_LINK_OUT` above, as today; any other directory's `..` is
ext's own `..` entry (which ext keeps right across renames), or for a
removed directory the parent remembered at its removal. A climb that
reaches the volume's root without passing the view's root means the
directory has left the view: `ERR_NOT_FOUND` (the backstop behind 3.2's
check). `fs.resolve`'s `FS_RESOLVE_INSIDE` answer (the place relative to
the view's root) is the walk's own names after the start's place, which
the service finds by climbing and naming each directory (a scan of the
parent for the child's inode; `ERR_NAME_TOO_LONG` past 4,095 bytes).
That answer is what `getcwd` and `/proc/self/fd` read (4.4, 4.6).

### 3.2 Where a directory lies: a region, not the text

The views' checks read `phys`, the names from the volume's root as text
(at most 4,095 bytes): `fs_view_hides`, `fs_view_in_etc`,
`fs_view_in_pkg`, `name_hidden`, readdir's `etc` filter and a subview's
root. They only ever look at the first two names. The walk keeps
instead a **region**: the depth below the volume's root and the class of
its first two names (the root; `etc` itself; in `etc`; `etc/ssh` and
below; `etc/authorized_keys`; `pkg` and below; anywhere else), pushed and
popped as `phys` was, exact at any depth. `<fsview.h>` gets the same
three questions asked of a region. `phys` stays only for
`fs.resolve`'s answer.

A directory channel keeps its directory's region and depth, found when
it is opened and again after a rename that could change it: a counter
(the epoch, in memory) goes up at every rename that moves a directory to
another parent or renames one in the volume's top two levels (where the
classes' names are), and a channel whose region is older climbs to the
volume's root once at its next request (one `..` lookup per level),
which also finds whether it still reaches its view's root (Q2). A
successor starts the epoch again, so every channel climbs once after a
restart.

### 3.3 Pinned directories

- **`rmdir` of a held directory** (`unlink` with `UNLINK_DIR`, or a
  rename that replaces an empty directory): its name goes and the
  parent's link count drops as today, but the inode isn't freed: it has
  no links and waits on ext's orphan list, as an open unlinked file does,
  until its last channel closes (`defer_drop` asks the directory table as
  `files_held` asks the files). Its records remember the parent it had.
  Its blocks are freed at once or at the drop (*unsure*: Linux's ext4
  frees them at the inode's end); it answers no name either way.
- **A removed directory's channel:** `""` and `.` (getattr with `nlink`
  0, `sync`, `statfs`); `..` the remembered parent (`ERR_NOT_FOUND` if
  that inode's generation changed); every other name, a creation too,
  `ERR_NOT_FOUND`; `readdir` `ERR_NOT_FOUND` (Linux's `getdents` says
  `ENOENT`).
- **A rename of a held directory** only moves the epoch: its channels
  follow its inode. `rename_at`'s "a directory into itself" check climbs
  from the target by inode (*unsure* whether `ext_rename`'s own check
  does already).
- **After a cut:** an orphan directory is on the journal's orphan list,
  which the next mount (`recover_orphans`) and e2fsck both finish.

### 3.4 fat

FatFs walks whole paths from the volume's root (`FAT_PATH_MAX` 4,096),
FAT has no inode to pin, and fat's views judge paths lexically from the
root. With Q3's (A) fat answers `opendir` with the generated
`ERR_NOT_SUPPORTED`, its `statfs` has no `FS_FEAT_DIRS`, and libc keeps
text descriptions there: 4,095 bytes of depth, renames not followed.
What fat could do later (track DF, *unsure* in its details): FatFs's
relative paths (`FF_FS_RPATH` 2 in `user/services/fat/ffport/ffconf.h`,
0 now) walk from the volume's current directory, a cluster number, and
`..` through FAT's own `..` entries; a channel would hold its
directory's first cluster and set it as the current directory for each
of its requests, `f_getcwd` would give `getcwd`'s text, a moved
directory keeps its cluster (FatFs rewrites its `..`), and a `rmdir` of
a held directory would mark its channels removed (the clusters are
freed at once: nothing can pin them). Hiding views would keep text
(their checks need the path from the root); exFAT's current directory
needs more than a cluster.

### 3.5 The boot image, the memory /tmp, ramfs

The boot image (bootfs, read-only, a few levels) keeps text. The memory
`/tmp` is extfs on a VMO (`user/services/init/tmp.c`), so it has
directory channels with extfs. The tests' ramfs answers
`ERR_NOT_SUPPORTED`: text.

## 4. libc

### 4.1 The description

`struct dir` gets the directory's channel (`HANDLE_INVALID` for a text
description), its mount's name (for the read-only and features caches,
`st_dev` and errors), its id and volume, and its last known absolute
text (a hint: NULL when too long or unknown; the text description's path
as today). A
directory on a mount whose `statfs` says `FS_FEAT_DIRS` gets a channel;
anything else stays text, and so does one the service has no room for
(2.6). A channel that answers `ERR_PEER_CLOSED` (the mount went, or a
fresh start) is replaced once by an open of the last known text through
the namespace; failing that the description answers `ENOENT`.

### 4.2 Paths from a directory

`struct path` gets `at`: a reference to the directory description a
relative path starts from (the descriptor's, or the current
directory's), NULL for a path from `/`. With `at` set, `abs` holds the
path as the program wrote it, with no join: the service walks it.
`path_fs` gives `at`'s channel (borrowed under the reference: no
`ns_channel`, nothing to close), `path_rel` the path as written, and a
new `path_done` drops the reference at the end of every call (DL0's
sweep). A link out of the mount (`PATH_LINK_OUT`) is asked on the same
channel and goes on from `/`, as today. The `fd_ops` `path` hook becomes
one that names a description as a `struct path` (its channel and `""`,
or a file's directory and name, or text), for meta.c's f* calls,
procfs.c's names and spawn's give.

### 4.3 The *at calls

| Call | On a directory channel |
|---|---|
| `openat` | `fs.open` of the path; a directory found (`ERR_IS_DIR`, or `O_DIRECTORY`, `O_PATH`, a trailing `/`) `fs.opendir` with `FS_READ` (none for `O_PATH`), `FS_NOFOLLOW` for `O_NOFOLLOW` |
| `mkdirat`, `symlinkat`, `readlinkat`, `fchmodat`, `fchownat`, `utimensat`, `fstatat`, `statx`, `faccessat` | the method with the relative path; `AT_EMPTY_PATH` with `""` |
| `unlinkat` | `fs.unlink`, `UNLINK_DIR` for `AT_REMOVEDIR` |
| `renameat2`, `linkat` | both sides from one channel: `fs.rename`, `fs.link` on it; two channels (an absolute side's parent opened as one first) on one mount and volume: `fs.rename_at`, `fs.link_at` with the second's id (2.4); else `EXDEV`; both absolute: today's |
| `fdopendir`, `getdents64` | `fs.readdir("")` with the description's cookie, as today; `.` and `..` from `getattr("")` and `getattr("..")` (dir.c's `id_of` and its 8 KiB of stack go) |
| `fsync`, `fstatfs`, `fstat` | `fs.sync`, `fs.statfs`, `fs.getattr("")` |

### 4.4 The current directory

The current directory is a directory description with a reference count
under `cwd_lock`. `chdir` opens the path as `openat` with `O_DIRECTORY`
would, without `FS_READ` (Linux's asks the search bit alone, which
neither today's `chdir` nor this one checks: the next walk from it does),
one call where today's makes two, and swaps it in; `fchdir` takes a
duplicate of the descriptor's channel. A relative path with `AT_FDCWD`
goes on the current directory's channel, so `chdir("..")` out of a
removed directory reaches the parent it had. `getcwd` is one call, as
CBF6's check is now: `fs.resolve("")` on the channel, the mount's name
joined with its answer (`FS_RESOLVE_INSIDE`); `ERR_NOT_FOUND` is
`ENOENT`, `ERR_NAME_TOO_LONG` `ENAMETOOLONG`, a short buffer `ERANGE`.
The answer refreshes the text hint. A text current directory works as
today.

### 4.5 fork, spawn and exec

- **fork:** the directory kind's fork hook duplicates the channel
  (`fd_fork_dup`); the child's description is the same directory, its
  listing position its own copy (FL2's rule for files). The current
  directory's channel goes in the fork stream under a key of its own.
- **Files opened relative to a directory channel** keep a duplicate of
  that channel and the name they were opened by, instead of a joined
  text: their reopen (FL2's first use in a forked child, spawn's give)
  opens the name on that channel. fobj's sharing by path is FAT's (no
  `FS_FEAT_MULTI_WRITER`), so a channel-relative open (ext only) never
  shares.
- **spawn and exec (FL3's in place too):** a new hand-over kind,
  `POSIXFDS_DIR_CHANNEL` 14: one handle (the channel); data: its id and
  volume (two u64s), the mount's name and its NUL, then the text hint or
  nothing, and its NUL. The current directory: Q4. Children of
  `/svc/exec` start in their starter's directory (the owner's D2, track
  SX: the hand-over's text, which `exec.open` carries); with Q4's (A)
  its channel goes the same way, if init's exec passes the hand-over's
  handles on as they are (*unsure* until SX lands).

### 4.6 /proc

`readlink` of `/proc/self/fd/<n>` for a directory and of
`/proc/self/cwd` is `getcwd`'s question asked of that channel; a removed
directory answers its last known text with " (deleted)", as Linux's
does; past `PATH_BYTES`, `ENAMETOOLONG`.

### 4.7 Where it differs from Linux

- The volume's directory channels are 256 at once (64 per process);
  past them a directory is a text description (2.6), not an error.
- A directory that leaves the channel's view is cut off (Q2).
- `getdents` on a removed directory: `ENOENT` from its first batch; libc
  doesn't list `.` and `..` before it either.
- FAT and the boot image: text, as today (Q3).
- After a fresh start of extfs a directory is opened again by its last
  known text (4.1); Linux has no such event.

### 4.8 Stack frames

Going: `path_get`'s `base[PATH_BYTES]` (a relative path is no longer
joined; a text base is copied into `out->abs` and the path resolved in
place), dir.c `id_of`'s buffer and its `struct path` (8 KiB), meta.c
`stat_at`'s `cwd[PATH_BYTES]` (the current directory's description
instead). An `openat` frame goes from about 8.2 KiB (`struct path` and
`base`) to about 4.1 KiB. Staying: `struct path`'s `abs` (the program's
string, copied in once), which moves to the layer heap with LH; until
then the clone child's 64 KiB layer stack and the signal stacks keep
their present margins.

## 5. The kernel

Nothing changes:

- **A directory channel is a channel.** Requests, replies, peer-closed
  and the handle limits exist.
- **The two-directory requests** need no handle in a request: the second
  directory goes by id (2.4), and genidl's no-handles rule for a `kept`
  protocol stays.
- **fork and exec** duplicate and move handles (`handle_duplicate`, the
  fork stream, the hand-over), which they do for every other kind.
- **Pinning** is the service's: an inode on its own orphan list.
- **The per-process share** reads `CHANNEL_READ_PID` (CBF2a).

Considered and dropped: a kernel object for a directory (a "dentry"):
the kernel knows no file system, and a service would still have to name
the inode in it.

## 6. Questions for the owner

**Decided 2026-10-18** (the owner delegated these decisions): Q1 (A) a
handle reaches its view, `..` up to the view's root; Q2 (A) a directory
moved out of its channel's view is cut off (`ENOENT`), as the inotify
plan's Q2; Q3 (A) FAT later, text descriptions there; Q4 (A)
`POSIXFDS_VERSION` 3 with the current directory's channel (static
programs relinked once, as after FL3).

Decided here as technical (by the reasons above unless the owner says
otherwise): directories as `fs` channels, not a new protocol; paths with
`/` from the view's root on every channel (whole paths stay); the second
directory of a rename or link by id with the reach check; the region in
place of the text in extfs's checks; the epoch; 256 channels a volume and
64 a process, a text description past them; a channel replaced from its
text once after a fresh start; `getcwd` asking the service each time;
files opened relative to a directory reopened by name on it; no kernel
change.

### Q1. How far a directory handle reaches

- **(A)** Its view: `..` climbs to the view's root, as a Linux
  descriptor's climbs to the root of the tree.
- **(B)** Its subtree: `..` stops at the directory (Fuchsia's handles).
  `openat(fd, "..")`, `chdir("..")` with the current directory as a
  channel, and every program that walks up from a descriptor would fail;
  `fs.subview` already gives (B) to whoever wants it.

*Recommendation: (A).* POSIX needs it, and a view is already the
boundary every path is judged by.

### Q2. A directory whose place leaves its channel's view

Someone with a wider view renames an open directory (or one above it) to
a place the channel's view can't reach: above a subview's root, or into a
hidden `etc/ssh`.

- **(A)** The channel is cut off: every request `ERR_NOT_FOUND`
  (`ENOENT`), found at its next request after the rename (3.2).
- **(B)** Linux's way: it keeps working wherever the directory went, and
  lists to the holder names its view can no longer reach.

*Recommendation: (A),* as the owner answered the inotify plan's Q2 for
watches: a view never shows what it can't reach. The check costs one
climb per channel per directory rename.

### Q3. Directory handles on FAT

- **(A)** Later: FAT keeps text descriptions (depth 4,095 bytes, renames
  not followed, a remade directory counts); track DF when a FAT `/data`
  needs more.
- **(B)** Now: DF (FatFs's relative paths, 3.4), an hour, *unsure* of
  exFAT.

*Recommendation: (A).* Drive boot first and ports by need: Chromium's
tests run on ext, and a drive boot's `/home`, `/tmp` and `/jam` are ext.

### Q4. The current directory across exec

- **(A)** `POSIXFDS_VERSION` 3: the first message carries the current
  directory's channel (a flag, and the first handle), its id and volume
  beside its text, so a deep, renamed or removed current directory
  survives `execve` and spawn as on Linux. A program linked before it
  gets no hand-over (no descriptors), as FL3's version 2 did: every
  static program relinked once (the stage's `base_unittests` already
  must be after any libc change), and the ABI stamp moves.
- **(B)** Version 2 kept: the starter asks its directory's place
  (`fs.resolve("")`) and sends the text. A directory the text can't name
  (deeper than 4,095 bytes, or removed) gives the child `/` (*unsure*
  which answer is least bad; Linux's child gets the same directory). The
  new descriptor kind 14 is refused whole by an old reader either way,
  but only when a directory descriptor crosses an exec.

*Recommendation: (A).* The rule is Linux's, the relink is routine, and
(B) leaves exactly the cases this plan exists for wrong at every exec.

## 7. Tracks

Each about an agent-hour. Gates name the tracks running or planned now:
**IP** (`abi/idl/fs.idl` method 17, running), **WE2** (extfs
`pager.c`, `main.c`, `fsops.c`, `test.c`, `extfs.h`, running), **IE1**
and **IE2** after them (extfs `fsops.c`, `files.c`, `main.c`,
`request.c`, `walk.c`, `views.c`, `state.c`, `keeper.c`, `adopt.c`,
`test.c`, `extfs.h`, a new watch.c), **IL1** (libc `kinds.h`,
`calls.def`, a new inotify.c) and **IL2** (libc `procfs.c`, posixtest
`procfs.c` and `forkfd.c`), **IC** (the only `base` runner while it
runs); the owner's queue of 10-18: **RL** (record locks, extfs and an
IDL, after IE2), **FF** (named pipes, extfs and libc, after RL), **LH**
(the layer heap: ~200 sites in 30 libc files, after SGR and FR2), **PR**
(other processes' `/proc`, `procfs.c`, after IL2) and **TP** (threads'
ids and `CLONE_FS`, libc threads and fork). extfs's files take one track
at a time, in the queue's order: WE2, IE1, IE2, RL, FF, then this plan's
DE0 to DE2. **Method numbers:** 18 to 20 are the next free ones after
IP's 17 today; if RL or FF adds `fs` methods first, DP takes the next
free ones after theirs (libc and extfs use the generated names, not the
numbers).

| Track | What | Files | Gate | Tests | Size | Model |
|---|---|---|---|---|---|---|
| **DL0. libc: a path's reference** | 4.2's mechanics with no change of behaviour: `struct path`'s `at` (always NULL yet) and `path_done` at every user; `path_get`'s base joined in place (no `base[PATH_BYTES]`); dir.c `id_of`'s buffers on the heap | `user/posix/sys/files.h`, `user/posix/sys/path.c` and every `struct path` user: `user/posix/sys/dir.c`, `user/posix/sys/file.c`, `user/posix/sys/meta.c`, `user/posix/sys/fobj.c`, `user/posix/sys/procfs.c` | none (CBF6 is in); not beside IL2, PR, FF's libc part or LH (same files) | posixtest's default sections and `files`; `tools/shell-tests/posix-files.txt`, posix-fork; shell scripts basic, cmds, commands, files; init run | 0.5 | Sonnet 5.5 |
| **DP. The protocol and the keeper's room** (built: [DP notes](#dp-notes)) | 2.2 and 2.6's keeper: `fs.opendir`, `rename_at`, `link_at`, `FEAT_DIRS` and the path rule in `fs.idl`; `keepch`'s `MAX_HANDLES` 2048; the keepers' job limits checked and raised | `abi/idl/fs.idl`, `abi/idl/keepch.idl`, the generated headers, `user/lib/keep.c` (its count), the keepers' limits (volumes', init's: *unsure* where), `user/tests/utest/keep.c` (a service past 1,024 handles) | IP merged (`fs.idl`); not beside RL or FF if they edit `fs.idl` | `make check` (genidl); utest `keep`; init run; shell scripts basic, cmds, commands, files and kdump save (an IDL changed) | 0.5 | **Opus** |
| **DE0. extfs: the region** (built: [DE0 notes](#de0-notes)) | 3.2's first half, no change of behaviour: the walk and the views' checks on a region, `<fsview.h>`'s region questions, readdir's filter and subview roots on it; `phys` kept for `fs.resolve` | `user/services/extfs/walk.c`, `user/services/extfs/fsops.c`, `user/services/extfs/views.c`, `user/services/extfs/extfs.h`, `user/lib/fsview.c` and `user/include/fsview.h` (functions added) | WE2, IE2, RL and FF merged (the queue's order); never beside another extfs track | utest `extfs` (its views and links cases unchanged); `tools/extfs-svc-test.sh`; `tools/layout-test.sh` drive; init run | 1 | **Opus** |
| **DE1. extfs: directory channels** | 2.1 to 2.4 and 3.1: `opendir`, the table (records, keeper slots, `XF_MADE_*`, port keys and channel ids, the per-process share), the walk from a directory and its `..`, `""`, the epoch and the cut-off, the dispatch's flags, `fs.resolve`'s place by climbing, `rename_at` and `link_at` with the reach check, `view`/`subview`/`watch` refused | a new user/services/extfs/dirs.c, `user/services/extfs/walk.c`, `user/services/extfs/fsops.c`, `user/services/extfs/views.c` (`views_lexical`), `user/services/extfs/request.c`, `user/services/extfs/main.c`, `user/services/extfs/extfs.h`, a new utest file user/tests/utest/extfs_dirs.c, `tools/shell-tests/extfs-svc.txt` (its count) | DE0 and DP merged | utest `extfs_dirs`: 26 levels of 255-byte names made, listed and removed through channels (no path over 300 bytes sent), a 4,095-byte relative path, `..` up to the view's root and `ERR_LINK_OUT` above it, links inside and out; each view flag through a channel (read-only, the guards, `HIDE_KEYS`, `NO_ETC`: `readdir("..")` at the root without `etc`); a channel opened from a channel keeps the narrower view; a subview's channel cut off when its directory moves above the root, a `HIDE_KEYS` one when moved into `etc/ssh`; `rename_at`/`link_at` (another directory, one the view doesn't reach refused, a removed one); 256 channels and the 257th `ERR_NO_RESOURCES`, 64 from one process; `tools/extfs-svc-test.sh`; init run | 1.5 | **Opus** |
| **DE2. extfs: removal and restarts** | 3.3 and 2.5: a held directory removed (orphan, `..` remembered, `ERR_NOT_FOUND` rules), replaced by a rename, renamed; orphan directories across a restart and at a mount; the records in the state (`XF_STATE_LAYOUT` bumped), adopt, a fresh start closing the channels; test powers that kill extfs between a `rmdir` of a held directory and its answer | user/services/extfs/dirs.c, `user/services/extfs/fsops.c`, `user/services/extfs/files.c` (`files_held` with the directories), `user/services/extfs/request.c` (`defer_drop`), `user/services/extfs/state.c`, `user/services/extfs/adopt.c`, `user/services/extfs/keeper.c`, `user/services/extfs/test.c`, `user/services/extfs/extfs.h`, `user/tests/utest/extfs_restart.c`, user/tests/utest/extfs_dirs.c | DE1 merged; beside DL1 | utest `extfs_dirs`' removal cases (made again at the same name: the channel stays on the removed one; `..` from a removed one and from one whose parent went too; a rename over a held empty directory), `extfs_restart` (killed at each point with channels open, deep ones too: the same directories after; a held removed directory across a restart; a fresh start: the channels closed); `tools/extfs-svc-test.sh`; `tools/extfs-test.sh` basic (e2fsck after a deep tree and held removed directories); `tools/layout-test.sh` drive; init run | 1 | **Opus** |
| **DL1. libc: descriptions and the *at calls** | 4.1 to 4.3: the channel in `struct dir`, `openat`'s directory by `fs.opendir`, `getdents`, `fstat`, `fsync`, `fstatfs`, the text fallback and the replacement after `ERR_PEER_CLOSED`, every *at call through `at`, `rename_at`/`link_at`, the `path` hook's replacement, files opened relative keeping their directory and name | `user/posix/sys/dir.c`, `user/posix/sys/path.c`, `user/posix/sys/files.h`, `user/posix/sys/file.c`, `user/posix/sys/meta.c`, `user/posix/sys/fobj.c`, `user/posix/sys/fd.h`, `user/posix/sys/spawnfd.c` (the hook), a new posixtest file user/posix/test/dirfd.c (section `dirfd`) and a new tools/shell-tests/posix-dirfd.txt, `user/posix/test/files2.c` (`deep_names`: 26 levels on ext, 15 on FAT) | DL0 and DE1 merged; beside DE2; not beside LH, PR or FF's libc part | posixtest `dirfd` on ext `/data`, on `/tmp`, and with `JAMOS_DATA_FS=fat` (today's answers): DeleteDeep copied (26 levels, `up`, the file, `DeletePathRecursively`'s calls); a rename of an open directory followed; a directory removed and made again; `renameat`/`linkat` across two descriptors; `O_PATH`; `AT_EMPTY_PATH`; 300 directories open (the fallback); posixtest `files`; posix-files; shell scripts basic, cmds, commands, files; init run | 1 | **Opus** |
| **DL2. libc: the current directory, fork, exec, /proc** | 4.4 to 4.6: the current directory as a description, `chdir`, `fchdir`, `getcwd`, `chdir("..")` out of a removed one; the fork hook and the current directory's fork key; kind 14 and Q4's hand-over; `/proc/self/fd` and `cwd` | `user/posix/sys/path.c`, `user/posix/sys/dir.c`, `user/posix/sys/fd.c` (fork keys), `user/posix/sys/procfs.c`, `user/posix/sys/spawn.c`, `user/include/posixfds.h`, `user/lib/fdmsg.c`, `user/posix/crt/start.c`, `user/tests/utest/posixfds.c`, `user/tests/utest/posixfds_bad.c`, `user/posix/test/forkfd.c`, user/posix/test/dirfd.c | DL1 and DE2 merged; IL1, IL2 and PR merged (kinds, `procfs.c`, `forkfd.c`); not beside TP or LH | utest `posixfds` (kind 14, the version's cases); posixtest `dirfd`'s current-directory cases (deep, renamed, removed: `getcwd`, `..`; `fchdir`; a forked child's descriptors and directory; an exec'd child's; `/proc/self/fd` names), `fork`, `procfs`; `tools/shell-tests/posix-fork.txt`, posix-run; shell scripts basic, cmds, commands, files; kdump save; init run; one ktest pass at 4 CPUs | 1 | **Opus** |
| **DC. Chromium's tests** | `FileUtilTest.DeleteDeep` off `tools/cr/base_unittests.skip` (and any `getcwd` entry CBF6 left), the stage relinked; what still fails listed with its reason | `tools/cr/base_unittests.skip`, `docs/CR-PLAN.md`'s rows | DL2 merged; no other agent running `base` | `CR_BASE_ONLY='FileUtilTest*:ScopedTempDir*:FileEnumerator*'` through `tools/cr-test.sh` on ext `/data` and once on `/tmp` | 0.5 | Sonnet 5.5 |
| **DR. Independent review and fix** | everything above, findings first (a new docs/DIRHANDLE-REVIEW.md), then High and Medium fixed with a test each: can a channel reach past its view (a climb, an id, a hidden `etc` through `..`, a cut-off missed after a restart), can channels or orphans leak (a process that dies, a fork, a failed exec), can a removed directory's inode be reused while held, is a hand-over's channel ever dropped or doubled | the files its fixes need | DC | its own; hostile: 8 threads renaming directories in and out of a subview while others walk through channels, extfs killed every 200 ms during a deep `DeletePathRecursively`, a process holding 64 channels killed, a directory removed while 4 processes stand in it; full init run; the extfs and posix scripts | 1.5 | **Opus** |
| **DJ. The join** | ARCHITECTURE (Storage: directory channels; the POSIX layer: the description), TESTING's rows, CR-PLAN, ROADMAP, HISTORY; this plan deleted when HISTORY covers it | the docs | DR | `make check` | 0.5 | Sonnet 5.5 |
| **DF. fat** (only with Q3's (B)) | 3.4 on fat: FatFs's relative paths, channels by cluster, `f_getcwd`, removal marks, its state and keeper | `user/services/fat/` (a new dirs.c, fsops, its loop, state, keeper, adopt), `user/services/fat/ffport/ffconf.h`, a new utest file user/tests/utest/fat_dirs.c | DE2 merged (the pattern) | utest `fat_dirs`, `fat_restart` unchanged; posixtest `dirfd` with `JAMOS_DATA_FS=fat` | 1 | **Opus** |

**Order.** DL0 can start now (before LH, which then moves fewer
buffers); DP when IP has merged. DE0 waits for extfs's queue (WE2, IE1,
IE2, RL, FF), then DE1; then DE2 and DL1 side by side; then DL2 (after
IL1, IL2 and PR too), DC, DR, DJ. **Total: 9 agent-hours** (10 with DF).
The critical path is extfs's queue; DE0 to DE2 after IE2 is the
recommended place (inotify clears 165 tests and this plan one, and DE1's
cut-off shares IE2's reach rule), RL and FF before or after them as the
main session likes.

### DP notes

Built (2026-10-18): `fs.opendir` (method 18), `fs.rename_at` (19) and
`fs.link_at` (20), the numbers 2.2 named (nothing had taken them),
`FS_FEAT_DIRS` 32 (`FS_FEAT_MASK` 63), the header's path rule and its
restart paragraph, and `view`, `subview` and `watch` saying
`ERR_NOT_SUPPORTED` on a directory channel; `keepch`'s `MAX_HANDLES`
2048; utest `keep_room` and `fs_no_dirs`
([TESTING](TESTING.md#the-fs-and-file-protocols-unix-calls-m136s-u5)). Every
`fs` server answers the three with the generated `ERR_NOT_SUPPORTED`
(fat, bootfs, ramfs unchanged; extfs too, until DE1). `FS_REQ_MAX` grew
from 8,206 to 8,214 bytes (`rename_at`'s id): the services' request
slots (`REQ_CAP_FS` in extfs's and fat's state.c) follow it and their
asserts hold.

**Decided in the contract text** (2.2 left these open):
- `rename_at`'s and `link_at`'s `to` follows the header's rule like
  every path: one that starts with `/` is walked from the view's root,
  `to_dir` checked all the same. No special case in the walk; libc never
  sends one (2.4).
- Q2's cut-off lasts while the directory lies out of reach: a channel
  whose directory is moved back works again. That grants nothing (a path
  from the view's root names it then), needs no flag in the state, and a
  successor's climb at each channel's first request (3.2) finds the same
  answer.
- `opendir`'s flags: `FS_READ` and `FS_NOFOLLOW`, any other
  `ERR_INVALID_ARGS`; without `FS_NOFOLLOW` a link as the last name is
  followed (as `open`).
- A removed directory's channel, in 3.3's words: getattr, sync and
  statfs on `""`, a walk through `..`, `ERR_NOT_FOUND` to the rest
  (readdir, other names, creations).

**The keepers and the jobs (checked).** The keepers are volumes' (one
`struct keeper` in each mount's `sup_kept`, `MAX_VOLS` 32: fat's and
extfs's) and init's (the mixer's); devmgr keeps nothing for its drivers.
`struct keeper` doesn't grow (`MAX_SLOTS` stays 1024, 4 handles a slot).
volumes' job is made by init (`start_volumes`) with no limits of its
own, so what its keepers hold is charged up to the root job's limits
(`userboot_root_job`: at most 16,384 handle units and 64 MiB of message
bytes, less on a small machine); init's own job is the root job. A full
extfs's 1,216 kept handles are as many again in volumes' table as in
extfs's. A restore of 2,048 handles is at most 34 messages of 61 to 64
handles on the new keep channel (`CHANNEL_MAX_QUEUED` 1024), about
2.1 MiB of volumes' message bytes until the successor reads them.
extfs's own job (`ext_limits`: 2,048 handles, 4 MiB queued) holds its
1,216 client handles and its own few dozen; fat's (`fat_limits`, 256)
doesn't change (Q3). Nothing needed raising but `MAX_HANDLES`.

**For DE1.**
- `user/lib/fsview.c` knows none of 18 to 20: `fs_view_check`'s switch
  and `hidden_request`'s table pass a method they don't list unchecked
  (harmless while no server has them). Add them: `opendir`'s path judged
  as a read (the hiding flags, as readdir's), `rename_at`'s and
  `link_at`'s `from` as rename's and link's (`FS_VIEW_READ_ONLY`, the
  guards); their `to` starts at `to_dir`, which the lexical check can't
  place, so extfs's region check judges it (3.2). (Done in DE0:
  [its notes](#de0-notes).)
- `user/services/extfs/keeper.c`'s handle assert: add the directory
  channels (and IE2's watchers); `KEEP_MAX_HANDLES` is 2048.
- The generated names: `struct fs_ops`' `.opendir`, `.rename_at`,
  `.link_at`; the client's `fs_opendir`, `fs_rename_at`, `fs_link_at`
  (and `_until`, `_within`, `_send`, `_result`).

**For DL1.** `FS_FEAT_DIRS` in statfs's features says `opendir` works
(path.c masks features with `FS_FEAT_MASK`, now 63, so the bit reaches
libc). A server without it answers all three `ERR_NOT_SUPPORTED`: keep
the text description there.

### DE0 notes

Built (2026-10-19), 3.2's first half, with no change of behaviour:
- **`<fsview.h>`'s region.** `struct fs_region`: the depth below the
  volume's root and one of seven classes of the first two names
  (`FS_REGION_ROOT`, `_ETC` for `etc` itself, `_ETC_REST`, `_ETC_SSH`,
  `_ETC_AUTH`, `_PKG`, `_OTHER`); `fs_region_child`, `fs_region_parent`
  and `fs_region_of` (a cleaned path's), and the three questions
  `fs_region_in_etc`, `fs_region_in_pkg`, `fs_region_hides`. The text
  questions (`fs_view_in_etc`, `_in_pkg`, `_in_keys`, `fs_view_hides`)
  now ask the same code of `fs_region_of(text)`, so the two can't drift.
  `etc/authorized_keys` and every name under it share a class: the
  depth tells the file (2) from a name under it (the text's "nothing
  after it" rule), so a `..` from under it gives the file back exactly.
- **extfs's walk** keeps `w->region` beside `phys` (`place_push`,
  `place_pop`, where `phys_push` and `phys_pop` were); `hidden_dir`,
  `name_hidden`, `walk_hidden`, `walk_may_change`, readdir's `etc`
  filter and `views_sub`'s root check ask the region. `phys` is read by
  `walk_inside` (fs.resolve's answer) only, and `place_push`'s 4,095-byte
  limit on it stays: it is what still makes DeleteDeep's 16th level
  `ERR_NAME_TOO_LONG` until DE1 finds resolve's answer by climbing and
  drops `phys`.
- **The same answers:** a host check (scratch, not kept) ran the old
  text code and the regions side by side: every path of one to three
  names from 23 awkward ones (`ETC`, ` etc`, `etc.`, `SSH.`,
  `authorized_keys. `, `AUTHOR~1`, `..`, `""`, ...), 200,000 random paths
  of up to six, each under all 64 flag sets with and without `stat`, and
  200,000 random walks (names pushed, `..` popped) against the region of
  their text: no difference in 2.6 million checks. utest
  `view_hides_names` now walks a region down and back against its text
  too. One edge differs, on purpose: `name_hidden` called a name whose
  place from the volume's root would pass 4,095 bytes hidden ("too long
  to judge"); a region judges it exactly (only a subview with a root
  ~4 KiB deep could meet it).
- **A view's root** keeps its text (`struct xf_view`'s `root`, in the
  state; no layout change, `XF_STATE_LAYOUT` stays 8): each walk starts
  at `fs_region_of(root)`. So a subview whose root directory is renamed
  later is still judged where its root was (IE2's "stale renamed-root
  string"), as before DE0. *For DE1:* 3.2's epoch names only directory
  channels; it should re-place a view's root the same way (its region
  cached per view slot with the epoch it was found at, climbed again
  after a move; a root moved into a place its flags hide, `etc/ssh`
  under `HIDE_KEYS`, then hides everything under it), and watch.c's
  record-built `struct xf_ctx` (IR2's file) must fill whatever field
  DE1 adds for it.
- **`user/lib/fsview.c` taught the three methods** (the DP notes' first
  "For DE1" item, done here since DE0 owns the file): `opendir`'s path
  judged as a read (hiding flags, not the `stat` rule); `rename_at`'s and
  `link_at`'s `from` as rename's and link's (read-only, the guards, the
  hiding flags), and their `to` too when it starts with `/` (from the
  view's root; a relative one starts at `to_dir`, left to the service).
  Every `fs` method is now listed in `fs_view_check`'s switch, and one
  it doesn't list is `ERR_ACCESS_DENIED` on a view with flags (a view
  can't judge what it would reach); 0 flags and another protocol's
  ordinal are left to the dispatch as before. utest `fs_view_dirs`;
  `fs_no_dirs`'s fat view is now a writable guarded one (a read-only
  view refuses `rename_at` and `link_at` before fat answers).
- Tests (batch 19193056 on the PC): `make check`, the init run (utest
  `view_hides_names`, `fs_no_dirs`, `fs_view_dirs`, the views and links
  cases unchanged), ktest at 4 CPUs, `tools/extfs-svc-test.sh` (37
  passed, every disk clean), the eight `tools/extfs-test.sh` cases,
  `tools/layout-test.sh` drive, the shell scripts basic, cmds, commands,
  files, files-fat and posix-files (its first try met the boot race
  where `/data` isn't mounted yet; passed again), kdump save: all PASS.
  `tools/ssh-test.sh` (the SSH server's views) failed there only in its
  typed-ahead case, both tries, on a loaded PC, and passed alone on this
  branch and on its base.

## 8. Tests

| What | Where |
|---|---|
| Depth, views, links, the reach, limits | utest `extfs_dirs` (DE1, DE2) |
| The region swap changes nothing | utest `extfs`; `tools/extfs-svc-test.sh` (DE0) |
| Restarts with channels open | utest `extfs_restart` (DE2) |
| The disk after deep trees and removed held directories | `tools/extfs-test.sh` basic (e2fsck) |
| Views of one volume on a drive boot | `tools/layout-test.sh` drive |
| The keeper past 1,024 handles | utest `keep` (DP) |
| The Linux API through libc | posixtest `dirfd` (tools/shell-tests' posix-dirfd.txt), `files`, `fork`, `procfs` |
| The hand-over | utest `posixfds`, `posixfds_bad` |
| FAT keeps today's answers | posixtest `dirfd` with `JAMOS_DATA_FS=fat` |
| Chromium | `tools/cr-test.sh` base with `CR_BASE_ONLY` (DC) |
