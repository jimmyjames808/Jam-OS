# History

What each milestone delivered, the bugs worth remembering, and the lessons.
Newest first. Status and what comes next are in [ROADMAP.md](ROADMAP.md);
the finished plans and reviews were deleted once this file covered them
(they stay in git history). Versions are the kernel's version string;
hashes are commits on main.

## Lessons that keep coming back

- **QEMU is not the PC.** QEMU's TCG has no PCIDs and no TSC-deadline
  timer, so those ran for the first time on the PC. Several bugs only the
  PC could show (M6's BAR sizing, M5's slow-console rate limit). QEMU also
  can't measure page-allocator scaling: stores to some physical pages cost
  100-400x a normal page there, which moves page-related numbers by up to
  30x between builds ([BENCH.md](BENCH.md)).
- **Stress runs find the races.** The 10-minute stress found the lock
  checker's IRQ race (M3), the per-CPU access rule (a thread reading
  another CPU's current thread through `this_cpu()` from preemptible
  code), and a job credited after its unlock (M5).
- **A reviewer who didn't write the code finds what the author can't.**
  M5's review found four quota escapes; M7's found that a `run` program
  could hijack the keyboard. Big changes now always get an independent
  review after the merge.
- **Kernel-first bring-up bought nothing.** M6's xHCI driver behaved the
  same as a kernel process and as a user process, while the process path
  gave better crash reports and could always be killed. Since then drivers
  and services are processes from the start.
- **Measure before optimising.** M5.5's benchmark showed PCIDs saved only
  2% of a process-to-process call; the cost was on the user side of the
  boundary.

## Directory handles

*Planned 2026-10-18, built 2026-10-18 to 2026-10-20, reviewed and fixed
2026-10-20.* A directory description in libc was its absolute path as text,
and so was the current directory, while the `fs` protocol takes paths of at
most 4,095 bytes from a view's root. Chromium's `FileUtilTest.DeleteDeep`
makes 26 levels of 255-byte names through `mkdirat` and `openat` on each
level's descriptor (about 6.6 KB deep) and deletes them through descriptors
alone: its 16th level was `ENAMETOOLONG`. Text cost elsewhere too:
`chdir("..")` out of a removed directory failed, a directory made again at
the same path counted as the one removed, the descriptor of a renamed open
directory kept the old name, and `struct path` (4,120 bytes) sat on stacks
beside a second 4 KiB buffer (an `openat` frame was 8.4 KiB). The owner:
"fix everything".

What it is:
- **A directory is an `fs` channel.** `fs.opendir` (method 18) gives a
  channel whose paths start at that directory; a path that starts with `/`
  still starts at the view's root, so every path sent before means what it
  meant. Every `fs` method works on it but `view`, `subview` and `watch`
  (`ERR_NOT_SUPPORTED`); `""` is the directory itself and `..` climbs to the
  view's root and stops (`ERR_LINK_OUT` above it, as from the root). A call
  sends one relative path of at most 4,095 bytes however deep the directory
  lies. The channel keeps a copy of the view it was opened through, so it
  never sees or changes more than a path through its mount could. There is
  no new protocol: fat, the boot image and ramfs answer the new methods
  `ERR_NOT_SUPPORTED` with no change, and `FS_FEAT_DIRS` in `statfs` says a
  service has them. `fs.rename_at` and `fs.link_at` (19, 20) name the second
  directory by its id, because a request the keeper replays carries no
  handle; the service places it by climbing, in the view of the channel the
  request came on, and `fs.opendir` also answers a `volume`, so libc knows
  whether two directories can meet in one call (else `EXDEV`). The kernel
  changed nothing.
- **extfs** (`user/services/extfs/dirs.c`, `walk.c`, `fsops.c`, `views.c`):
  the walk starts at the channel's inode. The view checks, which read a
  place as text from the volume's root, now read a *region*: the depth below
  the root and the class of the first two names (`<fsview.h>`), exact at any
  depth. That swap went in first with no change of behaviour (DE0), checked
  against the old text code on 2.6 million awkward paths. A channel's place
  is found by climbing its `..` entries and kept with an *epoch* that goes up
  at every rename that moves a directory to another parent or touches the
  top two levels, so a directory moved where its view cannot reach (above a
  subview's root, into a hidden `etc/ssh`) is cut off, `ERR_NOT_FOUND`, and
  works again if moved back. A subview's root is a place too: one renamed
  into `etc` is guarded there.
- **Held directories.** One removed while a channel holds it stays on ext's
  orphan list with no name and its inode is not used again: its channel
  answers `""` and `.` (link count 0), `sync`, `statfs` and `opendir` of
  `""`, and `..` as the parent it had (remembered in the slot), and
  `ERR_NOT_FOUND` to the rest. The last close drops it and frees its blocks,
  as ext4 does at the inode's end. A renamed directory is followed, since the
  channel holds the inode.
- **Limits and restarts.** 256 channels a volume and 64 from one process
  (the writer pid of the `opendir` request, also for a channel handed on).
  The records are in the state VMO (`XF_STATE_LAYOUT` 11) and the channels
  with the keeper, as views are: a successor takes them back and each
  climbs again at its first request; a fresh start closes them. `keepch`'s
  `MAX_HANDLES` went to 2048. A full service is not an error: libc makes a
  text description instead and logs it once a process.
- **libc** (`user/posix/sys/dir.c`, `path.c`, `file.c`, `fobj.c`,
  `meta.c`): a directory description on a mount with `FS_FEAT_DIRS` holds
  the channel (fat, the boot image, `/`, `/svc` and libc's own trees stay
  text). `struct path` has `at`, the description a relative path starts
  from, and `path_done` ends it; `tools/checkpathdone.py`, in `make check`,
  finds a function that makes a path and never ends it, since a leaked
  description is one of 256. The *at calls send the program's path on the
  channel; `getdents`, `fstat` and `fsync` go to it; a file opened from a
  channel keeps a duplicate of it and its name, so its reopen opens the name
  there. After a fresh start (the channel's peer closed) libc opens the last
  known text once and keeps the new channel only when it is the same inode
  on the same volume, else the descriptor answers `ENOENT`.
- **The current directory** is a description too: `chdir` is `fs.opendir`
  without the read bit, `fchdir` shares the descriptor's description,
  `getcwd` is `fs.resolve("")` on the channel (`ENOENT` when removed,
  `ENAMETOOLONG` past a page), `chdir("..")` out of a removed directory
  reaches the parent it had, and `/proc/self/cwd` and a directory's
  `/proc/self/fd` link say the same, with " (deleted)" for a removed one.
- **fork, exec, spawn, sockets.** `fork` duplicates each channel. The
  hand-over has a kind for it (15, `POSIXFDS_DIR_CHANNEL`) and a flag for the
  current directory's channel (`POSIXFDS_CWD`): `POSIXFDS_VERSION` 3, in the
  ABI stamp, so a static program linked before reads no hand-over until it
  is relinked (Chromium's programs were, once). Init's `/svc/exec`
  (`user/services/init/exec.c`) passes the caller's directory channel to a
  child that isn't raised; a raised child still starts in `/`. A directory
  with a channel crosses `SCM_RIGHTS` as the channel and the receiver does
  not believe its text. A child given grants (`posix_spawnattr_setgrants_np`)
  starts in its current directory as text, never the channel, which would
  reach past its grants.

The owner's answers (2026-10-18: he delegated the four questions; decided as
recommended): **a handle reaches its view**, `..` up to the view's root
(Fuchsia's, stopping at the directory, would fail every program that walks
up from a descriptor, and `fs.subview` already gives it to whoever wants
it); **a directory moved out of its channel's view is cut off**, as the
inotify plan's watches are; **FAT later**: it keeps text descriptions (4,095
bytes of depth, renames not followed), since Chromium's tests and a drive
boot are on ext; **the current directory's channel survives `exec`**
(`POSIXFDS_VERSION` 3, one relink). Decided as technical: directories as
`fs` channels and not a new protocol, whole paths unchanged, the second
directory by id with the reach check, regions in place of text, 256 and 64,
a text description past the limit, a channel opened again from its text once
after a fresh start, `getcwd` asking the service every time, and the main
session's calls on the way (the lint, restarts in DE1's track, the channel
over a socketpair, a whole path given with a removed directory's descriptor
going through the mount).

