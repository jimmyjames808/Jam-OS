#!/bin/bash
# Fetch the build host's toolchains (docs/CR-PLAN.md section 1, track
# CT1): each one in tools/cr/toolchain.pins, by its size and SHA-256, into
# the cache, then put where the drivers and the builder look:
#
#   clang   Chromium's prebuilt clang and lld, unpacked into
#           ~/.cache/jamos/toolchains/clang-<version>/ (bin/clang,
#           bin/ld.lld, bin/llvm-ar, lib/clang/<major>/include ...): what
#           tools/jamos-clang runs by default
#   cmake   Kitware's CMake wheel, installed into the ports builder's venv
#           (~/.cache/jamos/tools, or JAMOS_TOOLS), beside Meson and Ninja
#
#   tools/cr/toolchain.sh [clang] [cmake]     (no names: all of them)
#   tools/cr/toolchain.sh --dir clang         prints clang's directory
#   JAMOS_REMOTE=jamos-pc JAMOS_REMOTE_ONLY=1 tools/remote-test.sh tools/cr/toolchain.sh
#
# A download is looked for in the distfiles cache (JAMOS_DISTFILES, default
# ~/.cache/jamos/distfiles), then the owner's mirror (JAMOS_DISTFILES_MIRROR,
# default ~/jamos-distfiles; `none`: no mirror), then fetched from its URL;
# every candidate is checked by size and SHA-256 before it is used, and a
# good download is copied into the mirror, as tools/portslib/fetch.py does
# for a port's sources. Done once per version: a toolchain whose directory
# holds the pin's line already is left alone. One run at a time (flock).
# The clang package is a Linux x86_64 binary: on another host this script
# says so and stops (on the Mac, JAMOS_CLANG names Homebrew's LLVM instead).
# Rust (CT2) adds its own line here.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
pins=$here/toolchain.pins
toolchains=${JAMOS_TOOLCHAINS:-$HOME/.cache/jamos/toolchains}
distfiles=${JAMOS_DISTFILES:-$HOME/.cache/jamos/distfiles}
mirror=${JAMOS_DISTFILES_MIRROR:-$HOME/jamos-distfiles}
tools=${JAMOS_TOOLS:-$HOME/.cache/jamos/tools}

pin() {   # pin <name>: the pin's line's words, or an error
    local line
    line=$(awk -v n="$1" '$1 == n && NF == 5' "$pins")
    [ -n "$line" ] || { echo "toolchain: no pin for $1 in $pins" >&2; exit 2; }
    echo "$line"
}

if [ "${1:-}" = --dir ]; then
    read -r name version _ < <(pin "${2:-clang}")
    echo "$toolchains/$name-$version"
    exit 0
fi

sha256() { if command -v sha256sum > /dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | awk '{ print $1 }'; }
size_of() { wc -c < "$1" | tr -d ' '; }
good() {   # good <file> <size> <sha256>
    [ -f "$1" ] && [ "$(size_of "$1")" = "$2" ] && [ "$(sha256 "$1")" = "$3" ]
}

# fetch <size> <sha256> <url>: the file in the cache, checked; prints its path.
fetch() {
    local size=$1 hash=$2 url=$3 file dst
    file=${url##*/}
    dst=$distfiles/$file
    mkdir -p "$distfiles"
    if good "$dst" "$size" "$hash"; then
        echo "$dst"
        return
    fi
    rm -f "$dst"
    if [ "$mirror" != none ] && good "$mirror/$file" "$size" "$hash"; then
        cp "$mirror/$file" "$dst.part"
    else
        echo "toolchain: fetching $url ($((size / 1048576)) MB)" >&2
        curl -fsSL --retry 3 --max-filesize "$size" -o "$dst.part" "$url" ||
            { rm -f "$dst.part"; echo "toolchain: $file: the download failed" >&2; exit 1; }
    fi
    if ! good "$dst.part" "$size" "$hash"; then
        echo "toolchain: $file: size $(size_of "$dst.part") sha256 $(sha256 "$dst.part")," \
             "want $size $hash: refused" >&2
        rm -f "$dst.part"
        exit 1
    fi
    mv "$dst.part" "$dst"
    if [ "$mirror" != none ] && mkdir -p "$mirror" 2> /dev/null && [ ! -f "$mirror/$file" ]; then
        cp "$dst" "$mirror/$file" || true
    fi
    echo "$dst"
}

get_clang() {
    local name version size hash url dir tarball
    read -r name version size hash url < <(pin clang)
    if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then
        echo "toolchain: clang: Chromium's package runs on Linux x86_64 only (here:" \
             "$(uname -s) $(uname -m)); on the Mac set JAMOS_CLANG to Homebrew's LLVM" >&2
        return 1
    fi
    dir=$toolchains/$name-$version
    if [ -f "$dir/.jamos-pin" ] && [ "$(cat "$dir/.jamos-pin")" = "$name $version $size $hash $url" ]; then
        echo "toolchain: clang $version: there ($dir)"
        return 0
    fi
    tarball=$(fetch "$size" "$hash" "$url")
    rm -rf "$dir" "$dir.tmp"
    mkdir -p "$dir.tmp"
    tar -xJf "$tarball" -C "$dir.tmp"
    "$dir.tmp/bin/clang" --version > /dev/null ||
        { echo "toolchain: clang $version doesn't run here" >&2; exit 1; }
    echo "$name $version $size $hash $url" > "$dir.tmp/.jamos-pin"
    mv "$dir.tmp" "$dir"
    echo "toolchain: clang $version: unpacked into $dir ($(du -sh "$dir" | awk '{ print $1 }'))"
    "$dir/bin/clang" --version | head -1
}

get_cmake() {
    local name version size hash url wheel have
    read -r name version size hash url < <(pin cmake)
    have=$("$tools/bin/cmake" --version 2> /dev/null | awk 'NR == 1 { print $3 }') || true
    if [ "$have" = "$version" ]; then
        echo "toolchain: cmake $version: there ($tools/bin/cmake)"
        return 0
    fi
    if [ "$(uname -s)" != Linux ]; then
        echo "toolchain: cmake: the pinned wheel is Linux's; on the Mac, Homebrew's cmake" >&2
        return 1
    fi
    wheel=$(fetch "$size" "$hash" "$url")
    [ -x "$tools/bin/pip" ] || python3 -m venv "$tools"
    # --no-deps --no-index: the one checked wheel, nothing else from the network.
    "$tools/bin/pip" install --quiet --no-deps --no-index "$wheel"
    echo "toolchain: cmake: $("$tools/bin/cmake" --version | head -1) in $tools/bin"
}

names=("$@")
[ ${#names[@]} -gt 0 ] || names=(clang cmake)
mkdir -p "$toolchains"
exec 9> "$toolchains/.lock"
flock 9 2> /dev/null || true   # the Mac has no flock(1): one run at a time is the user's there
rc=0
for n in "${names[@]}"; do
    case $n in
    clang) get_clang || rc=1 ;;
    cmake) get_cmake || rc=1 ;;
    *) echo "toolchain: no toolchain called $n (clang, cmake)" >&2; rc=2 ;;
    esac
done
exit $rc
