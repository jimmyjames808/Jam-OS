#!/bin/sh
# The init run in parts: what `tools/qemu-test.sh <outdir> <name> init`
# runs in one boot (boot/init.cfg's programs: utest, usbtest, posixtest,
# one after another, about 130 s on the PC), split over a boot for each of
# init.cfg's `part <k>` lines, all at once (about 45 s). Each boot is an
# `init` run with the kernel word initpart=<k>: its RESULTS box and its
# verdict are those of a plain run of its programs, its root job checked
# clean as ever.
#
# Before the boots, the parts are checked against init.cfg's plain lines
# (also `--check`, no QEMU: `make check`): every program but utest in
# exactly one part, with its arguments; utest's cases split by `utest
# only <prefixes>` lines and one `utest except <prefixes>` whose list is
# every `only` list, no prefix the start of another (so no case runs
# twice and none is left out). After them: every part's boot ended with
# "run complete: no problems", and the parts' "utest: running <n> of <m>
# tests" lines add up to m.
#
# Logs: <outdir>/<name>-<k>.log for part k; <outdir>/<name>.log has each
# part's RESULTS box (its verdict line said as "part <k> complete: ..." or
# "part <k> FINISHED WITH PROBLEMS") and then the run's one verdict, as a
# one-boot run's log ends: "run complete: no problems" or "run FINISHED
# WITH PROBLEMS". Kernel words after the name go to every boot; QEMU_*
# pass through to tools/qemu-test.sh. On the PC each part costs a QEMU of
# the CPU budget (tools/remote-test.sh): at QEMU_SMP=8 that is 24 CPUs,
# which waits for an almost idle PC, so run the one-boot run at 8 instead.
# Usage: tools/init-test.sh <outdir> [name] [words...]
#        tools/init-test.sh --check
# Exit 0 if the run is complete with no problems.
set -u
cfg=boot/init.cfg

# The parts' problems, one line each (none: they are sound).
check_parts() {
    awk '
    function fail(m) { print "init-test: " FILENAME ": " m; bad = 1 }
    /^[ \t]*(#|$)/ { next }
    $1 != "part" {
        line = $0; sub(/^[ \t]+/, "", line); gsub(/[ \t]+/, " ", line); sub(/ $/, "", line)
        if ($1 == "bin/utest") { if (NF > 1) fail("the plain bin/utest line has arguments"); utest++ }
        else plain[line]++
        next
    }
    {
        if (NF < 3) { fail("line " NR ": a part with no program"); next }
        if ($2 !~ /^[1-9][0-9]?$/) { fail("line " NR ": part " $2 " is not 1 to 99"); next }
        parts[$2] = 1
        if ($3 != "bin/utest") {
            line = $3; for (i = 4; i <= NF; i++) line = line " " $i
            inpart[line]++
            next
        }
        if (NF != 5 || ($4 != "only" && $4 != "except")) {
            fail("line " NR ": utest in a part takes only <prefixes> or except <prefixes>"); next
        }
        n = split($5, p, ",")
        for (i = 1; i <= n; i++) {
            if (p[i] == "") { fail("line " NR ": an empty prefix"); continue }
            if ($4 == "only") onlyp[++nonly] = p[i]; else exceptp[p[i]]++
        }
        if ($4 == "except") excepts++
    }
    END {
        if (utest != 1) fail("want one plain bin/utest line, not " utest)
        if (excepts != 1) fail("want one utest except line in the parts, not " excepts + 0)
        for (l in plain) if (inpart[l] != plain[l]) fail("\"" l "\" is in " inpart[l] + 0 " part(s), want " plain[l])
        for (l in inpart) if (!(l in plain)) fail("\"" l "\" is in a part, not in the plain lines")
        for (i = 1; i <= nonly; i++) {
            if (!(onlyp[i] in exceptp)) fail("only prefix " onlyp[i] " is not in the except list")
            if (index("nvmew_", onlyp[i]) == 1) fail("only prefix " onlyp[i] " names nvmew_ (drive writes)")
            for (j = 1; j <= nonly; j++)
                if (i != j && index(onlyp[j], onlyp[i]) == 1)
                    fail("only prefix " onlyp[i] " starts " onlyp[j] ": a case would run twice")
            inonly[onlyp[i]] = 1
        }
        for (x in exceptp) if (!(x in inonly)) fail("except prefix " x " is in no only list")
        k = 0; for (x in parts) k++
        for (i = 1; i <= k; i++) if (!(i in parts)) fail("no part " i " (parts are 1 to " k ")")
        exit bad
    }' "$cfg"
}

if [ "${1:-}" = --check ]; then
    check_parts && echo "init-test: $cfg: the parts are sound"
    exit
fi
[ $# -ge 1 ] || { echo "usage: tools/init-test.sh <outdir> [name] [words...] | --check"; exit 2; }
out=$1 name=${2:-init}
[ $# -ge 2 ] && shift 2 || shift
mkdir -p "$out"
check_parts || { echo "$name: the parts don't add up to the run: not run"; exit 1; }
parts=$(awk '$1 == "part" { print $2 }' "$cfg" | sort -nu)

for k in $parts; do
    tools/qemu-test.sh "$out" "$name-$k" init "initpart=$k" "$@" > "$out/$name-$k.out" 2>&1 &
    eval "pid_$k=$!"
done
ok=1
for k in $parts; do
    eval "wait \$pid_$k" || ok=0
done

# The RESULTS box of a part's log, its verdict line renamed.
box() {
    awk '/==== RESULTS/ { on = 1; next } on && /=====/ { exit } on' "$out/$name-$1.log" |
        sed -e "s/run complete: no problems/part $1 complete: no problems/" \
            -e "s/run FINISHED WITH PROBLEMS/part $1 FINISHED WITH PROBLEMS/"
}
total=0 want=
{
    for k in $parts; do
        echo "==================== part $k: $(awk -v k="$k" '$1 == "part" && $2 == k { $1 = $2 = ""; sub(/^ +/, ""); printf "%s%s", s, $0; s = "; " }' "$cfg") ($name-$k.log) ===================="
        grep -v '^qemu-system' "$out/$name-$k.out"
        box "$k"
        if ! box "$k" | grep -q "part $k complete: no problems"; then
            echo "$name: part $k did not end with \"run complete: no problems\""
            ok=0
        fi
        counts=$(box "$k" | sed -n 's/.*utest: running \([0-9]*\) of \([0-9]*\) tests.*/\1 \2/p')
        if [ -n "$counts" ]; then
            set -- $counts
            total=$((total + $1))
            [ -z "$want" ] || [ "$want" = "$2" ] || { echo "$name: part $k's utest has $2 tests, another's $want"; ok=0; }
            want=$2
        fi
    done
    if [ -z "$want" ] || [ "$total" != "$want" ]; then
        echo "$name: the parts ran $total utest cases of ${want:-?}"
        ok=0
    fi
    echo "========================================================================="
    if [ $ok = 1 ]; then
        echo "run complete: no problems ($total utest cases in $(echo $parts | wc -w | tr -d ' ') parts)"
    else
        echo "run FINISHED WITH PROBLEMS"
    fi
} > "$out/$name.log"
grep -v '^\[' "$out/$name.log" | grep -v '^====='
for k in $parts; do
    grep -h 'init: exited with code' "$out/$name-$k.log" | tail -1 | sed "s/^/$name-$k: /"
done
[ $ok = 1 ]
