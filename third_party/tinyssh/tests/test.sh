#!/bin/sh

cd "$(dirname "$0")" || exit 111

status=0
if test "$#" -eq 0; then
    set -- test-*.sh
fi

for script do
    name=${script%.sh}
    expfile=${name}.exp
    outfile=${name}.out

    if test ! -f "$expfile"; then
        echo "$script FAILED: missing $expfile"
        status=1
        continue
    fi
    if ! sh "$script" >"$outfile"; then
        echo "$script FAILED: script returned non-zero"
        status=1
        continue
    fi
    if cmp "$expfile" "$outfile"; then
        echo "$script OK"
        continue
    fi

    echo "$script FAILED"
    if command -v diff >/dev/null 2>&1; then
        diff -u "$expfile" "$outfile"
    else
        cat "$outfile"
    fi
    status=1
done

exit "$status"
