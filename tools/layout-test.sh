#!/bin/sh
# Which volume is /data and which /jam on each kind of boot, and the views
# that follow them (docs/M13.6-PLAN.md's DB1 notes): /usr /var /home, /tmp
# (a file in it mapped: posixtest's mmap-tmp) and /pkg. Cases, each its own
# boot with its own shell script (tools/shell-tests/layout-<case>.txt):
#   drive     booted from an installed Jam OS drive (QEMU_BOOT_NVME=1) whose
#             data partition is ext: /data and /jam are the one volume (a
#             file written through either is read through the other, and is
#             on the volume afterwards), /jam's views, /tmp a view of
#             /jam/tmp, /pkg a read-only view of /jam/pkg; volumes says the
#             data partition is /jam too, one extfs serves both names
#   both      a stick with a FAT /data beside a Jam OS drive with an ext data
#             partition: /data the stick's and /jam the drive's, two volumes;
#             /jam's views, /tmp of /jam/tmp, /pkg of /jam/pkg
#   stick     a stick with a FAT /data and no drive: no /jam, /tmp in memory
#             with its own pager, no /pkg (FAT has no views)
#   stickext  a stick with an ext /data and no drive: no /jam, /tmp of
#             /data/tmp, /pkg of /data/pkg
# The images are made from build/jamos.img (`make image` first), whichever
# filesystem its /data has: the other one is made on a copy
# (tools/ext-stick.sh for ext, the same files; `make image DATA_FS=fat` for a
# FAT one, empty).
# Every ext volume a run wrote is clean for Linux's e2fsck afterwards (the
# journal replayed first). Needs e2fsprogs and mtools (the test PC's WSL):
# PC only. QEMU_SMP and QEMU_MEM pass through.
# Usage: tools/layout-test.sh <outdir> [case...]; exit 0 on PASS.
set -eu
. tools/datafs.sh
out=$1
shift
cases=${*:-drive both stick stickext}
name=layout
mkdir -p "$out"
ok=1
PATH=/usr/sbin:/sbin:$PATH
for t in mke2fs debugfs e2fsck mcopy mformat; do
    command -v $t > /dev/null 2>&1 ||
        { echo "$name: no $t here (needs e2fsprogs and mtools: the test PC's WSL has them)"; exit 1; }
done

count() { grep -acE -- "$1" "$2" || true; }
want() {   # want <case> <how many times at least> <text (a regex)> <log>
    n=$(count "$3" "$4")
    [ "$n" -ge "$2" ] || { echo "$name: $1: \"$3\" $n times in $(basename "$4"), wanted $2 or more"; ok=0; }
}
never() {   # never <case> <text> <log>
    n=$(count "$2" "$3")
    [ "$n" -eq 0 ] || { echo "$name: $1: \"$2\" $n times in $(basename "$3")"; ok=0; }
}

# stick_with fat|ext <image>: a copy of build/jamos.img whose /data has
# that filesystem. A FAT one is `make image DATA_FS=fat` (tools/datafs.sh: its
# partition has FAT's type); an ext one is the image itself (ext is the
# default) or, on a FAT image, the same files made again by mke2fs (the type
# stays: volumes reads the boot disk's data partition by what it holds).
stick_with() {
    if [ "$1" = fat ]; then
        data_fat_image "$2" || { echo "$name: the FAT stick couldn't be made"; return 1; }
        return 0
    fi
    cp build/jamos.img "$2"
    [ "$(python3 tools/mkimage.py --data-fs "$2")" = ext ] && return 0
    tools/ext-stick.sh "$2" > "$2.out" 2>&1 ||
        { echo "$name: the ext stick couldn't be made (see $2.out)"; return 1; }
}

# ext_clean <case> <image>: the image's ext data partition (MBR or GPT),
# copied out, its journal replayed, is clean for e2fsck -fn; $vol keeps the
# copy for debugfs.
ext_clean() {
    at=$(python3 tools/mkimage.py --data-at "$2")
    vol="$out/$name-$1-data.img"
    dd if="$2" of="$vol" bs=1M skip="${at%M}" status=none
    e2fsck -E journal_only -y "$vol" > "$out/$name-$1-e2fsck-1.txt" 2>&1 || [ $? -le 1 ] ||
        { echo "$name: $1: e2fsck (see $out/$name-$1-e2fsck-1.txt)"; ok=0; }
    e2fsck -fn "$vol" > "$out/$name-$1-e2fsck-2.txt" 2>&1 ||
        { echo "$name: $1: the volume isn't clean once its journal is replayed (see $out/$name-$1-e2fsck-2.txt)"; ok=0; }
}

