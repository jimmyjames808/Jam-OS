#!/bin/bash
# Run a batch of tests on the test PC (tools/remote-test.sh) from one copy of
# this checkout: copy and build once, then every run at once (they queue on
# the PC's CPU budget), and one table at the end (docs/TESTING.md, "Running
# tests on the PC").
#
#   tools/pc-batch.sh start [--detach] [--rerun-failed] <runlist>
#                                     start it; prints the batch id at once
#   tools/pc-batch.sh wait [<id>]     back when the batch is done (the table) or after
#                                     PC_BATCH_WAIT (270) seconds (one progress line, exit 124)
#   tools/pc-batch.sh status [<id>]   the table now (a detached batch: after a look at the PC)
#   tools/pc-batch.sh report [<id>]   the table now, from what is here
#   tools/pc-batch.sh cancel [<id>]   stop its runs
#   tools/pc-batch.sh start --detach --resume <id>
#                                     a detached batch the PC lost (a reboot): run what didn't finish
#   tools/pc-batch.sh [run] [--detach] [--rerun-failed] <runlist>   start, then wait to the end (a terminal)
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
#
# --detach: the copy as above (the Mac awake for that alone, seconds), then
# the whole batch goes to the PC, which runs it on its own
# (tools/pc-batch-driver.sh, started by the PC's keeper, tools/pc-keeper.sh):
# no connection stays open, so the Mac may sleep once `wait` says "on the
# PC". Each run is what remote-test would run there (the same build check,
# variables and CPU budget): the first run builds, the others find it built. `wait` and `status` look at the PC with one
# short connection every PC_BATCH_POLL (30) s, and once it is over copy each
# run's output directory back to where the runlist named it (again, or days
# later, is fine). A run in flight when the PC rebooted is LOST; `start
# --detach --resume <id>` runs what didn't finish. `wait` exits 3 when the PC
# never answered during the call (the batch goes on there). One detached
# batch at a time for each checkout: it tests the checkout's copy on the PC,
# which another run from the checkout would change under it.
set -u
top=$(git rev-parse --show-toplevel 2> /dev/null) || top=$(pwd)
cd "$top" || exit 2
export JAMOS_REMOTE=${JAMOS_REMOTE:-jamos-pc} JAMOS_REMOTE_ONLY=1
base=${PC_BATCH_DIR:-build/batch}
rt=${PC_BATCH_REMOTE_TEST:-tools/remote-test.sh}   # tools/pc-batch-test.sh: a stand-in
sshr=${PC_BATCH_SSH:-$top/tools/ssh-retry.sh}      # and for ssh
ssh_opts=(-o BatchMode=yes -o ConnectTimeout=5 -o ServerAliveInterval=10 -o ServerAliveCountMax=3)
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
    runsdir
}

# rd: where the runs' files are: the batch's directory, or for a detached
# batch the copy of the PC's (pc/, as last seen; pc/runs/ the runs').
runsdir() {
    rd=$d
    [ -f "$d/detached" ] && rd=$d/pc/runs
    return 0
}

now() { date +%s; }

# ---- a detached batch's PC (one short connection each) ----

pc_sh() {   # the bash script on stdin, run on the PC
    "$sshr" "${ssh_opts[@]}" "$(cat "$d/pc-host")" "wsl -e bash -s"
}

# The PC's batch directory copied to pc/ (the driver's `pull` first notes
# whether the batch still lives there); fails when the PC didn't answer. A
# file the driver renamed while rsync looked (exit 24) is no failure: the
# next look has it.
pc_pull() {
    local dir rc
    dir=$(cat "$d/pc-dir")
    rsync -az --delete --rsync-path="wsl --cd ~ bash $dir/driver.sh pull" -e "$sshr ${ssh_opts[*]}" \
        "$(cat "$d/pc-host"):$dir/" "$d/pc/" > "$d/pull.log" 2>&1
    rc=$?
    [ $rc = 0 ] || [ $rc = 24 ] || return 1
    # How far the PC's clock is ahead of this one's (WSL's can drift).
    [ -s "$d/pc/now" ] && echo $(($(cat "$d/pc/now") - $(now))) > "$d/pc-skew"
    return 0
}

