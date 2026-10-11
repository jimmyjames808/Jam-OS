#!/bin/sh

LANG=C
export LANG
LC_ALL=C
export LC_ALL

exec 2>&1

echo '--- SSH channel flow control, EOF and close'
./_tinysshd-test-channel-flow 2>/dev/null
echo $?
