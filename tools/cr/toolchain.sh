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
#   rust    Chromium's pinned Rust (rustc, cargo, rust-src with its vendored
#           crates, bindgen), unpacked into
#           ~/.cache/jamos/toolchains/rust-<version>/ (track CT2): what a
#           `toolchain clang rust` port builds with (tools/portslib/toolchain.py)
#   gperf   GNU gperf, built from its pinned source (needs a C++ compiler and
#           make on the host) into ~/.cache/jamos/tools/bin, where the ports
#           builder, tools/cr/hostcheck.sh and Chromium's build find it
#           first on PATH: Chromium's build runs gperf, and the build host's
#           own is not depended on
#
#   tools/cr/toolchain.sh [clang] [cmake] [rust] [gperf]     (no names: all)
#   tools/cr/toolchain.sh --dir clang         prints clang's directory (or rust's)
#   JAMOS_REMOTE=jamos-pc JAMOS_REMOTE_ONLY=1 tools/remote-test.sh tools/cr/toolchain.sh
#
# A download is looked for in the distfiles cache (JAMOS_DISTFILES, default
# ~/.cache/jamos/distfiles), then the owner's mirror (JAMOS_DISTFILES_MIRROR,
# default ~/jamos-distfiles; `none`: no mirror), then fetched from its URL;
# every candidate is checked by size and SHA-256 before it is used, and a
# good download is copied into the mirror, as tools/portslib/fetch.py does
# for a port's sources. Done once per version: a toolchain whose directory
# holds the pin's line already is left alone. One run at a time (flock).
# The clang and Rust packages are Linux x86_64 binaries: on another host this
# script says so and stops (on the Mac, JAMOS_CLANG names Homebrew's LLVM
# instead). The version of each is a line of tools/cr/toolchain.pins.
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

get_rust() {
    local name version size hash url dir tarball
    read -r name version size hash url < <(pin rust)
    if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then
        echo "toolchain: rust: Chromium's package runs on Linux x86_64 only (here:" \
             "$(uname -s) $(uname -m))" >&2
        return 1
    fi
    dir=$toolchains/$name-$version
    if [ -f "$dir/.jamos-pin" ] && [ "$(cat "$dir/.jamos-pin")" = "$name $version $size $hash $url" ]; then
        echo "toolchain: rust $version: there ($dir)"
        return 0
    fi
    tarball=$(fetch "$size" "$hash" "$url")
    rm -rf "$dir" "$dir.tmp"
    mkdir -p "$dir.tmp"
    tar -xJf "$tarball" -C "$dir.tmp"
    "$dir.tmp/bin/rustc" --version > /dev/null ||
        { echo "toolchain: rust $version doesn't run here" >&2; exit 1; }
    [ -f "$dir.tmp/lib/rustlib/src/rust/library/Cargo.toml" ] && [ -d "$dir.tmp/lib/rustlib/src/rust/library/vendor" ] ||
        { echo "toolchain: rust $version: no rust-src with its vendored crates" >&2; exit 1; }
    echo "$name $version $size $hash $url" > "$dir.tmp/.jamos-pin"
    mv "$dir.tmp" "$dir"
    echo "toolchain: rust $version: unpacked into $dir ($(du -sh "$dir" | awk '{ print $1 }'))"
    "$dir/bin/rustc" --version
}

get_gperf() {
    local name version size hash url have tarball work
    read -r name version size hash url < <(pin gperf)
    have=$("$tools/bin/gperf" --version 2> /dev/null | awk 'NR == 1 { print $NF }') || true
    if [ "$have" = "$version" ]; then
        echo "toolchain: gperf $version: there ($tools/bin/gperf)"
        return 0
    fi
    for t in make "${CXX:-c++}" tar; do
        command -v "$t" > /dev/null ||
            { echo "toolchain: gperf: building it needs $t on the host" >&2; return 1; }
    done
    tarball=$(fetch "$size" "$hash" "$url")
    work=$(mktemp -d "${TMPDIR:-/tmp}/gperf.XXXXXX")
    # Only lib/ and src/ are built: the documentation's generated files can
    # look out of date to make and ask for makeinfo.
    (
        cd "$work" || exit 1
        tar -xzf "$tarball"
        cd "gperf-$version" || exit 1
        ./configure --prefix="$tools" > "$work/configure.log" 2>&1 &&
            make -C lib > "$work/make.log" 2>&1 &&
            make -C src >> "$work/make.log" 2>&1
    ) || { tail -20 "$work"/*.log >&2; echo "toolchain: gperf $version: the build failed (logs in $work)" >&2; return 1; }
    mkdir -p "$tools/bin"
    cp "$work/gperf-$version/src/gperf" "$tools/bin/gperf.part"
    chmod 755 "$tools/bin/gperf.part"
    mv "$tools/bin/gperf.part" "$tools/bin/gperf"
    rm -rf "$work"
    echo "toolchain: gperf: $("$tools/bin/gperf" --version | head -1) in $tools/bin"
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
[ ${#names[@]} -gt 0 ] || names=(clang cmake rust gperf)
mkdir -p "$toolchains"
exec 9> "$toolchains/.lock"
flock 9 2> /dev/null || true   # the Mac has no flock(1): one run at a time is the user's there
rc=0
for n in "${names[@]}"; do
    case $n in
    clang) get_clang || rc=1 ;;
    cmake) get_cmake || rc=1 ;;
    rust) get_rust || rc=1 ;;
    gperf) get_gperf || rc=1 ;;
    *) echo "toolchain: no toolchain called $n (clang, cmake, rust, gperf)" >&2; rc=2 ;;
    esac
done
exit $rc
