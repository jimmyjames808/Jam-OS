#!/bin/sh

LANG=C
export LANG
LC_ALL=C
export LC_ALL

exec 2>&1

echo '--- SSH session channel open and requests'
./_tinysshd-test-channel-protocol 2>/dev/null
echo $?
