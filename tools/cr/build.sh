#!/bin/bash
# Chromium's build on the PC (docs/CR-PLAN.md section 15, track CB1): the
# port ports/chromium built in a persistent tree on WSL, ~/cr/, outside
# the per-checkout copies tools/remote-test.sh makes, one build at a time
# (its own lock), at low priority, with a claim in the machine's budget
# (tools/remote-slots.py) for the CPUs and memory it uses for as long as it
# runs, so the QEMU tests shrink rather than overcommit the machine.
#
#   tools/cr/build.sh start [--clean] [--gen] [--dry] [--keep=N] [<ninja target>...]
#   tools/cr/build.sh status | wait [<seconds>] | stop | scan <outdir> | fetch <outdir>
#   tools/cr/build.sh size <ninja -n output file>      (the claim for a plan, no build)
#   JAMOS_REMOTE=jamos-pc JAMOS_REMOTE_ONLY=1 tools/remote-test.sh \
#       tools/cr/build.sh start
#   JAMOS_REMOTE_LIGHT_CPUS=1 JAMOS_REMOTE_LIGHT_MEM_MB=64 ... tools/remote-test.sh \
#       tools/cr/build.sh wait 270
#
# start   fetches Chromium's pinned tools (tools/cr/toolchain.sh), copies
#         this checkout to ~/cr/runs/<run> (a run never sees a later copy
#         of the checkout), counts what ninja would do (below), then claims
#         the CPUs and memory that work needs in the budget and starts the
#         build in the background (remote-slots.py --detach), and returns:
#         `ports.py build -f -j <CPUs> --kept ~/cr/<version> chromium` under
#         nice 19 and ionice's idle class, the ports' output in ~/cr/ports.
#         --clean removes the
#         kept tree first (a clean build); --gen stops after GN; --dry counts and says the
#         claim, then stops (nothing claimed or started; a plan changes no program and not the stage); --keep=N
#         lets ninja go on past N failures (to see many at once); targets
#         replace base_unittests, test_child_process, headless_shell and content_shell
#         (ports/chromium/build.sh reads them from ~/cr/<version>/build.conf).
#         A target linked against an older libc is linked again, and every
#         run that builds installs out/jamos's programs into the stage
#         (~/cr/ports/chromium/stage/lib/chromium, stripped: what cr-test.sh's
#         base case runs), so a relink refreshes it; the summary's "stage:"
#         lines say each program's build id and whether it is linked against
#         this run's libc (the stage's file times are the port's fixed
#         epoch, not the link's: they never tell).
#
# The claim is sized to the work (track BC, 2026-10-20: a claim of the whole
# 20 GB budget waits until every test on the machine has ended, and the
# tests of all the agents stand still behind it; a relink needs a few GB,
# not a compile's). start runs the port's `plan` (`ports.py build --plan`,
# which brings the kept tree up to date, runs GN if args.gn changed, and
# asks the builder's own ninja, ~/.cache/jamos/tools/bin/ninja, what `-n`
# would do: ports/chromium/build.sh, ~/cr/<version>/plan.txt) under the
# light claim of the remote-test.sh call that runs start (2 CPUs, 4 GB:
# remote-test.sh's cost_of), and counts the steps by kind: compiles (CXX,
# CC, ASM, Rust), links (LINK, SOLINK), the rest (archives, scripts,
# copies; STAMPs are free). Then:
#   nothing to do, or no compile and no link and at most 100 others,
#     or --gen (GN only)     2 CPUs, 4 GB
#   up to CR_LINK_BELOW (100) compiles and any links (a link of
#     content_shell takes about 10 GB, one at a time)   6 CPUs, 12 GB
#   more compiles (a header everyone includes, a changed GN argument,
#     --clean, no kept tree yet)    CR_JOBS CPUs (10), CR_MEM_MB (12288)
# CR_JOBS and CR_MEM_MB, each one given, win over the sizing (both given:
# nothing is counted, a hand-made claim). The step count and the claim are
# the first line start prints, the first line of the build's log and a line
# of `status` (~/cr/runs/<run>/claim). A count that fails (a patch that
# doesn't apply, a missing tool) ends start there, as the build would fail
# after waiting for its claim; a count GN leaves unsettled claims the full
# default. `size <file>` prints the claim for a saved `ninja -n` count
# (tools/cr/build-test.sh).
# status  whether a build runs, its run, its time so far, ninja's last
#         progress line, the peak memory so far; at the end its result,
#         time, peak memory and disk
# wait    status, after waiting up to <seconds> (270) for the build to end.
#         WSL 2.4 ends every process a session started when that session
#         ends (seen 2026-10-15), so `start` hands the build to the keeper
#         (tools/pc-keeper.sh, run at startup by the Windows task "Jam OS
#         WSL keep-alive"); with no keeper, keep a `wait 36000` connected
# stop    ends the running build (its session: nothing else is touched)
# pkgs    the pkg-config packages GN asks for with the kept tree's args.gn
#         (each one a system library the sysroot would need)
# scan    tools/cr/syscall-scan.py on the built programs
# fetch   the port's build directory (stage, stamp, port.sha256) into
#         <outdir>/chromium, for `ports.py package --from` on the Mac
#
# Memory is sampled every 5 s while the build runs: the sum of its
# processes' proportional set sizes (~/cr/runs/<run>/mem.txt, with the
# system's used memory; the peak in summary.txt) and the largest single
# process. The figures for BENCH.md
# are summary.txt's. Never kills or renices anything but its own build.
set -u
cr=${CR_DIR:-$HOME/cr}
# What the claim asks for: the variables when given, else (start) sized to the
# work, else (the build itself, which start gave them in its environment) the
# full default.
jobs=${CR_JOBS:-}
mem=${CR_MEM_MB:-}
full_jobs=10
full_mem=12288
tiny_jobs=2
tiny_mem=4096
link_jobs=6
link_mem=12288
link_below=${CR_LINK_BELOW:-100}   # compile steps up to this many are "link-mostly"
here=$(cd "$(dirname "$0")/../.." && pwd)
version=$(awk '$1 == "version" { print $2 }' "$here/ports/chromium/recipe")
kept=$cr/$version
ports=$cr/ports
mkdir -p "$cr/runs"

