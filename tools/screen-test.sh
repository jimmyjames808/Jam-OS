#!/bin/sh
# The screen on a plain boot, the desktop's (tools/shell-tests/screen.txt;
# before M12's Q14 this was the full-screen console's own screen, booted
# with `nocomp`), booted with the splash (QEMU_SPLASH=1) and so with the
# kernel log off the first terminal's screen, and a stick image "a" (MBR,
# one FAT32 partition) for QEMU's monitor to plug in and pull. The log has
# the desktop's notices ("compositor: notice <n>: ..."): exactly one "USB
# stick added" (`mount -w` and `-r` said nothing), "USB stick removed",
# and the boot stick's "Jam OS stick removed" and "... is back". Then the
# screenshots, with PIL: the shell after the splash shows no log line in
# its window (few pixels of the log's grey, dark-grey stamp or green
# process colours: the desktop around it has none of them), and no RESULTS
# box; `ktest` puts its log lines in the window while it runs (many more
# of them). Then a `verbose` boot: the log is in the first terminal's
# window as it comes. QEMU_SMP passes through.
# Usage: tools/screen-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-screen}
mkdir -p "$out"
a="$out/$name-a.img"
python3 tools/mkstick.py "$a" 64 0c
mformat -i "$a@@1M" -T $((63 * 2048)) -F -v STICK-A ::
echo "hello" > "$out/$name-hello.txt"
mcopy -i "$a@@1M" "$out/$name-hello.txt" ::/hello.txt
rm -f "$out/$name-hello.txt"

ok=1
if ! QEMU_SPLASH=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/screen.txt \
     QEMU_USB="-drive if=none,id=aimg,format=raw,file=$a " \
     tools/qemu-test.sh "$out" "$name" shell > "$out/$name.out" 2>&1; then
    echo "$name: the shell script FAILED (see $out/$name.log)"
    tail -3 "$out/$name.out"
    ok=0
fi
log="$out/$name.log"
n=$(grep -acE "compositor: notice [0-9]+: USB stick added" "$log" || true)
[ "$n" = 1 ] || { echo "$name: 'USB stick added' posted $n times, want 1"; ok=0; }
if grep -aq "RESULTS (read these lines out)" "$log"; then
    echo "$name: a RESULTS box on a plain boot"; ok=0
fi

# The verbose boot: the log in the first terminal's window as ever.
if ! QEMU_SPLASH=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/screen-verbose.txt \
     tools/qemu-test.sh "$out" "$name-verbose" shell verbose > "$out/$name-verbose.out" 2>&1; then
    echo "$name-verbose: the shell script FAILED"
    ok=0
fi

python3 - "$out" <<'PY' || ok=0
import sys
from PIL import Image
out = sys.argv[1]
# The terminal's palette (user/services/console/cellpaint.c): the log's
# grey, its stamps' dark grey, a process's green.
LOG = {(0xb0, 0xb0, 0xb0), (0x70, 0x70, 0x70), (0x66, 0xdd, 0x66)}
def count(path):
    img = Image.open(path).convert("RGB")
    return sum(n for n, c in img.getcolors(1 << 24) if c in LOG)
bad = False
def check(cond, msg):
    global bad
    print(("screen: ok: " if cond else "screen: FAIL: ") + msg)
    bad |= not cond
quiet = {}
for shot in ("screen-boot", "screen-stick", "screen-back"):
    quiet[shot] = count("%s/%s.png" % (out, shot))
    check(quiet[shot] < 300, "%s: no kernel log on the screen (%d log-coloured pixels)"
          % (shot, quiet[shot]))
g = count(out + "/screen-ktest.png")
check(g > quiet["screen-boot"] + 1000, "screen-ktest: ktest's log lines on the screen (%d pixels)" % g)
g = count(out + "/screen-verbose.png")
check(g > quiet["screen-boot"] + 1000, "screen-verbose: verbose: the log on the shell's screen (%d pixels)" % g)
sys.exit(1 if bad else 0)
PY
rm -f "$a"
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $log)"
exit 1
