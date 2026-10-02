#!/bin/sh
# make check: in the RTL8125 driver (drivers/rtl8125) only tx.c can make
# the chip transmit, only in full mode with a VLAN, and the listen-only
# probe has no path to it (docs/M9-PLAN.md, "Where the VLAN tag is
# enforced"). Grep-level rules over the driver's sources; notx.h's guard
# and gate enforce the same at run time (utest checks them).
#
#   1. Registers are written (drv_write*) in two files only: regs.c, where
#      every write is the line after a call of refused() (the guard that
#      refuses every transmit register), and tx.c, where every write is
#      the line after rtl_tx_allowed(...) (the gate).
#   2. No regs.c accessor (wr8/wr16/wr32/set8/clr8) names a transmit
#      register: TXDESC, TXSTART, TXCFG, TDFNR, or an offset 0x20-0x2f,
#      0x40-0x43, 0x57, 0x90-0x93 written as a number.
#   3. The transmitter enable bit (RTL_CMD_TXENB) is named only in notx.h
#      and tx.c, and elsewhere only in tests of it (`& RTL_CMD_TXENB`).
#   4. Every function tx.c gives other files starts with the gate (its
#      first statement calls gate(t, ...) or rtl_tx_allowed(t->mode,
#      t->vlan)); gate() itself starts with rtl_tx_allowed(t->mode,
#      t->vlan); and notx.h's rtl_tx_allowed is "full mode and a valid
#      VLAN".
#   5. The probe's own files (probe.c, census.c) call nothing of tx.c.
#   6. The mode and the VLAN are each set in one place: one assignment of
#      ->mode and one of ->vlan, both in main.c.
#
# Then it checks itself: tools/checknotx-tests/ breaks every part of every
# rule once, and each rule must catch exactly its offences there
# (CHECKNOTX_SHOW=1 lists them). Exit 1 naming every offence.

# rules <dir>: print each offence, one per line, prefixed by its rule.
rules()
{
    dir=$1
    for f in "$dir"/*.c "$dir"/*.h; do
        [ -f "$f" ] || continue
        base=$(basename "$f")
        case $base in
        regs.c) awk -v f="$f" '/drv_write[0-9]+\(/ && prev !~ /refused\(/ { print "1 " f ":" NR ": " $0 }
                               { prev = $0 }' "$f" ;;
        tx.c)   awk -v f="$f" '/drv_write[0-9]+\(/ && prev !~ /rtl_tx_allowed\(/ { print "1 " f ":" NR ": " $0 }
                               { prev = $0 }' "$f" ;;
        *)      grep -n -E 'drv_write[0-9]+\(' "$f" | sed "s|^|1 $f:|" ;;
        esac
        grep -n -E '\b(wr8|wr16|wr32|set8|clr8)\(' "$f" |
            grep -E 'TXDESC|TXSTART|TXCFG|TDFNR|\(t, *0x(2[0-9a-fA-F]|4[0-3]|57|9[0-3])\b' |
            sed "s|^|2 $f:|"
        if [ "$base" != notx.h ] && [ "$base" != tx.c ]; then
            grep -n 'TXENB' "$f" | grep -v -E '& *RTL_CMD_TXENB' | sed "s|^|3 $f:|"
        fi
        if [ "$base" = probe.c ] || [ "$base" = census.c ]; then
            grep -n -E '\btx_[a-z_]+\(' "$f" | sed "s|^|5 $f:|"
        fi
    done
    gates "$dir"
    once "$dir" mode
    once "$dir" vlan
}

# Rule 4 over <dir>/tx.c and <dir>/notx.h.
gates()
{
    tx=$1/tx.c
    if [ ! -f "$tx" ]; then
        echo "4 $tx: missing"
        return
    fi
    awk -v f="$tx" '
        # a definition other files can call: not static, a name and "(", no ";"
        /^[a-z_][a-z_0-9]* \**[a-z_][a-z_0-9]*\(.*\)$/ && $0 !~ /^static/ { want = NR + 2; name = $0 }
        /^[a-z_][a-z_0-9]* \**[a-z_][a-z_0-9]*\([^)]*$/ && $0 !~ /^static/ {
            print "4 " f ":" NR ": a definition on several lines: keep it on one, to be checked"
        }
        /^static bool gate\(/ { gatedef = NR + 2 }
        NR == want && $0 !~ /if \(!gate\(t, / && $0 !~ /if \(!rtl_tx_allowed\(t->mode, t->vlan\)\)/ {
            print "4 " f ":" NR ": " name " does not start with the gate"
        }
        NR == gatedef && $0 !~ /if \(rtl_tx_allowed\(t->mode, t->vlan\)\)/ {
            print "4 " f ":" NR ": gate() does not start with rtl_tx_allowed(t->mode, t->vlan)"
        }
        END { if (!gatedef) print "4 " f ": no static bool gate(...)" }' "$tx"
    grep -q 'return mode == RTL_MODE_FULL && netframe_vlan_ok(vlan);' "$1/notx.h" 2>/dev/null ||
        echo "4 $1/notx.h: rtl_tx_allowed is not \"full mode and a valid VLAN\""
}

# Rule 6: <field> is assigned (->field = ...) exactly once, in main.c.
once()
{
    hits=$(grep -n -E -- "->$2 *=[^=]" "$1"/*.c "$1"/*.h 2>/dev/null)
    if [ -z "$hits" ]; then
        echo "6 ->$2 is never set in $1/main.c"
        return
    fi
    n=$(printf '%s\n' "$hits" | grep -c .)
    if [ "$n" != 1 ] || ! printf '%s\n' "$hits" | grep -q '/main\.c:'; then
        printf '%s\n' "$hits" | sed "s|^|6 ->$2 set $n time(s), not once in main.c: |"
    fi
}

here=$(dirname "$0")
found=$(rules "$here/../drivers/rtl8125")
if [ -n "$found" ]; then
    echo "checknotx: drivers/rtl8125 could transmit outside tx.c's gate (rule, file:line):"
    echo "$found"
    exit 1
fi
# The self-check: every rule must fire on the bad examples.
bad=$(rules "$here/checknotx-tests")
[ -z "${CHECKNOTX_SHOW:-}" ] || echo "$bad"   # CHECKNOTX_SHOW=1: what the self-check caught
# Each rule's offences there, counted (every part of a rule has one).
for want in 1:2 2:3 3:1 4:4 5:1 6:2; do
    r=${want%:*}
    n=$(echo "$bad" | grep -c "^$r ")
    if [ "$n" != "${want#*:}" ]; then
        echo "checknotx: rule $r caught $n offence(s) in tools/checknotx-tests/, not ${want#*:}"
        exit 1
    fi
done
echo "checknotx: drivers/rtl8125 transmits only through tx.c's gate (6 rules, each self-checked)"
