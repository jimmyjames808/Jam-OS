#!/bin/sh
# M12.7's QEMU test: drv/nvme on emulated NVMe drives: read-only on every
# drive that isn't Jam OS's (stage 1's cases), and writes to a Jam OS
# drive only (stage 2's `write` and `diskro`). Every case keeps a
# Windows-like drive attached (the PC-like image; `4k` and `damaged` its
# 4096-byte and broken-header forms) whose SHA-256 must be the same at the
# end.
# For each case it makes the drive image(s) (tools/nvme-img.py: a
# Windows-like GPT disk like the PC's, a second namespace, 4096-byte
# blocks, a damaged primary GPT), boots the shell with them attached
# (tools/qemu-test.sh's QEMU_NVME; the images are opened writable, so a
# write would land) with QEMU's trace of every NVMe command, and checks:
#   1. the image's SHA-256, taken before and after the run, is identical
#      (the guest wrote nothing);
#   2. the trace (tools/nvme-trace-check.py): every I/O opcode is Read
#      (02h) and every admin opcode is in the stage's list, whoever sent it
#      (OVMF's driver, then drv/nvme); an empty trace fails;
#   3. what the guest says it found, up to the LEVEL of the driver stack
#      that is built (below).
# Cases (default: windows; "all" runs every one in turn; run them in
# parallel with a separate <outdir> each):
#   windows  the PC-like disk: found, listed (4 partitions), the ESP mounted
#            at /nvme0 read-only and a file on it read (its SHA-256 the
#            host's), `disk digest`'s region lines equal
#            tools/disk-regions.py's on the image, `mount -w` refused
#   twons    two namespaces: the second is logged "not served"
#   4k       4096-byte blocks: listed and digested, not mounted (fat is 512 only)
#   damaged  the primary GPT header broken: read from the backup, listed as damaged
#   kill     drv/nvme killed right after a read: its mounts go with it (volumes: "its driver
#            ended"), then devmgr's restart is attached again and /nvme0 and the file are back
#   iommu    the windows case behind QEMU's VT-d (QEMU_IOMMU=1, iommu=on)
#   plain    the windows case booted with no nvmetest= word at all, as the PC's
#            everyday entry boots (devmgr binds drv/nvme on every boot since
#            M12.7's N4): the same checks
#   block    the windows disk under utest's NVMe cases (user/tests/utest/
#            nvme.c: storage, block, 32 reads in flight, a client that leaves
#            with reads out), and their storage.digest region lines equal
#            tools/disk-regions.py's on the image (from level 2 on)
#   write    stage 2: the Windows-like drive and a blank one (640 MiB), with
#            utest's write cases straight at the driver (`utest only
#            nvmew_`, user/tests/utest/nvmewrite.c: volumes bypassed, so
#            every refusal is the driver's): every writable open, write and
#            create_table on the first refused; the second prepared
#            (create_table), written and read back. Then a second boot of
#            the same images: the driver finds the second drive Jam OS's
#            from its own table, create_table refused, the first boot's
#            blocks there, written again. After each boot: the Windows-like
#            image's hash unchanged; the Jam OS image's table right, its
#            written blocks in their places and nothing outside its table and
#            partitions (tools/nvme-written.py); QEMU's trace holds stage 2's
#            opcodes, and every Write lies inside the Jam OS drive's table or
#            partitions (`-trace pci_nvme_write`, --write-ranges)
#   wiommu   the write case behind QEMU's VT-d (QEMU_IOMMU=1, iommu=on): the
#            device reads what it writes through the pins, and no fault
#   diskro   the Windows-like drive, a Jam OS one (tools/mkgpt.py's layout)
#            and a blank one, booted with `diskro`: every drive refuses a
#            writable open and create_table (`utest only nvmero_`), the
#            trace holds stage 1's list only (no Write, no Flush), and all
#            three images are unchanged
#   prepare  stage 2's volumes, init and shell (M12.7 W2): the Windows-like
#            drive and a blank one (640 MiB); `disk prepare` of the
#            Windows-like one shows it isn't empty (its GPT, its partitions)
#            and asks for its erase: the serial's end typed wrong, nothing
#            written; of the blank one with the serial's last four
#            characters typed wrong (stopped: the shell says so and the
#            drive is still blank), then right: the table,
#            both partitions formatted: the ESP FAT32, read-only (/nvme1),
#            the data partition ext (M13.6's U7b), read-write at /jam, and
#            /tmp a view of it; `disks` shows a Jam OS disk; `mount -w
#            /nvme0` refused; `disk prepare` again says it is one
#            already; a file written to /jam. Then the
#            Windows-like image is unchanged, the blank one's table is Jam
#            OS's (tools/nvme-written.py), its ESP a FAT volume (mtools),
#            its data partition an ext volume e2fsck finds clean holding the
#            file, every traced Write lies inside the Jam OS drive's table
#            or partitions
#   piommu   the prepare case behind QEMU's VT-d
#   cutprep  the blank drive cut after create_table's first write (tools/
#            mkgpt.py --cut backup-array: only the backup entry array is
#            there): the driver calls it blank (create_table may write), and
#            `disk prepare nvme1` finishes it, with the checks of `prepare`
#   cutbad   the same drive with one stray byte in the array's zeros: not
#            empty, so `disk prepare` shows it as data Jam OS doesn't
#            recognise and asks for its erase: the serial's end typed right,
#            then not `erase`: nothing written, both images unchanged, no
#            Write in the trace
#   cuterase an erase of a GPT drive cut after create_table's primary
#            entry array (tools/mkgpt.py --layout jamos --break
#            primary-array: the old primary header, whose array CRC is now
#            wrong, and Jam OS's backup; both partitions hold the erase's
#            zeros): volumes' mark reads Jam OS's from the backup, the
#            driver calls it someone else's, so `disk prepare` shows it as
#            not empty and asks for its erase again (R3 stage 3's S2), and
#            the serial's end and `erase` make it a Jam OS disk, with the
#            checks of `prepare`
#   erase    a drive that isn't empty erased (the owner's decision,
#            2026-10-14): two Windows-like drives; the second's erase asked
#            and answered wrong; the first shown (GPT, 4 partitions, the
#            ESP's FAT volume), its serial's end and `erase` typed: erased
#            and prepared, a file written to its data partition (/jam). Its
#            image: Jam OS's table, every block that differs from the image
#            as made inside the table or the partitions, the new ESP without
#            the old one's files, the file in the ext volume (e2fsck clean);
#            the trace's Writes inside the table and partitions; the second
#            image unchanged
#   erasestop  the same drives, the first never erased: the serial's end
#            wrong; right, then `yes`; right, then Ctrl+C. `disk digest` of
#            its first 34 blocks the same before and after (and the host's),
#            `disk prepare usb0` refused (the disk Jam OS booted from); both
#            images unchanged, no Write in the trace
#   jam      M13.6's U7a: a Jam OS drive whose data partition is an ext volume
#            (made by Linux's mke2fs, Jam OS's feature set, on the test PC)
#            beside the Windows-like drive: volumes starts bin/extfs on it,
#            /jam read-write; init mounts /usr, /var, /home and /tmp as
#            views of it (/tmp in memory until /jam comes: U7b) and empties
#            the old /tmp; files go in and come back through both names;
#            `disks` says (ext); then `install` over the drive (its data
#            partition held writable for init and let go: /jam and its
#            views gone and back, /tmp in memory meanwhile); afterwards
#            e2fsck finds the volume clean (its journal replayed first) with
#            the files in it, the ESP holds the build, and the Windows-like
#            image is unchanged
#   jamro    M13.6's U7b, R3 stage 3's question 2 and design question 3: the
#            jam case's drive with one byte of its primary entry array
#            changed (tools/mkgpt.py --break primary-array): the driver reads
#            it as someone else's, volumes' mark as Jam OS's. /jam is
#            mounted, read-only (not lost: a writable open the driver
#            refuses), its file read, a write refused, /tmp in memory; then
#            `disk prepare` shows it as not empty (its two volumes, /jam
#            among them) and asks for its erase: the serial's end and `no`
#            leave the image unchanged; the serial's end and `erase` make it
#            a Jam OS disk again: /jam read-write and empty, a file written,
#            e2fsck clean on the host
#   format   M13.6's U7b, `disk format <disk> data [fat]`: a Jam OS drive as
#            M12.7's W2 made it (a FAT32 data partition, /nvmeN read-only,
#            a file on it): the serial's end typed wrong (nothing written),
#            then right: the data partition is ext at /jam, read-write,
#            without the old file, /tmp a view of it; a file written; then
#            `disk format ... data fat`: FAT32 at /nvmeN, read-only, /tmp in
#            memory again. On the host: the ESP as it was, the data partition
#            a clean FAT volume (tools/fatcheck.py), every traced Write
#            inside the data partition
#   checker  no QEMU: tools/nvme-trace-check.py on made-up traces (a clean
#            one passes; a Write, a Flush, a Format NVM, a Dataset Management,
#            an unparsable line and a silent trace each fail in stage 1; in
#            stage 2 a Write outside the ranges, or one whose place isn't
#            traced, fails), and tools/disk-regions.py refusing a file
#            without --image ("all" runs it first)
# LEVEL (JAMOS_NVME_LEVEL; by default what the tree has):
#   0  no driver yet: the drive is on the PCI bus (class NVMe), nothing
#      sent it a write, the trace is clean (the firmware's own reads and
#      admin commands)
#   1  drivers/nvme (N1): the controller line is logged, Identify worked,
#      and the boot word nvmetest=selftest makes it read (nothing else
#      reads yet): the namespace's ends are hashed
#   2  + the partition library and serving storage (N3): "GPT, 4
#      partitions, read-only"; the `block` case
#   3  + volumes and the shell (N4: the shell's `disks`): disks, /nvme0,
#      disk digest, mount -w, disk smart, as the plan's P1 types them
# The log lines it greps for are the plan's examples (docs/M12.7-PLAN.md,
# "The design"); when a driver's wording differs, change the patterns in
# this file's L1, L2 and L3 functions (and the typed commands in the
# generated script), nothing else.
# A QEMU other than the PC's 10.0.2 is fine but its trace event format may
# differ: the checker fails on a silent or unparsable trace, never passes it.
# QEMU_TIMEOUT, QEMU_SMP pass through. Nothing here writes to anything but
# files under <outdir>.
# Usage: tools/nvme-test.sh <outdir> [case...]; exit 0 on PASS.
set -eu
out=$1
shift
cases=${*:-windows}
[ "$cases" = all ] && cases="checker windows plain twons 4k damaged kill iommu block write wiommu diskro prepare piommu cutprep cutbad cuterase erase erasestop jam jamro format"
mkdir -p "$out"

