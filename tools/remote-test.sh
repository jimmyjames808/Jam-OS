#!/bin/bash
# Run a test command on a faster machine, if there is one: this checkout is
# copied to it (no .git, no build/), built there (`make` and `make image`, so the image
# is never stale) and the command run there, with KVM; the output
# directories named in its arguments (those under build/) come back, but
# stick images, and the command's exit status is
# this script's. Without JAMOS_REMOTE, or when it doesn't answer (the PC is
# off, or booted into Jam OS: 5 s a try, 3 tries), the command runs here as it
# would have, unless JAMOS_REMOTE_ONLY=1: then it isn't run (exit 3). Every
# ssh and rsync goes through tools/ssh-retry.sh: a connection that the PC's
# sshd cut during the login (many runs starting at once) is tried again, a
# command that has started never is.
#
#   tools/remote-test.sh tools/qemu-test.sh build/test init init
#   QEMU_SMP=8 tools/remote-test.sh tools/qemu-test.sh build/test kt8 ktest
#   tools/remote-test.sh tools/mixer-test.sh build/test/mixer
#
# JAMOS_REMOTE: an ssh host whose login runs Windows' OpenSSH, with WSL's
# Ubuntu behind `wsl` (docs/TESTING.md, "Another machine for the tests").
# The QEMU_*, KTESTS, JAMOS_*, PC_RUNS, *_CASES, SOAK_*, PORTS_TEST_* and WL_PORT_* variables go with the command
# (others: `tools/remote-test.sh env VAR=value <command>`). Each
# checkout has a directory of its own there (~/work/<its name>), so several
# checkouts (agents' worktrees) test at once; one checkout's builds take
# turns (flock).
#
# The machine's CPUs are a budget (tools/remote-slots.py, docs/TESTING.md):
# every command, whichever checkout it comes from, costs what it can use,
# and starts when that fits in JAMOS_REMOTE_CPUS (26 virtual CPUs of the
# PC's 28) and JAMOS_REMOTE_MEM_GB (20) with the commands already running;
# otherwise it waits ("waiting: 24 of 26 CPUs in use"). More at once made
# timing tests fail; fewer left the PC idle behind a queue of small runs.
#   - a QEMU run: QEMU_SMP CPUs (4 by default, qemu-test.sh's) and QEMU_MEM
#     (2G) of memory; a script that boots several at once (nvme-driver-test
#     one for each case, ports-test PORTS_TEST_JOBS, wl-port-test WL_PORT_JOBS,
#     init-test one for each part of boot/init.cfg) the total of them all;
#   - the build before it: JAMOS_REMOTE_BUILD_CPUS (8), and `make -j` that;
#   - anything else (`make check`, a checker): JAMOS_REMOTE_LIGHT_CPUS (2) and
#     JAMOS_REMOTE_LIGHT_MEM_MB (1024); say more for a heavy one (a big build).
# A command that costs more than the whole budget runs alone. For fewer
# QEMUs at once (the owner is using the PC), one variable: JAMOS_REMOTE_CPUS=12
# (JAMOS_REMOTE_SLOTS=n caps the number of commands too). To change it for
# everyone waiting at once, write the number into ~/work/.cpu-budget on the PC.
#
# A build with nothing to do costs nothing: the machine keeps a stamp of the
# tree it last built for this checkout (build/.remote-built-k<KTESTS>: every
# file's name, size and time, and KTESTS, JAMOS_VLAN and JAMOS_DATA_FS), and when the tree
# and the stamp agree and the image is there, the build is skipped without
# asking the budget for its CPUs. JAMOS_REMOTE_REBUILD=1 builds anyway.
#
# Several commands from one copy (tools/pc-batch.sh does this):
#   tools/remote-test.sh --prepare     copy and build only, run nothing
#                                      (JAMOS_REMOTE_COPY_ONLY=1: copy only)
#   JAMOS_REMOTE_NOSYNC=1 tools/remote-test.sh <command>
#                                      don't copy: use what the last copy left
#   JAMOS_REMOTE_SCRIPT_ONLY=1 tools/remote-test.sh <command>
#                                      print what the PC would run, run nothing
#                                      (pc-batch.sh start --detach; JAMOS_REMOTE_RUNID
#                                      names the run instead of a new id)
set -u
[ $# -gt 0 ] || { echo "usage: tools/remote-test.sh <command> [args...] | --prepare"; exit 2; }
prepare=0
if [ "$1" = --prepare ]; then
    [ $# -eq 1 ] || { echo "usage: tools/remote-test.sh --prepare" >&2; exit 2; }
    prepare=1
fi
top=$(git rev-parse --show-toplevel 2>/dev/null) || top=$(pwd)
# Every ssh and rsync below goes through tools/ssh-retry.sh: a connection the
# PC's sshd cut during the login (many runs starting at once) is tried again.
sshr=$(cd "$(dirname "$0")" && pwd)/ssh-retry.sh
cd "$top" || exit 2

# What the command takes of the machine: "<virtual CPUs> <MB of memory>".
# JAMOS_REMOTE_COST_ONLY=1 prints it and stops (tools/remote-slots-test.sh).
cost_of() {
    local smp=${QEMU_SMP:-} mem=${QEMU_MEM:-} jobs=${PORTS_TEST_JOBS:-} wljobs=${WL_PORT_JOBS:-} itjobs=${INIT_TEST_JOBS:-} i=0 a script qemus=1 after knob=
    local args=("$@")
    if [ "${args[0]:-}" = env ]; then
        i=1
        while [[ ${args[i]:-} == [A-Za-z_]*=* ]]; do
            a=${args[i]}
            case $a in QEMU_SMP=*) smp=${a#*=} ;; QEMU_MEM=*) mem=${a#*=} ;; PORTS_TEST_JOBS=*) jobs=${a#*=} ;; WL_PORT_JOBS=*) wljobs=${a#*=} ;; INIT_TEST_JOBS=*) itjobs=${a#*=} ;; esac
            i=$((i + 1))
        done
    fi
    case ${args[i]:-} in sh | bash | python3) i=$((i + 1)) ;; esac
    script=${args[i]:-}
    after=$(($# - i - 2))   # arguments after the script and its output directory
    [ "$after" -ge 0 ] || after=0
    smp=${smp%%[!0-9]*}
    [ -n "$smp" ] && [ "$smp" -ge 1 ] || smp=4   # qemu-test.sh's default
    case $mem in
    *[Gg]) mem=$((${mem%?} * 1024)) ;;
    *[Mm]) mem=${mem%?} ;;
    esac
    [[ $mem =~ ^[0-9]+$ ]] || mem=2048
    case ${script##*/} in
    qemu-test.sh) ;;
    nvme-driver-test.sh) qemus=6; [ "$after" -eq 0 ] || qemus=$after ;;   # a boot for each case, all at once
    init-test.sh)   # a boot for each part, INIT_TEST_JOBS at a time
        knob=INIT_TEST_JOBS
        qemus=$(awk '$1 == "part" { print $2 }' boot/init.cfg | sort -u | wc -l | tr -d ' ')
        [[ ${itjobs:-} =~ ^[0-9]+$ ]] && [ "$itjobs" -ge 1 ] && [ "$itjobs" -lt "$qemus" ] && qemus=$itjobs ;;
    ports-test.sh)
        knob=PORTS_TEST_JOBS
        [[ ${jobs:-} =~ ^[0-9]+$ ]] || jobs=3
        qemus=$jobs
        [ "$after" -eq 0 ] || [ "$after" -ge "$jobs" ] || qemus=$after ;;
    wl-port-test.sh)   # WL_PORT_JOBS (3) groups at a time, one QEMU each
        knob=WL_PORT_JOBS
        [[ ${wljobs:-} =~ ^[0-9]+$ ]] || wljobs=3
        qemus=$wljobs
        [ "$after" -ge 1 ] || after=9   # no group named: all nine
        [ "$after" -ge "$wljobs" ] || qemus=$after ;;
    update-menu-test.sh) smp=2 ;;   # its QEMUs are -smp 2
    *) if [ ! -f "$script" ] || ! grep -q qemu "$script"; then   # no QEMU: make check, a checker
           echo "${JAMOS_REMOTE_LIGHT_CPUS:-2} ${JAMOS_REMOTE_LIGHT_MEM_MB:-1024}"
           return
       fi ;;
    esac
    # A script that runs its parts a few at a time also says how to run fewer at
    # once ("VAR:cpus:mem_mb:parts", remote-slots.py --shrink): it takes fewer when
    # a long claim leaves the machine too little for all of them.
    echo "$((qemus * smp)) $((qemus * mem))${knob:+ $knob:$smp:$mem:$qemus}"
}
if [ "${JAMOS_REMOTE_COST_ONLY:-0}" = 1 ]; then
    cost_of "$@"
    exit 0
