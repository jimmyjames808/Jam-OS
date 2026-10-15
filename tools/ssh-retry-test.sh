#!/bin/bash
# Host-side test of tools/ssh-retry.sh: a stand-in for ssh (SSH_RETRY_SSH)
# that plays a script of outcomes, no network, no PC. About 5 s.
#   - a login cut by the server (kex_exchange_identification, "Connection
#     reset by", "Connection closed by") is tried again, up to 6 tries, the
#     script piped in arrives whole on the try that connects, and the remote
#     command's stderr is passed through;
#   - nothing there ("Connection refused", "timed out") is tried 3 times;
#   - "Permission denied", a failure after the login (a reset in a session,
#     a remote command that exits 255 or 7) is NEVER tried again;
#   - the waits grow (1, 2, 4 s, each with up to half as much again) and
#     SSH_RETRY_TRIES=1 turns the retries off.
# Usage: tools/ssh-retry-test.sh; exit 0 on PASS.
set -u
cd "$(dirname "$0")/.." || exit 2
ok=1
fail() { echo "ssh-retry-test: FAIL: $*"; ok=0; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/sshretry.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/t"
export SSH_RETRY_SSH=$tmp/fake-ssh FAKE_DIR=$tmp SSH_RETRY_DELAY_MS=20 TMPDIR=$tmp/t SSH_RETRY_LOCKDIR=$tmp/locks
cat > "$tmp/fake-ssh" <<'EOF'
#!/bin/bash
# Pops the next outcome of $FAKE_DIR/plan; -E and LocalCommand as ssh does them.
log= lc=
while [ $# -gt 0 ]; do
    case $1 in
    -E) log=$2; shift ;;
    -o) case $2 in LocalCommand=*) lc=${2#LocalCommand=} ;; esac; shift ;;
    *) break ;;
    esac
    shift
done
echo x >> "$FAKE_DIR/calls"
if [ -n "${FAKE_SLOW:-}" ]; then   # the throttle test: logs in slowly, counts the logins in flight
    mkdir -p "$FAKE_DIR/inflight"
    touch "$FAKE_DIR/inflight/$$"
    ls "$FAKE_DIR/inflight" | wc -l | tr -d ' ' >> "$FAKE_DIR/inflight.counts"
    sleep 0.4
    rm -f "$FAKE_DIR/inflight/$$"
    sh -c "$lc"
    exit 0
fi
what=$(head -1 "$FAKE_DIR/plan")
sed 1d "$FAKE_DIR/plan" > "$FAKE_DIR/plan.new" && mv "$FAKE_DIR/plan.new" "$FAKE_DIR/plan"
login() { sh -c "$lc"; }
case $what in
reset)   echo "kex_exchange_identification: read: Connection reset by peer" >> "$log"
         echo "Connection reset by 10.0.0.1 port 22" >> "$log"; exit 255 ;;
closed)  echo "Connection closed by 10.0.0.1 port 22" >> "$log"; exit 255 ;;
refused) echo "ssh: connect to host 10.0.0.1 port 22: Connection refused" >> "$log"; exit 255 ;;
timeout) echo "ssh: connect to host 10.0.0.1 port 22: Operation timed out" >> "$log"; exit 255 ;;
denied)  echo "Jimmy@10.0.0.1: Permission denied (publickey)." >> "$log"; exit 255 ;;
mid)     login; echo "Connection reset by 10.0.0.1 port 22" >> "$log"; exit 255 ;;
r255)    login; exit 255 ;;
rc7)     login; exit 7 ;;
ok)      login; cat; echo "remote stderr" >&2; exit 0 ;;
*)       echo "fake-ssh: no outcome left" >&2; exit 99 ;;
esac
EOF
chmod +x "$tmp/fake-ssh"
R=tools/ssh-retry.sh

# run <outcomes...>: plan, then the helper with "hello" piped in; out= err= rc= calls=
run() {
    printf '%s\n' "$@" > "$tmp/plan"
    : > "$tmp/calls"
    out=$(printf 'hello\nscript\n' | $R -o BatchMode=yes host "wsl -e bash -s" 2> "$tmp/err")
    rc=$?
    err=$(cat "$tmp/err")
    calls=$(wc -l < "$tmp/calls" | tr -d ' ')
}
expect() {   # expect <what> <rc> <calls>
    [ "$rc" = "$2" ] || fail "$1: exit $rc, wanted $2: $err"
    [ "$calls" = "$3" ] || fail "$1: $calls tries, wanted $3: $err"
}

run ok
expect "a login that works" 0 1
[ "$out" = "$(printf 'hello\nscript')" ] || fail "stdin didn't arrive: $out"
echo "$err" | grep -q "^remote stderr$" || fail "the remote command's stderr is gone: $err"
echo "$err" | grep -q "ssh-retry" && fail "a note on a first-try login: $err"

run reset closed reset ok
expect "cut three times, then in" 0 4
[ "$out" = "$(printf 'hello\nscript')" ] || fail "stdin after retries: $out"
echo "$err" | grep -q "ssh-retry: kex_exchange_identification: read: Connection reset by peer: try 2 of 6 in " || fail "no note for try 2: $err"
echo "$err" | grep -q "ssh-retry: Connection closed by 10.0.0.1 port 22: try 3 of 6 in " || fail "no note for try 3: $err"
echo "$err" | grep -q "ssh-retry: connected on try 4" || fail "no connected note: $err"