The tracks: DP (the protocol and the keeper's room), DL0 (libc's path
reference), DE0 (the region), DE1 (extfs's channels, with their restarts),
DL1 (libc's descriptions and the *at calls), DE2 (removal and orphans), DL2
(the current directory, fork, exec, `/proc`), DC (Chromium's tests), DR (the
review). **DC: `FileUtilTest.DeleteDeep` passes** on ext `/data` and on
init's `/tmp` and is off the skip list; the whole of `base_unittests` was 8,901
pass, 37 self-skip and 18 listed, and it found the one regression, below.

The review (docs/DIRHANDLE-REVIEW.md, by an agent that wrote none of it): 0
High, 2 Medium, 3 Low, 8 notes. No channel reached past its view by any path
tried, no held inode was freed, no hand-over dropped or doubled a channel.
Fixed with a case that fails without each fix: the Medium (DC's lead),
`FilePathWatcherTest.RecursiveWatch`, because `futimens`, `fchmod` and
`fchown` on a directory descriptor send `fs.setattr` with the path `""` and
the walk ended in no name, so the parent's watch heard nothing (Linux's
does); extfs now finds the directory's parent by its `..` entry and its name
by a scan, at the event, with no table of names for a rename to follow and a
restart to lose. L1: libc's reopen after a fresh start compared the inode
number and not the volume. L2: a freed directory inode read as a removed
held one (no path reached it; ext gained `ext_inode_used`, which asks the
bitmap). The hostile runs: 8 threads moving directories in and out of a
subview under 4 walkers (29,684 moves, none answered from outside), extfs
killed every 200 ms in the middle of DeleteDeep's tree (86 rounds), a
process holding its 64 channels killed, and 4 processes standing in a
removed directory. The deep tree under kills found the second Medium, on a
loaded PC only (the merged-main check): `fs.open` and `fs.opendir` took
their slot in the state before naming it the request's, so a kill between
kept the slot at the rerun, and for a new file the successor found its inode
missing and started fresh, closing every client's channels; the mark comes
first now (test power `die=slot`, utest `extfs_restart_slot`).

Left, none on Chromium's path:
- **L3.** A removed directory's `..` can name removed parents that lay
  outside its view: their attributes only, and it needs a wider process's
  help.
- **N5.** The reopen compares an inode number without its generation: after
  a fresh start dropped a removed directory, a directory made later at the
  same text that took the freed number is taken for it. `fs.opendir` answers
  no generation, so closing it is an IDL change, to go in with the next one.
- **N7.** A full keep channel fails the request: extfs sends every put and
  drop to its keeper without waiting, so a keeper 1,024 messages behind
  fails the open that made the handle (`ERR_SHOULD_WAIT`). volumes takes
  everything at each event, so only utest's slow supervisor met it.
  Recommended: a put that waits a little for room.
- **DF, fat.** FatFs's relative paths (`FF_FS_RPATH`) would give channels by
  first cluster, with removal marks and no pin; built only if a FAT `/data`
  needs more than text (the owner's "later").
- **The rest** (each in the review or the plan): an inherited directory
  descriptor of a narrowed child crosses as its channel, as a file's does
  (N1); a cycle of sockets queued in each other pins a channel until a fresh
  start (N2, by design: no in-kernel GC); four processes can take a volume's
  256 and everyone else then gets text descriptions (N3); `opendir` makes no
  inotify event (`IN_OPEN` with `IN_ISDIR`); `openat2` stays `ENOSYS`;
  `RecursiveWatch` is off the skip list and a `base_unittests` run is still
  to confirm it.

Tests worth knowing: utest `extfs_dirs`, `extfs_dirs_views`,
`extfs_dirs_limits` and `extfs_dirs_gone` (`user/tests/utest/extfs_dirs.c`:
DeleteDeep's tree through channels, the view flags, the cut-off, the 256 and
64, removal), `extfs_restart_dirs`, `extfs_restart_gone` and
`extfs_restart_tree` (extfs killed at each point with channels open), the
hostile `extfs_dirs_storm`, `extfs_dirs_killed` and `extfs_restart_deep`, and
`extfs_watch_nameless`; `fs_no_dirs` and `fs_view_dirs` (the servers without
channels, the view code); utest `posixfds_*` for kind 15 and `POSIXFDS_CWD`
(59 malformed forms); posixtest's `dirfd` section (`user/posix/test/dirfd.c`,
on `/data` and `/tmp`, also with `JAMOS_DATA_FS=fat`, which keeps today's
answers: DeleteDeep copied, renamed and removed directories, the current
directory deep, renamed, removed, forked and exec'd, `stand` and `replaced`);
`tools/extfs-svc-test.sh` (53 cases, every disk judged by `e2fsck`) and
`tools/extfs-test.sh basic` (`exttest dirs`: a deep tree and held removed
directories). [TESTING.md](TESTING.md) has each.

What it taught: the swap that changes nothing goes first and is checked
against the old code (DE0), so the track that adds behaviour has one thing to
get wrong. And the one regression was found by the whole Chromium run and by
no case of ours: a descriptor that is a channel has no name in it, and an
event that wanted the name had to find it at the service.

## Writable file mappings (on ext)

*Planned 2026-10-16, built 2026-10-16 to 2026-10-18, reviewed and fixed
2026-10-18, the review's error question closed 2026-10-20.* A shared
mapping of a file with write access (`MAP_SHARED` with `PROT_WRITE`) was
`ENODEV`. Chromium's `base::MemoryMappedFile` with `READ_WRITE` backs its
persistent histograms and some databases, and the first `base_unittests`
run met the error. The owner (2026-10-16): build them now, **on ext only**;
FAT keeps read-only mappings. This reversed, for ext, M12.5's "every page of
a pager VMO is clean" and M13.6's "no writable shared mappings".

How it works:
- **The kernel** (`kernel/object/vmo.c`, `pager.c`, `pager_cache.c`,
  `kernel/mm/aspace.c`). A pager VMO may be made writable (a flag at its
  creation). Its pages are clean, dirty or writing. A clean page is mapped
  read-only everywhere, so the first store faults and the kernel marks it
  dirty. The filesystem asks for dirty pages (`pager_dirty`): the kernel
  write-protects them in every mapping, shoots the stale entries down, and
  only then copies them out, so a store after the copy faults and dirties
  the page again and is never lost. The filesystem writes them and says
  `PAGER_OP_WRITTEN`, which makes them clean unless a store came
  meanwhile. Write-back requests are a new request kind, made by the
  background count, `msync` (`vmo_sync`) and the dirty limit.
- **Never evicted, never unbounded.** A dirty page is never evicted. A
  pager may have a quarter of its budget dirty; a writer that meets the
  limit waits for write-back with a deadline (the pager's answer time),
  never forever, and a copy that cannot wait returns `ERR_IO`. A dirty page
  holds its VMO ("the hold"), so stores outlive `munmap`, the program and
  an extfs restart.
- **extfs keeps one cache** (`user/services/extfs/pager.c`, `wback.c`):
  one VMO per file, keyed by the inode and found again while it lives
  (`PAGER_VMO_FIND`); `file.write` stays write-through (an answered write is
  on the medium) and writes the bytes into the pages too, `read` peeks at
  dirty pages, so a mapping, `read` and `write` see one file. Write-back is
  a journal transaction of its own (up to 32 runs, clipped to the size,
  holes and unwritten extents filled in ordered mode); by age once a
  second, for `file.sync`, `fs.sync`, the dirty limit and an orderly stop.
  A file with a writable mapping cannot be run, and a running file cannot
  be mapped writable (`ETXTBSY`/`ERR_BUSY`, M12.5's Q7 both ways).
- **libc** (`user/posix/sys/mem.c`, `file.c`): every `MAP_SHARED` of an
  `O_RDWR` descriptor asks for a writable VMO, as Linux decides at `mmap`;
  `msync` calls `vmo_sync` per shared entry, `MS_SYNC` waiting; FAT answers
  `ENODEV`.

The owner's answers: a write-back that fails (**Linux's way**: the pages
become clean, the error is kept and the next `msync` or `fsync` reports it
once; a "reserve at the first store" check can come later), and **how long a
store may wait for the disk: 10 seconds to start with**, a setting
(`files.writeback`, 1 to 600 s) read at mount and on a settings change, not
a constant. Decided as technical: one VMO per file keyed by the inode,
dirty bits by write-protection, the limit at a quarter of the budget, no
kernel timer, `vmo_write` refused, `MS_ASYNC` asking for write-back.

The tracks: WK1 (writable pager VMOs, the states, FIND, the hold, `pager_dirty`,
`pager_write`), WK2 (write-back), WK3 (the dirty limit, the store's wait,
`vmo_sync`), WE1 and WE2 (extfs's cache and write-back), WL1 (libc), WT1 (the
end-to-end tests), WR (the review), WR2 and WQ (errors to every mapper). New
calls: `pager_dirty` (165), `pager_write` (166), `vmo_sync` (50); the ABI
stamp moved with WK3, WR and WR2.

Tests worth knowing: `pagertest rec` stores CRC'd records through a writable
mapping, and `tools/pager-test.sh` runs them hard: `wmsync` (extfs killed with
`msync`ed and dirty records, a monitor quit, `e2fsck` clean), `wcut` (QEMU
killed at six points of a store-and-`msync` storm: every acknowledged
record whole after Linux's replay), `wpress` (64 MiB stored three times
under an 8 MiB dirty limit beside a reader), and `whostile` (eight writers
in two processes, extfs killed every 200 ms, then a power cut). ktests in
`test_pager_dirty.c` cover the states, the waits, the hold and an 8-thread
store storm. Eight writable-mapping `base_unittests` came off the skip list
(35 of 35 in their suites; on the FAT list instead).

The review (docs/WRITEMAP-REVIEW.md, ahead of CB5's processes sharing
memory): 0 High, 3 Medium, 6 Low. Fixed with a test each: a page whose
write-back failed stays "kept", so `read` shows what every mapping shows;
a shared VMO a fresh extfs instance finds with no slot is found again on
`write`, `truncate` and the run check, instead of being overwritten by its
dirty page's older bytes; a map racing another process's last `unmap` takes
over the dying VMO's key. Its design question on errors was answered yes
and built (WR2): the error is reported once to **each** mapping and each
open (Linux's errseq: `vmo_sync` takes a sample, libc samples each shared
mapping, extfs numbers errors per slot), and WQ closed a window where an
`fsync` run again after an extfs death said 0.

Still open (design questions the review left to the main session): a store's
deadline counts from its first wait, so a healthy writer behind a busy pager
can be killed (recommended: from the pager's last progress), and an orderly
stop writes back for about 3 s at most (recommended: bounded by progress).
Both are now tracks of [RUNWELL-PLAN](RUNWELL-PLAN.md#3-wr2s-two-questions):
the deadline is RW7 and the orderly stop RW8. A third, a freeze that starves
behind a waiting writer (FR1's option (b)), stays for before the sandbox's
renderers. None blocks Chromium. FAT stays read-only for mappings.

## File watching (inotify)

*Planned 2026-10-17, built, reviewed and fixed 2026-10-18 to 2026-10-19.*
`inotify_init` was `ENOSYS`, so Chromium's `base::FilePathWatcher` could not
start and all 165 `FilePathWatcher*` tests of `base_unittests` sat on the
skip list. The owner's rule (build the layer, don't patch Chromium) gave
the design: the file services make the events, because only they see every
change, through every view, by every process.

What it is:
- **The protocol.** A new `fs` method (`fs.watch`, 17) returns a *watcher
  channel*; the `fswatch` protocol on it adds, removes and takes watches.
  A kernel event with a user signal, "the bell", rings when a channel's
  queue stops being empty. The queues, merge and overflow rules, ids with
  generations and the caps are one library (`user/lib/fswatch.c`) that any
  service links.
- **extfs makes the events** (`user/services/extfs/watch.c`, with
  `files.c`, `fsops.c`, `main.c`): every row of Linux's table (create,
  delete, move with cookies, attrib, modify, open, close, `IN_DELETE_SELF`
  when an orphan goes), held until the request's commit and queued then.
  Watches are on inodes, so a change through `/jam` reaches a watch made
  through `/tmp`'s view of the same volume. A close made before a request
  is reported before that request's events (the order rule, which leans on
  the port's order: utest `port_order` pins it).
- **Views keep their rights.** A watcher channel is never wider than its
  view: hidden names never appear in an event, and a watch whose inode a
  rename takes out of the watcher's view ends (`IN_MOVE_SELF` if asked,
  then `IN_IGNORED`).
- **Restarts.** Watches and watcher records are in the state VMO, each
  channel and its bell held by the keeper; a successor puts the watches
  back and queues `IN_Q_OVERFLOW`, and rings the bell. A keeper that gives
  a service up rings the bells itself (`keepch.put` carries a ring), so
  `poll` and `epoll` wake when extfs is gone.
- **libc's descriptor** (`user/posix/sys/inotify.c`): the four calls, a
  kind whose readiness is the bell (so `poll`, `epoll` and `select` work as
  for eventfd), `read` and `FIONREAD` taking records from each service and
  handing out Linux's `struct inotify_event`. Watches in libc's own trees
  (`/`, `/svc`, `/dev`, `/etc`, `/proc`, `/sys`) and on read-only mounts
  are accepted and never fire ("quiet"). The limits are
  `/proc/sys/fs/inotify/`: 8 descriptors a process (`EMFILE`), 1,024
  watches a process on each volume and across its descriptors (`ENOSPC`),
  512 queued events; a volume holds 8,192 watches.

The owner's answers (2026-10-17, all three as recommended): **FAT watches
later**: no track for fat, which answers `fs.watch` `ERR_NOT_SUPPORTED`,
and libc refuses a watch on a writable FAT mount (`EOPNOTSUPP`; Chromium's
watcher then stops at the first FAT component); **a watch whose inode
leaves the watcher's view ends**, not Linux's way of following the inode;
**the descriptor as `epoll`'s**: absent in a forked child, closed at
`exec`, not handed to a spawned child. Decided as technical: events made
by the services with one watcher channel per view, libc's cyclic `wd`s,
Linux's merge rule, random cookie starts, a 0-byte `read` that never waits,
and no kernel change (the one ABI change came later, with the review).

The tracks: the protocol and library (IP), libc's descriptor (IL1), extfs's
events (IE1), libc's limits and the fork and exec rules (IL2), extfs's
rights and restarts (IE2), Chromium's tests (IC), the review (IR) and its
follow-ups (IR2). **IC: all 165 `FilePathWatcher*` tests pass** on an ext
`/data`, with `TMPDIR` on it or not; they moved to the FAT skip list.
Nothing failed in libc's or extfs's watching; `RacyRecursiveWatch` takes
208 to 337 s because each of its 2,000 make-write-remove cycles is a commit
on extfs, not because of the watches (posixtest times both). The whole
program: 8,874 pass / 37 self-skip / 45 listed, no regression.

The review (docs/INOTIFY-REVIEW.md, by an agent that wrote none of it):
0 High, 5 Medium, 8 Low. Fixed with a test each: a successor runs the reach
check of a rename it answers from the journal; libc ends a removed watch
whose `IN_IGNORED` an overflow dropped; a watch the reach check cannot
place (deeper than 2,048 directories, unreadable) now ends instead of
staying on; the reach check looks only where a rename can change a reach.
Two design questions were answered by the main session: a process's share
(1,024 watches on a volume across its channels, counted by the writer pid
that `fs.watch` carries; libc counts across its descriptors; the volume's
cap 4,096 to 8,192) and a given-up service's bells (the keeper's ring).
IR2 added the hostile runs as utests on extfs itself: a rename storm with
extfs killed every 200 ms (some 65,000 renames, none failed, only whole
records, one overflow a restart), eight threads closing and renaming while
another reads (each thread's records in order), protocol abuse, and a
watcher that never reads (extfs's pages and handles flat). None found a
High or Medium. Two Lows came out of them and were fixed later: a call with
txid 0 is now dropped by every generated server (genidl), and a process at
its limit can still change an existing watch's mask.

Open, none on Chromium's path: fat's watches (the owner's "later"); a share
per job, since eight processes can still fill a volume's 8,192 watches (the
sandbox's renderers would be one job); a directory removed in the window of
a crash loses its `IN_DELETE_SELF`; two closes before one request come in
slot order, not close order; `max_user_instances` 8 counts a descriptor
watching both `/data` and `/tmp` twice. The review lists each.

What it taught: Linux's events were checked against its source before the
table was written (a replaced rename target gets `IN_ATTRIB`;
`RENAME_EXCHANGE` is two moves; `IN_ISDIR` is never on `IN_DELETE_SELF`),
and the first notes' deviations (a bell with two bits so a `poll` cannot
sleep through records another thread's take left) came from writing the
tests against fake services before any real one existed.

## M12.7: NVMe and the installer

*Planned 2026-10-09, stage 1 built 2026-10-09 to 2026-10-10, stage 2
2026-10-13, stage 3 2026-10-14 to 2026-10-16, checked on the PC 2026-10-11
and 2026-10-18.* A user-space NVMe driver behind the IOMMU on the block
interface M12 had just reviewed, in three stages because the PC's only NVMe
drive held Windows: read-only, writes to a Jam OS disk only, an installer so
the PC boots Jam OS from an internal disk. The owner fitted a second drive
(blank, never initialised, Windows' prompt to initialise it refused) for
stages 2 and 3; the driver refuses every write to any other drive.

What was built:
- **Stage 1, read-only** (`drivers/nvme/`): the controller (admin queue,
  identify, an I/O pair, MSI-X, PRP lists, timeouts, abort, reset, the
  shutdown notice), `storage` and `block` with 32 requests in flight
  answered by txid, and no write opcode in the file that can be built:
  `tools/checknvme.py` runs in `make check` and refuses one. The partition
  library (`drivers/lib/parttable.c`: MBR, GPT with both headers and CRCs,
  blank detection, the Jam OS mark) is checked on the host by
  `tools/gptcheck.c`; `volumes` mounts a drive's FAT partitions read-only as `/nvme0`,
  `/nvme1`..., and the shell gained `disks`, `disk digest` and `disk smart`.
- **Stage 2, writes to a Jam OS disk only.** `sqe_write` (FUA) and
  `sqe_flush` are reachable only through the drive's mode (other, blank,
  Jam OS) and a range check; `create_table` works on a blank drive in a
  fixed order; `disk prepare <disk>` asks for the last four characters of
  the serial and makes a GPT with a "Jam OS ESP" and a "Jam OS data"
  partition (type 71bae565-d4a8-4315-8c81-9b35462cc158 for data). `diskro`
  from the boot menu keeps every drive read-only. The owner decided
  later (2026-10-14, "leave it to the user, just warn") that `disk prepare`
  works on any drive he picks: the empty rules decide the confirmation, not
  whether, and a drive that is not empty is shown and erased only after the
  serial and a typed `erase` (NV-warn; the rules include a MiB at 1 MiB and
  at 513 MiB, where Jam OS would put its partitions).
- **Stage 3, the installer.** The kernel passes Limine's GPT disk GUID
  (`bootdisk=gpt:<GUID>`) to init and into the kexec command line, so the
  boot disk is known by GUID; `install <disk>` (init's worker thread,
  volumes' hold of the target's partitions from other mounts) copies the
  build, `/data/etc` and the menu, the menu last, with read-backs; `update`
  writes the disk the PC booted from. A stick plugged in beside an installed
  drive boots itself and mounts the drive's partitions read-only.
- **Names.** Drives are named in PCI order (`disks.attach` carries a
  `unit`): on the owner's PC the names swapped between a boot from the
  stick and a boot from the new drive until then.

The owner's answers (2026-10-09, "your recommendation for everything unless
unsure"): the GPT layout is the mark, written only onto a blank drive after
the serial's last four are typed; the installed disk boots from the
firmware's default path and the owner sets the boot order; no Windows entry
in the boot menu (F8 or the boot order); the Jam OS drive's filesystem FAT
32 at first (ext came with M13.6); `/nvme0`, `/nvme1` for the mounts; the
installer copies the build plus `/data/etc`; the stick stays a working stick.
**Stage 1 is checked in Jam OS as it is**: no Linux stick and no separate
hash of the drive; the drive's own host-write counter, printed in the
driver's start and end lines, is the record. Reviews (2026-10-11): R1 and R2
dropped, R3 once after each of stages 2 and 3; in their place mechanical
checks (checknvme.py for the write path; every NVMe QEMU test keeps a
Windows-like disk attached whose hash must be unchanged). Stage 3 was
deferred for porting (2026-10-11) and then, on 2026-10-14, "finished whole".

The reviews (docs/M12.7-REVIEW.md, by an agent that wrote none of it). Stage
2: 0 High, 2 Medium, 5 Low; fixed with a test each: "blank" was decided from
a buffer prefilled with 0xA5 rather than zeros, and a second one fixed by the
merge of main itself. Stage 3: 0 High, 3 Medium, 10 Low; fixed with a test
each, on the PC before and after: a held disk could become the boot disk of
a GPT boot after the wait (S1), a cut erase was offered as "finish" for good
instead of being offered for erase again (S2), install refused a read-only
mount before anything was stopped (S3), and the installer's clearing also
removes a menu at Limine 11's other places (S11); S4 (a reinstall cut after
the new kernel, the drive picked beside a copy of the stick) has a guard test.

On the PC: **P1 passed 2026-10-11** (Crucial/Windows drive: the host write
counter unchanged across two Jam OS boots, the new drive at 0 writes).
**P2 and P3 on 2026-10-18**: `disk prepare` of the new drive, the refusals
(`mount -w` of Windows' drive no; `disk prepare` of it showed "not empty" and
was stopped), the Windows drive's write count unchanged, `install` of the
new drive, the PC booting Jam OS from it without the stick, Windows starting
from F8. The two power-cut steps of P2 (holding the power button during a
`disktest`) were skipped at the owner's choice, since QEMU covers the cuts
(`tools/nvme-write-test.sh`, `tools/install-test.sh`'s ten cut points,
`tools/extfs-test.sh`); only the real drive honouring its flush is unproven
on hardware. headless_shell then fetched pages over the internet from the
installed drive.

Open: the milestone's closing sign-off (All tests and `soak 10` on the PC
booted from the internal disk) is the main session's pre-push pass, not a
separate run; the real power-cut check above.

## M13.6: the ext filesystem

*Planned 2026-10-11, built 2026-10-11 to 2026-10-17, reviewed and fixed
2026-10-17.* "Porting first" (the owner, 2026-10-11): FAT32 has no symbolic
links, permission bits, owners or hard links, a 4 GiB file limit and
case-blind names, and Unix software breaks on each of them. The owner's
2026-09-30 decision (FAT32 only) was reopened for this and only this.

What was built:
- **The format** (`user/services/extfs/ext/`, no Jam OS calls, built for
  the host too): a subset of ext4's on-disk format, Jam OS's own code
  (about 10,500 lines in all): superblock and feature table, group
  descriptors, CRC32C on every structure, inodes, extent trees, bitmaps
  with flex groups, plain and hashed directories, symbolic links, hard
  links, atomic rename (replace and exchange), the orphan list, mkfs, and
  jbd2's journal (descriptor, revoke and commit blocks, checksum v3, replay).
  Linux reads and checks it: `e2fsck -fn` is the judge of every crash test.
- **The service** (`bin/extfs`, `user/services/extfs/`): the `fs` and
  `file` protocols on the ext core, a journal transaction per request, a
  restart as recovery (the state VMO and the keeper; the warm spare is fat's only: an ext restart starts a process), views
  judged where paths resolve, subviews, many writers (`FS_APPEND`),
  unlink of open files, remount read-only on an error. New protocol calls
  (`getattr`, `setattr`, `symlink`, `readlink`, `link`, `resolve`,
  `subview`, `ERR_LINK_OUT`) with answers from fat and bootfs.
- **libc** follows links in the caller's own namespace (`ELOOP` at 40),
  sends paths uncleaned, keeps `..` above a mount's root in `/`, and has
  `stat`, `chmod`, `chown`, `utimensat`, `symlink`, `readlink`, `link`,
  `RENAME_EXCHANGE`, `umask`, and `fsync` backed by the journal.
- **extfs as a pager** (U10): `file.map` read and exec from extents,
  invalidation on write and truncate, `ERR_BUSY` for running programs,
  unlinked mapped files kept on the orphan list, maps carried across
  restarts. Writable shared maps came later ([above](#writable-file-mappings-on-ext)).
- **volumes and init**: an ext superblock on a partition starts `bin/extfs`
  (the Jam OS data type, GPT's Linux type, MBR 0x83); the Jam OS drive's
  data partition is `/jam`, read-write; foreign ext volumes are always
  read-only; init mounts `/usr`, `/var`, `/home` and `/tmp` as views of
  `/jam`; 16 mounts. **On a drive boot the data partition is `/data` and
  `/jam` at once** (DB1, "drive boot first"), `/tmp` is a view of
  `/jam/tmp`, and `/pkg` is the package area.
- **Tools**: `tools/jamext` (mkfs, an image from a directory, import, put,
  get, ls, ln, chmod, check; reproducible images), the checker shared by
  `jamext check` and the shell's `disk check`, `ls -l`, `stat`, `ln`,
  `chmod`, `readlink` in the shell, `tools/extcheck.c` (cut storms on the
  Mac), and `tools/extfs-test.sh` (basic, links, case, a 6 GiB file and a
  file sparse at 5 GiB checked by hash, cuts at 20 write points judged by
  `e2fsck`, extfs killed 0/1/10/100 times a second during a copy, interop
  with three `mke2fs` volumes, foreign volumes read-only).

The owner's answers (2026-10-11, all as recommended): **own code for ext4's
format** (compared with lwext4, libext2fs, FreeBSD's and Haiku's ext2: none
permissive, current and journalled at once; libext2fs, LGPL, in extfs was
the fallback if the first tracks ran into trouble; FreeBSD's and Haiku's
code may be read as references; no code from Linux or e2fsprogs); the Jam
OS drive's data partition first; the stick unchanged until the owner
revised that on **2026-10-15: ext is the default data filesystem**, FAT only
when asked for (`disk format <disk> data fat`; `make image` and `make usb`
build the stick's `/data` as ext with `jamext`; the ESP stays FAT32 because
the firmware reads only FAT). A write is on the disk when its request is
answered; the Mac does not read the stick (logs by network, files by
`update` and SSH).

The review (docs/M13.6-REVIEW.md, by an agent that wrote none of it): 1
High, 2 Medium, 9 Low. The High: **extfs never flushed a disk's cache.**
drv/nvme's writes are FUA, so the journal's order held, but usb-storage's
writes land in the stick's volatile cache in any order, and since ext became
the stick's default `/data` a power cut could break the journal's order.
Now the journal flushes around the commit block, before the tail moves and
before the log is marked empty, at close and at mkfs; requests, `fs.sync`
and `file.sync` flush; NVMe uses `EXTFS_ARG_FUA`. The Mediums: a request in
progress at two deaths in a row ran again instead of being answered; one
extfs held only 32 open files, 32 views and 64 maps for a whole volume
(now 256, 128 and 256, with room in the keeper and volumes' job limits).
Also fixed with tests: a name a view hides gives nothing away when walked
through, and a journal's region is counted in 64 bits. Left as reported
Lows: a stick's writes cost two or three cache flushes a request; the
guards judge a place by its top-level name (only the unviewed shell can make
an `etc` link); `statfs` has no inode counts (an IDL change wanted before M14
freezes `fs`); `NS_MAX_MOUNTS` 16 is tight for a stick beside three drives.

Open: the PC check P1 (hold the power button during a `disktest` write on
the drive's ext `/jam`, then check the journal replays and `e2fsck` is
clean) was not run; the owner skipped the same step for M12.7 and QEMU's
cuts cover it, so only the real drive honouring its flush is unproven. The
three review questions (flushes per request, links named `etc`, inode counts
in `statfs`) are the owner's.

## M13: POSIX on musl

*Planned 2026-10-09, built 2026-10-09 to 2026-10-12, reviewed and fixed
2026-10-11 to 2026-10-14.* Unmodified POSIX programs build and run on
Jam OS: musl 1.2.6 over a call layer in libc that turns Linux's system calls
into handles, channels and the services' protocols. The first port was an SSH
server, and `ssh` from the Mac opened a shell on the PC. The plan stays in
the tree as [M13-PLAN.md](M13-PLAN.md) because the call layer's code and the
other plans cite its design sections and per-track notes.

What was built:
- **The kernel's part** (K1, K2, F1a): small process and thread ids (a full
  lap before reuse), `thread_set_tid_word`, `thread_interrupt` and
  `ERR_INTERRUPTED` on the four interruptible waits, `cpu_ns`,
  `sys_public_info`; the time page (`clock_gettime` in about 17 ns from user
  space) and `VMAR_REPLACE` (an unmap and a map in one step, for
  `MAP_FIXED`); copy-on-write child VMOs (`vmo_create_child`). The call
  path got no slower.
- **musl and the toolchain** (A1, A2): musl 1.2.6 vendored with a checked
  list of overrides (`user/posix/musl/OVERRIDES`), the sysroot, `jamos-cc`
  (one compiler wrapper, one sysroot, on the Mac), `posixtest` (the layer's
  own test program, in the init run), libjam (libos without its libc parts)
  and the POSIX hand-over message (`posixfds.h`, `fdmsg.c`: fuzzed) that
  gives a child its descriptors.
- **Descriptors, files, pipes, processes** (B1, B2, B3, B5, B6): a descriptor
  table over open file descriptions, the file, path, directory and `*at`
  calls, `pipe2`, `dup`, `fcntl`, `/dev` and `/etc`, memfd, eventfd,
  `socketpair`, the one user (uid 1000, home `/data`); `posix_spawn` with
  file actions and attributes, the child table, `wait4`, process groups and
  sessions, `execve` as spawn-wait-exit (no `fork`: that came later as
  [FORK-PLAN](FORK-PLAN.md)); `poll`, `select` and `epoll` over the wait
  sets, each kind with its ready and arm hooks; AF_INET sockets and
  `getaddrinfo` through `/svc/dns`.
- **Signals** (B4): libc's own signal thread, delivery at the call boundary
  with `thread_interrupt`, `EINTR` and `SA_RESTART`, timers, `pthread_cancel`,
  SIGCHLD, SIGPIPE, SIGWINCH, exit status 128 + N, the `signal` protocol.
  Faults to a handler and signals to a busy thread came later
  ([SG](SIGNALS-PLAN.md)).
- **Terminals and sessions** (T1, T2, X1, S1, S2): the console split into a
  front end and back ends (a connection back end: output as bytes on a master
  channel, input through `termkeys.h`, size by `conn_ctl`); the `tty` protocol
  and a line discipline (`user/lib/ldisc.c`, `keybytes.c`) with Ctrl+C under
  `ISIG`; `/svc/session` in init (at most 8 sessions, a console and shell
  each, `posix_openpt`/`openpty`); the shell gives programs descriptors 0 to
  2, redirections, an 8 MiB stack and the environment, runs pipelines at
  once, and sends Ctrl+C as SIGINT.
- **Ports** (P1, P2, X2): sbase (70 tools in three multi-call programs) and
  Lua 5.4.9 in `/boot/usr/bin`; libc-test (113 passing and 25 listed with
  reasons at first, 114 later); tinyssh 20261001 as `bin/sshd` (one program
  whose mode is `argv[0]`, key login only, a host key made at the first boot,
  `authorized_keys` in `/data/etc`, port 22 while it holds a key, `ssh = off`
  stops it).
- **Jam commands as programs** (the owner's decision, 2026-10-11): the shell's
  own commands also exist as programs in the boot image (`user/sysbox/`, the
  same source files compiled with `-DSH_BOX`), so a POSIX shell, a script or
  an `ssh` session can run them.
- **Mapped files** (F1b, after M12.5): read-only and shared mappings of the
  pager VMO, `MAP_PRIVATE` as a copy-on-write child, `msync`, and
  `posix_spawn` of programs on `/data` through the pager's loader.

The owner's answers (2026-10-09, "your recommendation for everything unless
unsure", all as recommended): unmodified musl with the build's overrides and
a Linux call layer; the line discipline in the terminal service with a `tty`
protocol; signals in libc at the call boundary plus `thread_interrupt` (a
kernel upcall was added later, under it, in SG); `posix_spawn` only, no
`fork` (later reopened: [FORK-PLAN](FORK-PLAN.md)); a file's offset per
process in libc; with `ISIG` off a program gets the byte, and Ctrl+Alt+C
always reaches the shell on the desktop; one user `jam`, uid and gid 1000;
sshd on port 22 whenever `authorized_keys` holds a key; sbase in
`/boot/usr/bin` after the shell's builtins; the `jamos-cc` wrapper.

The reviews (each by an agent that wrote none of it, `docs/M13-REVIEW-*.md`):
- **Kernel** (K1, K2): 0 High, 1 Medium, 7 Low; the Medium and two Lows
  fixed.
- **POSIX layer** (R1): 0 High, 7 Medium, 14 Low, 4 notes. The Mediums fixed
  with a test each (smaller programs, writable files handed to children, and
  the rest); the hand-over parser, handles on every path, the signal model
  and readiness were found sound.
- **Terminal, sessions, SSH and shell** (R2, 2026-10-13): 1 High, 4 Medium,
  7 Low. The High: **sshd's host key was readable by any program with `mount
  /data r`** (a view guarded `etc` against changes only). The key is now
  hidden from `/data` views and `kill sshd` was checked to end its sessions.
  The Mediums: a POSIX program's output came after the shell's prompt, typed
  keys went to a pipeline's last stage instead of its first, the console
  served requests without the unread bound, and a program silently lost
  arguments past 18.

Open: the join row of the plan is only partly done: no PORTING.md
(a guide to porting a program) was written, and the release `0.2.0` that the plan
tied to M13 has not been cut (the version is still `0.1.0`). M13.5, the
POSIX shell (mksh), is after Chromium. The milestone's PC sign-off ("All
tests and `soak 10`") is the main session's pre-push pass.

## M12.1: the code check

*2026-10-09 to 2026-10-11.* A check of what M12 and G1 changed, by fresh
agents that wrote none of it: findings listed first, then every High and
Medium fixed one commit at a time with a test that fails without the fix
(Lows when trivial; design questions reported and answered by the owner).
Rescoped on 2026-10-07 from a sweep of everything since M8.6 (about
166,000 lines) to where reading finds what tests don't: six parts, each
with a findings document in `docs/` (`M12.1-REVIEW-*.md`): the kernel
core, the kernel's devices, the compositor, the desktop's clients, M12's
protocol and storage lines, and init, devmgr and the namespace. The fixes
were all in the build that M12's PC sign-off ran.

What it found, by part:
- **Kernel core** (about 6,800 lines): one High. Any holder of a shared
  channel end could set `CHANNEL_NO_HANDLES` on it for everyone (every
  program's namespace holds a duplicate of init's published ends); the
  mark is now refused unless the caller is the end's only holder. Its
  design question, who pays for messages nobody reads, ended with the
  owner reversing M12's answer: the writer keeps paying until the message
  is read or dropped, also after the writer closes.
- **Kernel devices** (VT-d, kexec, the panic screen): no High, two Medium
  (an invalidation not confirmed after a failed map kept the pages;
  `reset=none` was not kept across kexec). Two owner decisions came out
  of it: a pin is charged to the driver that pins, and a read-only pin
  works only over pages already committed; and PCIe ACS is turned on at
  boot (root ports, switch ports, multi-function devices), and a device
  that a peer could reach without the IOMMU gets no domain. On the PC
  that refuses one function, the RTX card's HDMI audio (01:00.1), which
  has no driver anyway.
- **Compositor**: one High (Alt+Tab kept pointers to windows a client
  could destroy: a use after free that a hostile client could repeat in a
  loop), four Medium (animation snapshots up to 8192x8192, a late overlay
  taking the keys, unbounded minimum sizes, log floods).
- **Desktop clients** (libjwl, the console's window mode, libfun): one
  High (a stream of empty batches froze the compositor's loop), two
  Medium. A compositor that drops a slow client is now a lost connection
  the client reconnects from (50 ms to 5 s back-off), not the end of the
  program.
- **M12's protocols and storage**: one High. A client that never read
  fat's answers filled its job's message quota and took `/data` away from
  everyone. The fix is general: every generated server stops reading a
  client that has 8 answers or 8 KiB unread (`unread` in the IDL) until it
  reads them.
- **init, devmgr and the namespace**: one High, caused by that very fix:
  the shared client ends init keeps (audio, music, the network, the
  compositor) were held for good by the unread bound, so one flood
  starved every later opener. init now reads and drops what lands there.

The new hostile tests are worth more than the fixes: a seeded fuzzer of
compositor batches, `idl_fuzz` (a hostile message for every generated
dispatch), random system calls and channel races in the kernel, the
desktop under clients doing everything careless ones do while a user
types (`comp_chaos`), and live fuzzing of init, devmgr, fat and volumes.

Open when M12 was signed off, for a follow-up: init's own private channel
for its asynchronous calls (instead of publishing an end it does not
read), a cap of 64 requests a turn in devmgr's and volumes' serve loops,
the console's dead control channel and writers table, and three Lows (a
request waiting on a volumes that ended, devmgr's 30 s first pass, init
reading and writing `/data` settings itself).

## M12: the interface review

*2026-10-07 to 2026-10-11.* Signed off on the PC on 2026-10-11. The
system calls and the service protocols were reviewed by four fresh agents
(K: 38 findings on the system calls, 6 of them bugs; P: the protocols;
W: the compositor's; S: the storage split), the owner answered their
questions, and 14 fix tracks changed what was cheap to change while
nothing depended on it: before POSIX (M13) builds on these interfaces and
M14 freezes them. Nothing was frozen.

What changed:
- **Channels and handles.** Handles given to a send are always moved,
  whatever the outcome; each pair of ends has its own transaction counter;
  a channel end can be marked "receives no handles" (so a supervisor's
  queue never holds a channel end); `handle_info` tells a program what a
  handle is; five new error codes; one rule for time in arguments.
- **Memory, threads and DMA.** The FS base per thread and a futex (thread
  local storage, locks that sleep); read-only DMA pins; the table pages of
  an IOMMU domain charged to the driver's job.
- **genidl.** Variable-length arrays (`u8[<=N]`, `str[<=N]`), one-way
  events that may carry handles, several protocols in a file, typed handle
  arguments, one registry of protocol ids, and a fuzz table of every
  protocol. `console.write` no longer copies 2048 bytes for each line.
- **Protocols reshaped.** Input as events without the character (the
  keymap is libos's); the compositor's transport JWL2 (flow control by the
  kernel's count of unread messages); audio, network, USB and file
  protocols (paths as strings, batched `readdir`, exclusive create);
  initctl's strings; the keep channel and the namespace's messages as IDL.
- **Storage split.** A new service, `volumes`, holds the disks,
  filesystem services and mounts; devmgr keeps the drivers and nothing of
  storage, and its protocol is IDL with ids of its own. The storage
  protocols are shaped for GPT and NVMe. One supervision module serves
  devmgr, init and volumes, and neither devmgr nor init waits inside a
  request any more.
- **`nocomp` removed**, against the recommendation, at the owner's
  choice: the boot word, the "Jam OS (no compositor)" entry, the console's
  full-screen mode and libfun's borrowed screen are gone. The desktop is
  the only way Jam OS draws; the previous build is the way back from a bad
  build.

The owner's answers (2026-10-07; "your recommendation for the rest unless
unsure or very important", so all but one as recommended). The kernel: a
send's handles are always moved; **the writer keeps paying for a message
nobody reads** (first answered "the reader pays once the writer closes",
reversed on 2026-10-09 after M12.1's kernel review); `handle_info`; a
read-only DMA pin; a futex (private only, with requeue) and the FS base;
five new error codes; one rule for time in arguments (deadlines,
`sleep_until`); a dead service's kept objects stay charged to its job; a
`vmo_pin` argument struct with a flags word; a port binding ends when its
object has no handle left anywhere. The protocols: `u8[<=N]` and a checked
`str[<=N]`, scalar arrays (records stay byte arrays until M14), one-way
`event` methods that may carry handles, a protocol per kind of channel,
`const` lines in each `.idl`, typed handle arguments checked by the
generated server, the kernel dropping replies to calls that gave up, a
pipe as a channel of byte messages, libos's heap reserving 1 GiB, the
re-arm flag of `channel_reply_wait` left for the speed pass. The
compositor: events go from server to client, `notify` split off, flow
control by the kernel's count (JWL2), at most 28 handles queued a
connection. The storage split: the new service named `volumes`, one shared
supervision module, disks reaching it on fresh `disks` pairs, **a volumes
crash ends its filesystem services and every stick remounts fresh** (a
service that outlives its process waits for the drivers' milestone), and
the disk protocols shaped for GPT and NVMe with nothing built for them.
The one against the recommendation was `nocomp`, above. M12.1 is M12's
independent review.

Where it lives: the channel, handle and wait rules in `kernel/object/`
(`channel.c`, `channel_send.c`, `handle.c`, `port.c`) and `kernel/sched/`;
the calls and their contracts in `abi/syscalls.def` and `<jam/abi.h>`; the
IDL's new shapes in `tools/genidl.py` and `abi/idl/` (every protocol id in
`abi/idl/ids.txt`); the supervision module in `user/lib/supervise.c`; the
storage split in `user/services/volumes/` and `user/services/devmgr/`; the
compositor's transport in `user/lib/jwl_transport.c`. The four reviews
(K, P, W, S) are in `docs/history/`.

The PC sign-off: the build updated over the network, ACS isolation, `storm`
at 10 and 100 kills a second (the copy matched), a stick pulled and put
back, the network and sound, utest from the shell (594 passed), All tests
with no problems, and `soak 10` passed (832 s on 28 CPUs, 3,176 kernel
tests, 5 utest runs, 2,679 file cycles, 0 failed). `bench` was unchanged
from 2026-10-07: reply-and-wait 765.5 ns, switch 30.2 ns, process to
process 1089.3 ns ([BENCH.md](BENCH.md#m12-on-the-pc-2026-10-11)), so the
review cost the call path nothing.

What the last day taught:
- **QEMU still hides what a real stick does.** A real SuperSpeed stick
  answered the first command of a new owner of its bulk pipes with a
  Transaction Error: the controller restarts its toggles at 0 and the
  device keeps what the last owner left. usb-bus now clears both
  endpoints' halt when it opens a pair, and usb-storage and `usbtest` run
  Bulk-Only reset recovery before their first command.
- **The RESULTS box was full.** The PC's many device lines used up the 24
  lines every program shared, so utest's, usbtest's and posixtest's
  summaries (and init's "exited with code" lines) never reached the box.
  It holds 256 lines now (the first 192 and the newest 64) and says how
  many fell in between.
- **Tests that assumed an environment.** posixtest run from the shell
  assumed descriptor 0 was `/dev/null` and that `/svc` existed; the soaks
  timed out at 600 s under the PC's load though every loop reported 0
  failures (the limit is 1500 s).
- **A pipe 64 MiB ahead of its reader made a write fail.** A pipe is now
  full at 1 MiB unread (`CHANNEL_BYTE_BOUND`), so the writer waits. And a
  panic in `thread_reap` at the end of a test run (the boot thread's reap
  put a NULL stack back into a full cache) was found by the init run at 2
  CPUs and fixed before the push.

## M11.6: services that outlive their process

*2026-10-03 to 2026-10-05 built, 2026-10-11 on the PC.* A service's state
lives outside its process, so a restarted service carries on where the
dead one stopped and its clients see no error: first for `fat` (one per
volume) and the mixer, the drivers being a later milestone.

How it works: a keeper (devmgr for the filesystem services, init for the
mixer) holds the service's channels and a state VMO. fat keeps every open
file, view and the writes it holds back in 1.2 MiB of state at a fixed
address; every request is an operation that can be rerun (an undo copy, the
writes held, a commit, the send, the answer), and the state records
whether the answer left. A warm spare (`fat-spare`, one at a time, about
100 KiB) waits to be promoted. The mixer keeps its streams, its openers
and its position in the sound ring, and mixes the periods since its last
commit again. The kernel sets a mark when a reply goes out with the next
`channel_reply_wait`, which closes the window where a duplicate reply could
be sent. usb-storage got a fence so that a write cannot land after the
service that asked for it was killed. `storm <from> <to> <kills/s>` copies
a file while killing fat again and again and prints the throughput beside
the kill rate; `tools/fatcheck.py` then checks the stick's FAT.

In QEMU (with three or four other runs going): killing fat to its first
answer took a median of 10 ms at 1, 10 and 100 kills a second; a 32 MiB
copy matched at every rate and ran at about a third of its speed at 100
kills a second (4.0 MB/s against 12-15 at 1 and 10); the mixer was back in
4-6 ms with at least 123 ms of sound in hand, so no period was late. The
independent review found no High and two Medium: a truncate to 0 that goes
out in steps could leave a directory entry pointing at freed clusters, and
a program allowed to play sound could make every later mixer restart fail
until the next boot.

On the PC (inside M12's sign-off, 2026-10-11): `storm` copied a 12.6 MiB
file with the stick's fat killed at 10 and at 100 kills a second, and both
copies matched. The restarts were slower than in QEMU: 123 and 205 kills
landed (6.7 and 11.8 a second), a median of about 60 ms from the kill to
the first answer (p99 0.6 s and 0.33 s), and the copy ran at 0.72 and 0.76
MB/s. Not yet run on the PC: 0 and 1 kills a second and `storm mixer` by
ear. In QEMU a promoted spare was no faster than starting a process;
whether the spares earn their memory is still open.

## G1: the desktop

*2026-10-05 to 2026-10-07.* A compositor of Jam OS's own that speaks the
Wayland protocol over channels (the design as built is in
[ARCHITECTURE.md](../ARCHITECTURE.md#boot); the keys are in
[USING.md](USING.md)): dwindle tiling and floating windows, virtual
screens made as needed, the frosted top bar with its popovers, search,
Alt+Tab, notices (held until the splash is over), up to 16 terminals
with copy and paste, Jamjar as a window, the splash as a boot overlay, a
calm panic screen, cursors of its own, and a layout that survives the
compositor's own restart. Signed off on the PC (now in its new case):
the checklist (hostile programs against the live desktop, the desktop by hand, the tests), All tests, `soak 10`
(3672 kernel tests, 8 utest runs, 2253 file cycles, 0 FAILED, the IOMMU
on by default), and a `soak 2` after the last fixes (+6.9 KB of message
bytes). 502 commits pushed after the full Mac regression.
G1 kept a boot entry without the compositor (`nocomp`) until its sign-off;
M12 removed it, with the full-screen console it booted.

What the last day found:
- **X1's hostile clients, run against the live compositor, found a
  leak the headless tests hid**: a port binding holds its object and its
  1216-byte charge until it is unbound, and closing the handle doesn't
  unbind it. The compositor leaked one per client it lost; the sweep that
  followed fixed the same mistake in the clipboard, dns, music, netstack,
  the mixer and five drivers. (M12 makes a binding end with its object's
  last handle: review K's finding 26.)
- **Keys typed during boot were lost** past 128 events (the compositor's
  early-key buffer) and 256 (libjwl's queue): both fixed, with a
  typeahead test.
- **The desktop's paint threads share the CPUs** with tests that assumed
  an idle machine: the pathstat tests now measure again when their CPU was
  shared, and the staged scheduler races run above every user thread.

## M11.5: the performance pass

*2026-10-02 to 2026-10-07.* The IPC fast path, as M5.5 was the first pass,
before M12 reviews the interfaces and M14 freezes them. The target was a
process-to-process call at 600 ns on the PC (1533 ns at M8.6). Reviewed
(no High or Medium finding), measured on the PC on 2026-10-04 and again
with every follow-up on 2026-10-07 ([BENCH.md](BENCH.md#m115-with-every-follow-up-pc-2026-10-07)).

What was built, by track:
- **Stage 0, the probes**: per-call counts (system calls, copies, locks,
  scheduler passes, FPU saves, CR3 loads) and timelines of one round trip,
  `bench`'s `path:` lines, so each change could be checked against a count
  before a stopwatch.
- **The lock checker's fast path** (no interrupt toggles on every lock; the
  held list a stack): the spinlock pair from 26.7 to 13.6 ns.
- **One system call for a server's turn**: `channel_reply_wait` sends the
  reply and waits for the next request (or a port's packet) in one call;
  a reader waiting in it is handed the message directly (the hand-off), so
  a round trip against such a server went from 5 system calls to 2.
  genidl's generated servers use it, and `_within` calls carry a timeout
  without a clock read.
- **One copy in, one out** (the message built in the kernel stack buffer
  is gone) and a per-thread message slot for a message to a waiting reader
  (no allocation or charge).
- **No FPU save on a voluntary switch**: a switch inside a system call
  keeps only MXCSR and the x87 control word, as a C call does.
- **Per-operation lines** (`perop`: a cached 4 KiB read, a block read) and
  `tools/linuxbench` for a Linux column on the same PC (not yet run on a
  real Linux).

On the PC: the context switch regression is gone (92 ns at M8.6, 30.1-30.8
ns now, M5.5's number), most likely through the lock checker's fast path
which took the interrupt toggles off every lock. A call to a
reply-and-wait server takes **765 ns**: 165 ns over the target. The owner
settled on 2026-10-07 that the target is read on that line with the lock
checker on (checker off it would be about 670 ns), and the last 165 ns
is a later speed pass. bench-echo's old process-to-process call went from
1533 to 1089 ns.

What it taught:
- **The hand-off gained nothing at first** (886.7 ns off, 884.8 on),
  though the counts showed it firing: the handed wake still took the run
  queue lock and the woken reader took its endpoint's lock again. Taking
  those out (15 locks a round trip to 11) made it a win (765 against 786)
  and the slots saved another 84 ns.
- **Count first, time second.** The counts are exact in QEMU, where the
  times are not, and every change was checked against its count before the
  PC was asked.
- **QEMU cannot measure page-related lines**: stores to some physical
  pages cost 100-400 times a normal page there.

## M11: the IOMMU

*2026-10-04 to 2026-10-07.* A device can reach only the memory its driver
pinned for it and can raise only the interrupts it was given: Intel VT-d,
DMA remapping and interrupt remapping, behind `dma_cap`. Signed off on the
PC on 2026-10-07 and on by default since (`iommu=off` turns it off);
reviewed (no High, three Medium, eleven Low). With it, drivers count as
contained, not only crash-isolated.

What was built:
- **Stage 0, a read-only probe** run on the PC first: one remapping unit
  at 0xfed91000 covering every PCI function, no RMRR, 256 domains,
  4-level tables, queued invalidation, interrupt remapping with x2APIC
  ids, page walks not coherent with the CPU's caches (so every table line
  the kernel writes is flushed), and at a cold boot everything off.
- **One domain per `dma_cap`**, holding exactly what it pinned (IOVA =
  physical address); a function with no driver sits in a blocking domain;
  RMRR regions get boot domains; the table pages are charged to a job
  (M12 moved the charge to the driver's). A closed cap's domain is
  quarantined until the unit confirms it gone.
- **The unit**: an invalidation queue with a status slot per waiter (100 ms
  deadline, no lock held while waiting), the fault interrupt and a fault
  ring, a driver muted after 8 faults. The takeover of a unit the firmware
  left translating, and the jump off before a kexec or a panic so the next
  kernel starts clean.
- **Interrupt remapping**: one table of 1024 entries shared by all units;
  MSI and the I/O APIC's pins validated against the requester.
- **Checks and tests**: hda's `vtdtest` word reads a ring from a page the
  driver unpinned and points the response ring at the interrupt window;
  the boot entries "Jam OS (IOMMU)" and "IOMMU checks"; the `iommu`
  command; every device's area test run with the IOMMU on in QEMU
  (`QEMU_IOMMU`). Every encoder was checked bit by bit against Intel's VT-d
  4.1 specification before the pieces were wired together.

On the PC: the checks entry shows the blocked read and the blocked write
to the interrupt window (fault reason 25h, "compatibility-format interrupt
blocked"); every device worked with no fault; All tests with it on (382
passed) and `soak=10 iommu=on` passed (4026 kernel tests, 9 utest runs,
2186 file cycles). A pin and unpin of one page costs about 5 us (an
invalidation wait each way), 64 pages 23 us, so 0.36 us a page batched.

What it taught:
- **QEMU's unit is not the PC's**: caching mode on, an explicit scope per
  function instead of INCLUDE_PCI_ALL, and old-format writes to the
  interrupt window pass through, so the window check is only the PC's.
- **The review's Mediums** were all about a unit that does not snoop: the
  root table's cleared page was never flushed, a storm of fault events
  needed the interrupt masked and the log thread polling, and a domain
  the unit does not confirm gone must keep its pages (tried again every
  second).
- **A function's fault count must start again at each attach**: a new driver
  after one that had faulted 8 times was muted at its first fault.

## M9.5: network streams

*2026-10-02 to 2026-10-04.* TCP and sockets, built once properly on M9's
stack. Reviewed (nothing High or Medium). Every frame is still in the
configured network mode.

What was built:
- **Socket data on shared rings.** Each socket is a pair of shared-memory
  rings to netstack (as netdev and the mixer), UDP too; setup stays IDL.
  Fair shares of the rings, with a reserve for the network's own services.
- **TCP** in lwIP with stream sockets (`connect`, `listen`, `accept`,
  `send`, `recv`, `shutdown`, `close`), backpressure by ring room (netstack
  never waits on a program), per-program limits.
- **Waiting on many sockets** (`<netwait.h>`, on the kernel's ports), the
  groundwork M13's `poll` and `select` stand on.
- **Listening is a permission of its own**: `svc net listen` in a
  program's list, approved with `allow`; nothing listens by default.
  Ports below 1024 are another (`net-low`).
- **Programs**: `fetch` (plain HTTP downloads), `serve <file> [port]` (one
  file shared read-only from the background), `speed` with `tools/speed.py`
  on the Mac, and SNTP, which sets the clock from the network.
- **The RTL8125 driver fails closed** (found by the M9 review): it stops
  the chip and exits when its tally shows a frame it did not queue.

On the PC the first `speed` over 5 GHz Wi-Fi gave 8.9 MB/s out and 9.0
in: 64,240 bytes (the unscaled window) every 6.4 ms round trip is about
10 MB/s, so the window was the limit, not the link. **Window scaling** (shift 6, up to 2 MiB) with 2
MiB rings for the bulk programs, and **SACK with out-of-order segments
kept** (lwIP dropped everything past a hole), then gave 34.7 MB/s out and
11.6 in; UDP ran at 47 MB/s, the Wi-Fi's limit. netstack's worst-case
memory went from 17 to about 30 MiB.

No separate PC sign-off was recorded for M9.5 or M9. M9's features and
M9.5's `speed` and `sntp` ran on the PC, and every later PC sign-off (M11,
G1, M12) ran `soak 10` on a plain boot, with the network up and netlog
sending the log to the Mac. No PC result is recorded for `fetch` and
`serve`.

## M9: networking

*2026-10-02.* Jam OS on the network, through its own driver for the
board's Realtek RTL8125B and a network stack in a process of its own.
Reviewed (no High; Mediums such as netstack's loop skipping its port wait
while a channel had work, and kexec dropping the network word so the PC
came back without its network).

What was built, in stages:
- **The RTL8125 driver** (`drivers/rtl8125`) and QEMU's e1000e for the
  tests, behind one netdev contract of shared rings. **The VLAN rule**,
  enforced in the drivers: every frame goes out in the configured mode only
  (the owner's builds: tagged VLAN 21; a public build: plain untagged,
  never a tagged frame) and other frames are dropped; `vlan=off` leaves the
  card alone. The default is the build's: `local.mk` sets the owner's.
- **netstack** (lwIP: IPv4, ARP, ICMP, UDP) with `/svc/net` sockets;
  **dhcp** and **dns** as processes of their own (`host`, `ping <name>`),
  a slow peer delaying only its own requests; **netlog**, the whole boot
  log sent to the Mac as it is written.
- **`update`**: the Mac's build fetched, its manifest's Ed25519 signature
  checked against the key in the running build (90 us on the PC), then each
  file's size and hash, and run by kexec in RAM; `update -w` also writes
  it to the stick, keeping the previous build as a boot entry. The first
  signed build went on by `make flash`; the manifest format is stable
  since (new lines are skipped by builds that do not know them).
- `tools/net-vlan-test.sh`: one QEMU boot with every path that transmits,
  whose capture holds nothing but the configured mode (7,129 frames, all
  tagged 21), and the same commands with `vlan=off` sending nothing.

On the PC (2026-10-02): a listen-only probe first (the chip, the link at
1000 full in 2.2 s, a trunk port carrying VLANs 10, 11, 20 and 21 and
untagged traffic, wake-on-LAN left armed by the firmware), then sending,
receiving, `ping 1.1.1.1`, `ping google.com`, a DHCP lease, netlog and
`update` from the Mac, twice in a row.

What the PC taught:
- **A descriptor of 16 bytes, not 32.** With the chip's extended mode
  bit set, it steps through the transmit ring 32 bytes at a time and read
  only every other descriptor of the driver's. Half the frames went out,
  the driver waited for descriptor 1 for good, and a doorbell storm sat on
  top.
- **The receive address is written again at every hand-back.** Receive
  stopped about 75 s in, after the chip's first lap of 256 descriptors: it
  shares the descriptor's address field with a timestamp, so a frame was
  written wherever that pointed. 7,609 frames in 148 s, none missed,
  after the fix.
- **`update -w` did not finish in 400 s on the PC.** The ESP's clusters
  are one 512-byte sector, so fat sent about 40,000 one-sector writes
  (445 s in QEMU with a stick throttled to 100 writes a second). Writes of
  a file opened for gathering are now held and sent in 64 KiB runs, unlinks
  batched, and usb-bus's bulk buffer is 68 KiB: 5.9 s in the same QEMU
  test, 31 s on the PC.
- **The PC's port is a trunk**, so Jam OS drops what is not its mode; the
  switch could enforce the same rule as well.

## M8.6: cleanup and polish

*2026-10-01 to 2026-10-02, 0.0.28-m8.6.* Signed off on the PC:
`soak 10` passed (663 s on 28 CPUs, 4525 kernel tests, 17 utest runs, 3443
file cycles, 0 FAILED) with the SanDisk mounted read-write, live `ktest`
248 passed, utest 117, and `bench` run on the PC for the first time since
M5.5 ([BENCH.md](BENCH.md#m86-pc-2026-10-02): every line through a context
switch is slower than at M5.5, to find before M11.5). Before that, a full
QEMU regression of the merged tree: ktest 260 at 4 and 8 CPUs, utest 117,
every area test and shell script.

In order:
- **The independent review of M8.5:**
  the stored kernel's region down to 32 MiB, the crash loop at three
  panics in a row.
- **Track A:** the keyboard and mouse ready 0.29 s after the kernel's
  start on the PC (6.3 s before), keys typed during the splash kept for the
  shell; a write-through block cache with read-ahead (`mount -w` of a
  15 GB stick from 20.4 s to 0.35 s); the boot disk is the one Limine
  booted from; Ctrl+Alt+Del's wait; a `make flash` that checks what it
  wrote; a 4 MiB log ring; linear FAT directory cursors.
- **Track B:** `/svc`, a per-program view of the services it may open;
  a program's list of what it wants (an ELF note, checked at build time),
  approved by the owner with `allow` (SHA-256 of the file), which is also
  what lets a program on `/data` run; `play` and jamjar's cover decoding
  in small programs of their own; the root resource's powers split (the
  log, the serial port, the clock, reboot, kexec, debug).
- **Track C:** the kernel log off the screen on a plain boot, with a
  short list of notices instead; UTF-8 in the log; `kernel load`; the
  real date and time from the RTC with time zones; `/data/etc/settings`.
- **The code check:** five fresh agents over the whole tree, reviewing
  and fixing (kcore, krest, drivers, services, libs).
- **The pre-M9 batch:** the
  service-loop rule (a loop serving several clients never blocks inside a
  request) with its tools (genidl's deferred replies and asynchronous
  calls, libos's tasks); devmgr channels scoped to one device in place of
  the sound card's special case; a channel of its own for each opener of
  `/svc/audio`, `/svc/devmgr` and `/svc/logd`, and at most 4 streams each;
  a VMO made executable is sealed; the screen and the serial port's
  output get root powers of their own; the kernel marks each log line
  with its writer, so only the real init, devmgr and logd make notices;
  programs on `/data` never get `right debug`; init never gives up on
  the console or the shell; driver isolation stated honestly (crash
  isolation, not containment, until the IOMMU) with a threat model.

What it taught:
- A bump allocator for kernel virtual space strands page tables: ktest
  at 8 CPUs reported 3 pages leaked only once bootfs grew past a size.
- Waiting for USB to settle before input was most of the boot: the
  devices were ready in a fraction of a second.
- A key typed in the same instant as Enter can arrive after a question
  it was typed before: what the shell can promise is only about keys
  that have reached it.

## Audio (A1, A2, AS) and M8.5 (kexec)

*2026-10-01, 0.0.25-m8 to 0.0.27-m8.5.* Signed off together on the PC:
All tests with no problems and `soak 10` passed (645 s on 28 CPUs, 4495
kernel tests, 19 utest runs, 2929 file cycles, 0 FAILED).

Audio, all in user processes, on the PC's Realtek ALC897:
- **A1:** `drivers/hda`: controller reset, CORB/RIRB, the codec's widget
  graph, a path finder to the front headphone jack, an allow-list of SET
  verbs, one output stream with MSI and clear-behind, jack detection
  (unsolicited responses with a polling fallback), `beep`, `hda`. The
  output stage goes on once at the driver's start with every amp muted
  and settles 400 ms, so no stream starts with a thump.
- **A2:** the mixer service (client rings in shared VMOs, Q15 gains,
  24-bit output, dither, a limiter), `<audio.h>` with a Kaiser-sinc
  resampler, `play` (WAV and MP3 through dr_mp3), `vol`, the background
  `music` player owned by init, and jamjar, its full-screen remote: the
  library from the stick, real album covers from ID3 tags (stb_image),
  stereo spectrum bars and a sunburst.
- **AS:** the boot splash: the owner's 2560x1440 animation as MPEG-1
  with MP2 sound (pl_mpeg), picture and sound starting together, the
  kernel quiet behind it.

M8.5, after a first version as Linux's kdump and the owner's Revision 2:
one kernel with two ways in. A fresh copy of
the system waits in reserved RAM (unmapped, checksummed). `reboot` and a
panic both jump into it: the screen goes to the splash background, the
next boot is a normal one, and after a panic logd saves the dead boot's
log first and the shell says so. A second panic within 30 s halts on the
panic screen. The kernel starts the other CPUs itself (INIT-SIPI-SIPI),
which a kexec'd kernel needs.

What the PC taught:
- The AP trampoline's GDT descriptors need their accessed bit set: the
  CPU writes it into a read-only page otherwise, and the triple fault
  reset the PC before anything was on the screen. QEMU's TCG never sets
  the bit, so every QEMU test passed.
- A reboot waited 30 s for the hda driver, whose client (the mixer) was
  still running; QEMU's tests had no sound card. Stopping the console
  instead made the kernel redraw its log over the blank screen.
- Real mice send the wheel only in the report protocol: the boot
  protocol has three bytes.
- At 2560x1440 jamjar's picture cache filled the heap after ten albums.

## M8: storage

*2026-09-30 to 2026-10-01, 0.0.25-m8.*

USB mass storage and FAT32, all in
user processes:
- `drivers/usb-storage` (Bulk-Only Transport over new bulk transfers in
  usb-bus) serves a `block` channel per partition; read-only channels are
  refused writes in the driver itself.
- FatFs R0.16 (third_party/fatfs, with its author's patches) as the `fat`
  service, one per volume: the ESP read-only at `/esp`, the data partition
  at `/data`, formatted on first use (only the boot stick's own blank
  data partition is ever formatted).
- devmgr finds disks and partitions and publishes mounts; init keeps the
  system's file namespace and sends each program its mounts (as one
  whole-namespace message, so a program that never looks is never
  flooded); programs open files by path; the shell's file commands
  (`ls`, `cat`, ...) work on every mount, and it got `mkdir touch write
  cp mv rm df sync mount`.
- logd saves each boot's log to `/data/logs/boot-NNNN.txt`, syncing every
  250 ms and flushing before a reboot.
- Other sticks mount read-only at `/usb0`, `/usb1`, ... (`mount -w` to
  write); a stick with no partition table works too.
- `make usb` makes the two-partition stick; `make flash` updates one in
  place (macOS doesn't mount an MBR ESP by itself).

Tracks A (usb-storage), B (fat), C (namespace, init, shell) and D (devmgr,
logd) in parallel, a join, other sticks, then the review (28 findings;
three High: fat mapped a client's buffer, so any program with `/data` could
crash it; a partitioned stick whose first sector looked like a FAT volume
was served whole; and the Alder/Raptor Lake INVLPG erratum, which old
microcode leaves open while PCIDs are on, so the kernel now leaves them
off there). The PC rounds found more: a second `ktest` in
one boot always panicked (a test's static fake CPUs), the boot log lost
its tail, two leaks (mount notices nobody read, 30 KB per utest run; the
user heap never merged freed blocks), a global fault hook that failed
other threads' process starts, and a placement test failing on the PC
that turned out to be two scheduler bugs (a CPU taking its next thread
looked idle to placement; stealing ignored the core layout). The soak
test (`ktest loops= seed= keep load`, `soak`) was built to find this
kind of bug, and replaced the stress test as the PC's tier.

On the PC (an i7-14700, Raptor Lake, with microcode 0x11f: the erratum is
fixed there, so PCIDs stay on): the stick's `/esp` and
`/data`, a SanDisk at `/usb0` read and written (macOS's check clean
afterwards), the boot stick pulled mid-soak and replugged (the soak
passed, the volume came back dirty and was mounted), the logs read on the
Mac; All tests 224 passed (27 clients: 975,166 calls/s), `stress 600`
passed, `soak 10` passed (645 s, 21 loops: 4285 kernel tests, 19 utest
runs and 1889 file cycles, 0 failed; the SanDisk pulled twice).

## M7.5: cleanup

*2026-09-30.*

Readability and
structure with no behaviour change: same commands, output, boot menu,
ktest names, syscall numbers and IDL; the version string stayed 0.0.24-m7.
Five parallel tracks, then one pass for the moves that would collide:

- The kernel build of drivers went (the kernel-process machinery,
  `drivers=kernel`, the xhci-noop driver and its `xhcitest` entry, the
  weak-symbol stubs: a missing syscall handler is now a link error).
  `<jam/driver.h>` has one implementation and gained `drv_snprintf`.
- The shell got one command table and one file per command; usb-bus's
  1834-line enumeration file was split by step; the kernel was split by
  subsystem (`sched`, `proc`, `debug`) with tests named by subject; user
  programs by role (`services`, `apps`, `tests`) on libos and libfun.
- CODING-GUIDE.md; `-Wvla` everywhere and `-Wframe-larger-than=3072` for
  the kernel.
- 66,065 lines in 237 files became 64,094 in 326; files over 800 lines
  20 -> 13; ktests 227 -> 219 (the kernel build's tests went with it).

Bugs found and fixed on the way: the shell wrote one past its segment array
for 32 segments ending in an unclosed quote; usb-bus left a slot enabled on
an out-of-range slot id, parsed an alternate setting without its
SuperSpeed companion, and didn't clamp `bMaxBurst`; the dma_cap quarantine
counters could be read mid-update (an 8-CPU ktest failure); Tab completion
ignored the line cap.

After the tracks: an independent review (no High findings), then two
style passes that brought kernel/, drivers/ and user/ in line with
CODING-GUIDE.md (every struct field commented, nesting over 3 levels gone,
`volatile` kept for device and loader memory with explicit atomics
elsewhere, the long functions split into named steps) and a docs overhaul
(one home per fact, `tools/checkdocs.py`). The repository went public on
GitHub under the BSD-2-Clause licence.

Behaviour changes accepted on the way (not bugs): usb-bus now counts every
endpoint descriptor of an alternate setting against its 8-endpoint limit
and uses only the first copy of a duplicated (interface, alternate)
descriptor; the demo, rebuilt on libfun, refuses to start without the
keyboard, needs a screen of at least 320x200 and names its threads
"worker"; `sys_pci_enum`'s `dma_quarantined` also counts batches that are
mid-release.

The last fix round of the day: the flaky devmgr ktest (it read devmgr's
job usage before devmgr was back asleep), an unbounded HPET calibration
loop that could hang the boot, `stress` failures that stuck across runs,
a devmgr handle leak, the console letting one flooding client starve Ctrl+C
and the others, the console ignoring failed port binds, and the shell's
pipe drain (no limit, deaf to Ctrl+C, stuck on an oversized message).
ktests 219 -> 221.

Signed off on the PC the same day: All tests 221/221, the shell's `ktest`
212 passed, the 10-minute stress passed. The PC round found two tests that
were wrong, not the code: `stress_failure_does_not_stick` saw 4 pages kept
(the first stress run on 28 CPUs grows per-CPU caches to a high-water
mark; the test now checks a third, identical run gives every page back,
through the new `KT_OWN_LEAK_CHECK`), and `serial_irq_drains_ring` expected
the shared COM1 ring to empty while the console kept it full from the shell
(its counts are now boot-menu-only checks).

## M7: USB, console, shell

*2026-09-29, 0.0.18-m7a to 0.0.24-m7.*

Four tracks, then the join:

- **usb-bus**: xHCI, hubs (inside usb-bus, as Linux's usbcore does),
  Transaction Translator routing, hot-plug. On the PC it enumerated all 8
  devices, 0 failed, the first time.
- **hid**: boot keyboard and mouse on each interface of composite devices,
  US layout, key repeat, Ctrl+Alt+Del.
- **console** owns the framebuffer (the kernel's log mirrored in it) and
  **shell**: line editing, history, then ~60 commands with pipes,
  variables, aliases, Tab completion and Ctrl+C.
- Kernel services: reading the kernel log, the framebuffer hand-off,
  `debug_command` (ktest, bench and stress from the shell), `reboot`,
  COM1 input.
- **Supervision and safe rebind**: devmgr restarts dead drivers with
  backoff; only a driver that quiesced its device turns bus mastering back
  on; pins a dead driver left are quarantined. This fixed M6's confirmed
  stale-DMA-after-rebind finding.
- Apps with real pixels through `console.lend_screen` (0.0.24-m7): fractal
  (AVX2, deep zoom to 1e28 with double-double perturbation), life, tetris.

On the PC: typing into the shell with the real keyboard, `kill hid-10:0`
recovers, reboot and Ctrl+Alt+Del. The PC run found that init didn't
restart devmgr (killing it lost USB until a reset): fixed, devmgr is back in
~0.6 s. The review found that kill never reached ancestor jobs, a set of
usb-bus robustness gaps, and an authority hole: programs started with `run`
got devmgr's channel and a full console channel (keyboard hijack,
Ctrl+Alt+Del, keylogging). Fixed with separate query and control channels
for devmgr and console client levels (ADMIN, SHELL, PROGRAM). `ktest` from
the shell panicked on a busy system (global counts), so tests now relax
system-wide checks when run live. Signed off by `stress 600` from the shell
on 0.0.23-m7: 0 failures. On 0.0.24-m7 `ktest starvation` passed on the
PC (so 0 anti-starvation boosts in PC stress runs is legitimate: 28 CPUs
steal waiting threads first).

## M6: PCI, MSI, devmgr, drivers through handles

*2026-09-29, 0.0.10-m6a to 0.0.17-m6.*

The kernel's PCI core (ECAM, BAR
sizing, MSI/MSI-X, Bus Master Enable), per-CPU vectors and interrupt
objects, resources, physical VMOs, `dma_cap` bound to one function, the
config-write filter, `<jam/driver.h>`, the IDL generator, the driver build
check, devmgr, and the `edu` (QEMU) and xhci-noop drivers.

The done test passed on the PC: the Intel xHCI completed No-Op commands
with a real MSI, both as a kernel process and as a user process (median 46
us: the controller's 40 us interrupt moderation). Bugs only the PC showed:
a test assumed QEMU-only devices; a BAR neighbour check was too strict; and
the VMD controller's 64-bit BAR has a hard-wired-zero upper half, which
`pci_size_bars` sized as ~2^64 (now implausible sizes are logged and left
unsized). A pre-existing object teardown race was fixed on the way. The
second review confirmed stale DMA after a rebind, fixed in M7. All tests
201/201; one 10-minute stress (0 failures) signed off M5.5 and M6 together.

## M5.5: performance pass

*2026-09-29, 0.0.9-m5.5.*

Spin before idle, hybrid P/E placement, client/server pairs on sibling
hyperthreads, per-CPU kmalloc magazines, per-CPU one-shot timers,
interrupt-driven serial output, PCIDs and XSAVEOPT; each with a run-time
switch and a boot word, and the benchmark measures each line off and on in
one run. On the PC ([BENCH.md](BENCH.md)): cross-CPU wakeups 2-2.4x faster,
kmalloc 4.3x faster (about 800x with 28 CPUs at once), timers 350 ns late
instead of up to 10 ms, and M5's cross-CPU regression gone (a wake-affine
scan of every CPU's struct). PCIDs gave only 2%.

The first real PCID run passed after fixing a test-only expectation. The
review found a far-future deadline that wrapped (a user-triggerable timer
storm), a redundant spin-idle IPI and a missing PCID flush after a switch
flip, and fixed an old flake ("counter#14 made no progress"): mutexes now
hand off to the longest waiter after 1 ms.

## M5: ring 3, processes, init

*2026-09-29, 0.0.7-m5-phase1 to 0.0.8-m5.*

Phase 1 in three tracks: the entry
path (`syscall`/`sysret`, `swapgs`, SMEP/SMAP, user copies with a fixup
table, eager XSAVE), address spaces and VMARs, and the userland build (the
syscall table generator, bootfs, the ELF loader). Phase 2: processes,
threads, jobs and quotas, userboot, init, utest, per-CPU page stashes and
wake-affine `channel_call`.

The phase-1 build passed All tests and the 10-minute stress on the PC:
the first ring-3 code there. The review found no crash or cross-process
access but four quota escapes (VMO table pages and user page tables not
charged, unbounded job chains, a process raising its own job's limit) and
four smaller issues; the fixes added `RIGHT_MANAGE`, a job depth cap, a
charge for every kernel allocation a process can cause, and `job_kill`.
The PC then found two more: a test race with lazy thread reaping, and a
dead process's thread credited to its job after the unlock (the stress
failed at 14 s). Final PC run: init + utest with the root job clean, All
tests 131/131, the benchmark, 10-minute stress 0 failures. Per-CPU page
stashes took page alloc+free with 28 CPUs at once from 19 us to 24 ns.

## M4.5: hardening

*2026-09-29, 0.0.6-m4.5.*

From a conformance review: W^X also through the HHDM alias of the kernel
image; received handles no longer dropped when the table is full; a
channel "has room" signal; the lock checker got a lockless fast path, 256
classes and sleeping mutexes; cancellable waits (needed to kill a process
blocked in `channel_call`); `make KTESTS=0`. On the PC: 78/78, one server
with 27 clients at 836,077 calls/s (M4: 492,673), worst call 37 us,
10-minute stress passed.

## M0 to M4

*2026-09-28.*

- **M0**: toolchain, QEMU q35 + OVMF, the USB image, framebuffer console,
  panic screen; booted on the PC.
- **M1**: physical and virtual memory, heap, GDT/TSS/IDT, the panic screen
  with symbols.
- **M2**: ACPI tables, LAPIC (x2APIC and xAPIC), IOAPIC, TSC and APIC
  timers, all cores, P/E topology, loader memory reclaimed. On the PC: 28
  CPUs, each exactly 100 ticks, TSC-deadline. xAPIC mode only reached 20
  CPUs, so x2APIC it is.
- **M3**: scheduler, kernel threads, ticket spinlocks and the lock-order
  checker, IPIs, TLB shootdown, watchdog, the stress test. The first PC
  stress failed at 230 s on an IRQ race in the checker; after the fix the
  10-minute stress passed (112 threads, 28 CPUs).
- **M4**: objects and handles, channels, ports, events, timers, VMOs; a
  two-part audit and 20 fixes. PC: 492,673 calls/s, worst 67 us.

## Decisions

Dated decisions, newest first. The design they produced is in
[ARCHITECTURE.md](../ARCHITECTURE.md); this is the when and why.

- 2026-10-07, during G1: the owner removed the apps he considers bloat:
  sysmon, snake, tetris, mines, life and the visual demo (with the shell's
  `sysmon` and `demo` commands). The apps left are jamjar (with its
  helper jamcover), the splash and the terminal; the search box lists
  Terminal and Jamjar. fractal became a test program
  (`user/tests/fractal`) for its workload (vector maths on every CPU, its
  benchmark, its self-test); the tests that used the others moved to it
  and to the test window wltest (`wltest --mouse`). The shell's prompt
  became `jam:<cwd>>`.

- 2026-10-01, after the PC sign-off of the audio track and M8.5: M8.6's
  decisions: programs on `/data` run only once the owner marks them with `allow`,
  which also approves what each declares it wants (services under
  `/svc`, mounts); the splash always plays to its end; `play` and
  jamjar's cover decoding move into small programs of their own; the
  kernel log leaves the screen; the block cache is write-through; the
  real date and time, a split `RIGHT_READ` and a settings file come
  before networking.
- 2026-10-01, after the first PC run of the crash kernel: M8.5's
  Revision 2, one kernel with two ways in. There is no crash-kernel mode:
  a `reboot` and a panic both start a fresh copy of the same system as a
  normal boot, which after a panic saves the dead boot's log first.
- 2026-10-01: the soak test replaced the stress test as the PC's tier:
  `soak 2` after a fix round, All tests and `soak 10` to sign off a
  milestone; the stress test stays for kernel work. The audio track
  started before M8's sign-off had finished. HDMI/DisplayPort audio is not
  planned; USB audio maybe (A3). A boot splash with the logo animation and
  its sound (AS) comes right after the audio milestones.
- 2026-09-30: running programs from `/data` waits until after M8 (a
  policy choice: anything written to the stick could then run). Other
  sticks are read-only unless `mount -w`, and never formatted; GPT sticks
  are left out for now.
- 2026-09-30: storage stays as planned: the USB stick and FAT32 only. An
  NVMe driver (easier, and the internal Crucial drive already has a FAT
  partition from an earlier attempt) and other filesystems (exFAT,
  ext2, littlefs, an own copy-on-write one) were discussed and declined.
  FAT32 comes from a FatFs port, not a hand-written driver.
  The repository went public under BSD-2-Clause; no GPL code may be copied.
- 2026-09-30: the kernel build of drivers is removed (nothing used it once
  devmgr ran every driver as a process).
- 2026-09-29, after M7: the audio track comes right after M8. SSH was
  discussed and not added. The boot menu became Jam OS / safe mode / a
  Tests folder; everything else is a shell command.
- 2026-09-29, after M6: drivers and services are processes from the start
  (writing drivers in the kernel first turned out to buy nothing; M6
  showed it). This replaced "bring up in the kernel, move
  out in the same milestone".
- 2026-09-29, after the M6 device listing: networking uses the board's own
  RTL8125 natively, not a USB Ethernet adapter (the earlier lean, when it
  wasn't known the board had a wired NIC); every frame on VLAN 21 only.
- 2026-09-29: the roadmap gained M4.5 (hardening before processes), M5.5
  (performance), M8.5 (crash kernel and kexec), M13 (self-hosting) and the
  graphics and audio tracks. Reboot became a shell command plus
  Ctrl+Alt+Del in M7 (once there is a shell, tests are commands and need no
  reboot); driver restart moved to M7 (M11 keeps only the IOMMU); the boot
  partition is read-only to Jam OS, with a separate data partition; bulk
  data moves through shared VMOs, not large channel messages.
- 2026-09-29: single user, no accounts; handles are the only authority.
- 2026-09-28: the base choices (C, Make, Limine behind `struct boot_info`,
  SMP from day one, capability handles, no `fork`, FAT32 only).