# on_volume <case> <path> <text>: the file on $vol holds the text.
on_volume() {
    debugfs -R "cat $2" "$vol" 2> /dev/null | grep -q "$3" ||
        { echo "$name: $1: $2 on the volume doesn't hold $3"; ok=0; }
}

# run_case <case> <stick image> [VAR=value ...]: its shell script; the log
# into $run.
run_case() {
    c=$1 img=$2
    shift 2
    if ! env QEMU_TIMEOUT="${QEMU_TIMEOUT:-300}" QEMU_IMAGE="$img" \
         QEMU_INPUT="tools/shell-tests/layout-$c.txt" "$@" \
         tools/qemu-test.sh "$out" "$name-$c" shell > "$out/$name-$c.out" 2>&1; then
        echo "$name: $c: the shell script FAILED (see $out/$name-$c.log)"
        grep -a "serial-feed: .*no '" "$out/$name-$c.out" || true
        ok=0
    fi
    run="$out/$name-$c.log"
    never "$c" "extfs.*(crashed|giving up)|posixtest: FAILED" "$run"
}

case_drive() {
    stick_with ext "$out/$name-drive-stick.img" || { ok=0; return; }
    saved="$out/$name-drive-saved.img"
    run_case drive "$out/$name-drive-stick.img" QEMU_BOOT_NVME=1 QEMU_SAVE="$saved"
    want drive 1 "volumes: nvme[0-9]+ partition 2 -> bin/extfs at /data \(OK\)" "$run"
    want drive 1 "volumes: nvme[0-9]+: the machine booted from this Jam OS drive: its data partition is /jam too" "$run"
    # one service: extfs /data (no extfs-jam), and /tmp never /data/tmp's view
    want drive 1 "extfs /data: mounted" "$run"
    never drive "extfs /jam|extfs-jam|init: /tmp: a view of /data/tmp" "$run"
    [ -f "$saved" ] || { echo "$name: drive: no saved drive"; ok=0; return; }
    ext_clean drive "$saved"
    on_volume drive /one.txt one-d
    on_volume drive /two.txt two-d
    on_volume drive /usr/u.txt usr-d
    on_volume drive /tmp/t.txt tmp-d
    rm -f "$saved" "$vol" "$out/$name-drive-stick.img"
}

case_both() {
    stick_with ext "$out/$name-both-ext.img" || { ok=0; return; }
    drive="$out/$name-both-drive.img"
    python3 tools/mkimage.py --gpt "$out/$name-both-ext.img" "$drive" ||
        { echo "$name: both: the drive couldn't be made"; ok=0; return; }
    rm -f "$out/$name-both-ext.img"
    stick_with fat "$out/$name-both-stick.img" || { ok=0; return; }
    run_case both "$out/$name-both-stick.img" QEMU_NVME="$drive"
    want both 1 "volumes: nvme[0-9]+ partition 2 -> bin/extfs at /jam \(OK\)" "$run"
    want both 1 "volumes: nvme[0-9]+ partition 2 is /jam, read-write" "$run"
    never both "the machine booted from this Jam OS drive" "$run"
    ext_clean both "$drive"
    on_volume both /d.txt drive-b
    on_volume both /var/v.txt var-b
    debugfs -R "stat /s.txt" "$vol" 2>&1 | grep -q "not found" ||
        { echo "$name: both: the stick's /data/s.txt is on the drive"; ok=0; }
    rm -f "$drive" "$vol" "$out/$name-both-stick.img"
}

case_stick() {
    stick_with fat "$out/$name-stick-stick.img" || { ok=0; return; }
    run_case stick "$out/$name-stick-stick.img"
    want stick 1 "init: /tmp: in memory \([0-9]+ MiB at most, a pager of [0-9]+ pages\)" "$run"
    never stick "init: /jam mounted|init: /pkg: a view" "$run"
    rm -f "$out/$name-stick-stick.img"
}

case_stickext() {
    img="$out/$name-stickext-stick.img"
    stick_with ext "$img" || { ok=0; return; }
    saved="$out/$name-stickext-saved.img"
    run_case stickext "$img" QEMU_SAVE="$saved"
    never stickext "init: /jam mounted" "$run"
    [ -f "$saved" ] || { echo "$name: stickext: no saved stick"; ok=0; return; }
    ext_clean stickext "$saved"
    on_volume stickext /pkg/p.txt pkg-e
    on_volume stickext /tmp/t.txt tmp-e
    rm -f "$saved" "$vol" "$img"
}

for c in $cases; do
    case $c in
    drive) case_drive ;;
    both) case_both ;;
    stick) case_stick ;;
    stickext) case_stickext ;;
    *) echo "$name: no case called $c"; ok=0 ;;
    esac
done
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL"
exit 1