running() { ! flock -n "$cr/.lock" true 2> /dev/null; }
latest() { ls -1d "$cr"/runs/[0-9]* 2> /dev/null | tail -1; }

# count_value <counts file> <key>: a number from a plan's counts
# (tools/cr/ninja-count.awk's lines, or `unknown 1`); empty if not there.
count_value() { awk -v k="$2" '$1 == k { print $2 }' "$1" 2> /dev/null; }

# size_claim <counts file>: the claim for what ninja would do. Sets tier
# (tiny, link or full), counts (what ninja would do, in words) and why, and
# jobs and mem unless the caller gave them (CR_JOBS, CR_MEM_MB).
size_claim() {
    local f=$1 steps compile link stamp other j m
    steps=$(count_value "$f" steps)
    compile=$(count_value "$f" compile)
    link=$(count_value "$f" link)
    stamp=$(count_value "$f" stamp)
    other=$(count_value "$f" other)
    if [ -z "$steps" ]; then
        tier=full counts="ninja's steps could not be counted"
        why="the full default"
    else
        counts="ninja would run $steps steps ($compile compiles, $link links, $other others, $stamp stamps)"
        if [ "$steps" -eq 0 ]; then
            tier=tiny why="nothing to build: only the install"
        elif [ "$compile" -eq 0 ] && [ "$link" -eq 0 ] && [ "$other" -le 100 ]; then
            tier=tiny why="no compile and no link: generated files"
        elif [ "$compile" -le "$link_below" ]; then
            tier=link why="mostly links: a link's share"
        else
            tier=full why="a big compile: the full default"
        fi
    fi
    case $tier in
    tiny) j=$tiny_jobs m=$tiny_mem ;;
    link) j=$link_jobs m=$link_mem ;;
    *) j=$full_jobs m=$full_mem ;;
    esac
    [ -n "$jobs" ] || jobs=$j
    [ -n "$mem" ] || mem=$m
}

