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
#     a claim ends with its governor, not with what its command left behind:
#     a dead governor's command (and a process of its own session) is killed
#     by the next caller and its CPUs freed; a command that ends leaving a
#     process in a session of its own has that process killed; a cancelled
#     governor (SIGTERM) takes the whole tree down, a command that ignores
#     SIGTERM after JAMOS_REMOTE_KILL_GRACE;
#   - an older command waiting past JAMOS_REMOTE_AGE is not passed by a
#     younger small one (the big run isn't starved);
#   - JAMOS_REMOTE_SLOTS caps the commands, ~/work/.cpu-budget overrides the
#     variables, the exit status and a missing command come through;
#   - a --detach claim (a long build) returns at once, holds its CPUs while
#     its command runs in the background (the others wait for them), its
#     output is in its log, and the claim ends with the command; a --detach
#     made inside another claim survives that claim's end;
#   - the deadlock of 2026-10-15: a 16-CPU long claim and a 12-CPU waiter that
#     can't fit beside it are told so at once and hold no one up (4-CPU runs
#     start), the waiter starts when the long claim ends, and the fairness
#     rule still holds for a run that can fit beside the long claim;
#   - --shrink takes fewer parts of the command when the whole can't fit beside
#     a long claim; a waiter whose stderr is closed (its client gone) gives up
#     its place; --status lists governor pid, age and command, and a claim
#     whose governor is gone.
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
# gone <pid>: true once the process has ended (a zombie waiting for init counts as ended)
gone() { local i s; for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
             s=$(ps -o stat= -p "$1" 2> /dev/null)
             case $s in "" | Z*) return 0 ;; esac
             sleep 0.2; done; return 1; }
# What a QEMU does: a process in a session of its own that outlives the command that
# started it. `python3 $tmp/orphan.py <file>` writes its pid into the file.
cat > "$tmp/orphan.py" <<'EOF'
import os, sys, time
os.setsid()
open(sys.argv[1], "w").write(str(os.getpid()))
time.sleep(60)
EOF

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
# dead governor, living command: the next caller kills the command and starts at once
kill -9 "$gpid" 2> /dev/null
wait "$gpid" 2> /dev/null
t0=$(now)
$slots 1 512 true 2> "$tmp/reap.err"
t1=$(now)
python3 -c "import sys; sys.exit(not ($t1 - $t0 < 2))" || fail "a command waited for the dead governor's command to end"
gone "$cpid" || fail "the dead governor's command was left running"
grep -q "is gone: killed the 1 process" "$tmp/reap.err" || fail "no word of the killed run: $(cat "$tmp/reap.err")"
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

