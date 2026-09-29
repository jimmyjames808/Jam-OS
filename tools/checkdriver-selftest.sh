#!/bin/sh
# `make check`: prove the driver build check works. Each file in
# tools/checkdriver-tests/ is compiled exactly like a driver's kernel build
# (the Makefile passes CFLAGS = DRV_KCFLAGS: -nostdinc, only the driver
# include directory) and then run through tools/checkdriver.py. ok_*.c must
# get through both; every other file must be stopped by one of them.
# Environment: CC, NM, CFLAGS, SURFACE (driver.h abi.h status.h), OUT.
set -u
: "${CC:?}" "${NM:?}" "${CFLAGS:?}" "${SURFACE:?}" "${OUT:?}"
mkdir -p "$OUT"
fail=0
for f in tools/checkdriver-tests/*.c; do
    b=$(basename "$f" .c)
    o="$OUT/$b.o" log="$OUT/$b.log"
    # shellcheck disable=SC2086 # CFLAGS and SURFACE are word lists
    if $CC $CFLAGS -c "$f" -o "$o" >"$log" 2>&1 &&
       python3 tools/checkdriver.py "$NM" "$b" "$o" -- $SURFACE >>"$log" 2>&1; then
        got=accepted
    else
        got=rejected
    fi
    case "$b" in
    ok_*) want=accepted ;;
    *)    want=rejected ;;
    esac
    if [ "$got" = "$want" ]; then
        echo "checkdriver-selftest: $b: $got, as it should be ($(grep -m1 -E 'error:|: uses|: defines' "$log" | sed 's/^ *//' | cut -c1-110))"
    else
        echo "checkdriver-selftest: $b: $got, but it must be $want:"
        sed 's/^/    /' "$log"
        fail=1
    fi
done
[ $fail = 0 ] && echo "checkdriver-selftest: all passed"
exit $fail
