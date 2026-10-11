#!/bin/sh

LANG=C
export LANG
LC_ALL=C
export LC_ALL

./_tinysshd-test-sig || exit 111

tmpdir=$(mktemp -d "${TMPDIR-/tmp}/tinyssh-signals.XXXXXXXXXX") || exit 111
serverpid=
inputopen=0

cleanup() {
    if test -n "$serverpid"; then
        kill "$serverpid" 2>/dev/null
        wait "$serverpid" 2>/dev/null
    fi
    if test "$inputopen" -eq 1; then
        exec 3>&-
    fi
    rm -rf "$tmpdir"
}
trap cleanup EXIT HUP INT TERM

keydir=$tmpdir/keydir
input=$tmpdir/input
output=$tmpdir/output

./tinysshd-makekey "$keydir" >/dev/null 2>&1 || exit 111
mkfifo "$input" || exit 111

./tinysshd -q "$keydir" <"$input" >"$output" 2>/dev/null &
serverpid=$!
exec 3>"$input"
inputopen=1

i=0
while test ! -s "$output" && test "$i" -lt 5; do
    sleep 1
    i=$((i + 1))
done
test -s "$output" || exit 111

kill -ALRM "$serverpid" || exit 111
wait "$serverpid"
status=$?
serverpid=

test "$status" -eq 111 || exit 111
echo 'test-signal-handlers: ok'
