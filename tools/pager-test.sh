#!/bin/sh
# Mapped files and the pagers behind them (docs/M12.5-PLAN.md). The cases
# named in PAGER_CASES (default: all of them, one after the other; give each
# its own output directory to run them side by side), each a shell boot
# (tools/shell-tests/pager-<case>.txt) and a look at its log:
#   plumb  what volumes and libos do before fat serves a page: one pager a
#          mount (made with the mount, the same one across fat-data's
#          restart and the spare's promotion, closed with the stick), the
#          budget a sixteenth of the memory, `pagertest self` over plain
#          VMOs, and file.map answered ERR_NOT_SUPPORTED by fat through the
#          kept-channel machinery. Later tracks replace the last part with
#          real pages and add the cases below.
# Later cases (named here by the tracks that add them): map, write, pull, exec.
# QEMU_SMP and QEMU_MEM pass through.
# Usage: tools/pager-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-pager}
cases=${PAGER_CASES:-plumb}
mkdir -p "$out"
ok=1

count() { grep -acE -- "$1" "$2" || true; }
# want <case> <how many times exactly> <text (a regex)> <log>
want() {
    n=$(count "$3" "$4")
    [ "$n" -eq "$2" ] || { echo "$name: $1: \"$3\" $n times in $(basename "$4"), wanted $2"; ok=0; }
}
never() {   # never <case> <text> <log>
    n=$(count "$2" "$3")
    [ "$n" -eq 0 ] || { echo "$name: $1: \"$2\" $n times in $(basename "$3")"; ok=0; }
}

# A 64 MiB stick with one FAT32 partition (type 0c) holding pager.bin, 2 MiB
# of the same bytes every time, as sticks-test.sh makes its sticks.
make_stick() {
    stick=$1
    python3 tools/mkstick.py "$stick" 64 0c
    mformat -i "$stick@@1M" -T $((63 * 2048)) -F -v PAGER-A ::
    python3 -c "import random, sys; sys.stdout.buffer.write(random.Random(11).randbytes(2 << 20))" \
        > "$stick.bin"
    mcopy -i "$stick@@1M" "$stick.bin" ::/pager.bin
    rm -f "$stick.bin"
}

case_plumb() {
    stick="$out/$name-plumb-a.img"
    make_stick "$stick"
    if ! QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/pager-plumb.txt \
         QEMU_USB="-drive if=none,id=aimg,format=raw,file=$stick" \
         tools/qemu-test.sh "$out" "$name-plumb" shell > "$out/$name-plumb.out" 2>&1; then
        echo "$name: plumb: the shell script FAILED (see $out/$name-plumb.log)"
        grep "serial-feed: .*no '" "$out/$name-plumb.out" || true
        ok=0
    fi
    log="$out/$name-plumb.log"
    # Up to the `reboot -f` at the end: the shutdown closes everything.
    sed '/jam[^>]*>.* reboot -f/q' "$log" > "$out/$name-plumb.run.log"
    run="$out/$name-plumb.run.log"
    # One pager a mount, made with its first instance and kept: fat-data was
    # killed once (a spare promoted) and fat-usb0 once, and neither made
    # another.
    want plumb 1 "volumes: fat-data: pager made: " "$run"
    want plumb 1 "volumes: fat-esp: pager made: " "$run"
    want plumb 1 "volumes: fat-usb0: pager made: " "$run"
    want plumb 1 "volumes: fat-usb0: pager closed" "$run"
    never plumb "starts without a pager" "$run"
    never plumb "no pager power" "$run"
    never plumb "fat-data: pager closed" "$run"
    # The budget is a sixteenth of the memory (QEMU_MEM, 2G by default).
    pages=$(sed -n 's/.*volumes: fat-data: pager made: \([0-9]*\) pages.*/\1/p' "$run" | head -1)
    memarg=${QEMU_MEM:-2G}
    case $memarg in
    *G) mib=$(( ${memarg%G} * 1024 )) ;;
    *M) mib=${memarg%M} ;;
    *) mib=2048 ;;
    esac
    mem=$((mib * 256))   # pages in the machine, about
    if [ -z "$pages" ] || [ "$pages" -lt $((mem / 20)) ] || [ "$pages" -gt $((mem / 14)) ]; then
        echo "$name: plumb: the budget is ${pages:-none} pages, wanted about $((mem / 16))"
        ok=0
    fi
    want plumb 3 "answers within 60 s" "$run"
    never plumb "did not end cleanly|left [0-9]+ units of job kind" "$run"
    rm -f "$stick"
}

for c in $cases; do
    case $c in
    plumb) case_plumb ;;
    *) echo "$name: no case called $c"; ok=0 ;;
    esac
done
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL"
exit 1
