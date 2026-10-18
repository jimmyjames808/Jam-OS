# WR: the review of writable mappings (WK1 to WT1)

Track WR of [WRITEMAP-PLAN.md](WRITEMAP-PLAN.md) ("5. Tracks"): everything
WK1 to WT1 built, read in full context at main a36453d7 by an agent that
wrote none of it, ahead of CB5 (Chromium's processes share memory all the
time).

- **The kernel** (WK1 to WK3): the page states and the three lists
  (`kernel/object/pager_cache.c`), write-back requests, their bits, the
  background request, the throttle and AGAIN (`kernel/object/pager.c`), the
  pager-VMO paths of `kernel/object/vmo.c` (the store, BEGIN's protect and
  copy, WRITTEN, `vmo_sync`, the hold, shrink and detach), the store's wait
  in `kernel/mm/aspace.c`, `sysc_vmo_sync` and `sysc_pager_dirty`.
- **extfs** (WE1, WE2): `file.map`'s rights and Q7 (`files.c`, `fsops.c`),
  one VMO per file and one cache (`pager.c`), write-back, its batches, its
  errors and the deaths (`wback.c`, `adopt.c`'s re-run and fresh start).
- **libc** (WL1): `map_file`'s choice of `FILE_MAP_WRITE`, `msync`
  (`user/posix/sys/mem.c`).
- **The tests** (WT1): `pagertest rec`, `pager-test.sh`'s `wmsync`, `wcut`,
  `wpress`, utest `extfs_wpress`, posixtest `spawn-wmap`.

The plan's questions, answered first (each holds unless a finding below
says otherwise):

- **Can a store be lost?** BEGIN makes its pages writing and
  write-protects them in every mapping under the VMO lock (every mapping
  that may ever write is on the reverse map before it has an entry; a
  clean or writing page is never installed writable, so no other path
  makes a writable entry), shoots down with no lock held, and only then
  copies: a store through a stale entry lands before the copy, one after
  the shootdown faults and makes the page dirty again. A request's two
  bits a page (`need`, `begin`) are set and cleared under the VMO lock
  with the page states, and one write-back of a page at a time makes
  "the next end of that page's write-back" the right one after a death or
  a failed copy. AGAIN runs only once the instance before is dead (volumes
  restarts after `SIG_TERMINATED`, and a kill returns once the job is
  dead), so no stale WRITTEN can end a successor's write-back. Lost all
  the same: an answered `write` through a VMO a fresh instance doesn't
  know (2), and a new VMO's stores behind a stale GONE (4).
- **Can a writer or the kernel wait forever?** No: every wait is a
  request's, with its deadline; a store's waits have the answer time in
  all; `vmo_sync` one answer time a chunk and stops at the first failure;
  every wait is cancellable and none holds a mutex. A healthy but busy
  pager can get a writer killed, though (6).
- **Can a dirty page be evicted?** No. Evictors take only `pages` (clean);
  one dirtied after an evictor popped it is skipped (`take_batch_locked`'s
  `clean_only`); invalidation drops clean pages only; only a shrink and a
  detach drop dirty ones, by design.
- **Does a view's refusal hold for every way to a writable handle?** Yes.
  The one way is `file.map(FILE_MAP_WRITE)` on an open with `FS_READ` and
  `FS_WRITE` and not `nochange` (the view's judgement at the open, which
  refuses a writing open where the view forbids it), on a writable volume;
  `FILE_MAP_READ` hands out `PAGER_VMO_RIGHTS` (no `RIGHT_WRITE`); a
  re-sent answer (`maps_remake`) gets the rights of the make the state
  recorded for that very request (`kept->made` and `made_write` are set in
  the same make); a fork shares a mapping only with the write its maximum
  had; a child VMO (`MAP_PRIVATE`) is private; `vmo_write`, pins and
  kernel mappings of pager VMOs stay refused.
- **Is an acknowledged `msync` safe across every death?** Yes for the
  deaths of the writer, extfs (commit before WRITTEN; AGAIN; requests live
  in the pager object) and volumes (extfs holds the pager), and for a
  power cut after the answer. Not across another mapper taking the error
  (7).

Severity as FR1's: **High** is a loss of data, a crash, a hole, or a hang
a program can cause for others; **Medium** is wrong behaviour the system
or a test tier will meet, or a cost a program can push onto every other
one; **Low** is a wart, or a mismatch nothing relies on yet.

Counts: **0 High, 3 Medium, 6 Low, 3 notes.**

## Findings