# A time on the PC's clock as this machine's (a detached batch).
pctime() { echo $(($1 - $(cat "$d/pc-skew" 2> /dev/null || echo 0))); }

pc_ended() { [ -f "$d/pc/done" ] || [ -f "$d/pc/gone" ] || [ -f "$d/pc/cancelled" ]; }

# Each run's output directories, from the PC's copies (pc/out/<name>/) to
# where the runlist named them, as remote-test brings them back.
collect() {
    local n a
    for n in $(names); do
        [ -f "$rd/$n.outs" ] || continue
        while IFS= read -r a; do
            [ -n "$a" ] || continue
            if [ -d "$d/pc/out/$n/$a" ]; then
                mkdir -p "$a" && cp -Rp "$d/pc/out/$n/$a/." "$a/"
            elif [ -f "$rd/$n.rc" ]; then
                echo "pc-batch: nothing came back for $a" >&2
            fi
        done < "$rd/$n.outs"
    done
    now > "$d/collected"
}

# The hand-off: what the PC needs in its batch directory (written from
# base64, file by file), then the driver's submit (the keeper).
handoff_script() {
    local f
    echo 'set -u'
    printf 'b=~/%s\n' "$(cat "$d/pc-dir")"
    # Given to the keeper already (this is a second try after a lost connection): nothing to change.
    echo '[ -s "$b/job" ] && exec bash "$b/driver.sh" submit'
    echo 'mkdir -p "$b/runs" && cd "$b" || exit 2'
    (cd "$d/pc" && find . -type f) | while IFS= read -r f; do
        printf "base64 -d > %q <<'@@'\n" "$f"
        base64 < "$d/pc/$f"
        echo
        echo '@@'
    done
    echo 'exec bash ./driver.sh submit'
}

