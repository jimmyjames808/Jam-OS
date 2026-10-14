#!/bin/bash
# Run a test command on a faster machine, if there is one: this checkout is
# copied to it (no .git, no build/), built there (`make` and `make image`, so the image
# is never stale) and the command run there, with KVM; the output
# directories named in its arguments (those under build/) come back, but
# stick images, and the command's exit status is
# this script's. Without JAMOS_REMOTE, or when it doesn't answer in 5 s (the
# PC is off, or booted into Jam OS), the command runs here as it would have,
# unless JAMOS_REMOTE_ONLY=1: then it isn't run (exit 3).
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
#     one for each case, ports-test PORTS_TEST_JOBS, wl-port-test WL_PORT_JOBS) the total of them all;
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
# file's name, size and time, and KTESTS and JAMOS_VLAN), and when the tree
# and the stamp agree and the image is there, the build is skipped without
# asking the budget for its CPUs. JAMOS_REMOTE_REBUILD=1 builds anyway.
#
# Several commands from one copy (tools/pc-batch.sh does this):
#   tools/remote-test.sh --prepare     copy and build only, run nothing
#   JAMOS_REMOTE_NOSYNC=1 tools/remote-test.sh <command>
#                                      don't copy: use what the last copy left
set -u
[ $# -gt 0 ] || { echo "usage: tools/remote-test.sh <command> [args...] | --prepare"; exit 2; }
prepare=0
if [ "$1" = --prepare ]; then
    [ $# -eq 1 ] || { echo "usage: tools/remote-test.sh --prepare" >&2; exit 2; }
    prepare=1
fi
top=$(git rev-parse --show-toplevel 2>/dev/null) || top=$(pwd)
cd "$top" || exit 2

# What the command takes of the machine: "<virtual CPUs> <MB of memory>".
# JAMOS_REMOTE_COST_ONLY=1 prints it and stops (tools/remote-slots-test.sh).
cost_of() {
    local smp=${QEMU_SMP:-} mem=${QEMU_MEM:-} jobs=${PORTS_TEST_JOBS:-} wljobs=${WL_PORT_JOBS:-} i=0 a script qemus=1 after
    local args=("$@")
    if [ "${args[0]:-}" = env ]; then
        i=1
        while [[ ${args[i]:-} == [A-Za-z_]*=* ]]; do
            a=${args[i]}
            case $a in QEMU_SMP=*) smp=${a#*=} ;; QEMU_MEM=*) mem=${a#*=} ;; PORTS_TEST_JOBS=*) jobs=${a#*=} ;; WL_PORT_JOBS=*) wljobs=${a#*=} ;; esac
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
    ports-test.sh)
        [[ ${jobs:-} =~ ^[0-9]+$ ]] || jobs=3
        qemus=$jobs
        [ "$after" -eq 0 ] || [ "$after" -ge "$jobs" ] || qemus=$after ;;
    wl-port-test.sh)   # WL_PORT_JOBS (3) groups at a time, one QEMU each
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
    echo "$((qemus * smp)) $((qemus * mem))"
}
if [ "${JAMOS_REMOTE_COST_ONLY:-0}" = 1 ]; then
    cost_of "$@"
    exit 0
fi
cost=$(cost_of "$@")
host=${JAMOS_REMOTE:-}
ssh_opts=(-o BatchMode=yes -o ConnectTimeout=5)
if [ -z "$host" ] || ! ssh "${ssh_opts[@]}" "$host" "wsl -e true" < /dev/null > /dev/null 2>&1; then
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
    ssh "${ssh_opts[@]}" "$host" "wsl --cd ~ mkdir -p $dir" < /dev/null
    # Several runs from one checkout copy it one at a time (lockf, the Mac's
    # flock): two rsyncs into one directory at once failed.
    lock=()
    command -v lockf > /dev/null && lock=(lockf -k build/.remote-sync.lock)
    ${lock[@]+"${lock[@]}"} rsync -az --delete --exclude .git --exclude /build --exclude /.claude --exclude test.bin \
        --exclude compile_commands.json --rsync-path="wsl --cd ~ rsync" \
        -e "ssh ${ssh_opts[*]}" ./ "$host:$dir/" ||
        { echo "remote-test: copying to $host failed" >&2; exit 2; }
fi
{
    echo "set -u; cd ~/$dir 2> /dev/null && [ -f Makefile ] ||"
    echo "    { echo 'remote-test: nothing copied to ~/$dir yet (JAMOS_REMOTE_NOSYNC=1 needs an earlier copy)' >&2; exit 2; }"
    echo 'export PATH=$HOME/cross/bin:$HOME/qemu-10/bin:$HOME/mtools/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8'
    env | grep -E '^(QEMU_[A-Z0-9_]*|KTESTS|JAMOS_[A-Z0-9_]*|PC_RUNS|[A-Z]+_CASES|SOAK_[A-Z0-9_]*|PORTS_TEST_[A-Z0-9_]*|WL_PORT_[A-Z0-9_]*)=' | grep -v '^JAMOS_REMOTE\(_ONLY\)\?=' |
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
    fp=$({ echo "KTESTS=${KTESTS-} JAMOS_VLAN=${JAMOS_VLAN-}"
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
    echo "exec python3 tools/remote-slots.py $cost \"\$@\" < /dev/null"
} | ssh "${ssh_opts[@]}" "$host" "wsl -e bash -s"
rc=$?
for a in "$@"; do
    case $a in
    build/?*)
        a=${a%/}
        mkdir -p "$a"
        rsync -az --exclude '*.img' --rsync-path="wsl --cd ~ rsync" -e "ssh ${ssh_opts[*]}" \
            "$host:$dir/$a/" "$a/" 2> /dev/null ||
            echo "remote-test: nothing came back for $a" >&2 ;;
    esac
done
exit $rc