# plan_counts <run>: the port's plan (ports/chromium/build.sh: the kept tree
# brought up to date, GN if args.gn changed, ninja -n counted) into
# $kept/plan.txt, with the ~/cr/.lock held so that no build and no other
# plan changes the tree meanwhile. Its output is in <run>/plan.out.
plan_counts() {
    local r=$1 io=()
    command -v ionice > /dev/null && io=(ionice -c 3)
    rm -f "$kept/plan.txt"
    (
        flock -n 8 || { echo "cr-build: another build holds $cr/.lock"; exit 1; }
        cd "$r/checkout" || exit 2
        nice -n 19 ${io[@]+"${io[@]}"} python3 -u tools/ports.py --out "$ports" build --plan -f -j 1 \
            --kept "$kept" chromium
    ) 8> "$cr/.lock" > "$r/plan.out" 2>&1
}

status() {
    local run=${1:-$(latest)} now line
    [ -n "$run" ] || { echo "cr-build: no build has run here"; return; }
    now=$(date +%s)
    if running && [ -f "$run/started" ]; then
        echo "cr-build: running: run $(basename "$run"), $(( (now - $(cat "$run/started")) / 60 )) min so far"
    elif running; then
        echo "cr-build: run $(basename "$run") is waiting for its CPUs and memory, or another build runs"
        [ -f "$run/claim" ] && echo "cr-build: $(cat "$run/claim")"
    else
        echo "cr-build: not running; the last run: $(basename "$run")"
    fi
    line=$(grep -a '^\[[0-9]*/[0-9]*\]' "$ports/chromium/log" 2> /dev/null | tail -1 | cut -c1-160)
    [ -n "$line" ] && echo "cr-build: ninja: $line"
    [ -f "$run/peak" ] && echo "cr-build: peak memory so far: $(cat "$run/peak")"
    if [ -f "$run/summary.txt" ]; then
        sed 's/^/cr-build: /' "$run/summary.txt"
        grep -a -E 'error|FAILED|ports:' "$run/build.log" 2> /dev/null | tail -5 | sed 's/^/cr-build: /'
    fi
}

