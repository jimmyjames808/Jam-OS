#!/bin/sh
# The stick's filesystems end to end: three boots of one stick image in
# QEMU, each typed into by a shell script (tools/shell-tests/).
#   boot 1  data-1.txt: /esp (read-only) and /data mounted through
#           usb-storage, fat, devmgr and init; a file written; the fat
#           service and devmgr killed and the mounts back; `reboot` (init
#           syncs /data)
#   boot 2  data-2.txt: the file is there, the first boot's log can be
#           read, this boot has its own; then QEMU quits in the middle of
#           a run of writes (the pulled plug)
#   boot 3  data-3.txt: the stick still boots, /data is dirty and mounted
#           anyway, everything synced before the pull is there
# Then the stick is read from this side with mtools, as the Mac reads the
# real one: the boot logs and the files are on its JAMOS-DATA partition.
# QEMU_SMP and QEMU_XHCI pass through.
# Usage: tools/data-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-data}
mkdir -p "$out"
stick="$out/$name-stick.img"
ok=1
image=build/jamos.img
for n in 1 2 3; do
    if ! QEMU_TIMEOUT=${QEMU_TIMEOUT:-240} QEMU_IMAGE="$image" QEMU_SAVE="$stick" \
         QEMU_INPUT="tools/shell-tests/data-$n.txt" tools/qemu-test.sh "$out" "$name-$n" shell; then
        echo "$name: boot $n FAILED (see $out/$name-$n.log)"
        ok=0
        break
    fi
    image=$stick
done
# The data partition starts at 64 MiB (the Makefile's ESP_END_MIB).
data="$stick@@64M"
want() {
    "$@" > /dev/null 2>&1 || { echo "$name: on the stick: '$*' failed"; ok=0; }
}
if [ $ok = 1 ]; then
    want mdir -i "$data" ::/logs/boot-0001.txt ::/logs/boot-0002.txt ::/logs/boot-0003.txt
    want mdir -i "$data" ::/after.txt
    mtype -i "$data" ::/logs/boot-0001.txt 2>/dev/null | grep -q "init: /data mounted" ||
        { echo "$name: boot-0001.txt on the stick doesn't hold the first boot's log"; ok=0; }
    mtype -i "$data" ::/after.txt 2>/dev/null | grep -q "after the pull-c" ||
        { echo "$name: after.txt on the stick doesn't hold what boot 3 wrote"; ok=0; }
fi
rm -f "$stick"
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL"
exit 1
