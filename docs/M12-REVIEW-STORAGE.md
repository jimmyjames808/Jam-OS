# M12 review S: the storage split

Status (2026-10-07): review S of M12's stage A, docs only. Written against
main 56aa8904. It designs what [M12-PLAN.md](M12-PLAN.md) decided (Q6 (A):
a new service named **volumes**; Q7 (A): `storage` and `block` shaped for
GPT and NVMe; Q8 (A): the file protocol changed now; handles always moved
on send; events in the IDL; `u8[<=N]` and `str[<=N]`; one protocol per
kind of channel; the "receives no handles" mark; the five new error
codes). It decides nothing the owner has decided; what is still open is
under "Design questions".

Read for it: devmgr whole (`user/services/devmgr/`, 4,419 lines), fat's
startup, control and state (`user/services/fat/main.c`, `user/include/fatsvc.h`,
`user/include/svcstate.h`, `user/services/fat/fat.h`), init's mounts, spare,
control, services, reboot and ESP write (`user/services/init/`), the
shell's `mount`, `sync` and `storm`, `drivers/usb-storage/`, the storage
IDL files (`abi/idl/storage.idl`, `abi/idl/block.idl`, `abi/idl/fs.idl`,
`abi/idl/file.idl`, `abi/idl/fsctl.idl`), `user/include/devmgr.h`, the
storage tests (`tools/storage-test.sh`, `tools/sticks-test.sh`,
`tools/fat-restart-test.sh`, `tools/fat-spare-test.sh`,
`tools/fat-storm-test.sh`, `tools/data-test.sh`, `tools/bootdisk-test.sh`,
`tools/update-write-test.sh`, `tools/kexec-reboot-test.sh`, their shell
scripts, `user/tests/utest/disks.c`, `user/tests/usbtest/storage.c`), and
[ARCH-CHECK.md](history/ARCH-CHECK.md#1-devmgr-does-too-much),
[M11.6-PLAN.md](M11.6-PLAN.md) and [M11.6-REVIEW.md](history/M11.6-REVIEW.md).

## The design in ten lines

1. devmgr keeps the drivers, usb-storage included, and nothing of storage:
   no disks, no fat, no mounts, no ESP channel, no spare, no keeper.
2. volumes (new, `/svc/volumes` for tests only) owns disks, the boot-disk
   choice, fat's instances (spare, keepers, state VMOs, supervision), the
   mounts, remounts, the ESP write, test disks and the stick notices.
3. A disk reaches volumes as a `storage` channel in a one-way `disks.attach`
   event from devmgr, on a channel pair init makes fresh whenever either
   service starts; `disks.gone` says the device is gone for good.
4. Each `storage` channel volumes gets is its own (usb-storage answers
   `svc.connect` on its DR_SERVE channel), so no queue is shared and no
   dead instance's reply can block a hand-over.
5. init follows the mounts through a watch channel of events (`mounted`,
   `unmounted`, `settled`): no thread, no 2 s long-poll, no waiter table.
6. volumes' loop never waits inside a request: remount, the ESP write, a
   disk release, sync and stop are `later` methods run as cooperative tasks
   (`drivers/include/jam/task.h`, `user/lib/task.c`); disk steps are sends.
7. The supervision code is one libos module (spare, keeper and state VMO,
   first-answer probe, restart window): used by volumes and init (devmgr
   keeps only the restart window: after the split it has no spares).
8. `storage` gains the partition scheme, a 16-byte type GUID, a unique
   GUID and a name; `block` allows several requests in flight, each at its
   own buffer offset, with a buffer size the client asks for.
9. Start order: init, devmgr, volumes, then fat as volumes finds volumes;
   a volumes death remounts everything fresh (as a devmgr death does
   today); a devmgr death empties volumes quietly and the disks come back.
10. Tracks: S1a (the module), S1b (the move, wire unchanged), S2a (the
    protocols), S2b (the waits), D (devmgr in IDL), P5 (files).

## What there is today

devmgr serves three kinds of channel (`user/services/devmgr/chans.c`):
query, device and control, plus init's ESP channel. Its storage half:

| File | Lines | What |
|---|---|---|
| `user/services/devmgr/disk.c` | 547 | the disks and their steps (info, partitions, boot-disk check, other sticks), test disks, `disk_sync_all`, the mount list |
| `user/services/devmgr/disk.h` | 86 | `struct disk`, the constants (`INFO_WAIT`, `ESP_WAIT`, `CALL_WAIT`, `HELD_*`) |
| `user/services/devmgr/fsvc.c` | 447 | a filesystem service per partition: its handles, mount path, /usbN numbering, notices, REMOUNT, ESP_WRITE |
| `user/services/devmgr/mounts.c` | 135 | DEVMGR_MOUNTS: the generation and up to 8 waiting calls |
| `user/services/devmgr/spare.c` | 577 | the keeper and state VMO per mount, the warm spare fat, /data's `block` channel opened in advance, the first-answer probe |
| parts of `supervise.c`, `bind.c`, `request.c`, `chans.c`, `main.c`, `usb.c`, `internal.h` | about 250 | BIND_FS in the binding table and the loop |

About 2,040 of the 4,419 lines are storage. init holds devmgr's control
channel for its mounts watcher thread (`user/services/init/mounts.c`),
`kill fat-*` and `mount -w` (`user/services/init/ctl.c`), and the ESP
channel for `update -w` (`user/services/init/espwrite.c`, on update.c's
worker thread). Tests reach the same through `/svc/devmgr-ctl`
(`user/tests/utest/disks.c`, `user/tests/usbtest/storage.c`).

## What moves where

### devmgr, function by function

| From | What | Goes to |
|---|---|---|
| `user/services/devmgr/disk.c` | all: `disk_ch`, `disk_name`, `disk_alloc`, `next_msg`, `leave_alone`, `not_boot`, `ask_info`, `boot_disk_taken`, `inquiry_text` (to usb-storage, see `storage`), `ask_boot_file`, `look_at_esp`, `booted_from`, `hold_back`, `release_held`, `got_info`, `got_stat`, `disk_answers`, `fs_answers`, `disk_events`, `disk_attach`/`disk_detach` (become the `disks` events), `disk_started`/`disk_stopped` (become attach and the storage channel's peer closing), `disk_test`, `disk_key`, `held_over`, `disk_run_due`, `disk_next_deadline`, `disk_sync_all`, `disk_mounts` | volumes' disk.c. Every disk is a `storage` channel volumes holds and watches (real and test disks alike: one port key kind, today's KEY_DISK for test disks); `disk_ch` is always `d->ch`. The blocking `storage_partition_until` and `storage_disk_id_until` become sends (S2b) |
| `user/services/devmgr/disk.h` | `struct disk`, constants | volumes' header; `MAX_PARTS` grows with GPT only in M12.7 |
| `user/services/devmgr/fsvc.c` | all: `fs_mount_path`, `fs_ctl_close`, `fs_retire`, `fs_find`, `fs_find_mount`, `fs_channel_kept`, `fs_serve_end`, `fs_handles`, `fs_format_arg`, `fs_end_arg`, `usb_number`, `fs_binding`, `fs_start`, the notices (`disk_told_added`, `disk_told_boot`, `told_removed`), `drop_services`, `fat_type`, `others_pump`, `mount_others`, `ask_stat`, `fs_check_ended`, `fs_restart`, `disk_remount`, `disk_esp_write` | volumes' fsvc.c; a filesystem service is a `struct vol` of volumes' own (the BIND_FS fields of `struct binding`), not a devmgr binding |
| `user/services/devmgr/mounts.c` | the generation, `mounts_update`, the waiters | volumes' mounts.c: the same generation rule; S1b keeps the waiters, S2a replaces them by watch channels |
| `user/services/devmgr/spare.c` | the generic half: `spare_drop`, `spare_job`, `spare_start`, `spare_ended`, `spare_event`, `spare_due`, `spare_next_deadline`, `spare_stop`, `keep_unbind`, the state and keep handles, `hand_over`, `spare_keep_event`, the release, `probe_main`, `probe_start`, `restarted` | the libos supervision module (below) |
| `user/services/devmgr/spare.c` | fat's half: `data_binding`, `prepare_block`, `fs_block_prepared`, `drop_block`, `promote`'s handle making, `adopt`, `fs_run` | volumes' spare.c, on the module |
| `user/services/devmgr/supervise.c` | BIND_FS: `excused`'s other-stick rule, `plain_name`'s "File system", `backoff`'s rule for a service that outlives its process, `schedule`'s BIND_FS branch, `sup_died`'s `fs_check_ended` and `FAT_EXIT_NO_VOLUME`, `sup_run_due`'s `fs_run` and `fs_retire`, `disk_stopped` before a pulled stick's job check | volumes' supervise.c on the module's restart window. devmgr's `recent`, `backoff`, `schedule` move onto the same window code |
| `user/services/devmgr/bind.c` | `add_serve`'s BIND_FS branch, `spawn_driver`'s fat arguments, `start_driver`'s `fs_handles`, `forget_driver`'s `disk_stopped`; `watch_events` on a disk driver's channel | fat's start is volumes' own (spawn in a child job with a driver's limits, no PCI handles). devmgr no longer watches or reads a disk driver's channel at all |
| `user/services/devmgr/bind.c` | `job_empty`, `job_run_due`, `job_settle` (the dead job's recheck) | stays in devmgr; volumes needs it only for "fat ended cleanly", so it may move to the module too (S1a's choice, no behaviour change) |
| `user/services/devmgr/request.c` | `find`'s DEVMGR_FS_SVC and DEVMGR_FS_MOUNT, REMOUNT, ESP_WRITE, `release`'s `disk_sync_all`, `kill_request`'s `kill_at`, `supervision`'s BIND_FS fields | volumes (kill, supervision by mount path); RELEASE no longer syncs anything (`volumes.release_disk` comes first) |
| `user/services/devmgr/chans.c` | MOUNTS dispatch, TEST_DISK's handle, `LEVEL_ESP` and slot 2 | volumes' channels; devmgr takes one new handle-carrying request instead, SET_VOLUMES |
| `user/services/devmgr/usb.c` | `disk_attach` at bind, `disk_detach` in `usb_retire` | a small new devmgr file (disks.c): connect to a running usb-storage, `disks.attach`, `disks.gone`, re-announce every disk when a new volumes channel comes, drop the channel when its peer closes |
| `user/services/devmgr/main.c` | `nospare`, `bootdisk=`, the disk and mount deadlines, KEY_DISK, KEY_SPARE, KEY_KEEP, `stop_all`'s `spare_stop`, `disk_sync_all` and BIND_FS order | volumes' main.c (`nospare`, `bootdisk=` are volumes' words now) |
| `user/services/devmgr/internal.h` | BIND_FS, the binding's storage fields (`disk`, `part`, `ctl`, `other`, `rw`, `ready`, `usbn`, `probe`, `kept`, `deliberate`, `kill_at`, `ended_at`, `promoted`, `chan_gen`), `boot_mbr_id`, the disk/fs/mounts/spare prototypes | volumes; devmgr keeps `disk` only as "this binding runs a disk's driver" (GET_SERVICE refuses it) |
| `user/include/devmgr.h` | MOUNTS, TEST_DISK, REMOUNT, ESP_WRITE, DEVMGR_SR_ESP, DEVMGR_FS_SVC, DEVMGR_FS_MOUNT, DEVMGR_MOUNT_*, DEVMGR_USB_MOUNT, DEVMGR_REMOUNT_*, DEVMGR_PART_*, `devmgr_mounts`, `struct devmgr_mounts_rep`, the filesystem paragraph | S1b: a hand-written volumes header with the same wire; S2a: abi/idl/volumes.idl (new) |

After the move devmgr is about 2,400 lines; volumes about 1,900 (its
own loop and channels about 350 over the moved code); the module about
450, and init's and fat's halves of the old spare.c shrink to about 120
and 250.

### init

| From | What | Becomes |
|---|---|---|
| `user/services/init/services.c` `start_devmgr` | the ESP pair, `nospare`, `bootdisk=` | start_volumes (new): the control, ESP and disks pairs, the notify channel, `nospare`, `bootdisk=`; devmgr gets `set_volumes` once both run |
| `user/services/init/services.c` `services_closed` | DEVMGR: `mounts_unwatch` | DEVMGR: a fresh disks pair to the new devmgr and to volumes (`volumes.set_disks`). VOLUMES (new): its channels closed, `/svc/volumes` unpublished, every volume mount unmounted, followers told |
| `user/services/init/services.c` `shell_stop_devmgr` | DEVMGR_SHUTDOWN | `volumes.stop` (later: every fat stopped in order) and then DEVMGR_SHUTDOWN |
| `user/services/init/mounts.c` | the watcher thread and its 2 s long-poll | S1b: unchanged, pointed at volumes. S2a: a watch channel bound on init's port (`mounts` events), no thread; `mounts_sync` becomes `volumes.sync` where init serves a request |
| `user/services/init/ctl.c` | `kill_fs_mount` (DEVMGR_FS_MOUNT KILL), `op_mount` (REMOUNT, 25 s), `op_sync` (2 s per mount) | `volumes.kill` by mount, `volumes.remount` and `volumes.sync`, each answered later (S2b): init's `initctl.mount`, `initctl.sync` and `kill fat-*` stop waiting inside a request |
| `user/services/init/espwrite.c` | DEVMGR_ESP_WRITE on the ESP channel | `volesp.open_writable` and `close_writable` on volumes' ESP channel; still on update.c's worker thread |
| `user/services/init/reboot.c` `stop_everything` | sync, flush, devmgr's shutdown | sync, flush, volumes' stop, devmgr's shutdown; its lines say both times |
| `user/services/init/spare.c` | the mixer's spare, keeper, state, probe | the module; the mixer's glue stays (about 120 lines) |
| `user/services/init/shell.c`, `user/services/init/init.h` | the service table | VOLUMES after DEVMGR; given up like devmgr (`count_end`); its plain name "Storage" for the desktop's notices |
| `user/services/init/main.c` (test mode) | its own copy of devmgr's start and stop | the same start functions as shell mode (one copy) |

### Everything else

| Where | Change |
|---|---|
| `user/services/fat/` | nothing in S1 (fat takes its handles by role and doesn't know who started it); its comments name volumes. S2a: its block client asks `map_buffer(0)`. P5: the file protocol |
| `drivers/usb-storage/main.c` | S1b: answers `svc.connect` on DR_SERVE with a new `storage` channel. S2a: the reshaped `storage` and `block` (scheme, GUIDs, name, trimmed vendor and product, `map_buffer(size)`, `queue_depth` 1) |
| `user/include/os.h` | SVC_VOLUMES "volumes"; `NS_MAX_SVCS` 16 to 24 (finding 8) |
| `user/services/shell/sh_allow.c` | "volumes" in `refused[]` |
| `tools/checkwants.py` | "volumes" in TESTS_ONLY |
| `user/lib/start.c` | names for volumes' startup roles |
| the Makefile | bin/volumes in the boot image |
| `abi/idl/block.idl`, `abi/idl/fs.idl`, `abi/idl/file.idl`, `abi/idl/fsctl.idl`, `abi/idl/storage.idl` | their comments say volumes where they say devmgr (S1b); their shapes change in S2a and P5 |
| `user/include/logwriters.h` | nothing: the console turns only devmgr's driver lines into notices; volumes posts its own notices |

## The new service: volumes

**What it holds.** Per disk: its `storage` channel (its own, from
`svc.connect`), its state (`enum disk_state` as today), its partitions.
Per mount: both ends of the kept `fs` channel, the keeper and state VMO
(the module), the `fsctl` channel, the instance's process and job, the
restart window. One warm spare fat. The watch channels. The pending
`later` replies (struct idl_txn records) and the tasks that answer them.

**Its startup handles** (roles SR_USER + n, defined in volumes' own
header, not in `kernel/include/jam/startup.h`, which K3 empties of
service roles): the control channel's server end, the ESP channel's
server end, the disks channel's server end, a duplicate of `/svc/notify`'s
channel, SR_JOB (its fats' jobs are children of its job). No root
resource, no PCI resource, no namespace: it reaches nothing but what it is
handed. Its words: `bootdisk=0x<id>`, `nospare`.

**Its channels**, one protocol per kind (Q5):

| Channel | Protocol | Holders | Marked "receives no handles" |
|---|---|---|---|
| control (shared; `svc.connect` gives one per opener) | `volumes` | init; `/svc/volumes` for tests | no: `test_disk` and `set_disks` carry one |
| ESP | `volesp` | init alone, never published | yes |
| disks | `disks` (events, devmgr to volumes) | devmgr's end; volumes' end | no: `attach` carries one; a fresh pair, never kept |
| a watch | `mounts` (events, volumes to the watcher) | the watcher (init; tests) | volumes' end: yes (it receives nothing) |
| each mount's `fs` (kept) | `fs` | volumes (both ends), namespaces | yes, set by volumes when it makes it |
| each instance's `fsctl` | `fsctl` | volumes | yes |

**Its loop never waits inside a request** (ARCHITECTURE "How a service
waits"). Every call to another process is a send (`<proto>_<method>_send`)
whose answer comes from the port; an operation of several steps (a
remount: `fsctl.stop`, the instance's end, `storage.open_partition`, the
start, the first answer) is a cooperative task that waits at each step
with a deadline, and its request is a `later` method answered at the end.
What stays synchronous is bounded and local: `job_kill` (the kernel
returns once the job is dead), `spawn` from the boot image, a duplicate.
The first-answer probe keeps its own thread (it serves nothing).

## The protocols, drafted

The syntax follows `tools/genidl.py` with what G adds: `str[<=N]`,
`u8[<=N]`, `event` methods (one-way, may carry handles: Q1, Q4), handle
arguments, several protocols in one file. Protocol ids are placeholders
from 33 up (compctl is 32); G's registry assigns them (item 17's shared
id 3 included).

### volumes (abi/idl/volumes.idl, new)

```
# volumes: the disks, their filesystem services and the mounts
# (user/services/volumes). init starts it and holds its control channel;
# tests reach it at /svc/volumes (a channel per opener, svc.connect).
# Mounts are named by their path: "/data", "/esp", "/usb0", and a test
# disk's "/data-test", "/esp-test", "/usb0-test".
#
# Restart (init's supervision): when volumes ends, every channel it made
# closes: each mount's `fs` channel and everything opened through it, the
# watch channels, the ESP's writable channel. What its filesystem services
# held and had not written is lost (as when devmgr ended before M12).
# Open /svc/volumes again; init mounts what the next volumes lists.

protocol volumes 33

# A new watch channel: volumes sends `mounts` events on it (below): every
# mount there is now, then `settled`, then each change. A watcher that
# leaves 256 events unread is dropped (its channel closed); it watches
# again and gets the whole list.
1  watch  () -> (handle events)
# The mount `mount` (another stick's, or a test disk's /usbN-test)
# writable (1) or read-only (0): its service stops in order and starts on a
# `block` channel opened the new way: the mount's channel is a new one
# (`unmounted`, then `mounted`), files open on the old one fail
# ERR_PEER_CLOSED. Answered once the new instance runs. OK at once if it
# is that way already. ERR_NOT_FOUND: no such mount, or /data, /esp, /boot
# (they are what they are); ERR_BAD_STATE: its service isn't serving.
2  remount  (str[<=15] mount, u8 writable) -> () later
# Kill the mount's filesystem service (a deliberate kill: not counted,
# restarted at once from the warm spare; its clients see nothing).
# Answered once it is dead, with the koid of the instance killed.
# ERR_NOT_FOUND: no such mount, or no instance runs (its restart is due).
3  kill  (str[<=15] mount) -> (u64 koid)
# The mount's service: state (SUP_*, the module's), restarts since boot,
# the last backoff, restarts that promoted the warm spare, and 1 while a
# spare waits.
4  supervision  (str[<=15] mount) -> (u32 state, u32 restarts, u32 backoff_ms, u32 promoted, u8 spare_waits)
# A software disk for tests: `storage` speaks storage (below). Its mounts
# are the /...-test ones and never /data or /esp. Gone once the
# channel's server end closes. ERR_NO_RESOURCES: too many disks.
5  test_disk  (handle storage) -> (u32 id)
# Disk `id` let go: each of its mounts stops in order (files closed, the
# volume clean) and goes; the disk is not mounted again until it comes
# back (a new `attach`). For a test that drives the disk itself (usbtest
# asks this, then devmgr's `release`). Answered once all have stopped.
6  release_disk  (u32 id) -> () later
# Every writable mount on its medium (fs.sync each, at once). Answered
# when all have answered or after 5 s, with the first failure.
7  sync  () -> () later
# Stop, for a reboot: every mount's service stops in order, then this
# answers and volumes exits 0.
8  stop  () -> () later
# The server end of a new disks channel (devmgr was started again).
# The disks of the old one are forgotten, without notices.
9  set_disks  (handle disks) -> ()
```

The shared control channel answers `svc.connect` as `/svc/devmgr` does,
so a `later` reply never lands on a channel other callers share (finding
5). There is no query level: nothing but init and tests needs volumes.

### volesp: the ESP channel

```
# volesp: the boot disk's ESP made writable, for init's stick write
# (update -w) alone: init made this channel when it started volumes and
# gives it to nobody. While writable, /esp is no mount (it leaves every
# watch: `unmounted`), so no namespace holds the writable channel.
#
# Restart: ERR_PEER_CLOSED: volumes ended; the ESP is read-only again with
# the next volumes, and init has a new ESP channel.

protocol volesp 34

# The ESP's service stops in order and starts on a `block` channel opened
# read-write; `fs` is a channel to it, the caller's alone. A second call
# while it is writable: another handle to the same channel. A crash
# meanwhile restarts it writable; a pulled stick ends it.
# ERR_NOT_FOUND: no boot disk with an ESP service; ERR_BAD_STATE: it isn't
# serving.
1  open_writable  () -> (handle fs) later
# Back to read-only: it stops in order (everything on the stick, the
# volume clean) and /esp is a mount again (a new generation). OK at once
# if it is read-only already.
2  close_writable  () -> () later
```

### disks: devmgr to volumes

```
# disks: the disks devmgr's drivers serve, told to volumes. init makes a
# new pair whenever devmgr or volumes starts and hands one end to each
# (devmgr's `set_volumes`, volumes' startup role or `set_disks`).
#
# Reconnect: when the channel's peer closes, the other side drops it and
# waits for the next: devmgr announces every disk again on a new channel;
# volumes forgets every disk the old one announced, without notices.

protocol disks 35

# A disk is there: its driver runs. `storage` is a channel of volumes'
# own (from svc.connect on the driver's channel). `id` names the disk
# while the device is there (USB: usb-bus's device id; a later bus picks
# ids in its own range, never with the top bit, which test disks use);
# `bus` is 1 for USB (2 for NVMe at M12.7). A driver started again after a
# crash is announced again with the same id. ERR_PEER_CLOSED on `storage`
# alone means the driver ended, not that the device went.
1  attach  (u32 id, u8 bus, handle storage) event
# The device is gone (unplugged, or its bus driver ended): the disk will
# not come back under this id.
2  gone  (u32 id) event
```

### mounts: a watch

```
# mounts: what volumes sends on a watch channel (volumes.watch). Every
# event of one change carries the same generation; `settled` ends it.
#
# Restart: ERR_PEER_CLOSED: volumes ended (every mount went with it), or
# this watcher fell 256 events behind; watch again.

protocol mounts 36

# A mount: its path, its channel's generation (a remount, or a service
# started after it was given up on, is a new one; a restart on the
# channel volumes kept is not), flags 1 read-only, 2 a test disk's.
1  mounted  (str[<=15] path, u32 generation, u8 flags, handle fs) event
2  unmounted  (str[<=15] path, u32 generation) event
# The list is whole as of `generation`: tell the namespaces now.
3  settled  (u32 generation) event
```

init keeps a path-to-channel table from these events (mounts.c's `have`),
updates its namespace at each event and tells its followers at
`settled`, as `apply()` and `tell_mounts()` do today. Test mounts are
skipped by flag 2 instead of by the "-test" suffix.

### devmgr (abi/idl/devmgr.idl, new; track D's)

What is left after the split, with a real device address instead of the
magic vendors (item 18). Without struct types in the IDL (Q3) an address
is four arguments:

```
kind 0 SELF        the device channel's own device (a, b, n: 0)
kind 1 PCI_IDS     a vendor, b device, n the n-th function with those ids
kind 2 PCI_CLASS   a class << 16 | subclass << 8 | prog_if, n the n-th
kind 3 PCI_BOUND   n: the n-th PCI function with a driver bound
kind 4 PCI_MSIX    n: the n-th function with MSI-X that isn't a bridge or the boot display
kind 5 USB_IFACE   a usb-bus's device id, b the interface number
kind 6 TEST        the crash-test device
```

```
# devmgr: how a program finds a driver devmgr bound (query channels: one
# per opener of /svc/devmgr; device channels, each scoped to one device:
# the same methods about that device alone, anything else
# ERR_ACCESS_DENIED).
#
# Reconnect (devmgr's supervision): a driver's channel that fails
# ERR_PEER_CLOSED: ask get_service again (the restart's channel, at once);
# ERR_BAD_STATE: given up on. devmgr itself ended: open /svc/devmgr again.

protocol devmgr 37

# Answered once the first binding pass is done.
1  status  () -> (u32 bound, u32 failed, u32 skipped) later
# A channel to the driver. Never a disk's driver's (ERR_ACCESS_DENIED).
2  get_service  (u8 kind, u32 a, u32 b, u32 n) -> (handle service)
# Read-only views of the running driver's process and job.
3  get_driver  (u8 kind, u32 a, u32 b, u32 n) -> (handle process, handle job, u32 pci_index)
# A PCI function's config, read-only (get_driver's third handle today).
4  get_function  (u8 kind, u32 a, u32 b, u32 n) -> (handle function)
5  supervision  (u8 kind, u32 a, u32 b, u32 n) -> (u32 state, u32 restarts, u32 backoff_ms, u32 dma_quarantined, u32 dma_changed)

# devmgrctl: the control channel (init's; tests'). Query methods go to a
# query channel, which every holder of this one also has.
#
# Reconnect: as devmgr's.

protocol devmgrctl 38

1  kill  (u8 kind, u32 a, u32 b, u32 n) -> (u64 koid)
2  rebind  (u8 kind, u32 a, u32 b, u32 n) -> ()
# Stop the driver and leave the device without one until rebind. A USB
# interface's: `usb` is the interface channel its driver had.
3  release  (u8 kind, u32 a, u32 b, u32 n) -> ()
4  release_usb  (u32 dev, u32 iface) -> (handle usb)
# A driver's rights, for tests of what drivers can't do: the function,
# and the memory BAR mask; each BAR by driver_bar.
5  driver_view  (u8 kind, u32 a, u32 b, u32 n) -> (handle function, u32 bar_mask)
6  driver_bar  (u8 kind, u32 a, u32 b, u32 n, u8 bar) -> (handle bar)
7  test_driver  () -> ()
# What HID drivers send input to (a compctl INPUT channel).
8  set_input  (handle input) -> ()
# The client end of a new disks channel (volumes was started again).
9  set_volumes  (handle disks) -> ()
10 device_channel  (u8 kind, u32 a, u32 b, u32 n) -> (handle device)
11 shutdown  () -> ()
```

Gone from devmgr: MOUNTS, TEST_DISK, REMOUNT, ESP_WRITE, the ESP level
and DEVMGR_SR_ESP, DEVMGR_FS_SVC, DEVMGR_FS_MOUNT, `nospare`,
`bootdisk=`. Every varying handle count is gone too: DRIVER_VIEW's BARs
become one call each, GET_DRIVER's function a call of its own, RELEASE's
USB channel a method of its own, so genidl needs no handle arrays for
devmgr or volumes.

### storage and block, reshaped (Q7)

```
protocol storage 14

# The disk: INQUIRY's vendor and product (trimmed ASCII; anything else
# '?'), block size and count, the partition scheme (0 none: one partition
# that is the whole disk, a "superfloppy"; 1 MBR; 2 GPT), how many
# partitions it lists, the MBR disk id (0: none) and the GPT disk GUID
# (zeros: none). The largest transfer and the requests in flight a `block`
# channel takes (its own `info` says the same).
1  info  () -> (str[<=8] vendor, str[<=16] product, u32 block_size, u64 blocks, u8 scheme, u32 partitions, u32 mbr_id, u8[16] disk_guid) idempotent
# Partition `index` (0-based, in table order): its MBR type byte (0 on
# GPT), its GPT type GUID and unique GUID (zeros on MBR), first block,
# length, GPT attribute bits, and its GPT name (UTF-8; empty on MBR).
# Past the last: ERR_OUT_OF_RANGE.
2  partition  (u32 index) -> (u8 mbr_type, u8[16] type_guid, u8[16] unique_guid, u64 start, u64 blocks, u64 attributes, str[<=108] name) idempotent
# A `block` channel limited to partition `index`; read_only 1 refuses
# every write.
3  open_partition  (u32 index, u8 read_only) -> (handle block)
```

`disk_id` goes into `info` (one call fewer, and the 2 s blocking call in
devmgr goes). A GPT name is 36 UTF-16 units: at most 108 bytes of UTF-8.
In M12 usb-storage parses MBR only: a GPT disk shows its protective MBR
(scheme 1, one partition of type 0xEE), which volumes leaves alone as
today; M12.7 fills the GPT fields. The boot disk's identity stays the
32-bit MBR id the loader passes (`bootdisk=`); a GPT boot disk's GUID is
M12.7's (with the kernel's hand-over).

```
protocol block 15

# Block size, blocks in the partition, 1 if writes are refused; the most
# blocks one request moves; requests the server takes in flight at once
# (1 for usb-storage: more are queued, not refused); the largest buffer
# it makes.
1  info  () -> (u32 block_size, u64 blocks, u8 read_only, u32 max_blocks, u16 queue_depth, u32 buffer_max) idempotent
# The transfer buffer: `size` bytes asked for (0: the server's choice,
# 64 KiB for usb-storage; more than buffer_max: buffer_max), rounded to
# pages. Once per channel (a second call: ERR_BAD_STATE).
2  map_buffer  (u32 size) -> (handle buffer, u32 size)
3  read  (u64 lba, u32 count, u32 offset) -> () idempotent
4  write  (u64 lba, u32 count, u32 offset) -> () idempotent
# Every write answered before this was sent is on the medium.
5  sync  () -> () idempotent
```

The rules that make several in flight safe, for the header comment:
requests may be answered in any order, each by its txid; a client that
needs an order waits for an answer before it sends what depends on it
(fat sends one at a time and keeps doing so); two requests in flight must
not use overlapping buffer ranges, nor write overlapping blocks (the data
is then undefined, never anything outside the channel's partition and
buffer); `sync` covers the writes answered before it was sent. The fence
stays: a request from a client that has gone is never started, and with
several in flight that means "not yet started". `idempotent` keeps its
meaning for the drivers' restart (M11.6's note X).

`fsctl` needs no change: `stop` and `stats` stay; its restart paragraph
names volumes.

### fs and file (Q8, Q9; track P5's)

```
protocol fs 16

# flags: FS_READ 1, FS_WRITE 2, FS_CREATE 4, FS_TRUNCATE 8, FS_APPEND 16,
# FS_GATHER 32, FS_EXCL 64 (with FS_CREATE: ERR_ALREADY_EXISTS if it is
# there). ERR_IS_DIR: a directory; ERR_NOT_DIR: a component on the way is
# a file; ERR_NAME_TOO_LONG: a component over 255 bytes.
1  open  (str[<=4095] path, u32 flags) -> (handle file, handle buffer, u64 size)
2  stat  (str[<=4095] path) -> (u64 size, u8 is_dir, u64 mtime)
# Entries of directory `path` from `cookie` (0: the first), "." and ".."
# left out, as many as fit: records of u64 size, u64 mtime, u8 is_dir,
# u8 name_len, then the name (UTF-8, no NUL). `next` is the cookie to go
# on with; 0: that was the last. fat's cookie is the entry's index, kept
# with its cursor (one entry read per entry when listed in order).
3  readdir  (str[<=4095] path, u32 cookie) -> (u32 next, u8 count, u8[<=8000] entries)
4  mkdir  (str[<=4095] path) -> ()
# ERR_NOT_EMPTY: a directory with entries; ERR_BUSY: an open file.
5  unlink  (str[<=4095] path) -> ()
# ERR_BUSY: renaming an open file; ERR_ALREADY_EXISTS: `to` is there.
6  rename  (str[<=4095] from, str[<=4095] to) -> ()
7  sync  () -> ()
8  statfs  () -> (u64 total, u64 free, u8 read_only, str[<=11] label)
9  view  (u32 flags) -> (handle fs)

protocol file 17
...
4  stat  () -> (u64 size, u8 is_dir, u64 mtime)
```

`file.stat`'s `is_dir` is 0 until directory handles (M13, an addition).
fat's lock and sharing by directory entry: `struct fat_open` keyed by the
entry's sector and offset in it (FatFs's `dir_sect` and the entry's place
in the window), not by `path` (`user/services/fat/fat.h:163`). A file's
entry doesn't move when a directory above it is renamed, so the M11.6
review's finding 5 is gone whichever name or 8.3 alias reached it;
`file.stat`'s time is read from that entry, not `f_stat` of a stored
path. Renaming or removing an open file stays refused, now ERR_BUSY.
fat's state layout version goes up (its slots replay `fs` and `file`
requests), and the request area must grow: see design question 4.

## The shared supervision module

One libos module, user/lib/supervise.c and its header (new), from the
two `spare.c` files and devmgr's restart window. After the split devmgr
has no spare and no keeper: the module's users are **volumes** (fat:
everything), **init** (the mixer: spare, keeper, state, probe) and
**devmgr** (drivers: the restart window only).

```c
/* The restart window (devmgr's rule, and the rule of a service that
 * outlives its process): restarts in the last SUP_WINDOW, backoff from
 * SUP_BACKOFF_FIRST doubling to SUP_BACKOFF_MAX, the death after
 * SUP_RESTART_LIMIT restarts in the window gives up. */
struct sup_window { uint64_t restarted[SUP_RESTART_LIMIT]; uint32_t restarts, backoff_ms; };
enum sup_end { SUP_END_KILLED_ON_PURPOSE, SUP_END_CRASHED, SUP_END_EXITED };
/* After an end at t: true and *at (when to start again), or false: give
 * up. `outlives`: a deliberate kill isn't counted and the first crash in
 * the window restarts at once (M11.6's Q5). */
bool sup_window_end(struct sup_window *, uint64_t t, enum sup_end, bool outlives, uint64_t *at);
void sup_window_started(struct sup_window *, uint64_t t, bool counted);
void sup_window_reset(struct sup_window *);

/* The warm spare: one process started with SR_STANDBY alone. */
struct sup_spare_cfg {
    const char *path, *name;     /* bin/fat, "fat-spare" */
    const char *log;             /* "volumes", "init": the log lines' prefix */
    const struct sup_limit *limits; unsigned nlimits;   /* its job's (none: init's) */
    handle_t port; uint64_t key; /* its SIG_TERMINATED's packet */
    bool on;                     /* no boot word `nospare` */
};
void     sup_spare_init(struct sup_spare *, const struct sup_spare_cfg *);
void     sup_spare_arm(struct sup_spare *, uint64_t first_at);   /* spares wanted from then */
uint64_t sup_spare_due(struct sup_spare *, uint64_t t);          /* start one if due; next deadline */
void     sup_spare_event(struct sup_spare *);                    /* its key's packet */
bool     sup_spare_waits(const struct sup_spare *);
/* Promote it with a's handles (consumed unless ERR_NOT_FOUND: none waits,
 * or it died). OK: *proc and *job are the caller's. */
status_t sup_spare_promote(struct sup_spare *, const struct standby_args *a, handle_t *proc, handle_t *job);
void     sup_spare_stop(struct sup_spare *);

/* What outlives an instance: its keeper and its state VMO. */
void     sup_kept_init(struct sup_kept *, uint64_t state_size, const char *log, const char *name);
unsigned sup_kept_handles(struct sup_kept *, struct spawn_handle *x, rights_t *r, unsigned n);  /* + SR_STATE, SR_KEEP */
void     sup_kept_hand_over(struct sup_kept *, handle_t port, uint64_t key);  /* after the start */
void     sup_kept_event(struct sup_kept *);                    /* drain what it put */
void     sup_kept_release(struct sup_kept *, handle_t port);   /* given up, retired, stopped in order */

/* A restart measured: the "<log>: <name>: spare promoted N us after the
 * kill" line, then one cheap call on a thread of its own (one at a time):
 * "kill to first answer". ch is consumed. */
typedef status_t (*sup_probe_call)(handle_t ch, uint64_t deadline);
void sup_restarted(const char *log, const char *name, bool promoted, uint64_t kill_at,
                   uint64_t ended_at, uint64_t up, handle_t ch, sup_probe_call call);
```

The log lines keep their words with the new prefix ("volumes: fat-data:
spare promoted ...", "init: bin/mixer: ..."): the scripts grep them.
init's own restart counting (`count_end`, the services that are never
given up) stays init's: `tools/shell-tests/shell-forever.txt` relies on
it, and nothing in M12 asks to change it.

## Start order, hand-overs, deaths

### At boot

1. init starts devmgr as today (its control and query channels, input,
   notify), without the ESP channel, `nospare` or `bootdisk=`; it waits for
   `status` (D's to make that a send).
2. init starts volumes right after: control, ESP and disks pairs, a
   notify duplicate, `bootdisk=`, `nospare`. It publishes `/svc/volumes`
   (a channel per opener), opens a watch on its port, and sends devmgr
   `set_volumes` with the disks channel's client end.
3. devmgr, for each running usb-storage (and each one that starts
   later): `svc.connect` on its DR_SERVE channel (a send), then
   `disks.attach(id, 1, storage)` with the new channel.
4. volumes: `storage.info`, the partitions, the boot-disk check (the ESP's
   fat asked for `/boot/jamos.elf`), then fat on each partition, each with
   a new `block` channel; `mounted` events as each answers its first stat;
   `settled`.
5. init mounts each in its namespace and tells its followers; `/data`
   lets logd, netlog and sntp start, as today.

devmgr and volumes may start in either order: disks wait in devmgr until
`set_volumes`, and nothing is queued on a disks channel before both ends
have their owners, so the kernel's rule against sending a channel end
whose queue holds channel ends never meets a disks channel.

### Who hands what to whom

| From | To | What | When |
|---|---|---|---|
| init | devmgr | PCI resource, control and query server ends, input channel, notify | devmgr's start |
| init | volumes | control, ESP, disks server ends, notify, SR_JOB | volumes' start |
| init | devmgr | disks client end (`set_volumes`) | once both run; after either restarts |
| init | volumes | disks server end (`set_disks`) | after devmgr restarts |
| usb-storage | devmgr | a new `storage` channel (`svc.connect` reply) | per disk, per volumes instance |
| devmgr | volumes | that channel (`disks.attach`) | at once |
| volumes | fat | `block` (from `storage.open_partition`), a duplicate of the kept `fs` server end, `fsctl`, SR_STATE, SR_KEEP; or all of it in a promotion | each start or promotion |
| fat | volumes | each open file's and view's handles (`KEEP_PUT`) | as fat makes them |
| volumes | watchers | each mount's `fs` client end (`mounted`) | each change |
| volumes | init | the ESP's writable `fs` (`open_writable`) | `update -w` |

### When each one dies

| Who | Seen by | What survives | What is lost | Back |
|---|---|---|---|---|
| fat | volumes (its process) | everything: the kept `fs` channel, files and views (the keeper), the state VMO; calls wait | nothing (M11.6) | at once from the spare |
| usb-storage | devmgr (process), volumes (`storage` peer closed) | the device; its id | the disk's mounts: fats end (their `block` dead), `unmounted`; notices as today (finding 30) | devmgr restarts it, `attach` again (same id), mounts come back with new channels |
| volumes | init (process) | the drivers, every disk, the sticks themselves | every mount (fats are in its job), files open through them, what the fats held unwritten (up to the hold's ~1 MiB per mount) | init restarts it with backoff (given up like devmgr after `GIVE_UP_COUNT` in a minute: no `/data` until a reboot); new pairs; devmgr connects again; mounts back fresh |
| devmgr | init (process), volumes (disks peer closed, every `storage` peer closed) | volumes, its watchers, `/svc/volumes` | every driver and with them every mount (as today) | init restarts devmgr, a new disks pair; volumes forgets the old disks quietly; mounts come back |
| init | the kernel | nothing | everything | (as today) |

The state that must survive is all fat's, kept by volumes as devmgr keeps
it today. Nobody keeps devmgr's or volumes' own state: both are rebuilt
from the hardware and the disks in under a second (design question 3).

**The pulled stick.** usb-storage ends ("the device is gone") and usb-bus
reports the interface gone, so volumes sees two things in either order:
the `storage` channel's peer closing (the disk is down: kill its fats,
`unmounted`) and `disks.gone` (the disk is forgotten: "Jam OS stick
removed" or "USB stick removed"). `gone` first must work too: the
peer-closed packet is then stale. One race moves: devmgr's check that a
dead usb-storage's job is empty used to run after it had killed the fats
that map usb-storage's buffer (`supervise.c:179`); now volumes kills them,
so devmgr's first look may find the buffer still charged, and its recheck
(two seconds, `bind.c`'s JOB_RECHECK) must see them gone. volumes reacts
in milliseconds, but a test must watch for "did not end cleanly" (Tests).

### Test mode

init's test boots (init.cfg: utest, then usbtest) start devmgr and
volumes with the same code as shell mode (today `user/services/init/main.c`
has its own copy of devmgr's start), with no ESP channel. Tests open
`/svc/volumes` (TESTS_ONLY in `tools/checkwants.py`, refused to programs
from /data by `user/services/shell/sh_allow.c`).

## Waits inside requests: what goes and how

| Today | Wait | After |
|---|---|---|
| devmgr `got_info`: `storage_partition_until` per partition (`disk.c:269`) | 2 s each | volumes sends `partition(i)` one at a time, the next on each answer (GPT: up to 128) |
| devmgr `booted_from`: `storage_disk_id_until` (`disk.c:231`) | 2 s | gone: the id is in `info` |
| devmgr `fs_handles`: `storage_open_partition_until` (`fsvc.c:124`), each fat start | 2 s | a send; the start is a task step |
| devmgr `prepare_block` (`spare.c:173`), from `sup_run_due` after every request | 2 s, at most once a second | a send, its answer kept for the spare |
| devmgr `fs_restart`: `fsctl_stop_until` (`fsvc.c:394`) then `stop_driver` | 5 s + 15 s | the remount or ESP task: send `fsctl.stop`, wait for the answer and SIG_TERMINATED on the port |
| devmgr `release`: `disk_sync_all` (`request.c:111`) | 5 s per writable mount, every disk | gone from devmgr; `volumes.release_disk` (later) |
| devmgr `stop_all`: `disk_sync_all` | 5 s per mount | `volumes.stop` (later), before devmgr's shutdown |
| devmgr `kill_request` | `job_kill` then a wait that ends at once | volumes' kill: `job_kill` only (bounded by the kernel) |
| init `op_mount` (`ctl.c:59`, MOUNT_WAIT) | 25 s | `initctl.mount` later: init sends `volumes.remount`, answers on its reply |
| init `op_kill` for a mount | 15 s (KILL_WAIT) | `volumes.kill` answers at once after `job_kill`; init's kill goes `later` with D |
| init `op_sync` / `mounts_sync` | 2 s per mount | `initctl.sync` later, `volumes.sync`; the reboot path keeps a blocking sync (it is the end) |
| init `mounts_unwatch` when devmgr dies (`mounts.c:143`) | up to 4 s | gone: no watcher thread |
| init `services_devmgr_input` (SET_CONSOLE) | 5 s | D's (devmgr answers at once) |
| init `start_devmgr`'s `status` (`services.c:348`) | 30 s | D's (`status` is `later`; init sends it) |

## Tests

**Moved or changed, same checks:**

| Test | Change |
|---|---|
| `user/tests/utest/disks.c`, `user/tests/utest/diskmock.h` | S1b: `/svc/volumes` instead of `/svc/devmgr-ctl` for TEST_DISK, MOUNTS, REMOUNT, KILL, SUPERVISION. S2a: watch events instead of MOUNTS and generations; mounts by path; the GET_SERVICE-of-a-filesystem refusal becomes "devmgr has no filesystem names" (gone) and "the query channel can't reach volumes" |
| `user/tests/usbtest/storage.c` | `take_disk`: `volumes.release_disk(id)` first, then devmgr's release; `give_back`: unchanged (the new attach mounts it again) |
| `tools/fat-spare-test.sh`, `tools/fat-restart-test.sh`, `tools/sticks-test.sh`, `tools/quiet-test.sh`, `tools/update-write-test.sh`, `tools/update-pc-test.sh` | the greps' "devmgr: " prefix for storage lines becomes "volumes: " |
| `tools/shell-tests/fat-spare.txt`, `tools/shell-tests/fat-nospare.txt`, `tools/shell-tests/data-1.txt`, `tools/shell-tests/storm.txt` | the same in their `seen` lines |
| `tools/kexec-reboot-test.sh` | also volumes' stop time; "devmgr: stopping: ..." stays for drivers |
| `tools/mixer-spare-test.sh`, `tools/mixer-restart-test.sh` | unchanged: they check the module didn't change init's behaviour (S1a) |
| `tools/storage-test.sh` | S2a: usbtest's storage checks on the new `info` and `partition` |
| `tools/data-test.sh` boot 1 | "the fat service and devmgr killed": add `kill volumes` |

**New:**

| Test | Checks | Track |
|---|---|---|
| utest `volumes_watch` | a watch lists every mount then `settled`; a remount is `unmounted` + `mounted` with a new generation; a watcher that never reads is dropped at 256, and a new watch gets the whole list | S2a |
| utest `volumes_loop_answers` | while a remount waits on a mock disk that holds its `sync` for 3 s, `supervision` and a second test disk's attach are answered within 100 ms (fails today's way: blocked) | S2b |
| utest `volumes_release_disk` | the disk's mounts stop clean and stay gone until it is attached again | S2a |
| utest `block_in_flight` (usbtest, a real stick) | two reads in flight at two buffer offsets both answered right; a second `map_buffer` refused; `map_buffer(1 MiB)` capped at `buffer_max` | S2a |
| QEMU script volumes-restart (a shell test in `tools/fat-restart-test.sh`'s family) | `kill volumes`: /data and /esp go and come back, logd's file goes on; `kill devmgr`: the same; `kill usb-storage-...` of the boot stick: /data back, no "Jam OS stick removed" notice | S1b |
| sticks and storage unplug checks | no "did not end cleanly" for usb-storage after a pull (the moved race) | S1b |
| `tools/checkwants.py --selftest` | a list asking for `/svc/volumes` outside the tests is refused | S1b |

`tools/fat-storm-test.sh` and `tools/fat-restart-test.sh` must pass after
P5 too (fat's state layout version bump).

## Risks

- **The boot path.** A broken volumes means no `/data` and no `/esp`
  (logd, netlog, sntp and the settings wait for /data). S1b is a move with
  the wire unchanged, every storage script passing, and the first PC
  session starts with an everyday boot and both sticks. The previous build
  stays on the stick's "Jam OS (previous build)" entry.
- **`update -w`'s ESP write.** The writer is init's code on a worker
  thread; only the channel it asks changes. The pre-M12 build writing the
  M12 build uses its own devmgr (nothing crosses builds but FAT on the
  stick). The first M12-on-M12 `update -w` is the new path: run
  `tools/update-write-test.sh` (its write-stop and fail runs) before the
  PC. S-3 below matters here: a failed restart of the ESP's fat today
  leaves no `/esp` until a replug, so `update -w` fails for the rest of the
  boot.
- **kexec.** The stop order is volumes first (fats stopped in order:
  /data clean, the buffers usb-storage made unmapped), then devmgr's
  shutdown. If volumes doesn't stop within its deadline init kills its
  job: /data is then dirty, mounted dirty at the next boot (fat logs it).
  `tools/kexec-reboot-test.sh` checks both stop times stay a few ms.
- **The pulled stick.** Two signals in either order, and the job-empty
  race above. `tools/sticks-test.sh`, `tools/storage-test.sh` (the unplug
  during a read) and the soak's pull and replug on the PC.
- **The kernel's cycle rule.** Any channel end a supervisor keeps and
  hands to a new instance must never hold a channel end in its queue. The
  design avoids it: disks pairs are fresh, `storage` channels per instance,
  and every kept end that receives no handles carries the D2 mark.
- **The namespace is full** (finding 8): `/svc/volumes` silently missing
  would skip every disk test.
- **Merges with K1 and K2** in wave C1 (finding 13).
- **Libos paths of 4,096 bytes** (P5): libos and fat keep paths in stack
  buffers (`FS_PATH_MAX`); a 4 KiB path on a 16 KiB probe stack, or two on
  a driver thread, is a new overflow risk. P5 moves them off small stacks.

## Findings

Kinds: **bug** (wrong today, marked "today"), **latent** (wrong only if
something changes), **plan** (a gap or clash in M12-PLAN.md), **incons.**
(inconsistency), **cleanup**, **M13** (a need of M13's). "Breaks": breaks
callers (B) or adds (A).

| # | Kind | Where | What | Proposed | B/A | Callers |
|---|---|---|---|---|---|---|
| 1 | bug (today, Low) | `user/services/devmgr/request.c:111` | RELEASE of a disk's driver syncs every writable mount of every disk, 5 s each, inside the request, not the released disk's | `volumes.release_disk(id)` first; devmgr's release touches no filesystem | B | usbtest |
| 2 | bug (today, Low) | `user/services/devmgr/disk.c:529` | the mount list stops at 8 without a word, while /esp, /data, /usb0-3 and a test disk's mounts can be 12: a mount just never appears | watch events, no cap; init says when its namespace is full | B | init, utest |
| 3 | bug (today, Low) | `user/services/devmgr/fsvc.c:410` | a remount or ESP write whose new start fails retires the mount: gone until a replug; ESP_WRITE then answers ERR_NOT_FOUND for the rest of the boot | a failed start after a stop in order goes to supervision (restart with backoff, the ESP read-only) | A | none |
| 4 | latent | `user/services/devmgr/mounts.c:111` | a waiting MOUNTS keeps the raw channel handle; safe only because MOUNTS is control-only and slot 0 is never dropped (`chans.c:174`) | no waiters (events); any `later` record dies with its channel | B | init, utest |
| 5 | latent | `user/include/devmgr.h:178` | a MOUNTS caller that gives up early leaves its reply, with up to 8 `fs` channels, on the shared `/svc/devmgr-ctl` client end for ever (nobody reads another's txid), charged to devmgr and counted in the queue's 1024 | `/svc/volumes` per opener (`svc.connect`) | B | tests |
| 6 | latent | `user/services/devmgr/bind.c:236` | devmgr watches and reads every driver's DR_SERVE client end (`disk_events` reads storage answers); shared with volumes it would steal volumes' replies | devmgr never reads or watches a disk driver's channel; volumes' `storage` channels are its own (`svc.connect`) | A (usb-storage) | none |
| 7 | latent | the cycle rule (`kernel/object/channel_send.c`) | duplicates of one `storage` end handed to each new volumes would be refused whenever a dead instance left an `open_partition` reply (a `block` end) unread | per-instance channels; fresh disks pairs | design | none |
| 8 | plan | `user/include/os.h:247` | `NS_MAX_SVCS` is 16 and init publishes 16: `/svc/volumes` is refused (ERR_NO_RESOURCES, silently). Item 26 is P6's, last wave; S1b needs it first | S1b raises it to 24 (32 entries: `connect` is a 32-bit mask; an `ns_msg` grows to 528 bytes) | A | libos |
| 9 | plan | `user/include/os.h:312` | `SVC_NAME_MAX` is 10: "volumes-ctl" doesn't fit | one name, "volumes", for the control channel; the ESP channel is never published | none | none |
| 10 | plan | M12-PLAN.md, track S1's files | S1 misses `bind.c`, `chans.c`, `usb.c`; init's `services.c`, `mounts.c`, `ctl.c`, `espwrite.c`, `reboot.c`, `main.c`; `sh_allow.c`; `tools/checkwants.py`; `os.h`; usb-storage's `svc.connect`; the tests | the tables above | none | none |
| 11 | plan | M12-PLAN.md, S1 "every storage test unchanged" | ten scripts grep "devmgr: " storage lines (the table under Tests), and utest and usbtest send storage requests to `/svc/devmgr-ctl` | S1b changes the prefix and the channel only, the checks themselves unchanged | none | tests |
| 12 | plan | `tools/genidl.py:137`, `user/services/fat/fat.h:141` | Q8's `rename(str[<=4096], str[<=4096])` is 8,204 bytes: over genidl's 8 KiB message and fat's 4 KiB request area | design question 4 | B | fs callers |
| 13 | plan | K1, K2 and S1 in wave C1 | K1 marks devmgr's kept `fs` ends (`fs_serve_end`), which S1b moves; K2 reorders `start_driver` in `bind.c`, which S1b edits | S1b merges before K2; K1 marks fat's and init's ends, S1b (or S2a) volumes' after K1 | none | none |
| 14 | incons. | `user/services/init/mounts.c:41`, `user/services/init/ctl.c:185`, `user/services/devmgr/fsvc.c:26` | test mounts are told apart by a "-test" suffix in three places | flag 2 in `mounted`; paths unchanged | B | init |
| 15 | incons. | `user/include/devmgr.h:304`, `:335` | a filesystem service has two magic names (DEVMGR_FS_SVC by disk and partition, DEVMGR_FS_MOUNT by mount) | volumes names it by mount path alone | B | init, utest |
| 16 | incons. | `user/include/devmgr.h:240` | REMOUNT is (DEVMGR_USB_MOUNT, N, flags); kill by FS_MOUNT; the shell by path | paths everywhere | B | init, utest |
| 17 | incons. | `user/include/devmgr.h:286` | ESP_WRITE answers with a handle only when writable: a varying handle count | `open_writable`, `close_writable` | B | init |
| 18 | incons. | `user/include/devmgr.h:136`, `:119`, `:221` | DRIVER_VIEW (1 + BARs), GET_DRIVER (2 or 3), RELEASE (0 or 1) vary their handle counts, which genidl's results can't | one call per handle kind (devmgr's draft) | B | utest, usbtest, the shell's `devices`, init |
| 19 | cleanup | `user/services/devmgr/disk.c:189` | INQUIRY text trimmed by devmgr | usb-storage sends `str` vendor and product | B | volumes |
| 20 | cleanup | `abi/idl/storage.idl` `disk_id` | a call of its own, made blocking (`disk.c:231`) | in `info`, with the GPT disk GUID | B | volumes |
| 21 | cleanup | `user/services/devmgr/spare.c`, `user/services/init/spare.c` | near copies (577 and 439 lines) | the module; after the split its users are volumes and init | none | none |
| 22 | cleanup | `user/services/init/main.c:178` | test mode's own copy of devmgr's start and stop | one start for both modes, volumes included | none | none |
| 23 | cleanup | `user/services/devmgr/disk.c:120`, `user/services/devmgr/usb.c` | hand-written reading of answers to sends (`next_msg`, `discard`, txids from 0xd15c0000) | genidl's `_send`, `idl_reply_read`, `_result`; volumes' txids start from the clock, so a restarted volumes never matches a dead one's | B | none |
| 24 | M13 | `user/services/fat/ffport/ffconf.h:52` | `FF_LFN_BUF` 255 with UTF-8 names: a long non-ASCII name (FAT allows 255 UTF-16 units, up to 765 bytes of UTF-8) doesn't fit, and FatFs answers with the 8.3 alias | P5: `FF_LFN_BUF` 765 and readdir records sized for it | A | none |
| 25 | M13 | `abi/idl/fs.idl` `rename` | a FAT rename can't replace its target; POSIX `rename` does | none in M12: a replacing rename is an addition (a new method) M13 can add; noted for M13's row | A | none |
| 26 | M13 | `abi/idl/file.idl` `stat` | `is_dir` (Q8) is always 0 until directory handles | as decided; say so in the comment | B | libos |
| 27 | known | ARCH-CHECK claim 0 | devmgr's and init's storage waits | the waits table above | B | init |
| 28 | incons. | `abi/idl/block.idl`, `fs.idl`, `file.idl`, `fsctl.idl`, `storage.idl` | their restart paragraphs name devmgr | name volumes (S1b) | none | none |
| 29 | latent | `user/services/devmgr/supervise.c:179` | the pulled stick's job check relied on the fats dying first in the same call | the 2 s recheck, and a test (Risks) | none | none |
| 30 | incons. | `user/services/devmgr/fsvc.c:276` | a usb-storage crash on another stick posts "USB stick removed", then "USB stick added" when it is back (`drop_services` tells at DISK_DOWN as at DISK_FREE); the boot stick's notice waits for the disk to be forgotten | tell only at `gone` for every disk: a driver restart is no news | A | none |

Bugs today: 1, 2, 3 (all Low). None is worth fixing before S1b moves the
code: each fix lands in volumes' version.

## Design questions

Only what the owner's answers don't settle.

1. **How does init learn of mount changes?** (A) A watch channel of
   events (`mounted`, `unmounted`, `settled`): no waiting call, no thread
   in init, no waiter table, no handle arrays in genidl. (B) The plan's
   `mounts(known)` as a `later` method with a varying number of handles:
   genidl gains handle arrays. (C) B without handle arrays: `mounts`
   answers paths only, then one call per mount for its channel.
   *Recommendation: (A).*
2. **How does a disk reach volumes?** (A) A disks channel pair made fresh
   by init whenever devmgr or volumes starts; devmgr sends `attach` and
   `gone` events, each `storage` channel volumes' own (usb-storage answers
   `svc.connect`). (B) devmgr hands duplicates of its own `storage` end:
   devmgr must never read it (findings 6, 7) and must drain it before each
   hand-over. (C) volumes holds devmgr's control channel and asks for
   disks: more authority than it needs. *Recommendation: (A).*
3. **Does volumes outlive its process?** (A) No: its death ends every
   fat; init restarts it and every mount comes back fresh (open files
   closed, what the fats held unwritten lost), as a devmgr death does
   today. (B) Yes: init keeps volumes' kept `fs` channels, keepers and
   state VMOs, and fats run in a job init owns, so a volumes death is
   unseen. *Recommendation: (A) in M12;* (B) is the same machinery as the
   drivers' milestone after M12 and can come with it.
4. **Two 4,096-byte paths in one `rename`.** (A) genidl's message limit
   goes from 8 to 16 KiB, fat's request area from 4 to 12 KiB (its state
   layout version up), libos's path buffers leave small stacks. (B) Each
   path up to 4,095 bytes but both together under 8 KiB
   (ERR_NAME_TOO_LONG otherwise). (C) Paths up to 2,048 bytes.
   *Recommendation: (A):* POSIX's PATH_MAX is per path, and the cost is
   fat's state VMO (pages committed only as used).
5. **May `block` answer out of order?** (A) Yes: each request by its txid;
   a client that needs order waits for an answer; `sync` covers the writes
   answered before it was sent. (B) No: answers in the order sent, which an
   NVMe driver can only give by holding finished commands back.
   *Recommendation: (A):* fat sends one at a time, so nothing changes for
   it, and M12.7 gets the queue it was shaped for.

## Suggested tracks and order

The plan's S1 is two agent-hours with the module in it; split:

| Track | What | Needs | Files |
|---|---|---|---|
| **S1a. The module** (refactor) | user/lib/supervise.c and its header from the two `spare.c` and devmgr's restart window; devmgr and init on it; every log line and behaviour unchanged | the answers | `user/services/devmgr/spare.c`, `user/services/devmgr/supervise.c`, `user/services/init/spare.c`, libos's Makefile lines |
| **S1b. volumes, the move** (refactor) | the tables above; the wire of the moved methods unchanged (a hand-written header copies devmgr.h's); hand-written `disks` messages and SET_VOLUMES (as SET_CONSOLE); usb-storage's `svc.connect`; init starts volumes in both modes, the reboot order; `NS_MAX_SVCS` 24; the tests' channel and prefix; the volumes-restart script. The blocking calls move as they are (they then block volumes, not devmgr) | S1a | devmgr's storage files and sections, volumes' directory, `user/include/devmgr.h`, `drivers/usb-storage/main.c`, init's services, mounts, ctl, espwrite, reboot, main, shell and init.h, `sh_allow.c`, `tools/checkwants.py`, `os.h`, `user/lib/start.c`, the tests listed |
| **S2a. The protocols** | volumes.idl (volumes, volesp, disks, mounts), `storage` and `block` reshaped, `fsctl`'s comment; init's mounts on watch events (no thread); usb-storage's new `info`, `partition`, `map_buffer(size)`; fat's block client; utest and usbtest on the generated stubs; the new tests | S1b, G (strings, events, handle arguments), K1 (Q1) | volumes' directory, `abi/idl/storage.idl`, `abi/idl/block.idl`, `abi/idl/fsctl.idl`, `drivers/usb-storage/`, `user/services/fat/disk.c`, `user/services/init/mounts.c`, the disk tests |
| **S2b. No waits** | volumes' operations as tasks and `later` replies; init's `mount`, `sync` and kill of a mount answered later; `volumes_loop_answers` | S2a | volumes' directory, `user/services/init/ctl.c`, `user/services/init/mounts.c` |
| **D. devmgr in IDL** | devmgr.idl as drafted; the device address; SET_CONSOLE and SET_VOLUMES as handle arguments; every caller; init's other kills and `status` as sends | S2b (both edit init's `ctl.c` and `services.c`) | as the plan |
| **P5. Files** | `fs` and `file` as drafted, the five codes from fat, fat's lock by entry, `FF_LFN_BUF`, fat's state layout, design question 4's sizes | G; independent of the S tracks (fat's other files) | as the plan, plus `user/services/fat/ffport/ffconf.h` |

Order: S1a in wave C1 with K1, K2 and G; S1b right after S1a (before K2
merges, finding 13); S2a once S1b, G and K1 are in, with P5 beside it; S2b;
D; then P6. Each S track runs the storage scripts of the plan's test
table; S1b and S2b also run `tools/kexec-reboot-test.sh` and
`tools/update-write-test.sh`.
