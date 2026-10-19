#!/bin/sh
# Check the fonts packages' fontconfig configuration (G2's GP2, ports/fonts-jamos,
# and the FONTS track's ports/fonts-noto-extra) with the build host's own
# fc-match: Jam OS has no fontconfig port, so this is where fonts.conf is run.
# The staged packages (build/ports/<port>/stage, from `python3 tools/ports.py
# build fonts-jamos fonts-noto-extra`) are merged into one /pkg, the way init's
# view of /data/pkg holds them; the configuration is the staged
# share/fonts/fonts.conf (which includes fonts-noto-extra's) with /pkg replaced
# by that directory and the cache in a scratch directory.
#
#   tools/fonts-check.sh [stagedir...]
#
# Each generic family and each name pages ask for must resolve as
# docs/G2-PLAN.md section 4 says, each script's characters must find its Noto
# face, and each CJK language its face. Exit 0 if all do; a missing
# fonts-noto-extra stage skips its checks (fonts-jamos alone is a valid install).
set -u
if [ $# -eq 0 ]; then
    set -- build/ports/fonts-jamos/stage build/ports/fonts-noto-extra/stage
fi
command -v fc-match >/dev/null 2>&1 || { echo "fonts-check: no fc-match on this machine" >&2; exit 2; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/fonts-check.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT
tmp=$(cd "$tmp" && pwd -P)   # fc-match prints real paths
mkdir -p "$tmp/pkg"
extra=0
for d in "$@"; do
    [ -d "$d/share/fonts" ] || { echo "fonts-check: $d: no share/fonts (build the port first)" >&2; exit 2; }
    cp -R "$d/." "$tmp/pkg/"
done
[ -d "$tmp/pkg/share/fonts/noto-extra" ] && extra=1
pkg=$tmp/pkg
conf=$pkg/share/fonts/fonts.conf
[ -f "$conf" ] || { echo "fonts-check: no share/fonts/fonts.conf in the stages (fonts-jamos)" >&2; exit 2; }
# the packages' /pkg is this directory; the fallback cache is a scratch one
find "$pkg/share/fonts" -name fonts.conf | while read -r f; do
    sed -e "s|/pkg/|$pkg/|g" -e "s|/data/.cache/fontconfig|$tmp/cache2|g" -e "s|/data/.config|$tmp/none|g" "$f" > "$f.new"
    mv "$f.new" "$f"
done
FONTCONFIG_FILE=$conf
XDG_CACHE_HOME=$tmp/cache
export FONTCONFIG_FILE XDG_CACHE_HOME

fail=0
# check <pattern> <family> [style]: the best match of the pattern is from the
# packages, in that family (and that style).
check() {
    got=$(fc-match -f '%{family[0]}|%{style[0]}|%{file}' "$1" 2>&1)
    fam=${got%%|*}
    rest=${got#*|}
    style=${rest%%|*}
    file=${got##*|}
    case $file in
    "$pkg"/share/fonts/*) ;;
    *) echo "FAIL  $1: $got (not from the packages)"; fail=1; return ;;
    esac
    if [ "$fam" = "$2" ] && { [ -z "${3:-}" ] || [ "$style" = "$3" ]; }; then
        echo "ok    $1 -> $fam $style (${file#$pkg/share/fonts/})"
    else
        echo "FAIL  $1 -> $fam $style, want $2 ${3:-}"
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
# system-ui is the sans-serif one
check 'system\-ui' "Noto Sans"
# an emoji asked of the generic family comes from the emoji font.
em=$(fc-match -f '%{family[0]}' 'sans\-serif:charset=1f600')
if [ "$em" = "Noto Color Emoji" ]; then
    echo "ok    sans-serif with U+1F600 -> $em"
else
    echo "FAIL  sans-serif with U+1F600 -> $em, want Noto Color Emoji"
    fail=1
fi
# Latin, Greek and Cyrillic stay with Noto Sans (and Cyrillic in monospace with
# JetBrains Mono): the extras never take them over.
check 'sans\-serif:charset=3b1' "Noto Sans"
check 'sans\-serif:charset=430' "Noto Sans"
check 'monospace:charset=430' "JetBrains Mono"

if [ $extra = 1 ]; then
    # one character of each script, asked of the generic families
    script() {   # <codepoint> <family> <what>
        for gen in 'sans\-serif' serif monospace; do
            check "$gen:charset=$1" "$2"
        done
    }
    script 627 "Noto Sans Arabic"
    script 905 "Noto Sans Devanagari"
    script 985 "Noto Sans Bengali"
    script b85 "Noto Sans Tamil"
    script c05 "Noto Sans Telugu"
    script a85 "Noto Sans Gujarati"
    script a05 "Noto Sans Gurmukhi"
    script c85 "Noto Sans Kannada"
    script d05 "Noto Sans Malayalam"
    script e01 "Noto Sans Thai"
    script 10d0 "Noto Sans Georgian"
    script 531 "Noto Sans Armenian"
    script 1200 "Noto Sans Ethiopic"
    script 1780 "Noto Sans Khmer"
    script e81 "Noto Sans Lao"
    script 1000 "Noto Sans Myanmar"
    script d85 "Noto Sans Sinhala"
    script b05 "Noto Sans Oriya"
    script 1d400 "Noto Sans Math"
    script 1f700 "Noto Sans Symbols"
    script 1fb00 "Noto Sans Symbols 2"
    # Hebrew is in Liberation too (letters, points and marks), and fonts-jamos's
    # fonts come first: Noto Sans Hebrew is the fallback for what Liberation lacks
    check 'sans\-serif:charset=5d0' "Liberation Sans"
    check 'serif:charset=5d0' "Liberation Serif"
    check 'monospace:charset=5d0' "Liberation Mono"
    # a snowman has a plain glyph in the Symbols fonts: it is not drawn as an
    # emoji unless asked for as one (the emoji lang below)
    script 2603 "Noto Sans Symbols 2"
    # the bold faces
    check 'sans\-serif:bold:charset=627' "Noto Sans Arabic" Bold
    check 'sans\-serif:bold:charset=e01' "Noto Sans Thai" Bold

    # CJK: the face for each language, for a Han character (U+4E00), for the
    # kana and Hangul only some faces are named for; the page's lang reaches
    # fontconfig as it is written (Chromium passes the lang attribute).
    # Without a language the Simplified Chinese face.
    check 'sans\-serif:charset=4e00' "Noto Sans CJK SC"
    for l in ja ja-JP; do check "sans\\-serif:lang=$l:charset=4e00" "Noto Sans CJK JP"; done
    for l in ko ko-KR; do check "sans\\-serif:lang=$l:charset=4e00" "Noto Sans CJK KR"; done
    for l in zh zh-CN zh-SG zh-Hans zh-Hans-CN; do check "sans\\-serif:lang=$l:charset=4e00" "Noto Sans CJK SC"; done
    for l in zh-TW zh-MO zh-Hant zh-Hant-TW; do check "sans\\-serif:lang=$l:charset=4e00" "Noto Sans CJK TC"; done
    check 'sans\-serif:lang=zh-HK:charset=4e00' "Noto Sans CJK HK"
    check 'sans\-serif:lang=ja:charset=3042' "Noto Sans CJK JP"
    check 'sans\-serif:lang=ko:charset=ac00' "Noto Sans CJK KR"
    check 'serif:lang=ja:charset=4e00' "Noto Sans CJK JP"
    check 'monospace:lang=ko:charset=4e00' "Noto Sans CJK KR"
    check 'sans\-serif:bold:lang=ja:charset=4e00' "Noto Sans CJK JP" Bold
    check 'Noto Sans CJK JP' "Noto Sans CJK JP"
    check 'Noto Sans KR' "Noto Sans CJK KR"
    # What Chromium asks for a character no font of the page's has
    # (ui/gfx/font_fallback_linux.cc): the whole list sorted for a pattern of
    # the language alone, and the first font in it with the character. The
    # first CJK face it reaches must be the language's.
    cjk_first() {   # <lang> <family>
        if [ -n "$1" ]; then pat=":lang=$1:scalable=true"; else pat=":scalable=true"; fi
        got=$(fc-match -a -f '%{family[0]}\n' "$pat" | grep -m1 'CJK')
        if [ "$got" = "$2" ]; then
            echo "ok    sorted list for lang '${1:-none}': first CJK face $got"
        else
            echo "FAIL  sorted list for lang '${1:-none}': first CJK face $got, want $2"
            fail=1
        fi
    }
    cjk_first "" "Noto Sans CJK SC"
    cjk_first ja "Noto Sans CJK JP"
    cjk_first ko "Noto Sans CJK KR"
    cjk_first zh-CN "Noto Sans CJK SC"
    cjk_first zh-Hans "Noto Sans CJK SC"
    cjk_first zh-TW "Noto Sans CJK TC"
    cjk_first zh-Hant "Noto Sans CJK TC"
    cjk_first zh-HK "Noto Sans CJK HK"
    # and the emoji pattern (Blink's kColorEmojiLocale) reaches the emoji
    # font before the Noto Sans Symbols 2 pictographs
    got=$(fc-match -a -f '%{family[0]}\n' ':lang=und-zsye:scalable=true' | grep -m1 -e 'Emoji' -e 'Symbols 2')
    if [ "$got" = "Noto Color Emoji" ]; then
        echo "ok    sorted list for the emoji lang: $got before Symbols 2"
    else
        echo "FAIL  sorted list for the emoji lang: first of the emoji/pictograph fonts is $got"
        fail=1
    fi
fi
if [ $fail = 0 ]; then
    echo "fonts-check: PASS"
else
    echo "fonts-check: FAIL"
fi
exit $fail
