#!/bin/bash
# Run a batch of tests on the test PC (tools/remote-test.sh) from one copy of
# this checkout: copy and build once, then every run at once (they queue on
# the PC's CPU budget), and one table at the end (docs/TESTING.md, "Running
# tests on the PC").
#
#   tools/pc-batch.sh start [--rerun-failed] <runlist>   start it; prints the batch id at once
#   tools/pc-batch.sh wait [<id>]     back when the batch is done (the table) or after
#                                     PC_BATCH_WAIT (270) seconds (one progress line, exit 124)
#   tools/pc-batch.sh report [<id>]   the table now
#   tools/pc-batch.sh cancel [<id>]   stop its runs
#   tools/pc-batch.sh [run] [--rerun-failed] <runlist>   start, then wait to the end (a terminal)
#
# <id> left out: this checkout's newest batch. The batch's files are in
# build/batch/<id>/: <name>.log is a run's output (remote-test's lines and
# the command's), the run's own output directory comes back as usual.
#
# A runlist has a line for each run, `<name> <command...>`; # starts a comment:
#
#   init   tools/qemu-test.sh build/t/init init init
#   kt4    tools/qemu-test.sh build/t/kt4 kt4 ktest
#   kt8    QEMU_SMP=8 tools/qemu-test.sh build/t/kt8 kt8 ktest
#   nvt1   env NVME_CASES="windows plain" tools/nvme-test.sh build/t/nvt1
#   cmds   QEMU_INPUT=tools/shell-tests/cmds.txt tools/qemu-test.sh build/t/cmds cmds shell
#
# The command is shell words (quotes work). VAR=value words before it (with
# or without `env`) are the run's environment, here (the CPUs it costs, a
# KTESTS=0 build) and on the PC whatever remote-test forwards or not.
# Every run uses the copy the batch made (JAMOS_REMOTE_NOSYNC=1), so don't
# start a second batch from this checkout with other code while one runs.
#
# Exit status (wait, report, run): 0 every run passed (or passed when run
# again), 1 one failed, 124 not finished yet, 2 a usage error.
# --rerun-failed: a failed run runs once more and passes as "rerun-PASS".
# The PC: JAMOS_REMOTE (jamos-pc), and only there (JAMOS_REMOTE_ONLY=1); if it
# doesn't answer, the batch (and a run that finds it gone, before it ran
# anything) tries again once after a minute. The runs start PC_BATCH_STAGGER
# (0.3) s apart, at most PC_BATCH_JOBS (32) are out at once (the rest wait
# their turn here, not as ssh sessions), and every ssh connection is tried
# again when the PC's sshd cuts it during the login (tools/ssh-retry.sh).
set -u
top=$(git rev-parse --show-toplevel 2> /dev/null) || top=$(pwd)
cd "$top" || exit 2
export JAMOS_REMOTE=${JAMOS_REMOTE:-jamos-pc} JAMOS_REMOTE_ONLY=1
base=${PC_BATCH_DIR:-build/batch}
rt=${PC_BATCH_REMOTE_TEST:-tools/remote-test.sh}   # tools/pc-batch-test.sh: a stand-in
me=tools/pc-batch.sh

die() { echo "pc-batch: $*" >&2; exit 2; }