sha256() { if command -v sha256sum > /dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }

level=${JAMOS_NVME_LEVEL:-}
if [ -z "$level" ]; then
    level=0
    [ -f drivers/nvme/main.c ] && level=1
    [ $level = 1 ] && [ -f drivers/lib/parttable.c ] && level=2
    [ $level = 2 ] && grep -qs '"disks"' user/services/shell/sh_table.c && level=3
fi

fails=0
checker() {
    name=nvme-checker
    dir="$out/nvme-checker"
    rm -rf "$dir"
    mkdir -p "$dir"
    ok=1
    io() { echo "pci_nvme_io_cmd cid 7 nsid 0x1 sqid 1 opc 0x$1 opname 'X'"; }
    adm() { echo "pci_nvme_admin_cmd cid 7 sqid 0 opc 0x$1 opname 'X'"; }
    { adm 6; adm 5; adm 1; adm 9; adm 2; adm 8; adm 4; adm 0; io 2; io 2; } > "$dir/clean.txt"
    python3 tools/nvme-trace-check.py --require-io 02 --require-admin 06 "$dir/clean.txt" > "$dir/clean.out" ||
        { echo "$name: FAIL: a clean trace failed"; ok=0; }
    # Write, Flush, Dataset Management, Write Zeroes, Format NVM, Security
    # Send, Sanitize, Firmware Commit, Namespace Management, Get Features,
    # Async Event Request
    for bad_one in "io 1" "io 0" "io 9" "io 8" "adm 80" "adm 81" "adm 84" "adm 10" "adm d" "adm c" "adm a"; do
        { adm 6; io 2; $bad_one; } > "$dir/bad.txt"
        if python3 tools/nvme-trace-check.py "$dir/bad.txt" > "$dir/bad.out"; then
            echo "$name: FAIL: '$bad_one' passed the check"
            ok=0
        fi
    done
    { adm 6; echo "pci_nvme_io_cmd cid 7 something new"; } > "$dir/bad.txt"
    ! python3 tools/nvme-trace-check.py "$dir/bad.txt" > "$dir/bad.out" ||
        { echo "$name: FAIL: an unparsable command line passed"; ok=0; }
    : > "$dir/bad.txt"
    ! python3 tools/nvme-trace-check.py "$dir/bad.txt" > "$dir/bad.out" ||
        { echo "$name: FAIL: a silent trace passed"; ok=0; }
    { adm 6; } > "$dir/bad.txt"
    ! python3 tools/nvme-trace-check.py --require-io 02 "$dir/bad.txt" > "$dir/bad.out" ||
        { echo "$name: FAIL: a trace without the Read the test requires passed"; ok=0; }
    # Stage 2: Writes inside the ranges pass; one outside, or one whose
    # place the trace doesn't say, fails.
    wr() { echo "pci_nvme_write cid 7 opname 'NVME_NVM_CMD_WRITE' nsid 1 nlb $2 len 512 lba $1"; }
    { adm 6; io 2; io 1; wr 0x0 1; io 1; wr 0x800 8; io 0; } > "$dir/w.txt"
    python3 tools/nvme-trace-check.py --stage 2 --write-ranges 0-34,2048-4096 "$dir/w.txt" \
        > "$dir/w.out" || { echo "$name: FAIL: Writes inside the ranges failed"; ok=0; }
    { adm 6; io 1; wr 0x7fc 8; } > "$dir/bad.txt"
    ! python3 tools/nvme-trace-check.py --stage 2 --write-ranges 0-34,2048-4096 "$dir/bad.txt" \
        > "$dir/bad.out" || { echo "$name: FAIL: a Write across a range's edge passed"; ok=0; }
    { adm 6; io 1; io 1; wr 0x0 1; } > "$dir/bad.txt"
    ! python3 tools/nvme-trace-check.py --stage 2 --write-ranges 0-34 "$dir/bad.txt" \
        > "$dir/bad.out" || { echo "$name: FAIL: a Write with no place traced passed"; ok=0; }
    { adm 6; io 1; wr 0x0 1; } > "$dir/bad.txt"
    ! python3 tools/nvme-trace-check.py "$dir/bad.txt" > "$dir/bad.out" ||
        { echo "$name: FAIL: a Write passed stage 1"; ok=0; }
    python3 tools/nvme-img.py "$dir/f.img" small
    ! python3 tools/disk-regions.py "$dir/f.img" > "$dir/bad.out" 2>&1 ||
        { echo "$name: FAIL: disk-regions.py read a file without --image"; ok=0; }
    rm -f "$dir/f.img"
    if [ $ok = 1 ]; then echo "$name: PASS"; else fails=$((fails + 1)); fi
}
# ---- stage 2: write and diskro ------------------------------------------------------------

# s2_boot <dir> <name> <utest prefix> <words> <images>: one shell boot that
# runs `utest only <prefix>` with the drives attached and every command
# traced, the Writes with their blocks.
s2_boot() {
    d=$1 bn=$2
    {
        echo "# generated by tools/nvme-test.sh: $bn"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        # The tests need /svc/devmgr-ctl, which init publishes (and tells
        # the shell) only after devmgr's first binding pass: on a fast boot
        # the prompt comes first, and a utest started then skips every case
        # ("no devmgr control channel"). /data is mounted after that pass.
        echo "seen 60 init: /data mounted"
        echo "send utest only $3; echo nvme-utest-\$?"
        echo "wait 240 nvme-utest-"
        echo "wait {prompt}"
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$d/$bn.in"
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-240} QEMU_NVME="$5" QEMU_INPUT="$d/$bn.in" \
        QEMU_EXTRA="-trace pci_nvme_pci_reset -trace pci_nvme_admin_cmd -trace pci_nvme_io_cmd -trace pci_nvme_write -D $d/$bn.trace" \
        QEMU_WORDS="$4" QEMU_IOMMU=${s2_iommu:-} tools/qemu-test.sh "$d" "$bn" shell > "$d/$bn.out" 2>&1 ||
        { echo "$bn: FAIL: the run failed (see $d/$bn.out)"; grep -a "serial-feed: .*no '" "$d/$bn.out" || true; return 1; }
}

# s6_ext <image> <prefix> [<file> <want>]: the data partition of a Jam OS
# drive's image (from 513 MiB: the jamos layout's) copied to <prefix>.ext
# is an ext volume e2fsck finds clean once its journal is replayed (the
# replay alone, -E journal_only, then -fn with nothing to fix: the run ends
# with `reboot -f`, so the journal may hold the last transactions), holding
# <file> equal to the host's <want>. A FAIL line and 1 otherwise. Needs
# e2fsprogs (the test PC's WSL has it).
s6_ext() {
    PATH=/usr/sbin:/sbin:$PATH
    command -v e2fsck > /dev/null 2>&1 || { echo "nvme-$c: FAIL: no e2fsck here"; return 1; }
    dd if="$1" of="$2.ext" bs=1M skip=513 2> /dev/null
    e2fsck -E journal_only -y "$2.ext" > "$2.e2fsck-1.txt" 2>&1 || [ $? -le 1 ] ||
        { echo "nvme-$c: FAIL: e2fsck's journal replay (see $2.e2fsck-1.txt)"; return 1; }
    e2fsck -fn "$2.ext" > "$2.e2fsck-2.txt" 2>&1 ||
        { echo "nvme-$c: FAIL: the ext volume isn't clean after its journal was replayed (see $2.e2fsck-2.txt)"; return 1; }
    if [ -n "${3:-}" ]; then
        debugfs -R "cat /$3" "$2.ext" > "$2.got" 2> /dev/null
        cmp -s "$2.got" "$4" || { echo "nvme-$c: FAIL: /$3 isn't in the ext volume as written"; return 1; }
    fi
    rm -f "$2.ext" "$2.got"
}

# s2_want <log> <regex>...: each line there, or a FAIL line and 1.
s2_want() {
    f=$1
    shift
    r=0
    for re in "$@"; do
        grep -aqE -- "$re" "$f" || { echo "nvme-$c: FAIL: no line matching '$re' in $f"; r=1; }
    done
    return $r
}

# s2_trace <trace> <stage> [--write-ranges ...]: the trace's opcodes (and Writes).
s2_trace() {
    t=$1 stage=$2
    shift 2
    grep -aq "^pci_nvme_pci_reset" "$t" 2> /dev/null || { echo "nvme-$c: FAIL: the trace is dead: $t"; return 1; }
    python3 tools/nvme-trace-check.py --stage "$stage" --require-admin 06 --require-io 02 "$@" "$t" \
        > "$t.check" || { echo "nvme-$c: FAIL: the trace (see $t.check)"; return 1; }
    grep "^nvme-trace: [0-9]" "$t.check" | sed "s/^/nvme-$c: /"
}

