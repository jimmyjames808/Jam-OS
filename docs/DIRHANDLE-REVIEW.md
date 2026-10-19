# DR: the directory handles review

Track DR of the directory handles' plan (joined into
[HISTORY.md](HISTORY.md#directory-handles)): the
directory handles (DP, DE0, DE1, DE2 in extfs; DL0, DL1, DL2 in libc,
init's `/svc/exec` and the hand-over) read by an agent that wrote none of
it, at local main bcc76f8c (DC merged; line numbers are main's). WQ's
`struct xf_heard` (a write-back error put back for a rerun `file.sync`)
was read with extfs's restart paths. DC's full base run sent one lead
(M1).

The questions (the plan's DR row):
- can a channel reach past its view: a climb, an id, a hidden `etc`
  through `..`, a cut-off missed after a restart?
- can channels or orphans leak: a process that dies, a fork, a failed
  exec, a spawn with grants, `SCM_RIGHTS` in flight when a process dies?
- can a removed directory's inode be used again while held?
- is a hand-over's channel ever dropped or doubled: kind 15,
  `POSIXFDS_CWD`, init's relay, `/svc/exec`?

Severity, as in the other reviews: **High** is a hole (a channel reaches
past its view, an inode used again while held, a handle a process never
got), **Medium** is wrong behaviour the system or a test tier will meet,
**Low** is a wart or a mismatch with the documented rule that nothing
relies on yet.

Counts: **0 High, 1 Medium, 3 Low, 8 notes.** No channel reached past its
view by any path tried, by reading or by the hostile runs (climbs, ids,
`..` through hidden places, places across renames and restarts: 15,000
moves in and out of a subview under walkers, nothing from outside); no
held inode is freed; every hand-over path gives each channel once. The
Medium (DC's lead) is a missing inotify event. It and two Lows are fixed,
each with a case that fails without its fix; one Low is left, with why.

## Findings

### M1. A change of a directory by its channel tells its parent's watch nothing (fixed)

`user/services/extfs/fsops.c:28` (`ev_walk`): an event went to the
parent's watches only when the walk ended in a name (`w->has_name`).
Since DL1 a directory descriptor is its channel, and libc's `futimens`,
`fchmod` and `fchown` on it (meta.c's `change_at`, `AT_EMPTY_PATH`) send
`fs.setattr` with the path `""`: the walk ends in no name, so only the
directory's own watches heard `IN_ATTRIB`. Linux's `fsnotify_parent`
tells the parent's watch too, with the name. The same held for a path
ending in `.` or `..` on any channel.

*Failure:* Chromium's `FilePathWatcherTest.RecursiveWatch` (DC's full
base run) waits at file_path_watcher_unittest.cc:1103 for the event
`base::TouchFile(dir)` makes in dir's parent, and times out. Any inotify
user watching a directory for its children's attribute changes misses a
`touch` of a child directory done by descriptor.

*Fix* (the main session's call: in the service, not libc's text, which
races with a rename): `ev_walk` finds a nameless walk's directory's parent
by its `..` entry and its one name by a scan of the parent (walk.c's new
`walk_parent`), at the event. This is DC's option (a) without a table
(my call): a directory has exactly one name and ext keeps its `..` right
across renames, so nothing is kept that a rename must update or a restart
could lose (files.c's `at[]` is memory only and gone after a restart), and
a channel opened by `""` or `..`, which has no name to remember, is
covered too. The scan runs only while something on the volume is watched,
once per path-less change of a directory.

Tests: utest `extfs_watch_nameless` (`""` on a channel, `/d/s/.` and `..`
from a channel below; after a rename into another directory the new
parent and name; the volume's root its own watch alone); posixtest
inotify's `dir_fd_events` (`futimens`, `fchmod`, `fchown` on a directory
descriptor, each heard by the parent's watch as `IN_ATTRIB|IN_ISDIR` "s",
taken before the next change since Linux folds an equal record still
last in the queue). Without the fix (batch 20084227): "extfs_watch_nameless:
FAILED ... the directory itself: not the events wanted" and "a directory
descriptor's change unheard by its parent's watch: futimens fchmod
fchown". `FilePathWatcherTest.RecursiveWatch` is off
`tools/cr/base_unittests.skip`; it needs a base run to confirm.

### L1. A replaced channel is kept on another volume's inode of the same number (fixed)

`user/posix/sys/dir.c:121` (`replace_locked`, DL1's reopen after
`ERR_PEER_CLOSED`): the new channel was kept when its `id` equalled the
old one's, and its `volume` copied over without being compared. An id is
an inode number, which means something only on its own volume.

*Failure:* a descriptor open on `<mount>/a/b` (inode 14) of an ext
stick; the stick pulled and another ext volume mounted at the same name
(one made by the same tool lays its first directories out at the same
inode numbers): the descriptor's next call opens `<mount>/a/b` on the
other volume, finds inode 14 and carries on there as if it were the same
directory. Linux's descriptor stays on the old volume; here it should be
gone (`ENOENT`). No reach is gained: the reopen is through the caller's
own namespace.

*Fix:* the reopen is kept only when both the id and the volume match.
Test: posixtest dirfd's `replaced`, on a fake `fs` service with directory
channels (posixtest can't make extfs give its state up): three
descriptors; the service's channels closed and the same directory there
again: each carries on, on a new channel; then the same inode number on
another volume: `ENOENT`; another inode at the name: `ENOENT`; the old
directory back at its name: the gone ones stay gone, the third (never
asked in between) carries on. That is also DL1's untested reopen path.
Without the fix (batch 20084227): "FAILED user/posix/test/dirfd.c:882:
ino_of(a) == 0 && errno == ENOENT".

### L2. A freed inode reads as a removed one (DE2's finding; hardened)

`user/services/extfs/walk.c:235` (`gone_dir`), `:285` (`walk_gone`'s
parent) and `user/services/extfs/dirs.c:87` (`let_go`) told a removed
directory by `ext_iget(EXT_IGET_UNLINKED)` and a link count of 0. ext's
`ext_inode_free` keeps the mode and generation and sets the link count to
0, and `ext_iget` doesn't read the inode bitmap, so a freed directory
inode reads exactly as a removed held one. DE2 covered `..` with
`dirs_has`.

*Reachable?* Not by any path found: a held inode is never freed
(`files_held` asks `dirs_has` in `defer_run` and at every mount's
`recover_orphans`, and a slot is forgotten before its inode's drop, so a
death between leaves an orphan, not a freed inode a slot names). It is a
second line: had an invariant broken, `let_go` would drop a freed inode
again, `ext_inode_free` would call the volume damaged ("freeing an inode
not in use") and it would go read-only.

*Fix:* ext gains `ext_inode_used` (the inode's bit in its group's
bitmap; a group that never wrote its bitmap has none in use), extfs's
`inode_used` asks it, and `gone_dir`, `walk_gone`'s parent and `let_go`
require it. No case can make the state it guards; the removed-directory
cases (`extfs_dirs_gone`, `extfs_restart_gone`, `extfs_restart_tree`,
the hostile runs) pass with it.

### L3. A removed directory's channel names removed parents outside its view (left)

`user/services/extfs/walk.c:249` (`walk_gone`): `..` from a removed
directory goes to its remembered parent; a parent that was removed too
and is held (by anyone's channel) is walked as removed, with no reach
check (it has no place). A parent that is not removed is placed and
checked, as is everything after it.

*Failure:* a subview rooted at `/a` holds a channel on `/a/b`; a process
with the whole volume moves `b` to `/x/b` (the channel is cut off, Q2),
holds `/x` by a channel of its own, removes `/x/b` and then `/x`. The
subview's channel now answers `getattr("..")` with `/x`'s attributes, and
`opendir("..")` gives it a channel on the removed `/x`. It learns a
removed directory's attributes (mode, times, inode number); it can name,
make or read nothing in it (a removed directory answers only `""`), and
`..` from there is placed and refused. It needs a wider process's help.

Left as it is (my call): a reach check needs a place, which a removed
directory doesn't have (DE2's design: its `""` answers aren't judged by
the view either, as an open file's `fstat` isn't), and what leaks is the
attributes of an empty, removed directory.

## Notes (no change)

- **N1. Narrowed children get directory descriptors as channels.** DL2
  gives a `posix_spawnattr_setgrants_np` child its current directory as
  text, but an inherited directory descriptor (no `FD_CLOEXEC`) crosses
  as its channel (spawnfd.c `export_one`), reaching the parent's view.
  That is the plan's 2.3 rule (a descriptor gives its view's reach, as
  Linux's gives the tree through `..`) and what a file descriptor does;
  the current directory differs because every child has one without
  being given it. Chromium's launcher closes every descriptor it doesn't
  map, so nothing crosses by accident there.
- **N2. Capability cycles pin directory channels.** A directory channel
  queued in a socket whose ends are themselves queued in a cycle (two
  socket pairs, each end sent over the other: the kernel refuses only a
  socket sent over itself, D6) is never closed; its slot stays used and
  counts against its opener's 64 (by pid) until extfs starts fresh. The
  same holds for any handle: no in-kernel GC, by design.
- **N3. The volume's 256.** Four processes (or one that forks) can hold
  all 256 channels of a volume; everyone else then gets text descriptions
  (2.6's decided fallback, logged once a process), not an error.
- **N4. Ids from a hand-over are the starter's word.** A received
  description's `id` and `volume` steer only libc's choice between
  `rename_at`/`link_at` and `EXDEV`, and the reopen after a fresh start
  (when a text came with it). The service judges every `to_dir` id in the
  view of the channel the request comes on (`dirs_of_id`: the climb must
  pass that view's root, not where it hides), so a forged id names
  nothing that view can't name by a path; the reopen is in the receiver's
  own namespace.
- **N5. The reopen compares an inode number without its generation.**
  After a fresh start dropped a removed directory's orphan, a directory
  made later at the same text that took the freed inode number is taken
  for it. `fs.opendir` answers no generation, so closing this needs an
  IDL change (later, with the next protocol change). Needs a fresh start
  (the state given up) and the inode number used again at the same name.
- **N6. init's relay keeps the current directory's text.** A descriptor's
  directory channel crosses `/svc/exec` without its text (`dirch_untold`),
  the current directory's channel with it (`keep_cwd` copies the data
  whole). That is consistent with `child_dir`, which hands a child that
  isn't raised its caller's text directory anyway: such a child's
  namespace is no wider than its caller's (exec.c, "who is raised"), so
  the text steers it nowhere the caller couldn't go, and a raised child
  gets no channel. DL2's note "init's relay drops untrusted text" means
  the descriptors'.
- **N7. A full keep channel fails the request.** extfs sends every put
  and drop to its keeper without waiting (keepch's events); when the
  keeper has fallen 1,024 messages behind, a put fails and so does the
  request that made the handle (`ERR_SHOULD_WAIT` to an open or opendir),
  and a drop is only logged ("can't tell the keeper slot ... is closed").
  volumes takes everything at each event, so it isn't met there; utest's
  supervisor took 64 messages every 50 ms and `extfs_restart_deep` met it
  (fixed in the test's supervisor: all of them, as volumes). Directory
  channels add a put and a drop per directory opened, so a deep walk
  sends more than it did. Recommendation: later extfs hardening, a put
  that waits a little for room (the keeper reads) before it gives up.
- **N8. WQ's `xf_heard`.** The record is written whole before its mark
  (`hear`, a signal fence between), put back only by the request of its
  own sequence number (`wback_unhear` in `unmake`, before anything else of
  the rerun) and only where the mark is still the one recorded, then
  cleared; sequence numbers are global (svcstate's `next_seq`), so
  another slot's request never matches. A rerun at two crashes is answered
  `ERR_IO` with the mark put back, so the next `fsync` reports the error
  again (the main session's accepted double report).

## The questions, as checked

**Reach.** A walk from a directory channel checks its directory's place
at every request, `/` paths, `sync` and `statfs` too (walk.c `walk`,
`start_of`; fsops.c `dir_held`). A place is a climb of `..` entries to the
volume's root that must pass the view's root (with its generation), kept
until the epoch moves; every rename that moves a directory to another
parent, renames one in the top two levels, or exchanges with a directory
moves the epoch after `ext_rename` changed the blocks and before the
commit (fsops.c `rename_in`), so no place is computed between a change
and its epoch. `..` at a channel's start is ext's own entry, stopped at
the view's root (`ERR_LINK_OUT`), refused if it meets the volume's root
first, and each directory entered is judged by its region
(`hidden_dir`), so `..` to the root and down into a hidden `etc` is
refused as from the root. `rename_at`/`link_at`'s `to_dir` is placed the
same way in the view of the channel the request comes on. A successor
starts its epoch at 1 and its places at 0, so every channel climbs again
at its first request. Removed directories have no place (L3).

**Leaks.** A channel's slot goes when its last handle closes
(`SIG_PEER_CLOSED` armed with each request; `reap` when the table or the
share is full); a removed directory goes with its slot's last channel
(`let_go`). A process that dies closes its handles; a fork duplicates (the
slot goes at the last close); spawn and exec give duplicates that a
failure closes (`posixfds_discard`, the writer's "the handles go whatever
happens"); `SCM_RIGHTS` in flight is closed with the queue it is in (N2
for cycles). A request whose own channel `reap` closes (its client gone)
answers into a stale handle value (the kernel's handle generations), so
no other channel gets it.

**Inodes held.** `files_held` asks `dirs_has` wherever an orphan can be
dropped (`defer_run`, `recover_orphans` at every mount, the pager's), and
a slot is forgotten before its inode's drop. L2 hardens the reading.

**Hand-overs.** Kind 15 and `POSIXFDS_CWD` are parsed in one place
(`posixfds_dirch_parse`); the reader hands the cwd channel to the sink
once or closes it (every failure path of `read_all` and `posixfds_read`
read); the writer's first message carries it and a failure closes it with
that message. spawn moves `t->cwd`'s duplicate into the hand-over
(`spawnfd_export_cwd`), exec in place the same; init's relay passes the
cwd channel on or closes it (`cwd_dir`) and each descriptor's once
(`keep_fd`); `SCM_RIGHTS` drops the text (fdpass.c). A forked child's
current directory is met once under `POSIXFORK_CWD` and takes one
reference.

## Tests

The row's hostile runs, each passing on this branch (batch 20090927,
a-extfs: 53 passed; the init run and the shell's basic script run them
too) and with no fix to make in what they test:
- utest `extfs_dirs_storm`: 8 threads move `/v/in/d<k>` to `/out/d<k>`
  and back through the volume's whole channel for 3 s while 4 walk
  through channels on them opened in the subview `/v` and held across the
  moves: each probe answered from inside or `ERR_NOT_FOUND` (cut off),
  never `/out/secret` by `../secret`, `/out`'s listing through `..`, a
  place but `in/d<k>`, or a `rename_at` into `/out` by its id. 29,684
  moves, 59,951 answers from inside, 39,889 cut off, 0 from outside.
- utest `extfs_restart_deep`: DeleteDeep's tree (26 levels of 255-byte
  names, 4 files in each) made and removed through channels only (each
  level listed, files unlinked, each directory removed while its channel
  is held, which then answers as removed), round after round while the
  supervisor kills extfs every 200 ms: 86 rounds, 10 kills, every call
  answered, no channel lost, and the free bytes of before at the end
  (N7 met here first, in the test's own keeper).
- utest `extfs_dirs_killed`: a process holding its 64 channels (on
  `/h/k00` to `/h/k63`) killed after 32 of those directories were removed
  under it: their blocks free again, and the volume's 256 slots all taken
  again (ours and three children's).
- posixtest dirfd's `stand`: a directory removed while 4 forked children
  stand in it, each its current directory and a descriptor: `getcwd`
  `ENOENT`, no links (`fstat`, `stat(".")`), nothing made (relative and
  through the descriptor), `/proc/self/cwd` "(deleted)", `..` the parent
  it had (whose name now holds another directory) and `chdir("..")` out.
- posixtest dirfd's `replaced` (L1 above): DL1's reopen after
  `ERR_PEER_CLOSED`.

Runs (the PC; what was built is in
[HISTORY.md](HISTORY.md#directory-handles)): without the three fixes (a
branch with them reverted) batch 20084227: `extfs_watch_nameless`, posixtest
inotify's `dir_fd_events` and dirfd's `replaced` FAIL, as they should. With
them, batch 20085357 (17 runs): `make check`, the eight
`tools/extfs-test.sh` cases, posix-dirfd, posix-inotify, posix-files,
posix-fork and posix-run PASS; the init run, `tools/extfs-svc-test.sh` and
sh-basic FAIL on `extfs_restart_deep`, a fault of the test's own keeper (it
took 64 keep messages a poll: N7; the init run's first try also met
`ns_changes_stay_bounded`, which passed on its try again: load); after the
supervisor's fix batch
20090927: `make check`, the init run, `tools/extfs-svc-test.sh` (53 passed,
every disk clean) and sh-basic PASS; batch 20091345 with
`JAMOS_DATA_FS=fat`: posix-dirfd PASS. No kernel change, so no ktest.