# split <command text>: the words into assigns=(VAR=x ...) and cmdv=(program args...).
split() {
    assigns=() cmdv=()
    eval "set -- $1" || return 1
    [ "${1:-}" = env ] && shift
    while [ $# -gt 0 ] && [[ $1 == [A-Za-z_]*=* ]]; do
        assigns+=("$1")
        shift
    done
    [ $# -gt 0 ] || return 1
    cmdv=("$@")
}

# The names in a batch, in the runlist's order.
names() { cat "$d/names"; }

pick() {   # pick [<id>]: d=the batch's directory
    local id=${1:-}
    if [ -z "$id" ]; then
        id=$(ls -t "$base" 2> /dev/null | head -1)
        [ -n "$id" ] || die "no batch in $base"
    fi
    d=$base/$id
    [ -f "$d/names" ] || die "no batch $id (in $base)"
    bid=$id
}

now() { date +%s; }

# ---- the driver: runs in the background, one per batch ----

run_one() {   # run_one <name>: one run, its log, time and exit status
    local n=$1 rc tries=0
    split "$(cat "$d/$n.cmd")"
    now > "$d/$n.t0"
    : > "$d/$n.log"
    while :; do
        env ${assigns[@]+"${assigns[@]}"} JAMOS_REMOTE_NOSYNC=1 \
            "$rt" ${assigns[@]+env "${assigns[@]}"} "${cmdv[@]}" >> "$d/$n.log" 2>&1 < /dev/null
        rc=$?
        # remote-test's own "not run" (exit 3 with its line, after the ssh retries
        # of tools/ssh-retry.sh gave up): nothing ran, so once more is safe. A test
        # that exits 3 itself has no such line and is not run again.
        [ $rc = 3 ] && [ $tries = 0 ] && grep -q "doesn't answer, and JAMOS_REMOTE_ONLY=1: not run" "$d/$n.log" || break
        tries=1
        echo "pc-batch: the PC didn't answer: once more in a minute" >> "$d/$n.log"
        sleep "${PC_BATCH_RETRY_DELAY:-60}"
    done
    now > "$d/$n.t1"
    echo $rc > "$d/$n.rc.new"
    mv "$d/$n.rc.new" "$d/$n.rc"
}

launch() {   # launch <name>: run_one in the background once a job is free; adds to pids
    [ -z "$pids" ] || sleep "${PC_BATCH_STAGGER:-0.3}"
    while [ "$(jobs -rp | wc -l | tr -d ' ')" -ge "${PC_BATCH_JOBS:-32}" ]; do sleep 1; done
    run_one "$1" &
    pids="$pids $!"
}

drive() {
    d=$1
    local n tries=0 rc pids
    now > "$d/prepare.t0"
    while :; do
        "$rt" --prepare >> "$d/prepare.log" 2>&1 < /dev/null
        rc=$?
        # Runs that build without the kernel tests: that build too, now.
        if [ $rc = 0 ] && grep -q '^KTESTS=0$' "$d/variants" 2> /dev/null; then
            KTESTS=0 JAMOS_REMOTE_NOSYNC=1 "$rt" --prepare >> "$d/prepare.log" 2>&1 < /dev/null
            rc=$?
        fi
        [ $rc = 3 ] && [ $tries = 0 ] || break
        tries=1
        echo "pc-batch: the PC didn't answer: once more in a minute" >> "$d/prepare.log"
        sleep "${PC_BATCH_RETRY_DELAY:-60}"
    done
    now > "$d/prepare.t1"
    echo $rc > "$d/prepare.rc"
    if [ $rc = 0 ]; then
        # The runs start a little apart, and no more than PC_BATCH_JOBS are out
        # at once (each holds an ssh session and a waiting process on the PC
        # until the CPU budget has room for it): each opens its ssh connections
        # at once, and the PC's sshd turns some away when 10 or more are logging
        # in (tools/ssh-retry.sh also limits the logins).
        pids=
        for n in $(names); do
            launch "$n"
        done
        wait $pids
        if [ -f "$d/rerun-failed" ]; then
            pids=
            for n in $(names); do
                [ "$(cat "$d/$n.rc")" = 0 ] && continue
                for f in log rc t0 t1; do mv "$d/$n.$f" "$d/$n.try1.$f"; done
                launch "$n"
            done
            [ -z "$pids" ] || wait $pids
        fi
    fi
    now > "$d/done"
}

# ---- what the batch looks like ----

# result <name>: res= (PASS FAIL rerun-PASS RUNNING QUEUED NOT-RUN) secs= queued= note=
result() {
    local n=$1 rc t0 t1
    res= secs= queued= note=
    if [ -f "$d/$n.rc" ]; then
        rc=$(cat "$d/$n.rc")
        t0=$(cat "$d/$n.t0") t1=$(cat "$d/$n.t1")
        secs=$((t1 - t0))
        if [ "$rc" = 0 ]; then
            res=PASS
            [ -f "$d/$n.try1.rc" ] && res=rerun-PASS note="first try: exit $(cat "$d/$n.try1.rc") in $(($(cat "$d/$n.try1.t1") - $(cat "$d/$n.try1.t0"))) s, $d/$n.try1.log"
        else
            res=FAIL note="exit $rc"
            [ "$rc" = 3 ] && note="exit 3: the PC didn't answer"
            if [ "$rc" = 255 ]; then
                # The last word of tools/ssh-retry.sh in the log, and whether the command ever started.
                last=$(grep -E '^ssh-retry: (connection lost|not logged in)' "$d/$n.log" 2> /dev/null | tail -1)
                started=0
                grep -q '^remote-test: running with' "$d/$n.log" 2> /dev/null && started=1
                case $last in
                'ssh-retry: not logged in'*) note="exit 255: ssh could not log in to the PC (tools/ssh-retry.sh tried); the run did not finish" ;;
                'ssh-retry: connection lost'*)
                    if [ $started = 1 ]; then
                        note="exit 255: the ssh connection to the PC was lost after the run started (it may have gone on there)"
                    else
                        note="exit 255: the ssh connection to the PC was lost while the run waited for the budget (tried again, see the log)"
                    fi ;;
                esac
            fi
            [ -f "$d/$n.try1.rc" ] && note="$note (and the first try: exit $(cat "$d/$n.try1.rc"))"
        fi
    elif [ -f "$d/$n.t0" ]; then
        res=RUNNING secs=$(($(now) - $(cat "$d/$n.t0")))
        grep -q '^remote-test: running with' "$d/$n.log" 2> /dev/null || res=QUEUED
    elif [ -f "$d/prepare.rc" ] && [ "$(cat "$d/prepare.rc")" != 0 ]; then
        res=NOT-RUN note="copy or build failed"
    else
        res=QUEUED
    fi
    [ -f "$d/$n.log" ] && queued=$(sed -n 's/^remote-test: running with .*(after \([0-9]*\) s)$/\1/p' "$d/$n.log" | head -1)
    [ -n "$queued" ] || queued=0
}