# ---- a claim ends with its governor and its command, not with what the command left ----------------
# A command that ends leaving a process in a session of its own behind (a hung QEMU of a
# run that was cancelled): the governor kills it, and the claim is gone when it returns.
rm -rf "$JAMOS_REMOTE_LOCKDIR/.cpu" "$tmp/orphan.pid"
$slots 8 512 sh -c "python3 $tmp/orphan.py $tmp/orphan.pid & sleep 0.8" 2> "$tmp/left.err"
opid=$(cat "$tmp/orphan.pid" 2> /dev/null)
[ -n "$opid" ] || fail "the orphan never started"
gone "$opid" || fail "a process the command left behind (its own session) was not killed when the command ended"
grep -q "left 1 process" "$tmp/left.err" || fail "no word of the process left behind: $(cat "$tmp/left.err")"
[ -z "$(ls "$JAMOS_REMOTE_LOCKDIR/.cpu/run")" ] || fail "run files left after a command that left a process: $(ls "$JAMOS_REMOTE_LOCKDIR/.cpu/run")"
# The governor killed (kill -9) while its command and such a process live: the next caller
# kills them all and starts at once, the claim holding no CPUs.
rm -f "$tmp/orphan.pid"
$slots 8 512 sh -c "python3 $tmp/orphan.py $tmp/orphan.pid & sleep 60" 2> /dev/null &
gpid=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s "$tmp/orphan.pid" ] && break; sleep 0.3; done
opid=$(cat "$tmp/orphan.pid" 2> /dev/null)
cpid=$(pgrep -P "$gpid" sh | head -1)
[ -n "$opid" ] && [ -n "$cpid" ] || fail "no command or orphan of the holder found"
kill -9 "$gpid" 2> /dev/null
wait "$gpid" 2> /dev/null
t0=$(now)
$slots 8 512 true 2> /dev/null
t1=$(now)
python3 -c "import sys; sys.exit(not ($t1 - $t0 < 3))" || fail "a claim whose governor died, with its command and a process of another session alive, still held its CPUs"
gone "$cpid" || fail "the command of a dead governor was not killed"
gone "$opid" || fail "a process of a dead governor's claim, in a session of its own, was not killed"
# A cancel: SIGTERM to the governor goes to the whole group; a command that ignores it
# (SIGTERM ignored is inherited by everything below) is killed after JAMOS_REMOTE_KILL_GRACE.
rm -f "$tmp/orphan.pid"
export JAMOS_REMOTE_KILL_GRACE=1
$slots 8 512 sh -c "trap '' TERM; python3 $tmp/orphan.py $tmp/orphan.pid & sleep 60" 2> /dev/null &
gpid=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s "$tmp/orphan.pid" ] && break; sleep 0.3; done
opid=$(cat "$tmp/orphan.pid" 2> /dev/null)
cpid=$(pgrep -P "$gpid" sh | head -1)
t0=$(now)
kill -TERM "$gpid"
wait "$gpid" 2> /dev/null
rc=$?
t1=$(now)
python3 -c "import sys; sys.exit(not ($t1 - $t0 < 6))" || fail "a cancelled governor took more than 6 s to end its command (a grace of 1 s)"
[ "$rc" != 0 ] || fail "a cancelled run exited 0"
gone "$cpid" || fail "a cancelled run's command still runs"
gone "$opid" || fail "a cancelled run's process of another session still runs"
[ -z "$(ls "$JAMOS_REMOTE_LOCKDIR/.cpu/run")" ] || fail "run files left after a cancel: $(ls "$JAMOS_REMOTE_LOCKDIR/.cpu/run")"
# ... and a command that does end on SIGTERM ends at once, with its own status
$slots 8 512 sh -c 'trap "exit 9" TERM; sleep 60 & wait' 2> /dev/null &
gpid=$!
sleep 0.6
t0=$(now)
kill -TERM "$gpid"
wait "$gpid" 2> /dev/null
rc=$?
t1=$(now)
python3 -c "import sys; sys.exit(not ($t1 - $t0 < 1.5))" || fail "a command that ends on SIGTERM took the grace period to end"
[ "$rc" = 9 ] || fail "a cancelled command's own exit status was lost: $rc"
unset JAMOS_REMOTE_KILL_GRACE

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

