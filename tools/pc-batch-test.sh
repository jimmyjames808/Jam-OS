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
#   - cancel stops the runs.
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
echo "$* NOSYNC=${JAMOS_REMOTE_NOSYNC:-} KTESTS=${KTESTS:-}" >> "$FAKE_DIR/calls"
[ "$1" = --prepare ] && exit "${FAKE_PREPARE_RC:-0}"
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

[ "$ok" = 1 ] && { echo "pc-batch-test: PASS"; exit 0; }
echo "pc-batch-test: FAIL"
exit 1
