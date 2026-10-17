# WRITEMAP plan: writable shared file mappings on ext

The owner (2026-10-16): build writable shared mappings (`MAP_SHARED` with
`PROT_WRITE` of a file) now, **on ext only**; FAT keeps read-only mappings
([M12.5's Q1](M12.5-PLAN.md#q1-writable-mappings-now-or-later), answer
(A), stands for FAT). Chromium needs them: `base::MemoryMappedFile` with
`READ_WRITE` and `READ_WRITE_EXTEND` backs its persistent histograms and
metrics files (base's file-backed `PersistentMemoryAllocator`) and some of
its databases, and CB2's first `base_unittests` run met the `ENODEV` libc
answers today ([M13.6-PLAN's U10 notes](M13.6-PLAN.md#u10-notes)).
`msync` is the durability promise, as on Linux. This plan supersedes, for
ext, M13.6's "No writable shared mappings" and M12.5's "every page of a
pager VMO is clean". Nothing here is built.

**In one paragraph.** A pager VMO made writable (a new flag) may be mapped
with write. Its pages are clean, dirty or being written: a clean page is
mapped read-only everywhere, so the first store to it faults and the
kernel marks it dirty; extfs asks the kernel for dirty pages (the kernel
write-protects them in every mapping, then copies them out), writes them
in a journal transaction of its own, and tells the kernel they are
written, which makes them clean unless a store dirtied them again
meanwhile. The kernel asks for write-back through a new request kind when
a program calls `msync` or when a mount's dirty pages pass a limit; extfs
writes back on its own every few seconds and for `fsync`. Dirty pages are
never evicted: a quarter of the cache at most may be dirty, so eviction
always finds a clean page, and a writer that meets the limit waits for
write-back with a deadline, never forever. A dirty page holds its VMO, so
stores outlive `munmap`, the program and an extfs restart. extfs keeps one
VMO per file and writes `file.write`'s bytes into its pages too, so a
mapping, `read` and `write` see one file.

## What this plan is not

- **Not on FAT.** fat's mappings stay read-only; `MAP_SHARED` with
  `PROT_WRITE` on FAT stays `ENODEV`, and the kernel changes nothing for a
  pager VMO not made writable (fat's, extfs's programs).
- **No page-cache writes for `write`.** `file.write` stays write-through
  (M13.6's Q6 (A): an answered write is on the medium); it also updates
  the pages a mapping holds (section 2.2).
- **No reservation at the first store** (Linux's `page_mkwrite`): a store
  over a hole gets its block at write-back (Q1).
- **No kernel timer, no kernel write-back thread**: the kernel asks only
  when something waits or the limit is near; age is extfs's to watch.
- **No `vmo_write` on a pager VMO**: a program writes a file through the
  file protocol or a mapping; `vmo_write` stays `ERR_NOT_SUPPORTED`.
- **No writable programs**: a file mapped to run is never mapped writable,
  and the reverse (M12.5's Q7, kept).
- **No `mremap`**, no change to `MAP_PRIVATE` (a copy-on-write child of
  the file's VMO, as F1b built) or to memfds.
- **No ext volume on a USB stick for real use**: usb-storage's writes
  aren't FUA (M13.6's U4 notes); write-back there is as safe as `write`
  there, which is test-only.

## Where it starts

| Piece | Built by | What this plan does with it |
|---|---|---|
| The pager object, keys, requests that outlive the pager's process, `pager_supply`, `pager_op` | PK1, PK2 ([M12.5-PLAN](M12.5-PLAN.md)) | a new request kind and op; writable VMOs |
| The page cache: budget, oldest-first eviction, the record per resident page | PK3 (`pager_cache.c`) | dirty pages leave the eviction list; the record keeps the page's state |
| Verified VMOs for programs | PK4 | untouched: never writable |
| Coherence by invalidate and set-size | PF2 (fat), U10 (extfs) | extfs's writes update pages in place instead |
| extfs as a pager: `file.map`, slots `maps[]`, `XF_MAP_KEY`, Q7's `ERR_BUSY`, orphans held by slots, restarts | U10 (`user/services/extfs/pager.c`) | `FILE_MAP_WRITE`, one VMO per file, write-back |
| A journal transaction per request, ordered data, the slot's transaction id, replay as recovery | U3, U4 | a write-back is a transaction of its own |
| Copy-on-write children of pager VMOs | F1a | unchanged: `MAP_PRIVATE` |
| Clone modes (`VMAR_CLONE_SHARE`) | FK1 ([FORK-PLAN](FORK-PLAN.md#fk1-notes)) | a writable mapping of a writable pager VMO is always share |
| Batch budgets for zapping and protecting (`VMO_BATCH_BUDGET`), `VMAR_MAX_OVERLAP` | CR1 ([CR-REVIEW-KERNEL](CR-REVIEW-KERNEL.md)) | write-protect for write-back within them |
| libc's `mmap` of files on their pages, `msync` | F1b (`user/posix/sys/mem.c`, `user/posix/sys/file.c`) | the writable shared case, `msync` for real |
| Fault signals | SG1, SG3 ([SIGNALS-PLAN](SIGNALS-PLAN.md)) | a failed store is SIGBUS once they land, through their table |

Today a pager VMO's handle has no `RIGHT_WRITE` (`PAGER_VMO_RIGHTS`),
`vmo_umap_add` refuses a writable mapping of one (`ERR_ACCESS_DENIED`),
libc's `map_file` answers `MAP_SHARED` with `PROT_WRITE` `ENODEV` (`EACCES`
on a descriptor that can't write), and `lx_msync` has nothing to write.

## 1. The kernel

### 1.1 Writable pager VMOs

- `pager_create_vmo` takes a flag, `PAGER_VMO_WRITABLE`: the VMO's
  handle gets `RIGHT_WRITE` beside `PAGER_VMO_RIGHTS`, and with it may be
  mapped writable (`vmo_umap_add` allows it for such a VMO only). A
  pager whose budget is under 8 pages makes none (`ERR_INVALID_ARGS`: the
  dirty limit below would be 0).
- `RIGHT_WRITE` on a pager VMO means "may be mapped writable" and nothing
  more: `vmo_write`, `vmo_set_size`, commit, decommit, pin and kernel
  mappings stay refused, as for every pager VMO. A holder hands a
  read-only mapper a handle without it (`handle_duplicate`).
- A writable pager VMO is never executable and never verified
  (`vmo_make_exec_verified` refuses it, `ERR_BAD_STATE`).
- **One VMO per file needs a way back to it.** `pager_create_vmo` takes a
  second flag, `PAGER_VMO_FIND`: no new VMO, a new handle to the live VMO
  under that key (its rights as made), `ERR_NOT_FOUND` if there is none
  (never made, detached, or dying: the reference is taken only if its
  count isn't 0). A create of a key whose VMO died but whose
  `PAGER_REQ_GONE` the pager hasn't answered takes the key over: the GONE
  is withdrawn, so a `file.map` never waits for the pager's queue.

### 1.2 Dirty tracking

A page of a writable pager VMO is in one of four states (two bits in its
cache record, under the VMO lock):

| State | What it means | Mapped | On which list |
|---|---|---|---|
| absent | not in the table | no | none: a fault asks the pager (unchanged) |
| clean | the disk's bytes, as far as the pager knows | read-only in every mapping, writable or not | the cache's list: may be evicted |
| dirty | stored to since its last write-back began | writable where the mapping allows | the pager's dirty list, oldest first: never evicted |
| writing | its write-back began and hasn't ended; a store since makes it dirty as well (both bits) | read-only, until a store makes it dirty | the dirty list: never evicted |

The moves:

| Event | From | To |
|---|---|---|
| `pager_supply` | absent | clean |
| a store (a fault or a kernel copy to user memory) | clean | dirty (if the limit allows, section 1.4) |
| a store | writing | writing and dirty |
| write-back begins (`pager_dirty` with `PAGER_DIRTY_BEGIN`) | dirty | writing (entries write-protected first) |
| `PAGER_OP_WRITTEN`, OK | writing | clean; writing and dirty: dirty |
| `PAGER_OP_WRITTEN`, an error | writing | Q1 (recommended: clean, the error kept for the next `msync`) |
| `PAGER_NEXT_AGAIN` (a successor starts) | writing | dirty |
| eviction | clean | absent (unchanged; never another state) |
| `PAGER_OP_INVALIDATE` | clean | absent; dirty and writing pages stay (they are newer than the disk) |
| `pager_write` (section 1.7) | any present | the same state, new bytes |
| `PAGER_OP_SET_SIZE` shrink | any, past the end | gone (Linux drops dirty pages past a truncate) |
| detach, the pager's last handle | any | gone: dirty data is lost (a pulled disk) |

- **Software dirty bits, by write-protection**, not the page tables'
  hardware D bits: a clean page is installed without `ASPACE_WRITE` even
  in a writable mapping, so its first store faults and `vmo_fault_map`
  (under the VMO lock, the one path that installs entries: aspace.c's
  header) marks it dirty and installs it writable. `leaves_protect`
  already never adds the write bit, so an `mprotect` adding write leaves
  clean pages read-only. Harvesting D bits would walk every mapping under
  the VMO lock as protecting does, and gives no point to make a writer
  wait (section 1.4).
- **The record** (PK3's, 16 bytes a resident page) gains the two state
  bits and the time the page was first dirtied (for the oldest-first
  write-back); WK1 decides whether it grows to 24 bytes or dirty pages get
  a second record. A page is on the cache's list or the dirty list through
  its `node`, never both.
- **Counts**: each pager counts its dirty-or-writing pages; each VMO too
  (for the hold, section 1.8). Both are part of the budget's `resident`:
  dirtying moves a page, it doesn't add one.

### 1.3 Write-back: the protocol

**Requests.** A new kind, `PAGER_REQ_WRITEBACK`: "write back the dirty
pages of `[offset, offset + len)` of `key`'s VMO" (at most
`PAGER_SUPPLY_MAX` bytes). Like a read request it stays in the pager
object until answered, is delivered by `pager_next`, put back by
`PAGER_NEXT_AGAIN`, has a deadline (the answer time) and is charged as
message bytes (with two bits a page, 64 bytes more: "WK2 notes"). The
kernel makes one:

1. for `vmo_sync` (libc's `msync`), one per 1 MiB chunk of the range that
   holds a dirty or writing page, one at a time per calling thread;
2. when the pager's dirty count passes **half the dirty limit**: one
   request for the oldest dirty run, at most one such queued per pager
   (charged to the pager's job);
3. for a writer waiting at the limit (section 1.4).

**`pager_dirty`** (new call): the first run of dirty pages (with `PEEK`,
dirty or writing) in `[offset, offset + len)` of `key`'s VMO (consecutive
pages, at most
`PAGER_SUPPLY_MAX` bytes, cut shorter to stay within one batch's budget),
its bytes copied into the pager's buffer at their place, and where it is
(offset and length; length 0: none). Flags:
- `PAGER_DIRTY_BEGIN`: the run's write-back begins. Under the VMO lock
  every page becomes writing and loses its write bit in every mapping
  (`wrprotect_umaps_locked`, sized by `batch_end_locked` to
  `VMO_BATCH_BUDGET` at `VMO_BATCH_PROTECT_COST` a page a mapping); then,
  with no lock held, one shootdown; only then are the pages copied out
  (from page references). A store after the shootdown faults and makes
  its page dirty again, so it can't be lost; a store racing the copy may
  show in it torn, and the page is written again (Linux without "stable
  pages": ext's checksums cover metadata, not data).
- `PAGER_DIRTY_PEEK`: dirty or writing pages copied, nothing changed:
  extfs's `read` overlays them on the disk's bytes (section 2.2).
- `PAGER_DIRTY_OLDEST`: key and offset ignored: the run around the
  pager's oldest dirty page, whatever its VMO; the answer names its key
  and the age of that page (extfs's age write-back, section 2.4).

**`PAGER_OP_WRITTEN`** (a new `pager_op`; `arg` the status): the writing
pages of the range end their write-back (clean, or dirty if stored to
since; an error as Q1 says). A `PAGER_REQ_WRITEBACK` is answered when each
page it needs is done (below), so a pager answers a request by beginning
its range (as many `pager_dirty` calls as runs), writing, committing, and
one `PAGER_OP_WRITTEN` over the range; it may write back more than asked.

**What a request needs** (the rule that makes `msync` exact): each page
dirty when it was made must have been written by a write-back that
**began after** it was made (a store before `msync` may have landed after
an earlier write-back's copy), and each page writing then and not stored
to since needs only that write-back to end. Pages dirtied after the
request was made don't hold it up. The kernel stamps each write-back's
start (a sequence number per pager) to tell these apart; a page dropped
by a shrink or a detach counts as done (its data is gone with the file
or the disk).

**Successors.** `PAGER_NEXT_AGAIN` turns every writing page of the
pager's VMOs dirty again and puts every delivered, unanswered
`PAGER_REQ_WRITEBACK` back in the queue: a write-back cut off by a death
is done again, whether or not its transaction reached the disk (writing
the same bytes twice is harmless).

### 1.4 The dirty limit, and waiting without a hang

- **The limit** is a quarter of the pager's budget (512 MiB of the PC's
  2 GiB per mount). Dirty and writing pages count. Below it the cache
  always has three quarters of its budget for clean pages, so a supply
  always finds a page to evict and readers of other files never wait for
  a writer: Mach's hazard (M12.5's "The page cache") stays avoided.
- **A store at the limit** (a fault on a clean page when the pager's
  count is at the limit) queues or joins a write-back request for the
  pager's oldest dirty run and waits on it as a read fault waits for a
  supply: without the region lock (aspace.c's `fault_wait`), cancellable
  by a kill, never with a mutex held (PK2's rule, the same counter). It
  then looks again from `find`; while the count stays at the limit it
  waits again, **at most the answer time in all** (60 s,
  `FS_CALL_TIMEOUT`), then the store fails `ERR_TIMED_OUT`: the kill line
  "page fault: the file's pager didn't answer" (SIGBUS, `BUS_ADRERR`,
  once SG3 lands: SIGNALS-PLAN's table already maps it). A user copy in
  a system call returns `ERR_IO`, as for a read.
- **A slow pager** slows only its writers. **A dead pager**: its
  requests wait in the object for the successor (PK's rule); stores at
  the limit wait for it. **A pager given up on** (volumes closes it):
  every VMO is detached, dirty pages are dropped, waiting writers and
  `vmo_sync` callers fail at once, mappers fail at their next touch as
  for a pulled stick.

| A store's wait ends | A fault in user mode | A user copy | `vmo_sync` |
|---|---|---|---|
| the count fell below the limit | the store is retried | the copy goes on | (not a wait of `vmo_sync`'s) |
| its request answered with an error (Q1) | retried (the error is the request's, not the store's) | goes on | `ERR_IO` |
| the answer time passed | killed: the pager didn't answer | `ERR_IO` | `ERR_TIMED_OUT` |
| detached, the pager closed | killed: the file's pager is gone | `ERR_IO` | `ERR_PEER_CLOSED` |
| the thread was killed | (dying) | `ERR_CANCELED` | `ERR_CANCELED` |

### 1.5 `msync`, `fsync`, `munmap`, `close`, exit

`vmo_sync(vmo, offset, len, flags)` (new, `RIGHT_READ`): for a writable
pager VMO, makes the write-back requests of section 1.3 for the range and,
with `VMO_SYNC_WAIT`, waits for each (the answer time each, at most);
`OK` at once for any other VMO or a range with nothing dirty. The VMO
keeps the first write-back error since the last `vmo_sync` with the wait,
and that call returns it (`ERR_IO`, `ERR_NO_SPACE`), once.

| Call | What happens | Promise |
|---|---|---|
| `msync(MS_SYNC)` | `vmo_sync` with wait over each mapping in the range | 0: every store made before the call is on the disk (committed with FUA). `EIO`: a write-back failed (Q1), the pager didn't answer in time, or is gone |
| `msync(MS_ASYNC)` | `vmo_sync` without wait: the write-back is asked for | 0 (Linux's is a no-op; asking costs a request) |
| `MS_INVALIDATE` | nothing more: one cache | |
| `fsync`, `fdatasync` | `file.sync`: extfs writes back every dirty page of the file's VMO, in transactions committed before it answers | 0, or the kept error |
| `sync`, `syncfs` | `fs.sync`: every VMO of the mount | |
| `munmap`, `close`, `exit`, a crash, `kill` | nothing: the kernel keeps the pages (the hold, 1.8); extfs writes them within the window (Q2) | stores survive the program |
| extfs dies | nothing is lost: the pages stay dirty in the kernel, the successor writes them | stores survive the service |
| a power cut or a kernel panic | dirty pages not yet written are lost (at most the window's worth); the journal keeps the volume whole; what `msync` or `fsync` acknowledged is there | Linux's |

### 1.6 Eviction

Unchanged in rule: only clean pages are evicted, oldest supplied first,
by a supply over the budget (PK3). Dirty and writing pages are off the
cache's list, so the evictor never meets one and never asks the pager
anything. The dirty limit keeps three quarters of the budget evictable;
PK3's one excess (more supplies at once than the budget has pages) is
unchanged.

### 1.7 Size changes, invalidation, detach, and `pager_write`

- `PAGER_OP_SET_SIZE` shrink drops every page past the end, dirty and
  writing ones too, and counts them done for the requests that need them;
  a write-back request wholly past the end is answered OK. Grow: as now.
- `PAGER_OP_INVALIDATE` on a writable VMO drops clean pages only.
- `PAGER_OP_DETACH` and the pager's last handle drop dirty pages (lost),
  fail write-back requests `ERR_PEER_CLOSED` and release the hold.
- **`pager_write(pager, key, offset, buf, len)`** (new): byte-granular,
  copies `buf` into the pages of the range that are present (any state,
  states unchanged); absent pages stay absent (the disk has the bytes, and
  the next touch fetches them). For extfs's `write` and the zeroed tail of
  a truncate (section 2.2). At most `PAGER_SUPPLY_MAX` bytes; the pages are
  held by references and written with no lock held, so it races a
  mapper's stores to the same bytes as two threads' stores race.
  `ERR_BAD_STATE` on a verified VMO.

### 1.8 The hold: a dirty page keeps its VMO

When a VMO's count of dirty and writing pages goes from 0 to 1, its link
(`struct pager_vmo`) takes a reference on the VMO; when the count returns
to 0 (written, dropped by a shrink, a detach), the reference goes (with no
lock held, as `reap` frees requests). So a VMO with dirty pages lives on
after its last handle and mapping go, and `PAGER_REQ_GONE` comes only once
it is clean: a pager never loses a store because a program unmapped. While
it lives, `PAGER_VMO_FIND` hands it out again, so a program that maps the
file after another unmapped it sees the other's stores (section 2.1).
The hold is bounded: at most the dirty limit's pages, for at most the
window extfs writes back in (Q2).

### 1.9 Fork and children

- FK1's `clone_mode`: a writable mapping of a writable pager VMO is
  always **share**, whatever its flags (the way FK1 makes a JIT VMO's
  writable mapping none): it can only be `MAP_SHARED`, and a private
  snapshot of a file's pages is never what such a mapping means.
- FK2's share clone maps the same VMO in the child. Its entries follow
  section 1.2's rule: none installed writable for a clean page, and the
  child's mapping is on the VMO's reverse map before any entry is, so
  `PAGER_DIRTY_BEGIN` protects it too. WK1 checks FK2's walk against this
  (a ktest: fork with a dirty page mapped, write back, the child's store
  dirties it again).
- `vmo_create_child` of a writable pager VMO (libc's writable
  `MAP_PRIVATE`) is unchanged: an untouched page reads the shared VMO's
  bytes, dirty ones included (Linux shows them too).

### 1.10 Charges

| What | Charged to | When |
|---|---|---|
| a page, clean, dirty or writing | the pager's job (M12.5's Q4), within the budget | supply to eviction, invalidation, shrink or detach: dirtying charges nothing |
| the hold's reference | nothing more (the VMO's own charges) | |
| a record's growth (if WK1 grows it) | the pager's job, as message bytes | with the page |
| `vmo_sync`'s and a waiting writer's request | the calling thread's job, as message bytes (about 160 bytes) | queued to answered; one per thread at a time |
| the background request | the pager's job | one per pager at a time |

A program that dirties a mapped file and exits leaves pages charged to
the pager's job until written back: at most the dirty limit, for at most
the window.

### 1.11 Lock rules and budgets

- The state bits, the lists' moves and the counts change under the VMO
  lock and the cache's lock nested inside it (PK3's order: "vmo", then
  "pager cache"; never the pager's object lock with the cache's).
- `PAGER_DIRTY_BEGIN` protects within CR1's batch budget, one page under
  more mappings than that (up to `VMAR_MAX_OVERLAP` per address space) a
  batch of its own, as decommit and child creation do; the copy-out and
  `pager_write`'s copy-in run with no lock held, on page references.
- A store's wait is a pager wait: the same rules (no region lock, no
  mutex: `aspace_pager_waits_held` stays 0), the same cancellation, and
  FK3's `process_replace` ends it as it ends a read's.

### 1.12 The new interfaces

The numbers are the kernel track's to take at its ABI commit (165 and 166
are free in the pager group; 50 is free in the VMO group today; FK2, FK3
and SG1 add calls of their own).

| Nr | Call | What |
|---|---|---|
| 161 | `pager_create_vmo(..., flags, ...)` | flags `PAGER_VMO_WRITABLE` (1), `PAGER_VMO_FIND` (2): section 1.1 |
| 164 | `pager_op(..., PAGER_OP_WRITTEN, offset, len, status)` | section 1.3; `ERR_INVALID_ARGS` on a VMO not made writable |
| 165 | `pager_dirty(handle_t pager, uint64_t key, uint32_t flags, struct pager_run *run, void *buf)` | `run` in: offset, len (page-aligned, at most `PAGER_SUPPLY_MAX`); out: key, offset, len of the run (0: none), age of its oldest page; `buf` stands for the range asked, the run's bytes copied to their place in it; flags `PAGER_DIRTY_BEGIN`, `PAGER_DIRTY_PEEK`, `PAGER_DIRTY_OLDEST`; `RIGHT_WRITE` on the pager |
| 166 | `pager_write(handle_t pager, uint64_t key, uint64_t offset, const void *buf, uint64_t len)` | section 1.7; `RIGHT_WRITE` on the pager |
| (VMO group) | `vmo_sync(handle_t vmo, uint64_t offset, uint64_t len, uint32_t flags)` | section 1.5; flag `VMO_SYNC_WAIT`; `RIGHT_READ` |

And in `<jam/abi.h>`: `PAGER_REQ_WRITEBACK` (3), `struct pager_run`
(`key`, `offset`, `len`, `age_ns`), the flags above, and the
`PAGER_VMO_RIGHTS` comment's "never `RIGHT_WRITE`" made "but a writable
VMO's".

### 1.13 Compared with Linux and Fuchsia

Linux tracks dirty pages of a shared mapping the same way (write-protect
on write-back, a fault to dirty again), asks the filesystem before the
first store (`page_mkwrite`: blocks reserved, the time updated), throttles
writers (`balance_dirty_pages`) and writes back by age (30 s) from kernel
threads; a failed write-back marks the page clean and keeps the error for
`fsync`. Fuchsia's pagers have the same shape as this plan's: a dirty
request before the first store (optional), "query dirty ranges",
write-back begin and end ops, the pager reading the bytes through its own
handle. The differences here: no first-store request (Q1), the pager
names VMOs by key and holds no handle, so the bytes come out with the
begin (one call), and age is the pager's to watch (no kernel thread).

## 2. extfs

### 2.1 `file.map(MAP_WRITE)` and one VMO per file

- `file.idl` gains `MAP_WRITE` (4): the file's shared VMO, writable. Only
  on a file opened `FS_WRITE` and readable (`ERR_ACCESS_DENIED` otherwise:
  libc checks first, Linux's order), never on a file mapped to run or a
  directory. fat answers `ERR_NOT_SUPPORTED`, as it does for any map
  without a pager.
- **The shared VMO is made writable** (`PAGER_VMO_WRITABLE`) for every
  `MAP_READ` and `MAP_WRITE` of a file on ext: one VMO per file, so a
  read-only mapper sees a writer's stores. A `MAP_READ` caller gets the
  handle without `RIGHT_WRITE`. `MAP_EXEC` is unchanged (a VMO of the
  caller's own, never writable).
- **Keys from the file.** The shared VMO's key is the inode's number and
  generation (with a bit that keeps them apart from `MAP_EXEC`'s
  `XF_MAP_KEY` keys), so the kernel's key table is itself the "one VMO per
  file" index: a map asks `PAGER_VMO_FIND` first and makes a VMO only if
  that is `ERR_NOT_FOUND`. A VMO extfs let go of (U10's `maps_let_go`)
  but that still lives, in mappings or by its hold, is found again,
  instead of U10's "a new VMO each time the file is mapped while not
  open". And an instance that started fresh (its state given up) can
  still write back any key whose inode is there with that generation
  (section 2.6). WE1 keeps U10's slots for what they hold (the inode, the
  keeper's handle, the made-reply marks), looked up by key, and sets a
  found VMO's size from the inode.
- The slot records that the VMO was ever handed out writable, until its
  GONE: Q7's rule below needs it (extfs can't see the handles).

### 2.2 One cache: `write`, `truncate` and `read`

| Request | Today (U10) | With writable VMOs |
|---|---|---|
| `file.write` | data written, then `PAGER_OP_INVALIDATE` of the changed pages, size set, commit, answer | data written, then **`pager_write`** of the same bytes into the file's VMO, size set, commit, answer: a page a mapper stored to keeps both its stores and the write's bytes, and the next write-back writes both |
| `file.truncate`, shrink | size set (pages past it go) | the tail of the new last page zeroed with `pager_write` first, then the size (dirty pages past it go: Linux's rule) |
| grow (truncate or a write past the end) | size set | the old last page's bytes past the old end zeroed with `pager_write` first (a mapper's stores past the end are never file data), then the size |
| `file.read` | from the disk | from the disk, then **`PAGER_DIRTY_PEEK`** over the range: dirty and writing pages' bytes laid over what was read |

**The ordering** (pager.c's header gains it, U10's argument extended):
(a) extfs is one thread, and a write-back runs whole in one turn of its
loop (begin, data, commit, written), between requests, like a page read;
so no write-back runs between a write's data and its answer, and a
`pager_write` never meets a writing page except after a death (below).
(b) `pager_write` comes before the commit: a death before the commit
re-runs the request (its data and its `pager_write` again, the same
bytes); a death after it finds both done. Writing the pages after the
commit instead could let a successor answer a committed write whose bytes
never reached a dirty page, and the page's next write-back would put the
old bytes back over the answered write. (c) A request given up after
writing (U10's case (c): a failed commit, two crashes) leaves its bytes in
pages: clean ones are invalidated (fetched again from the disk); dirty
ones keep them and the next write-back writes them. A given-up write may
so appear through a page a mapper had stored to: rare (two crashes in one
request, or a volume error), and named here rather than paid for.

### 2.3 Serving write-back

A write-back is a journal transaction of its own (U4's begin, run,
commit), no request slot: the kernel holds what a death would lose.

1. `pager_dirty(BEGIN)` over the range, run by run, into a buffer of up
   to 1 MiB.
2. For each run, the file's bytes up to its size now (never past it:
   `ext_write`'s range ends at or before the size, so it never raises
   it): overwrites of written blocks in place; holes filled and unwritten
   extents made written in the transaction, their data written before the
   commit (ordered mode, the same as `write`); the inode's `mtime` and
   `ctime` set (a mapping's store has no earlier moment that extfs sees).
   As many runs as `ext_jnl_room` allows in one transaction.
3. Commit (FUA), then `PAGER_OP_WRITTEN` over the range with OK; on an
   error the transaction is given up and `PAGER_OP_WRITTEN` carries the
   status (Q1).

The journal's hazards are a write's (U3's freed-block revoke, a step
fitting the journal's quarter): a 1 MiB run is 256 blocks of data and a
few of metadata.

### 2.4 When extfs writes back

- **A `PAGER_REQ_WRITEBACK`**: served between requests like a page read,
  one per turn (the binding stays `PORT_BIND_ONCE`). extfs may write back
  the whole file while it is there.
- **By age**: at most once a second, idle or not (beside the journal's
  idle checkpoint in `main.c`), `pager_dirty(OLDEST | BEGIN)` while the
  oldest dirty page is older than the window (Q2).
- **`file.sync`, `fs.sync`**: the file's VMO (by its key), or every key
  extfs knows, written back from offset 0 run by run before the answer.
  `file.sync` returns the error kept for the file since its last sync.
- **An orderly stop** (volumes stopping the mount, a reboot's sync): every
  dirty page written back before extfs goes, bounded by the answer time.
  A disk that went loses them.

### 2.5 Q7, orphans, views

- **Q7 kept both ways**: a file mapped to run refuses `MAP_WRITE` (and,
  as now, an open for writing); a file whose shared VMO was handed out
  writable and still lives refuses `MAP_EXEC` (`ERR_BUSY`, Linux's
  `ETXTBSY` for a file with a writable mapping).
- **Orphans**: an unlinked file stays mapped and writable (U10); its
  slot holds the inode, and the hold keeps the VMO until it is clean, so
  write-backs end before the inode is dropped at its GONE. Writing back an
  orphan's pages is wasted but harmless; skipping them would let a clean
  page be evicted and fetched stale.
- **Rename** changes nothing (the inode is the key).

### 2.6 Restarts and crashes

| extfs dies | What the successor finds | The outcome |
|---|---|---|
| after `pager_dirty(BEGIN)`, before any disk write | writing pages | `PAGER_NEXT_AGAIN` makes them dirty; written again |
| data written in place, no commit | the disk holds some new bytes; the pages dirty again | written again |
| data in new blocks, no commit | the blocks free in the committed bitmaps (replay discards nothing, nothing was committed) | written again in a new transaction |
| committed, before `PAGER_OP_WRITTEN` | the transaction replayed; the pages dirty again (AGAIN); the `msync` waiting not answered | written again (the same bytes or newer), then answered: **an acknowledged `msync` is never lost**, since `PAGER_OP_WRITTEN` comes only after the commit |
| after `PAGER_OP_WRITTEN` | done | `msync` answered |
| in a `file.sync` | the request in its slot (U4) | run again, its write-backs with it |
| a fresh start (the state given up) | no slots | keys from the inode: a write-back request whose inode and generation are there is served; one whose file is gone is detached (its data went with the file) |
| volumes gives up, the disk goes | the pager closed | every VMO detached: dirty pages lost, mappers fail at their next touch |

Test powers (`test.c`): `die=wback:<n>` at each step above.

### 2.7 Errors

A write-back that fails (a full volume for a store over a hole, a disk
error, a volume turned read-only by an error) does what Q1's answer says.
With the recommendation: `PAGER_OP_WRITTEN` with the status (the pages
become clean: readable until evicted, then the disk's bytes come back),
the slot keeps the error for the file's next `file.sync`, and the kernel
keeps it for the VMO's next `vmo_sync`.

## 3. libc

- **`mmap`**: `MAP_SHARED` of a descriptor open for reading and writing
  asks `file.map(MAP_WRITE)` whatever `PROT_WRITE` says (so `mprotect` can
  add write later, as Linux allows), and maps with `VMAR_CLONE_SHARE`
  (FL2's flag; the kernel forces share for these anyway). The answers:
  `ERR_NOT_SUPPORTED` (fat, a mount without a pager) is `ENODEV` with
  `PROT_WRITE` and, without it, falls back to `MAP_READ` (today's
  read-only mapping); a descriptor that can't write: `EACCES` with
  `PROT_WRITE` (checked before asking, Linux's order), else `MAP_READ`.
  `MAP_PRIVATE` and memfds are unchanged.
- **`mprotect`** adding write over a shared file mapping: allowed where
  the VMO's handle has `RIGHT_WRITE` (the kernel's maximum), `EACCES`
  elsewhere (FAT, a read-only descriptor), as today.
- **`msync`**: each table entry in the range gets `vmo_sync` over its part
  (`MS_SYNC` with `VMO_SYNC_WAIT`, `MS_ASYNC` without); the kernel answers
  `OK` at once for anonymous memory and memfds. `EIO` for `ERR_IO`,
  `ERR_TIMED_OUT` and `ERR_PEER_CLOSED`; `ENOMEM` for a range not wholly
  mapped (today's check).
- **`munmap`, `close`, `madvise`**: unchanged (a shared mapping's pages
  are never decommitted by libc; the kernel refuses decommit of a pager
  VMO anyway).
- **`fsync`, `fdatasync`, `sync`, `syncfs`**: unchanged calls
  (`file.sync`, `fs.sync`); extfs gives them their new meaning.
- **FAT keeps `ENODEV`**, and posixtest says so on a FAT `/data`.

## 4. Security

- **A writable mapping needs a writable open.** extfs hands out a handle
  with `RIGHT_WRITE` only for `file.map(MAP_WRITE)` on a channel opened
  `FS_WRITE` and readable. Whether the open could write was judged by the
  view when it was made: `FS_VIEW_READ_ONLY`, `FS_VIEW_GUARD_ETC` (where
  the path resolves, U4's rule), `FS_VIEW_GUARD_PKG` and the permission
  bits (Q4) refuse such an open, and files a view hides (`/data/etc`'s
  keys and the rest of `etc`) can't be opened at all. So nothing a view
  forbids writing can be mapped writable.
- **A mapping is an open**: like a descriptor, it outlives a rename (into
  `etc` too, which only a view allowed to change `etc` can do), a
  `chmod` and its own `close`. The handle may be passed on (handles are
  capabilities); `MAP_READ`'s handle has no `RIGHT_WRITE`, so passing it
  passes no write.
- **Code never comes from a writable page**: a writable VMO is never
  executable or verified (1.1), a file mapped to run can't be mapped
  writable and the reverse (2.5), programs from `/jam` (U8) and packages
  run from `MAP_EXEC` VMOs, and the package area is guarded.
- **A hostile mapper** can dirty at most the dirty limit of its mount and
  then waits; it can't make extfs hang (each request has a deadline), make
  other mappers' reads wait (three quarters of the cache stay clean), or
  make the kernel wait (no lock is held across a wait). It can make extfs
  write at most the dirty limit once per window, no more than a program
  calling `write` in a loop can.
- **The pager** (only a holder of `RIGHT_ROOT_PAGER`: volumes' extfs)
  reads and writes only its own VMOs' pages through the new calls, file
  data it serves anyway. A confused pager can lose its own files' stores,
  as it can lose their writes.
- **No stale bytes leak**: pages past the end are supplied zero, a store
  past the end is never written to the disk, and a truncate or growth
  zeroes the tail of the last page (2.2).

## 5. Tracks

Each about one agent-hour; WE2 about one and a half.

| Track | What | Files | Gate | Tests | Size | Model |
|---|---|---|---|---|---|---|
| **WK1. Writable pager VMOs** | first commit "WK1 ABI:" (every call, flag, op and request kind of 1.12 with its contract text, the stubs `ERR_NOT_SUPPORTED`), so WE1 and WL1 can start; then 1.1, 1.2 (the states, the write fault, clean pages read-only, the lists and counts; `PAGER_DIRTY_BEGIN` left to WK2), `pager_dirty`'s `PEEK`, `pager_write`, the hold (1.8), `FIND` and the GONE takeover, 1.7's rules for shrink, invalidate and detach, FK1's clone-mode override; built, see "WK1 notes" | `kernel/object/vmo.c` (the pager-VMO paths, `vmo_fault_map`, `vmo_umap_add`), `kernel/object/pager.c`, `kernel/object/pager_cache.c` and its header, `kernel/mm/aspace.c` (`clone_mode` only), `kernel/abi/pager_sys.c` and `kernel/abi/sysc_pager.c`, `<jam/pager.h>`, `<jam/abi.h>` and `abi/syscalls.def` (their lines), `kernel/abi/abi_check.c`, a new ktest file kernel/test/test_pager_dirty.c | FK2 merged (vmo.c and aspace.c); beside FK3 and SG1/SG2 | ktests with a kernel-thread pager: a read fault on a writable mapping installs read-only, a store dirties, eviction never takes a dirty page, `PEEK`, `pager_write` on present pages only, the hold (GONE only after clean), `FIND` and the takeover, shrink, invalidate and detach with dirty pages, a fork (once FK2's clone is in) shares the VMO and its clean pages stay read-only; full ktest 4 and 8 | 1 | **Opus** |
| **WK2. Write-back** | 1.3: `PAGER_REQ_WRITEBACK` (made by the background count; `vmo_sync`'s and the throttle's come in WK3), `PAGER_DIRTY_BEGIN` (protect within the batch budget, one shootdown, then the copy), `PAGER_DIRTY_OLDEST`, `PAGER_OP_WRITTEN` and what a request needs (the stamps), the error kept (Q1), `PAGER_NEXT_AGAIN` turning writing pages dirty; built, see "WK2 notes" | WK1's kernel files, kernel/test/test_pager_dirty.c | WK1 merged | ktests: begin protects in every mapping (several address spaces, `VMAR_MAX_OVERLAP` mappings of one page), a store after begin dirties again and is never lost (8 threads storing counters while a pager thread loops begin, copy, written: the last image holds every thread's last count), written with a re-dirtied page leaves it dirty, a death (AGAIN) between begin and written, `vmo_batch_cost_most` within `VMO_BATCH_BUDGET`; full ktest 4 and 8 | 1 | **Opus** |
| **WK3. Waiting** | 1.4 and 1.5: the dirty limit, a store's wait in `aspace_fault` and in user copies (no region lock, the deadline in all, cancellable), the throttle's request, `vmo_sync` and its requests; the failure table; built, see "WK3 notes" | WK1's kernel files, `kernel/mm/aspace.c` (the fault's wait), `kernel/abi/sysc_vm.c` (`vmo_sync`), kernel/test/test_pager_dirty.c | WK2 merged | ktests: a writer at the limit waits and goes on when the pager writes; a pager that never answers: the writer is killed at the deadline with the line, a copy returns `ERR_IO`, a reader of another VMO of the same pager is never held up; `vmo_sync` answered exactly (a store before it, written by a write-back that began before it, keeps it waiting); `vmo_sync` failing on a close; a kill ends every wait; full ktest 4 and 8; soak 2 | 1 | **Opus** |
| **WE1. extfs: the map and one cache** | 2.1 and 2.2: `MAP_WRITE`, every shared VMO writable, keys from the inode, `FIND`, the sizes of a found VMO, rights reduced for `MAP_READ`; `pager_write` in `write` and truncate (the tails), `PEEK` in `read`; Q7 both ways; the ordering argument in pager.c's header; built, see "WE1 notes" | `user/services/extfs/pager.c`, `user/services/extfs/files.c`, `user/services/extfs/fsops.c`, `user/services/extfs/extfs.h`, `user/services/extfs/adopt.c` (the slots' lookups), `abi/idl/file.idl` (`MAP_WRITE`, the comments) and its generated header, a new utest file user/tests/utest/extfs_wmap.c | WK1 merged; U9 merged (it works in extfs); no other track in extfs | utest `extfs_wmap`: a store seen by `read`, by a second mapping and a second process; `write` and a store on one page both kept; truncate shrink and grow with the tails zero; a map after the first mapper unmapped (its pages still dirty) finds the same VMO; `MAP_WRITE` refused without `FS_WRITE`, through a read-only view and on a file mapped to run; `MAP_EXEC` refused while a writable map lives; `extfs_map` and `extfs_restart_pager` unchanged; init run | 1 | **Opus** |
| **WE2. extfs: write-back** | 2.3 to 2.7: serving `PAGER_REQ_WRITEBACK`, a transaction a batch of runs, the size clip, holes and unwritten extents in ordered mode, the times, the age check, `file.sync` and `fs.sync`, the orderly stop, errors kept per slot, a fresh start's keys; test powers `die=wback:<n>`; extcheck's cut storm gains a write-back step | `user/services/extfs/pager.c` (or a new user/services/extfs/wback.c beside it), `user/services/extfs/main.c` (the age check), `user/services/extfs/fsops.c` (sync), `user/services/extfs/test.c`, `user/services/extfs/extfs.h`, `tools/extcheck.c` (the storm's new step), new utest files for `extfs_wback` and `extfs_restart_wback` | WE1, WK3 merged | utest `extfs_wback` (stores written by `msync`, by `fsync`, by age; a store over a hole and in an unwritten extent; a full volume's error at the next sync), `extfs_restart_wback` (a death at every step of 2.6, the data right after each); `tools/extfs-svc-test.sh` (every disk `e2fsck -fn` clean); extcheck's cut storm (`make check`); init run | 1.5 | **Opus** |
| **WL1. libc** | section 3; built, see "WL1 notes" | `user/posix/sys/file.c` (`map_file`), `user/posix/sys/mem.c` (`lx_msync`, the share flag), `user/posix/sys/files.h`, posixtest's `user/posix/test/mmap.c` (a new section `mmap-wshared`), tools/shell-tests' new posix-mmap-wshared.txt and its case in `tools/posix-mmap-test.sh` | WE1 and WK3 merged (beside WE2: different files); after or before FL2 (both edit `mem.c`: one after the other) | posixtest `mmap-wshared` on ext (`MMAP_DATA=ext`): a store seen by `read` and `pread`, by a second mapping, by a child; `write` and stores on one page both kept; `mprotect` adding write on an `O_RDWR` mapping, `EACCES` on `O_RDONLY`; `msync` `MS_SYNC` and `MS_ASYNC` return 0; truncate's tail zero; on FAT `ENODEV` and `EACCES` as today; the `mmap` and `coherent` cases on FAT and ext unchanged; shell scripts basic, cmds, commands, files | 1 | Sonnet 5.5 |
| **WT1. The end-to-end tests** | the scripts below, and Chromium's tests through CB2's harness | `tools/pager-test.sh` (new cases), pagertest (`user/tests/pagertest/`, a new mode that stores records with sequence numbers and checksums), `tools/cr-test.sh`'s `base` filter list (a line) | WE2 and WL1 merged; CB2 merged and `/tmp` on ext (U7b or U11a) for the Chromium case | pager-test.sh `wmsync`: records stored and `msync`ed, `kill extfs-jam`, read back through `read` and a new mapping; again with no `msync` (the successor writes them); again across a reboot (`msync`ed records there). `wcut`: QEMU stopped dead during a store-and-`msync` storm, booted again: every acknowledged record whole, Linux's `e2fsck -fn` clean. `wpress`: a mapping twice the budget stored through (the writer throttled, done, every page right) while another program reads another file at speed; then with extfs held by a test power (`hang=wback`): the writer killed at the deadline with the line, the reader, the shell and `/jam` fine. cr-test.sh `base` with `MemoryMappedFileTest.*`, `FilePersistentMemoryAllocatorTest.*` and `PersistentHistogramAllocatorTest.*` (*unsure* of the exact suite names: CB2's list says) | 1 | Sonnet 5.5 |
| **WR. Independent review and fix** | everything above, findings first (a new docs/WRITEMAP-REVIEW.md), then High and Medium fixed with a test each: can a store be lost (begin's protect against the copy, the request's stamps, AGAIN), can a writer or the kernel wait forever, can a dirty page be evicted, does a view's refusal hold for every way to a writable handle, is an acknowledged `msync` safe across every death | the files its fixes need | WK1 to WT1 merged | its own; hostile: 8 writers on one file from two processes with fork and a pager killed every 200 ms; full ktest 4 and 8; soak 4 | 1.5 | **Opus** |
| **WJ. The join** | ARCHITECTURE (Memory: writable pager VMOs and the hold; Storage: write-back), M12.5-PLAN's and M13-PLAN's lines that say pager pages are always clean, TESTING (the new cases and ktests), ROADMAP, CR-PLAN, HISTORY; this plan deleted when HISTORY covers it | the docs | WR | `make check` | 0.5 | Sonnet 5.5 |

**Order.** WK1 as soon as FK2 is merged (both are in `vmo.c` and
`aspace.c`), beside FK3 (they share only `aspace.c`, in different
functions: merged one after the other, a rebase) and beside SG1 and SG2
(no shared file: SG's fault table already turns `ERR_TIMED_OUT`,
`ERR_IO` and `ERR_PEER_CLOSED` into SIGBUS). WK2 after WK1, WK3 after WK2
(the same files, one at a time, as CR's order has the kernel). WE1 once
WK1 is merged and U9 is out of extfs; WE2 after WE1 and WK3; WL1 after WE1
and WK3, beside WE2. WT1 after WE2 and WL1. WR after all of them, WJ last.
After each kernel merge the main session runs `make syscalls`.

**Size**: about 9.5 agent-hours. In lines: about 1,000 in the kernel (WK1
400, WK2 350, WK3 250), 700 in extfs, 150 in libc, besides tests.

**Risks**
1. *A lost store*: a store between the copy and the write-protect would be
   on no disk and in no dirty page. The protect and its shootdown come
   before the copy, a store after it faults; WK2's storm test and WR read
   this first.
2. *A wait without end*: every wait has the answer time, and nothing holds
   a lock across one; WK3's never-answering pager and WT1's `wpress`.
3. *A hole in "one cache"*: `write`, `read`, truncate and growth each
   meeting a dirty page; WE1's utest runs every class, WL1's posixtest
   again through libc.
4. *An acknowledged `msync` lost by a restart*: `PAGER_OP_WRITTEN` only
   after the commit, and AGAIN; WE2's deaths at every step and WT1's
   `wcut`.
5. *Memory held after programs exit* (the hold): bounded by the dirty
   limit and the window; the soak's growth figures show it.
6. *FK2's clone* written before writable pager VMOs exist: WK1's fork
   ktest checks its share mode against section 1.2.

### WK1 notes

What WK2, WK3 and WE1 start from (ktests `pager_dirty_*` in
kernel/test/test_pager_dirty.c):

- **The ABI** is in (commit "WK1 ABI:"): every number of 1.12 with its
  contract text in syscalls.def (`vmo_sync` took 50, `pager_dirty` 165,
  `pager_write` 166), `PAGER_WRITABLE_MIN_PAGES` (8) beside the flags.
  Still `ERR_NOT_SUPPORTED`: `vmo_sync` (WK3), `PAGER_OP_WRITTEN` and
  `pager_dirty`'s `BEGIN` and `OLDEST` (WK2). Decided in the text:
  `pager_dirty` with flags 0 copies dirty pages only (PEEK adds writing
  ones); with `OLDEST`, `buf` stands for the run itself; `PAGER_VMO_FIND`
  takes size 0 and no other flag; `pager_write`'s len 0 does nothing, and
  it works on any pager VMO but a verified one (a read-only VMO's clean
  pages take the bytes too).
- **The record** grew to 24 bytes (`PAGER_RECORD_BYTES`, charged as
  before): the VMO, a 32-bit index, the state (`PAGER_PAGE_DIRTY`,
  `PAGER_PAGE_WRITING`) and `dirtied`, the uptime of its first store since
  it was last clean. The cache has a second list, `dirty` (oldest dirtied
  first), and `ndirty`; a page is on `pages` or `dirty` by its `node`,
  never both, and the state and the list change together under the VMO
  lock and the cache's (`pager_cache_store_vlocked`).
- **The store** is `store_locked` in `vmo_fault_map` (the one path): a
  present page of a writable VMO is installed with the write bit only
  while its state has `DIRTY`. WK3's limit wait goes just before the
  store marks the page (the comment there). WK2's BEGIN clears `DIRTY`
  and sets `WRITING` under the VMO lock with the entries write-protected,
  and the same rule then keeps them read-only until the next store.
- **A popped page dirtied**: an evictor pops the oldest clean page and
  only then locks its VMO; a store in between makes it dirty, moves it to
  `dirty` and counts it again, and `vmo_pager_evict` (`take_batch_locked`'s
  `clean_only`) leaves it. `resident` is then one past the budget until a
  clean page goes (ktest `pager_dirty_popped_then_stored`, through the new
  hook `DBG_PAGER_EVICT`).
- **The hold** is `held` and `dirty` in struct vmo (the VMO lock): the
  first store takes a reference, `hold_end` gives it back once `dirty` is
  0. Called after a shrink and a detach; WK2's `PAGER_OP_WRITTEN` must
  call it after pages become clean. `vmo_destroy` asserts no hold.
- **The GONE takeover** withdraws a queued or delivered GONE (the
  contract tells a pager to answer one it holds before it makes the key
  again). The old test `pager_gone` now checks the takeover.
- **No dirty limit yet**: dirty pages can fill the budget; a supply with no
  clean page to evict then goes past it as reservations do (WK3).
- **The writing state** is there in the record and the fault rule, but
  nothing makes a page writing before WK2's BEGIN; its moves and their
  tests are WK2's.
- **Clones**: `clone_mode` makes a writable VMO's mapping with write in its
  maximum shared, whatever the flags; `clone_ok` refuses anything else.
  FK2's walk checked: a twin is on the VMO's reverse map when made and
  gets no entry (its faults make them), so no clean page is ever writable
  in a child. The plan's "write back, then the child's store dirties it
  again" half is WK2's to add to `pager_dirty_fork`.
- The kernel API keeps `pager_create_vmo(pg, key, size, out)` (flags 0)
  for the many tests that use it; `pager_create_vmo_flags` takes flags.

### WK2 notes

What WK3 and WE1 start from (ktests `pager_dirty_begin_protects` to
`pager_dirty_oldest` in kernel/test/test_pager_dirty.c, and the fork
test's write-back half):

- **Three lists.** The cache keeps `pages` (clean), `dirty` (dirty and not
  being written back, oldest first) and `writing` (writing, dirty as well
  or not); `ndirty` counts the last two, `nwriting` the last. A page's age
  (`dirtied`) is its first store while clean, or, for a page stored to
  while it was being written, the end of that write-back (a page stored
  to all the time is written once per age, not on every check); a
  write-back cut off puts its pages back at the head of `dirty` with their
  age (made no younger than the head's).
- **One write-back of a page at a time.** BEGIN takes pages that are dirty
  and not writing; one stored to while writing (both bits) waits for that
  write-back's WRITTEN, which leaves it dirty, and the next BEGIN takes
  it. Flags 0 still means the dirty bit (WK1's), PEEK dirty or writing.
- **BEGIN** (`vmo_pager_dirty_run` with `PAGER_DIRTY_BEGIN`): the run cut
  by `batch_end_locked` at `VMO_BATCH_PROTECT_COST`, the pages writing,
  the requests told, `wrprotect_umaps_locked` and `batch_note`, then the
  shootdown with no lock held, all before the copy. A copy that fails (a
  bad buf) makes the run dirty again (`ERR_INVALID_ARGS`, nothing begun);
  `sysc_pager_dirty` writes the run struct back before a BEGIN, so a run
  can't begin without the pager being told where.
- **OLDEST**: the head of `dirty` (`pager_cache_first`), the run reaching
  back before it as far as `run->len` allows. `run->age_ns` is now in and
  out: in, the least age wanted; a younger oldest page gives no run and
  its age, so the age write-back is one call a check and knows when the
  next is due. **The age setting (Q2):** `PAGER_WRITEBACK_AGE_NS` (10 s)
  in `<jam/abi.h>` is its default; the setting itself is the
  filesystem's (WE2 names it, reads it at mount and on a settings change,
  and passes it in each call), and the kernel keeps no copy and no timer.
- **What a request needs** (1.3's stamps, kept as bits instead of
  sequence numbers): a write-back request (`struct pager_wb`, pager.c) has
  two bits a page, set when it is made: `need` (the page was dirty or
  writing) and `begin` (it was dirty). BEGIN clears `begin`; WRITTEN
  clears `need` where `begin` is clear. Since a page has one write-back
  at a time, AGAIN and a failed copy change no bit (the next end of that
  page's write-back is of one begun later still). Charged
  `sizeof(struct pager_wb)` (a read request's size is unchanged). They
  are on the link's `wbs`; read requests stay alone on `reqs` (a page in
  one at most). Answered when no `need` bit is left (with the first error
  one of its pages ended with, else OK), by a shrink (bits past the end
  done; wholly past it, answered), failed `ERR_PEER_CLOSED` by a detach or
  the pager's close. A delivered one that a WRITTEN leaves needing only
  pages nobody is writing back (writing and dirty when it was made) goes
  back in the queue, so the pager is asked again. Deadlines as for reads:
  `pager_next` fails an expired one, a waiter times it out.
- **For WK3:** `vmo_pager_writeback(v, first, end, &r)` (256 pages at
  most) makes and queues one with a reference for the caller
  (`pager_req_wait`, `pager_req_put`), or none if nothing there needs
  writing: `vmo_sync`'s, a chunk at a time. The throttle's can take the
  shape of the background one (`vmo_pager_writeback_bg`: the run around
  the oldest dirty page). `pager_dirty_limit` is the quarter of the
  budget. The limit's wait goes before `store_locked` (WK1's comment).
  The kept error is `pwb_error` in struct vmo (the latest WRITTEN error;
  the pages clean all the same), taken by `vmo_pager_take_error` (once);
  a request's answer carries its pages' error too, so `vmo_sync` should
  take the kept one whenever it reports one, or it is reported twice.
- **The background request** (`kick` in pager.c) after a store that made
  a clean page dirty (`vmo_fault_map`, once the VMO lock is dropped; the
  region lock may be held: spinlocks and an allocation only) and after
  every WRITTEN: past half the dirty limit and with none outstanding, one
  for the run of dirty pages around the oldest, charged to the pager's
  job, with no waiter; the next kick fails one past its deadline.
- **AGAIN** walks `writing` from its head, one page per VMO lock hold, as
  many as were writing when it started (a live writer's new ones go to
  the tail and can't keep it going).
- **Small things decided:** `PAGER_OP_FAIL` fails read requests only (a
  pager that can't write answers with BEGIN and WRITTEN's error: Q1's A);
  WRITTEN takes any length, 256 pages per VMO lock hold, and clips at the
  size (a shrink may race a write-back) instead of refusing;
  `vmo_pager_writeback` refuses a VMO not writable `ERR_NOT_SUPPORTED`;
  the test pager thread (`kpager`) leaves write-back requests unanswered,
  as the WK1 tests with small budgets now get background ones.
- **For WE1**, the pager's side: on a `PAGER_REQ_WRITEBACK`, `pager_dirty`
  with BEGIN over its range from each run's end until none is left, write
  the runs, commit, then one WRITTEN over the range (OK, `ERR_IO` or
  `ERR_NO_SPACE`); by age, once a second, OLDEST with BEGIN and the
  setting in `age_ns` while it gives a run, each written and answered
  with a WRITTEN over the run; a successor's first `pager_next` with
  AGAIN makes the old writing pages dirty and gives the old requests
  back.

### WK3 notes

What WE2, WL1 and WR start from (ktests `pager_dirty_limit_waits` to
`pager_dirty_closed` in kernel/test/test_pager_dirty.c; the contract
commit "WK3 ABI:"):

- **The limit** is checked where the count is kept: a store makes a
  clean page dirty only while the cache's `ndirty` is under
  `pager_dirty_limit` (a quarter of the budget), in one hold of the
  cache's lock, so stores to two VMOs can't pass it together. A store to
  a dirty or writing page never waits (it adds nothing). At the limit
  `store_locked` changes nothing and `vmo_fault_map` answers
  `ERR_NO_RESOURCES` (kernel-internal: `aspace_fault` never returns it).
- **The wait** (`store_wait` in aspace.c, beside `fault_wait`): the
  region lock dropped, `vmo_pager_throttle` (pager.c's `pager_throttle`)
  gives the request to wait on: the pager's one request for its oldest
  run (`bg`), joined, or made if there is none (charged to the storer's
  job; one past its deadline fails first). With no dirty page left (all
  being written back) it is made around the first writing page, so its
  end is that write-back's end. The wait is `pager_req_wait_until`
  (cancellable; gives up at the store's own time without failing the
  request for its other waiters). However the request ends (written, an
  error, its own deadline, another VMO's detach), the store looks again
  from `find`; only the store's time (the pager's answer time from its
  first wait, checked at every look), a kill, or a closed pager or
  detached VMO end it. These waits don't count against `FAULT_ROUNDS`
  (they are bounded by time). The failure table of 1.4 holds as written:
  uentry.c's existing mapping turns `ERR_TIMED_OUT` into "page fault: the
  file's pager didn't answer" and every pager failure of a copy into
  `ERR_IO`.
- **`vmo_sync`** is `vmo_pager_sync` (vmo.c) behind `sysc_vmo_sync`:
  flags, then OK for any VMO not a writable pager VMO, then the range
  (`ERR_OUT_OF_RANGE`) and a detach (`ERR_PEER_CLOSED`, even with nothing
  dirty); a chunk (256 pages) at a time through `vmo_pager_writeback`,
  each made only once the one before it is answered (so each needs what
  was dirty when it was made: exact for every store before the call),
  none if the VMO has no dirty page. With the wait, the kept error is
  taken after the last chunk (and after a chunk that failed `ERR_IO` or
  `ERR_NO_SPACE`): it is now the first since the last report, as the
  contract said (WK2 kept the latest). Without the wait the requests are
  only queued, a chunk the job can't pay for ends the asking (the age
  write-back gets the rest), the kept error stays for a waiting call, and
  the answer is OK unless the VMO was cut off: the "WK3 ABI:" commit
  says so in the contract.
- **For WE2:** a request for the oldest run made at the limit can name
  pages that are all being written back already; a pager that serves one
  request at a time (begin, write, commit, WRITTEN, then the next) finds
  it answered before it reads it. A pager with write-backs in flight at
  once must not answer a request with a WRITTEN over pages another of its
  write-backs began and hasn't finished: WRITTEN ends whatever is writing
  in its range.
- **For WL1:** section 3's `msync` holds, with one more error to map:
  `ERR_NO_SPACE` (a write-back that met a full volume). A mapping reaching
  past a shrunk file's end makes the call `ERR_OUT_OF_RANGE`: clip the
  range at the VMO's size first.
- **Left:** `pager-test`'s `wpress` (a hung pager under memory pressure)
  is WT1's; the store's wait isn't made interruptible (a signal doesn't
  end it, as a read's doesn't).

### WE1 notes

What WE2 and WL1 start from (utest `extfs_wmap`; pager.c's header has
the design and the ordering argument):

- **`FILE_MAP_WRITE`** (4) in file.idl. extfs: only on an open with
  `FS_READ` and `FS_WRITE` (`FS_APPEND` beside them is allowed, as Linux
  allows `O_APPEND`), never with `nochange` or on a volume gone
  read-only (`ERR_ACCESS_DENIED`), never of a file mapped to run
  (`ERR_BUSY`, unreachable in practice: such a file can't be opened for
  writing). fat answers `ERR_NOT_SUPPORTED` (one line in fileops.c).
- **One VMO per file.** Every shared VMO is made `PAGER_VMO_WRITABLE`
  under `XF_SHARED_KEY(ino, igen)`: bit 63, the inode number in bits
  32..62, the generation in 0..31 (an inode number past 2^31 - 1 has no
  key: `ERR_NOT_SUPPORTED`). A map looks the slot up by key; if extfs
  holds no handle, `PAGER_VMO_FIND` first, `PAGER_OP_SET_SIZE` from the
  inode on a found VMO, else a new one (which takes a pending GONE over).
  `FILE_MAP_READ` hands out `PAGER_VMO_RIGHTS` (no `RIGHT_WRITE`),
  `FILE_MAP_WRITE` all of them. `FILE_MAP_EXEC` keys are unchanged
  (`XF_MAP_KEY`, bit 63 clear: the clock-based sequence must stay below
  2^55, over a year of uptime; it was 2^56).
- **The slot** gained `written` (handed out writable since made, until
  its GONE: Q7's `ERR_BUSY` to `FILE_MAP_EXEC`, and `read` peeks only
  such a VMO) and `made_write` (adopt.c's remade answer gets the rights
  the first one had), both in `struct xf_map`'s old padding (the state's
  layout is the same size). A shared slot is never unmade (request.c's
  failed reply, adopt.c's run-again): its key is the file's, the request
  run again finds it and its VMO (the keeper's copy, dropped by a message,
  can still hold it), and it goes at its GONE. A VMO found with no slot
  (a fresh start: the instance before's) counts as `written`.
- **One cache.** `write`: `pager_write` of its bytes (after ext wrote
  them, before the commit), the old last page's tail zeroed first when it
  grows, then the size. Truncate: the tail past the smaller of the two
  sizes zeroed, then the size. A failed write or truncate: the old
  INVALIDATE (clean pages only now) and the size. A request given up
  (maps_settle): the sizes from the inodes and an INVALIDATE of what
  `pager_write` touched. `read`: `PAGER_DIRTY_PEEK` over the range, in
  64 KiB pieces through pager.c's request buffer, for a slot `written`;
  an error there is `ERR_IO` (logged once).
- **Not yet (WE2):** a `PAGER_REQ_WRITEBACK` is logged once and left to
  its deadline, so a dirty page stays dirty and its VMO lives (the hold)
  until the pager closes; `file.sync` still only flushes. After a fresh
  start a request for a key the state doesn't know is still detached
  (pager.c's `read_request`, `gone`): section 2.6's "keys from the
  inode" means looking the inode up from the key's bits and making a
  slot (holding the inode as an orphan if unlinked). WE2's write-back
  buffer (up to 1 MiB) needs its own: pager.c's `buf` is the read
  requests' and `peek`'s.
- **For WL1:** `map_file`'s `MAP_SHARED` with `PROT_WRITE` on ext is
  `file.map(FILE_MAP_WRITE)`; `ERR_NOT_SUPPORTED` (fat) stays `ENODEV`,
  `ERR_ACCESS_DENIED` `EACCES`, `ERR_BUSY` `ETXTBSY`. A `FILE_MAP_READ`
  handle can't be mapped writable, so `mprotect` adding write needs the
  mapping to have come from `FILE_MAP_WRITE` (on an `O_RDWR`
  descriptor); asking for it for every shared mapping of an `O_RDWR`
  file makes the file `written` (not runnable) while that VMO lives.

### WL1 notes

What WE2's msync cases, WT1 and WR start from (posixtest `mmap-wshared` in
user/posix/test/mmap.c; tools/posix-mmap-test.sh case `wshared`):

- **WE1's open question, settled: every `MAP_SHARED` of a descriptor open
  for writing asks `FILE_MAP_WRITE`, whatever `PROT_WRITE` says**
  (file.c's `map_file`; section 3 as written). Why: Linux decides at
  `mmap` whether a shared mapping may ever be written (`VM_MAYWRITE`, from
  the descriptor's mode then), and `mprotect` adding write is allowed
  after the descriptor's close; the handle's right to write is fixed by
  what the service gave, so the choice has to be made at `mmap`. The
  price is Linux's own: its mapping keeps the file open for writing, so
  the file can't be run (`ETXTBSY`) while any mapping of it lives, a
  `PROT_READ` one too; here extfs marks the file `written` while its
  shared VMO lives (Q7). Kept narrow: only an `O_RDWR` `MAP_SHARED` pays
  it (a read-only descriptor, `MAP_PRIVATE`, a memfd, a file on fat or
  anywhere without a writable pager ask `FILE_MAP_READ` as before), and
  only for as long as the VMO lives. Without `PROT_WRITE` any refusal
  (`ERR_NOT_SUPPORTED`, `ERR_ACCESS_DENIED`, `ERR_BUSY`) falls back to
  `FILE_MAP_READ`, today's read-only mapping, whose `mprotect` adding
  write is `EACCES`; with `PROT_WRITE` the refusal is the answer
  (`ENODEV` fat, `ETXTBSY`, `EACCES` or `EROFS`). Rejected: asking only
  with `PROT_WRITE` (an `mprotect` Linux allows would be `EACCES`), and
  getting the writable VMO at `mprotect` (the descriptor may be closed by
  then; keeping the open for it holds the file for writing just the same,
  and costs state in every table entry). The check order is Linux's:
  `PROT_WRITE` on a descriptor that can't write is `EACCES` before the
  service is asked. libc calls the generated `file_map_within` itself
  (`fs_map_file` in libos still takes `FILE_MAP_READ` and `EXEC` alone).
- **`mprotect`** needed no change: the kernel allows write on a mapping
  whose VMO handle has `RIGHT_WRITE` (the `FILE_MAP_WRITE` one) and
  answers `ERR_ACCESS_DENIED`, so `EACCES`, for a `FILE_MAP_READ` one.
- **`msync`** (mem.c's `sync_shared`): `vmo_sync` over each `MAP_SHARED`
  table entry in the range, `VMO_SYNC_WAIT` for `MS_SYNC`, none for
  `MS_ASYNC`; the first error ends it, and an unmapped part of the range
  is `ENOMEM` after the mapped parts were synced (an error from them
  comes first, as Linux's). One entry at a time with `mem_lock` dropped
  for the call and the VMO's handle duplicated for it: an `MS_SYNC` can
  wait the pager's whole answer time, and the other threads' `mmap`,
  `munmap` and `malloc` must not wait with it. A private mapping and
  anonymous memory are never asked (Linux's `msync` syncs `VM_SHARED`
  alone); a shared memfd or anonymous mapping costs a call the kernel
  answers `OK` to. Errors: `ERR_IO`, `ERR_TIMED_OUT`, `ERR_PEER_CLOSED`
  are `EIO`, `ERR_NO_SPACE` (WK3's one more) `ENOSPC`, as `fsync`'s;
  `ERR_OUT_OF_RANGE` (a mapping reaching past a shrunk file) is clipped
  at the VMO's size and asked again, and what is left is no error;
  `ERR_ACCESS_DENIED` and `ERR_WRONG_TYPE` (no right to read the VMO) are
  `0`. `MS_INVALIDATE` is accepted and does nothing: every mapping of a
  file is the one VMO.
- **The tests.** `mmap-wshared` (asked for by name; not in a bare
  `posixtest` run until WE2 is in, because its last step waits): on ext, a
  store seen by `pread`, `read`, a second mapping from a read-only
  descriptor, a second writable one, and a forked child (through its
  inherited mapping, and through its own descriptor and mapping, its
  stores and `pwrite` seen by the parent); `write` and stores on one page
  kept in either order; a file grown by `write` seen through the mapping,
  the gap zero; `mprotect` adding write on an `O_RDWR` `PROT_READ` mapping,
  after the descriptor's close and on a `PROT_NONE` one, `EACCES` on
  `O_RDONLY` (also `mmap` with `PROT_WRITE`); `MAP_PRIVATE` writes never
  the file's; a store past the end of the last page, and a cut then
  regrown file, zero after growth by `ftruncate` and by a gap-leaving
  `write`; `msync` on a file nothing was stored to (both flags, a part,
  a hole `ENOMEM`), then over stored pages. On fat: `ENODEV` with
  `PROT_WRITE`, `EACCES` on a read-only descriptor, an `O_RDWR`
  descriptor's `PROT_READ` mapping with `mprotect` `EACCES`, a `write`
  seen through it. The `mmap` section's `shared_write_refused` now expects
  a mapping on ext (`statfs`' type) and `ENODEV` elsewhere. The case boots
  twice: the image (ext `/data`: `mmap-wshared`), then a twin with a FAT
  `/data` (`data_fat_image`: `mmap`, `mmap-coherent`, `mmap-wshared`). A
  second stick can't do the FAT half: it mounts read-only, and a POSIX
  program's view of it stays so after the shell's `mount -w`.
- **Waits for WE2.** `MS_SYNC` over stored pages on ext: until extfs
  serves `PAGER_REQ_WRITEBACK` it ends `EIO` after the pager's 60 s
  (`mmap-wshared`'s `msync` step, the last, fails alone, and its summary
  line says how long it took). **Merge WL1 with or after WE2**: with WL1
  alone a shared writable mapping on ext succeeds where it was `ENODEV`,
  and its stores are never written to the disk (the pages stay dirty and
  held; at the dirty limit a writer waits out the answer time and is
  killed).
- **Runlist.** `tools/posix-mmap-test.sh`'s default cases gain `wshared`;
  the prepush list needs `mm-wshared  MMAP_CASES=wshared
  tools/posix-mmap-test.sh build/t/mm-wshared`. Two runs of the script
  that differ only in `MMAP_DATA` need a second argument (their name), or
  their serial sockets clash and one boot's monitor never answers.

## 6. Tests

| What | Where |
|---|---|
| The page states, write-back, the waits, the hold, budgets, a store storm | ktests in kernel/test/test_pager_dirty.c (new); full ktest 4 and 8; soak |
| extfs's one cache and write-back, its deaths | utest `extfs_wmap`, `extfs_wback`, `extfs_restart_wback`; `tools/extfs-svc-test.sh` with `e2fsck -fn` |
| The write-back transaction cut at every write | extcheck's cut storm (`make check`, the Mac) |
| Shared-write coherence through libc | posixtest `mmap-wshared` (`tools/posix-mmap-test.sh`, `MMAP_DATA=ext`) |
| `msync`, then extfs killed, then read back | `tools/pager-test.sh` `wmsync` |
| A crash mid-write-back, then `e2fsck` clean | `tools/pager-test.sh` `wcut` |
| Eviction and the limit under memory pressure, a hung pager | `tools/pager-test.sh` `wpress` |
| Chromium's `MemoryMappedFile` and persistent allocator tests | `tools/cr-test.sh` `base` (CB2's harness) |
| FAT unchanged | posixtest `mmap` on FAT; `tools/pager-test.sh` fat cases |

## 7. Questions for the owner

One at a time, in this order; each ends with a recommendation, which the
plan assumes until the owner says otherwise. Decided here as technical
(by the recommendations above unless the owner says otherwise): one VMO
per file keyed by the inode, `write` staying write-through and updating
the pages, dirty bits by write-protection, the dirty limit at a quarter
of the budget, no kernel timer, `vmo_write` refused, `MS_ASYNC` asking
for write-back.

### Q1. A write-back that fails

When a page can't reach the disk: the volume is full (a store over a hole
of a sparse file: the block is taken only at write-back), the disk fails,
or an error turned the volume read-only.

- **(A)** Linux's way: the pages become clean (still readable until
  evicted; then the disk's bytes come back), the error is kept and
  returned once by the next `msync` of the mapping and the next `fsync` of
  the file (`EIO`, or `ENOSPC` for a full volume); the writer goes on.
- **(B)** Keep them dirty and retry: nothing lost while the machine runs,
  but a full or read-only volume pins them: the mount's dirty limit fills,
  and every writer of every mapped file on that mount waits and then dies
  at the deadline.
- **(C)** (A), plus a request before the first store to each clean page
  (Linux's `page_mkwrite`), so extfs takes the block then and a full
  volume is a SIGBUS at the store, not a lost page: one round trip to
  extfs per first store after every write-back (the kernel can't tell
  holes from written blocks), or a "needs a block" mark extfs sets at
  supply, more protocol.

*Recommendation: (A).* Chromium's `MemoryMappedFile` allocates its region
before it maps it (it writes a byte per block, since `fallocate` is
`EOPNOTSUPP` here), so its stores never meet a hole; databases write with
`write`. (C) can come later as an addition if a program needs it. Whose:
the owner's (what a program's data does when the disk can't take it).

**Decided by the main session (2026-10-16, a technical choice): (A)**,
Linux's way: the pages become clean and the error is reported once by the
next `msync` or `fsync`; (C)'s early check can come later.

### Q2. How long a store may wait for the disk

A store without `msync` reaches the disk when its page is written back.
A crash of the program or of extfs loses nothing (the kernel keeps the
page); a power cut or a kernel panic loses what wasn't written yet.

- **(A)** 5 s: extfs writes back pages dirty for 5 s or more, checking
  once a second (ext4's journal commit interval).
- **(B)** 30 s: Linux's default for dirty pages; fewer writes of a page
  stored to again and again.
- **(C)** Only for `msync`, `fsync`, the dirty limit and an orderly stop.

*Recommendation: (A).* Jam OS promises an answered `write` is on the disk;
5 s keeps a mapped file close to that, and costs little: a page is
written at most once per 5 s (Chromium's histograms store to the same
pages all the time), on NVMe. Whose: the owner's (a durability promise).

**Answered by the owner (2026-10-16): 10 seconds to start with, and easy to
change.** The age after which extfs writes a dirty run back by itself is
a setting (one value, read at mount and on a settings change, with 10 s
as its default; WE2 names it and documents it in USING), not a constant
in code.