handoff() {
    local n p=$d/pc rc tries=0 a
    now > "$d/handoff.t0"
    mkdir -p "$p/runs"
    for n in $(names); do
        split "$(cat "$d/$n.cmd")"
        # What remote-test would run on the PC for it (the same build check,
        # variables and claim on the budget), with a run id for each try.
        env ${assigns[@]+"${assigns[@]}"} JAMOS_REMOTE= JAMOS_REMOTE_NOSYNC=1 JAMOS_REMOTE_SCRIPT_ONLY=1 JAMOS_REMOTE_RUNID=@runid@ \
            "$rt" ${assigns[@]+env "${assigns[@]}"} "${cmdv[@]}" < /dev/null > "$p/runs/$n.sh" 2> "$d/script.err"
        rc=$?
        if [ $rc != 0 ] || ! grep -q . "$p/runs/$n.sh"; then
            cat "$d/script.err" >> "$d/handoff.log"
            echo "pc-batch: $n: remote-test made no script for the PC (exit $rc)" >> "$d/handoff.log"
            echo 2 > "$d/handoff.rc"
            return 1
        fi
        sed -i.x '/^export JAMOS_REMOTE_SCRIPT_ONLY=/d' "$p/runs/$n.sh" && rm -f "$p/runs/$n.sh.x"
        for a in "${cmdv[@]}"; do case $a in build/?*) echo "${a%/}" ;; esac; done > "$p/runs/$n.outs"
    done
    rm -f "$d/script.err"
    names > "$p/names"
    {
        printf 'id=%q\n' "$bid"
        printf 'work=~/work/%q\n' "$(basename "$top")"
        printf 'jobs=%q\nstagger=%q\n' "${PC_BATCH_JOBS:-32}" "${PC_BATCH_STAGGER:-0.3}"
        printf 'rerun=%d\n' "$([ -f "$d/rerun-failed" ] && echo 1 || echo 0)"
    } > "$p/settings"
    cp tools/pc-batch-driver.sh "$p/driver.sh"
    cp tools/pc-detach.sh "$p/detach.sh"
    while :; do
        handoff_script | pc_sh >> "$d/handoff.log" 2>&1
        rc=$?
        # ssh's own failure: once more in a minute (a batch the keeper has
        # already is not given to it again).
        [ $rc = 255 ] && [ $tries = 0 ] || break
        tries=1
        echo "pc-batch: the connection to the PC failed: once more in a minute" >> "$d/handoff.log"
        sleep "${PC_BATCH_RETRY_DELAY:-60}"
    done
    now > "$d/handoff.t1"
    echo $rc > "$d/handoff.rc"
    [ $rc = 0 ] && now > "$d/handed"
    [ $rc = 0 ]
}

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
    d=$1 bid=${1##*/}
    runsdir
    local n tries=0 rc pids
    now > "$d/prepare.t0"
    while :; do
        if [ -f "$d/detached" ]; then
            # Detached: the copy alone. The PC builds before the first run
            # (each run's own build check, remote-test's), so the Mac needn't
            # stay awake while the build waits for the budget.
            JAMOS_REMOTE_COPY_ONLY=1 "$rt" --prepare >> "$d/prepare.log" 2>&1 < /dev/null
            rc=$?
        else
            "$rt" --prepare >> "$d/prepare.log" 2>&1 < /dev/null
            rc=$?
        fi
        # Runs that build without the kernel tests: that build too, now.
        if [ $rc = 0 ] && [ ! -f "$d/detached" ] && grep -q '^KTESTS=0$' "$d/variants" 2> /dev/null; then
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
    # Detached: the PC has the batch now, and this driver's part is over
    # (done is the PC's, pc/done).
    if [ $rc = 0 ] && [ -f "$d/detached" ]; then
        handoff || now > "$d/done"
        return
    fi
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

# result <name>: res= (PASS FAIL rerun-PASS RUNNING QUEUED NOT-RUN LOST CANCELLED) secs= queued= note=
result() {
    local n=$1 rc t0 t1
    res= secs= queued= note=
    if [ -f "$rd/$n.rc" ]; then
        rc=$(cat "$rd/$n.rc")
        t0=$(cat "$rd/$n.t0") t1=$(cat "$rd/$n.t1")
        secs=$((t1 - t0))
        if [ "$rc" = 0 ]; then
            res=PASS
            [ -f "$rd/$n.try1.rc" ] && res=rerun-PASS note="first try: exit $(cat "$rd/$n.try1.rc") in $(($(cat "$rd/$n.try1.t1") - $(cat "$rd/$n.try1.t0"))) s, $rd/$n.try1.log"
        else
            res=FAIL note="exit $rc"
            [ "$rc" = 3 ] && note="exit 3: the PC didn't answer"
            if [ "$rc" = 255 ]; then
                # The last word of tools/ssh-retry.sh in the log, and whether the command ever started.
                last=$(grep -E '^ssh-retry: (connection lost|not logged in)' "$rd/$n.log" 2> /dev/null | tail -1)
                started=0
                grep -q '^remote-test: running with' "$rd/$n.log" 2> /dev/null && started=1
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
            [ -f "$rd/$n.try1.rc" ] && note="$note (and the first try: exit $(cat "$rd/$n.try1.rc"))"
        fi
    elif [ -f "$rd/$n.t0" ] && [ -f "$d/pc/gone" ]; then
        res=LOST note="the PC lost it while it ran (a reboot?): $me start --detach --resume $bid runs it again"
    elif [ -f "$rd/$n.t0" ] && [ -f "$d/pc/cancelled" ]; then
        res=CANCELLED note="cancelled before it finished"
    elif [ -f "$rd/$n.t0" ]; then
        res=RUNNING secs=$(($(now) - $(pctime "$(cat "$rd/$n.t0")")))
        grep -q '^remote-test: running with' "$rd/$n.log" 2> /dev/null || res=QUEUED
    elif [ -f "$d/prepare.rc" ] && [ "$(cat "$d/prepare.rc")" != 0 ]; then
        res=NOT-RUN note="copy or build failed"
    elif [ -f "$d/handoff.rc" ] && [ "$(cat "$d/handoff.rc")" != 0 ]; then
        res=NOT-RUN note="handing the batch to the PC failed"
    elif [ -f "$d/pc/gone" ] || [ -f "$d/pc/cancelled" ]; then
        res=NOT-RUN note="the batch ended before it ran (cancelled, or the PC lost it)"
    else
        res=QUEUED
    fi
    [ -f "$rd/$n.log" ] && queued=$(sed -n 's/^remote-test: running with .*(after \([0-9]*\) s)$/\1/p' "$rd/$n.log" | head -1)
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
    local n w=4 pass=0 fail=0 rerun=0 lost=0 cancelled=0 other=0 total=0 outs a
    for n in $(names); do [ ${#n} -gt $w ] && w=${#n}; done
    for n in $(names); do
        result "$n"
        case $res in
        PASS) pass=$((pass + 1)) ;;
        rerun-PASS) rerun=$((rerun + 1)) ;;
        FAIL | NOT-RUN) fail=$((fail + 1)) ;;
        LOST) lost=$((lost + 1)) ;;
        CANCELLED) cancelled=$((cancelled + 1)) ;;
        *) other=$((other + 1)) ;;
        esac
        total=$((total + 1))
    done
    local fin
    fin=$(cat "$d/done" 2> /dev/null)
    if [ -z "$fin" ]; then   # a detached batch's end, on the PC's clock
        fin=$(cat "$d/pc/done" "$d/pc/gone" "$d/pc/cancelled" 2> /dev/null | head -1)
        [ -z "$fin" ] || fin=$(pctime "$fin")
    fi
    local wall=$(( ${fin:-$(now)} - $(cat "$d/start") ))
    local prep=
    local how=copy+build
    [ -f "$d/detached" ] && how=copy
    [ -f "$d/prepare.t1" ] && prep=", $how $(($(cat "$d/prepare.t1") - $(cat "$d/prepare.t0"))) s"
    local more=
    [ $lost -gt 0 ] && more="$more, $lost LOST"
    [ $cancelled -gt 0 ] && more="$more, $cancelled CANCELLED"
    [ $other -gt 0 ] && more="$more, $other not finished"
    echo "batch $bid: $total runs: $pass PASS, $rerun rerun-PASS, $fail FAIL$more; ${wall} s$prep"
    if [ -f "$d/prepare.rc" ] && [ "$(cat "$d/prepare.rc")" != 0 ]; then
        echo "  copy and build FAILED (exit $(cat "$d/prepare.rc")): $d/prepare.log"
        why "$d/prepare.log"
    fi
    if [ -f "$d/handoff.rc" ] && [ "$(cat "$d/handoff.rc")" != 0 ]; then
        echo "  handing it to the PC FAILED (exit $(cat "$d/handoff.rc")): $d/handoff.log"
        why "$d/handoff.log"
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
            echo "      $note; log $rd/$n.log${outs:+; out$outs}"
            why "$rd/$n.log" ;;
        rerun-PASS)
            echo "      $note"
            why "$rd/$n.try1.log" ;;
        LOST | CANCELLED)
            echo "      $note; log $rd/$n.log" ;;
        esac
    done
    [ $other -gt 0 ] && return 124
    [ $((fail + lost + cancelled)) -gt 0 ] && return 1
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
    if [ ! -f "$d/prepare.rc" ]; then
        what="copying and building"
        [ -f "$d/detached" ] && what="copying"
    elif [ -f "$d/detached" ] && [ ! -f "$d/handed" ]; then
        what="handing it to the PC"
    elif [ -f "$d/pc/nokeeper" ]; then
        what="on the PC, but its keeper isn't running (tools/pc-keeper.sh): nothing runs"
    elif [ -f "$d/handed" ] && [ ! -f "$d/pc/started" ]; then
        what="on the PC (the Mac may sleep), waiting for its keeper to start it"
    elif [ -f "$d/handed" ]; then
        what="on the PC (the Mac may sleep): $what"
    fi
    echo "batch $bid: $what, $(( $(now) - $(cat "$d/start") )) s so far: again: $me wait $bid"
}

