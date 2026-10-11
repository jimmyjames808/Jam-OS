#!/bin/sh

LANG=C
export LANG
LC_ALL=C
export LC_ALL

exec 2>&1

echo '--- SSH packet sequence numbers and message order'
./_tinysshd-test-sequence 2>/dev/null
echo $?
