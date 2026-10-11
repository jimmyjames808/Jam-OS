#!/bin/sh

LANG=C
export LANG
LC_ALL=C
export LC_ALL

exec 2>&1

echo '--- SSH rekey negotiation during an active session'
./_tinysshd-test-rekey-protocol 2>/dev/null
echo $?
