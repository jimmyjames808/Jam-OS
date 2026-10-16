#!/bin/bash
# Host-side test of tools/pc-batch.sh: a stand-in for tools/remote-test.sh
# (PC_BATCH_REMOTE_TEST) that runs each command here, no ssh, no PC. About 30 s.
#   - start returns at once; wait with a short PC_BATCH_WAIT says "not yet"
#     (exit 124, one line); wait to the end gives the table (exit 1: one
#     failed): PASS, FAIL with its failure line and log, rerun-PASS for a run
#     that fails once (--rerun-failed), the queue time, VAR=value words with
#     and without `env`, every run with JAMOS_REMOTE_NOSYNC=1 after one --prepare;
#   - report gives the same; `run` of a runlist that passes exits 0;
#   - bad runlists (a name twice, a missing script, no command, nothing) are
#     refused (exit 2) and leave no batch;
#   - a PC that doesn't answer is tried once more, then the runs are NOT-RUN;
#     a run that finds it gone (remote-test's exit 3 and its "not run" line) is
#     run once more, a test that exits 3 by itself is not;
#   - the runs start PC_BATCH_STAGGER seconds apart, and no more than
#     PC_BATCH_JOBS are out at once;
#   - a KTESTS=0 run builds that image too (a second --prepare, no copy);
#   - cancel stops the runs;
#   - start --detach, against a fake PC (a home directory with the real
#     keeper, driver and remote-slots.py): the Mac's driver is gone once the
#     batch is handed over and nothing asks the PC until the wait; the table,
#     the outputs back (no disk images), a run id for each try, two
#     checkouts at once, a second wait and status that don't ask the PC
#     again; a second detached batch of one checkout refused; a wait with
#     the PC down (exit 3); cancel (every process, one in a session of its
#     own too, and the claim); a reboot (LOST, then --resume); no keeper.
# Usage: tools/pc-batch-test.sh; exit 0 on PASS.
set -u
cd "$(dirname "$0")/.." || exit 2
ok=1
fail() { echo "pc-batch-test: FAIL: $*"; ok=0; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/pcbatch.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
export PC_BATCH_DIR=$tmp/batch PC_BATCH_REMOTE_TEST=$tmp/fake-remote FAKE_DIR=$tmp PC_BATCH_RETRY_DELAY=1
cat > "$tmp/fake-remote" <<'EOF'
#!/bin/bash
if [ "${JAMOS_REMOTE_SCRIPT_ONLY:-}" = 1 ]; then   # start --detach: what the PC would run
    [ "${JAMOS_REMOTE_NOSYNC:-}" = 1 ] && [ -z "${JAMOS_REMOTE:-}" ] && [ "${JAMOS_REMOTE_RUNID:-}" = @runid@ ] ||
        { echo "fake: a script asked for with a copy, a host or no run id"; exit 99; }
    printf 'cd %q || exit 2\n' "$FAKE_HOME/work/${PWD##*/}"
    printf 'echo @runid@ >> %q\n' "$FAKE_DIR/runids"
    for v in JAMOS_TIER KTESTS; do [ -n "${!v:-}" ] && printf 'export %q\n' "$v=${!v}"; done
    printf 'set -- '; printf '%q ' "$@"; echo
    printf 'exec python3 %q 2 100 "$@"\n' "$FAKE_SLOTS"
    exit 0
fi
echo "$* NOSYNC=${JAMOS_REMOTE_NOSYNC:-} KTESTS=${KTESTS:-}" >> "$FAKE_DIR/calls"
if [ "$1" = --prepare ]; then
    [ "${JAMOS_REMOTE_COPY_ONLY:-}" = 1 ] && echo "${PWD##*/}" >> "$FAKE_DIR/copyonly"
    [ -n "${FAKE_HOME:-}" ] && mkdir -p "$FAKE_HOME/work/${PWD##*/}"
    exit "${FAKE_PREPARE_RC:-0}"
fi
[ "${JAMOS_REMOTE_NOSYNC:-}" = 1 ] || { echo "fake: a run that copies"; exit 99; }
[ -n "${FAKE_NO_RUNNING:-}" ] || echo "remote-test: running with 4 of 26 CPUs in use (after 2 s)" >&2
exec "$@"
EOF
chmod +x "$tmp/fake-remote"
B=tools/pc-batch.sh

# ---- a batch with a failure and a rerun ----
cat > "$tmp/list" <<'EOF'
# a comment, then a blank line

ok      true
bad     sh -c 'echo "bad: FAILED (input script)"; exit 1'
slow    sh -c 'sleep 4'
envq    X="a b" sh -c 'test "$X" = "a b"'
envw    env Y=1 Z=2 sh -c 'test "$Y$Z" = 12'
flaky   sh -c 'f=$FAKE_DIR/flaky; [ -f $f ] && exit 0; touch $f; echo "flaky: TIMEOUT"; exit 1'
EOF
s=$SECONDS
out=$($B start --rerun-failed "$tmp/list")
[ $((SECONDS - s)) -le 2 ] || fail "start took $((SECONDS - s)) s"
id=$(echo "$out" | sed -n 's/^batch \([^:]*\): 6 runs started.*/\1/p')
[ -n "$id" ] || fail "start said: $out"
out=$(PC_BATCH_WAIT=1 $B wait "$id")
rc=$?
[ $rc = 124 ] || fail "an early wait: exit $rc"
[ "$(echo "$out" | wc -l | tr -d ' ')" = 1 ] && echo "$out" | grep -q "done\|copying" ||
    fail "an early wait said: $out"
out=$($B wait "$id")
rc=$?
[ $rc = 1 ] || fail "wait to the end: exit $rc"
row() { echo "$out" | grep -qE "^  $1 +$2 +[0-9]+ +$3\$" || fail "no row \"$1 $2 ... $3\" in: $out"; }
row ok PASS 2
row bad FAIL 2
row slow PASS 2
row envq PASS 2
row envw PASS 2
row flaky rerun-PASS 2
echo "$out" | grep -q "^batch $id: 6 runs: 4 PASS, 1 rerun-PASS, 1 FAIL; " || fail "summary: $out"
echo "$out" | grep -q "| bad: FAILED (input script)" || fail "bad's failure line missing: $out"
echo "$out" | grep -q "exit 1 (and the first try: exit 1); log $PC_BATCH_DIR/$id/bad.log" || fail "bad's log path missing: $out"
echo "$out" | grep -q "first try: exit 1 in [0-9]* s, $PC_BATCH_DIR/$id/flaky.try1.log" || fail "flaky's first try: $out"
[ "$(grep -c -- '--prepare' "$tmp/calls")" = 1 ] || fail "prepares: $(grep -c -- '--prepare' "$tmp/calls")"
[ "$(grep -c 'NOSYNC=1' "$tmp/calls")" = 8 ] || fail "runs without a copy: $(cat "$tmp/calls")"
out2=$($B report "$id")
[ $? = 1 ] && [ "$out2" = "$out" ] || fail "report differs from wait's table"
[ "$($B report > /dev/null; echo $?)" = 1 ] || fail "report with no id isn't the newest batch"

# ---- run: everything passes ----
printf 'a true\nb sh -c "exit 0"\n' > "$tmp/list2"
out=$($B run "$tmp/list2")
rc=$?
[ $rc = 0 ] || fail "run of passing runs: exit $rc: $out"
echo "$out" | grep -q ": 2 runs: 2 PASS, 0 rerun-PASS, 0 FAIL" || fail "run said: $out"

# ---- refused runlists ----
n=$(ls "$PC_BATCH_DIR" | wc -l)
bad() {
    printf "$1" > "$tmp/badlist"
    $B start "$tmp/badlist" > /dev/null 2>&1
    rc=$?
    [ $rc = 2 ] || fail "runlist \"$1\": exit $rc"
}
bad 'a true\na true\n'
bad 'a tools/no-such-test.sh build/x\n'
bad 'a\n'
bad 'a X=1\n'
bad '# nothing\n'
bad 'a/b true\n'
[ "$(ls "$PC_BATCH_DIR" | wc -l)" = "$n" ] || fail "a refused runlist left a batch"

# ---- a PC that doesn't answer ----
: > "$tmp/calls"
out=$(FAKE_PREPARE_RC=3 $B run "$tmp/list2")
rc=$?
[ $rc = 1 ] || fail "no PC: exit $rc"
[ "$(grep -c -- '--prepare' "$tmp/calls")" = 2 ] || fail "no PC: prepares $(grep -c -- '--prepare' "$tmp/calls")"
echo "$out" | grep -qE "^  a +NOT-RUN" || fail "no PC: $out"

# ---- a run that finds the PC gone before it ran: once more; a test's own exit 3: not ----
cat > "$tmp/noans-once" <<'EOF'
#!/bin/bash
f=$FAKE_DIR/noans-once.seen
[ -f "$f" ] && { echo "second try ran"; exit 0; }
touch "$f"
echo "remote-test: jamos-pc doesn't answer, and JAMOS_REMOTE_ONLY=1: not run" >&2
exit 3
EOF
cat > "$tmp/own3" <<'EOF'
#!/bin/bash
echo x >> "$FAKE_DIR/own3.runs"
echo "own3: a test that exits 3 by itself" >&2
exit 3
EOF
chmod +x "$tmp/noans-once" "$tmp/own3"
printf 'noans %s\nown3 %s\nok true\n' "$tmp/noans-once" "$tmp/own3" > "$tmp/list5"
out=$($B run "$tmp/list5")
rc=$?
[ $rc = 1 ] || fail "a run that finds no PC: exit $rc: $out"
echo "$out" | grep -qE "^  noans +PASS" || fail "the run that found no PC wasn't run again: $out"
echo "$out" | grep -qE "^  own3 +FAIL" || fail "own3: $out"
[ "$(wc -l < "$tmp/own3.runs" | tr -d ' ')" = 1 ] || fail "a test's own exit 3 was run again"
grep -q "the PC didn't answer: once more" "$PC_BATCH_DIR"/*/noans.log &&
    grep -q "second try ran" "$PC_BATCH_DIR"/*/noans.log || fail "the run's log lacks the note or the second try"

# ---- the runs start a little apart ----
printf '#!/bin/bash\npython3 -c "import time; print(time.time())" > "$1"\n' > "$tmp/stamp"
chmod +x "$tmp/stamp"
printf 'a %s %s/t.a\nb %s %s/t.b\nc %s %s/t.c\n' "$tmp/stamp" "$tmp" "$tmp/stamp" "$tmp" "$tmp/stamp" "$tmp" > "$tmp/list6"
PC_BATCH_STAGGER=1 $B run "$tmp/list6" > /dev/null || fail "stagger batch failed"
[ -s "$tmp/t.a" ] && [ -s "$tmp/t.c" ] || fail "stagger: runs left no time"
t0=$(cat "$tmp/t.a")
t2=$(cat "$tmp/t.c")
awk -v a="$t0" -v c="$t2" 'BEGIN { exit !(c - a >= 1.8) }' || fail "stagger: the third run started only $(awk -v a="$t0" -v c="$t2" 'BEGIN { print c - a }') s after the first"

# ---- what exit 255 says in the table ----
printf '#!/bin/bash\necho "remote-test: running with 3 of 26 CPUs in use"\necho "ssh-retry: connection lost after login: x"\nexit 255\n' > "$tmp/lost-started"
printf '#!/bin/bash\necho "ssh-retry: connection lost after login: x"\nexit 255\n' > "$tmp/lost-waiting"
printf '#!/bin/bash\necho "ssh-retry: connection lost after login: x"\necho "ssh-retry: not logged in: gave up after 3 tries"\nexit 255\n' > "$tmp/no-login"
chmod +x "$tmp/lost-started" "$tmp/lost-waiting" "$tmp/no-login"
printf 'a %s\nb FAKE_NO_RUNNING=1 %s\nc FAKE_NO_RUNNING=1 %s\n' "$tmp/lost-started" "$tmp/lost-waiting" "$tmp/no-login" > "$tmp/list8"
out=$($B run "$tmp/list8")
echo "$out" | grep -q "exit 255: the ssh connection to the PC was lost after the run started" || fail "255 after start: $out"
echo "$out" | grep -q "exit 255: the ssh connection to the PC was lost while the run waited for the budget" || fail "255 while waiting: $out"
echo "$out" | grep -q "exit 255: ssh could not log in to the PC" || fail "255 not logged in: $out"

# ---- no more than PC_BATCH_JOBS runs out at once ----
printf '#!/bin/bash\necho "+ $$" >> "$FAKE_DIR/conc.log"\nsleep 1\necho "- $$" >> "$FAKE_DIR/conc.log"\n' > "$tmp/conc"
chmod +x "$tmp/conc"
: > "$tmp/list7"
for i in 1 2 3 4 5; do echo "c$i $tmp/conc" >> "$tmp/list7"; done
rm -f "$tmp/conc.log"
PC_BATCH_JOBS=2 PC_BATCH_STAGGER=0 $B run "$tmp/list7" > /dev/null || fail "jobs batch failed"
[ "$(grep -c '^+' "$tmp/conc.log")" = 5 ] || fail "jobs: not all 5 runs ran: $(cat "$tmp/conc.log")"
awk '$1 == "+" { n++; if (n > m) m = n } $1 == "-" { n-- } END { exit !(m == 2) }' "$tmp/conc.log" ||
    fail "jobs: more (or fewer) than 2 at once: $(tr '\n' ' ' < "$tmp/conc.log")"

# ---- KTESTS=0 builds its image too ----
: > "$tmp/calls"
printf 'a true\nb KTESTS=0 true\n' > "$tmp/list3"
$B run "$tmp/list3" > /dev/null || fail "KTESTS=0 batch failed"
grep -q -- '^--prepare NOSYNC=1 KTESTS=0$' "$tmp/calls" || fail "no KTESTS=0 prepare: $(cat "$tmp/calls")"
grep -q '^true NOSYNC=1 KTESTS=$' "$tmp/calls" && grep -q '^env KTESTS=0 true NOSYNC=1 KTESTS=0$' "$tmp/calls" ||
    fail "KTESTS reached the wrong run: $(cat "$tmp/calls")"

# ---- cancel ----
printf 'long sh -c "echo \\$\\$ > $FAKE_DIR/longpid; exec sleep 30"\n' > "$tmp/list4"
out=$($B start "$tmp/list4")
id=$(echo "$out" | sed -n 's/^batch \([^:]*\):.*/\1/p')
[ -n "$id" ] || fail "cancel's start said: $out"
i=0
until [ -s "$tmp/longpid" ] || [ $i -ge 50 ]; do sleep 0.1; i=$((i + 1)); done
$B cancel "$id" > /dev/null || fail "cancel failed"
sleep 1
kill -0 "$(cat "$tmp/longpid" 2> /dev/null)" 2> /dev/null && fail "cancel left the run going"
out=$($B wait "$id")
rc=$?
[ $rc != 0 ] && [ $rc != 124 ] && echo "$out" | grep -q "driver is gone" || fail "wait after cancel: exit $rc: $out"

# ---- detached: a fake PC (a home with the keeper, ssh and wsl stand-ins) ----
# The real keeper, driver and remote-slots.py run "on the PC"; setsid and
# flock are stand-ins where the host has none (the Mac).
pc=$tmp/pc/home
mkdir -p "$pc" "$tmp/bin" "$tmp/slots"
export FAKE_HOME=$pc FAKE_BIN=$tmp/bin FAKE_SLOTS=$PWD/tools/remote-slots.py PC_BATCH_SSH=$tmp/fake-ssh PC_BATCH_POLL=1
cat > "$tmp/fake-ssh" <<'EOF'
#!/bin/bash
echo "$*" >> "$FAKE_DIR/ssh.log"
while [ $# -gt 0 ]; do case $1 in -o) shift 2 ;; -*) shift ;; *) break ;; esac; done
shift   # the host
[ -n "${FAKE_PC_DOWN:-}" ] && { echo "ssh: connect to host pc port 22: Connection refused" >&2; exit 255; }
cd "$FAKE_HOME" && HOME=$FAKE_HOME PATH=$FAKE_BIN:$PATH exec bash -c "$*"
EOF
cat > "$tmp/bin/wsl" <<'EOF'
#!/bin/bash
while [ $# -gt 0 ]; do case $1 in --cd) cd "$2" || exit 1; shift 2 ;; -e) shift; break ;; *) break ;; esac; done
exec "$@"
EOF
cat > "$tmp/bin/setsid" <<'EOF'
#!/usr/bin/env python3
import os, sys
if os.getpgrp() == os.getpid():
    if os.fork():
        os._exit(0)
os.setsid()
os.execvp(sys.argv[1], sys.argv[1:])
EOF
cat > "$tmp/bin/flock" <<'EOF'
#!/usr/bin/env python3
import fcntl, os, subprocess, sys
a, how = sys.argv[1:], fcntl.LOCK_EX
if a[:1] == ["-n"]:
    a, how = a[1:], how | fcntl.LOCK_NB
if len(a) == 1 and a[0].isdigit():
    fd, cmd = int(a[0]), []
else:
    fd, cmd = os.open(a[0], os.O_RDONLY | os.O_CREAT, 0o666), a[1:]
try:
    fcntl.flock(fd, how)
except OSError:
    sys.exit(1)
sys.exit(subprocess.call(cmd) if cmd else 0)
EOF
# The PC's clock 300 s ahead (WSL's can drift).
cat > "$tmp/bin/date" <<'EOF'
#!/bin/bash
[ "$*" = +%s ] && { echo $(($(/bin/date +%s) + 300)); exit 0; }
exec /bin/date "$@"
EOF
chmod +x "$tmp/fake-ssh" "$tmp/bin/"*
keeper=
keeper_start() {
    HOME=$pc PATH=$tmp/bin:$PATH JAMOS_REMOTE_LOCKDIR=$tmp/slots JAMOS_REMOTE_POLL=0.2 \
        bash tools/pc-keeper.sh > /dev/null 2>&1 < /dev/null &
    keeper=$!
    disown $keeper
}
trap 'kill $keeper $(cat "$tmp"/kid* 2> /dev/null) 2> /dev/null; rm -rf "$tmp"' EXIT
keeper_start
for c in co co2; do
    mkdir -p "$tmp/$c/tools"
    cp tools/pc-batch.sh tools/pc-batch-driver.sh tools/pc-detach.sh "$tmp/$c/tools/"
done
D() { local c=$1; shift; (cd "$tmp/$c" && PC_BATCH_DIR= tools/pc-batch.sh "$@"); }
row2() { echo "$out" | grep -qE "^  $1 +$2( |\$)" || fail "no row \"$1 $2\" in: $out"; }   # no seconds
bidof() { echo "$1" | sed -n 's/^batch \([^:]*\):.*/\1/p'; }
upto() {   # upto <seconds> <test...>: until the test holds
    local i=0 n=$(($1 * 10))
    shift
    until "$@"; do [ $i -ge $n ] && return 1; sleep 0.1; i=$((i + 1)); done
}

# A batch the PC runs alone (the Mac's driver gone once it is handed over),
# and a second checkout's at the same time: each gets its own runs and outputs.
cat > "$tmp/co/list" <<EOF
ok     true
bad    sh -c 'echo "bad: FAILED (on the PC)"; exit 1'
flaky  sh -c 'f=$tmp/dflaky; [ -f \$f ] && exit 0; touch \$f; echo "flaky: TIMEOUT"; exit 1'
out    sh -c 'mkdir -p build/t/out && echo one > build/t/out/f && echo disk > build/t/out/x.img' build/t/out/
tier   sh -c 'test "\$JAMOS_TIER" = push'
EOF
printf '%s\n' "out2 sh -c 'mkdir -p build/t/out && echo two > build/t/out/f' build/t/out" > "$tmp/co2/list"
: > "$tmp/ssh.log"
out=$(JAMOS_TIER=push D co start --detach --rerun-failed list)
id=$(bidof "$out")
out2=$(D co2 start --detach list)
id2=$(bidof "$out2")
[ -n "$id" ] && [ -n "$id2" ] || fail "detached starts said: $out / $out2"
upto 20 test -f "$tmp/co/build/batch/$id/handed" || fail "never handed: $(cat "$tmp/co/build/batch/$id/handoff.log" 2> /dev/null)"
upto 5 eval '! kill -0 $(cat "$tmp/co/build/batch/$id/driver.pid") 2> /dev/null' || fail "the Mac's driver lives on after the hand-off"
calls=$(wc -l < "$tmp/ssh.log")
upto 30 test -f "$pc/work/.batch/co/$id/done" || fail "the PC never finished the batch: $(cat "$pc/work/.batch/co/$id/driver.out" 2> /dev/null)"
upto 30 test -f "$pc/work/.batch/co2/$id2/done" || fail "the PC never finished co2's batch"
[ "$(wc -l < "$tmp/ssh.log")" = "$calls" ] || fail "ssh while the Mac slept: $(tail -n +$((calls + 1)) "$tmp/ssh.log")"
out=$(D co wait "$id")
rc=$?
[ $rc = 1 ] || fail "detached wait: exit $rc: $out"
row ok PASS 0
row bad FAIL 0
row flaky rerun-PASS 0
row out PASS 0
row tier PASS 0
echo "$out" | grep -q "^batch $id: 5 runs: 3 PASS, 1 rerun-PASS, 1 FAIL; " || fail "detached summary: $out"
wall=$(echo "$out" | sed -n 's/^batch .* FAIL; \(-*[0-9]*\) s.*/\1/p')
[ -n "$wall" ] && [ "$wall" -ge 0 ] && [ "$wall" -lt 100 ] || fail "the PC's clock not taken into account: $wall s"
echo "$out" | grep -q "| bad: FAILED (on the PC)" || fail "detached: bad's failure line: $out"
echo "$out" | grep -q "log build/batch/$id/pc/runs/bad.log" || fail "detached: bad's log: $out"
[ "$(cat "$tmp/co/build/t/out/f" 2> /dev/null)" = one ] || fail "detached: build/t/out didn't come back"
[ ! -e "$tmp/co/build/t/out/x.img" ] || fail "detached: a disk image came back"
[ "$(cat "$tmp/co2/build/t/out/f" 2> /dev/null)" = "" ] || fail "co2's output before its wait"
[ "$(D co2 wait "$id2" > /dev/null; echo $?)" = 0 ] || fail "co2's batch"
[ "$(cat "$tmp/co2/build/t/out/f" 2> /dev/null)" = two ] && [ "$(cat "$tmp/co/build/t/out/f")" = one ] ||
    fail "the two checkouts' outputs mixed"
[ "$(sort "$tmp/copyonly" 2> /dev/null | tr '\n' ' ')" = "co co2 " ] || fail "detached batches didn't copy alone: $(cat "$tmp/copyonly" 2> /dev/null)"
grep -q @runid@ "$tmp/runids" && fail "a run id left as @runid@"
[ "$(sort -u "$tmp/runids" | wc -l | tr -d ' ')" = 8 ] || fail "run ids: not one for each of the 8 tries: $(cat "$tmp/runids")"
calls=$(wc -l < "$tmp/ssh.log")
out2=$(D co wait "$id")
[ $? = 1 ] && [ "$out2" = "$out" ] || fail "a second wait differs: $out2"
out2=$(D co status "$id")
[ $? = 1 ] && [ "$out2" = "$out" ] || fail "status differs: $out2"
[ "$(wc -l < "$tmp/ssh.log")" = "$calls" ] || fail "collected, yet the PC was asked again"

# Cancel: every process of the batch, one in a session of its own too, and
# its claim on the budget; meanwhile a second batch of the checkout is
# refused, and a wait while the PC doesn't answer says so (exit 3).
printf '%s\n' "long sh -c 'sleep 60 & echo \$! > $tmp/kid1; setsid sleep 60 & echo \$! > $tmp/kid2; wait'" > "$tmp/co/list2"
id=$(bidof "$(D co start --detach list2)")
upto 30 test -s "$tmp/kid2" || fail "cancel: the run never started"
out=$(D co start --detach list2 2>&1)
[ $? = 2 ] && echo "$out" | grep -q "still runs on the PC" || fail "a second detached batch of one checkout: $out"
out=$(FAKE_PC_DOWN=1 PC_BATCH_WAIT=1 D co wait "$id")
[ $? = 3 ] && echo "$out" | grep -q "the PC didn't answer" || fail "wait with the PC down: $out"
out=$(D co cancel "$id")
[ $? = 0 ] || fail "cancel: $out"
upto 5 eval '! kill -0 $(cat "$tmp/kid1") 2> /dev/null && ! kill -0 $(cat "$tmp/kid2") 2> /dev/null' ||
    fail "cancel left a process of the batch: $(ps -p "$(cat "$tmp/kid1")" -p "$(cat "$tmp/kid2")" 2>&1)"
JAMOS_REMOTE_LOCKDIR=$tmp/slots python3 tools/remote-slots.py --status | grep -q "^remote-slots: 0 of" ||
    fail "cancel left a claim: $(JAMOS_REMOTE_LOCKDIR=$tmp/slots python3 tools/remote-slots.py --status)"
out=$(D co wait "$id")
[ $? = 1 ] || fail "wait after a cancel: $out"
row2 long CANCELLED
echo "$out" | grep -q "1 CANCELLED" || fail "cancelled summary: $out"

# A reboot of the PC mid-run: LOST, then --resume runs it again.
printf '%s\n' "quick true" "long sh -c 'echo \$\$ > $tmp/kid3; f=$tmp/lost.seen; [ -f \$f ] && exit 0; touch \$f; exec sleep 60'" > "$tmp/co/list3"
id=$(bidof "$(D co start --detach list3)")
b=$pc/work/.batch/co/$id
upto 30 test -f "$tmp/lost.seen" -a -f "$b/runs/quick.rc" || fail "lost: the runs never started"
# Everything of it dies at once: the keeper, the batch's group (the driver,
# the governors) and the command in its own session.
kill -9 $keeper
kill -9 -- "-$(cat "$b/pgid")" "$(cat "$tmp/kid3")"
out=$(PC_BATCH_WAIT=1 D co wait "$id")
[ $? = 1 ] || fail "wait after a reboot: $out"
row quick PASS 0
row2 long LOST
echo "$out" | grep -q -- "start --detach --resume $id" || fail "no resume hint: $out"
keeper_start
out=$(D co start --detach --resume "$id")
[ $? = 0 ] || fail "resume: $out"
out=$(D co wait "$id")
[ $? = 0 ] || fail "wait after resume: $out"
row quick PASS 0
row long PASS 0
[ -f "$tmp/co/build/batch/$id/pc/runs/long.lost.log" ] || fail "the lost try's log is gone"

# No keeper on the PC: the hand-off fails and says so.
kill $keeper
upto 5 eval '! kill -0 $keeper 2> /dev/null'
id=$(bidof "$(D co start --detach list3)")
out=$(D co wait "$id")
[ $? = 1 ] && echo "$out" | grep -q "no keeper runs on the PC" || fail "no keeper: $out"
row2 quick NOT-RUN

[ "$ok" = 1 ] && { echo "pc-batch-test: PASS"; exit 0; }
echo "pc-batch-test: FAIL"
exit 1
