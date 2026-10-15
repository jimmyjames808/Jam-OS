#!/bin/bash
# Start a command on the test PC that outlives this session: through the
# keeper (tools/pc-keeper.sh) when it runs, else detached as before (which
# WSL 2.4 ends with the session). Returns at once.
#
#   tools/pc-detach.sh <log> <command...>
#
# The command runs in the current directory with the current environment's
# PATH, its output appended to <log>.
set -u
[ $# -ge 2 ] || { echo "usage: pc-detach.sh <log> <command...>" >&2; exit 2; }
log=$1
shift
spool=$HOME/.jamos-spool
pid=$(cat "$spool/keeper.pid" 2> /dev/null)
if [ -n "$pid" ] && kill -0 "$pid" 2> /dev/null; then
    id=$(date +%Y%m%d-%H%M%S)-$$
    {
        printf 'cd %q || exit 1\n' "$PWD"
        printf 'export PATH=%q\n' "$PATH"
        printf 'exec >> %q 2>&1\n' "$log"
        printf 'exec'
        printf ' %q' "$@"
        printf '\n'
    } > "$spool/$id.tmp" && mv "$spool/$id.tmp" "$spool/$id.job"
    echo "pc-detach: job $id given to the keeper (log $log)"
else
    setsid "$@" < /dev/null >> "$log" 2>&1 &
    echo "pc-detach: no keeper running: started detached (WSL ends it with this session)"
fi