s2_write() {
    dir=$1 ok=1 words="nvmetest=on${s2_iommu:+ iommu=on}"
    win="$dir/windows.img" jam="$dir/jamos.img"
    python3 tools/nvme-img.py "$win" windows
    python3 tools/mkgpt.py "$jam" --layout blank --size 640M > /dev/null
    before=$(sha256 "$win" | cut -d' ' -f1)
    for run in 1 2; do
        bn=nvme-write-$run log="$dir/nvme-write-$run.log"
        s2_boot "$dir" $bn nvmew_ "$words" "$win $jam" || ok=0
        if [ -n "${s2_iommu:-}" ] && grep -aq "vtd: fault" "$log"; then echo "nvme-$c: FAIL: a VT-d fault"; ok=0; fi
        [ "$(sha256 "$win" | cut -d' ' -f1)" = "$before" ] || { echo "nvme-$c: FAIL: run $run: the Windows-like image changed"; ok=0; }
        s2_want "$log" "nvme-utest-0" "utest: 4 passed" \
            "nvmew_refuse: drive [0-9]+ \(someone else's, 4 partitions\): every write refused" \
            "nvmew_write: drive [0-9]+: 288 blocks written in both partitions and read back" \
            "GPT, 4 partitions, read-only" || ok=0
        if [ $run = 1 ]; then
            s2_want "$log" "blank, blank: create_table may write its table" \
                "nvmew_prepare: drive [0-9]+ is Jam OS's now" "nvmew_check: drive [0-9]+: nothing written yet" \
                "create_table: Jam OS's table written and read back: GPT, 2 partitions, a Jam OS disk" || ok=0
        else
            s2_want "$log" "GPT, 2 partitions, a Jam OS disk: its two partitions writable" \
                "nvmew_prepare: drive [0-9]+ is Jam OS's already: create_table refused" \
                "nvmew_check: drive [0-9]+: the blocks written by an earlier boot are there" || ok=0
        fi
        python3 tools/nvme-written.py "$jam" --pattern all --ranges > "$dir/written-$run.txt" ||
            { echo "nvme-$c: FAIL: run $run: $(tail -1 "$dir/written-$run.txt")"; ok=0; continue; }
        head -1 "$dir/written-$run.txt" | sed "s/^/nvme-$c: run $run: /"
        s2_trace "$dir/$bn.trace" 2 --write-ranges "$(tail -1 "$dir/written-$run.txt")" || ok=0
    done
    if [ $ok = 1 ]; then rm -f "$win" "$jam"; echo "nvme-$c: PASS"; else echo "nvme-$c: FAIL (see $dir)"; fails=$((fails + 1)); fi
}

s2_diskro() {
    dir=$1 ok=1
    win="$dir/windows.img" jam="$dir/jamos.img" blank="$dir/blank.img"
    python3 tools/nvme-img.py "$win" windows
    python3 tools/mkgpt.py "$jam" --layout jamos --size 640M > /dev/null
    python3 tools/mkgpt.py "$blank" --layout blank --size 640M > /dev/null
    sums=$(sha256 "$win" "$jam" "$blank" | cut -d' ' -f1)
    log="$dir/nvme-diskro.log"
    s2_boot "$dir" nvme-diskro nvmero_ "nvmetest=on diskro" "$win $jam $blank" || ok=0
    [ "$(sha256 "$win" "$jam" "$blank" | cut -d' ' -f1)" = "$sums" ] || { echo "nvme-diskro: FAIL: an image changed"; ok=0; }
    s2_want "$log" "nvme-utest-0" "utest: 1 passed" "nvmero_refuse: drive [0-9]+ \(someone else's\): read-only" \
        "nvmero_refuse: drive [0-9]+ \(blank\): read-only" "nvmero_refuse: drive [0-9]+ \(Jam OS's\): read-only" \
        "the drive is read-only \(the ro word\)" || ok=0
    [ "$(grep -ac 'the drive is read-only (the ro word)' "$log")" -ge 3 ] || { echo "nvme-diskro: FAIL: not every drive said ro"; ok=0; }
    s2_trace "$dir/nvme-diskro.trace" 1 || ok=0
    if [ $ok = 1 ]; then rm -f "$win" "$jam" "$blank"; echo "nvme-diskro: PASS"; else echo "nvme-diskro: FAIL (see $dir)"; fails=$((fails + 1)); fi
}

# ---- stage 2: disk prepare (volumes, init, the shell) ------------------------------------

# s3_names <slot0 var> <slot1 var>: `disks`, and from its answer the disk
# numbers of the drives in QEMU's slots 0 and 1 (QEMU_NVME's order: serial
# jamos-nvme0 and jamos-nvme1) as {<var>}; which is nvme0 depends on which
# driver answered volumes first. Waits a moment first: the prompt is up
# before every driver has answered volumes.
s3_names() {
    echo "send sleep 4"
    echo "wait {prompt}"
    echo "send disks"
    printf '%s\n' "seen re:nvme(?P<$1>[0-9]+) +QEMU NVMe Ctrl +serial jamos-nvme0 "
    printf '%s\n' "seen re:nvme(?P<$2>[0-9]+) +QEMU NVMe Ctrl +serial jamos-nvme1 "
    echo "wait {prompt}"
}

# s3_prepare <dir> <kind>: a blank 640 MiB drive (QEMU's slot 0: serial
# jamos-nvme0, its end vme0) and the Windows-like one (slot 1: vme1),
# booted with the shell. kind: prepare (the blank drive), cutprep (cut
# after create_table's first write), cutbad (that, and a stray byte: not
# empty), cuterase (an erase cut after the primary array: erased again).
# The Windows-like drive is shown as not empty and its erase asked
# for each time, and answered wrong (nothing written); the blank one is
# prepared (the wrong end of its serial first in `prepare`); the stray
# byte's drive is shown as not empty too, its serial's end typed right and
# then not `erase` (nothing written). The mounts are in order: Windows'
# ESP is /nvme0 from the boot, the new disk's ESP and data partition
# /nvme1 and /nvme2. What each step printed is counted in the log
# afterwards.
s3_prepare() {
    dir=$1 kind=$2 ok=1 words="nvmetest=on${s2_iommu:+ iommu=on}"
    win="$dir/windows.img" jam="$dir/jamos.img" log="$dir/nvme-$c.log"
    python3 tools/nvme-img.py "$win" windows
    case $kind in
    prepare) python3 tools/mkgpt.py "$jam" --layout blank --size 640M > /dev/null ;;
    cutprep) python3 tools/mkgpt.py "$jam" --layout jamos --size 640M --cut backup-array > /dev/null ;;
    cutbad) python3 tools/mkgpt.py "$jam" --layout jamos --size 640M --cut backup-array --poke -16596 > /dev/null ;;
    cuterase) python3 tools/mkgpt.py "$jam" --layout jamos --size 640M --break primary-array > /dev/null ;;
    esac
    before=$(sha256 "$win" "$jam" | cut -d' ' -f1)
    in="$dir/nvme-$c.in"
    wrong=1   # the Windows-like drive's wrong answer, every kind
    [ $kind = prepare ] && wrong=2
    {
        echo "# generated by tools/nvme-test.sh: $c"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "seen 60 init: /data mounted"   # volumes is up, and has been told of the disks
        s3_names bd wd
        echo "send disks"
        if [ $kind = cuterase ]; then
            echo "wait Jam OS data"
        else
            echo "wait nothing found on it"
        fi
        echo "wait {prompt}"
        # Windows' drive: shown, its erase asked for, the wrong end of its serial typed.
        echo "send disk prepare nvme{wd}"
        echo "wait It is NOT empty"
        echo "wait a GPT partition table with 4 partitions"
        echo "wait EVERYTHING ON nvme{wd} WILL BE LOST"
        echo "wait Type the last 4 characters"
        echo "send zzzz"
        echo "wait is not the end of"
        echo "wait {prompt}"
        if [ $kind = prepare ]; then
            echo "send disk prepare nvme{bd}"
            echo "wait Type the last 4 characters"
            echo "send zzzz"
            echo "wait is not the end of"
            echo "wait {prompt}"
            echo "send disks"
            echo "wait nothing found on it"
            echo "wait {prompt}"
        fi
        echo "send disk prepare nvme{bd}"
        if [ $kind = cutbad ]; then
            echo "wait It is NOT empty"
            echo "wait data Jam OS doesn't recognise"
            echo "wait Type the last 4 characters"
            echo "send vme0"
            echo "wait type erase"
            echo "send no"
            echo "wait not erased: nothing was written"
            echo "wait {prompt}"
            echo "send disks"
            echo "wait nothing found on it"
            echo "wait {prompt}"
        else
            if [ $kind = cuterase ]; then
                echo "wait It is NOT empty"
                echo "wait Type the last 4 characters"
                echo "send vme0"
                echo "wait type erase"
                echo "send erase"
            else
                echo "wait nothing is on it"
                echo "wait Type the last 4 characters"
                echo "send vme0"
            fi
            echo "wait 300 is a Jam OS disk now: ESP"
            echo "wait {prompt}"
            echo "send disks"
            echo "wait Jam OS disk"
            echo "wait Jam OS data"
            echo "wait {prompt}"
            # the ESP read-only at /nvme1, the data partition ext at /jam, /tmp a view of it
            echo "seen 60 init: /tmp: a view of /jam/tmp"
            echo "send T=x"
            echo "wait {prompt}"
            echo "send mount | grep /jam | grep -c read-write | grep ^1\$ && mount | grep /nvme1 | grep -c read-only | grep ^1\$ && echo ok-\$T-mounts"
            echo "wait ok-x-mounts"
            echo "send disks | grep -c \"/jam (ext)\" | grep ^1\$ && echo ok-\$T-ext"
            echo "wait ok-x-ext"
            echo "send disk prepare nvme{bd}"
            echo "wait is a Jam OS disk already"
            echo "wait {prompt}"
            echo "send mount -w /nvme0"
            echo "wait /nvme0 is not on a Jam OS disk: it stays read-only"
            echo "wait {prompt}"
            echo "send cp /boot/build.txt /jam/build.txt; sync; sha256sum /jam/build.txt"
            echo "wait $(sha256 build/build.txt | cut -d' ' -f1)"
            echo "wait {prompt}"
            echo "send write /tmp/t.txt in-tmp-\$T && cat /jam/tmp/t.txt"
            echo "wait in-tmp-x"
            echo "wait {prompt}"
            echo "send sync"
            echo "wait {prompt}"
        fi
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$in"
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-420} QEMU_NVME="$jam $win" QEMU_INPUT="$in" \
        QEMU_EXTRA="-trace pci_nvme_pci_reset -trace pci_nvme_admin_cmd -trace pci_nvme_io_cmd -trace pci_nvme_write -D $dir/$c.trace" \
        QEMU_WORDS="$words" QEMU_IOMMU=${s2_iommu:-} tools/qemu-test.sh "$dir" "nvme-$c" shell > "$dir/nvme-$c.out" 2>&1 ||
        { echo "nvme-$c: FAIL: the run failed (see $dir/nvme-$c.out)"; grep -a "serial-feed: .*no '" "$dir/nvme-$c.out" || true; ok=0; }
    [ "$(sha256 "$win" | cut -d' ' -f1)" = "$(echo "$before" | sed -n 1p)" ] || { echo "nvme-$c: FAIL: the Windows-like image changed"; ok=0; }
    if [ -n "${s2_iommu:-}" ] && grep -aq "vtd: fault" "$log"; then echo "nvme-$c: FAIL: a VT-d fault"; ok=0; fi
    [ "$(grep -ac "that is not the end of" "$log")" -eq "$wrong" ] ||
        { echo "nvme-$c: FAIL: a wrong serial was taken, or not stopped ($wrong expected)"; ok=0; }
    if [ $kind = cuterase ]; then
        s2_want "$log" "volumes: nvme[01] is not empty: erasing it, as the owner confirmed" || ok=0
    elif grep -aq "erasing it, as the owner confirmed" "$log"; then
        echo "nvme-$c: FAIL: a drive was erased"
        ok=0
    fi
    case $kind in
    cutbad)
        [ "$(sha256 "$jam" | cut -d' ' -f1)" = "$(echo "$before" | sed -n 2p)" ] || { echo "nvme-$c: FAIL: the refused drive's image changed"; ok=0; }
        [ "$(grep -ac "the drive is read-only" "$log")" -ge 2 ] ||
            { echo "nvme-$c: FAIL: the stray byte's drive wasn't called someone else's"; ok=0; }
        grep -aq "the drive is blank" "$log" && { echo "nvme-$c: FAIL: a drive with a stray byte was called blank"; ok=0; }
        grep -aq "is a Jam OS disk now" "$log" && { echo "nvme-$c: FAIL: a disk was made Jam OS's"; ok=0; }
        s2_want "$log" "not erased: nothing was written" || ok=0
        s2_trace "$dir/$c.trace" 1 || ok=0
        ;;
    *)
        if [ $kind = cutprep ]; then
            s2_want "$log" "blank: create_table may write its table" || ok=0
        fi
        s2_want "$log" "create_table: Jam OS's table written and read back: " \
            "volumes: nvme[01] partition 2 is blank: bin/extfs makes an ext volume on it" \
            "volumes: nvme[01] is a Jam OS disk now: /nvme1 \(FAT\), read-only and /jam \(ext\), read-write" \
            "nvme[01] is a Jam OS disk already" || ok=0
        if python3 tools/nvme-written.py "$jam" --pattern none --ranges > "$dir/written.txt"; then
            head -1 "$dir/written.txt" | sed "s/^/nvme-$c: /"
            s2_trace "$dir/$c.trace" 2 --write-ranges "$(tail -1 "$dir/written.txt")" || ok=0
        else
            echo "nvme-$c: FAIL: $(tail -1 "$dir/written.txt")"
            ok=0
        fi
        # The ESP is an empty FAT volume, the data partition an ext volume with the file.
        if command -v mdir > /dev/null 2>&1; then
            mdir -i "$jam@@1M" :: > "$dir/esp.txt" 2>&1 || { echo "nvme-$c: FAIL: the ESP isn't a FAT volume"; ok=0; }
        else
            echo "nvme-$c: (no mtools: the ESP not read on the host)"
        fi
        s6_ext "$jam" "$dir/data" build.txt build/build.txt || ok=0
        ;;
    esac
    if [ $ok = 1 ]; then rm -f "$win" "$jam"; echo "nvme-$c: PASS"; else echo "nvme-$c: FAIL (see $dir)"; fails=$((fails + 1)); fi
}

