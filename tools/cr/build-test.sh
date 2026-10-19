#!/bin/bash
# Host test of how tools/cr/build.sh sizes its claim in the PC's budget
# (track BC, docs/TESTING.md "Another machine for the tests"): no PC, no
# Chromium, about 5 s.
#   - `build.sh size <file>` on faked `ninja -n` outputs (and counts files):
#     nothing to do, STAMPs only, a relink of the four programs, a few
#     compiles and their links, exactly CR_LINK_BELOW compiles and one more, a
#     17,700-step build, CR_JOBS and CR_MEM_MB winning over the count, a count
#     GN left unsettled;
#   - tools/cr/ninja-count.awk's kinds (CXX CC ASM RUST compile; LINK SOLINK
#     SOLINK_MODULE link; STAMP free; AR ACTION COPY other);
#   - `build.sh start` in a fake checkout (a stand-in for the port's plan,
#     for the keeper and for the toolchain fetch): the first line says the
#     step count and the claim, the claim given to remote-slots.py --detach
#     and to the build's environment is the sized one, <run>/claim says it,
#     --gen and --clean and no kept tree and a claim given by hand count
#     nothing, a count that fails ends start without a claim, and the build's
#     own first line is the claim's.
# Usage: tools/cr/build-test.sh; exit 0 on PASS.
set -u
cd "$(dirname "$0")/../.." || exit 2
ok=1
fail() { echo "build-test: FAIL: $*"; ok=0; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/crbuild.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
build=tools/cr/build.sh

# gen <kind> <count> <total>: ninja -n's lines for <count> steps of one kind
gen() { awk -v k="$1" -v n="$2" -v t="$3" 'BEGIN { for (i = 1; i <= n; i++) printf "[%d/%d] %s obj/x%d.o\n", i, t, k, i }'; }
hdr() { printf "ninja: Entering directory \`out/jamos'\n"; }

# size <name> <expected "jobs mem tier"> <ninja -n output file> [VAR=value ...]
size() {
    local name=$1 want=$2 file=$3 got
    shift 3
    got=$(env -u CR_JOBS -u CR_MEM_MB -u CR_LINK_BELOW "$@" "$build" size "$file" 2>&1 | head -1)
    [ "$got" = "$want" ] || fail "size $name: \"$got\", wanted \"$want\""
}

{ hdr; echo "ninja: no work to do."; } > "$tmp/nothing"
size "nothing to do" "jobs=2 mem=4096 tier=tiny" "$tmp/nothing"
{ hdr; gen STAMP 3 3; } > "$tmp/stamps"
size "stamps only" "jobs=2 mem=4096 tier=tiny" "$tmp/stamps"
{ hdr; gen ACTION 40 41; gen STAMP 1 41; } > "$tmp/actions"
size "a few scripts" "jobs=2 mem=4096 tier=tiny" "$tmp/actions"
{ hdr; gen ACTION 101 101; } > "$tmp/manyactions"
size "over 100 scripts and no compile" "jobs=6 mem=12288 tier=link" "$tmp/manyactions"
{ hdr; gen LINK 4 7; gen ACTION 2 7; gen STAMP 1 7; } > "$tmp/relink"
size "a relink of four programs" "jobs=6 mem=12288 tier=link" "$tmp/relink"
{ hdr; gen CXX 3 9; gen AR 1 9; gen LINK 4 9; gen SOLINK 1 9; } > "$tmp/fewcc"
size "three compiles and the links" "jobs=6 mem=12288 tier=link" "$tmp/fewcc"
{ hdr; gen CXX 100 104; gen LINK 4 104; } > "$tmp/c100"
size "exactly 100 compiles" "jobs=6 mem=12288 tier=link" "$tmp/c100"
{ hdr; gen CXX 101 105; gen LINK 4 105; } > "$tmp/c101"
size "101 compiles" "jobs=10 mem=12288 tier=full" "$tmp/c101"
{ hdr; gen CXX 15000 17700; gen CC 90 17700; gen RUST 40 17700; gen ACTION 1900 17700; gen AR 600 17700; gen LINK 10 17700; gen STAMP 60 17700; } > "$tmp/chrome"
size "the chrome target (17,700 steps)" "jobs=10 mem=12288 tier=full" "$tmp/chrome"
{ hdr; gen CC 60 60; gen ASM 30 60; gen RUST 20 60; } > "$tmp/mixed"
size "CC, ASM and Rust count as compiles" "jobs=10 mem=12288 tier=full" "$tmp/mixed"
# CR_JOBS and CR_MEM_MB each win over the sizing; CR_LINK_BELOW moves the line
size "CR_JOBS" "jobs=20 mem=12288 tier=link" "$tmp/relink" CR_JOBS=20
size "CR_MEM_MB" "jobs=2 mem=16384 tier=tiny" "$tmp/nothing" CR_MEM_MB=16384
size "both" "jobs=16 mem=16384 tier=link" "$tmp/relink" CR_JOBS=16 CR_MEM_MB=16384
size "CR_LINK_BELOW" "jobs=10 mem=12288 tier=full" "$tmp/c100" CR_LINK_BELOW=50
# a counts file (what the port's plan writes): used as it is; `unknown` claims the full default
printf 'steps 12\ncompile 2\nlink 5\nstamp 1\nother 4\n' > "$tmp/counts"
size "a counts file" "jobs=6 mem=12288 tier=link" "$tmp/counts"
printf 'unknown 1\n' > "$tmp/unknown"
size "a count GN left unsettled" "jobs=10 mem=12288 tier=full" "$tmp/unknown"
# the counting's kinds
{ hdr; gen CXX 2 20; gen CC 1 20; gen ASM 1 20; gen RUST 1 20; gen LINK 1 20; gen SOLINK 1 20; gen SOLINK_MODULE 1 20
  gen STAMP 3 20; gen AR 2 20; gen ACTION 4 20; gen COPY 3 20; echo "ninja: build stopped"; } > "$tmp/kinds"
got=$(awk -f tools/cr/ninja-count.awk "$tmp/kinds" | tr '\n' ' ')
[ "$got" = "steps 20 compile 5 link 3 stamp 3 other 9 " ] || fail "ninja-count.awk: \"$got\""
got=$(awk -f tools/cr/ninja-count.awk "$tmp/nothing" | tr '\n' ' ')
[ "$got" = "steps 0 compile 0 link 0 stamp 0 other 0 " ] || fail "ninja-count.awk on no work: \"$got\""
"$build" size "$tmp/c101" 2> /dev/null | tail -1 | grep -q "ninja would run 105 steps (101 compiles, 4 links, 0 others, 0 stamps): claiming 10 CPUs and 12 GB" ||
    fail "size's second line: $("$build" size "$tmp/c101" 2>&1 | tail -1)"

# ---- start, in a fake checkout --------------------------------------------------------------------
fc=$tmp/checkout cr=$tmp/cr
mkdir -p "$fc/tools/cr" "$fc/ports/chromium" "$fc/build/sysroot" "$tmp/bin" "$cr/1.2.3/src/chromium-1.2.3"
cp "$build" "$fc/tools/cr/build.sh"
cp tools/cr/ninja-count.awk "$fc/tools/cr/"
echo "version 1.2.3" > "$fc/ports/chromium/recipe"
cat > "$fc/tools/cr/toolchain.sh" <<'EOF'
#!/bin/sh
exit 0
EOF
# the keeper's hand-off: says what it was asked to start
cat > "$fc/tools/pc-detach.sh" <<'EOF'
#!/bin/sh
echo "$*" > "$FAKE_DIR/detach.args"
EOF
# the port's plan: copies the faked counts into the kept tree, or fails
cat > "$fc/tools/ports.py" <<'EOF'
import os, shutil, sys
a = sys.argv[1:]
with open(os.path.join(os.environ["FAKE_DIR"], "ports.calls"), "a") as f:
    f.write(" ".join(a) + "\n")
if "--plan" not in a:
    sys.exit(0)
if os.environ.get("FAKE_PLAN_FAIL"):
    print("ports: chromium: patch 0913 doesn't apply")
    sys.exit(1)
shutil.copy(os.environ["FAKE_COUNTS"], os.path.join(a[a.index("--kept") + 1], "plan.txt"))
EOF
# no flock on the Mac: nothing is ever locked
printf '#!/bin/sh\nexit 0\n' > "$tmp/bin/flock"
chmod +x "$fc/tools/cr/toolchain.sh" "$fc/tools/pc-detach.sh" "$fc/tools/cr/build.sh" "$tmp/bin/flock"
export FAKE_DIR=$tmp
fb=$fc/tools/cr/build.sh

# start <name> <wanted first line's text> <wanted claim "jobs mem"|""> <wants a plan: yes|no> [start args...] (VAR=value in the environment)
start() {
    local name=$1 text=$2 claim=$3 plan=$4 out rc
    shift 4
    rm -f "$tmp/detach.args" "$tmp/ports.calls"
    out=$(env PATH="$tmp/bin:$PATH" CR_DIR="${CRD:-$cr}" "$fb" start "$@" 2>&1)
    rc=$?
    if [ -n "$claim" ]; then
        [ $rc = 0 ] || fail "start $name: exit $rc: $out"
        echo "$out" | grep -q "^cr-build: run [0-9-]*: $text" || fail "start $name: no line \"$text\": $out"
        grep -q " --detach .*build.log ${claim% *} ${claim#* } env CR_JOBS=${claim% *} CR_MEM_MB=${claim#* } " "$tmp/detach.args" 2> /dev/null ||
            fail "start $name: the claim given to --detach is not $claim: $(cat "$tmp/detach.args" 2> /dev/null)"
        run=$(ls -1d "${CRD:-$cr}"/runs/[0-9]* | tail -1)
        grep -q "^run [0-9-]*: $text" "$run/claim" 2> /dev/null || fail "start $name: $run/claim: $(cat "$run/claim" 2> /dev/null)"
    else
        [ $rc != 0 ] || fail "start $name: a failed count did not end start: $out"
        [ ! -f "$tmp/detach.args" ] || fail "start $name: a claim was made after a failed count"
        echo "$out" | grep -q "$text" || fail "start $name: no \"$text\": $out"
    fi
    if [ "$plan" = yes ]; then
        grep -q -e "--plan" "$tmp/ports.calls" 2> /dev/null || fail "start $name: the plan did not run"
    else
        ! grep -q -e "--plan" "$tmp/ports.calls" 2> /dev/null || fail "start $name: counted though it should not have: $(cat "$tmp/ports.calls")"
    fi
}
printf 'steps 7\ncompile 0\nlink 4\nstamp 1\nother 2\n' > "$tmp/c-link"
printf 'steps 0\ncompile 0\nlink 0\nstamp 0\nother 0\n' > "$tmp/c-nothing"
printf 'steps 17702\ncompile 15190\nlink 12\nstamp 60\nother 2440\n' > "$tmp/c-big"
export FAKE_COUNTS=$tmp/c-link
start "a relink" "ninja would run 7 steps (0 compiles, 4 links, 2 others, 1 stamps): claiming 6 CPUs and 12 GB (mostly links: a link's share)" "6 12288" yes
export FAKE_COUNTS=$tmp/c-nothing
start "nothing to do" "ninja would run 0 steps (0 compiles, 0 links, 0 others, 0 stamps): claiming 2 CPUs and 4 GB (nothing to build" "2 4096" yes
export FAKE_COUNTS=$tmp/c-big
start "a big compile" "ninja would run 17702 steps (15190 compiles, 12 links, 2440 others, 60 stamps): claiming 10 CPUs and 12 GB (a big compile" "10 12288" yes
start "a target" "ninja would run 17702 steps" "10 12288" yes chrome
grep -q 'targets="chrome"' "$cr/1.2.3/build.conf" || fail "the target did not reach build.conf: $(cat "$cr/1.2.3/build.conf" 2> /dev/null)"
export FAKE_COUNTS=$tmp/c-link
start "--gen" "GN only: claiming 2 CPUs and 4 GB" "2 4096" no --gen
grep -q "stop=gen" "$cr/1.2.3/build.conf" || fail "--gen did not reach build.conf"
start "--clean" "a clean build: everything compiles: claiming 10 CPUs and 12 GB" "10 12288" no --clean
CRD=$tmp/cr2 start "no kept tree yet" "a clean build: everything compiles: claiming 10 CPUs and 12 GB" "10 12288" no
export CR_JOBS=16 CR_MEM_MB=16384
start "a claim by hand" "a claim made by hand: claiming 16 CPUs and 16 GB (CR_JOBS and CR_MEM_MB)" "16 16384" no
unset CR_MEM_MB
start "CR_JOBS alone" "ninja would run 7 steps (0 compiles, 4 links, 2 others, 1 stamps): claiming 16 CPUs and 12 GB" "16 12288" yes
unset CR_JOBS
export FAKE_PLAN_FAIL=1
start "a failed count" "patch 0913 doesn't apply" "" yes
unset FAKE_PLAN_FAIL
# --dry: counts and says the claim, claims and starts nothing, leaves no run behind
# (a run directory is named by the second: the fake starts above must be over)
sleep 1.1
runs0=$(ls "$cr/runs" | wc -l)
rm -f "$tmp/detach.args" "$tmp/ports.calls"
out=$(env PATH="$tmp/bin:$PATH" CR_DIR="$cr" "$fb" start --dry 2>&1)
echo "$out" | grep -q "ninja would run 7 steps.*claiming 6 CPUs and 12 GB" && echo "$out" | grep -q "nothing claimed, nothing started" ||
    fail "start --dry: $out"
[ ! -f "$tmp/detach.args" ] || fail "start --dry claimed: $(cat "$tmp/detach.args")"
grep -q -e "--plan" "$tmp/ports.calls" || fail "start --dry did not count"
[ "$(ls "$cr/runs" | wc -l)" = "$runs0" ] || fail "start --dry left a run behind"
# the build's own first line is the claim (and status shows it while the run waits)
run=$(ls -1d "$cr"/runs/[0-9]* | tail -1)
echo "run X: ninja would run 7 steps: claiming 6 CPUs and 12 GB" > "$run/claim"
first=$(env PATH="$tmp/bin:$PATH" CR_DIR="$cr" CR_JOBS=6 CR_MEM_MB=12288 "$fb" run "$run" 2> /dev/null | head -1)
[ "$first" = "cr-build: run X: ninja would run 7 steps: claiming 6 CPUs and 12 GB" ] || fail "the build's first line: \"$first\""
grep -q -e "build -f -j 6 " "$tmp/ports.calls" || fail "the build did not run with -j 6: $(cat "$tmp/ports.calls")"

[ "$ok" = 1 ] && { echo "build-test: PASS"; exit 0; }
echo "build-test: FAIL"
exit 1
