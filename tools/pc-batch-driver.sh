#!/bin/bash
# The PC's half of a detached batch (tools/pc-batch.sh start --detach): runs
# a whole runlist on the test PC with nothing open to the Mac, so the Mac may
# sleep. pc-batch.sh copies this script into the batch's directory on the PC,
# ~/work/.batch/<checkout>/<id>/, as driver.sh, with what it runs:
#
#   names            the runs, in the runlist's order
#   settings         id, work (the checkout's copy, ~/work/<checkout>),
#                    jobs, stagger, rerun (shell assignments)
#   runs/<name>.sh   what remote-test.sh would run on the PC for the run (its
#                    JAMOS_REMOTE_SCRIPT_ONLY=1 output: the build check, then
#                    the command under the CPU budget, tools/remote-slots.py),
#                    with @runid@ where remote-test's run id goes
#   runs/<name>.outs the run's output directories (its arguments under build/)
#   detach.sh        tools/pc-detach.sh, which hands a job to the keeper
#
#   driver.sh submit   hand `driver.sh run` to the keeper (tools/pc-keeper.sh)
#   driver.sh run      the keeper's job: every run, then the file done
#   driver.sh pull <rsync's arguments>
#                      rsync's --rsync-path when the Mac looks: first notes
#                      the PC's time (now) and whether the batch still lives
#                      (gone, nokeeper), then rsync
#   driver.sh cancel   stop every process of the batch (its claims on the
#                      budget end with them)
#   driver.sh resume   after a reboot or a cancel: run what didn't finish
#
# The runs leave what pc-batch.sh's table reads, in runs/: <name>.t0, .t1,
# .rc and .log (and .try1.* for the first try of a run run again), and their
# output directories (but disk images) in out/<name>/, copied there as each
# run ends so that a later run of the checkout can't overwrite them before
# the Mac collects them. The driver holds an flock on lock for as long as it
# (or a run of it) lives: started, no done and a free lock means the PC lost
# the batch (a reboot), and `pull` writes gone.
set -u
b=$(cd "$(dirname "$0")" && pwd) || exit 2
cd "$b" || exit 2
id=? work=? jobs=32 stagger=0.3 rerun=0
. ./settings
now() { date +%s; }
names() { cat names; }

# Not running: nothing holds the lock.
idle() { flock -n lock true; }

# ---- run ----

run_one() {   # run_one <name> <try>: one try of a run
    local n=$1 r=runs/$1 rc a
    now > "$r.t0"
    : > "$r.log"
    # A run id of its own for each try (remote-test's ~/work/.runs/<id>).
    sed "s/@runid@/batch-$id-$n-$2-$(now)/g" "$r.sh" > "$r.run.sh"
    bash "$r.run.sh" < /dev/null >> "$r.log" 2>&1 9>&-
    rc=$?
    while IFS= read -r a; do
        [ -n "$a" ] && [ -d "$work/$a" ] || continue
        mkdir -p "out/$n/$a"
        rsync -a --delete --exclude '*.img' "$work/$a/" "out/$n/$a/" >> "$r.log" 2>&1
    done < "$r.outs"
    now > "$r.t1"
    echo $rc > "$r.rc.new"
    mv "$r.rc.new" "$r.rc"
}

launch() {   # launch <name> <try>: in the background once a job is free
    [ -z "$pids" ] || sleep "$stagger"
    while [ "$(jobs -rp | wc -l | tr -d ' ')" -ge "$jobs" ]; do sleep 1; done
    run_one "$1" "$2" &
    pids="$pids $!"
}

run() {
    local n
    exec 9>> lock
    flock -n 9 || { echo "pc-batch: batch $id runs already"; exit 1; }
    [ -f cancelled ] && { echo "pc-batch: batch $id was cancelled before it started"; exit 0; }
    # The keeper started this in a session of its own: its process group is
    # every process of the batch (cancel).
    ps -o pgid= -p $$ 2> /dev/null | tr -d ' ' > pgid
    now > started
    echo "pc-batch: batch $id started $(date)"
    pids=
    for n in $(names); do
        [ -f "runs/$n.rc" ] || launch "$n" 1
    done
    wait
    if [ "$rerun" = 1 ]; then
        pids=
        for n in $(names); do
            { [ "$(cat "runs/$n.rc")" = 0 ] || [ -f "runs/$n.try1.rc" ]; } && continue
            for f in log rc t0 t1; do mv "runs/$n.$f" "runs/$n.try1.$f"; done
            launch "$n" 2
        done
        wait
    fi
    now > done
    echo "pc-batch: batch $id done $(date)"
}

# ---- the Mac's looks ----