# s4_erase <dir> <kind>: two Windows-like drives (tools/nvme-img.py's, the
# same bytes): the one in QEMU's slot 0 (serial jamos-nvme0, its end vme0)
# is the one `disk prepare` is asked to erase, the one in slot 1 (vme1)
# must come out unchanged. kind:
#   erase      the other drive asked first, the wrong end of its serial
#              typed (it isn't erased); then the first shown (its GPT, 4
#              partitions, the ESP's FAT volume), its serial's end and
#              `erase` typed: erased, prepared, both partitions formatted
#              and mounted, a file written. On the host: its table is Jam
#              OS's, every block that differs from the image as made lies in
#              the table or the partitions (tools/nvme-written.py --before),
#              the new ESP holds none of the old one's files, every traced
#              Write lies in the table or the partitions; the other image's
#              hash unchanged
#   erasestop  the first drive asked three times and never erased: the
#              wrong end of its serial; the right end, then `yes`; the
#              right end, then Ctrl+C. Its first 34 blocks hashed in the
#              guest before and after (`disk digest`: the same), `disk
#              prepare usb0` refused (the boot disk), `mount -w` of its ESP
#              refused; on the host both images unchanged, and the trace
#              holds no Write at all
s4_erase() {
    dir=$1 kind=$2 ok=1 words="nvmetest=on"
    era="$dir/erase.img" keep="$dir/keep.img" log="$dir/nvme-$c.log"
    python3 tools/nvme-img.py "$era" windows
    python3 tools/nvme-img.py "$keep" windows
    before=$(sha256 "$era" "$keep" | cut -d' ' -f1)
    boot_hash=$(sha256 "$era.files/bootmgfw.efi" | cut -d' ' -f1)
    in="$dir/nvme-$c.in"
    {
        echo "# generated by tools/nvme-test.sh: $c"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "seen 60 init: /data mounted"
        s3_names ad kd
        # each drive's ESP mount ({ae}, {ke}), from the same `disks`
        printf '%s\n' "seen re:serial jamos-nvme0 [^\r\n]*\r?\n +1 +EFI system [^\r\n]*?/nvme(?P<ae>[0-9]+)"
        printf '%s\n' "seen re:serial jamos-nvme1 [^\r\n]*\r?\n +1 +EFI system [^\r\n]*?/nvme(?P<ke>[0-9]+)"
        echo "send sha256sum /nvme{ae}/EFI/Microsoft/Boot/bootmgfw.efi"
        echo "wait $boot_hash"
        echo "wait {prompt}"
        if [ $kind = erasestop ]; then
            echo "send disk digest nvme{ad} 0 34"
            echo "wait range lba 0 count 34 sha256"
            echo "wait {prompt}"
            echo "send disk prepare nvme{ad}"
            echo "wait It is NOT empty"
            echo "wait Type the last 4 characters"
            echo "send zzzz"
            echo "wait is not the end of"
            echo "wait {prompt}"
            echo "send disk prepare nvme{ad}"
            echo "wait Type the last 4 characters"
            echo "send vme0"
            echo "wait type erase"
            echo "send yes"
            echo "wait not erased: nothing was written"
            echo "wait {prompt}"
            echo "send disk prepare nvme{ad}"
            echo "wait Type the last 4 characters"
            echo "send vme0"
            echo "wait type erase"
            echo "type er\\x03"
            echo "wait not erased: nothing was written"
            echo "wait {prompt}"
            echo "send disk prepare usb0"
            echo "wait is the disk Jam OS booted from"
            echo "wait {prompt}"
            echo "send mount -w /nvme{ae}"
            echo "wait not on a Jam OS disk"
            echo "wait {prompt}"
            echo "send disk digest nvme{ad} 0 34"
            echo "wait range lba 0 count 34 sha256"
            echo "wait {prompt}"
        else
            echo "send disk prepare nvme{kd}"
            echo "wait It is NOT empty"
            echo "wait Type the last 4 characters"
            echo "send vme0"
            echo "wait is not the end of"
            echo "wait {prompt}"
            echo "send disk prepare nvme{ad}"
            echo "wait It is NOT empty"
            echo "wait a GPT partition table with 4 partitions"
            echo "wait EFI system partition"
            echo "wait FAT volume \"SYSTEM\""
            echo "wait mounted read-only at /nvme{ae}"
            echo "wait Microsoft reserved"
            echo "wait Windows recovery"
            echo "wait EVERYTHING ON nvme{ad} WILL BE LOST"
            echo "wait Type the last 4 characters"
            echo "send vme0"
            echo "wait type erase"
            echo "send erase"
            echo "wait 300 is a Jam OS disk now: ESP"
            echo "wait {prompt}"
            echo "send disks"
            printf '%s\n' "seen re:serial jamos-nvme0 [^\r\n]*\r?\n +1 +EFI system [^\r\n]*?/nvme(?P<je>[0-9]+)[^\r\n]*\r?\n +2 +Jam OS data [^\r\n]*?/jam \(ext\)"
            echo "wait {prompt}"
            echo "send disk prepare nvme{ad}"
            echo "wait is a Jam OS disk already"
            echo "wait {prompt}"
            echo "send ls /nvme{je}; echo esp-ls-\$?"
            echo "wait esp-ls-0"
            echo "wait {prompt}"
            echo "send cp /boot/build.txt /jam/build.txt; sync; sha256sum /jam/build.txt"
            echo "wait $(sha256 build/build.txt | cut -d' ' -f1)"
            echo "wait {prompt}"
        fi
        # the other drive, still mounted and read as it was
        echo "send sha256sum /nvme{ke}/EFI/Microsoft/Boot/bootmgfw.efi"
        echo "wait $boot_hash"
        echo "wait {prompt}"
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$in"
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-420} QEMU_NVME="$era $keep" QEMU_INPUT="$in" \
        QEMU_EXTRA="-trace pci_nvme_pci_reset -trace pci_nvme_admin_cmd -trace pci_nvme_io_cmd -trace pci_nvme_write -D $dir/$c.trace" \
        QEMU_WORDS="$words" tools/qemu-test.sh "$dir" "nvme-$c" shell > "$dir/nvme-$c.out" 2>&1 ||
        { echo "nvme-$c: FAIL: the run failed (see $dir/nvme-$c.out)"; grep -a "serial-feed: .*no '" "$dir/nvme-$c.out" || true; ok=0; }
    [ "$(sha256 "$keep" | cut -d' ' -f1)" = "$(echo "$before" | sed -n 2p)" ] || { echo "nvme-$c: FAIL: the other Windows-like image changed"; ok=0; }
    if [ $kind = erasestop ]; then
        [ "$(sha256 "$era" | cut -d' ' -f1)" = "$(echo "$before" | sed -n 1p)" ] || { echo "nvme-$c: FAIL: the image not erased changed"; ok=0; }
        [ "$(grep -ac "that is not the end of" "$log")" -eq 1 ] || { echo "nvme-$c: FAIL: the wrong serial wasn't stopped once"; ok=0; }
        [ "$(grep -ac "not erased: nothing was written" "$log")" -eq 2 ] || { echo "nvme-$c: FAIL: \`yes\` and Ctrl+C weren't each stopped"; ok=0; }
        grep -aq "create_table" "$log" && { echo "nvme-$c: FAIL: create_table was asked"; ok=0; }
        # the guest's hash of the first 34 blocks, before and after: the same, and the host's
        host=$(head -c $((34 * 512)) "$era" | sha256 | cut -d' ' -f1)
        sums=$(grep -aoE "range lba 0 count 34 sha256 [0-9a-f]{64}" "$log" | awk '{print $NF}' | sort -u)
        [ "$sums" = "$host" ] || { echo "nvme-$c: FAIL: the guest's digests of the first 34 blocks ($sums) aren't the host's ($host)"; ok=0; }
        s2_trace "$dir/$c.trace" 1 || ok=0
    else
        s2_want "$log" "create_table: the drive is NOT blank and its erase was confirmed" \
            "create_table: Jam OS's table written and read back: GPT, 2 partitions, a Jam OS disk" \
            "volumes: nvme[0-9] is not empty: erasing it, as the owner confirmed" \
            "volumes: nvme[0-9] is a Jam OS disk now" "is a Jam OS disk already" || ok=0
        [ "$(grep -ac "that is not the end of" "$log")" -eq 1 ] || { echo "nvme-$c: FAIL: the other drive's wrong serial wasn't stopped once"; ok=0; }
        python3 tools/nvme-img.py "$dir/made.img" windows   # the same bytes as the erased one was
        if python3 tools/nvme-written.py "$era" --before "$dir/made.img" --pattern none --ranges > "$dir/written.txt"; then
            head -1 "$dir/written.txt" | sed "s/^/nvme-$c: /"
            s2_trace "$dir/$c.trace" 2 --write-ranges "$(tail -1 "$dir/written.txt")" || ok=0
        else
            echo "nvme-$c: FAIL: $(tail -1 "$dir/written.txt")"
            ok=0
        fi
        if command -v mdir > /dev/null 2>&1; then
            mdir -/ -i "$era@@1M" :: > "$dir/esp.txt" 2>&1 || { echo "nvme-$c: FAIL: the new ESP isn't a FAT volume"; ok=0; }
            grep -aqi "bootmgfw\|Microsoft" "$dir/esp.txt" && { echo "nvme-$c: FAIL: the old ESP's files are on the new one"; ok=0; }
        else
            echo "nvme-$c: (no mtools: the ESP not read on the host)"
        fi
        s6_ext "$era" "$dir/data" build.txt build/build.txt || ok=0
    fi
    if [ $ok = 1 ]; then rm -rf "$era" "$keep" "$dir/made.img" "$era.files" "$keep.files"; echo "nvme-$c: PASS"; else echo "nvme-$c: FAIL (see $dir)"; fails=$((fails + 1)); fi
}

