#!/bin/bash
# Host-side test of the machine's CPU budget (tools/remote-slots.py, and the
# costs tools/remote-test.sh works out): a fake lock directory and fake
# commands (sleep, a log of starts and ends), no ssh, no QEMU, no PC. About
# 40 s.
#   - the cost of each kind of command (JAMOS_REMOTE_COST_ONLY);
#   - six commands of 3 CPUs on a budget of 8 never run more than two at
#     once, and all six run;
#   - a command bigger than the whole budget runs alone, once the others end;
#   - a crashed governor and its command (kill -9 of both) leaks nothing;
#     a dead governor whose command lives on still counts, until it ends;
#   - an older command waiting past JAMOS_REMOTE_AGE is not passed by a
#     younger small one (the big run isn't starved);
#   - JAMOS_REMOTE_SLOTS caps the commands, ~/work/.cpu-budget overrides the
#     variables, the exit status and a missing command come through.
# Usage: tools/remote-slots-test.sh; exit 0 on PASS.
set -u
cd "$(dirname "$0")/.." || exit 2
ok=1
fail() { echo "remote-slots-test: FAIL: $*"; ok=0; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/slots.XXXXXX")
trap 'kill $(jobs -p) 2> /dev/null; rm -rf "$tmp"' EXIT
export JAMOS_REMOTE_LOCKDIR=$tmp/lock JAMOS_REMOTE_POLL=0.2
mkdir -p "$JAMOS_REMOTE_LOCKDIR"
slots="python3 tools/remote-slots.py"

# ---- the costs --------------------------------------------------------------------------------
c() { local want=$1; shift; local got; got=$(env -u QEMU_SMP -u QEMU_MEM -u PORTS_TEST_JOBS -u WL_PORT_JOBS JAMOS_REMOTE_COST_ONLY=1 "$@" 2> /dev/null)
      [ "$got" = "$want" ] || fail "cost of ${*:2}: \"$got\", wanted \"$want\""; }
R=tools/remote-test.sh
c "4 2048" $R tools/qemu-test.sh build/test init init
c "8 2048" env QEMU_SMP=8 $R tools/qemu-test.sh build/test kt8 ktest
c "8 4096" env QEMU_SMP=8 QEMU_MEM=4G $R tools/qemu-test.sh build/test kt8 ktest
c "8 2048" $R env QEMU_SMP=8 tools/qemu-test.sh build/test kt8 ktest
c "4 2048" $R tools/mixer-test.sh build/test/mixer
c "24 12288" $R tools/nvme-driver-test.sh build/test/nd
c "8 4096" $R tools/nvme-driver-test.sh build/test/nd init kill
c "12 6144" $R tools/ports-test.sh build/test/p zlib xz lua musl
c "8 4096" $R tools/ports-test.sh build/test/p zlib xz
c "4 2048" env PORTS_TEST_JOBS=1 $R tools/ports-test.sh build/test/p zlib xz
c "12 6144" $R tools/wl-port-test.sh build/test/wl
c "8 4096" $R tools/wl-port-test.sh build/test/wl proto paint
c "4 2048" env WL_PORT_JOBS=1 $R tools/wl-port-test.sh build/test/wl
c "2 2048" $R tools/update-menu-test.sh build/test/um   # its QEMUs are -smp 2
c "2 1024" $R make check
c "2 1024" $R python3 tools/fatcheck.py --selftest
c "1 1024" env JAMOS_REMOTE_LIGHT_CPUS=1 $R make check

# ---- the budget ---------------------------------------------------------------------------------
now() { python3 -c 'import time; print(time.time())'; }
# A command for the budget: logs "+name" at its start and "-name" at its end.
job() {   # job <name> <seconds>
    echo "+$1 $(now)" >> "$tmp/events"; sleep "$2"; echo "-$1 $(now)" >> "$tmp/events"
}
export -f job now
export tmp
run() { local cpus=$1; shift; $slots "$cpus" 512 bash -c "$@"; }

rm -f "$tmp/events"
export JAMOS_REMOTE_CPUS=8 JAMOS_REMOTE_MEM_GB=20 JAMOS_REMOTE_SLOTS=16 JAMOS_REMOTE_QUIET=1
for n in 1 2 3 4 5 6; do run 3 "job small$n 1.5" 2> /dev/null & done
wait
peak=$(awk '/^\+/ { n++; if (n > m) m = n } /^-/ { n-- } END { print m + 0 }' "$tmp/events")
done_n=$(grep -c '^-' "$tmp/events")
[ "$peak" -le 2 ] || fail "6 commands of 3 CPUs on a budget of 8: $peak ran at once"
[ "$peak" -ge 2 ] || fail "the budget was not used: only $peak at once"
[ "$done_n" = 6 ] || fail "$done_n of 6 commands finished"

# ---- bigger than the budget: alone ----------------------------------------------------------------
rm -f "$tmp/events"
run 3 "job a 1.5" 2> /dev/null &
sleep 0.4
run 40 "job big 1" 2> /dev/null &
sleep 0.4
run 3 "job b 0.5" 2> /dev/null &
wait
seq=$(awk '{ printf "%s ", $1 }' "$tmp/events")
case $seq in
"+a -a +big -big +b -b " | "+a -a +big -big +b -b") ;;
*"+big "*)
    # between big's start and end nothing else may start or end
    inside=$(awk '/^\+big/ { i = 1; next } /^-big/ { i = 0 } i { print $1 }' "$tmp/events")
    [ -z "$inside" ] || fail "a command ran beside the one bigger than the budget: $inside" ;;