| # | Sev | Where | What is wrong | How to make it happen | Outcome |
|---|---|---|---|---|---|
| 1 | Medium | `kernel/object/pager_cache.c:247` (`pager_cache_written_vlocked`), `kernel/object/vmo.c:2343` (`run_page_locked`'s PEEK), `user/services/extfs/pager.c:705` (`maps_peek`) | WE2's one-cache gap. A page whose write-back failed (Q1's A) becomes clean, so `PAGER_DIRTY_PEEK` no longer copies it and extfs's `read` gives the disk's bytes, while every mapping still shows the store until the page is evicted: two answers for one file (Linux's page cache gives the store to both). | A store over a hole of a full volume; `msync` says `ERR_NO_SPACE`; `read` of the page gives zeros, the mapping the store. | Fixed: see "Fixes" |
| 2 | Medium | `user/services/extfs/pager.c:637`, `:648`, `:658` (`maps_wrote`, `maps_resized`, `maps_changed`) and `:220` (`maps_written`) | A shared VMO that lives with no slot (made by an instance before one that started fresh, or without its state) is invisible to `write`, `truncate` and the run check: they look only at slots, and such a VMO gets one only when a map finds it or a page or write-back request of its names its key. So a `write` doesn't reach its pages: a present clean page keeps the old bytes (the mapping never sees the write), and a dirty one's next write-back puts the old bytes back over the answered write (a lost write); a truncate doesn't zero the tail or shrink it, so a store past the new end becomes file data again after a regrow; and `FILE_MAP_EXEC` is not refused while it is writable (Q7). WE1's "a slotless VMO counts as written" holds only for a map that finds it. | Map a file writable, store, kill extfs, start an instance with a new state; then `write` over the stored page (before the age write-back), `msync`: the disk holds the store and the old bytes, not the write. | Fixed: see "Fixes" |
| 3 | Medium | `kernel/object/pager.c:903` (`pager_create_vmo_flags`), `kernel/object/vmo.c:1000` (`vmo_destroy`) | A VMO whose last reference went is destroyed with its link still LINKED until `pager_vmo_gone`, after every page is freed (`drop_from_locked`: milliseconds for a big file). Meanwhile `PAGER_VMO_FIND` says `ERR_NOT_FOUND` (the reference can't be taken) and a create of the key says `ERR_ALREADY_EXISTS`: extfs's `file.map` fails. Before WE1 every unopened map took a new key; with one key per file, the last unmap of a file by one process racing a map of it by another fails the map. CB5's processes map the same files (paks, fonts, ICU) as they start and end. | Process A unmaps the file's only mapping (its VMO's last reference) while process B maps the file: B's `mmap` fails. (A ktest with a hook in `vmo_destroy` makes it every time.) | Fixed: see "Fixes" |
| 4 | Low | `user/services/extfs/pager.c:909` (`maps_again`) and `:872` (`serve`) | The request `PAGER_NEXT_AGAIN` gives back is kept (`again_first`) and served at the next `serve`. When the first `maps_again` comes from a sync request run again by adopt.c before `pager_start`, and that request is a GONE, a map run again in between (the other request in progress) makes the key's VMO again, which takes the GONE over in the kernel; the kept GONE is then served: the new slot is forgotten and the new VMO detached (its mapper fails, its stores are lost). | Two requests in progress at a death: a `file.sync` and a map of a file whose VMO's GONE was queued. Not practical to make in a test. | Fixed: see "Fixes" (no test: see there) |
| 5 | Low | `user/services/extfs/pager.c:364` (`make_shared`) | A map of a file whose slot extfs holds no handle for (its opens closed since) rewrites the slot whole, so the write-back error kept for the file's next `file.sync` (`error`) is dropped: `fsync` after a re-map says 0 although a write-back failed. | `msync` fails `ERR_NO_SPACE`; close; map the file again; `fsync` returns 0. | Fixed: see "Fixes" |
| 6 | Low (design) | `kernel/object/pager.c:610` (`pager_throttle`), `kernel/mm/aspace.c:2146` (`store_wait`) | A store at the dirty limit has the answer time (60 s) in all from its first wait, whether or not the pager is answering meanwhile. A pager that is busy but working (a `file.sync` or `fs.sync` of the dirty limit's worth on a slow stick: extfs serves no request while it runs) or contended (other writers take the room each write-back frees) gets a healthy writer killed "the file's pager didn't answer". The plan's 1.4 says so ("at most the answer time in all"). | 512 MiB dirty on a 10 MB/s stick; `sync` in one process while another stores at the limit. | Design question 1 |
| 7 | Low (design) | `kernel/object/vmo.c:2449` (`vmo_pager_take_error`), `user/services/extfs/wback.c:386` | A write-back error is kept once per VMO (and once per file for `file.sync`) and reported to whichever caller comes first: another mapper's `msync` takes it, and this one's `msync` then says 0 for stores the disk never got. Linux reports it to every open file description (errseq). Q1's answer says "once". | Two processes map a file; an age write-back fails; process B's `msync` returns `EIO`, then process A's returns 0. | Design question 2 |
| 8 | Low (design) | `user/services/extfs/wback.c:67` (`WB_STOP_NS` 3 s), `user/services/volumes/disk.h:23` (`SYNC_WAIT` 5 s) | An orderly stop or reboot writes mapped stores back for about 8 s at most (volumes' `fs.sync` wait, then the stop's 3 s); the dirty limit (512 MiB on the PC) is far more than a stick writes in 8 s, so stores that weren't `msync`ed are lost at an orderly reboot or unmount (Linux's writes them all). Logged ("stores left unwritten"). | Store 400 MiB through a mapping on a stick, `reboot` at once. | Design question 3 |
| 9 | Low | `user/services/extfs/wback.c:234` (`one_run`) | A batch given up (a disk error) goes on beginning runs (up to 32) that all fail with it, rather than ending the batch so the next run gets a transaction of its own. Not fixed: the errors that give a batch up (a failed data write, a failed commit, a volume that can't be written) turn the volume read-only, after which every run fails anyway. | A disk error in a batch's first run. | Left |
| 10 | Low | `kernel/abi/sysc_pager.c:60` (`sysc_pager_dirty`) | After a BEGIN, the run is copied back to the pager; if that fails (another thread of the pager unmapped or protected the struct between the first write-back and the last), the pages stay writing until the pager's next AGAIN: its own `msync` callers time out and its dirty limit is pinned. Only a pager can do it, to its own files. | A two-threaded pager unmapping its run struct mid-call. | Left (a pager's own harm) |
| 11 | Low (accepted) | `user/services/extfs/pager.c:705` (`maps_peek`) | WE2's "Left": after a fresh start, `read` doesn't lay a slotless VMO's dirty pages over the disk's until something gives it a slot (at most the age window, or, with finding 2's fix, the first `write`, `truncate` or run check). Closing it costs a `PAGER_VMO_FIND` on every read of a file with no slot. | As finding 2, then `read`. | Left (accepted by the main session) |
| 12 | Medium (not High) | `kernel/object/vmo.c:490` (`cow_wait`) | FR1's design question 1, handed on: a writer waits as long as freezes of its VMO overlap. Not a writable-mapping path: a pager VMO is never frozen (`vmo_clone_begin` refuses it, a child of one takes a window, not a share). For anonymous memory and memfds it is a stall a peer holding a writable mapping can cause (a kill or an exec ends it since FR1's fix). Not High: the peer can scribble on the buffer anyway, and nothing hangs for good; High once CB5's renderers are treated as hostile. | FR1's: two processes mapping one VMO as copies fork in loops. | Left (design question 4) |

Notes (no change needed):

- **N1.** `kick` runs from `vmo_fault_map` with the region lock held and
  may drop the last reference of another VMO (`queue_oldest`), so
  `vmo_destroy` can run under a region lock, against its comment ("no
  lock held"). `vmo_destroy` takes only spinlocks (the pager's, the
  cache's) and the job's counters, never an address-space lock: safe.
- **N2.** `vmo_pager_sync` without the wait queues a request per chunk
  per call, charged to the caller's job: a loop of `MS_ASYNC` costs the
  pager a turn each, bounded by the caller's message-byte limit, no more
  than `write` in a loop.
- **N3.** A confused pager's buffer that is itself a mapping of one of its
  writable VMOs makes its BEGIN's copy fault and wait on itself at the
  dirty limit (the answer time, then `ERR_INVALID_ARGS`, the run dirty
  again). extfs's buffers are its own memory; a pager is trusted.

## Fixes

One commit each, its test with it (the contract text of finding 1 in its
own commit first, bb351a91: the ABI stamp moves).

- **Finding 1** (eb21ea09): a page whose write-back ends with an error is
  clean (Q1's A: it doesn't pin the dirty limit, it may be evicted) but
  *kept* (`REC_KEPT` in its cache record, `pager_cache.h`'s "Kept"):
  `PAGER_DIRTY_PEEK` copies it with the dirty and writing ones until a
  store makes it dirty again or it leaves its table (evicted, invalidated,
  shrunk, detached), so extfs's `read`, which already lays PEEK's pages
  over the disk's, shows what every mapping shows. Nothing else sees the
  mark: its state stays 0, flags 0 and BEGIN never take it, the counts and
  the hold are a clean page's, and the background request's run (any
  dirty or writing page) has a look of its own (`RUN_COUNTED`). ktest
  `pager_dirty_failed_kept`; utest `extfs_wback`'s full volume now reads
  the file as the mapping shows it after the failed `msync` (the disk's
  bytes are checked by an instance with no pager).
- **Finding 2** (6174bf4c): `write`, `truncate` (and a failed one's
  `maps_changed`) and the run check (`maps_written`) of a file with no
  shared slot ask the kernel for its key's VMO first (`find_lost`: one
  `PAGER_VMO_FIND`, answered `ERR_NOT_FOUND` at once for a file nobody
  maps); a VMO an instance before made gets a slot there, `written`, its
  size as the kernel has it, so the usual one-cache steps reach its pages.
  No free slot: it is cut off (detached) rather than left to write older
  bytes back. utest `extfs_wback`'s fifth phase: three files mapped and
  stored to, extfs killed, an instance with a new state: a map to run is
  `ERR_BUSY`, a write over a stored page shows in the mapping and both
  reach the disk, a truncate down and up again zeroes the stores past the
  cut, there and on the disk. `read` is left as it was (finding 11).
- **Finding 3** (e6f3ce2f): `pager_create_vmo_flags` takes over a LINKED
  link whose VMO has no reference left (its destruction under way) as it
  takes over a GONE one: out of the key table, DETACHED (its requests
  failed; none has a waiter), freed by that destruction's
  `pager_vmo_gone`, which sends no GONE for it. A live VMO is still
  `ERR_ALREADY_EXISTS`. New hook `DBG_PAGER_VMO_DYING` (in `vmo_destroy`,
  after the pages are freed, before the link is told); ktest
  `pager_dirty_dying_key` makes the key again from the hook: OK, the new
  VMO found by FIND, no GONE for the old one, the new one's GONE in turn.
- **Finding 4** (6c9af029): a map that makes its key again (taking the
  GONE over in the kernel) drops a GONE of that key that `maps_again` kept
  for `serve`. No test: it needs two requests in progress at one death (a
  sync and the map) with the key's GONE queued; extfs's test powers stop
  one request at a time.
- **Finding 5** (2f85b7cb): `make_shared` keeps the slot's `error` when it
  rewrites an existing slot. utest `extfs_wback`'s full volume maps the
  file again (its VMO alive, extfs's handle let go) between the failed
  `msync` and the `fsync`, which still says `ERR_NO_SPACE`.

## Design questions

1. **A store's deadline (finding 6).** Keep 1.4's "the answer time in
   all", or count it from the pager's last progress: the store's time
   starts again whenever a write-back of the pager ends (a WRITTEN, of any
   VMO), so a writer dies only when the pager has written nothing for the
   answer time, as a read's request dies only when its own page doesn't
   come. *Recommendation:* from the last progress (Linux's writers wait as
   long as write-back goes on); a counter in the pager bumped by WRITTEN,
   read by the throttle, small.
2. **Errors to every mapper (finding 7).** Keep "once per VMO", or
   Linux's errseq: the VMO keeps an error and a count of errors; each
   mapping (libc's table entry) samples the count when it maps, and
   `vmo_sync` returns the error when the count passed its sample. An ABI
   change (`vmo_sync` takes and returns the sample). *Recommendation:*
   errseq before programs that check `msync` share files across
   processes (databases); not needed for CB5's histograms.
3. **An orderly stop's write-back (finding 8).** Bound the stop by
   progress, not by 3 s (go on while each second writes something, up to
   the answer time per run), and make volumes' reboot wait for the mounts'
   `fs.sync` as long as they make progress. *Recommendation:* yes, with
   the dirty limit scaled down on a slow disk later (M14).
4. **Freezes and a writer (finding 12).** FR1's choices stand; *my
   call:* not High now, (b) of FR1's list (a freeze that would begin while
   a writer of that VMO waits gives up and starts again) as a small kernel
   track before renderers are sandboxed.

## Tests

**The fixes' tests** (each in its fix's commit): ktests
`pager_dirty_failed_kept` and `pager_dirty_dying_key`
(`kernel/test/test_pager_dirty.c`); utest `extfs_wback` (its full volume
and its new fifth phase; it runs in the init run, where utest has the
pager power).

**The hostile test** (the plan's WR row): `pagertest rec swarm` and
`tools/pager-test.sh`'s `whostile` case (`PAGER_CASES=whostile`):
`/jam/rh.bin`, 512 records of 5000 bytes, mapped writable; the process
forks; four threads in each process store the records 100 times over
(writer w the records w, w + 8, ...; round r with salt 41 + r; records
cross pages, so the writers share pages), the child through the mapping it
inherited, each writer msyncing its records every 4 of them, while a
thread of the parent kills `extfs-jam` every 200 ms (initctl.kill, then
one call on the mount, as `storm` does). Every `msync` must say OK and
every record end whole by the mapping, by `read` and by a new mapping; then
QEMU's monitor quit with no stop and no sync, and Linux's `e2fsck -fy` of
a copy of the disk exits 0 and debugfs finds all 512 records whole with the
last round's salt (every one was acknowledged).