pull() {
    # The PC's clock (WSL's can drift from the Mac's, after Windows slept):
    # the Mac turns the PC's times into its own with it.
    now > "$b/now"
    if [ ! -f "$b/done" ] && [ ! -f "$b/gone" ] && [ ! -f "$b/cancelled" ]; then
        if [ -f "$b/started" ]; then
            # The driver writes done before it lets go of the lock: with the
            # lock in hand and still no done, it died (a reboot).
            flock -n "$b/lock" bash -c '[ -f "$1/done" ] || [ -f "$1/cancelled" ] || date +%s > "$1/gone"' - "$b"
        elif kill -0 "$(cat ~/.jamos-spool/keeper.pid 2> /dev/null)" 2> /dev/null; then
            rm -f "$b/nokeeper"
        else
            now > "$b/nokeeper"
        fi
    fi
    cd ~ && exec rsync "$@"
}

# ---- cancel, resume ----

procs() {   # procs <process group>: its processes and every descendant of them
    ps -A -o pid= -o ppid= -o pgid= 2> /dev/null | awk -v g="$1" '
        { p[NR] = $1; pp[NR] = $2; if ($3 == g) s[$1] = 1 }
        END {
            do {
                more = 0
                for (i = 1; i <= NR; i++)
                    if (!(p[i] in s) && (pp[i] in s)) { s[p[i]] = 1; more = 1 }
            } while (more)
            for (x in s) print x
        }'
}

cancel() {
    local pg list i=0
    [ -f done ] && { echo "pc-batch: batch $id had finished"; return 0; }
    now > cancelled
    # A job the keeper hasn't started yet: out of its spool.
    [ -s job ] && rm -f ~/.jamos-spool/"$(cat job)".job
    if idle; then
        echo "pc-batch: batch $id: nothing of it was running"
        return 0
    fi
    until [ -s pgid ] || [ $i -ge 10 ]; do sleep 1; i=$((i + 1)); done
    pg=$(cat pgid 2> /dev/null)
    [ -n "$pg" ] || { echo "pc-batch: batch $id: no process group"; return 1; }
    # The whole tree first, then the signal, so a process that a dying parent
    # leaves behind is not missed; what TERM leaves after 20 s gets KILL.
    list=$(procs "$pg")
    kill -TERM $list 2> /dev/null
    i=0
    until idle || [ $i -ge 20 ]; do sleep 1; i=$((i + 1)); done
    list="$list $(procs "$pg")"
    kill -KILL $list 2> /dev/null
    echo "pc-batch: batch $id: cancelled ($(echo $list | wc -w | tr -d ' ') processes stopped)"
}

submit() {
    local kp out
    [ -s job ] && { echo "pc-batch: batch $id: given to the keeper already (job $(cat job))"; return 0; }
    kp=$(cat ~/.jamos-spool/keeper.pid 2> /dev/null)
    if [ -z "$kp" ] || ! kill -0 "$kp" 2> /dev/null; then
        echo "pc-batch: no keeper runs on the PC (tools/pc-keeper.sh and its Windows task): a detached batch needs it" >&2
        return 4
    fi
    # Old batches (a week untouched, not running), every checkout's.
    find "${b%/*/*}" -mindepth 2 -maxdepth 2 -type d -mtime +7 2> /dev/null | while IFS= read -r o; do
        flock -n "$o/lock" true && rm -rf "$o"
    done
    out=$(bash ./detach.sh "$b/driver.out" bash "$b/driver.sh" run) || return 4
    echo "$out"
    echo "$out" | sed -n 's/^pc-detach: job \([^ ]*\) given to the keeper.*/\1/p' > job
    [ -s job ] || { echo "pc-batch: the keeper didn't take the job" >&2; return 4; }
}

resume() {
    local n k=0
    idle || { echo "pc-batch: batch $id is still running"; return 1; }
    [ -f done ] && { echo "pc-batch: batch $id had finished: nothing to resume"; return 1; }
    for n in $(names); do
        [ -f "runs/$n.rc" ] && continue
        [ -f "runs/$n.log" ] && mv "runs/$n.log" "runs/$n.lost.log"
        rm -f "runs/$n.t0" "runs/$n.t1"
        k=$((k + 1))
    done
    rm -f gone cancelled nokeeper started pgid job
    echo "pc-batch: batch $id: $k runs to run again"
    submit
}

case ${1:-} in
run) run ;;
pull) shift; pull "$@" ;;
cancel) cancel ;;
submit) submit ;;
resume) resume ;;
*) echo "usage: driver.sh run | pull <rsync args> | cancel | submit | resume" >&2; exit 2 ;;
esac