# The detached build itself (start's command): runs from the run's copy.
run() {
    local run=$1 t0 t1 rc
    jobs=${jobs:-$full_jobs}
    [ -f "$run/claim" ] && echo "cr-build: $(cat "$run/claim")"   # the first line of the build's log
    exec 9> "$cr/.lock"
    flock -n 9 || { echo "cr-build: another build holds $cr/.lock"; exit 1; }
    t0=$(date +%s)
    echo "$t0" > "$run/started"
    ps -o sid= -p $$ | tr -d ' ' > "$run/session"
    trap 'echo "result: stopped by a signal at $(date +%H:%M:%S)" > "$run/summary.txt"; exit 143' TERM HUP INT
    sample "$run" &
    local sampler=$!
    cd "$run/checkout" || { kill "$sampler" 2> /dev/null; exit 2; }
    local io=()
    command -v ionice > /dev/null && io=(ionice -c 3)
    local clean=()
    [ -f "$run/clean" ] && clean=(--clean)
    nice -n 19 ${io[@]+"${io[@]}"} python3 -u tools/ports.py --out "$ports" build -f -j "$jobs" \
        --kept "$kept" "${clean[@]}" chromium
    rc=$?
    t1=$(date +%s)
    kill "$sampler" 2> /dev/null
    wait "$sampler" 2> /dev/null
    {
        echo "result: $([ $rc = 0 ] && echo built || echo "FAILED (exit $rc)")"
        echo "time: $(( (t1 - t0) / 3600 )) h $(( (t1 - t0) % 3600 / 60 )) min ($((t1 - t0)) s), -j $jobs, links 1 at a time"
        echo "peak memory: $(cat "$run/peak" 2> /dev/null)"
        # ninja's own times (.ninja_log: start and end in ms of each step):
        # the wall time of its steps and their sum, the CPU work.
        awk -F'\t' 'NR > 1 { if (min == "" || $1 < min) min = $1; if ($2 > max) max = $2; s += $2 - $1; n++ }
            END { if (n) printf "ninja: %d steps in %d min of wall time, %d min of steps\n", n, (max - min) / 60000, s / 60000 }' \
            "$kept"/src/*/out/jamos/.ninja_log 2> /dev/null
        echo "disk: $(du -sh "$kept/src" 2> /dev/null | cut -f1) source and output ($(du -sh "$kept"/src/*/out 2> /dev/null | cut -f1) of it the output), the toolchains $(du -sh "${JAMOS_TOOLCHAINS:-$HOME/.cache/jamos/toolchains}" 2> /dev/null | cut -f1)"
        [ $rc = 0 ] && stage_lines
    } > "$run/summary.txt"
    cat "$run/summary.txt"
    exit $rc
}