# ---- a long claim: the deadlock of 2026-10-15 ---------------------------------------------------------
# The Chromium build holds 16 of 26 CPUs for hours. A 12-CPU run (init-test.sh) can never fit
# beside it; the fairness rule used to hold every younger run behind it for as long. Now it is
# told at once, holds no one up, and starts when the long claim ends. A run that does fit
# beside the long claim (8 CPUs, once the 4-CPU run ahead of it ends) keeps its place.
pos() { awk -v k="$1" '$1 == k { print NR; exit }' "$tmp/events"; }
rm -rf "$JAMOS_REMOTE_LOCKDIR/.cpu" "$tmp/events"
export JAMOS_REMOTE_CPUS=26 JAMOS_REMOTE_MEM_GB=20 JAMOS_REMOTE_AGE=1
said=$($slots --detach "$tmp/long.log" 16 4096 bash -c "job long 9" 2> /dev/null)
case $said in "started "[0-9]*) ;; *) fail "the long claim: \"$said\"" ;; esac
sleep 0.4
run 4 "job a 3.5" 2> /dev/null &       # beside the long claim: 20 of 26
sleep 0.2
run 12 "job big12 0.5" 2> "$tmp/big12.err" &   # 16 + 12 = 28 of 26: never, while the long claim lives
sleep 0.2
run 8 "job w8 0.5" 2> /dev/null &      # 16 + 4 + 8 = 28: not yet; 16 + 8 = 24: fits when a ends
sleep 2                                # both have waited past JAMOS_REMOTE_AGE
run 4 "job s1 0.4" 2> /dev/null &
run 4 "job s2 0.4" 2> /dev/null &
sleep 1
grep -q "needs 12 CPUs" "$tmp/big12.err" && grep -q "at most 10 CPUs" "$tmp/big12.err" || fail "the 12-CPU run was not told it can't fit beside the long claim: $(cat "$tmp/big12.err")"
wait
for k in long a w8 s1 s2 big12; do [ -n "$(pos "+$k")" ] && [ -n "$(pos "-$k")" ] || fail "$k did not run: $(awk '{ printf "%s ", $1 }' "$tmp/events")"; done
seq=$(awk '{ printf "%s ", $1 }' "$tmp/events")
[ "$(pos +a)" -lt "$(pos +w8)" ] && [ "$(pos -a)" -lt "$(pos +w8)" ] || fail "the 8-CPU run did not wait for the 4-CPU run: $seq"
[ "$(pos +w8)" -lt "$(pos +s1)" ] && [ "$(pos +w8)" -lt "$(pos +s2)" ] || fail "a younger run started ahead of the older 8-CPU run that fits: $seq"
[ "$(pos -s1)" -lt "$(pos -long)" ] && [ "$(pos -s2)" -lt "$(pos -long)" ] || fail "the 4-CPU runs waited behind the 12-CPU run that can't fit: $seq"
[ "$(pos +big12)" -gt "$(pos -long)" ] || fail "the 12-CPU run started beside the long claim: $seq"
unset JAMOS_REMOTE_AGE

# --shrink: a command made of 3 parts of 4 CPUs, which asks for 12, takes 2 parts (10 CPUs are left
# beside the long claim); with no long claim it takes all 3 (VAR is not set).
rm -rf "$JAMOS_REMOTE_LOCKDIR/.cpu" "$tmp/shrunk"
$slots --detach "$tmp/long.log" 16 6144 sleep 5 > /dev/null 2>&1
sleep 0.4
$slots --shrink PARTS:4:2048:3 12 6144 sh -c 'echo "PARTS=${PARTS-unset}" > "$1"' sh "$tmp/shrunk" 2> "$tmp/shrunk.err"
[ "$(cat "$tmp/shrunk" 2> /dev/null)" = "PARTS=2" ] || fail "--shrink beside a long claim: $(cat "$tmp/shrunk" 2> /dev/null), wanted PARTS=2"
grep -q "PARTS=2 part(s) at a time (8 CPUs" "$tmp/shrunk.err" || fail "--shrink said nothing of it: $(cat "$tmp/shrunk.err")"
rm -rf "$JAMOS_REMOTE_LOCKDIR/.cpu"   # a state without the long claim (its sleep ends by itself)
$slots --shrink PARTS:4:2048:3 12 6144 sh -c 'echo "PARTS=${PARTS-unset}" > "$1"' sh "$tmp/shrunk" 2> /dev/null
[ "$(cat "$tmp/shrunk" 2> /dev/null)" = "PARTS=unset" ] || fail "--shrink changed a command that fits: $(cat "$tmp/shrunk" 2> /dev/null)"

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
gpid=$!
sleep 0.6
st=$($slots --status)
echo "$st" | grep -q "1 of 26 CPUs in use by 1 run" || fail "--status: $st"
echo "$st" | grep -q "pid $gpid .*run .*sleep 3" || fail "--status doesn't show the governor's pid and the command: $st"
wait
# a claim whose governor is gone is shown as such, and removed by the next caller
$slots 2 512 sleep 3 2> /dev/null &
gpid=$!
sleep 0.6
kill -9 "$gpid" 2> /dev/null
wait "$gpid" 2> /dev/null
st=$($slots --status)
echo "$st" | grep -q "GONE .*sleep 3" && echo "$st" | grep -q "its governor is gone" || fail "--status of a claim whose governor died: $st"
echo "$st" | grep -q "0 of 26 CPUs in use by 0 run" || fail "--status counted a dead governor's claim as running: $st"
$slots 1 512 true 2> /dev/null
echo "$($slots --status)" | grep -q "GONE" && fail "the dead governor's claim was not removed by the next caller"
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

