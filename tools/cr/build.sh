#!/bin/bash
# Chromium's build on the PC (docs/CR-PLAN.md section 15, track CB1): the
# port ports/chromium built in a persistent tree on WSL, ~/cr/, outside
# the per-checkout copies tools/remote-test.sh makes, one build at a time
# (its own lock), at low priority, with a claim in the machine's budget
# (tools/remote-slots.py) for the CPUs and memory it uses for as long as it
# runs, so the QEMU tests shrink rather than overcommit the machine.
#
#   tools/cr/build.sh start [--clean] [--gen] [--keep=N] [<ninja target>...]
#   tools/cr/build.sh status | wait [<seconds>] | stop | scan <outdir> | fetch <outdir>
#   JAMOS_REMOTE=jamos-pc JAMOS_REMOTE_ONLY=1 tools/remote-test.sh \
#       tools/cr/build.sh start
#   JAMOS_REMOTE_LIGHT_CPUS=1 JAMOS_REMOTE_LIGHT_MEM_MB=64 ... tools/remote-test.sh \
#       tools/cr/build.sh wait 270
#
# start   fetches Chromium's pinned tools (tools/cr/toolchain.sh), copies
#         this checkout to ~/cr/runs/<run> (a run never sees a later copy
#         of the checkout), then claims CR_JOBS CPUs (10) and CR_MEM_MB of
#         memory (12288) in the budget and starts the build in the background
#         (remote-slots.py --detach), and returns: `ports.py build -f -j
#         CR_JOBS --kept ~/cr/<version> chromium` under nice 19 and ionice's
#         idle class, the ports' output in ~/cr/ports. --clean removes the
#         kept tree first (a clean build); --gen stops after GN; --keep=N
#         lets ninja go on past N failures (to see many at once); targets
#         replace base_unittests and headless_shell (ports/chromium/build.sh
#         reads these from ~/cr/<version>/build.conf).
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
jobs=${CR_JOBS:-10}
mem=${CR_MEM_MB:-12288}
here=$(cd "$(dirname "$0")/../.." && pwd)
version=$(awk '$1 == "version" { print $2 }' "$here/ports/chromium/recipe")
kept=$cr/$version
ports=$cr/ports
mkdir -p "$cr/runs"

running() { ! flock -n "$cr/.lock" true 2> /dev/null; }
latest() { ls -1d "$cr"/runs/[0-9]* 2> /dev/null | tail -1; }

status() {
    local run=${1:-$(latest)} now line
    [ -n "$run" ] || { echo "cr-build: no build has run here"; return; }
    now=$(date +%s)
    if running && [ -f "$run/started" ]; then
        echo "cr-build: running: run $(basename "$run"), $(( (now - $(cat "$run/started")) / 60 )) min so far"
    elif running; then
        echo "cr-build: run $(basename "$run") is waiting for its CPUs and memory, or another build runs"
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
    exec 9> "$cr/.lock"
    flock -n 9 || { echo "cr-build: another build holds $cr/.lock"; exit 1; }
    t0=$(date +%s)
    echo "$t0" > "$run/started"
    ps -o sid= -p $$ | tr -d ' ' > "$run/session"
    trap 'echo "result: stopped by a signal at $(date +%H:%M:%S)" > "$run/summary.txt"; exit 143' TERM HUP INT
    sample "$run" &
    local sampler=$!
    cd "$run/checkout" || exit 2
    local io=()
    command -v ionice > /dev/null && io=(ionice -c 3)
    local clean=()
    [ -f "$run/clean" ] && clean=(--clean)
    nice -n 19 "${io[@]}" python3 -u tools/ports.py --out "$ports" build -f -j "$jobs" \
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
    } > "$run/summary.txt"
    cat "$run/summary.txt"
    exit $rc
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
    clean=0 gen=0 keep= targets=()
    for a in "$@"; do
        case $a in
        --clean) clean=1 ;;
        --gen) gen=1 ;;
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
    echo "cr-build: run $(basename "$r"): claiming $jobs CPUs and $((mem / 1024)) GB, log $r/build.log"
    # Through the keeper (tools/pc-keeper.sh) when it runs, so the build
    # outlives this session; tools/pc-detach.sh falls back to a plain start.
    JAMOS_REMOTE_LABEL="Chromium build" "$here/tools/pc-detach.sh" "$r/start.log" \
        python3 "$here/tools/remote-slots.py" --detach "$r/build.log" \
        "$jobs" "$mem" "$r/checkout/tools/cr/build.sh" run "$r" || exit 2
    ;;
run) run "$1" ;;
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
    echo "usage: tools/cr/build.sh start [--clean] [--gen] [--keep=N] [<target>...] | status | wait [<s>] | stop | pkgs | scan|fetch <outdir>" >&2
    exit 2 ;;
esac