# The first lines of a failed run's log that say what failed.
why() {
    local f=$1 lines
    lines=$(grep -E "FAIL|PANIC|TIMEOUT|PROBLEMS|MISMATCH|[Ee]rror|failed|can't|refus|doesn't answer|not run|usage:|: no '.*' within" "$f" 2> /dev/null |
        grep -vE '(^|[^0-9])0 FAIL|^remote-test: on ' | head -4)
    [ -n "$lines" ] || lines=$(tail -3 "$f" 2> /dev/null)
    printf '%s\n' "$lines" | cut -c1-200 | sed 's/^/      | /'
}

report() {
    local n w=4 pass=0 fail=0 rerun=0 other=0 total=0 outs a
    for n in $(names); do [ ${#n} -gt $w ] && w=${#n}; done
    for n in $(names); do
        result "$n"
        case $res in PASS) pass=$((pass + 1)) ;; rerun-PASS) rerun=$((rerun + 1)) ;; FAIL | NOT-RUN) fail=$((fail + 1)) ;; *) other=$((other + 1)) ;; esac
        total=$((total + 1))
    done
    local wall=$(( $(cat "$d/done" 2> /dev/null || now) - $(cat "$d/start") ))
    local prep=
    [ -f "$d/prepare.t1" ] && prep=", copy+build $(($(cat "$d/prepare.t1") - $(cat "$d/prepare.t0"))) s"
    local unfinished=
    [ $other -gt 0 ] && unfinished=", $other not finished"
    echo "batch $bid: $total runs: $pass PASS, $rerun rerun-PASS, $fail FAIL$unfinished; ${wall} s$prep"
    if [ -f "$d/prepare.rc" ] && [ "$(cat "$d/prepare.rc")" != 0 ]; then
        echo "  copy and build FAILED (exit $(cat "$d/prepare.rc")): $d/prepare.log"
        why "$d/prepare.log"
    fi
    printf "  %-${w}s  %-10s  %5s  %6s\n" name result secs queued
    for n in $(names); do
        result "$n"
        printf "  %-${w}s  %-10s  %5s  %6s\n" "$n" "$res" "$secs" "$queued"
        case $res in
        FAIL)
            outs=
            split "$(cat "$d/$n.cmd")"
            for a in "${cmdv[@]}"; do case $a in build/?*) outs="$outs ${a%/}" ;; esac; done
            echo "      $note; log $d/$n.log${outs:+; out$outs}"
            why "$d/$n.log" ;;
        rerun-PASS)
            echo "      $note"
            why "$d/$n.try1.log" ;;
        esac
    done
    [ $other -gt 0 ] && return 124
    [ $fail -gt 0 ] && return 1
    return 0
}

progress() {
    local n done=0 total=0 failed=0 running=0
    for n in $(names); do
        total=$((total + 1))
        result "$n"
        case $res in PASS | rerun-PASS) done=$((done + 1)) ;; FAIL | NOT-RUN) done=$((done + 1)); failed=$((failed + 1)) ;; RUNNING) running=$((running + 1)) ;; esac
    done
    local what="$done of $total done ($failed failed), $running running"
    [ -f "$d/prepare.rc" ] || what="copying and building"
    echo "batch $bid: $what, $(( $(now) - $(cat "$d/start") )) s so far: again: $me wait $bid"
}

alive() { kill -0 "$(cat "$d/driver.pid" 2> /dev/null)" 2> /dev/null; }

