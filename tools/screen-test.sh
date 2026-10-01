#!/bin/sh
# The screen on a plain boot (tools/shell-tests/screen.txt), booted with
# the splash (QEMU_SPLASH=1) and so with the kernel log off the screen,
# and a stick image "a" (MBR, one FAT32 partition) for QEMU's monitor to
# plug in and pull. Then the screenshots, with PIL: the shell after the
# splash shows no log line (none of the log's grey, dark-grey stamp or
# green process colours) and no RESULTS box; a notice is a yellow line
# above the prompt; `ktest` puts its log lines on the screen while it
# runs. The log has the notices ("console: notice: ...") and exactly one
# "a stick is at /usb0" before the pull (`mount -w` and `-r` said
# nothing). Then a `verbose` boot: the log is on the shell's screen as
# before. QEMU_SMP passes through.
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
n=$(grep -ac "console: notice: a stick is at /usb0" "$log" || true)
[ "$n" = 1 ] || { echo "$name: 'a stick is at /usb0' said $n times, want 1"; ok=0; }
grep -aq "console: selftest PASSED" "$log" || { echo "$name: the console's selftest failed"; ok=0; }
if grep -aq "RESULTS (read these lines out)" "$log"; then
    echo "$name: a RESULTS box on a plain boot"; ok=0
fi

# The verbose boot: the log on the shell's screen as ever.
if ! QEMU_SPLASH=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/screen-verbose.txt \
     tools/qemu-test.sh "$out" "$name-verbose" shell verbose > "$out/$name-verbose.out" 2>&1; then
    echo "$name-verbose: the shell script FAILED"
    ok=0
fi

python3 - "$out" <<'PY' || ok=0
import sys
from PIL import Image
out = sys.argv[1]
# The console's palette (user/services/console/screen.c): the log's grey,
# its stamps' dark grey, a process's green; the notices' yellow (also the
# prompt's).
GREY, DARK, GREEN, YELLOW = (0xb0, 0xb0, 0xb0), (0x70, 0x70, 0x70), (0x66, 0xdd, 0x66), \
                            (0xff, 0xdd, 0x55)
def count(path, colour, rows=None):
    img = Image.open(path).convert("RGB")
    w, h = img.size
    px = img.load()
    y0, y1 = rows if rows else (0, h)
    return sum(1 for y in range(y0, y1) for x in range(w) if px[x, y] == colour)
bad = False
def check(cond, msg):
    global bad
    print(("screen: ok: " if cond else "screen: FAIL: ") + msg)
    bad |= not cond
for shot in ("screen-boot", "screen-stick", "screen-back"):
    p = "%s/%s.png" % (out, shot)
    log = count(p, GREY) + count(p, DARK) + count(p, GREEN)
    check(log == 0, "%s: no kernel log on the screen (%d log-coloured pixels)" % (shot, log))
h = Image.open(out + "/screen-stick.png").size[1]
y = count(out + "/screen-stick.png", YELLOW, (0, h - 16))
check(y > 100, "screen-stick: a yellow notice above the prompt (%d pixels)" % y)
y = count(out + "/screen-boot.png", YELLOW, (0, h - 16))
check(y == 0, "screen-boot: no notice at boot (%d yellow pixels above the prompt)" % y)
g = count(out + "/screen-ktest.png", GREY) + count(out + "/screen-ktest.png", GREEN)
check(g > 1000, "screen-ktest: ktest's log lines on the screen (%d pixels)" % g)
g = count(out + "/screen-verbose.png", GREY) + count(out + "/screen-verbose.png", GREEN)
check(g > 1000, "screen-verbose: verbose: the log on the shell's screen (%d pixels)" % g)
sys.exit(1 if bad else 0)
PY
rm -f "$a"
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $log)"
exit 1
