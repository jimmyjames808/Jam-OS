#!/bin/sh
# Other people's USB sticks: one shell boot (tools/shell-tests/sticks.txt)
# with five more disks that the script plugs in and pulls through QEMU's
# monitor, so the order they are found in is the script's:
#   a  an MBR with one FAT32 partition (type 0c), read at 2 MiB/s so a big
#      file takes seconds: mounted read-only at /usb0, its files read and
#      copied, every change refused; `mount -w`, a file written, `mount
#      -r`, changes refused again; pulled while a file is being read
#   b  no partition table, one FAT32 volume (a "superfloppy"): /usb1 next
#      to /usb0; only ever read
#   f  like a, written at 1 MiB/s: made writable, then pulled in the middle
#      of a copy of a big file onto it
#   c  an MBR with a blank partition of type 0c (what a freshly flashed Jam
#      OS data partition looks like) and a type 07 partition of noise
#   d  noise from the first byte to the last: no table, no filesystem
# Then the images are looked at from this side. a holds the file written
# while it was writable (mtools) and none of the refused ones; b, c and d
# are byte for byte what they were: a stick that is only read, or that
# holds nothing Jam OS can mount, is never written, let alone formatted.
# QEMU_SMP and QEMU_XHCI pass through.
# Usage: tools/sticks-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-sticks}
mkdir -p "$out"
a="$out/$name-a.img" b="$out/$name-b.img" c="$out/$name-c.img" d="$out/$name-d.img"
f="$out/$name-f.img"
tmp="$out/$name-files"
mkdir -p "$tmp"
echo "hello from stick a" > "$tmp/hello.txt"
echo "a nested file" > "$tmp/nested.txt"
echo "hello from the superfloppy" > "$tmp/floppy.txt"
echo "stick f was here" > "$tmp/f.txt"
python3 -c "import random, sys; sys.stdout.buffer.write(random.Random(7).randbytes(24 << 20))" \
    > "$tmp/big.bin"

python3 tools/mkstick.py "$a" 64 0c
mformat -i "$a@@1M" -T $((63 * 2048)) -F -v STICK-A ::
mmd -i "$a@@1M" ::/photos
mcopy -i "$a@@1M" "$tmp/hello.txt" "$tmp/big.bin" ::/
mcopy -i "$a@@1M" "$tmp/nested.txt" ::/photos/
python3 tools/mkstick.py "$b" 64
mformat -i "$b" -T $((64 * 2048)) -F -v FLOPPY ::
mcopy -i "$b" "$tmp/floppy.txt" ::/
python3 tools/mkstick.py "$f" 64 0c
mformat -i "$f@@1M" -T $((63 * 2048)) -F -v STICK-F ::
mcopy -i "$f@@1M" "$tmp/f.txt" "$tmp/big.bin" ::/
python3 tools/mkstick.py "$c" 16 0c:8 07 --fill
dd if=/dev/zero of="$c" bs=1048576 seek=1 count=8 conv=notrunc 2>/dev/null   # a blank 0c
python3 tools/mkstick.py "$d" 8 --fill
for i in b c d; do
    eval "cp \"\$$i\" \"\$$i.before\""
done

ok=1
drive() {
    printf -- '-drive if=none,id=%simg,format=raw,file=%s%s ' "$1" "$2" "${3:-}"
}
if ! QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/sticks.txt \
     QEMU_USB="$(drive a "$a" ,throttling.bps-read=2097152)$(drive b "$b")$(drive c "$c")$(drive d "$d")$(drive f "$f" ,throttling.bps-write=1048576)" \
     tools/qemu-test.sh "$out" "$name" shell; then
    echo "$name: the shell script FAILED (see $out/$name.log)"
    ok=0
fi
log="$out/$name.log"
want() {
    grep -qE "$1" "$log" || { echo "$name: no line matching '$1'"; ok=0; }
}
never() {
    ! grep -qE "$1" "$log" || { echo "$name: a line matching '$1'"; ok=0; }
}
want "devmgr: disk [0-9]+ partition 1 is /usb0, read-only"
want "devmgr: disk [0-9]+ partition 1 is /usb0, read-write"
want "devmgr: disk [0-9]+ partition 1 is /usb1, read-only"
want "devmgr: disk [0-9]+ partition 1 \(type 0c\) holds no FAT volume bin/fat can read: left alone"
want "devmgr: disk [0-9]+ partition 2: type 07 is not FAT: left alone"
want "devmgr: disk [0-9]+: no FAT partition to mount: left alone"
never "formatting it|formatted"
never "did not end cleanly|left [0-9]+ units of job kind"

# The images, from this side.
same() {
    cmp -s "$1" "$1.before" || { echo "$name: $2 was written to: $1 differs from $1.before"; ok=0; }
}
same "$b" "the superfloppy, only ever mounted read-only,"
same "$c" "the stick with a blank partition and a foreign one"
same "$d" "the stick without any filesystem"
mtype -i "$a@@1M" ::/new.txt 2>/dev/null | grep -q "written while writable" ||
    { echo "$name: stick a doesn't hold the file written after mount -w"; ok=0; }
for gone in refused.txt refused2.txt newdir; do
    ! mdir -i "$a@@1M" "::/$gone" > /dev/null 2>&1 ||
        { echo "$name: stick a holds $gone, which was written while it was read-only"; ok=0; }
done
mtype -i "$a@@1M" ::/hello.txt 2>/dev/null | grep -q "hello from stick a" ||
    { echo "$name: stick a lost hello.txt"; ok=0; }
mtype -i "$f@@1M" ::/f.txt 2>/dev/null | grep -q "stick f was here" ||
    { echo "$name: stick f lost f.txt"; ok=0; }
rm -rf "$tmp" "$b.before" "$c.before" "$d.before"
if [ $ok = 1 ]; then
    rm -f "$a" "$b" "$c" "$d" "$f"
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $log)"
exit 1
