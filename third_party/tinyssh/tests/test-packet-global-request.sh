#!/bin/sh

LANG=C
export LANG

LC_ALL=C
export LC_ALL

echo '--- packet global request'
./_tinysshd-test-global-request 2>/dev/null
echo $?
