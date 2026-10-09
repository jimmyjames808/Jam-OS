#!/bin/bash
# Run a test command on a faster machine, if there is one: this checkout is
# copied to it (no .git, no build/), built there (`make image`, so the image
# is never stale) and the command run there, with KVM; the output
# directories named in its arguments (those under build/) come back, but
# stick images, and the command's exit status is
# this script's. Without JAMOS_REMOTE, or when it doesn't answer in 5 s (the
# PC is off, or booted into Jam OS), the command runs here as it would have.
#
#   tools/remote-test.sh tools/qemu-test.sh build/test init init
#   QEMU_SMP=8 tools/remote-test.sh tools/qemu-test.sh build/test kt8 ktest
#   tools/remote-test.sh tools/mixer-test.sh build/test/mixer
#
# JAMOS_REMOTE: an ssh host whose login runs Windows' OpenSSH, with WSL's
# Ubuntu behind `wsl` (docs/TESTING.md, "Another machine for the tests").
# The QEMU_*, KTESTS and JAMOS_* variables go with the command. Each
# checkout has a directory of its own there (~/work/<its name>), so several
# checkouts (agents' worktrees) test at once; one checkout's builds take
# turns (flock), its tests don't.
set -u
[ $# -gt 0 ] || { echo "usage: tools/remote-test.sh <command> [args...]"; exit 2; }
top=$(git rev-parse --show-toplevel 2>/dev/null) || top=$(pwd)
cd "$top" || exit 2
host=${JAMOS_REMOTE:-}
ssh_opts=(-o BatchMode=yes -o ConnectTimeout=5)
if [ -z "$host" ] || ! ssh "${ssh_opts[@]}" "$host" "wsl -e true" < /dev/null > /dev/null 2>&1; then
    [ -n "$host" ] && echo "remote-test: $host doesn't answer: running here" >&2
    exec "$@"
fi
name=$(basename "$top")
dir=work/$name
echo "remote-test: on $host in ~/$dir: $*" >&2
ssh "${ssh_opts[@]}" "$host" "wsl --cd ~ mkdir -p $dir" < /dev/null
rsync -az --delete --exclude .git --exclude /build --exclude /.claude --exclude test.bin \
    --exclude compile_commands.json --rsync-path="wsl --cd ~ rsync" \
    -e "ssh ${ssh_opts[*]}" ./ "$host:$dir/" ||
    { echo "remote-test: copying to $host failed" >&2; exit 2; }
{
    echo "set -u; cd ~/$dir || exit 2"
    echo 'export PATH=$HOME/cross/bin:$HOME/qemu-10/bin:/usr/bin:/bin LANG=C.UTF-8 LC_ALL=C.UTF-8'
    env | grep -E '^(QEMU_[A-Z0-9_]*|KTESTS|JAMOS_[A-Z0-9_]*)=' | grep -v '^JAMOS_REMOTE=' |
        while IFS= read -r kv; do printf 'export %q\n' "$kv"; done
    echo 'flock ~/work/.build-$(basename "$PWD").lock make -s -j16 image > build-remote.log 2>&1 ||
        { tail -20 build-remote.log; echo "remote-test: the build failed"; exit 2; }'
    printf '%q ' "$@"; echo
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
