#!/bin/bash
# Host-side test of tools/remote-test.sh's ssh handling: a stand-in for ssh
# (SSH_RETRY_SSH, which tools/ssh-retry.sh runs) that plays a script of
# outcomes for the session that runs the command; no network, no PC. About 10 s.
#   - a session lost after the login while the command was still waiting for the
#     budget (no "running with" line yet) is started again, with the same run id
#     (the PC side stops what the lost one left), up to JAMOS_REMOTE_REQUEUE
#     (3) times, then exit 255;
#   - a session lost after the command started is NOT started again (exit 255);
#   - a command's own exit status (5) comes back, once;
#   - a login cut by the server is tried again by tools/ssh-retry.sh, invisibly;
#   - the session has keepalives, and its script has the run id;
#   - a cancel (SIGTERM) ends the session's ssh and asks the PC, in one more ssh,
#     to end the run by its pid file; a script with parts to run a few at a time
#     (init-test.sh) passes remote-slots.py its --shrink spec.
# Usage: tools/remote-test-test.sh; exit 0 on PASS.
set -u
cd "$(dirname "$0")/.." || exit 2
ok=1
fail() { echo "remote-test-test: FAIL: $*"; ok=0; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/rttest.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/t"
export SSH_RETRY_SSH=$tmp/fake-ssh FAKE_DIR=$tmp SSH_RETRY_DELAY_MS=20 SSH_RETRY_LOCKDIR=$tmp/locks
export JAMOS_REMOTE=fakehost JAMOS_REMOTE_ONLY=1 JAMOS_REMOTE_NOSYNC=1 JAMOS_REMOTE_REQUEUE_WAIT=0 TMPDIR=$tmp/t
cat > "$tmp/fake-ssh" <<'EOF'
#!/bin/bash
log= lc=
while [ $# -gt 0 ]; do
    case $1 in
    -E) log=$2; shift ;;
    -o) case $2 in LocalCommand=*) lc=${2#LocalCommand=} ;; esac; opts="${opts:-} $2"; shift ;;
    *) break ;;
    esac
    shift
done
host=$1
cmd=$2
login() { sh -c "$lc"; }
if [ "$cmd" != "wsl -e bash -s" ]; then login; exit 0; fi   # the probe
n=$(($(cat "$FAKE_DIR/sessions" 2> /dev/null || echo 0) + 1))
echo $n > "$FAKE_DIR/sessions"
what=$(head -1 "$FAKE_DIR/plan")
sed 1d "$FAKE_DIR/plan" > "$FAKE_DIR/plan.new" && mv "$FAKE_DIR/plan.new" "$FAKE_DIR/plan"
echo "$opts" > "$FAKE_DIR/opts.$n"
case $what in
reset)  echo "kex_exchange_identification: read: Connection reset by peer" >> "$log"; exit 255 ;;
esac
login
cat > "$FAKE_DIR/script.$n"
case $what in
lost)        echo "remote-test: waiting: 24 of 26 CPUs in use by 6 run(s) (this run needs 2 CPUs, 1.0 GB)" >&2
             echo "Read from remote host fakehost: Operation timed out" >> "$log"; exit 255 ;;
lostrunning) echo "remote-test: running with 3 of 26 CPUs in use (after 4 s)" >&2
             echo "client_loop: send disconnect: Broken pipe" >> "$log"; exit 255 ;;
ok)          echo "remote-test: running with 3 of 26 CPUs in use" >&2; echo "the command's output"; exit 0 ;;
rc5)         echo "remote-test: running with 3 of 26 CPUs in use" >&2; exit 5 ;;
hang)        echo "remote-test: waiting: 24 of 26 CPUs in use by 6 run(s) (this run needs 2 CPUs, 1.0 GB)" >&2; exec sleep 30 ;;
cancelled)   exit 0 ;;
*)           echo "fake-ssh: no outcome left" >&2; exit 99 ;;
esac
EOF
chmod +x "$tmp/fake-ssh"

# run <outcomes...>: out= err= rc= sessions=
run() {
    printf '%s\n' "$@" > "$tmp/plan"
    rm -f "$tmp/sessions" "$tmp"/script.* "$tmp"/opts.*
    out=$(tools/remote-test.sh true 2> "$tmp/err")
    rc=$?
    err=$(cat "$tmp/err")
    sessions=$(cat "$tmp/sessions" 2> /dev/null || echo 0)
}
expect() {   # expect <what> <rc> <sessions>
    [ "$rc" = "$2" ] || fail "$1: exit $rc, wanted $2: $err"
    [ "$sessions" = "$3" ] || fail "$1: $sessions sessions, wanted $3: $err"
}

