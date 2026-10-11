#!/bin/sh
# Mapped files and the pagers behind them (docs/M12.5-PLAN.md). The cases
# named in PAGER_CASES (default: all of them, one after the other; give each
# its own output directory to run them side by side), each a shell boot
# (tools/shell-tests/pager-<case>.txt) and a look at its log:
#   plumb  what volumes and libos do: one pager a mount (made with the
#          mount, the same one across fat-data's restart and the spare's
#          promotion, closed with the stick), the budget a sixteenth of
#          the memory, `pagertest self` over plain VMOs, and file.map
#          answered by fat through the kept-channel machinery, also after
#          fat-data and fat-usb0 were killed.
#   map    a file on the second stick and its copy on /data, hashed
#          through a mapping twice (the second pass asks fat for nothing:
#          its "gone" line counts the requests) and through a VMO of our
#          own (FILE_MAP_EXEC, read with vmo_read): every hash the host's
#          sha256sum; a touch on a cue asks for its 64 KiB alone.
#   budget a file half again as big as the mount's budget (QEMU_MEM 512M:
#          a budget of 32 MiB, a 48 MiB file), hashed through a mapping
#          twice: right both times, and the second pass asks for every
#          page again (the first's were evicted, oldest first).
#   kill   that file's pages, twice, while fat-usb0 is killed again and
#          again: the successors take the requests the dead ones left
#          (PAGER_NEXT_AGAIN), the mapper sees the right bytes and no page
#          fails.
#   write  files that change under their mappings: pagertest's own
#          writes and truncates of every kind, on /data and the stick,
#          each seen through the mapping; the shell's `write` emptying a
#          mapped file (a copy past the new end refused, a load there a
#          bus error); `rm` of a mapped file (cut off: a copy ERR_IO, a
#          load killed "the file's pager is gone").
#   pull   the stick pulled under a mapping (QEMU's device_del): a copy
#          from a page read before and one never read both fail ERR_IO in
#          under a second, a load is killed "the file's pager is gone";
#          /data and the shell still answer; plugged back, the file maps
#          and hashes right again.
# Later cases (named here by the tracks that add them): exec.
# QEMU_SMP and QEMU_MEM pass through (budget sets its own QEMU_MEM).
# Usage: tools/pager-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-pager}
cases=${PAGER_CASES:-plumb map budget kill write pull}
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

# put_file <stick> <name> <MiB> <seed>: that many MiB of the same bytes
# every time, as ::/<name>; their SHA-256 into <stick>.<name>.sha.
put_file() {
    python3 -c "import random, sys; sys.stdout.buffer.write(random.Random($4).randbytes($3 << 20))" \
        > "$1.$2"
    sha256sum "$1.$2" | cut -d' ' -f1 > "$1.$2.sha"
    mcopy -i "$1@@1M" "$1.$2" "::/$2"
    rm -f "$1.$2"
}

# make_stick <stick> [MiB]: a 64 MiB stick with one FAT32 partition (type
# 0c), as sticks-test.sh makes its sticks, holding pager.bin (2 MiB) and,
# with MiB, big.bin of that many.
make_stick() {
    python3 tools/mkstick.py "$1" 64 0c
    mformat -i "$1@@1M" -T $((63 * 2048)) -F -v PAGER-A ::
    put_file "$1" pager.bin 2 11
    [ -z "${2:-}" ] || put_file "$1" big.bin "$2" 12
}

# run_case <case> <stick>: the shell script pager-<case>.txt with the stick
# as the drive aimg; its log up to the `reboot -f` at the end (the shutdown
# closes everything) into $run.
run_case() {
    if ! QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT="tools/shell-tests/pager-$1.txt" \
         QEMU_USB="-drive if=none,id=aimg,format=raw,file=$2" \
         tools/qemu-test.sh "$out" "$name-$1" shell > "$out/$name-$1.out" 2>&1; then
        echo "$name: $1: the shell script FAILED (see $out/$name-$1.log)"
        grep "serial-feed: .*no '" "$out/$name-$1.out" || true
        ok=0
    fi
    run="$out/$name-$1.run.log"
    sed '/jam[^>]*>.* reboot -f/q' "$out/$name-$1.log" > "$run"
}

# hashed <case> <path> <sha> <how many>: pagertest's result lines for path,
# each a match with the host's hash (through the mapping or our own VMO,
# and through file_read alike).
hashed() {
    lines=$(grep -a "pagertest: $2: [0-9]* bytes: " "$run" || true)
    n=$(printf '%s\n' "$lines" | grep -c "sha256 $3: read the same" || true)
    [ "$n" -eq "$4" ] ||
        { echo "$name: $1: $n of $4 hashes of $2 are the host's ($3)"; echo "$lines"; ok=0; }
}

# Nothing went wrong with a page, in fat or in a mapper.
no_page_errors() {
    never "$1" "MISMATCH|page fault|doesn't know|can't detach|can't read|can't fail" "$run"
    never "$1" "pages not supplied|can't wait on the pager|don't match the approved" "$run"
    never "$1" "did not end cleanly|left [0-9]+ units of job kind" "$run"
}

case_plumb() {
    stick="$out/$name-plumb-a.img"
    make_stick "$stick"
    run_case plumb "$stick"
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
    hashed plumb /data/pager.txt "$(printf 'a file to map\n' | sha256sum | cut -d' ' -f1)" 1
    no_page_errors plumb
    rm -f "$stick" "$stick".*
}

