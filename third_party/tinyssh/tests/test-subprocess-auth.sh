#!/bin/sh

LANG=C
export LANG
LC_ALL=C
export LC_ALL

exec 2>&1

echo '--- authorized_keys handling'
./_tinysshd-test-subprocess-auth 2>/dev/null
