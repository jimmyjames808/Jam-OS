#!/bin/sh
# Check the fonts package's fontconfig configuration (G2's GP2, ports/fonts-jamos)
# with the build host's own fc-match: Jam OS has no fontconfig port, so this
# is where fonts.conf is run. The staged package (build/ports/fonts-jamos/stage,
# from `python3 tools/ports.py build fonts-jamos`) is the font directory; the
# configuration is the staged one with /pkg replaced by the stage's path
# and the cache in a scratch directory.
#
#   tools/fonts-check.sh [stagedir]
#
# Each generic family and each name pages ask for must resolve as
# docs/G2-PLAN.md section 4 says. Exit 0 if all do.
set -u
stage=${1:-build/ports/fonts-jamos/stage}
conf=$stage/share/fonts/fonts.conf
[ -f "$conf" ] || { echo "fonts-check: $conf: no such file (build fonts-jamos first)" >&2; exit 2; }
command -v fc-match >/dev/null 2>&1 || { echo "fonts-check: no fc-match on this machine" >&2; exit 2; }
stage=$(cd "$stage" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/fonts-check.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT
sed "s|/pkg/|$stage/|g" "$conf" > "$tmp/fonts.conf"
FONTCONFIG_FILE=$tmp/fonts.conf
XDG_CACHE_HOME=$tmp/cache
export FONTCONFIG_FILE XDG_CACHE_HOME

fail=0
check() {
    got=$(fc-match -f '%{family[0]}|%{style[0]}|%{file}' "$1" 2>&1)
    fam=${got%%|*}
    file=${got##*|}
    case $file in
    "$stage"/share/fonts/jamos/*) ;;
    *) echo "FAIL  $1: $got (not from the package)"; fail=1; return ;;
    esac
    if [ "$fam" = "$2" ]; then
        echo "ok    $1 -> $fam (${file#$stage/share/fonts/jamos/})"
    else
        echo "FAIL  $1 -> $fam, want $2"
        fail=1
    fi
}

# fc-match reads its argument as a pattern, where "-" starts a size: the
# family sans-serif is written sans\-serif.
check 'sans\-serif' "Noto Sans"
check serif "Noto Serif"
check monospace "JetBrains Mono"
check Arial "Liberation Sans"
check Helvetica "Liberation Sans"
check "Times New Roman" "Liberation Serif"
check Times "Liberation Serif"
check "Courier New" "Liberation Mono"
check Courier "Liberation Mono"
check emoji "Noto Color Emoji"
check 'sans\-serif:bold' "Noto Sans"
check "monospace:italic" "JetBrains Mono"
check Inter "Inter"
check "Noto Sans Mono" "Noto Sans Mono"
# An emoji asked of the generic family comes from the emoji font.
em=$(fc-match -f '%{family[0]}' 'sans\-serif:charset=1f600')
if [ "$em" = "Noto Color Emoji" ]; then
    echo "ok    sans-serif with U+1F600 -> $em"
else
    echo "FAIL  sans-serif with U+1F600 -> $em, want Noto Color Emoji"
    fail=1
fi
if [ $fail = 0 ]; then
    echo "fonts-check: PASS"
else
    echo "fonts-check: FAIL"
fi
exit $fail
