#!/bin/sh
# The mixer's warm spare and restart rule (user/services/init/spare.c,
# docs/M11.6-PLAN.md "The warm spare", "Supervision") in QEMU, with an
# intel-hda sound card so the mixer holds its device channel as on the PC.
# Two boots:
#   1. tools/shell-tests/mixer-spare.txt: a spare waits from the boot;
#      `kill mixer` promotes it and a new spare starts; 13 deliberate
#      kills in a row are never given up on (none counts); `kill devmgr`
#      makes the mixer exit by itself (code 3: counted as a crash), and the
#      spare is promoted at once when devmgr is back; the mixer answers
#      `vol` after each.
#   2. tools/shell-tests/mixer-nospare.txt, with the boot word `nospare`:
#      no spare; each kill starts a new process, which answers.
# The log is checked for each step, then the numbers are printed: the
# spare's memory (its job's, from `ps`), and the restart and kill-to-first-
# answer times (init's lines; median and worst) with and without a spare.
# QEMU_SMP passes through. Usage: tools/mixer-spare-test.sh <outdir>; exit
# 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
devs="-audiodev none,id=snd0 -device intel-hda,id=hda0 -device hda-output,bus=hda0.0,cad=0,audiodev=snd0"
ok=1

QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_EXTRA="$devs" QEMU_INPUT=tools/shell-tests/mixer-spare.txt \
    tools/qemu-test.sh "$out" spare shell > "$out/spare.out" 2>&1 ||
    { echo "mixer-spare: the spare script failed"; grep "serial-feed: .*no '" "$out/spare.out" || true; ok=0; }
QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_EXTRA="$devs" QEMU_INPUT=tools/shell-tests/mixer-nospare.txt \
    tools/qemu-test.sh "$out" nospare shell nospare > "$out/nospare.out" 2>&1 ||
    { echo "mixer-spare: the nospare script failed"; grep "serial-feed: .*no '" "$out/nospare.out" || true; ok=0; }
log="$out/spare.log"
nolog="$out/nospare.log"

count() { grep -ac -- "$1" "$2" || true; }
need() {   # need <min> <text> <log>
    n=$(count "$2" "$3")
    [ "$n" -ge "$1" ] || { echo "mixer-spare: $(basename "$3"): \"$2\" $n times, wanted $1 or more"; ok=0; }
}
never() {   # never <text> <log>
    n=$(count "$1" "$2")
    [ "$n" -eq 0 ] || { echo "mixer-spare: $(basename "$2"): \"$1\" $n times, wanted none"; ok=0; }
}

# Boot 1: 13 kills plus the crash, each promoted; a spare after each.
need 13 "init: bin/mixer: killed on purpose: not counted" "$log"
need 14 "init: bin/mixer: spare promoted" "$log"
need 15 "init: a warm spare bin/mixer waits" "$log"
need 1 "init: bin/mixer: kill to first answer" "$log"
need 1 "init: bin/mixer: end to first answer" "$log"
need 1 "spare promoted [0-9]* us after its end was seen" "$log"
never "times in a minute" "$log"
never "new process started" "$log"
never "the warm spare bin/mixer couldn't be promoted" "$log"
never "a spare given no usable promotion" "$log"
never "svcstate: the state of service" "$log"
# Boot 2: no spare at all; each kill a new process.
need 1 "init: nospare: no warm spare" "$nolog"
need 5 "init: bin/mixer: new process started (no spare waited)" "$nolog"
need 5 "init: bin/mixer: kill to first answer" "$nolog"
never "init: a warm spare" "$nolog"
never "spare promoted" "$nolog"

# The numbers. ps: PID JOB THR STATE CPU-TIME(2 words) JOB-MEM NAME; the
# spare is the process the first "waits" line names.
spare=$(grep -a "init: a warm spare bin/mixer waits (process" "$log" | head -1 |
        sed 's/.*(process \([0-9]*\)).*/\1/')
echo "mixer-spare: ps right after the boot (the spare is process $spare):"
awk '/sp-""1/ {on=1; next} on && /PID|mixer/ {print "  " $0} /^sp-1/ {exit}' "$log" | tr -d '\r'
stats() {   # stats <label> <pattern> <log>: median and worst of the number before " us"
    grep -ao -- "$2 [0-9]* us" "$3" | awk '{print $(NF-1)}' | sort -n |
        awk -v l="$1" '{v[NR]=$1} END {if (NR) printf "mixer-spare: %s: %d samples, median %d us, worst %d us\n", l, NR, v[int((NR+1)/2)], v[NR]}'
}
stats "spare promoted after the kill" "spare promoted" "$log"
stats "kill to first answer, spare" "kill to first answer" "$log"
stats "new process started after the kill" "new process started (no spare waited)" "$nolog"
stats "kill to first answer, no spare" "kill to first answer" "$nolog"

[ "$ok" = 1 ] && echo "mixer-spare: PASS" && exit 0
echo "mixer-spare: FAIL"
exit 1
