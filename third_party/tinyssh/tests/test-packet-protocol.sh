#!/bin/sh

LANG=C
export LANG
LC_ALL=C
export LC_ALL

exec 2>&1

echo '--- SSH packet parser and framing'
./_tinysshd-test-packet 2>/dev/null
echo $?