fi
cost=$(cost_of "$@")
host=${JAMOS_REMOTE:-}
ssh_opts=(-o BatchMode=yes -o ConnectTimeout=5)
# The session that runs the command lives for as long as it waits for the
# budget and runs (hours, in a big batch): keepalives stop an idle connection
# from being dropped by a VPN or NAT on the way, and end a dead one in ~3 min.
ssh_main_opts=("${ssh_opts[@]}" -o ServerAliveInterval=20 -o ServerAliveCountMax=9)
script_only=${JAMOS_REMOTE_SCRIPT_ONLY:-0}
if [ "$script_only" != 1 ] && { [ -z "$host" ] || ! "$sshr" "${ssh_opts[@]}" "$host" "wsl -e true" < /dev/null > /dev/null 2>&1; }; then
    if [ "${JAMOS_REMOTE_ONLY:-0}" = 1 ]; then   # the tests are the other machine's alone
        echo "remote-test: ${host:-no JAMOS_REMOTE} doesn't answer, and JAMOS_REMOTE_ONLY=1: not run" >&2
        exit 3
    fi
    [ -n "$host" ] && echo "remote-test: $host doesn't answer: running here" >&2
    [ $prepare = 1 ] && { make -s && make -s image; exit; }
    exec "$@"
fi
name=$(basename "$top")
dir=work/$name
nosync=${JAMOS_REMOTE_NOSYNC:-0}
how=
[ "$nosync" = 1 ] && how=" (no copy)"
if [ $prepare = 1 ]; then
    echo "remote-test: on $host in ~/$dir$how: the build" >&2