case_map() {
    stick="$out/$name-map-a.img"
    make_stick "$stick"
    run_case map "$stick"
    sha=$(cat "$stick.pager.bin.sha")
    hashed map /usb0/pager.bin "$sha" 2
    hashed map /data/pager.bin "$sha" 2
    want map 1 "^$sha  */usb0/pager.bin" "$run"
    # Each mapping hashed twice asked for each 64 KiB once, the second pass
    # for nothing (both passes right); each VMO of our own the same once.
    want map 2 "pagertest: pass [12] of /usb0/pager.bin: 2097152 bytes at 0 in [0-9]+ ms: sha256 $sha" "$run"
    want map 2 "pagertest: pass [12] of /data/pager.bin: 2097152 bytes at 0 in [0-9]+ ms: sha256 $sha" "$run"
    want map 2 "fat /usb0: mapped file [0-9a-f]+ gone: 32 page requests, 512 pages supplied" "$run"
    want map 2 "fat /data: mapped file [0-9a-f]+ gone: 32 page requests, 512 pages supplied" "$run"
    # The touch at 4096 asked for the 64 KiB from there.
    want map 1 "fat /data: mapped file [0-9a-f]+ gone: 1 page requests, 16 pages supplied" "$run"
    no_page_errors map
    rm -f "$stick" "$stick".*
}

case_budget() {
    stick="$out/$name-budget-a.img"
    make_stick "$stick" 48
    QEMU_MEM=512M run_case budget "$stick"
    sha=$(cat "$stick.big.bin.sha")
    pages=$(sed -n 's/.*volumes: fat-usb0: pager made: \([0-9]*\) pages.*/\1/p' "$run" | head -1)
    if [ -z "$pages" ] || [ "$pages" -ge $((48 * 256)) ]; then
        echo "$name: budget: the budget is ${pages:-none} pages: not smaller than the file"
        ok=0
    fi
    hashed budget /usb0/big.bin "$sha" 1
    want budget 2 "pagertest: pass [12] of /usb0/big.bin: 50331648 bytes at 0 in [0-9]+ ms: sha256 $sha" "$run"
    # Both passes asked for all of it: 768 requests of 16 pages each.
    want budget 1 "fat /usb0: mapped file [0-9a-f]+ gone: 1536 page requests, 24576 pages supplied" "$run"
    no_page_errors budget
    rm -f "$stick" "$stick".*
}

case_kill() {
    stick="$out/$name-kill-a.img"
    make_stick "$stick" 48
    run_case kill "$stick"
    sha=$(cat "$stick.big.bin.sha")
    hashed kill /usb0/big.bin "$sha" 2
    n=$(count "fat /usb0: restart \(killed" "$run")
    [ "$n" -ge 6 ] || { echo "$name: kill: $n restarts of fat /usb0 carried on, wanted 6 or more"; ok=0; }
    never kill "fat /usb0: restart .*starting fresh" "$run"
    no_page_errors kill
    rm -f "$stick" "$stick".*
}

case_write() {
    stick="$out/$name-write-a.img"
    make_stick "$stick"
    run_case write "$stick"
    want write 1 "pagertest: write /data/w.bin: PASS" "$run"
    want write 1 "pagertest: write /usb0/w.bin: PASS" "$run"
    want write 1 "pagertest: touch /data/t.bin at 1048576 by a copy: ERR_INVALID_ARGS" "$run"
    want write 1 "pagertest\" killed: bus error: page past the end of a shrunk VMO" "$run"
    want write 1 "pagertest: touch /data/u.bin at 4096 by a copy: ERR_IO" "$run"
    want write 1 "pagertest\" killed: page fault: the file's pager is gone" "$run"
    want write 2 "fat /data: mapped file [0-9a-f]+ cut off: its file was removed" "$run"
    never write "MISMATCH|doesn't know|can't detach|can't read|can't fail" "$run"
    never write "can't follow its size|can't drop its old pages|couldn't read it" "$run"
    never write "pages not supplied|can't wait on the pager|don't match the approved" "$run"
    never write "did not end cleanly|left [0-9]+ units of job kind" "$run"
    rm -f "$stick" "$stick".*
}

case_pull() {
    stick="$out/$name-pull-a.img"
    make_stick "$stick" 16
    run_case pull "$stick"
    sha=$(cat "$stick.big.bin.sha")
    # Both runs read the first half (8 MiB) before the pull, right.
    want pull 2 "pagertest: pull /usb0/big.bin: read 8388608 bytes: sha256 " "$run"
    for at in 0 8388608; do
        line=$(grep -a "pagertest: pull /usb0/big.bin: a copy at $at: " "$run" | head -1 || true)
        case $line in
        *": ERR_IO in "*" ms"*)
            ms=${line##*ERR_IO in }
            ms=${ms%% ms*}
            [ "$ms" -lt 1000 ] ||
                { echo "$name: pull: the copy at $at failed after $ms ms, wanted under 1000"; ok=0; } ;;
        *) echo "$name: pull: the copy at $at: \"${line:-no line}\", wanted ERR_IO"; ok=0 ;;
        esac
    done
    want pull 1 "pagertest\" killed: page fault: the file's pager is gone" "$run"
    never pull "still readable|couldn't read it|didn't answer" "$run"
    want pull 2 "volumes: fat-usb0: pager closed" "$run"
    hashed pull /usb0/big.bin "$sha" 1
    never pull "MISMATCH|can't detach|can't fail|pages not supplied|can't wait on the pager" "$run"
    rm -f "$stick" "$stick".*
}

for c in $cases; do
    case $c in
    plumb) case_plumb ;;
    map) case_map ;;
    budget) case_budget ;;
    kill) case_kill ;;
    write) case_write ;;
    pull) case_pull ;;
    *) echo "$name: no case called $c"; ok=0 ;;
    esac
done
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL"
exit 1