run ok
expect "a run that works" 0 1
[ "$out" = "the command's output" ] || fail "stdout: $out"
echo "$err" | grep -q "^remote-test: running with 3 of 26" || fail "the session's stderr isn't live: $err"
grep -q 'ServerAliveInterval=20' "$tmp/opts.1" && grep -q -- 'ServerAliveCountMax=9' "$tmp/opts.1" || fail "no keepalives: $(cat "$tmp/opts.1")"
id=$(sed -n 's/^echo \$\$ > ~\/work\/\.runs\/\(.*\)$/\1/p' "$tmp/script.1")
[ -n "$id" ] || fail "no run id in the script: $(head -5 "$tmp/script.1")"
grep -q "^set -- 'true'\|^set -- true" "$tmp/script.1" || fail "the command isn't in the script: $(tail -3 "$tmp/script.1")"

run lost ok
expect "lost while waiting, then in" 0 2
echo "$err" | grep -q "the connection to fakehost was lost before the command started: again in" || fail "no note: $err"
id1=$(sed -n 's/^echo \$\$ > ~\/work\/\.runs\/\(.*\)$/\1/p' "$tmp/script.1")
id2=$(sed -n 's/^echo \$\$ > ~\/work\/\.runs\/\(.*\)$/\1/p' "$tmp/script.2")
[ -n "$id1" ] && [ "$id1" = "$id2" ] || fail "run ids differ: $id1 / $id2"
grep -q "kill \$old" "$tmp/script.2" || fail "the script doesn't stop the lost session's leftovers"

run lost lost lost lost lost
expect "lost every time" 255 4

run lostrunning ok
expect "lost after the command started" 255 1
echo "$err" | grep -q "again in" && fail "a started command was queued again: $err"

run rc5 ok
expect "the command's own exit status" 5 1

JAMOS_REMOTE_REQUEUE=0 run lost ok
expect "JAMOS_REMOTE_REQUEUE=0" 255 1

run reset reset ok
expect "a login cut twice (ssh-retry's)" 0 3
echo "$err" | grep -q "ssh-retry: kex_exchange_identification" || fail "no ssh-retry note: $err"

# A cancel (pc-batch cancel, ^C): SIGTERM ends the session's ssh at once and one more
# ssh asks the PC to end the run by its pid file (the governor kills the command's tree).
printf '%s\n' hang cancelled > "$tmp/plan"
rm -f "$tmp/sessions" "$tmp"/script.* "$tmp"/opts.*
tools/remote-test.sh true 2> "$tmp/err" &
rtpid=$!
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do [ -s "$tmp/script.1" ] && break; sleep 0.2; done
sleep 0.5
t0=$(date +%s)
kill -TERM "$rtpid"
wait "$rtpid"
rc=$?
t1=$(date +%s)
[ "$rc" = 143 ] || fail "a cancelled remote-test exited $rc, wanted 143: $(cat "$tmp/err")"
[ "$((t1 - t0))" -lt 10 ] || fail "a cancelled remote-test took $((t1 - t0)) s"
[ "$(cat "$tmp/sessions" 2> /dev/null)" = 2 ] || fail "a cancel made $(cat "$tmp/sessions" 2> /dev/null) sessions, wanted 2 (the run and the one that ends it)"
id=$(sed -n 's/^echo \$\$ > ~\/work\/\.runs\/\(.*\)$/\1/p' "$tmp/script.1")
grep -q "cat ~/work/.runs/$id" "$tmp/script.2" 2> /dev/null && grep -q 'kill \$p' "$tmp/script.2" || fail "the cancel doesn't kill the run by its pid file: $(cat "$tmp/script.2" 2> /dev/null)"
grep -q "remote-test: cancelled" "$tmp/err" || fail "no word of the cancel: $(cat "$tmp/err")"

# A script that runs parts a few at a time tells the governor how to run fewer (--shrink),
# and INIT_TEST_JOBS is forwarded.
printf '%s\n' ok > "$tmp/plan"
rm -f "$tmp/sessions" "$tmp"/script.*
INIT_TEST_JOBS=2 tools/remote-test.sh tools/init-test.sh build/test/x > /dev/null 2>&1
grep -q '^exec python3 tools/remote-slots.py --shrink INIT_TEST_JOBS:4:2048:2 8 4096 ' "$tmp/script.1" || fail "no --shrink in the script: $(tail -2 "$tmp/script.1")"
grep -q "INIT_TEST_JOBS=2" "$tmp/script.1" || fail "INIT_TEST_JOBS is not forwarded to the PC"

[ -z "$(ls "$tmp/t")" ] || fail "temp files left: $(ls "$tmp/t")"

[ "$ok" = 1 ] && { echo "remote-test-test: PASS"; exit 0; }
echo "remote-test-test: FAIL"
exit 1
