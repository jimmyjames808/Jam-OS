#!/bin/bash
# ssh that does not flood the PC's sshd and tries again when a connection
# fails before it is logged in: the one way the tools reach the test PC
# (tools/remote-test.sh uses it for every ssh and, as rsync's -e command, for
# every rsync).
#
#   tools/ssh-retry.sh <the arguments of ssh>
#
# Why: the PC's sshd turns new connections away at random while 10 or more are
# still logging in (its MaxStartups, more of them as there are more) and under
# load answers too late ("Connection timed out during banner exchange"). A
# batch that starts 16 runs at once, or 160, opened that many logins together;
# some got "kex_exchange_identification: read: Connection reset by peer" (exit
# 255 within 2 s) and the run was lost.
#
# 1. Few logins at once. Before a try this script takes one of
#    SSH_RETRY_LOGINS (5) login slots, machine-wide on this Mac (directories
#    under SSH_RETRY_LOCKDIR, /tmp/jamos-ssh-logins-<uid>: every agent's
#    runs share them), and frees it as soon as ssh is logged in, so a session
#    that lasts hours holds none. A slot whose holder died, or that is 2
#    minutes old, is taken over. After SSH_RETRY_LOGIN_WAIT (180) s of
#    waiting for a slot it goes on without one.
# 2. Tries again, but only when ALL of these hold, so a command that has
#    started is never run twice:
#    - ssh exited 255 (its own failure);
#    - the login never completed: ssh runs `touch <marker>` (LocalCommand) as
#      soon as it is logged in, before the remote command starts, and there is
#      no marker;
#    - the message is a connection that was cut or never made: a reset, a
#      close or a timeout before the banner, kex_exchange_identification,
#      "Connection refused", "Connection timed out", "No route to host" ...
#      "Permission denied", a changed host key and the like are not.
#    The stdin of the first try is the stdin of the next (ssh reads it only
#    once logged in), so a script piped into ssh, and rsync's protocol, are
#    not touched.
# 3. The waits are 1, 2, 4, 8, 8 s, each with up to half as much again added at
#    random, so the connections that were turned away together do not come back
#    together. A reset or close is tried 6 times; a PC that is off or
#    unreachable ("refused", "timed out", "no route") 3, so a machine that is
#    really down is found out in about 20 s.
#
# ssh's own messages go to a log file (-E), so the remote command's stderr
# stays live on stderr; they are printed after the last try, with a line
# "ssh-retry: ..." for every try repeated, and one of these at the end:
#   ssh-retry: not logged in: ...        nothing ran on the PC
#   ssh-retry: connection lost after login: ...   exit 255 with the command
#                                        started (or waiting there): the caller decides
#
#   SSH_RETRY_TRIES=6         tries for a reset or a close (1 = never repeat)
#   SSH_RETRY_DOWN_TRIES=3    tries when nothing answers
#   SSH_RETRY_DELAY_MS=1000   the first wait
#   SSH_RETRY_LOGINS=5        logins at once on this Mac (0 = no limit)
#   SSH_RETRY_SSH=ssh         the program to run (tools/ssh-retry-test.sh: a stand-in)
#
# Not multiplexing (ControlMaster): the PC's sshd allows 10 sessions on one
# connection (MaxSessions), a batch holds many long ones, and a master that
# outlives a PC reboot leaves stale sockets.
set -u
ssh_bin=${SSH_RETRY_SSH:-ssh}
tries=${SSH_RETRY_TRIES:-6}
down_tries=${SSH_RETRY_DOWN_TRIES:-3}
base_ms=${SSH_RETRY_DELAY_MS:-1000}
logins=${SSH_RETRY_LOGINS:-5}
login_wait=${SSH_RETRY_LOGIN_WAIT:-180}
lockroot=${SSH_RETRY_LOCKDIR:-/tmp/jamos-ssh-logins-$(id -u)}

tmpdir=${TMPDIR:-/tmp}
case $tmpdir in *[!A-Za-z0-9._/-]*) tmpdir=/tmp ;; esac   # the marker's path goes into an ssh option
case $lockroot in *[!A-Za-z0-9._/-]*) logins=0 ;; esac    # so does the slot's
tmp=$(mktemp -d "${tmpdir%/}/ssh-retry.XXXXXX") || exec "$ssh_bin" "$@"
slot=

