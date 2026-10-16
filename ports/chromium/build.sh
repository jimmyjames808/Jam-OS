# chromium (ports/chromium/recipe): Chromium's own build, GN then ninja,
# in the kept tree (tools/portslib/kept.py): the output is out/jamos inside
# the source, kept between builds, so a change to args.gn or a patch
# rebuilds only what it changes.
#
# Chromium's DEPS would put its pinned host tools into the tree; here they
# are links to where tools/cr/toolchain.sh keeps the same ones: its clang
# (third_party/llvm-build/Release+Asserts) and Rust
# (third_party/rust-toolchain), its Node.js, the Debian sysroot its host
# tools are built against, its test fonts, Dawn's Go, and the pinned gperf
# in place of the tarball's prebuilt one. GN is the tarball's own (buildtools/linux64/gn, Chromium's
# pinned build); ninja is the ports builder's.
#
# The arguments are files/args.gn plus the machine's three lines: the
# sysroot, the link driver and pkg-config, all in the kept tree (their
# paths stay the same from build to build, so the commands do).
# $kept/build.conf, if there,
# can say `targets=...` (the ninja targets, default base_unittests and
# headless_shell), `keep=N` (ninja goes on past N failures: -k) or
# `stop=gen` (GN only, then stop).
build() {
    top=$portdir/../..
    clang=$(dirname "$JAMOS_CLANG")
    node=$("$top/tools/cr/toolchain.sh" --dir node)/node-linux-x64
    deb=$("$top/tools/cr/toolchain.sh" --dir sysroot)
    fonts=$("$top/tools/cr/toolchain.sh" --dir test-fonts)
    go=$("$top/tools/cr/toolchain.sh" --dir go)/go
    gperf=$(command -v gperf) || { echo "chromium: no gperf (tools/cr/toolchain.sh gperf)"; exit 1; }
    for f in "$clang/bin/clang" "$JAMOS_RUST/bin/rustc" "$JAMOS_RUST/VERSION" "$node/bin/node" \
             "$deb/usr/include/stdio.h" "$fonts/Ahem.ttf" "$go/bin/go"; do
        [ -e "$f" ] || { echo "chromium: no $f (tools/cr/toolchain.sh fetches it)"; exit 1; }
    done
    link "$clang" third_party/llvm-build/Release+Asserts
    link "$JAMOS_RUST" third_party/rust-toolchain
    link "$node" third_party/node/linux/node-linux-x64
    link "$deb" build/linux/debian_bullseye_amd64-sysroot
    link "$gperf" third_party/gperf/cipd/bin/gperf
    link "$fonts" third_party/test_fonts/test_fonts
    link "$go" third_party/dawn/tools/golang/linux-amd64
    # Dawn's generator (`go run`) offline, with this Go only, its caches here.
    export GOPROXY=off GOTOOLCHAIN=local GOTELEMETRY=off GOFLAGS=-mod=mod \
        GOCACHE=$kept/go/cache GOPATH=$kept/go/path

    targets="base_unittests headless_shell"
    stop=
    keep=1
    # shellcheck disable=SC1091
    [ -f "$kept/build.conf" ] && . "$kept/build.conf"

    out=out/jamos
    mkdir -p "$out"
    {
        cat "$portdir/files/args.gn"
        echo "target_sysroot = \"$JAMOS_SYSROOT\""
        echo "jamos_link_driver = \"$CC\""
        # The port's pkg-config (it knows the sysroot's layout: lib/pkgconfig).
        echo "pkg_config = \"$PKG_CONFIG\""
    } > "$TMPDIR/args.gn"
    # Written only when it changes: a new args.gn makes ninja run GN again.
    cmp -s "$TMPDIR/args.gn" "$out/args.gn" || cp "$TMPDIR/args.gn" "$out/args.gn"
    if [ ! -f "$out/build.ninja" ] || [ "$stop" = gen ]; then
        buildtools/linux64/gn gen "$out" || exit 1
    fi
    if [ "$stop" = gen ]; then
        echo "chromium: GN only ($kept/build.conf says stop=gen)"
        exit 3
    fi
    ulimit -n 4096 2> /dev/null || true   # the big links open many files
    # shellcheck disable=SC2086 # the targets are words
    ninja -C "$out" -j "$jobs" -k "$keep" $targets
}

# link <target> <path>: path made a symbolic link to target (a file or
# directory the tarball has there is replaced).
link() {
    [ "$(readlink "$2" 2> /dev/null)" = "$1" ] && return
    mkdir -p "$(dirname "$2")"
    rm -rf "$2"
    ln -s "$1" "$2"
}

install() {
    d=$destdir$prefix/lib/chromium
    mkdir -p "$d"
    for f in base_unittests test_child_process headless_shell headless_lib_data.pak \
             headless_lib_strings.pak; do
        cp "out/jamos/$f" "$d/$f"
    done
}
