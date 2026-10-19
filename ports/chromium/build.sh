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
# can say `targets=...` (the ninja targets, default base_unittests,
# test_child_process, which base_unittests starts, headless_shell and
# content_shell: each named so a stale link of it is redone, below), `keep=N` (ninja goes
# on past N failures: -k) or
# `stop=gen` (GN only, then stop).
#
# plan (`ports.py build --plan`, run by tools/cr/build.sh start to size its
# claim in the machine's budget): everything build does up to ninja, then
# `ninja -n` to count what the build would do, and no build. GN is run
# first if args.gn changed (ninja -n only says "Regenerating ninja files"
# when it must, and would stop there). The counts are written to
# $kept/plan.txt: `steps` (ninja's total), `compile` (CXX, CC, ASM, Rust),
# `link` (LINK, SOLINK), `stamp` and `other`, or `unknown 1` when ninja
# still asked for GN after a run of it. A program linked with other libraries
# than the sysroot's now is put aside while ninja counts and put back, so its
# link is among the steps and a plan changes no program (nor links.now, nor
# a <program>.links); the kept tree's source, sysroot and tools are brought
# up to date, as a build would.
prepare() {
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

    targets="base_unittests test_child_process headless_shell content_shell"
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
    args_changed=0
    cmp -s "$TMPDIR/args.gn" "$out/args.gn" || { cp "$TMPDIR/args.gn" "$out/args.gn"; args_changed=1; }
    if [ ! -f "$out/build.ninja" ] || [ "$stop" = gen ] || { [ -n "${planning:-}" ] && [ "$args_changed" = 1 ]; }; then
        buildtools/linux64/gn gen "$out" || exit 1
    fi
    if [ "$stop" = gen ] && [ -z "${planning:-}" ]; then
        echo "chromium: GN only ($kept/build.conf says stop=gen)"
        exit 3
    fi
    # A program's link takes the sysroot's libraries, crt files and posix.ld
    # through the port's wrapper (its image-base) and tools/jamos-clang,
    # none of which ninja sees: a target's program linked with others
    # than these (its <target>.links says which, as a hash) goes, so ninja
    # links it again.
    links=$(cat "$CC" "$top/tools/jamos-clang" "$JAMOS_SYSROOT"/lib/posix.ld \
                "$JAMOS_SYSROOT"/lib/*.a "$JAMOS_SYSROOT"/lib/*.o | sha256sum | cut -c1-16)
    # This build's, for tools/cr/build.sh's summary: a program whose
    # <target>.links differs was linked by an earlier build (not a target).
    # A plan changes none of this (the stale programs stay where they are).
    stale=
    for t in $targets; do
        [ "$(cat "$out/$t.links" 2> /dev/null)" = "$links" ] || stale="$stale $t"
    done
    if [ -z "${planning:-}" ]; then
        echo "$links" > "$out/links.now"
        for t in $stale; do
            rm -f "$out/$t"
            echo "$links" > "$out/$t.links"
        done
    fi
    ulimit -n 4096 2> /dev/null || true   # the big links open many files
}

build() {
    prepare
    # shellcheck disable=SC2086 # the targets are words
    ninja -C "$out" -j "$jobs" -k "$keep" $targets
}

plan() {
    planning=1
    prepare
    rm -f "$kept/plan.txt"
    # The stale programs are put aside while ninja counts (a missing output is a
    # step, their link) and put back, so a plan that is not followed by a build
    # leaves them as they were.
    for t in $stale; do
        if [ -e "$out/$t" ]; then mv "$out/$t" "$out/$t.planhold"; fi
    done
    trap 'for t in $stale; do if [ -e "$out/$t.planhold" ]; then mv "$out/$t.planhold" "$out/$t"; fi; done' EXIT
    pass=1
    while :; do
        # shellcheck disable=SC2086 # the targets are words
        ninja -C "$out" -n $targets > "$TMPDIR/plan.ninja"
        grep -q 'Regenerating ninja files' "$TMPDIR/plan.ninja" || break
        if [ "$pass" = 2 ]; then   # GN ran and ninja wants it again: no count to give
            echo "unknown 1" > "$kept/plan.txt"
            return 0
        fi
        buildtools/linux64/gn gen "$out" || exit 1
        pass=2
    done
    awk -f "$top/tools/cr/ninja-count.awk" "$TMPDIR/plan.ninja" > "$kept/plan.txt.new"
    mv "$kept/plan.txt.new" "$kept/plan.txt"
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
    # headless_command_resources.pak: the page --dump-dom, --screenshot and
    # --print-to-pdf run in, read from beside the program (without it they
    # print nothing and exit 0); content_shell.pak: content_shell's
    # resources, read from beside it too (without it it aborts at start)
    for f in base_unittests test_child_process headless_shell headless_lib_data.pak \
             headless_lib_strings.pak headless_command_resources.pak content_shell \
             content_shell.pak; do
        cp "out/jamos/$f" "$d/$f"
    done
}
