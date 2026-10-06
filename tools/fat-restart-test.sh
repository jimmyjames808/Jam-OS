#!/bin/sh
# fat carries on from a dead instance's state (user/services/fat/adopt.c,
# docs/M11.6-PLAN.md) in QEMU, on the boot stick's /data, through devmgr's
# kills. Two boots of tools/shell-tests/fat-restart.txt: with the warm
# spare, and with `nospare` (a new process adopts the state the same way).
# Checked: every instance that got going carried on (its restart line;
# never "starting fresh"), the mount never goes and nothing is given up
# on; writes never synced before a kill are there (in the guest and on the
# stick afterwards); the copies' checksums are the originals'; and the
# stick's files, read on the host with mtools, are what was written.
# The exact deaths (each step of a request, the bad-request rule, the
# replies that carried handles, FS_GATHER held across deaths, the disks
# byte for byte) are utest's fat_restart_* tests, in the `init` run.
# Prints kill-to-first-answer (devmgr's probe) and fat's own restart time,
# with and without the spare. QEMU_SMP passes through.
# Usage: tools/fat-restart-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
ok=1

for run in spare nospare; do
    words=shell
    [ "$run" = nospare ] && words="shell nospare"
    # shellcheck disable=SC2086 # the boot words are separate arguments
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/fat-restart.txt \
        QEMU_SAVE="$out/fr-$run-stick.img" \
        tools/qemu-test.sh "$out" "fr-$run" $words > "$out/fr-$run.out" 2>&1 ||
        { echo "fat-restart: the $run script failed"; grep "serial-feed: .*no '" "$out/fr-$run.out" || true; ok=0; }
done

count() { grep -ac -- "$1" "$2" || true; }
need() {   # need <min> <text> <log>
    n=$(count "$2" "$3")
    [ "$n" -ge "$1" ] || { echo "fat-restart: $(basename "$3"): \"$2\" $n times, wanted $1 or more"; ok=0; }
}
never() {   # never <text> <log>
    n=$(count "$1" "$2")
    [ "$n" -eq 0 ] || { echo "fat-restart: $(basename "$2"): \"$1\" $n times, wanted none"; ok=0; }
}
stats() {   # stats <label> <pattern> <log>: median and worst of the number before " us"
    grep -ao -- "$2 [0-9]* us" "$3" | awk '{print $(NF-1)}' | sort -n |
        awk -v l="$1" '{v[NR]=$1} END {if (NR) printf "fat-restart: %s: %d samples, median %d us, worst %d us\n", l, NR, v[int((NR+1)/2)], v[NR]}'
}

for run in spare nospare; do
    log="$out/fr-$run.log"
    run_log="$out/fr-$run.run.log"
    sed '/jam[^>]*>.* reboot -f/q' "$log" > "$run_log"   # the shutdown takes the mounts away
    # A kill that lands on an instance still starting (the bursts) ends it
    # before it took anything over: the next one carries on from the same
    # state. So fewer restart lines than kills, never one that starts fresh.
    kills=$(count "devmgr: fat-data bin/fat was killed (KILL)" "$log")
    restarts=$(count "fat /data: restart (killed" "$log")
    [ "$kills" -ge 20 ] || { echo "fat-restart: $run: $kills kills, wanted 20 or more"; ok=0; }
    [ "$restarts" -ge 8 ] ||
        { echo "fat-restart: $run: $restarts restarts that carried on, wanted 8 or more"; ok=0; }
    never "starting fresh" "$log"
    never "giving up" "$log"
    never "init: /data is gone" "$run_log"
    never "svcstate: the state of service" "$log"
    never "couldn't be taken back" "$log"
    never "shell: .*ERR_" "$run_log"
    [ "$(count "init: /data mounted" "$run_log")" -eq 1 ] ||
        { echo "fat-restart: $run: /data mounted more than once"; ok=0; }
    # The guest's checksums: each copy's is its original's.
    for pair in "k1.bin jamos.elf" "k2.bin bootfs.img"; do
        set -- $pair
        a=$(grep -ao "[0-9a-f]\{64\}  /data/fr/$1" "$log" | head -1 | cut -c1-64)
        b=$(grep -ao "[0-9a-f]\{64\}  /esp/boot/$2" "$log" | head -1 | cut -c1-64)
        [ -n "$a" ] && [ "$a" = "$b" ] ||
            { echo "fat-restart: $run: /data/fr/$1 is \"$a\", /esp/boot/$2 \"$b\""; ok=0; }
    done
    # The stick, on the host.
    img="$out/fr-$run-stick.img"
    for f in u1.txt u2.txt u3.txt burst.txt; do
        mtype -i "$img@@64M" "::/fr/$f" > "$out/fr-$run-$f" 2>/dev/null ||
            { echo "fat-restart: $run: /fr/$f isn't on the stick"; ok=0; }
    done
    grep -q "unsynced-1" "$out/fr-$run-u1.txt" && grep -q "again-1" "$out/fr-$run-u1.txt" &&
        grep -q "unsynced-3" "$out/fr-$run-u3.txt" && grep -q "b-2" "$out/fr-$run-burst.txt" ||
        { echo "fat-restart: $run: the stick's files don't hold what was written"; ok=0; }
    mcopy -o -i "$img@@64M" ::/fr/k1.bin "$out/fr-$run-k1.bin" 2>/dev/null &&
        mcopy -o -i "$img@@1M" ::/boot/jamos.elf "$out/fr-$run-jamos.elf" 2>/dev/null &&
        cmp -s "$out/fr-$run-k1.bin" "$out/fr-$run-jamos.elf" ||
        { echo "fat-restart: $run: the stick's /fr/k1.bin isn't its kernel"; ok=0; }
    echo "fat-restart: $run: $kills kills, $restarts instances carried on, none started fresh"
    stats "$run: kill to first answer" "fat-data: kill to first answer" "$log"
    grep -a "fat /data: restart (killed" "$log" | grep -ao "[0-9]* us after the kill" |
        awk '{print $1}' | sort -n | awk -v l="$run" '{v[NR]=$1} END {if (NR) printf "fat-restart: %s: the kill to fat carrying on (promoted spares only): %d samples, median %d us, worst %d us\n", l, NR, v[int((NR+1)/2)], v[NR]}'
done
need 1 "spare promoted" "$out/fr-spare.log"
never "spare promoted" "$out/fr-nospare.log"

[ "$ok" = 1 ] && echo "fat-restart: PASS" && exit 0
echo "fat-restart: FAIL"
exit 1