waitfor() {   # back when done or after PC_BATCH_WAIT seconds
    local end=$((SECONDS + ${PC_BATCH_WAIT:-270}))
    until [ -f "$d/done" ] || [ $SECONDS -ge $end ]; do
        alive || break
        sleep 3
    done
    if [ -f "$d/done" ]; then
        report
        return
    fi
    if ! alive; then
        echo "pc-batch: batch $bid's driver is gone (cancelled?): what it left:"
        report
        return 1
    fi
    progress
    return 124
}

start() {
    local rerun=0 list line name rest n id
    [ "${1:-}" = --rerun-failed ] && { rerun=1; shift; }
    [ $# = 1 ] || die "usage: $me start [--rerun-failed] <runlist>"
    list=$1
    [ -f "$list" ] || die "no runlist $list"
    mkdir -p "$base"
    id=$(date +%d%H%M%S)
    n=0
    until mkdir "$base/$id" 2> /dev/null; do n=$((n + 1)); id=$(date +%d%H%M%S)-$n; done
    d=$base/$id bid=$id
    : > "$d/names"
    while IFS= read -r line || [ -n "$line" ]; do
        line=${line#"${line%%[![:space:]]*}"}
        case $line in '' | '#'*) continue ;; esac
        read -r name rest <<< "$line"
        [[ $name =~ ^[A-Za-z0-9._-]+$ ]] || { rm -rf "$d"; die "$list: a name is letters, digits, . _ -: $name"; }
        grep -qx -- "$name" "$d/names" && { rm -rf "$d"; die "$list: $name twice"; }
        split "$rest" 2> /dev/null || { rm -rf "$d"; die "$list: $name: no command, or not shell words: $rest"; }
        case ${cmdv[0]} in */*) [ -f "${cmdv[0]}" ] || { rm -rf "$d"; die "$list: $name: no ${cmdv[0]} here"; } ;; esac
        for a in ${assigns[@]+"${assigns[@]}"}; do [ "$a" = KTESTS=0 ] && echo KTESTS=0 >> "$d/variants"; done
        printf '%s\n' "$rest" > "$d/$name.cmd"
        echo "$name" >> "$d/names"
    done < "$list"
    [ -s "$d/names" ] || { rm -rf "$d"; die "$list: no runs in it"; }
    cp "$list" "$d/runlist"
    [ $rerun = 1 ] && : > "$d/rerun-failed"
    now > "$d/start"
    for o in $(ls "$base"); do
        [ "$o" != "$id" ] && [ ! -f "$base/$o/done" ] && kill -0 "$(cat "$base/$o/driver.pid" 2> /dev/null)" 2> /dev/null &&
            echo "pc-batch: note: batch $o from this checkout is still running (both use one copy on the PC)" >&2
    done
    # The driver in a session of its own, so it outlives the caller's shell
    # (and cancel stops all of it); it writes driver.pid.
    nohup python3 -c '
import os, sys
try:
    os.setsid()
except OSError:          # a process group leader (a shell with job control): fork first
    if os.fork():
        os._exit(0)
    os.setsid()
open(sys.argv[1], "w").write("%d\n" % os.getpid())
os.execvp(sys.argv[2], sys.argv[2:])' "$d/driver.pid" bash "$me" _drive "$d" > "$d/driver.log" 2>&1 < /dev/null &
    n=0
    until [ -s "$d/driver.pid" ] || [ $n -ge 50 ]; do sleep 0.1; n=$((n + 1)); done
    [ -s "$d/driver.pid" ] || die "batch $id: the driver didn't start ($d/driver.log)"
    echo "batch $id: $(wc -l < "$d/names" | tr -d ' ') runs started; next: $me wait $id"
}

cmd=${1:-}
case $cmd in
start) shift; start "$@" ;;
wait) shift; pick "${1:-}"; waitfor ;;
report) shift; pick "${1:-}"; report ;;
cancel)
    shift; pick "${1:-}"
    alive || die "batch $bid isn't running"
    kill -TERM -- "-$(cat "$d/driver.pid")" 2> /dev/null || kill -TERM "$(cat "$d/driver.pid")"
    echo "batch $bid: cancelled" ;;
_drive) drive "$2" ;;
'' | -h | --help) sed -n '2,/^set -u/p' "$me" | sed '$d' | sed 's/^# \{0,1\}//'; exit 2 ;;
*)
    [ "$cmd" = run ] && shift
    out=$(start "$@") || exit $?
    echo "$out"
    pick "$(echo "$out" | sed -n 's/^batch \([^:]*\):.*/\1/p')"
    while :; do
        waitfor
        rc=$?
        [ $rc = 124 ] || exit $rc
    done ;;
esac
