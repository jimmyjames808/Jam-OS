#!/bin/sh
# The stick with an ext /data, end to end (M13.6's U11a, "Ext by default"):
# two boots of the default ext image (`make image`) in QEMU, each typed into by a
# shell script (tools/shell-tests/stickext-1.txt, stickext-2.txt).
#   boot 1  the mounts (/data is ext, /esp FAT and read-only), a file with
#           a mode and a symbolic link, a copy of a program, the boot log;
#           `reboot -f` (init syncs /data)
#   boot 2  all of it is there, the first boot's log can be read, this boot
#           has its own; then QEMU quits in the middle of a write (the
#           pulled plug)
# Then the stick is read from this side, as the Mac reads the real one:
# jamext replays the journal and checks the volume, Linux's e2fsck -fn (where
# it is installed: the test PC) agrees, and the files, the mode, the link and
# the boot logs are on it. Exit 2 when the image's data partition is FAT
# (built with DATA_FS=fat: JAMOS_DATA_FS=fat).
# QEMU_SMP and QEMU_XHCI pass through.
# Usage: tools/stickext-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
export PATH=/usr/sbin:/sbin:$PATH LC_ALL=C
JX=build/host/jamext
out=$1 name=${2:-stickext}
mkdir -p "$out"
[ "$(python3 tools/mkimage.py --data-fs build/jamos.img)" = ext ] ||
    { echo "$name: build/jamos.img's data partition is not ext (built with DATA_FS=fat?)"; exit 2; }
make -s "$JX" || { echo "$name: can't build jamext"; exit 1; }
stick="$out/$name-stick.img"
ok=1
image=build/jamos.img
for n in 1 2; do
    if ! QEMU_TIMEOUT=${QEMU_TIMEOUT:-240} QEMU_IMAGE="$image" QEMU_SAVE="$stick" \
         QEMU_INPUT="tools/shell-tests/stickext-$n.txt" tools/qemu-test.sh "$out" "$name-$n" shell; then
        echo "$name: boot $n FAILED (see $out/$name-$n.log)"
        ok=0
        break
    fi
    image=$stick
done
want() {
    "$@" > /dev/null 2>&1 || { echo "$name: on the stick: '$*' failed"; ok=0; }
}
if [ $ok = 1 ]; then
    # the plug was pulled in boot 2: the journal is replayed (the way the next mount does)
    "$JX" --part 2 check --replay "$stick" > "$out/$name-check.txt" 2>&1 ||
        { echo "$name: jamext check --replay: $(cat "$out/$name-check.txt")"; ok=0; }
    if command -v e2fsck > /dev/null 2>&1; then
        off=$(python3 tools/mkimage.py --data-at "$stick")
        dd if="$stick" of="$out/$name-data.img" bs=1048576 skip="${off%M}" 2> /dev/null
        e2fsck -fn "$out/$name-data.img" > "$out/$name-fsck.txt" 2>&1 ||
            { echo "$name: e2fsck -fn: $(tail -3 "$out/$name-fsck.txt" | tr '\n' ' ')"; ok=0; }
        rm -f "$out/$name-data.img"
    else
        echo "$name: no e2fsck here: only jamext checked the volume"
    fi
    get() { "$JX" --part 2 get "$stick" "$1" - 2> /dev/null; }
    get "/My Notes/first boot.txt" | grep -q "kept across a reboot-a" ||
        { echo "$name: the first boot's file isn't on the stick"; ok=0; }
    get "/My Notes/second boot.txt" | grep -q "written on boot two-b" ||
        { echo "$name: the second boot's file isn't on the stick"; ok=0; }
    get /utest.copy | head -c 4 | od -An -tx1 | grep -q "7f 45 4c 46" ||
        { echo "$name: /utest.copy is no ELF file"; ok=0; }
    "$JX" --part 2 ls -l "$stick" "/My Notes" 2> /dev/null | grep -q "^-rw-------.* first boot.txt$" ||
        { echo "$name: first boot.txt lost its mode"; ok=0; }
    "$JX" --part 2 ls -l "$stick" "/My Notes" 2> /dev/null | grep -q "^lrwxrwxrwx.* link -> first boot.txt$" ||
        { echo "$name: the link isn't on the stick"; ok=0; }
    get /logs/boot-0001.txt | grep -q "init: /data mounted" ||
        { echo "$name: boot-0001.txt on the stick doesn't hold the first boot's log"; ok=0; }
    # boot 1 ended with `reboot -f`: its log holds the shutdown's own lines
    get /logs/boot-0001.txt | grep -q "init: /data synced in" ||
        { echo "$name: boot-0001.txt on the stick ends before the reboot's sync line"; ok=0; }
    want "$JX" --part 2 ls "$stick" /logs/boot-0002.txt
fi
rm -f "$stick"
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL"
exit 1