# The stage's programs after a run that built (every run installs out/jamos's
# programs there, stripped, so a relink refreshes it), one line each: its
# build id (the same as out/jamos's program's; the stage's file times are
# the port's fixed epoch, never the link's, so they can't tell) and whether
# this run linked it against its libc (out/jamos/<program>.links against
# links.now, ports/chromium/build.sh's): one that wasn't among the targets
# keeps an older link.
stage_lines() {
    local out p f id now readelf
    out=$(ls -1d "$kept"/src/*/out/jamos 2> /dev/null | head -1)
    readelf=$("$here/tools/cr/toolchain.sh" --dir clang 2> /dev/null)/bin/llvm-readelf
    now=$(cat "$out/links.now" 2> /dev/null)
    for p in base_unittests test_child_process headless_shell content_shell; do
        f=$ports/chromium/stage/lib/chromium/$p
        [ -f "$f" ] || continue
        id=$("$readelf" -n "$f" 2> /dev/null | awk '/Build ID:/ { print $NF }')
        if [ -n "$now" ] && [ "$(cat "$out/$p.links" 2> /dev/null)" = "$now" ]; then
            echo "stage: $p, build id ${id:-?}: linked against this run's libc"
        else
            echo "stage: $p, build id ${id:-?}: an OLDER link (not a target: tools/cr/build.sh start $p)"
        fi
    done
}

# sample <run>: every 5 s, the build's processes' memory (its session's:
# the detached build is a session of its own), the peak kept. Each
# process counts its proportional share (Pss: the compilers' shared text
# once, not once a process), the system's used memory beside it in mem.txt.
sample() {
    local run=$1 sid peak=0 peak_one=0 one sum tick=0 peak_name=
    sid=$(ps -o sid= -p $$ | tr -d ' ')
    while sleep 5; do
        read -r sum one < <(for p in $(ps -o pid= --sid "$sid" 2> /dev/null); do
                awk -v c="$(cat "/proc/$p/comm" 2> /dev/null)" '/^Pss:/ { print $2, c }' \
                    "/proc/$p/smaps_rollup" 2> /dev/null
            done | awk '{ s += $1; if ($1 > m) { m = $1; c = $2 } } END { printf "%d %d:%s\n", s, m, c }')
        sum=${sum:-0}
        one=${one:-0:}
        if [ "$sum" -gt "$peak" ]; then
            peak=$sum
        fi
        if [ "${one%%:*}" -gt "$peak_one" ]; then
            peak_one=${one%%:*}
            peak_name=${one#*:}
        fi
        echo "$((peak / 1024)) MB in all, the largest process $((peak_one / 1024)) MB ($peak_name)" > "$run/peak"
        tick=$((tick + 1))
        [ $((tick % 6)) = 0 ] && echo "$(date +%s) $((sum / 1024)) $(free -m | awk '/^Mem:/ { print $3 }')" >> "$run/mem.txt"
    done
}

cmd=${1:-}
shift || true
case $cmd in
start)
    clean=0 gen=0 dry=0 keep= targets=()
    for a in "$@"; do
        case $a in
        --clean) clean=1 ;;
        --gen) gen=1 ;;
        --dry) dry=1 ;;
        --keep=*) keep=${a#--keep=} ;;
        *) targets+=("$a") ;;
        esac
    done
    if running; then
        echo "cr-build: a build is running (status: tools/cr/build.sh status)"
        status
        exit 1
    fi
    "$here/tools/cr/toolchain.sh" clang cmake rust gperf node sysroot test-fonts go || exit 1
    r=$cr/runs/$(date +%Y%m%d-%H%M%S)
    mkdir -p "$r/checkout"
    rsync -a --delete --exclude .git --exclude /build "$here/" "$r/checkout/" &&
        mkdir -p "$r/checkout/build" &&
        rsync -a "$here/build/sysroot" "$r/checkout/build/" || { echo "cr-build: copying the checkout failed"; exit 2; }
    [ $clean = 1 ] && touch "$r/clean"
    mkdir -p "$kept"
    {
        [ ${#targets[@]} -gt 0 ] && echo "targets=\"${targets[*]}\""
        [ $gen = 1 ] && echo "stop=gen"
        [ -n "$keep" ] && echo "keep=$keep"
    } > "$kept/build.conf"
    # Older runs' copies go (their logs and figures stay).
    ls -1d "$cr"/runs/[0-9]*/checkout 2> /dev/null | head -n -3 | xargs -r rm -rf
    # The claim: what the caller gave, else what the work needs.
    name=$(basename "$r")
    tier= counts= why=
    if [ -n "$jobs" ] && [ -n "$mem" ]; then
        counts="a claim made by hand" why="CR_JOBS and CR_MEM_MB"
    elif [ $gen = 1 ]; then
        counts="GN only" why="no compile, no link"
        [ -n "$jobs" ] || jobs=$tiny_jobs
        [ -n "$mem" ] || mem=$tiny_mem
    elif [ $clean = 1 ] || ! ls -d "$kept"/src/*/ > /dev/null 2>&1; then
        counts="a clean build: everything compiles" why="the full default"
        [ -n "$jobs" ] || jobs=$full_jobs
        [ -n "$mem" ] || mem=$full_mem
    else
        echo "cr-build: run $name: counting what ninja would do (the kept tree up to date, GN if args.gn changed, ninja -n)..."
        if ! plan_counts "$r"; then
            echo "cr-build: the count failed, so the build would fail the same way (not started); the end of $r/plan.out:"
            tail -n 15 "$r/plan.out" | sed 's/^/  /'
            exit 2
        fi
        size_claim "$kept/plan.txt"
    fi
    echo "run $name: $counts: claiming $jobs CPUs and $((mem / 1024)) GB ($why), log $r/build.log" > "$r/claim"
    echo "cr-build: $(cat "$r/claim")"
    if [ $dry = 1 ]; then
        echo "cr-build: --dry: nothing claimed, nothing started"
        rm -rf "$r"
        exit 0
    fi
    # Through the keeper (tools/pc-keeper.sh) when it runs, so the build
    # outlives this session; tools/pc-detach.sh falls back to a plain start.
    JAMOS_REMOTE_LABEL="Chromium build" "$here/tools/pc-detach.sh" "$r/start.log" \
        python3 "$here/tools/remote-slots.py" --detach "$r/build.log" \
        "$jobs" "$mem" env CR_JOBS="$jobs" CR_MEM_MB="$mem" \
        "$r/checkout/tools/cr/build.sh" run "$r" || exit 2
    ;;
run) run "$1" ;;
size)
    # The claim for a count with no build and no machine: a saved `ninja -n`
    # output (or tools/cr/ninja-count.awk's counts, as ~/cr/<version>/plan.txt
    # has them). tools/cr/build-test.sh runs it on faked outputs.
    f=${1:-}
    [ -f "$f" ] || { echo "usage: tools/cr/build.sh size <ninja -n output or counts file>" >&2; exit 2; }
    if ! grep -q -e '^steps ' -e '^unknown ' "$f"; then
        t=$(mktemp)
        awk -f "$here/tools/cr/ninja-count.awk" "$f" > "$t"
        f=$t
    fi
    tier=
    size_claim "$f"
    echo "jobs=$jobs mem=$mem tier=$tier"
    echo "$counts: claiming $jobs CPUs and $((mem / 1024)) GB ($why)"
    [ -z "${t:-}" ] || rm -f "$t"
    ;;
status) status ;;
pkgs)
    # Every system library (pkg-config package) GN asks for with the kept
    # tree's arguments: a GN run into a scratch directory whose pkg-config
    # only writes down what it is asked and answers nothing.
    src=$(ls -1d "$kept"/src/*/ | head -1)
    p=$cr/pkgs
    rm -rf "$p"
    mkdir -p "$p/out"
    {
        echo '#!/bin/sh'
        echo "echo \"\$*\" >> $p/asked"
    } > "$p/pkg-config"
    chmod +x "$p/pkg-config"
    grep -v '^pkg_config' "$src/out/jamos/args.gn" > "$p/out/args.gn"
    echo "pkg_config = \"$p/pkg-config\"" >> "$p/out/args.gn"
    (cd "$src" && buildtools/linux64/gn gen "$p/out" 2>&1 | grep -v '^ ' | head -20)
    echo "cr-build: pkg-config was asked for:"
    sort -u "$p/asked" 2> /dev/null | sed 's/^/  /'
    ;;
stop)
    # The running build's session (its own: remote-slots.py --detach), and
    # nothing else.
    r=$(latest)
    if ! running || [ ! -s "$r/session" ]; then
        echo "cr-build: no build is running"
        exit 1
    fi
    kill -TERM -- "-$(cat "$r/session")" && echo "cr-build: stopped run $(basename "$r")"
    ;;
