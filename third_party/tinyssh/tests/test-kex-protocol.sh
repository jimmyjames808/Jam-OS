#!/bin/sh

LANG=C
export LANG
LC_ALL=C
export LC_ALL

exec 2>&1

echo '--- SSH KEXINIT negotiation and validation'
./_tinysshd-test-kex-protocol 2>/dev/null
echo $?
