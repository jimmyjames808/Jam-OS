# IR: the review of file watching (IP to IC)

Track IR of [INOTIFY-PLAN.md](INOTIFY-PLAN.md) ("5. Tracks"): everything
IP, IE1, IE2, IL1, IL2 and IC built, read in full at main 243156b4 by an
agent that wrote none of it, ahead of CB6 (Chromium's profiles watch their
files).

- **The protocol and the library** (IP): `fs.watch` (abi/idl/fs.idl method
  17), abi/idl/fswatch.idl and its bell, user/lib/fswatch.c (the queues,
  the merge and overflow rules, ids, the caps, the table a successor
  adopts, the order queue).
- **extfs** (IE1, IE2): user/services/extfs/watch.c (the channels, the
  events held to the commit, the hidden-name filter, the reach check),
  the close rule (main.c's `order_closes`, files.c), the events' places in
  fsops.c and files.c, the state and the keeper (state.c, adopt.c).
- **libc** (IL1, IL2): user/posix/sys/inotify.c (the descriptor, the
  takes, the wd table, the limits, fork and exec), `/proc/sys/fs/inotify`.
- **base** (IC): `FilePathWatcher`'s use of the descriptor (one descriptor
  per process, a reader thread on `poll`, `FIONREAD` and `read`, watches
  removed with `inotify_rm_watch`, `IN_IGNORED` and `IN_Q_OVERFLOW`
  never acted on).

The questions of the plan's IR row, answered first (each holds unless a
finding below says otherwise):

- **Can an event be lost or reordered against the close rule?** No
  event of a request is lost while extfs lives: the request's events are
  held until its commit and queued then, a close's and an orphan's going
  at once. A close made before a request is handled before it: a file's
  request runs the rule when its channel has a message, a watcher's always,
  an fs or view request reaps every closed file first, a held (unread-full)
  file's close is looked for by its channel's signals, and a close folded
  into an earlier packet is judged by the channel, not the packet. Two
  closes before one fs request come in slot order, not close order (6).
  Events are lost only as the plan says (the queue's bound, a restart:
  `IN_Q_OVERFLOW` says so), and a crash between a rename's commit and its
  events also skips that rename's reach check (1).
- **Is a wake ever lost?** No: the service rings when a queue goes from
  empty to not empty (the merge drops only an event equal to the queued
  tail, the overflow is queued like any record), libc clears the bell
  before its takes and rings it itself for a channel it couldn't empty,
  and libc's own `INO_HELD` bit covers records another thread's take left
  in the buffer. A channel the service ends rings first; one a dead
  service's keeper drops does not (7).
- **Can a watcher pin memory or handles without bound?** Not in the
  service: a channel's queue is 16 KiB of records (some 3 times that in
  heap, 9), the volume's 32 channels, the replies by the unread bound, a
  channel without its bell by the caps. In libc, wds whose `IN_IGNORED` an
  overflow dropped stay held for good and count against the 1,024 (2);
  a program's own quiet-watch removals grow its buffer (11).
- **Can a `wd` be reused while Chromium holds it?** No: libc's wds only
  grow (to `INT_MAX`), and a service id carries a 20-bit count, so an id
  libc still holds comes back only after a million watches in its slot
  (then libc ends the old wd first).