else
    echo "remote-test: on $host in ~/$dir$how: $*" >&2
fi
mkdir -p build
if [ "$nosync" != 1 ]; then
    "$sshr" "${ssh_opts[@]}" "$host" "wsl --cd ~ mkdir -p $dir" < /dev/null
    # Several runs from one checkout copy it one at a time (lockf, the Mac's
    # flock): two rsyncs into one directory at once failed.
    lock=()
    command -v lockf > /dev/null && lock=(lockf -k build/.remote-sync.lock)
    ${lock[@]+"${lock[@]}"} rsync -az --delete --exclude .git --exclude /build --exclude /.claude --exclude test.bin \
        --exclude compile_commands.json --rsync-path="wsl --cd ~ rsync" \
        -e "$sshr ${ssh_opts[*]}" ./ "$host:$dir/" ||
        { echo "remote-test: copying to $host failed" >&2; exit 2; }
fi
[ $prepare = 1 ] && [ "${JAMOS_REMOTE_COPY_ONLY:-0}" = 1 ] && exit 0   # the copy alone (the runs build)
# What the PC runs: written to its bash on stdin. runid names this call; a
# repeat of the session (below) first stops what the lost one left there.
runid=${JAMOS_REMOTE_RUNID:-$$-$(date +%s)-$RANDOM}
remote_script() {
    echo "mkdir -p ~/work/.runs; find ~/work/.runs -type f -mtime +2 -delete 2> /dev/null"
    echo "old=\$(cat ~/work/.runs/$runid 2> /dev/null)"
    echo 'case $(ps -o comm= -p "${old:-0}" 2> /dev/null) in bash | python3) kill $old 2> /dev/null; sleep 1 ;; esac'
    echo "echo \$\$ > ~/work/.runs/$runid"
    echo "set -u; cd ~/$dir 2> /dev/null && [ -f Makefile ] ||"
    echo "    { echo 'remote-test: nothing copied to ~/$dir yet (JAMOS_REMOTE_NOSYNC=1 needs an earlier copy)' >&2; exit 2; }"
    echo 'export PATH=$HOME/cross/bin:$HOME/qemu-10/bin:$HOME/mtools/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8'
    env | grep -E '^(QEMU_[A-Z0-9_]*|KTESTS|JAMOS_[A-Z0-9_]*|PC_RUNS|[A-Z]+_CASES|SOAK_[A-Z0-9_]*|PORTS_TEST_[A-Z0-9_]*|WL_PORT_[A-Z0-9_]*|INIT_TEST_[A-Z0-9_]*)=' | grep -v '^JAMOS_REMOTE\(_ONLY\)\?=' |
        while IFS= read -r kv; do printf 'export %q\n' "$kv"; done
    # The build takes its share of the budget too (make -j as many as it costs);
    # a checkout's builds take turns (flock). A tree this checkout has built
    # already (the stamp: every file's name, size, time and link, and
    # the variables the Makefile reads) with its image there isn't built
    # again, and asks the budget for nothing.
    bj=${JAMOS_REMOTE_BUILD_CPUS:-8}
    echo 'img=build/jamos.img; [ "${KTESTS:-1}" = 0 ] && img=build/noktests/jamos.img
(
    flock 9 || exit 1
    stamp=build/.remote-built-k${KTESTS:-1}
    fp=$({ echo "KTESTS=${KTESTS-} JAMOS_VLAN=${JAMOS_VLAN-} JAMOS_DATA_FS=${JAMOS_DATA_FS-}"
           find . \( -path ./build -o -path ./build-remote.log -o -name __pycache__ \) -prune -o ! -type d -printf "%y %P %s %T@ %l\n" |
               LC_ALL=C sort; } | md5sum)
    fp=${fp%% *}
    if [ "${JAMOS_REMOTE_REBUILD:-0}" != 1 ] && [ -s "$img" ] && [ "$(cat "$stamp" 2> /dev/null)" = "$fp" ]; then
        echo "remote-test: built already (this tree): no build" >&2
        exit 0
    fi
    mkdir -p build && rm -f "$stamp"
    JAMOS_REMOTE_QUIET=1 JAMOS_REMOTE_LABEL=build python3 tools/remote-slots.py '"$bj"' 2048 \
        sh -c "{ make -s -j'"$bj"' && make -s -j'"$bj"' image; } > build-remote.log 2>&1" || exit 1
    echo "$fp" > "$stamp"
) 9> ~/work/.build-$(basename "$PWD").lock ||
    { tail -20 build-remote.log; echo "remote-test: the build failed"; exit 2; }'
    [ $prepare = 1 ] && echo 'exit 0'
    printf 'set -- '; printf '%q ' "$@"; echo
    # The command, when its CPUs and memory fit in the machine's budget (above).
    # A third word of the cost says how to run fewer parts at once (--shrink):
    # used only when a long claim (the Chromium build) leaves too little for all.
    local ccpus cmem cshrink
    read -r ccpus cmem cshrink <<< "$cost"
    echo "exec python3 tools/remote-slots.py ${cshrink:+--shrink $cshrink }$ccpus $cmem \"\$@\" < /dev/null"
}
[ "$script_only" = 1 ] && { remote_script "$@"; exit 0; }
# The session, with its stderr also kept in a file (through a fifo, so it is
# still live). If the connection is lost after the login while the command is
# still waiting for the budget (no "running with" line yet: a VPN drop in the
# hours of a big batch's queue), nothing has run: start the session again, up
# to JAMOS_REMOTE_REQUEUE (3) times. Once the command has started a lost
# connection is the run's failure (exit 255), never a second run.
rt_tmp=$(mktemp -d "${TMPDIR:-/tmp}/remote-test.XXXXXX") || exit 2
trap 'rm -rf "$rt_tmp"' EXIT
# Told to stop (pc-batch cancel, ^C, kill): end the run on the PC too. The ssh
# session ending is not enough: WSL can leave what it started running for
# hours, holding its CPUs. The PC's side is the pid file of this call (above):
# the governor (tools/remote-slots.py), which on SIGTERM takes the command's
# whole tree down. One short ssh, then exit 143.
sshpid=
cancel_remote() {
    trap - TERM INT HUP
    echo "remote-test: cancelled: ending the run on $host" >&2
    if [ -n "$sshpid" ]; then   # the session's ssh (and its ssh child), now
        pkill -TERM -P "$sshpid" 2> /dev/null
        kill -TERM "$sshpid" 2> /dev/null
    fi
    printf '%s\n' "p=\$(cat ~/work/.runs/$runid 2> /dev/null)" \
        'case $(ps -o comm= -p "${p:-0}" 2> /dev/null) in bash | python3) kill $p ;; esac' |
        "$sshr" "${ssh_opts[@]}" -o ServerAliveInterval=5 -o ServerAliveCountMax=3 "$host" "wsl -e bash -s" > /dev/null 2>&1
    exit 143
}
trap cancel_remote TERM INT HUP
attempt=0
while :; do
    attempt=$((attempt + 1))
    rm -f "$rt_tmp/err"
    mkfifo "$rt_tmp/err"
    tee "$rt_tmp/errlog" < "$rt_tmp/err" >&2 &
    teepid=$!
    # In the background and waited for, so that a signal is handled at once (a
    # trap waits for a foreground command to end).
    remote_script "$@" | "$sshr" "${ssh_main_opts[@]}" "$host" "wsl -e bash -s" 2> "$rt_tmp/err" &
    sshpid=$!
    wait "$sshpid"
    rc=$?
    sshpid=
    wait "$teepid" 2> /dev/null
    if [ $rc = 255 ] && [ $attempt -le "${JAMOS_REMOTE_REQUEUE:-3}" ] && grep -q '^ssh-retry: connection lost' "$rt_tmp/errlog" &&
        ! grep -q '^remote-test: running with' "$rt_tmp/errlog"; then
        pause=${JAMOS_REMOTE_REQUEUE_WAIT:-5}
        pause=$((pause + RANDOM % (pause * 2 + 1)))   # 5 to 15 s: the PC drops the lost session meanwhile
        echo "remote-test: the connection to $host was lost before the command started: again in $pause s (try $((attempt + 1)))" >&2
        sleep "$pause"
        continue
    fi
    break
done
trap - TERM INT HUP   # the run is over: nothing left on the PC to end
for a in "$@"; do
    case $a in
    build/?*)
        a=${a%/}
        mkdir -p "$a"
        rsync -az --exclude '*.img' --rsync-path="wsl --cd ~ rsync" -e "$sshr ${ssh_opts[*]}" \
            "$host:$dir/$a/" "$a/" 2> /dev/null ||
            echo "remote-test: nothing came back for $a" >&2 ;;
    esac
done
exit $rc