# ---- a detached claim (a long build) ---------------------------------------------------------------
rm -rf "$JAMOS_REMOTE_LOCKDIR/.cpu"
export JAMOS_REMOTE_CPUS=4
t0=$(now)
said=$($slots --detach "$tmp/detached.log" 3 512 sh -c 'echo detached-out; sleep 2' 2> /dev/null)
t1=$(now)
case $said in "started "[0-9]*) ;; *) fail "--detach said \"$said\", wanted \"started <pid>\"" ;; esac
python3 -c "import sys; sys.exit(not ($t1 - $t0 < 1.5))" || fail "--detach did not return at once"
$slots 2 512 true 2> /dev/null   # 3 of 4 CPUs held: waits for the detached command's end
t2=$(now)
python3 -c "import sys; sys.exit(not ($t2 - $t0 >= 1.5))" || fail "a command ran beside a detached claim that left no room"
grep -q detached-out "$tmp/detached.log" || fail "the detached command's output is not in its log"
sleep 0.5
[ -z "$(ls "$JAMOS_REMOTE_LOCKDIR/.cpu/run")" ] || fail "a detached claim left its run file"
# a detached claim made inside another claim (a command that starts a build) outlives it:
# the outer claim's end must not sweep the inner governor or its command away
rm -rf "$JAMOS_REMOTE_LOCKDIR/.cpu"
$slots 1 512 sh -c "$slots --detach '$tmp/nested.log' 3 512 sh -c 'echo nested-out; sleep 3' > /dev/null 2>&1" 2> /dev/null
sleep 1
st=$($slots --status)
echo "$st" | grep -q "3 of 4 CPUs in use by 1 run" && echo "$st" | grep -q "long: sh -c echo nested-out" || fail "the claim detached inside another claim did not outlive it: $st"
sleep 3
[ -z "$(ls "$JAMOS_REMOTE_LOCKDIR/.cpu/run")" ] || fail "the nested detached claim left its run file"
grep -q nested-out "$tmp/nested.log" || fail "the nested detached command did not run"

# a waiting run whose client is gone (nobody reads its stderr any more) gives up its place
rm -rf "$JAMOS_REMOTE_LOCKDIR/.cpu"
export JAMOS_REMOTE_CPUS=2 JAMOS_REMOTE_SAY_EVERY=0.3
$slots 2 512 sleep 4 2> /dev/null &
sleep 0.5
mkfifo "$tmp/ef"
( $slots 2 512 true 2> "$tmp/ef"; echo $? > "$tmp/gaverc" ) &
( read -r line < "$tmp/ef"; echo "$line" > "$tmp/firstline" ) &   # one line, then the reader is gone
sleep 2
[ "$(cat "$tmp/gaverc" 2> /dev/null)" = 141 ] || fail "a waiting run whose stderr is closed did not give up (rc $(cat "$tmp/gaverc" 2> /dev/null))"
grep -q "waiting: " "$tmp/firstline" 2> /dev/null || fail "the waiting run said nothing before its reader left"
[ -z "$(ls "$JAMOS_REMOTE_LOCKDIR/.cpu/wait")" ] || fail "a waiting run that gave up left its place: $(ls "$JAMOS_REMOTE_LOCKDIR/.cpu/wait")"
unset JAMOS_REMOTE_SAY_EVERY
wait

[ "$ok" = 1 ] && { echo "remote-slots-test: PASS"; exit 0; }
echo "remote-slots-test: FAIL"
exit 1
