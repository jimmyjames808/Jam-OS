#!/bin/sh

LANG=C
export LANG
LC_ALL=C
export LC_ALL

exec 2>&1

echo '--- SSH user authentication protocol'
./_tinysshd-test-auth-protocol 2>/dev/null
echo $?