run reset reset reset reset reset reset reset
expect "cut every time" 255 6
echo "$err" | grep -q "ssh-retry: not logged in: gave up after 6 tries" || fail "no give-up note: $err"
echo "$err" | grep -q "^kex_exchange_identification" || fail "ssh's own message not shown at the end: $err"

run refused refused refused refused
expect "nothing there" 255 3
run timeout refused ok
expect "timed out then refused then in" 0 3

run denied ok
expect "permission denied" 255 1
echo "$err" | grep -q "^ssh-retry: not logged in: this failure is not tried again" || fail "no 'not logged in' note: $err"
run mid ok
expect "a reset after the login" 255 1
echo "$err" | grep -q "^ssh-retry: connection lost after login: Connection reset by 10.0.0.1 port 22" || fail "no 'connection lost' note: $err"
run r255 ok
expect "a remote command that exits 255" 255 1
echo "$err" | grep -q "ssh-retry" && fail "a remote command's own exit 255 was called a lost connection: $err"
run rc7 ok
expect "a remote command that exits 7" 7 1
SSH_RETRY_TRIES=1 run reset ok
expect "SSH_RETRY_TRIES=1" 255 1

# The waits: 1, 2, 4 s (here 20, 40, 80 ms) plus up to half as much again.
SSH_RETRY_DELAY_MS=100 run reset reset reset ok
expect "waits" 0 4
waits=$(echo "$err" | sed -n 's/.*try [0-9] of 6 in \([0-9.]*\) s$/\1/p' | tr '\n' ' ')
echo "$waits" | awk '{ exit !($1 >= 0.100 && $1 <= 0.150 && $2 >= 0.200 && $2 <= 0.300 && $3 >= 0.400 && $3 <= 0.600 && NF == 3) }' ||
    fail "waits were: $waits"

# The login slots: 8 helpers at once, 2 slots: never more than 2 logging in, all get in,
# and a slot is free again once the login is through (not held by a session).
rm -rf "$tmp/inflight" "$tmp/inflight.counts" "$tmp/locks"
s=$SECONDS
for i in 1 2 3 4 5 6 7 8; do
    FAKE_SLOW=1 SSH_RETRY_LOGINS=2 $R host cmd < /dev/null > /dev/null 2>&1 &
done
wait
[ "$(wc -l < "$tmp/inflight.counts" | tr -d ' ')" = 8 ] || fail "throttle: $(wc -l < "$tmp/inflight.counts") logins of 8"
[ "$(sort -n "$tmp/inflight.counts" | tail -1)" -le 2 ] || fail "throttle: $(sort -n "$tmp/inflight.counts" | tail -1) logins at once with 2 slots"
[ $((SECONDS - s)) -ge 1 ] || fail "throttle: 8 logins of 0.4 s in 2 slots took only $((SECONDS - s)) s"
[ -z "$(ls "$tmp/locks")" ] || fail "throttle: slots left: $(ls "$tmp/locks")"
# Slots held by dead processes are taken over at once; a slot 5 minutes old too.
mkdir -p "$tmp/locks/slot1" "$tmp/locks/slot2"
echo 2147483646 > "$tmp/locks/slot1/pid"
echo 2147483645 > "$tmp/locks/slot2/pid"
printf 'ok\n' > "$tmp/plan"
s=$SECONDS
SSH_RETRY_LOGINS=2 $R host cmd < /dev/null > /dev/null 2>&1 || fail "stale slots: failed"
[ $((SECONDS - s)) -le 2 ] || fail "stale slots: waited $((SECONDS - s)) s"
mkdir -p "$tmp/locks/slot1"
echo $$ > "$tmp/locks/slot1/pid"
touch -t 202001010000 "$tmp/locks/slot1"
printf 'ok\n' > "$tmp/plan"
s=$SECONDS
SSH_RETRY_LOGINS=1 $R host cmd < /dev/null > /dev/null 2>&1 || fail "old slot: failed"
[ $((SECONDS - s)) -le 2 ] || fail "old slot: waited $((SECONDS - s)) s"
# With the limit off there is no lock directory at all.
rm -rf "$tmp/locks"
printf 'ok\n' > "$tmp/plan"
SSH_RETRY_LOGINS=0 $R host cmd < /dev/null > /dev/null 2>&1
[ ! -e "$tmp/locks" ] || fail "SSH_RETRY_LOGINS=0 made a lock directory"

# A script from a file and a plain pipe arrive alike; a command with no stdin at all.
printf 'reset\nok\n' > "$tmp/plan"
out=$($R host cmd < /dev/null 2> /dev/null)
[ $? = 0 ] && [ -z "$out" ] || fail "a run with /dev/null as stdin: $out"

# No marker file is left behind.
[ -z "$(ls "$tmp/t")" ] || fail "ssh-retry left a temp directory: $(ls "$tmp/t")"

[ "$ok" = 1 ] && { echo "ssh-retry-test: PASS"; exit 0; }
echo "ssh-retry-test: FAIL"
exit 1