wait)
    secs=${1:-270}
    case $secs in *[!0-9]*) secs=270 ;; esac
    end=$(( $(date +%s) + secs ))
    while running && [ "$(date +%s)" -lt "$end" ]; do sleep 10; done
    status
    ;;
scan)
    out=${1:-build/cr}
    mkdir -p "$out"
    rc=0
    for p in base_unittests headless_shell; do
        f=$ports/chromium/stage/lib/chromium/$p
        [ -f "$f" ] || { echo "cr-build: no $f"; rc=1; continue; }
        python3 "$here/tools/cr/syscall-scan.py" "$f" > "$out/scan-$p.txt" 2>&1 || rc=1
        echo "cr-build: scan $p: $(tail -1 "$out/scan-$p.txt")"
    done
    exit $rc
    ;;
fetch)
    out=${1:-build/cr}
    mkdir -p "$out/chromium"
    rsync -a --delete "$ports/chromium/stage" "$ports/chromium/stamp" "$ports/chromium/port.sha256" \
        "$out/chromium/" && echo "cr-build: $(du -sh "$out/chromium" | cut -f1) in $out/chromium"
    ;;
*)
    echo "usage: tools/cr/build.sh start [--clean] [--gen] [--dry] [--keep=N] [<target>...] | status | wait [<s>] | stop | size <file> | pkgs | scan|fetch <outdir>" >&2
    exit 2 ;;
esac