alive() { kill -0 "$(cat "$d/driver.pid" 2> /dev/null)" 2> /dev/null; }

pcwait() {   # pcwait <end>: a detached batch on the PC: a short look every PC_BATCH_POLL s
    local end=$1 seen=0 pause poll=${PC_BATCH_POLL:-30}
    if [ -f "$d/collected" ]; then
        report
        return
    fi
    while :; do
        if pc_pull; then
            seen=1
            if pc_ended; then
                collect
                report
                return
            fi
        fi
        pause=$((end - SECONDS))
        [ $pause -gt 0 ] || break
        [ $pause -le "$poll" ] || pause=$poll
        sleep "$pause"
    done
    if [ $seen = 0 ]; then
        echo "batch $bid: the PC didn't answer ($(grep . "$d/pull.log" | tail -1 | cut -c1-120)); the batch goes on there: again later: $me wait $bid"
        return 3
    fi
    progress
    return 124
}

waitfor() {   # back when done or after PC_BATCH_WAIT seconds
    local end=$((SECONDS + ${PC_BATCH_WAIT:-270}))
    until [ -f "$d/done" ] || [ -f "$d/handed" ] || [ $SECONDS -ge $end ]; do
        alive || break
        sleep 3
    done
    if [ -f "$d/handed" ]; then
        pcwait $end
        return
    fi
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

resume() {   # resume <id>: a detached batch's runs that didn't finish, on the PC again
    pick "$1"
    [ -f "$d/handed" ] || die "batch $bid was not handed to the PC (only a detached batch resumes)"
    pc_pull || die "batch $bid: the PC doesn't answer"
    printf 'exec bash ~/%s/driver.sh resume\n' "$(cat "$d/pc-dir")" | pc_sh || exit 1
    rm -f "$d/collected"
    pc_pull
    echo "batch $bid: on the PC again; next: $me wait $bid"
}

start() {
    local rerun=0 detach=0 list line name rest n id busy
    while :; do
        case ${1:-} in
        --rerun-failed) rerun=1; shift ;;
        --detach) detach=1; shift ;;
        --resume) [ $detach = 1 ] && [ $# = 2 ] || die "usage: $me start --detach --resume <id>"; resume "$2"; return ;;
        *) break ;;
        esac
    done
    [ $# = 1 ] || die "usage: $me start [--detach] [--rerun-failed] <runlist>"
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
    if [ $detach = 1 ]; then
        echo "$JAMOS_REMOTE" > "$d/pc-host"
        echo "work/.batch/$(basename "$top")/$id" > "$d/pc-dir"
        # A detached batch of this checkout still running there tests the
        # checkout's copy on the PC, which this one would change under it.
        busy=$(printf 'for o in ~/work/.batch/%s/*/; do [ -f "$o/started" ] && [ ! -f "$o/done" ] && ! flock -n "$o/lock" true && basename "$o"; done; exit 0\n' \
            "$(basename "$top")" | pc_sh 2> /dev/null)
        [ -z "$busy" ] || { rm -rf "$d"; die "batch $(echo $busy) of this checkout still runs on the PC (on this checkout's copy there): wait for it or cancel it first"; }
        : > "$d/detached"
    fi
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
    local how=
    [ $detach = 1 ] && how=" (detached: the PC builds and runs them once copied; the Mac may sleep once wait says \"on the PC\")"
    echo "batch $id: $(wc -l < "$d/names" | tr -d ' ') runs started$how; next: $me wait $id"
}

