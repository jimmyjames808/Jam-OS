#!/bin/sh
# make check: the RTL8125 listen-only probe (drivers/rtl8125) has no way
# to transmit (docs/M9-PLAN.md, stage 0). Grep-level rules over its
# sources; drivers/rtl8125/notx.h's guard enforces the same at run time.
#
#   1. Only regs.c touches registers (drv_write*), and there every write is
#      the line after a call of refused() (the guard).
#   2. No write accessor (wr8/wr16/wr32/set8/clr8) names a transmit
#      register: TXDESC, TXSTART, TXCFG, or an offset 0x20-0x2f, 0x40-0x43,
#      0x90-0x93 written as a number.
#   3. The transmitter enable bit (RTL_CMD_TXENB) is named only in notx.h
#      and in tests of it (`& RTL_CMD_TXENB`), never or-ed into a value.
#
# Then it checks itself: tools/checknotx-tests/bad.c breaks every rule
# once, and each rule must catch it. Exit 1 naming every offence.

# rules <dir>: print each offence, one per line, prefixed by its rule.
rules()
{
    dir=$1
    for f in "$dir"/*.c "$dir"/*.h; do
        [ -f "$f" ] || continue
        base=$(basename "$f")
        if [ "$base" != regs.c ]; then
            grep -n -E 'drv_write[0-9]+\(' "$f" | sed "s|^|1 $f:|"
        else
            awk -v f="$f" '/drv_write[0-9]+\(/ && prev !~ /refused\(/ { print "1 " f ":" NR ": " $0 }
                           { prev = $0 }' "$f"
        fi
        grep -n -E '\b(wr8|wr16|wr32|set8|clr8)\(' "$f" |
            grep -E 'TXDESC|TXSTART|TXCFG|\(t, *0x(2[0-9a-fA-F]|4[0-3]|9[0-3])\b' |
            sed "s|^|2 $f:|"
        if [ "$base" != notx.h ]; then
            grep -n 'TXENB' "$f" | grep -v -E '& *RTL_CMD_TXENB' | sed "s|^|3 $f:|"
        fi
    done
}

here=$(dirname "$0")
found=$(rules "$here/../drivers/rtl8125")
if [ -n "$found" ]; then
    echo "checknotx: drivers/rtl8125 could transmit (rule, file:line):"
    echo "$found"
    exit 1
fi
# The self-check: every rule must fire on the bad example.
bad=$(rules "$here/checknotx-tests")
for r in 1 2 3; do
    if ! echo "$bad" | grep -q "^$r "; then
        echo "checknotx: rule $r missed its offence in tools/checknotx-tests/bad.c"
        exit 1
    fi
done
echo "checknotx: drivers/rtl8125 writes no transmit register (3 rules, each self-checked)"
