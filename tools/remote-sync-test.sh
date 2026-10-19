#!/bin/bash
# Test, on the PC, that tools/remote-test.sh's copy cannot leave a stale
# object behind: a file edited here while an older copy of it was being built
# there must be rebuilt by the next build, and a build with nothing new in it
# must still be skipped. About 1 minute; the PC must answer (JAMOS_REMOTE, jamos-pc).
#
# A tiny checkout (a Makefile whose one object takes SLOW seconds, and
# tools/remote-slots.py) is copied by the real remote-test.sh to
# ~/work/jamos-synctest on the PC, a few KB which the next run reuses:
#   1. version v1 of its source is copied and built (the recipe reads it, then
#      sleeps, then writes the object);
#   2. while the build sleeps, v2 is written here and copied alone
#      (JAMOS_REMOTE_COPY_ONLY=1): the older build ends after v2 arrived, so its
#      object (v1) is newer than the file (what the old copy, which kept this
#      machine's file times, left behind: the file was older still);
#   3. the next build (JAMOS_REMOTE_NOSYNC=1) must make the object again, from
#      v2: the PC's build log reads v1, v2, and the object says v2;
#   4. a copy and build once more, with nothing changed, says "built already".
# JAMOS_SYNC_TEST_RT=<remote-test.sh> runs the same steps with another copy of
# the script (the before/after of the change that made this test: the old
# script leaves the object at v1). REMOTE_SYNC_SLOW (20) is the recipe's sleep
# and REMOTE_SYNC_EDIT_AFTER (8) the seconds the first build gets to read v1
# before v2 is written: on a slow connection raise both.
# Usage: tools/remote-sync-test.sh; prints PASS or FAIL (exit 0 on PASS).
set -u
cd "$(dirname "$0")/.." || exit 2
repo=$(pwd)
rt=${JAMOS_SYNC_TEST_RT:-$repo/tools/remote-test.sh}
slow=${REMOTE_SYNC_SLOW:-20}
edit_after=${REMOTE_SYNC_EDIT_AFTER:-8}
export JAMOS_REMOTE=${JAMOS_REMOTE:-jamos-pc} JAMOS_REMOTE_ONLY=1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/rsynctest.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
mini=$tmp/jamos-synctest
mkdir -p "$mini/tools" || exit 2
cp tools/remote-slots.py "$mini/tools/"
printf '%s\n' \
    'all: build/obj' \
    'image: build/jamos.img' \
    'build/obj: src.txt' \
    "$(printf '\tmkdir -p build')" \
    "$(printf '\tv=$$(cat src.txt); echo "$$v" >> build/builds.log; sleep %d; echo "$$v" > build/obj' "$slow")" \
    'build/jamos.img: build/obj' \
    "$(printf '\tcp build/obj build/jamos.img')" > "$mini/Makefile"
run=$(date +%s)-$$
v1=v1-$run v2=v2-$run
fail() { echo "remote-sync-test: FAIL: $*"; exit 1; }
rtc() { (cd "$mini" && "$rt" "$@"); }   # the copy of the little checkout, like a run of any other

echo "$v1" > "$mini/src.txt"
rtc --prepare > "$tmp/b1.out" 2>&1 &
b1=$!
sleep "$edit_after"
echo "$v2" > "$mini/src.txt"
JAMOS_REMOTE_COPY_ONLY=1 rtc --prepare > "$tmp/copy.out" 2>&1 || { cat "$tmp/copy.out"; fail "the second copy failed"; }
wait "$b1" || { cat "$tmp/b1.out"; fail "the first copy and build failed"; }

JAMOS_REMOTE_NOSYNC=1 rtc sh -c 'cat build/obj; tail -2 build/builds.log' > "$tmp/c.out" 2> "$tmp/c.err" ||
    { cat "$tmp/c.err"; fail "the build after the copies failed"; }
obj=$(sed -n 1p "$tmp/c.out")
log=$(sed -n '2,3p' "$tmp/c.out" | tr '\n' ' ')
echo "remote-sync-test: object '$obj', the PC's build log '${log% }'"
[ "$obj" = "$v2" ] || fail "stale object: it says '$obj', the source is '$v2' and the build after the copy kept it"
[ "$log" = "$v1 $v2 " ] || fail "the first build did not read v1 before v2 arrived (timing? raise REMOTE_SYNC_EDIT_AFTER): its log should end '$v1 $v2'"

rtc --prepare > "$tmp/d.out" 2>&1 || { cat "$tmp/d.out"; fail "the last copy and build failed"; }
grep -q 'built already' "$tmp/d.out" || { cat "$tmp/d.out"; fail "a copy with nothing changed built again"; }
echo "remote-sync-test: PASS"