# ---- M13.6's U7a: /jam, the Jam OS drive's ext data partition --------------------------------

# s5_make <dir> [mkgpt.py's options...]: $win, the Windows-like drive, and
# $jam, a Jam OS drive (the jamos layout, 1 GiB; QEMU's slot 0: serial
# jamos-nvme0, its end vme0) whose ESP is a FAT volume (mtools) and whose
# data partition is an ext volume of Jam OS's feature set that Linux's
# mke2fs made from a tree (docs/hello.txt, and a tmp/ the last boot "left
# behind", three entries), its files uid 1000's; the options go to
# tools/mkgpt.py (--break primary-array: jamro). A FAIL line and 1 if a
# tool is missing or fails.
s5_make() {
    d=$1 esp="$1/esp.img" ext="$1/data.img" tree="$1/tree"
    shift
    PATH=/usr/sbin:/sbin:$PATH
    for t in mke2fs debugfs e2fsck mformat mdir; do
        command -v $t > /dev/null 2>&1 ||
            { echo "nvme-$c: FAIL: no $t here (needs e2fsprogs and mtools: the test PC's WSL has them)"; return 1; }
    done
    python3 tools/nvme-img.py "$win" windows
    mkdir -p "$tree/docs" "$tree/tmp/old"
    echo "hello from the Linux-made volume" > "$tree/docs/hello.txt"
    echo "left by the last boot" > "$tree/tmp/left.txt"
    echo "left too" > "$tree/tmp/old/more.txt"
    rm -f "$ext" "$esp"
    truncate -s 64M "$ext"
    MKE2FS_CONFIG=/dev/null mke2fs -q -F -o Linux -b 4096 -I 256 -m 0 -J size=4 \
        -O none,has_journal,extent,flex_bg,64bit,sparse_super,large_file,huge_file,dir_nlink,extra_isize,metadata_csum,filetype \
        -U 4a414d34-0007-4000-8000-000000000007 -E lazy_itable_init=0,lazy_journal_init=0,root_owner=1000:1000 \
        -L jamdata -d "$tree" "$ext" > "$d/mke2fs.log" 2>&1 || { echo "nvme-$c: FAIL: mke2fs (see $d/mke2fs.log)"; return 1; }
    # the one user of Jam OS is uid 1000: the tree's files are its own
    for p in docs docs/hello.txt tmp tmp/left.txt tmp/old tmp/old/more.txt; do
        echo "sif /$p uid 1000"
        echo "sif /$p gid 1000"
    done > "$d/owner.cmds"
    debugfs -w -f "$d/owner.cmds" "$ext" > "$d/debugfs.log" 2>&1 || { echo "nvme-$c: FAIL: debugfs (see $d/debugfs.log)"; return 1; }
    truncate -s 128M "$esp"
    mformat -i "$esp" -T $((128 * 2048)) -F -v JAMOS-ESP :: ||
        { echo "nvme-$c: FAIL: mformat"; return 1; }
    python3 tools/mkgpt.py "$jam" --layout jamos --size 1G --fill "1=$esp" --fill "2=$ext" "$@" > /dev/null
    rm -f "$esp"
}

