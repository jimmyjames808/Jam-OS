# linuxbench: the Linux column

`linuxbench` measures, on Linux, the equivalent of each line of
[BENCH.md](../../docs/BENCH.md), on the same PC and with the same method
(the TSC, 4000 samples, median and p99, 20 ms of warm-up, the same
CPUs P, P2, HT and E, every thread pinned at SCHED_FIFO). Each line it
prints is named like the Jam OS line it stands beside, followed by `=`
and what Linux does for it. Why and how: [M11.5-PLAN.md](../../docs/M11.5-PLAN.md#the-linux-column);
the method in detail: [lb.h](lb.h)'s header.

It runs twice on the PC, from an Ubuntu 24.04 live USB stick (nothing is
installed; the PC's SSD is never touched): once as Ubuntu comes, once
with `mitigations=off`. Each run writes one text file to the SanDisk.

## What you need

- A spare USB stick of 8 GB or more. **Everything on it is erased.** Not
  the Jam OS stick, not the SanDisk.
- The SanDisk (FAT32, the one Jam OS mounts at `/usb0`): it carries the
  program to the PC and the results back.
- About 30 minutes, most of it the download and writing the stick.

## 1. Build the program (on the Mac)

Start Docker Desktop, then from the Jam OS checkout:

```sh
make -C tools/linuxbench docker
file build/linuxbench/linuxbench
```

The first build downloads Docker's `gcc:14` image (about 1.5 GB). `file`
must say `ELF 64-bit LSB executable, x86-64 ... statically linked`.
Without Docker: on any x86_64 Linux machine with gcc, the same command
without `docker` builds it; or build it on the live system itself (step
4's last box).

## 2. Put it on the SanDisk (on the Mac)

Plug in the SanDisk, then (its name is in `ls /Volumes`):

```sh
cp build/linuxbench/linuxbench /Volumes/<SANDISK>/
cp -R tools/linuxbench /Volumes/<SANDISK>/linuxbench-src    # only to build it on the PC
diskutil eject /Volumes/<SANDISK>
```

## 3. Make the Ubuntu stick (on the Mac)

1. Download `ubuntu-24.04.x-desktop-amd64.iso` (the newest 24.04.x) from
   <https://ubuntu.com/download/desktop>, and `SHA256SUMS` from
   <https://releases.ubuntu.com/24.04/>. Check it: the output of the
   first command must be the line for that file in `SHA256SUMS`.

   ```sh
   shasum -a 256 ~/Downloads/ubuntu-24.04*-desktop-amd64.iso
   grep desktop-amd64 ~/Downloads/SHA256SUMS
   ```

2. Plug in the spare stick and find it by its size:

   ```sh
   diskutil list external
   ```

   Say it is `/dev/disk6`. **Check twice** that it is not the Jam OS
   stick (partitions `JAMOS` and `JAMOS-DATA`) or the SanDisk (15.4 GB).

3. Write the image (your password; about 5 to 10 minutes, Ctrl+T shows
   how far it is):

   ```sh
   diskutil unmountDisk /dev/disk6
   sudo dd if=$(ls ~/Downloads/ubuntu-24.04*-desktop-amd64.iso) of=/dev/rdisk6 bs=4m
   diskutil eject /dev/disk6
   ```

   If macOS says the disk is not readable, click Ignore.

## 4. First boot: Linux as it comes

1. With the PC off, plug in the Ubuntu stick and the SanDisk. Power on and
   press **F8** for the board's boot menu; pick the Ubuntu stick (its
   UEFI entry).
2. At GRUB's menu, on "Try or Install Ubuntu", press **e**. At the end of
   the line that starts with `linux`, add a space and:

   ```
   nomodeset systemd.unit=multi-user.target
   ```

   (text mode: no desktop runs beside the benchmark; `nomodeset` keeps
   the firmware's screen, so the graphics card's driver doesn't matter).
   Press **F10** to boot.
3. At `ubuntu login:` type `ubuntu` and Enter. If it asks for a password,
   just press Enter (the live user has none).
4. Find the SanDisk (about 14.3G, one FAT partition; say `sdb1`), mount
   it, and run the program as root:

   ```sh
   lsblk -o NAME,SIZE,FSTYPE,LABEL
   sudo mount /dev/sdb1 /mnt
   cp /mnt/linuxbench /tmp/ && chmod +x /tmp/linuxbench
   sudo /tmp/linuxbench --dir /mnt
   ```

   It takes about a minute and prints its lines as it goes; the last one
   is `bench: done`. They are also in `/mnt/linuxbench-default.txt`. It
   sets the CPUs' governor to `performance` for the run (in memory only:
   a reboot undoes it; `--as-is` leaves it alone). It writes and deletes a
   3.4 MB scratch file on the SanDisk.

   No program on the stick? Build it here from `linuxbench-src` (step 2)
   instead; the PC needs its network cable for `apt`:

   ```sh
   sudo apt update && sudo apt install -y gcc libc6-dev
   cp -r /mnt/linuxbench-src /tmp/lb && cd /tmp/lb
   gcc -std=gnu17 -O2 -D_GNU_SOURCE -static -pthread -o /tmp/linuxbench *.c
   ```

5. Then:

   ```sh
   sudo umount /mnt
   sudo poweroff
   ```

   Ubuntu may ask you to remove the stick and press Enter.

## 5. Second boot: `mitigations=off`

The same as step 4, but at GRUB add:

```
nomodeset systemd.unit=multi-user.target mitigations=off
```

This run's file is `linuxbench-mitigations-off.txt` (the program reads
the word from the kernel's command line). Then unmount and power off as
before.

## 6. Bring them back

Plug the SanDisk into the Mac; the two files are at its top:
`linuxbench-default.txt` and `linuxbench-mitigations-off.txt` (a `-2` is
added rather than overwrite a file already there). Hand them to the
main session, which puts the numbers in BENCH.md.

The per-operation lines compare best on the same stick: in Jam OS, with
the SanDisk in, run `mount -w /usb0`, then `perop /usb0` (and `perop` for
the boot stick's `/data`), then `mount -r /usb0`.

## What each file holds

The header: the date, the CPU and its microcode, the measured TSC rate,
which CPUs played P, P2, HT and E (with their APIC ids; Jam OS's bench
chooses the same way, so they should be cpu2, cpu4, cpu3 and cpu16), the
kernel and its command line, every file of
`/sys/devices/system/cpu/vulnerabilities` (what Linux mitigates), the
governor before and during the run, the idle driver, the load and
whether a desktop was running. Then one line per measurement, and at the
end whether every thread got SCHED_FIFO (if not, it wasn't run as root
and the numbers don't compare).