cmd=${1:-}
case $cmd in
start) shift; start "$@" ;;
wait) shift; pick "${1:-}"; waitfor ;;
report) shift; pick "${1:-}"; report ;;
status)
    shift; pick "${1:-}"
    if [ -f "$d/handed" ] && [ ! -f "$d/collected" ]; then
        if pc_pull; then
            pc_ended && collect
        else
            echo "pc-batch: the PC didn't answer: the table as last seen" >&2
        fi
    fi
    report ;;
cancel)
    shift; pick "${1:-}"
    did=0
    if alive; then
        kill -TERM -- "-$(cat "$d/driver.pid")" 2> /dev/null || kill -TERM "$(cat "$d/driver.pid")"
        did=1
    fi
    # Detached: on the PC too, once the hand-off has begun.
    if [ -f "$d/detached" ] && [ -f "$d/handoff.t0" ]; then
        printf 'f=~/%s/driver.sh\n[ -f "$f" ] && exec bash "$f" cancel\necho "pc-batch: nothing of batch %s on the PC"\n' \
            "$(cat "$d/pc-dir")" "$bid" | pc_sh || die "batch $bid: the PC didn't answer: cancel again"
        pc_pull
        did=1
    fi
    [ $did = 1 ] || die "batch $bid isn't running"
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
