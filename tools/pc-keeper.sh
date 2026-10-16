#!/bin/bash
# The keeper on the test PC (WSL): runs long jobs that must outlive the ssh
# session that started them. WSL 2.4 ends every process a session started,
# setsid or not, when that session's wsl.exe ends; a job started by this
# loop belongs to the keeper's session instead, which the Windows task "Jam
# OS WSL keep-alive" opens at startup (as the owner, with no login) and
# never closes.
#
#   install:  tools/remote-test.sh tools/pc-keeper.sh --install
#   jobs:     tools/pc-detach.sh <log> <command...>   (writes a job file;
#             tools/cr/build.sh start and tools/pc-batch.sh start --detach use it)
#
# A job is a file ~/.jamos-spool/<id>.job: a bash script, run once with its
# output in the log it names. The keeper renames it <id>.run when it starts
# it, so a job never runs twice.
set -u
spool=$HOME/.jamos-spool

if [ "${1:-}" = --install ]; then
    mkdir -p "$HOME/.local/bin" "$spool"
    install -m 755 "$(dirname "$0")/pc-keeper.sh" "$HOME/.local/bin/jamos-keeper"
    install -m 755 "$(dirname "$0")/pc-detach.sh" "$HOME/.local/bin/jamos-detach"
    echo "pc-keeper: installed ~/.local/bin/jamos-keeper and jamos-detach"
    exit 0
fi

mkdir -p "$spool"
echo $$ > "$spool/keeper.pid"
while :; do
    for f in "$spool"/*.job; do
        [ -e "$f" ] || continue
        r=${f%.job}.run
        mv "$f" "$r" 2> /dev/null || continue
        setsid bash "$r" < /dev/null > /dev/null 2>&1 &
    done
    sleep 1
done