- **Can a process watch what it can't reach?** An add walks as the view's
  requests do (hidden names, a subview's root, the read bit); an event
  names only what the channel's view lists; a rename that moves a watched
  inode out of a narrower view ends the watch. Except: a watch the reach
  check can't place (deeper than 2,048 directories, which a subview's
  client can build itself, or a directory it can't read) is left on (3),
  and the crash of (1). Known and left by IE2: a hard link keeps a file's
  watch where the link is (its own events, no names), and a subview whose
  root was renamed judges adds by its old place (views.c's, DE0's).
- **Do watches survive an extfs restart and end correctly?** Yes for the
  watches, the queues (`IN_Q_OVERFLOW` first, a channel that never had its
  bell none), the bells and an `fs.watch` in progress; `IN_IGNORED` after
  `IN_MOVE_SELF` and `IN_DELETE_SELF`, never with `IN_ISDIR`. Not for a
  rename committed and not yet settled (1), nor a directory removed in the
  same window (12).
- **Can a hostile client make extfs block or crash?** Not crash: every
  message is checked by the generated code or `fsw_chan_bell` (a call
  first, a give with a txid, a give of a non-event or without
  `RIGHT_SIGNAL`, a second give, handles on a request: refused, the
  channel ended), ids are checked against their channel, `take`'s room is
  clipped. Block: yes, the reach check reads each watched directory's
  `..` chain to the volume's root for every watch of every subview channel
  on every rename anywhere on the volume (4). And one process can take
  every watch a volume has (5).

## Findings

| # | Sev | Where | What is wrong | How to make it happen | Outcome |
|---|---|---|---|---|---|
| 1 | Medium | `user/services/extfs/request.c:407` and `:418` (`commit`: the journal's commit, then `ev_settle`), `user/services/extfs/adopt.c:326` (`journal_has`: answered from the slot) | A rename's reach check (watch.c's `moved`, held to the commit) runs after the journal's commit. extfs dying between the two leaves a request the successor finds in the journal and answers from its slot: the check never runs. A watch the rename took out of its view's reach stays on, and a file's watch keeps its old directory for later checks. `IN_Q_OVERFLOW` says events were lost; it doesn't say a watch is now past its view. | Test power `die=journal:<n>` on a rename moving a subview's watched directory above its root; after the restart a `mkdir` in that directory reaches the subview's watcher, with its name. | Fixed: see "Fixes" |
| 2 | Medium | `user/posix/sys/inotify.c:741` (`lx_inotify_rm_watch`), `:292` (`convert_locked`), `:205` (`watch_room_locked`) | A removed wd is held until its `IN_IGNORED` comes. The service drops `IN_IGNORED` like any record once a queue is full (fswatch.idl, Linux's rule), and answers OK, with no `IN_IGNORED`, a removal of a watch that already ended (its own `IN_IGNORED` lost to an overflow). Either way the wd is held for good and counts against the descriptor's 1,024 (`ENOSPC`). Chromium never reads `IN_Q_OVERFLOW` and removes watches as its paths change: a busy watcher that overflows now and then loses its room over a long session. Linux frees the wd at once. | Fill a channel's queue, `inotify_rm_watch` a wd, read everything: no `IN_IGNORED` for it, and it is held (posixtest's new `lost_ignored`). | Fixed: see "Fixes" |
| 3 | Medium | `user/services/extfs/watch.c:298` (`moved`: `if (!n ...) continue`), `:185` (`CHAIN_MAX`) | Fail-open: a watch whose place the reach check can't find (its directory deeper than 2,048, a directory that can't be read, a file's directory gone) is left on. A subview's client can make its own watch too deep: watch a directory, then move the directory's top under fresh 32-deep chains again and again (each move short of path, inside the view, so judged by nothing). When something later moves that tree out of the subview the watch stays, and an open file in it reports to the client (with Directory handles, DIRHANDLE-PLAN, so will names made there). | utest `extfs_watch_deep`: a watch 2,100 directories down, its tree moved above the subview's root, then a write to a file open in it: the subview's watcher gets `IN_MODIFY` with the name. | Fixed: see "Fixes" |
| 4 | Medium | `user/services/extfs/watch.c:268` to `:289` (`moved`'s choice of channels), `:296` (a chain per watch) | Every rename on the volume judges every subview channel with watches unless the rename's target is under the channel's root: a rename in `/data` judges the watchers of `/tmp`. For each, each of its watches reads its whole `..` chain to the volume's root (an inode and a directory block each step). A subview client's 1,024 watches 2,000 directories down make every rename anyone makes on the volume read some 2 million directories while extfs serves nothing else; ordinary deep watches still cost each unrelated rename thousands of reads. | utest `extfs_watch_deep`: 256 watches 1,000 directories down in a subview; renames elsewhere on the volume take some 250,000 directory reads each (seconds), against well under a millisecond with no watch. | Fixed: see "Fixes" |
| 5 | Medium (design) | `user/include/fswatch.h:54` to `:57`, `user/lib/fswatch.c:251` (`fsw_chan_new`), `:381` (`fsw_add`) | The volume's caps are shared by every process: one process with four inotify descriptors holds all 4,096 watches of `/data` (1,024 a channel), four processes all 32 channels. Every other program's add then fails `ENOSPC` (Chromium's watchers stop). Linux's caps are a user's, and its default watch limit is far above what one program uses. | Four descriptors in one process, each with 1,024 watches on `/data` (or `/tmp`): a second process's `inotify_add_watch` is `ENOSPC`. | Left: design question 1 |
| 6 | Low | `user/services/extfs/files.c:534` (`files_reap`) | Before an fs request, the files closed since are handled in slot order, not in the order they were closed: two closes and then an fs request (behind another client's fs packet) give the two `IN_CLOSE_*` in slot order. Nothing known depends on the order of two files' closes. | Close the file in the higher slot, then the lower, then a `mkdir`, while another process's fs request is queued. | Left |
| 7 | Low (design) | `user/lib/keep.c:435` (`keeper_release`) | IL1's open item (the main session's "a mount going should wake poll/epoll"): IE2 made every channel extfs itself ends ring its bell (a fresh start, a restart that can't keep it, the end, the disk gone). A dead extfs that volumes gives up on has its channels closed by the keeper, which knows no bell: `poll` and `epoll` sleep on until something else wakes them; a blocked `read` sees it within a second. | Kill extfs past its restart limit while a thread polls an inotify descriptor watching `/data`. | Left: design question 2 |
| 8 | Low | `user/services/extfs/request.c:363` (`unmake`) | A request whose commit fails has what it made dropped (a file, a view, a map), but not a watcher channel: `fs.watch` answered `ERR_IO` leaves the channel's record and keeper slot until the client's end, closed with the failed reply, is seen closed at the channel's next packet (at once). Self-healing; one slot for a moment. | A journal commit failing at an `fs.watch` (a disk error). | Left |
| 9 | Low | `user/lib/fswatch.c:129` (`queue`), docs/INOTIFY-PLAN.md 2.6 ("Memory") | A queue's bound is 16 KiB of records, but each record is its own allocation (a 24-byte header and the allocator's own), so a full queue of short names is some 50 KiB of the service's heap; 32 channels some 1.6 MiB. Bounded; the plan's "costs the service its 16 KiB" understates it. | 1,170 events of no name on one channel. | Left (the plan's text says so now) |
| 10 | Low | `user/services/extfs/watch.c:88` (`SPECIAL_MAX`) | The hidden-name filter knows at most 16 etc-like directories (`etc`, `ETC`, `etc.`, ... in the root, and their `ssh` and `authorized_keys`). A volume whose root holds more names the views call etc may leave the real etc unknown, and its `ssh` named in events to a view hiding the keys. Only a writer of the volume's root (a whole view) can make them. | 16 directories `etc.`, `ETC.`, ... made before `etc` in the root's order. | Left |
| 11 | Low | `user/posix/sys/inotify.c:736` (a quiet watch's removal), `:149` (`put_locked`) | Records libc makes itself (a quiet watch's `IN_IGNORED`, a closed channel's `IN_UNMOUNT` and `IN_IGNORED`) have no bound: a program that adds and removes quiet watches in a loop without reading grows its own buffer (Linux: the queue's bound, then the overflow). Only the program's own memory. | `inotify_add_watch("/proc")` and `inotify_rm_watch` a million times. | Left |
| 12 | Low | `user/services/extfs/request.c:407` (as 1), `user/services/extfs/watch.c:606` (`watch_adopt_table`) | A directory removed (`UNLINK_DIR`) in the same window as 1 loses its watches' `IN_DELETE_SELF` and `IN_IGNORED`, and the watches stay on a freed inode (its generation changes when reused, so they never fire) until their client removes them. Files are fine: their going is the orphan list's, which the successor finishes. | `die=journal:<n>` on an `rmdir` of a watched directory. | Left (the overflow says events were lost) |
| 13 | Low | docs/INOTIFY-PLAN.md 3.5, `user/posix/sys/kinds.h` (`INOTIFY_MAX_INSTANCES`) | `max_user_instances` 8 is reachable only for descriptors that each watch one mount of a volume: `/data` and `/tmp` are two mounts of one extfs, so a descriptor watching both takes two of the process's 8 channels there, and the fifth such descriptor's second add is `ENOSPC`. Chromium holds one. | Eight descriptors each adding a watch on `/data` and one on `/tmp`. | Left (the plan says so now) |

Notes (no change needed):

- **N1.** Cookies come from one counter per service instance (a random
  start), so a watcher can count the renames made elsewhere on the volume
  between two of its own events. Linux's cookie is one global counter, the
  same.
- **N2.** `IN_CLOSE_*` comes when a file's channel closes; a file still
  mapped after its last descriptor closed has already had its close
  (Linux's comes when the mapping's reference goes). Chromium's watchers
  don't use the mapping case.
- **N3.** A watcher channel that never gives its bell holds its slot and
  its process's share until its client closes it; the caps bound it, and
  `fs.watch` refused for the share first reaps channels whose client is
  gone.
- **N4.** `fsw_take` copies records out and frees them before the reply
  is committed; a restart between the two answers the take from its slot
  (committed) or runs it again (the queue then holds the overflow): no
  record is given twice.

## Fixes

## Design questions

## Tests
