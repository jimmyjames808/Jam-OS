#!/bin/sh
# devmgr's warm spare fat and the restart rule of a filesystem service
# (user/services/devmgr/spare.c, docs/M11.6-PLAN.md "The warm spare",
# "Supervision") in QEMU, on the boot stick's /data and /esp. Two boots:
#   1. tools/shell-tests/fat-spare.txt: a spare waits from the boot; fat-data
#      is killed again and again while files are written and read through
#      /data (logd writes there all the while); each kill promotes the spare
#      and a new spare starts; the mount never goes; fat-esp once; then ten
#      kills in a row; nothing is given up on (a deliberate kill doesn't
#      count), and the files are all there.
#   2. tools/shell-tests/fat-nospare.txt, with the boot word `nospare`: no
#      spare; each kill starts a new process, on the same kept channel.
# The log is checked for each step, then the numbers are printed: the
# spare's memory (its job's, from `ps`), and the restart and kill-to-first-
# answer times (devmgr's lines; median and worst) with and without a spare.
# QEMU_SMP passes through. Usage: tools/fat-spare-test.sh <outdir>; exit 0
# on PASS.
set -eu
out=$1
mkdir -p "$out"
ok=1

QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/fat-spare.txt \
    tools/qemu-test.sh "$out" fatspare shell > "$out/fatspare.out" 2>&1 ||
    { echo "fat-spare: the spare script failed"; grep "serial-feed: .*no '" "$out/fatspare.out" || true; ok=0; }
QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/fat-nospare.txt \
    tools/qemu-test.sh "$out" fatnospare shell nospare > "$out/fatnospare.out" 2>&1 ||
    { echo "fat-spare: the nospare script failed"; grep "serial-feed: .*no '" "$out/fatnospare.out" || true; ok=0; }
log="$out/fatspare.log"
nolog="$out/fatnospare.log"
# Up to the `reboot -f` at the end: the shutdown takes every mount away.
sed '/jam>.* reboot -f/q' "$log" > "$out/fatspare.run.log"
sed '/jam>.* reboot -f/q' "$nolog" > "$out/fatnospare.run.log"

count() { grep -ac -- "$1" "$2" || true; }
need() {   # need <min> <text> <log>
    n=$(count "$2" "$3")
    [ "$n" -ge "$1" ] || { echo "fat-spare: $(basename "$3"): \"$2\" $n times, wanted $1 or more"; ok=0; }
}
never() {   # never <text> <log>
    n=$(count "$1" "$2")
    [ "$n" -eq 0 ] || { echo "fat-spare: $(basename "$2"): \"$1\" $n times, wanted none"; ok=0; }
}

# Boot 1: 9 kills one by one, all promoted, then 10 in a row (each either
# promoted or a new process: 19 restarts of fat-data), and fat-esp's.
need 19 "devmgr: fat-data bin/fat was killed (KILL): not counted, restarted at once" "$log"
need 9 "devmgr: fat-data: spare promoted" "$log"
need 1 "devmgr: fat-esp: spare promoted" "$log"
need 10 "devmgr: a warm spare bin/fat waits" "$log"
need 1 "devmgr: fat-data: kill to first answer" "$log"
spared=$(count "devmgr: fat-data: spare promoted" "$log")
fresh=$(count "devmgr: fat-data: new process started" "$log")
[ $((spared + fresh)) -ge 19 ] ||
    { echo "fat-spare: fatspare.log: $spared promoted + $fresh started, wanted 19 restarts"; ok=0; }
never "giving up" "$log"
never "init: /data is gone" "$out/fatspare.run.log"
never "init: /esp is gone" "$out/fatspare.run.log"
never "couldn't be promoted" "$log"
never "a spare given no usable promotion" "$log"
never "could not be restarted" "$log"
# Boot 2: no spare at all; each kill a new process.
need 1 "devmgr: nospare: no warm spare" "$nolog"
need 5 "devmgr: fat-data: new process started (no spare waited)" "$nolog"
need 5 "devmgr: fat-data: kill to first answer" "$nolog"
never "devmgr: a warm spare" "$nolog"
never "spare promoted" "$nolog"
never "giving up" "$nolog"
never "init: /data is gone" "$out/fatnospare.run.log"

# The numbers. ps: PID JOB THR STATE CPU-TIME(2 words) JOB-MEM NAME.
echo "fat-spare: ps right after the boot:"
awk '/fs-""1/ {on=1; next} on && /PID|fat/ {print "  " $0} /^fs-1/ {exit}' "$log" | tr -d '\r'
stats() {   # stats <label> <pattern> <log>: median and worst of the number before " us"
    grep -ao -- "$2 [0-9]* us" "$3" | awk '{print $(NF-1)}' | sort -n |
        awk -v l="$1" '{v[NR]=$1} END {if (NR) printf "fat-spare: %s: %d samples, median %d us, worst %d us\n", l, NR, v[int((NR+1)/2)], v[NR]}'
}
stats "spare promoted after the kill" "fat-data: spare promoted" "$log"
stats "the kill itself (its end seen)" "its end seen at" "$log"
stats "kill to first answer, spare" "fat-data: kill to first answer" "$log"
stats "new process started after the kill" "fat-data: new process started (no spare waited)" "$nolog"
stats "kill to first answer, no spare" "fat-data: kill to first answer" "$nolog"

[ "$ok" = 1 ] && echo "fat-spare: PASS" && exit 0
echo "fat-spare: FAIL"
exit 1
