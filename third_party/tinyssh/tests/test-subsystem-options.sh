#!/bin/sh

LANG=C
export LANG
LC_ALL=C
export LC_ALL

exec 2>&1

check() {
    description=$1
    expected=$2
    shift 2

    "$@" </dev/null >/dev/null 2>/dev/null
    actual=$?
    if test "$actual" -ne "$expected"; then
        echo "$description: expected $expected, got $actual"
        exit 1
    fi
    echo "$description: $actual"
}

echo '--- tinysshd subsystem option validation'

check 'attached valid registration' 111 \
    ./tinysshd -xsftp=/usr/libexec/sftp-server missing-keydir
check 'separate valid registration' 111 \
    ./tinysshd -x sftp=/usr/libexec/sftp-server missing-keydir
check 'command containing equals sign' 111 \
    ./tinysshd -xsftp=/bin/program=argument missing-keydir

check 'missing separator' 100 ./tinysshd -xsftp missing-keydir
check 'missing separator in separate argument' 100 \
    ./tinysshd -x sftp missing-keydir
check 'missing name' 100 ./tinysshd -x=/bin/program missing-keydir
check 'missing command' 100 ./tinysshd -xsftp= missing-keydir
check 'empty separate registration' 100 ./tinysshd -x '' missing-keydir
check 'missing separate registration' 100 ./tinysshd -x

set --
i=0
while test "$i" -lt 64; do
    set -- "$@" "-xsubsystem$i=/bin/program"
    i=$((i + 1))
done
check 'maximum registrations' 111 ./tinysshd "$@" missing-keydir
set -- "$@" "-xsubsystem$i=/bin/program"
check 'too many registrations' 100 ./tinysshd "$@" missing-keydir