# s5_jam <dir>: a Jam OS drive (tools/mkgpt.py's jamos layout, 1 GiB; QEMU's
# slot 0: serial jamos-nvme0, its end vme0) whose ESP is a FAT volume
# (mtools) and whose data partition is an ext volume of Jam OS's feature set
# that Linux's mke2fs made from a tree (docs/hello.txt, and a tmp/ the last
# boot "left behind"), beside the Windows-like drive; booted from the stick.
# volumes finds the superblock and starts bin/extfs on the data partition,
# /jam, read-write; init gives the namespace /usr, /var, /home and /tmp as
# views of it and empties the old /tmp. Files go in through /jam and through
# the views and are read back through the other name; `disks` says (ext);
# `mount -w` of the Windows-like drive's ESP is still refused. Then `install`
# over the Jam OS drive (it is Jam OS's already: its ESP's files replaced, its
# /data kept, /data/etc copied in): the data partition is held writable for
# init alone (/jam and its views gone meanwhile) and let go (/jam and its
# views back), and nothing it held is lost. Afterwards, on the host: the
# ext volume through Linux's e2fsck (the journal replayed first), the file
# and the copied settings in it, the ESP holding the build, and the Windows-
# like image unchanged.
s5_jam() {
    dir=$1 ok=1
    win="$dir/windows.img" jam="$dir/jamos.img" log="$dir/nvme-$c.log"
    s5_make "$dir" || { fails=$((fails + 1)); return; }
    before=$(sha256 "$win" | cut -d' ' -f1)
    in="$dir/nvme-$c.in"
    {
        echo "# generated by tools/nvme-test.sh: $c"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "seen 60 init: /data mounted"
        s3_names bd wd
        echo "send T=x"
        echo "wait {prompt}"
        # /jam and its views
        echo "seen 60 init: /jam mounted"
        echo "seen 60 init: /jam: mounted as views: /usr /var /home"
        echo "seen 60 init: /tmp: a view of /jam/tmp"
        echo "send mount | grep /jam | grep -c read-write | grep ^1\$ && echo ok-\$T-rw"
        echo "wait ok-x-rw"
        # (the shell's grep takes one plain pattern, anchors allowed: no -E)
        # (and the shell's line is short: a long one is cut where the terminal scrolls)
        echo "send mount | grep -c ^/usr | grep ^1\$ && mount | grep -c ^/var | grep ^1\$ && echo ok-\$T-views1"
        echo "wait ok-x-views1"
        echo "send mount | grep -c ^/home | grep ^1\$ && mount | grep -c ^/tmp | grep ^1\$ && echo ok-\$T-views2"
        echo "wait ok-x-views2"
        echo "send cat /jam/docs/hello.txt"
        echo "wait hello from the Linux-made volume"
        echo "send ls /jam/tmp | grep -c . | grep ^0\$ && ls /tmp | grep -c . | grep ^0\$ && echo ok-\$T-tmp"
        echo "wait ok-x-tmp"
        echo "send write /tmp/t.txt via-the-tmp-view-\$T && cat /jam/tmp/t.txt"
        echo "wait via-the-tmp-view-x"
        echo "send mkdir /usr/bin; write /usr/bin/hi.txt in-usr && cat /jam/usr/bin/hi.txt"
        echo "wait in-usr"
        echo "send write /var/v.txt in-var && cat /jam/var/v.txt"
        echo "wait in-var"
        echo "send write /home/h.txt in-home && cat /jam/home/h.txt"
        echo "wait in-home"
        echo "send write /jam/new.txt written-to-jam && cat /jam/new.txt"
        echo "wait written-to-jam"
        # (ls shows a directory with a / after it)
        echo "send ls /jam | grep -c ^docs/\$ | grep ^1\$ && ls /jam | grep -c ^new.txt\$ | grep ^1\$ && echo ok-\$T-ls1"
        echo "wait ok-x-ls1"
        echo "send ls /jam | grep -c ^usr/\$ | grep ^1\$ && ls /jam | grep -c ^tmp/\$ | grep ^1\$ && echo ok-\$T-ls2"
        echo "wait ok-x-ls2"
        echo "send disks | grep -c \"/jam (ext)\" | grep ^1\$ && echo ok-\$T-disks"
        echo "wait ok-x-disks"
        # (the Windows-like drive's ESP's mount: its number is not its disk's)
        echo "send disks"
        printf '%s\n' "seen re:serial jamos-nvme1 [^\r\n]*\r?\n +1 +EFI system [^\r\n]*?/nvme(?P<we>[0-9]+)"
        echo "wait {prompt}"
        echo "send mount -w /nvme{we}"
        echo "wait is not on a Jam OS disk: it stays read-only"
        echo "wait {prompt}"
        # the installer over it: the data partition held and let go
        echo "send mkdir /data/etc; write /data/etc/probe.txt jam-probe && sync"
        echo "wait {prompt}"
        echo "send install nvme{bd}"
        echo "wait a Jam OS disk already"
        echo "wait Type the last 4 characters"
        echo "send vme0"
        echo "wait 300 installed on nvme"
        echo "wait {prompt}"
        echo "send sleep 3"
        echo "wait {prompt}"
        echo "send cat /jam/etc/probe.txt /jam/docs/hello.txt /jam/new.txt"
        echo "wait jam-probe"
        echo "wait hello from the Linux-made volume"
        echo "wait written-to-jam"
        echo "send mount | grep -c ^/jam | grep ^1\$ && mount | grep -c ^/usr | grep ^1\$ && echo ok-\$T-back1"
        echo "wait ok-x-back1"
        echo "send mount | grep -c ^/var | grep ^1\$ && mount | grep -c ^/home | grep ^1\$ && echo ok-\$T-back2"
        echo "wait ok-x-back2"
        echo "send mount | grep -c ^/tmp | grep ^1\$ && echo ok-\$T-back3"
        echo "wait ok-x-back3"
        echo "send write /tmp/t2.txt again-\$T && cat /jam/tmp/t2.txt && cat /jam/usr/bin/hi.txt"
        echo "wait again-x"
        echo "wait in-usr"
        echo "send sync"
        echo "wait {prompt}"
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$in"
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-420} QEMU_NVME="$jam $win" QEMU_INPUT="$in" \
        QEMU_EXTRA="-trace pci_nvme_pci_reset -trace pci_nvme_admin_cmd -trace pci_nvme_io_cmd -trace pci_nvme_write -D $dir/$c.trace" \
        tools/qemu-test.sh "$dir" "nvme-$c" shell > "$dir/nvme-$c.out" 2>&1 ||
        { echo "nvme-$c: FAIL: the run failed (see $dir/nvme-$c.out)"; grep -a "serial-feed: .*no '" "$dir/nvme-$c.out" || true; ok=0; }
    [ "$(sha256 "$win" | cut -d' ' -f1)" = "$before" ] || { echo "nvme-$c: FAIL: the Windows-like image changed"; ok=0; }
    s2_want "$log" "volumes: nvme[0-9]+ partition 2 -> bin/extfs at /jam \(OK\)" \
        "volumes: nvme[0-9]+ partition 2 is /jam, read-write" \
        "extfs /jam: mounted" \
        "init: /jam: mounted as views: /usr /var /home" \
        "init: /tmp: a view of /jam/tmp \(emptied: 3 removed\)" \
        "volumes: /jam is the installer's, read-write, and no mount" \
        "init: /tmp: in memory" \
        "volumes: /jam is read-write and a mount again \(OK\)" \
        "installed on nvme" || ok=0
    # /tmp went to memory while the installer held /jam, and came back to /jam/tmp, not emptied again
    [ "$(grep -ac "init: /tmp: a view of /jam/tmp" "$log")" -ge 2 ] && [ "$(grep -ac "emptied:" "$log")" -eq 1 ] ||
        { echo "nvme-$c: FAIL: /tmp didn't come back to /jam/tmp once, emptied once"; ok=0; }
    s2_want "$log" "volumes: nvme[0-9]+ partition 1 -> bin/fat at /nvme[0-9]+, read-only" || ok=0
    grep -aqE "extfs-jam.*(crashed|killed)|giving up|did not end cleanly" "$log" &&
        { echo "nvme-$c: FAIL: a service crashed or was given up on"; ok=0; }
    # the ext volume on the host: the journal replayed, then clean
    dd if="$jam" of="$dir/data.out" bs=1M skip=513 count=64 2> /dev/null
    e2fsck -E journal_only -y "$dir/data.out" > "$dir/e2fsck-1.txt" 2>&1 || [ $? -le 1 ] ||
        { echo "nvme-$c: FAIL: e2fsck (see $dir/e2fsck-1.txt)"; ok=0; }
    e2fsck -fn "$dir/data.out" > "$dir/e2fsck-2.txt" 2>&1 ||
        { echo "nvme-$c: FAIL: the volume isn't clean after its journal was replayed (see $dir/e2fsck-2.txt)"; ok=0; }
    for f in docs/hello.txt new.txt tmp/t2.txt usr/bin/hi.txt etc/probe.txt; do
        debugfs -R "cat /$f" "$dir/data.out" > "$dir/got.txt" 2> /dev/null
        [ -s "$dir/got.txt" ] || { echo "nvme-$c: FAIL: /$f isn't on the data partition"; ok=0; }
    done
    debugfs -R "ls /tmp" "$dir/data.out" 2> /dev/null | grep -q "left.txt\|old" &&
        { echo "nvme-$c: FAIL: the old /tmp wasn't emptied"; ok=0; }
    # the ESP: the build there
    mdir -i "$jam@@1M" ::/boot > "$dir/esp-boot.txt" 2>&1 || true
    grep -qi "jamos.elf" "$dir/esp-boot.txt" && grep -qi "bootfs.img" "$dir/esp-boot.txt" ||
        { echo "nvme-$c: FAIL: the ESP doesn't hold the build (see $dir/esp-boot.txt)"; ok=0; }
    if [ $ok = 1 ]; then rm -rf "$win" "$jam" "$tree" "$dir/data.out" "$ext"; echo "nvme-$c: PASS"; else echo "nvme-$c: FAIL (see $dir)"; fails=$((fails + 1)); fi
}

# ---- M13.6's U7b: a damaged Jam OS drive, and `disk format` ------------------------------------

# s6_boot <dir> <name> <input> <nvme images>: one boot from the stick with
# these drives, its NVMe commands traced to <dir>/<name>.trace. 1 if the
# run failed.
s6_boot() {
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-420} QEMU_NVME="$4" QEMU_INPUT="$3" \
        QEMU_EXTRA="-trace pci_nvme_pci_reset -trace pci_nvme_admin_cmd -trace pci_nvme_io_cmd -trace pci_nvme_write -D $1/$2.trace" \
        tools/qemu-test.sh "$1" "$2" shell > "$1/$2.out" 2>&1 ||
        { echo "nvme-$c: FAIL: the run failed (see $1/$2.out)"; grep -a "serial-feed: .*no '" "$1/$2.out" || true; return 1; }
}