*) fail "the big command never ran: $seq" ;;
esac
[ "$(awk '/^\+big/ { s = NR } /^\+a/ { a = NR } END { print (s > a) }' "$tmp/events")" = 1 ] ||
    fail "the big command started before the one running ahead of it ended"

# ---- a crashed holder leaks nothing ---------------------------------------------------------------
rm -rf "$JAMOS_REMOTE_LOCKDIR/.cpu"
$slots 8 512 sleep 60 2> /dev/null &
gpid=$!
sleep 0.6
cpid=$(pgrep -P "$gpid" sleep | head -1)
[ -n "$cpid" ] || fail "no command of the holder found"
# dead governor, living command: the CPUs still count
kill -9 "$gpid" 2> /dev/null
wait "$gpid" 2> /dev/null
t0=$(now)
( $slots 1 512 true 2> /dev/null; echo "$(now)" > "$tmp/late" ) &
sleep 1.5
[ ! -e "$tmp/late" ] || fail "a command ran while the dead governor's command still held the CPUs"
kill -9 "$cpid" 2> /dev/null
wait
[ -e "$tmp/late" ] || fail "the CPUs of a crashed holder were never freed"
[ -z "$(ls "$JAMOS_REMOTE_LOCKDIR/.cpu/run")" ] || fail "run files left: $(ls "$JAMOS_REMOTE_LOCKDIR/.cpu/run")"
# the whole holder (governor and command) killed at once
$slots 8 512 sleep 60 2> /dev/null &
gpid=$!
sleep 0.6
cpid=$(pgrep -P "$gpid" sleep | head -1)
kill -9 "$gpid" "$cpid" 2> /dev/null
wait "$gpid" 2> /dev/null
t0=$(now)
$slots 8 512 true 2> /dev/null
t1=$(now)
python3 -c "import sys; sys.exit(not ($t1 - $t0 < 3))" || fail "a crashed holder still held the CPUs after 3 s"

# ---- an old waiter is not passed ------------------------------------------------------------------
rm -rf "$JAMOS_REMOTE_LOCKDIR/.cpu" "$tmp/events"
export JAMOS_REMOTE_CPUS=4 JAMOS_REMOTE_AGE=1
run 2 "job a 3" 2> /dev/null &     # 2 of 4 CPUs for 3 s
sleep 0.4
run 4 "job big 0.5" 2> /dev/null &  # needs all 4: waits for a
sleep 1.5                           # ... past JAMOS_REMOTE_AGE
run 2 "job late 0.5" 2> /dev/null & # fits beside a, but big is older and has waited
wait
order=$(awk '/^\+/ { printf "%s ", $1 }' "$tmp/events")
[ "$order" = "+a +big +late " ] || fail "start order: $order, wanted +a +big +late (the big one first)"
unset JAMOS_REMOTE_AGE

# ---- the slot cap, the budget file, the status, the exit status ---------------------------------------
rm -rf "$JAMOS_REMOTE_LOCKDIR/.cpu" "$tmp/events"
export JAMOS_REMOTE_CPUS=26 JAMOS_REMOTE_SLOTS=1
run 1 "job x 1" 2> /dev/null &
run 1 "job y 1" 2> /dev/null &
wait
peak=$(awk '/^\+/ { n++; if (n > m) m = n } /^-/ { n-- } END { print m + 0 }' "$tmp/events")
[ "$peak" = 1 ] || fail "JAMOS_REMOTE_SLOTS=1: $peak commands at once"
export JAMOS_REMOTE_SLOTS=16
rm -f "$tmp/events"
echo "2" > "$JAMOS_REMOTE_LOCKDIR/.cpu-budget"   # the owner's file wins over JAMOS_REMOTE_CPUS=26
for n in 1 2 3; do run 1 "job f$n 1" 2> /dev/null & done
wait
peak=$(awk '/^\+/ { n++; if (n > m) m = n } /^-/ { n-- } END { print m + 0 }' "$tmp/events")
[ "$peak" = 2 ] || fail "a budget file of 2: $peak commands of 1 CPU at once"
rm -f "$JAMOS_REMOTE_LOCKDIR/.cpu-budget"
$slots 1 512 sleep 3 2> /dev/null &
sleep 0.6
st=$($slots --status)
echo "$st" | grep -q "1 of 26 CPUs in use by 1 run" || fail "--status: $st"
wait
$slots 1 512 sh -c 'exit 7' 2> /dev/null
[ $? = 7 ] || fail "the command's exit status was not passed on"
$slots 1 512 /nonexistent/cmd 2> /dev/null
[ $? = 127 ] || fail "a missing command did not give 127"
JAMOS_REMOTE_CPUS=1 $slots 1 512 sleep 2 2> /dev/null &
sleep 0.5
JAMOS_REMOTE_CPUS=1 JAMOS_REMOTE_QUIET= $slots 1 512 true 2> "$tmp/waiting" &
sleep 1
msg=$(grep waiting "$tmp/waiting")
echo "$msg" | grep -q "waiting: 1 of 1 CPUs in use by 1 run(s)" || fail "the waiting line: \"$msg\""
wait

[ "$ok" = 1 ] && { echo "remote-slots-test: PASS"; exit 0; }
echo "remote-slots-test: FAIL"
exit 1
