# M8 (storage) review

An independent read of everything M8 added, at commit 6a9a899: the usb
bulk path and usb-storage, the FatFs port and the fat service, logd, the
bootfs server, devmgr's disks and mounts, init's mounts and control
channel, libos's namespace and file calls, the shell's file commands, the
image and flashing tools, and the tests. Findings first; each one's
outcome is filled in as it is fixed.

Severity: **High** loses data or lets a program take down a service it
should not reach; **Medium** is wrong behaviour a user will meet; **Low**
is a wart.

## Findings

| # | Sev | Where | What |
|---|---|---|---|
| 1 | Medium | `user/services/fat/fileops.c` (files_open, FatFs's FF_FS_LOCK) | `cat /data/logs/boot-NNNN.txt` of the log logd is writing fails (ERR_BAD_STATE): a second open of a file open for writing is refused, readers too. Decided: read-only opens are allowed next to the one writer and see what it has written; a second writer stays refused. |
| 2 | Medium | `user/services/init/ctl.c` (op_reboot), `user/services/logd/main.c`, `user/services/fat/disk.c` (disk_settle) | The end of every boot log is lost: init syncs and resets without telling logd, so the lines of the shutdown path ("init: /data synced ...") never reach the file. And a file.sync is dear: FatFs's f_sync sends one block.sync, disk_settle a second, with two FAT sectors read and written to set the clean bit and again to clear it at the next write. |
| 3 | Medium | `Makefile` (mformat -v), `user/services/fat/main.c` (format) | The volume labels on disk. mformat's label is a root-directory entry preceded by a long-name entry (fsck_msdos: "Invalid long filename entry for volume label"), on both the ESP and the data volume. A volume fat formats has a proper root entry `JAMOS-DATA` but its boot sector's label field (offset 71, and the backup at sector 6) says `NO NAME`: FatFs never writes that field. Checked on the image and on a volume formatted in QEMU. |
| 4 | High | `kernel/arch/x86_64/pcid.c:79` | Alder Lake / Raptor Lake erratum (Intel ADL063, RPL042): INVLPG may leave global TLB entries when PCIDs are on. Jam OS enables PCIDs on these CPUs, maps the kernel global and shoots kernel pages down with invlpg. Verified against Linux's table (arch/x86/mm/init.c, invlpg_miss_ids): see below. |
| 5 | Medium | `user/services/devmgr/disk.c:635` (disk_remount) | `mount -r` / `mount -w` syncs the filesystem and then kills its service. A program writing between the sync and the kill loses that write and may leave the volume dirty. |
| 6 | Medium | `user/services/shell/sh_vfs.c:166` (SH_FILE_MAX) | `cat` of a file over 4 MiB fails at once with ERR_OUT_OF_RANGE: it reads the whole file first. |
| 7 | Medium | `user/services/devmgr/supervise.c:117,49` | Another stick whose fat keeps ending with a disk error is restarted five times and then counted as a devmgr problem, with a RESULTS line: a bad foreign stick makes the `init` run fail. |
| 8 | Low | `user/services/devmgr/disk.c` (CALL_WAIT), `drivers/usb-bus/bulk.c` (td_wait) | devmgr waits up to 2 s per call on a usb-storage that is in the middle of a slow command; usb-bus runs one bulk transfer inside its loop. Assessed below. |
| 9 | Low | `user/services/init/shell.c` (root_with) | RIGHT_READ on the root resource is one right for klog_open, serial_open, proc_list and the clock: sysmon and logd each get more than they use. |
| 10 | High | `drivers/usb-storage/block.c:87` | The superfloppy check runs before the partition table is read. A partitioned stick whose MBR also carries a BPB (some formatters write one) is served as one whole-disk volume: block 0 and every partition reachable through one `block` channel, and after `mount -w` a FAT driver writing FATs and a root directory over the partitions. |
| 11 | Low | `tools/flash-usb.sh` | Reviewed: see below. |
| 12 | Low | several | Tidying: `job_find_process` is used only by its own test; `kill` does not reach PCI drivers; ARCHITECTURE's Storage section opens with "Not built yet."; two scripts accept either order of two lines; the mouse test runs at one resolution. |
| 13 | Low | fat, logd, shell | Foreign entries on /data (`.Spotlight-V100`, `.fseventsd`, `System Volume Information`): assessed below. |
| 14 | High | `user/services/fat/fileops.c:212` (attach), `:91`, `:116` | fat maps each open file's transfer buffer and hands the client a handle with RIGHT_WRITE, which also allows `vmo_set_size`. A client that shrinks the buffer to nothing and then reads makes fat fault inside f_read: any program with `/data` in its namespace can kill the filesystem service (and whatever was not synced). usb-storage avoids exactly this by never mapping its client's buffer. |
| 15 | Medium | `user/services/devmgr/disk.c:462` (got_info, got_stat) | "The boot disk" is the first disk with an ESP holding boot/jamos.elf and a second partition of type 0C, not the disk the machine booted from. With two Jam OS sticks plugged in, which one becomes /data (and is formatted if blank) depends on the order they enumerate. Design question. |
| 16 | Low | `user/services/fat/disk.c:95` (disk_is_blank) | "Blank" is "no 55 AA at the end of the partition's first sector". A /data whose first sector is damaged would be formatted again. A stricter test (the first 64 KiB all zero) would not re-format after a format that was cut short, so it is left: design note. |
| 17 | Low | `drivers/usb-storage/main.c:102` | The configuration descriptor's length comes from usb-bus and is not clamped to the 1024-byte buffer before it is walked. |
| 18 | Low | `user/services/shell/cmd/ls.c`, `df.c`, `mount.c` | Names and volume labels from a stick are printed as they are: a crafted name can carry escape sequences to the console. |
| 19 | Low | `user/services/shell/sh.h` (SH_DIR_MAX) | `ls` and `find` show the first 256 entries of a directory and say nothing about the rest. |
| 20 | Low | `user/services/init/ctl.c` (MOUNT_WAIT, KILL_WAIT) | init's one loop waits up to 25 s in `mount` and 15 s in `kill`: no service is restarted meanwhile. |
| 21 | Low | `user/services/fat/fsops.c:97` | readdir walks the directory from the start for every entry asked for: a listing is quadratic. Fine at 256 entries. |
| 22 | Low | `user/services/init/shell.c` (start_shell, NS_ALL) | The shell, and every program it runs, gets every mount read-write. The plan allows a namespace without /data; nothing uses one yet. Design note. |

### Item 4: the erratum

- Linux commit ce0b15d11ad8 ("x86/mm: Avoid incomplete Global INVLPG
  flushes", 6.4): "some INVLPG implementations can leave Global
  translations unflushed when PCIDs are enabled"; PCID is never enabled on
  the listed models.
- The later fix ("x86/mm: Don't disable PCID if 'incomplete Global INVLPG
  flushes' is fixed by microcode") gives the first fixed microcode
  revision per model, from Intel engineers, citing Intel's specification
  updates (ADL063, RPL042) and the intel-microcode-20240312 release. The
  table in today's arch/x86/mm/init.c: Alder Lake (06_97h) 0x2e, Alder
  Lake L (06_9Ah) 0x42c, Gracemont / Alder Lake N (06_BEh) 0x11, Raptor
  Lake (06_B7h) 0x118, Raptor Lake P (06_BAh) 0x4117, Raptor Lake S
  (06_BFh) 0x2e. PCID is turned off when the running revision is lower.
- Intel's documents themselves could not be fetched from here; the ids and
  revisions are as quoted on LKML.

So it holds. Jam OS's exposure is the same as Linux's was: global kernel
mappings, invlpg for kernel shootdowns, PCIDs on.

### Item 8: the waits

- devmgr: `storage.partition` (got_info) and `storage.open_partition`
  (fs_handles) wait at most 2 s each, fs.sync before a remount or a stop
  5 s. All bounded; a late answer is drained and its handle closed
  (next_msg). While devmgr waits it starts and restarts nothing. Making
  them asynchronous like storage.info is a contained change per call but
  touches the disk state machine: left, reported.
- usb-bus: td_wait keeps servicing the controller (hc_wait), so interrupt
  reports of other devices still flow, but no other channel is answered
  and no port change is acted on until the transfer ends or times out
  (5 s reads, 10 s writes). Asynchronous transfers are a redesign of the
  serve loop: design question.

### Item 11: flash-usb.sh

Quoting is sound. The EXIT trap unmounts after any failure past the
mount. Found: the device pattern `/dev/disk[0-9]*` also matches
`/dev/disk4s1` and anything after a digit; an interrupt (Ctrl+C) relies
on the shell running the EXIT trap; the three files are copied in place,
so a copy that fails half way leaves a stick that does not boot (copy to
a new name and rename would not). The auto-detect reads `diskutil list`
columns, which is as good as macOS allows.

### Item 12: the ordering in usbkeys.txt and review-killinit.txt

Not a bug: "shell: killed process" goes through the console, init's line
through the kernel log, two writers to one serial port. `seen` is right.

### Item 13: foreign entries

fat lists hidden and system entries like any other; a long name that does
not fit 255 bytes of UTF-8 comes out as its short name (FatFs); odd
timestamps (month 0, day 0) read as "no date". logd finds its number with
stat calls on its own names, not by walking the directory. `rm -r` stops
at the first entry it cannot remove. Nothing assumes /data holds only
Jam OS's files.

## Outcomes

Filled in as the fixes land.