# s7_jamro <dir>: the jamro case, two boots. The first: /jam read-only on a
# drive the driver calls someone else's, /tmp in memory, `disk prepare`
# offering its erase and `no` (both images unchanged afterwards, no Write in
# the trace); the second: the erase, /jam read-write and new.
s7_jamro() {
    dir=$1 ok=1
    win="$dir/windows.img" jam="$dir/jamos.img" log="$dir/nvme-$c.log"
    s5_make "$dir" --break primary-array || { fails=$((fails + 1)); return; }
    before=$(sha256 "$win" "$jam" | cut -d' ' -f1)
    {
        echo "# generated by tools/nvme-test.sh: $c, boot 1"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "seen 60 init: /data mounted"
        s3_names bd wd
        echo "send T=x"
        echo "wait {prompt}"
        echo "seen 60 init: /jam mounted"
        echo "send mount | grep /jam | grep -c read-only | grep ^1\$ && echo ok-\$T-ro"
        echo "wait ok-x-ro"
        echo "send cat /jam/docs/hello.txt"
        echo "wait hello from the Linux-made volume"
        echo "send write /jam/no.txt x || echo refused-\$T"
        echo "wait refused-x"
        echo "send write /tmp/t.txt in-tmp-\$T && cat /tmp/t.txt"
        echo "wait in-tmp-x"
        echo "send disk prepare nvme{bd}"
        echo "wait It is NOT empty"
        echo "wait ext volume"
        echo "wait mounted read-only at /jam"
        echo "wait EVERYTHING ON nvme{bd} WILL BE LOST"
        echo "wait Type the last 4 characters"
        echo "send vme0"
        echo "wait type erase"
        echo "send no"
        echo "wait not erased: nothing was written"
        echo "wait {prompt}"
        echo "send install nvme{bd}"
        echo "wait It is NOT empty"
        echo "wait Type the last 4 characters"
        echo "send zzzz"
        echo "wait is not the end of"
        echo "wait {prompt}"
        echo "send cat /jam/docs/hello.txt"
        echo "wait hello from the Linux-made volume"
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$dir/nvme-$c.in"
    s6_boot "$dir" "nvme-$c" "$dir/nvme-$c.in" "$jam $win" || ok=0
    [ "$(sha256 "$win" "$jam" | cut -d' ' -f1)" = "$before" ] || { echo "nvme-$c: FAIL: an image changed before the erase"; ok=0; }
    s2_want "$log" "volumes: nvme[01] partition 2 is /jam, read-only" \
        "volumes: nvme[01]: its driver doesn't read the drive as Jam OS's" \
        "init: /tmp: in memory" "not erased: nothing was written" || ok=0
    grep -aq "init: /tmp: a view of /jam/tmp" "$log" && { echo "nvme-$c: FAIL: /tmp on a read-only /jam"; ok=0; }
    grep -aq "a Jam OS disk already" "$log" && { echo "nvme-$c: FAIL: the damaged drive was taken as a Jam OS disk"; ok=0; }
    s2_trace "$dir/nvme-$c.trace" 1 || ok=0
    {
        echo "# generated by tools/nvme-test.sh: $c, boot 2"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "seen 60 init: /data mounted"
        s3_names bd wd
        echo "send T=x"
        echo "wait {prompt}"
        echo "send disk prepare nvme{bd}"
        echo "wait It is NOT empty"
        echo "wait Type the last 4 characters"
        echo "send vme0"
        echo "wait type erase"
        echo "send erase"
        echo "wait 300 is a Jam OS disk now: ESP"
        echo "wait {prompt}"
        echo "seen 60 init: /tmp: a view of /jam/tmp"
        echo "send mount | grep /jam | grep -c read-write | grep ^1\$ && echo ok-\$T-rw"
        echo "wait ok-x-rw"
        echo "send test -e /jam/docs || echo gone-\$T"
        echo "wait gone-x"
        echo "send cp /boot/build.txt /jam/build.txt; sync; sha256sum /jam/build.txt"
        echo "wait $(sha256 build/build.txt | cut -d' ' -f1)"
        echo "wait {prompt}"
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$dir/nvme-$c-2.in"
    s6_boot "$dir" "nvme-$c-2" "$dir/nvme-$c-2.in" "$jam $win" || ok=0
    [ "$(sha256 "$win" | cut -d' ' -f1)" = "$(echo "$before" | sed -n 1p)" ] || { echo "nvme-$c: FAIL: the Windows-like image changed"; ok=0; }
    s2_want "$dir/nvme-$c-2.log" "volumes: nvme[01] is not empty: erasing it, as the owner confirmed" \
        "volumes: nvme[01] is a Jam OS disk now: /nvme[0-9] \(FAT\), read-only and /jam \(ext\), read-write" || ok=0
    s6_ext "$jam" "$dir/data" build.txt build/build.txt || ok=0
    if [ $ok = 1 ]; then rm -rf "$win" "$jam" "$tree" "$ext"; echo "nvme-$c: PASS"; else echo "nvme-$c: FAIL (see $dir)"; fails=$((fails + 1)); fi
}

# s8_format <dir>: the format case, two boots on a Jam OS drive as W2 made
# it (both partitions FAT, the data partition's FAT volume 64 MiB with
# old.txt) beside the Windows-like drive: FAT to ext (the serial's end wrong
# first), then ext to FAT32 (`data fat`).
s8_format() {
    dir=$1 ok=1
    win="$dir/windows.img" jam="$dir/jamos.img" log="$dir/nvme-$c.log"
    esp="$dir/esp.img" fat="$dir/data.img"
    for t in mformat mcopy mdir e2fsck debugfs; do
        PATH=/usr/sbin:/sbin:$PATH command -v $t > /dev/null 2>&1 ||
            { echo "nvme-$c: FAIL: no $t here (needs e2fsprogs and mtools)"; fails=$((fails + 1)); return; }
    done
    python3 tools/nvme-img.py "$win" windows
    rm -f "$esp" "$fat"
    truncate -s 128M "$esp"
    mformat -i "$esp" -T $((128 * 2048)) -F -v JAMOS-ESP :: || { echo "nvme-$c: FAIL: mformat"; fails=$((fails + 1)); return; }
    truncate -s 64M "$fat"
    mformat -i "$fat" -T $((64 * 2048)) -F -v JAMOS-DATA :: || { echo "nvme-$c: FAIL: mformat"; fails=$((fails + 1)); return; }
    echo "old FAT file" > "$dir/old.txt"
    mcopy -i "$fat" "$dir/old.txt" ::/old.txt
    python3 tools/mkgpt.py "$jam" --layout jamos --size 1G --fill "1=$esp" --fill "2=$fat" > /dev/null
    rm -f "$esp" "$fat"
    win_before=$(sha256 "$win" | cut -d' ' -f1)
    esp_before=$(dd if="$jam" bs=1M skip=1 count=512 2> /dev/null | sha256 | cut -d' ' -f1)
    {
        echo "# generated by tools/nvme-test.sh: $c, boot 1: FAT to ext"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "seen 60 init: /data mounted"
        s3_names bd wd
        echo "send T=x"
        echo "wait {prompt}"
        echo "send disks"
        printf '%s\n' "seen re:serial jamos-nvme0 [^\r\n]*\r?\n +1 +EFI system [^\r\n]*\r?\n +2 +Jam OS data [^\r\n]*?/nvme(?P<dm>[0-9]+)"
        echo "wait {prompt}"
        echo "send cat /nvme{dm}/old.txt"
        echo "wait old FAT file"
        echo "send disk format nvme{wd} data"
        echo "wait is not a Jam OS disk"
        echo "wait {prompt}"
        echo "send disk format nvme{bd} data"
        echo "wait a FAT volume, mounted read-only at /nvme{dm}"
        echo "wait EVERYTHING ON IT WILL BE LOST"
        echo "wait formats it ext"
        echo "wait Type the last 4 characters"
        echo "send zzzz"
        echo "wait is not the end of"
        echo "wait {prompt}"
        echo "send cat /nvme{dm}/old.txt"
        echo "wait old FAT file"
        echo "send disk format nvme{bd} data"
        echo "wait Type the last 4 characters"
        echo "send vme0"
        echo "wait 300 data partition is formatted:"
        echo "wait an ext volume, mounted read-write at /jam"
        echo "wait {prompt}"
        echo "seen 60 init: /tmp: a view of /jam/tmp"
        echo "send test -e /jam/old.txt || echo gone-\$T"
        echo "wait gone-x"
        echo "send cp /boot/build.txt /jam/build.txt; sync; sha256sum /jam/build.txt"
        echo "wait $(sha256 build/build.txt | cut -d' ' -f1)"
        echo "wait {prompt}"
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$dir/nvme-$c.in"
    s6_boot "$dir" "nvme-$c" "$dir/nvme-$c.in" "$jam $win" || ok=0
    s2_want "$log" "volumes: nvme[01]: its data partition is formatted again, as the owner confirmed: /nvme[0-9] stops first" \
        "volumes: nvme[01] partition 2 is blank: bin/extfs makes an ext volume on it" \
        "volumes: nvme[01]'s data partition is formatted: /jam \(ext\), read-write" || ok=0
    [ "$(grep -ac "that is not the end of" "$log")" -eq 1 ] || { echo "nvme-$c: FAIL: the wrong serial wasn't stopped once"; ok=0; }
    s6_ext "$jam" "$dir/data" build.txt build/build.txt || ok=0
    {
        echo "# generated by tools/nvme-test.sh: $c, boot 2: ext to FAT32"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "seen 60 init: /data mounted"
        s3_names bd wd
        echo "send T=x"
        echo "wait {prompt}"
        echo "seen 60 init: /tmp: a view of /jam/tmp"
        echo "send sha256sum /jam/build.txt"
        echo "wait $(sha256 build/build.txt | cut -d' ' -f1)"
        echo "wait {prompt}"
        echo "send disk format nvme{bd} data fat"
        echo "wait an ext volume, mounted read-write at /jam"
        echo "wait formats it FAT32"
        echo "wait Type the last 4 characters"
        echo "send vme0"
        echo "wait 300 data partition is formatted:"
        echo "wait a FAT volume, mounted read-only at /nvme"
        echo "wait {prompt}"
        echo "send mount | grep -c ^/jam | grep ^0\$ && echo nojam-\$T"
        echo "wait nojam-x"
        echo "send write /tmp/t.txt in-tmp-\$T && cat /tmp/t.txt"
        echo "wait in-tmp-x"
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$dir/nvme-$c-2.in"
    s6_boot "$dir" "nvme-$c-2" "$dir/nvme-$c-2.in" "$jam $win" || ok=0
    s2_want "$dir/nvme-$c-2.log" "volumes: nvme[01]: its data partition is formatted again, as the owner confirmed: /jam stops first" \
        "volumes: nvme[01]'s data partition is formatted: /nvme[0-9] \(FAT\), read-only" || ok=0
    # /tmp went back to memory when /jam went
    [ "$(grep -ac "init: /tmp: in memory" "$dir/nvme-$c-2.log")" -ge 2 ] || { echo "nvme-$c: FAIL: /tmp didn't go to memory"; ok=0; }
    [ "$(sha256 "$win" | cut -d' ' -f1)" = "$win_before" ] || { echo "nvme-$c: FAIL: the Windows-like image changed"; ok=0; }
    [ "$(dd if="$jam" bs=1M skip=1 count=512 2> /dev/null | sha256 | cut -d' ' -f1)" = "$esp_before" ] ||
        { echo "nvme-$c: FAIL: the ESP changed"; ok=0; }
    if python3 tools/nvme-written.py "$jam" --pattern none > "$dir/written.txt"; then
        data=$(sed -n 's/.*data \([0-9]*\)+\([0-9]*\);.*/\1 \2/p' "$dir/written.txt")
        set -- $data
        for t in "$dir/nvme-$c.trace" "$dir/nvme-$c-2.trace"; do
            s2_trace "$t" 2 --write-ranges "$1-$(($1 + $2))" || ok=0
        done
    else
        echo "nvme-$c: FAIL: $(tail -1 "$dir/written.txt")"
        ok=0
    fi
    python3 tools/fatcheck.py "$jam@@513M" --require-clean > "$dir/fatcheck.txt" 2>&1 ||
        { echo "nvme-$c: FAIL: the FAT32 data partition (see $dir/fatcheck.txt)"; ok=0; }
    if [ $ok = 1 ]; then rm -rf "$win" "$jam"; echo "nvme-$c: PASS"; else echo "nvme-$c: FAIL (see $dir)"; fails=$((fails + 1)); fi
}

for c in $cases; do
    if [ $c = checker ]; then checker; continue; fi
    if [ $c = write ] || [ $c = wiommu ] || [ $c = diskro ] || [ $c = prepare ] || [ $c = piommu ] ||
       [ $c = cutprep ] || [ $c = cutbad ] || [ $c = cuterase ] || [ $c = erase ] || [ $c = erasestop ] ||
       [ $c = jam ] || [ $c = jamro ] || [ $c = format ]; then
        rm -rf "$out/nvme-$c"
        mkdir -p "$out/nvme-$c"
        s2_iommu=
        [ $c = wiommu ] || [ $c = piommu ] && s2_iommu=1
        case $c in
        diskro) s2_diskro "$out/nvme-$c" ;;
        jam) s5_jam "$out/nvme-$c" ;;
        jamro) s7_jamro "$out/nvme-$c" ;;
        format) s8_format "$out/nvme-$c" ;;
        prepare | piommu) s3_prepare "$out/nvme-$c" prepare ;;
        cutprep | cutbad | cuterase) s3_prepare "$out/nvme-$c" $c ;;
        erase | erasestop) s4_erase "$out/nvme-$c" $c ;;
        *) s2_write "$out/nvme-$c" ;;
        esac
        continue
    fi
    name=nvme-$c
    dir="$out/$name"
    rm -rf "$dir"
    mkdir -p "$dir"
    log="$dir/$name.log" trace="$dir/$name.trace"
    ok=1
    say() { echo "$name: $*"; }
    bad() { say "FAIL: $*"; ok=0; }
    want() { grep -aqE -- "$1" "$log" || bad "no line matching '$1'"; }   # want <regex>

    # ---- the images -------------------------------------------------------------------------
    bs=512 props= words= iommu= dmg=
    img="$dir/disk.img" imgs="$img"
    case $c in
    windows | kill | plain) ;;
    block) words="nvmetest=slow" ;;   # answers 20 ms apart: the drain rule is seen
    iommu) iommu=1 words="iommu=on" ;;
    twons) imgs="$img+$dir/ns2.img" ;;
    4k) bs=4096 props=",logical_block_size=4096,physical_block_size=4096" ;;
    damaged) dmg="--damage primary-header" ;;
    *) echo "nvme-test: no case '$c'"; exit 2 ;;
    esac
    # Level 1 has no client that reads: the driver's test word reads the
    # namespace's ends (drivers/nvme/selftest.c), so the trace has Reads.
    [ $level = 1 ] && words="${words:+$words }nvmetest=selftest"
    # nvmetest=on: no test words (drv/nvme binds on every boot; the plain case gives none).
    case " $words " in *" nvmetest="*) ;; *) [ $c = plain ] || words="${words:+$words }nvmetest=on" ;; esac
    # shellcheck disable=SC2086
    python3 tools/nvme-img.py "$img" windows --block-size $bs $dmg
    before2=
    if [ $c = twons ]; then
        python3 tools/nvme-img.py "$dir/ns2.img" small
        before2=$(sha256 "$dir/ns2.img" | cut -d' ' -f1)
    fi
    before=$(sha256 "$img" | cut -d' ' -f1)
    boot_hash=$(sha256 "$img.files/bootmgfw.efi" | cut -d' ' -f1)
    python3 tools/disk-regions.py --image --block-size $bs "$img" | grep -v '^#' > "$dir/regions-host.txt"

    # ---- the shell script the guest is given ------------------------------------------------
    in="$dir/$name.in"
    {
        echo "# generated by tools/nvme-test.sh for case $c, level $level"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "seen 60 init: /data mounted"   # volumes is up, and has been told of the disks
        if [ $c = block ] && [ $level -ge 2 ]; then
            echo "send utest only nvme_; echo nvme-utest-\$?"
            echo "wait 240 passed"
            echo "wait {prompt}"
        fi
        if [ $level -ge 3 ] && [ $c != block ]; then
            echo "send disks"
            echo "wait nvme0"
            echo "wait {prompt}"
            if [ $c != 4k ]; then
                echo "send mount"
                echo "wait /nvme0"
                echo "wait {prompt}"
                echo "send ls /nvme0/EFI/Microsoft/Boot"
                echo "wait BCD"   # ls sorts upper case first: BCD, then bootmgfw.efi
                echo "wait bootmgfw.efi"
                echo "wait {prompt}"
                if [ $c = kill ]; then
                    # The driver's end takes the mounts with it (volumes: "its driver
                    # ended"); they are back once devmgr's restart has attached the
                    # disk again, a moment later.
                    echo "send ls /nvme0 > /dev/null; kill nvme; sleep 2; sha256sum /nvme0/EFI/Microsoft/Boot/bootmgfw.efi"
                else
                    echo "send sha256sum /nvme0/EFI/Microsoft/Boot/bootmgfw.efi"
                fi
                echo "wait $boot_hash"
                echo "wait {prompt}"
                echo "send mount -w /nvme0"
                echo "wait not on a Jam OS disk"
                echo "wait {prompt}"
            fi
            echo "send disk smart nvme0"
            echo "wait {prompt}"
            echo "send disk digest nvme0"
            echo "wait 120 disk digest: done"
            echo "wait {prompt}"
            echo "send disk smart nvme0"
            echo "wait {prompt}"
        fi
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$in"

    # ---- the run ----------------------------------------------------------------------------
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-240} QEMU_NVME="$imgs$props" QEMU_INPUT="$in" \
        QEMU_EXTRA="-trace pci_nvme_pci_reset -trace pci_nvme_admin_cmd -trace pci_nvme_io_cmd -D $trace" \
        QEMU_IOMMU=$iommu QEMU_WORDS="$words" \
        tools/qemu-test.sh "$dir" "$name" shell > "$dir/$name.out" 2>&1 ||
        { bad "the run failed (see $dir/$name.out)"; grep -a "serial-feed: .*no '" "$dir/$name.out" || true; }

    # ---- 1. the image is unchanged ----------------------------------------------------------
    after=$(sha256 "$img" | cut -d' ' -f1)
    [ "$before" = "$after" ] || bad "the drive image changed: $before -> $after"
    if [ $c = twons ] && [ "$before2" != "$(sha256 "$dir/ns2.img" | cut -d' ' -f1)" ]; then
        bad "the second namespace's image changed"
    fi

    # ---- 2. the trace has only allowed opcodes ----------------------------------------------
    # The device's reset at power-on is traced too: proof that the trace is
    # on and was written out, whether or not anything sent a command. Nothing
    # talks to the drive until drv/nvme runs (the firmware boots the stick and
    # leaves the controller alone): at level 0 no command is expected; from
    # level 1 on the checker fails on a trace with none (and with no Identify
    # and no Read).
    grep -aq "^pci_nvme_pci_reset" "$trace" 2> /dev/null || bad "the trace is dead: no device reset in $trace"
    req=
    [ $level -ge 1 ] && req="--require-admin 06 --require-io 02"
    if [ $level -ge 1 ] || grep -aqE "^pci_nvme_(admin|io)_cmd" "$trace" 2> /dev/null; then
        # shellcheck disable=SC2086
        python3 tools/nvme-trace-check.py --stage 1 $req "$trace" > "$dir/trace-check.txt" ||
            bad "the trace has an opcode stage 1 never sends, or no command (see $dir/trace-check.txt)"
        tail -1 "$dir/trace-check.txt" | sed "s/^/$name: /"
    else
        say "no NVMe command in the trace: nothing has talked to the drive (no driver yet)"
    fi

    # ---- 3. what the guest found ------------------------------------------------------------
    want "NVMe"   # level 0: the PCI report names the function
    L1() {
        want "nvme [0-9a-f:.]+: .*namespace"
        if [ $level = 1 ]; then want "selftest: both ends read and hashed"; fi
        if [ $c = twons ]; then want "namespace 2: not served"; fi
    }
    L2() {
        case $c in
        damaged) want "primary GPT damaged" ;;
        *) want "GPT, 4 partitions, read-only" ;;
        esac
    }
    L3() {
        if [ $c = 4k ]; then want "no FAT"; else want "EFI system"; fi
        if [ $c = plain ] && grep -aq "hello from ring 3.*nvmetest" "$log"; then bad "the plain case was given an nvmetest= word"; fi
        if [ $c = kill ]; then want "nvme0: its driver ended"; want "nvme0 partition 1 is /nvme0, read-only"; fi
        # `disk digest`'s lines against Linux's tool on the same image
        sed -nE 's/.*((gpt-head|part[0-9]+(-[a-z0-9]+)?|gpt-tail) lba [0-9]+ count [0-9]+ sha256 [0-9a-f]{64}).*/\1/p' \
            "$log" > "$dir/regions-guest.txt"
        cmp -s "$dir/regions-guest.txt" "$dir/regions-host.txt" ||
            bad "disk digest differs from tools/disk-regions.py (diff $dir/regions-guest.txt $dir/regions-host.txt)"
    }
    # utest's NVMe cases (user/tests/utest/nvme.c), and their region lines
    # against the host's.
    LB() {
        want "nvme-utest-0"
        want "utest: 4 passed"
        sed -nE 's/.*utest: nvme region ((gpt-head|part[0-9]+|gpt-tail) lba [0-9]+ count [0-9]+ sha256 [0-9a-f]{64}).*/\1/p' \
            "$log" > "$dir/regions-utest.txt"
        [ "$(wc -l < "$dir/regions-utest.txt")" -eq 4 ] || bad "not 4 region lines from utest"
        while read -r line; do
            grep -qxF "$line" "$dir/regions-host.txt" || bad "utest's '$line' isn't the host's"
        done < "$dir/regions-utest.txt"
        # the client that left with reads out: dropped, the partition drained
        want "partition 1: its client left with [1-9][0-9]* request\(s\) out"
        want "partition 1: the gone client's requests are done"
    }
    if [ $level -ge 1 ]; then L1; fi
    if [ $level -ge 2 ]; then L2; fi
    if [ $level -ge 2 ] && [ $c = block ]; then LB; fi
    if [ $level -ge 3 ] && [ $c != block ]; then L3; fi
    if [ $c = iommu ] && grep -aq "vtd: fault" "$log"; then bad "a VT-d fault"; fi

    if [ $ok = 1 ]; then
        rm -f "$img" "$dir/ns2.img"
        say "PASS (level $level)"
    else
        say "FAIL (see $dir)"
        fails=$((fails + 1))
    fi
done
[ $fails = 0 ]
