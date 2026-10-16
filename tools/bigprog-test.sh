#!/bin/sh
# Programs as big as Chromium's on /data (docs/CR-PLAN.md section 6, track
# CP1). The program is build/bigprog/bigprog: bin/bigprog with a table of
# 76,800 pages (300 MiB of constant data), generated here (`make
# build/bigprog/bigprog`), never kept in the tree. It goes on a stick
# whose /data is grown to DATA_MIB (the image's 64 MiB is too small), as
# /data/big, and runs in two boots of that stick (their headers say every
# step):
#   boot 1 (bigprog.txt): refused until allowed; `allow` says how far it
#          got while it reads and hashes the 300 MiB, the question comes,
#          and its line and page table (2.4 MB, named by its hash) are
#          written; then it runs from its pages: `lazy` touches none of the
#          table and fetches a few of its pages (the fs's "gone" line between
#          the marks), `check` reads every 64th page of the table and its
#          last, each holding the right words.
#   then this script changes one byte of a page of the table that `check`
#   reads, on this side of the image, as the Mac would (the size kept);
#   boot 2 (bigprog2.txt): `lazy` still runs (the changed page isn't
#          touched), `check` dies at the changed page ("a page doesn't
#          match the approved program"), and the shell lives.
# QEMU_SMP and QEMU_MEM pass through; DATA_MIB (448) sizes /data.
# Usage: tools/bigprog-test.sh <outdir> (after `make -s image`); exit 0 on PASS.
set -u
. tools/datafs.sh
out=$1
name=bigprog
data_mib=${DATA_MIB:-448}
changed=38400   # the table's page changed for boot 2: one `check` reads (64 * 600)
mkdir -p "$out"
ok=1

count() { grep -acE -- "$1" "$2" || true; }
want() {   # want <how many times exactly> <text (a regex)> <log>
    n=$(count "$2" "$3")
    [ "$n" -eq "$1" ] || { echo "$name: \"$2\" $n times in $(basename "$3"), wanted $1"; ok=0; }
}
never() {   # never <text> <log>
    n=$(count "$1" "$2")
    [ "$n" -eq 0 ] || { echo "$name: \"$1\" $n times in $(basename "$2")"; ok=0; }
}
# gone_pages <from> <to>: the fs's (fat or extfs) "gone" lines for /data's mapped files
# between the output lines <from> and <to> of $run, as the pages each
# supplied, one a line.
gone_pages() {
    sed -n "/$1/,/$2/p" "$run" |
        sed -n -E 's/.*(fat|extfs) \/data: mapped file [0-9a-f]* gone: [0-9]* page requests, ([0-9]*) pages supplied.*/\2/p'
}

prog=build/bigprog/bigprog
if ! make -s "$prog" > "$out/$name-make.log" 2>&1; then
    echo "$name: can't make $prog (see $out/$name-make.log)"
    exit 1
fi
size=$(wc -c < "$prog" | tr -d ' ')
pages=$(( (size + 4095) / 4096 ))
sha=$(sha256sum "$prog" | cut -d' ' -f1)
echo "$name: $prog is $size bytes ($((size >> 20)) MiB), $pages pages"

# The stick: the image with its data partition grown to data_mib (the MBR
# as tools/write-usb.sh grows a stick's), formatted here, /data/big on it.
stick="$out/$name-stick.img"
cp build/jamos.img "$stick" &&
    data_grow "$stick" "$data_mib" &&
    data_put "$stick" "$prog" big 755 ||
    { echo "$name: can't make the stick"; rm -f "$stick"; exit 1; }

# ---- boot 1: allowed, run ---------------------------------------------------------------------
saved="$out/$name-stick2.img"
if ! QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} QEMU_INPUT=tools/shell-tests/bigprog.txt \
     QEMU_IMAGE="$stick" QEMU_SAVE="$saved" tools/qemu-test.sh "$out" "$name" shell \
     > "$out/$name.out" 2>&1; then
    echo "$name: boot 1's shell script FAILED (see $out/$name.log)"
    grep "serial-feed: .*no '" "$out/$name.out" || true
    rm -f "$stick" "$saved"
    exit 1
fi
rm -f "$stick"
run="$out/$name.run.log"
sed '/jam[^>]*>.* reboot -f/q' "$out/$name.log" > "$run"
# The progress line, both times the whole file was read (the refusal
# explained from its bytes, and `allow`), each to 100 % (a log line may
# come between its pieces).
want 2 "checking /data/big \($((size >> 20)) MiB\): 10%" "$run"
want 2 "90% 100%" "$run"
# The table: one file, named by the program's hash.
want 1 "$sha" "$run"
# Its table untouched, it fetched a few of its pages: far fewer than 1 %.
lazy=$(gone_pages mark-b-lazy0 mark-b-lazy1)
if [ "$(printf '%s\n' "$lazy" | grep -c .)" -ne 1 ] || [ "$lazy" -gt $((pages / 100)) ]; then
    echo "$name: the lazy run's pages supplied: '${lazy:-none}', wanted one VMO and at most" \
         "$((pages / 100)) of its $pages"
    ok=0
fi
echo "$name: the lazy run fetched ${lazy:-?} of $pages pages"
grep -a "bigprog: checked" "$run" | sed "s/^.*bigprog:/$name: boot 1:/" | head -2
never "page fault|don't match the approved|doesn't match its approval" "$run"
never "did not end cleanly|left [0-9]+ units of job kind|its job still holds" "$run"

# ---- on this side: a byte of the table's page `changed` ---------------------------------------
copy="$out/$name-changed"
python3 - "$prog" "$copy" "$changed" <<'EOF' ||
import struct, sys
src, dst, page = sys.argv[1], sys.argv[2], int(sys.argv[3])
d = bytearray(open(src, "rb").read())
start = d.index(struct.pack("<Q", 1) * 512) - 4096   # page 1 of the table, page-aligned
assert start % 4096 == 0 and d[start + page * 4096:start + page * 4096 + 8] == struct.pack("<Q", page)
d[start + page * 4096 + 100] ^= 0x5a
open(dst, "wb").write(d)
EOF
    { echo "$name: can't change the program"; ok=0; }
data_put "$saved" "$copy" big 755 ||
    { echo "$name: can't put the changed program on the stick"; ok=0; }
rm -f "$copy"

# ---- boot 2: the changed page refused --------------------------------------------------------------
if ! QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} QEMU_INPUT=tools/shell-tests/bigprog2.txt \
     QEMU_IMAGE="$saved" tools/qemu-test.sh "$out" "$name"2 shell > "$out/$name"2.out 2>&1; then
    echo "$name: boot 2's shell script FAILED (see $out/${name}2.log)"
    grep "serial-feed: .*no '" "$out/$name"2.out || true
    ok=0
fi
rm -f "$saved"
run="$out/${name}2.run.log"
sed '/jam[^>]*>.* reboot -f/q' "$out/${name}2.log" > "$run"
want 1 'user: process "big" killed: page fault: a page doesn.t match the approved program' "$run"
n=$(count "(fat|extfs) /data: mapped file [0-9a-f]+: the disk.s pages don.t match the approved" "$run")
[ "$n" -ge 1 ] || { echo "$name: the fs's mismatch line $n times, wanted 1 or more"; ok=0; }
never 'user: process "shell" killed' "$run"
never "did not end cleanly|left [0-9]+ units of job kind|its job still holds" "$run"

if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL"
exit 1