free_slot() {   # ours only: a slot taken over from us belongs to someone else
    if [ -n "$slot" ] && [ "$(cat "$slot/pid" 2> /dev/null)" = $$ ]; then
        rm -rf "$slot"
    fi
    slot=
}
trap 'free_slot; rm -rf "$tmp"' EXIT
trap 'exit 143' HUP INT TERM

jitter() {   # jitter <ms>: wait_s=that long plus up to half as much again, in seconds
    local d=$(($1 + RANDOM % ($1 / 2 + 1)))
    printf -v wait_s '%d.%03d' $((d / 1000)) $((d % 1000))
}

take_slot() {
    local i s p waited=0
    slot=
    [ "$logins" -ge 1 ] 2> /dev/null || return 0
    mkdir -p "$lockroot" 2> /dev/null || return 0
    while [ $waited -lt $((login_wait * 5)) ]; do
        i=0
        while [ $i -lt "$logins" ]; do
            i=$((i + 1))
            s=$lockroot/slot$i
            if mkdir "$s" 2> /dev/null; then
                echo $$ > "$s/pid"
                slot=$s
                return 0
            fi
            p=$(cat "$s/pid" 2> /dev/null)
            if { [ -n "$p" ] && ! kill -0 "$p" 2> /dev/null; } || [ -n "$(find "$s" -maxdepth 0 -mmin +1 2> /dev/null)" ]; then
                rm -rf "$s"   # its holder is gone, or it has been logging in for 2 minutes
            fi
        done
        jitter 200
        sleep "$wait_s"
        waited=$((waited + 1))
    done
    echo "ssh-retry: no login slot free for $login_wait s: going on without one" >&2
}

# class <log>: reset (cut during the login), down (nothing there) or none.
class() {
    if grep -qiE 'kex_exchange_identification|ssh_exchange_identification|connection reset|connection closed by|banner exchange|broken pipe|connection corrupted' "$1"; then
        echo reset
    elif grep -qiE 'connection refused|timed out|no route to host|network is unreachable|host is down|could not resolve|name resolution' "$1"; then
        echo down
    else
        echo none
    fi
}

n=0
while :; do
    n=$((n + 1))
    rm -f "$tmp/up"
    : > "$tmp/log"
    take_slot
    lc="touch $tmp/up"
    [ -z "$slot" ] || lc="$lc; rm -rf $slot"   # logged in: the slot is free for the next
    "$ssh_bin" -E "$tmp/log" -o PermitLocalCommand=yes -o "LocalCommand=$lc" "$@"
    rc=$?
    free_slot
    if [ $rc != 255 ] || [ -e "$tmp/up" ]; then
        [ $n -gt 1 ] && echo "ssh-retry: connected on try $n" >&2
        if [ $rc = 255 ] && grep -qv '^$' "$tmp/log" && ! grep -q 'Killed by signal' "$tmp/log"; then
            echo "ssh-retry: connection lost after login: $(grep -v '^$' "$tmp/log" | head -1 | cut -c1-120)" >&2
        fi
        break
    fi
    kind=$(class "$tmp/log")
    max=0
    case $kind in reset) max=$tries ;; down) max=$down_tries ;; esac
    if [ $n -ge "$max" ]; then
        if [ "$max" = 0 ]; then
            echo "ssh-retry: not logged in: this failure is not tried again" >&2
        else
            echo "ssh-retry: not logged in: gave up after $n tr$([ $n = 1 ] && echo y || echo ies)" >&2
        fi
        break
    fi
    d=$((base_ms << (n - 1)))
    [ $d -le $((base_ms * 8)) ] || d=$((base_ms * 8))
    jitter "$d"
    echo "ssh-retry: $(grep -v '^$' "$tmp/log" | head -1 | cut -c1-120): try $((n + 1)) of $max in $wait_s s" >&2
    sleep "$wait_s"
done
cat "$tmp/log" >&2
exit $rc
