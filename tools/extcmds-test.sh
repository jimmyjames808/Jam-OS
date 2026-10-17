#!/bin/bash
# The shell's file commands on an ext volume (docs/M13.6-PLAN.md, U8), in
# QEMU, on the test PC (it needs mtools for the ESP and, for a second look,
# e2fsprogs):
#
#     JAMOS_REMOTE=jamos-pc JAMOS_REMOTE_ONLY=1 \
#         tools/remote-test.sh tools/extcmds-test.sh build/t/extcmds
#
# A Jam OS drive (tools/mkgpt.py's jamos layout; QEMU's NVMe) whose data
# partition is an ext volume jamext (tools/jamext) made from a directory:
# the stick boots, volumes starts bin/extfs on that partition as /jam, and
# tools/shell-tests/extcmds.txt types ls -l, stat, ln, ln -s, chmod,
# readlink and allow at the shell. Then, on the host: `jamext check`
# calls the volume clean (the journal replayed first), Linux's e2fsck -fn
# does too when it is installed, and the links, modes and files the shell
# made are read back with jamext. Needs M13.6's U7a (volumes mounting an
# ext data partition as /jam).
#
# Into <outdir>: the drive's files while the run lasts, extcmds.log and
# extcmds.png from qemu-test.sh, the checks' output. Exit 0: PASS.
set -u
out=${1:?usage: tools/extcmds-test.sh <outdir>}
PATH=/usr/sbin:/sbin:$PATH
for t in mformat; do
    command -v $t > /dev/null 2>&1 || { echo "extcmds: FAIL: no $t here (mtools)"; exit 1; }
done
make -s build/host/jamext || { echo "extcmds: FAIL: jamext didn't build"; exit 1; }
jx=build/host/jamext
rm -rf "$out"
mkdir -p "$out/tree/docs"
jam="$out/jamos.img" ext="$out/data.img" esp="$out/esp.img"
echo "hello from the host tool" > "$out/tree/docs/hello.txt"
printf '#!/bin/sh\necho hi\n' > "$out/tree/docs/run.sh"
chmod 755 "$out/tree/docs/run.sh"
ln -s hello.txt "$out/tree/docs/ln-hello"
ln "$out/tree/docs/hello.txt" "$out/tree/docs/hard"
$jx --seed 11 --time 1700000000 -q mkimage "$ext" 64M "$out/tree" ||
    { echo "extcmds: FAIL: jamext mkimage"; exit 1; }
$jx check "$ext" > "$out/check-before.txt" 2>&1 ||
    { echo "extcmds: FAIL: the image jamext made isn't clean (see $out/check-before.txt)"; exit 1; }
truncate -s 128M "$esp"
mformat -i "$esp" -T $((128 * 2048)) -F -v JAMOS-ESP :: || { echo "extcmds: FAIL: mformat"; exit 1; }
python3 tools/mkgpt.py "$jam" --layout jamos --size 1G --fill "1=$esp" --fill "2=$ext" > /dev/null ||
    { echo "extcmds: FAIL: mkgpt"; exit 1; }
rm -f "$esp"
# The stick's /data FAT (the default image's is ext): `disk check /data` is
# the checker's answer for a volume that isn't ext.
. tools/datafs.sh
stick="$out/fat-stick.img"
data_fat_image "$stick" || { echo "extcmds: FAIL: no FAT stick"; exit 1; }
ok=1
QEMU_TIMEOUT=${QEMU_TIMEOUT:-420} QEMU_IMAGE="$stick" QEMU_NVME="$jam" \
    QEMU_INPUT=${EXTCMDS_INPUT:-tools/shell-tests/extcmds.txt} \
    tools/qemu-test.sh "$out" extcmds shell > "$out/qemu.out" 2>&1 ||
    { echo "extcmds: FAIL: the run failed (see $out/qemu.out)"; grep -a "serial-feed: .*no '" "$out/qemu.out" || true; ok=0; }
# the volume on the host: partition 2 of the drive, its journal replayed, then clean
$jx --part 2 check --replay "$jam" > "$out/check-after.txt" 2>&1 ||
    { echo "extcmds: FAIL: jamext check of the volume after the run (see $out/check-after.txt)"; ok=0; }
dd if="$jam" of="$out/data.out" bs=1M skip=513 count=64 2> /dev/null
if command -v e2fsck > /dev/null 2>&1; then
    e2fsck -fn "$out/data.out" > "$out/e2fsck.txt" 2>&1 ||
        { echo "extcmds: FAIL: e2fsck -fn says the volume isn't clean (see $out/e2fsck.txt)"; ok=0; }
fi
# what the shell made: links, a hard link made and one unlinked, modes
ls_t=$($jx --part 2 ls -l "$jam" /t 2> /dev/null)
echo "$ls_t" > "$out/ls-t.txt"
echo "$ls_t" | grep -Eq '^lrwxrwxrwx +1 .* b -> a$' || { echo "extcmds: FAIL: /t/b isn't a link to a"; ok=0; }
echo "$ls_t" | grep -Eq '^lrwxrwxrwx +1 .* dangling -> nowhere$' || { echo "extcmds: FAIL: /t/dangling"; ok=0; }
echo "$ls_t" | grep -Eq '^lrwxrwxrwx +1 .* abs -> /jam/t/a$' || { echo "extcmds: FAIL: /t/abs"; ok=0; }
echo "$ls_t" | grep -Eq '^-rw------- +2 .* c$' || { echo "extcmds: FAIL: /t/c's mode and links"; ok=0; }
$jx --part 2 get "$jam" /t/c "$out/c.txt" 2> /dev/null && [ "$(wc -l < "$out/c.txt" | tr -d ' ')" = 2 ] ||
    { echo "extcmds: FAIL: /t/c isn't the two lines the shell wrote"; ok=0; }
$jx --part 2 get "$jam" /docs/hello.txt "$out/hello.txt" 2> /dev/null &&
    cmp -s "$out/hello.txt" "$out/tree/docs/hello.txt" || { echo "extcmds: FAIL: docs/hello.txt changed"; ok=0; }
if [ $ok = 1 ]; then
    rm -rf "$out/tree" "$out/data.out" "$ext" "$jam" "$stick"
    echo "extcmds: PASS"
else
    echo "extcmds: FAIL (see $out)"
    exit 1
fi
