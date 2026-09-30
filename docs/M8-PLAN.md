# M8 plan: storage (USB stick, FAT32, files for every program)

Goal ([roadmap](ROADMAP.md#next-m8-storage)): **`ls /boot` and writing a
file under `/data` work from a user-space filesystem service; every boot's
log is saved as `/data/logs/boot-NNNN.txt`; the stick still boots after a
pulled-plug test; a PC run's log can be read on the Mac from the stick.**

Decided already ([HISTORY.md](HISTORY.md#decisions)): the USB stick and
FAT32 only (no NVMe, no other filesystem); FAT32 comes from a **FatFs**
port, not a hand-written driver. Everything is a process; the kernel gains
nothing but what enforcement needs.

The PC's stick ([HARDWARE.md](HARDWARE.md#usb)): 058f:6387, high speed,
behind the ASMedia hub, so every block goes through the hub (the TT is not
involved: a high-speed device behind a high-speed hub). In QEMU the boot
stick is a SuperSpeed `usb-storage` device on root port 1.

## What M8 adds

| Piece | What it is |
|---|---|
| Bulk transfers (usb-bus) | bulk-IN and bulk-OUT endpoints: transfer rings, completion, STALL and Clear Feature(ENDPOINT_HALT), and a shared DMA buffer per opened pair so data never travels in messages |
| usb-storage (driver process) | USB mass storage, Bulk-Only Transport: CBW / data / CSW, the SCSI commands a stick needs, BOT reset recovery. Reads the partition table and serves the `block` protocol, one channel per partition, each limited to that partition's sectors |
| fat (service process) | FatFs over one `block` channel, serving the `fs` protocol; mounted at `/data`, and a second, read-only instance over the ESP at `/esp` |
| The file namespace (libos + init) | `fs` and `file` protocols; each program gets a namespace (mount point -> `fs` channel) at startup; libos `open`, `read`, `write`, `readdir`, `mkdir`, `unlink`, `rename`, `stat`, `sync`; `/boot` is served by a bootfs server under the same protocol |
| spawn from a file | libos spawn takes a VMO (a file's contents) instead of a bootfs name, so `run /data/bin/x` works |
| logd (service process) | follows the kernel log from its first byte and appends it to `/data/logs/boot-NNNN.txt`, syncing as it goes |
| The stick's layout | the ESP (Limine, kernel, bootfs), which Jam OS never writes, plus a data partition formatted on first boot |
| Smaller | `debug_command`'s `kill <name>` moves to init's control channel (the cleanup review) |

## Fixed decisions

**Authority.** No program gets more of the disk than it needs:
- usb-bus owns the controller and the DMA (as in M7). usb-storage gets its
  interface's `usb` channel and a shared buffer; never a dma_cap.
- usb-storage serves one `block` channel **per partition**, each refusing
  any sector outside its partition (ERR_OUT_OF_RANGE). It never serves the
  whole disk to anyone, so nothing but usb-storage can touch the ESP or
  the partition table. The ESP's channel is read-only.
- fat gets only one partition's channel. A bug in fat or FatFs can't
  write outside `/data`; the `/esp` instance's channel is read-only, and
  fat refuses every write on a read-only `block` channel
  (ERR_ACCESS_DENIED) before FatFs sees it.
- A program sees only the mounts its namespace holds. The shell and the
  programs it runs get `/boot` and `/data`; a program can be started with
  a namespace without `/data`.

**Bulk data never goes through messages.** IDL messages top out at 8 KiB
and live on a kernel stack. Every bulk path uses a **shared VMO** handed
out once as a handle result (already supported by genidl), with messages
carrying only offsets and lengths:
- usb: `open_bulk(ep_in, ep_out) -> (buffer VMO, size)`, then
  `bulk_in(offset, length)` / `bulk_out(offset, length)`. usb-bus pins the
  buffer with its own dma_cap; the class driver maps it.
- block: `open` gives a buffer VMO; `read(lba, count, buffer offset)`,
  `write(...)`, `sync()`, `info() -> (block size, blocks, read-only)`.
- file: `open` returns the file's channel plus a buffer VMO;
  `read(offset, length)` fills the buffer, `write` takes what the client
  put there. 64 KiB buffers; larger transfers loop.
No new IDL feature is needed. Handle *arguments* stay refused.

**The protocols** (new files under abi/idl: usb.idl grows, plus block,
fs, file):
- `fs`: `open(path, flags) -> (handle file, handle buffer, u64 size)`,
  `stat(path)`, `readdir(path, cookie) -> (entries...)`, `mkdir`,
  `unlink`, `rename(from, to)`, `sync()`. Paths are relative to the mount
  point, `/`-separated, at most 255 bytes; `..` never escapes the mount
  (the server resolves it). Flags: read, write, create, truncate, append.
- `file`: `read`, `write`, `truncate`, `stat`, `sync`; closing the channel
  closes the file.
- Errors map FatFs's FRESULT onto ERR_* (not found, exists, no space,
  read-only, name invalid, ...), never passed through raw.

**Namespace.** A startup handle list SR_NS (mount path -> `fs` channel)
given by whoever starts a program; libos resolves a path to the longest
matching mount and calls that `fs` channel. init builds the first one:
`/boot` from the bootfs server (the bootfs image served read-only through
`fs`; always there, even with no USB storage), `/esp` and `/data` when
devmgr reports their fat services up.
Mounts that arrive later (the stick replugged, fat restarted) reach
running programs the way devmgr's new channel reaches the shell today (a
channel from init). The shell's mount table (sh_vfs.c) becomes a thin
client of libos's namespace.

**Who starts what.** devmgr binds usb-storage to mass-storage interfaces
(class 08, subclass 06 SCSI, protocol 50 BOT). usb-storage reports its
partitions to devmgr; devmgr starts fat for the data partition of the
boot disk (see the layout) and a read-only fat for the ESP of the disk
it booted from, and hands init their `fs` channels (DEVMGR_MOUNTS in user/include/devmgr.h, which init waits on in a loop); init mounts
them at `/data` and `/esp`. Restarts follow devmgr's supervision (M7).

**The stick's layout** (built by the foundation: tools/mkimage.py,
tools/mbr-grow.py). An MBR with two partitions:
1. the ESP (type 0xEF, 1 MiB to 64 MiB, FAT32 volume `JAMOS`): Limine,
   jamos.elf, bootfs.img, limine.conf; never written by Jam OS;
2. the data partition (type 0x0C, FAT32 LBA, from 64 MiB to the end),
   volume label `JAMOS-DATA`, so Windows and macOS mount it and the Mac
   can read the logs.
Jam OS mounts or formats a data partition only on **the disk it booted
from**: the disk whose partition 1 is an ESP holding boot/jamos.elf, with
partition 2 of type 0x0C. The QEMU image carries a formatted 64 MiB data
partition, so every track can test against a real FAT volume from day one.
`make usb` writes the image, then grows partition 2 to the end of the stick
and wipes its first MiB; fat finds no FAT volume there and formats it
(FatFs f_mkfs, label `JAMOS-DATA`) on the first boot. Flashing the new
layout **erases the stick once** (`make usb`); after that, updates copy
files onto the ESP as now.

**FatFs** (ChaN, BSD-style licence): vendored unmodified in a new
third_party/fatfs directory with its licence and a VERSIONS.md entry.
Configuration: long file names on (UTF-8 API), exFAT off, f_mkfs on,
re-entrancy off (fat is single-threaded), the `diskio` callbacks
implemented over the `block` channel, `get_fattime` from the RTC.

**Write safety** ([ARCHITECTURE.md](../ARCHITECTURE.md#storage)): FatFs
writes data, then the FATs, then the directory entry. `sync` (and every
`fs.sync`) flushes FatFs and sends SCSI SYNCHRONIZE CACHE. A volume found
dirty is mounted anyway and logged (there is no fsck); the ESP is never
written, so the stick always boots. `reboot` and Ctrl+Alt+Del ask init to
sync `/data` (bounded: 2 s) before resetting.

**Boot logs.** logd opens a klog reader from byte 0, picks the next free
`boot-NNNN.txt` in `/data/logs` (creating the directory), writes whatever
the kernel logged before `/data` existed, then follows the log, syncing
at most once a second and on every `reboot`. A panic can't be saved yet
(that is M8.5's crash kernel); the file then ends at the last sync.

**Bounded everything.** Every SCSI command has a timeout (10 s for a
write, 5 s otherwise) and BOT reset recovery on failure; a stick that
stops answering makes usb-storage fail its requests with ERR_TIMED_OUT,
never hang fat, logd or the shell. Unplugging the stick at any time must
leave the system running (`/data` goes away; files return
ERR_PEER_CLOSED).

## Tracks

Phase 1: four tracks in parallel. The protocol
files (usb.idl changes, block, fs, file) land on main first as the
contract; the tracks build on them.

### Foundation (on main, before the tracks)
- The IDL files above, generated, with the error mapping written down.
- The SR_NS startup role and the file API in os.h (`file_open`,
  `file_read`, ..., `fs_readdir`, FS_* flags), with placeholder stubs in
  user/lib/fs.c that Track C replaces.
- The stick layout (tools/mkimage.py, tools/mbr-grow.py, write-usb.sh)
  and the three new error codes ERR_IO, ERR_ALREADY_EXISTS, ERR_NO_SPACE.

### Track A: bulk transfers + usb-storage
- usb-bus: bulk endpoints in the endpoint-context fill (config.c), bulk
  transfer rings and completion (a new file beside intr.c), STALL and
  Clear Feature(ENDPOINT_HALT) recovery, the `open_bulk` / `bulk_in` /
  `bulk_out` methods with the pinned shared buffer.
- The usb-storage driver: GET MAX LUN, CBW/CSW with tag checks, INQUIRY,
  TEST UNIT READY (with retries while the stick spins up), READ
  CAPACITY(10), READ(10), WRITE(10), REQUEST SENSE, SYNCHRONIZE CACHE; BOT
  reset recovery (Bulk-Only Mass Storage Reset, clear both halts); MBR
  parsing; the `block` protocol per partition, with the range
  check.
- QEMU: the boot stick itself (usb-storage), a second usb-storage disk
  behind the usb-hub, unplug mid-read, a STALL on an unknown command.

### Track B: FatFs + the fat service
- Vendor FatFs; the diskio glue over `block`; the `fs` and `file`
  protocols; FRESULT -> ERR_* mapping; f_mkfs on an unformatted
  JAMOS-DATA.
- Built and tested against a **RAM-disk block server** in utest (a mock
  usb-storage), so it doesn't wait for Track A: format, create, write,
  read back, long names with spaces and lowercase (the 2025 attempt's
  failures as tests: `touch "My Notes.txt"`, `notes.txt` stays lowercase,
  `holiday-photos.txt` and `holiday-plans.txt` get distinct aliases,
  forbidden characters refused), a full disk, a dirty volume.

### Track C: the namespace, spawn from a file, init, the shell
- libos: SR_NS (its encoding is this track's), path resolution to a mount,
  the file calls in user/lib/fs.c; spawn from a VMO (spawn.c loads from
  any VMO range; bootfs becomes one caller).
- A bootfs server (the image served read-only through `fs`) so `/boot` is
  a mount like any other.
- init: builds the namespace (`/boot` at once; `/data` and `/esp` from
  DEVMGR_MOUNTS, waited on in a loop) and passes it to what it starts, and
  new mounts to the running shell; syncs `/data` on `reboot` and
  Ctrl+Alt+Del (bounded: 2 s); serves `kill <name>` on its control channel,
  and `debug_command`'s kill goes (the kernel keeps only ktest, bench,
  stress, crash and panic).
- The shell: sh_vfs.c on the namespace; `ls`, `cat`, `cd`, `find` work on
  every mount; new commands `mkdir`, `rm`, `mv`, `cp`, `touch`, `write`
  (text from the command line into a file), `df`, `sync`; `run` takes a
  path; `kill` goes through init.
- Tested against the bootfs server and a stand-in `fs` server until
  Track B's fat lands.

### Track D: devmgr's storage side, and logd
- devmgr: for each usb-storage disk, read its partitions (`storage`
  protocol); if it is the boot disk (partition 1 an ESP holding
  boot/jamos.elf, partition 2 type 0x0C), start a read-only fat on the ESP
  and a read-write fat on the data partition, and publish them through
  DEVMGR_MOUNTS; restart them under the usual supervision; a stick that
  goes away removes its mounts.
- logd: the boot log files as above (a klog reader from byte 0, the next
  free `/data/logs/boot-NNNN.txt`, sync at most once a second).
- Tested with a mock usb-storage (a `storage` + `block` server in utest
  over a RAM disk) until Track A lands.

### Phase 2: join, then the PC
- devmgr's match table (08/06/50 -> usb-storage: Track A adds the line),
  partitions -> fat -> DEVMGR_MOUNTS -> init mounts `/data` and `/esp`. End to end in QEMU: boot, `ls /data`,
  write a file, reboot, read it back; a boot log per boot.
- The pulled-plug test in QEMU: kill QEMU while logd writes; the next boot
  still boots and mounts `/data` (dirty volume logged).
- Independent review of the whole milestone, by someone who built none of it.
- PC rounds: **read-only first** (the partition list and `ls /data` on a
  stick flashed with the new layout), then writes, then the boot logs,
  then the pulled-plug test on the real stick.

### Phase 2b: other sticks (after Track C, before the review)
Any other USB stick with a FAT partition becomes usable, safely:
- devmgr starts a **read-only** fat for each FAT partition of a disk that
  is not the boot disk and publishes it as `/usb0`, `/usb1`, ... (in the
  order found; the mount goes when the stick does). `ls`, `cat` and `cp`
  from it work; nothing on it can be changed.
- `mount -w /usbN` (shell -> init -> devmgr's control channel) reopens
  that partition read-write and restarts its fat; `mount -r /usbN` goes
  back. `mount` alone lists the mounts and whether each is writable.
- **Never formatted**: fat formats only the boot disk's blank data
  partition. For any other disk it is started with formatting off, in
  either mode; a partition it can't mount is left alone and reported.
- Tests: a second usb-storage disk in QEMU with a FAT volume (read its
  files; a write refused; `mount -w`, write, `mount -r`; unplug it while
  mounted), and a non-FAT disk (no mount, nothing written).

## Done when
- QEMU at 4 and 8 CPUs: all ktests; init + utest (with the RAM-disk fat
  tests); the end-to-end storage test; unplug mid-read; the pulled-plug
  test; stress; the shell scripts.
- The real PC: another FAT32 stick plugged in shows up at `/usb0`, its
  files can be read, a write is refused until `mount -w /usb0`.
- The real PC: `ls /boot`, `ls /esp` (Limine, the kernel, limine.conf,
  bootfs.img) and `ls /data` from the shell; `cat
  /esp/boot/limine/limine.conf`; writing to `/esp` refused; `write` then
  `cat` a file in `/data`; a boot log per boot on the stick, read on the
  Mac; pulling the stick mid-write and replugging leaves it bootable;
  All tests, the 2-minute stress, and the 10-minute sign-off.

## Decisions for this plan (2026-09-30)
1. Nothing on the stick needs keeping: flashing the new layout (which
   erases it once) is fine.
2. The data partition takes the rest of the 2 GB stick.
3. Both: `/boot` is the bootfs image (the programs, in RAM, always
   there), and `/esp` is the ESP's own files, read-only, when USB storage
   works.

## Rules for every track
- The foundation's IDL files and headers are the contract between tracks;
  a track that needs one changed says so instead of changing another
  track's side.
- Follow [CODING-GUIDE.md](../CODING-GUIDE.md): bounded waits, handles
  with the narrowest rights, a test for every fix, no GPL code (FatFs is
  BSD-style; Linux's usb-storage may be read for facts, never copied).
- Every commit leaves the tree building with the existing tests passing at
  4 and 8 CPUs; `make check` passes (docs and include order too).
- Anything only the real PC can show is written down with what the PC run
  should print.
