# Sourced by tools/pc-batch.sh and tools/remote-test.sh (not run): which batches
# of this checkout still run. Every batch of a checkout tests ONE copy of it on
# the PC (work/<checkout dir>), so a second batch, or a single remote-test.sh
# run, that copies the checkout there changes the code under the first batch's
# remaining runs. Needs these variables: base (the batches' directory, relative
# to the checkout's top), top, sshr and ssh_opts (tools/ssh-retry.sh and its
# options), JAMOS_REMOTE (the PC).

# busy_batches <id to leave out> <1: ask the PC too>: the ids of this checkout's
# other batches still running: a driver alive here, or (a detached batch, its
# driver gone after the hand-off) the batch's own driver alive on the PC. The
# PC is asked when the second argument is 1, or when an earlier detached batch
# isn't collected yet.
busy_batches() {
    local skip=$1 ask=${2:-0} o ids=
    for o in $(ls "$base" 2> /dev/null); do
        [ "$o" = "$skip" ] && continue
        [ -f "$base/$o/handed" ] && [ ! -f "$base/$o/collected" ] && ask=1
        [ ! -f "$base/$o/done" ] && kill -0 "$(cat "$base/$o/driver.pid" 2> /dev/null)" 2> /dev/null && ids="$ids $o"
    done
    if [ "$ask" = 1 ]; then
        ids="$ids $(printf 'for o in ~/work/.batch/%s/*/; do [ -f "$o/started" ] && [ ! -f "$o/done" ] && ! flock -n "$o/lock" true && basename "$o"; done; exit 0\n' \
            "$(basename "$top")" | "$sshr" "${ssh_opts[@]}" "${JAMOS_REMOTE:-}" "wsl -e bash -s" 2> /dev/null)"
    fi
    echo $ids
}
