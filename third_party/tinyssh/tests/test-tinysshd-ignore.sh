#!/bin/sh

LANG=C
export LANG

LC_ALL=C
export LC_ALL

exec 2>&1

rm -rf keydir-ignore
./tinysshd-makekey keydir-ignore

for mode in \
  nonstrict-before-ignore \
  nonstrict-before-debug \
  nonstrict-before-multiple \
  nonstrict-during-ignore \
  nonstrict-during-debug \
  strict-before-ignore \
  strict-before-debug \
  strict-during-ignore \
  strict-during-debug
do
  echo "--- tinysshd handles ${mode}"
  ./_tinysshd-test-ignore "${mode}" ./tinysshd keydir-ignore
  echo $?
  if test "${mode}" != strict-during-debug; then
    echo
  fi
done

rm -rf keydir-ignore
