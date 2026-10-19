#!/bin/sh
# Chromium's tests on Jam OS (docs/CR-PLAN.md, "How runs reach QEMU"),
# one case each, run on the PC through tools/remote-test.sh. A case builds
# what it runs (on the build host: the PC's WSL), checks it there, then
# boots it in QEMU (tools/ports-test.sh's way: the package preinstalled on
# /data, approved, its test fed to the shell) and checks the output.
#
#   tools/cr-test.sh <outdir> <case>...
#   JAMOS_REMOTE=jamos-pc JAMOS_REMOTE_ONLY=1 tools/remote-test.sh \
#       tools/cr-test.sh build/test/cr hello
#
# Cases (milestone CR-a now; the CB tracks add theirs):
#   hello   ports/cr-hello, the C++ program (track CT1): built by
#           tools/jamos-clang++ with LLVM's runtimes (ports/llvm-runtimes,
#           ports/linux-uapi); the syscall scan on it (tools/cr/syscall-scan.py);
#           the same source built by the build host's own c++ prints
#           ports/cr-hello/files/expected.txt; then on Jam OS from /data it
#           prints the same, byte for byte (ports/cr-hello/test)
#   rust    ports/cr-rust-hello, the Rust program (track CT2): Chromium's
#           pinned Rust builds the standard library from rust-src for
#           x86_64-unknown-linux-musl and the port's clang driver links it
#           against Jam OS's musl; the syscall scan on it; the same source
#           built for the build host by the same rustc prints
#           ports/cr-rust-hello/files/expected.txt (and its `panic` mode
#           aborts with expected-panic.txt); then on Jam OS from /data it
#           prints the same, byte for byte, and aborts the same way
#           (ports/cr-rust-hello/test)
#   base    base_unittests (track CB2, CR-b) from the kept tree's stage
#           (~/cr/ports/chromium/stage: tools/cr/build.sh start first), on a
#           stick with a 6 GiB /data, a sparse file (tools/cr/base-stick.py: no package,
#           the format holds no file over 64 MiB), run suite by suite with
#           the ENOSYS log on (tools/cr/base-run.py); the tests in
#           tools/cr/base_unittests.skip are not run. CR_BASE_SHARD=i/n,
#           CR_BASE_ALL=1, CR_BASE_LISTED=1 (only the suites with a test on
#           the list; with CR_BASE_ALL=1 the way to re-measure it),
#           CR_BASE_DEATH=fast (death tests in a fork only; the default is
#           threadsafe, the way base sets them: the child execve()s the program),
#           CR_BASE_ONLY=<glob>, CR_BASE_TIMEOUT=<seconds a suite may run;
#           240 by default>, CR_BASE_ENV="NAME=value ..."
#           (default TMPDIR and TEST_TMPDIR=/data/cr/tmp; CR_BASE_ENV= empty
#           leaves both unset and the programs use init's /tmp); the stick's
#           /data is the image's: ext, or FAT when JAMOS_DATA_FS=fat made the
#           image (tools/cr/base-stick.py fills an ext one with jamext;
#           QEMU_BOOT_NVME=1 boots the stick as an installed NVMe drive); results in
#           <outdir>/base/run/ (results.txt, failures/, enosys.txt, summary.txt)
#   shell-wl content_shell (track CB4) on the desktop, Ozone's Wayland, one
#           process, software output: the kept tree's out/jamos program (a
#           relink leaves it there: tools/cr/build.sh start content_shell),
#           its pak and Inter on a 2 GiB stick (tools/cr/base-stick.py's
#           lists), a USB keyboard and mouse; it loads tools/cr/shell-wl.html
#           by name from an http.server here (netpeer's DNS and relay), and
#           tools/cr/shell-wl.txt clicks, types, opens the list's popup and
#           picks from it by keys and by a click, turns the wheel and closes
#           the window (Super+Q: exit 0), each step waited for as the page's
#           console line; then tools/cr/shell-wl-check.py looks at the
#           screenshots (<outdir>/shell-wl/run/shell-wl-*.png) where the page
#           drew; 1280x800 only (the clicks are at pixel positions)
#   headless  headless_shell (track CB3) in one process (--no-sandbox
#           --single-process --no-zygote --headless --disable-gpu
#           --js-flags=--jitless): the kept tree's out/jamos program (a relink
#           leaves it there: tools/cr/build.sh start headless_shell) and its
#           three paks, Inter and a fonts.conf on a 2 GiB stick; one boot runs
#           it twice on each of three copies of tools/cr/headless.html,
#           --dump-dom and --screenshot (800x600): the page on /data
#           (file://); the page by name over HTTP (www.jam, netpeer's DNS and
#           relay to a server here) with its picture from a second name
#           (mac.jam); the page over HTTPS (a server here with a certificate
#           openssl made for the run, trusted by this run only through
#           --ignore-certificate-errors-spki-list with its SPKI hash, not the
#           blanket --ignore-certificate-errors), and once more without the
#           flag, which must be refused. tools/cr/headless-check.py checks
#           the DOM dumps, the PNGs (size, colours at a few pixels) and what
#           the servers saw (tools/cr/headless-server.py's logs); needs
#           openssl and an ext /data; <outdir>/headless/ has them all
#   multi   content_shell (track CB5) with every process type its own
#           (--no-zygote --no-sandbox: Chromium's launcher forks and execs
#           /proc/self/exe for the GPU process, the network and storage
#           services and the renderers), the same stick and server as
#           shell-wl (6 GiB of QEMU memory: CR_MULTI_MEM); it loads
#           tools/cr/multi.html by name, tools/cr/multi.txt lists the
#           processes (the browser and at least four children), kills the
#           renderer the way chrome://kill does (it sends itself SIGTERM),
#           reloads with the toolbar's Refresh (a renderer again, the tab's
#           session storage says visit 2) and closes the window (exit 0, no
#           process left); tools/cr/multi-check.py checks the screenshots
#           (<outdir>/multi/run/multi-{page,killed,reload}.png) and the log;
#           about 2 minutes
#   jit     content_shell (CR-e) as multi, with V8's compilers in the
#           renderer (its list's `right jit`; ports/chromium's patches 0913
#           and 0914): it loads tools/cr/jit.html by name twice, with the JIT
#           (--js-flags=--trace-opt) and --jitless; the page times a hot loop
#           in JavaScript and in WebAssembly and makes a WebAssembly memory
#           trap; tools/cr/jit.txt checks that `ps` marks exactly one process
#           [jit] (the renderer) the first time and none the second, and
#           tools/cr/jit-check.py the results, TurboFan's line, the speed-up
#           (3x at least) and the screenshots; the timings in
#           <outdir>/jit/check.out; about 3 minutes
#   webgl   content_shell (CR-e) as jit, with WebGL in software: SwiftShader
#           linked in as ANGLE's Vulkan (ports/chromium's patch 0915), its
#           Reactor compiling the shaders in the GPU process, which keeps
#           the JIT power when it runs SwiftShader (patch 0914): it loads
#           tools/cr/webgl.html by name twice, with
#           --enable-unsafe-swiftshader (the browser starts the GPU process
#           with --use-angle=swiftshader-webgl) and without (no WebGL); the
#           page draws a band with a shader and reads three pixels back;
#           tools/cr/webgl-check.py checks the page's lines (a WebGL context,
#           SwiftShader's renderer, the pixels; none the second time), `ps`
#           (one child more marked [jit] the first time: the GPU process) and
#           the screenshots (the band where the canvas is); about 2 minutes
#   bench   content_shell as jit, running parts of JetStream 2.2 and
#           Speedometer 3.1 (tools/cr/toolchain.sh fetches them, pinned;
#           tools/cr/bench-server.py serves them with a reporter that says
#           the results as console lines): CR_BENCH_RUNS (default
#           "jetstream-js:jit jetstream-js:jitless jetstream-wasm:jit
#           speedometer:jit speedometer:jitless", each <run>:<jit|jitless>,
#           the runs bench-server.py's RUNS), one content_shell each, in
#           one boot (CR_BENCH_TIMEOUT seconds in all, 14400);
#           tools/cr/bench-check.py makes the table (<outdir>/bench/check.out,
#           the lines docs/BENCH.md takes); PASS if every run said a score;
#           an hour or more: a long run (pc-batch.sh start --detach)
#
# The toolchain is fetched first if it isn't there (tools/cr/toolchain.sh:
# clang, cmake, and for `rust` Rust; gperf, which Chromium's build needs, is
# built into ~/.cache/jamos/tools by the same script). The ports build at low priority with CR_JOBS jobs (8;
# the first build of the runtimes is a few minutes: an agent with CPUs to
# spare builds them first, `JAMOS_REMOTE_LIGHT_CPUS=16 tools/remote-test.sh
# nice python3 tools/ports.py --out=build/ports build -j 16 cr-hello`).
# <outdir>/<case>/ gets the case's logs and its program (small things only).
# Prints "cr-test: <case>: PASS" or "FAIL (<why>)"; exit 0 if all passed.
set -u
if [ $# -lt 2 ]; then
    echo "usage: tools/cr-test.sh <outdir> <case>..." >&2
    exit 2
fi
out=$1
shift
jobs=${CR_JOBS:-8}
mkdir -p "$out"
rc=0

say() { echo "cr-test: $*"; }

# The ports a case needs, built (with what they need) at low priority.
build_ports() {
    log=$1
    shift
    if ! nice python3 tools/ports.py --out build/ports build -j "$jobs" "$@" > "$log" 2>&1; then
        tail -20 "$log" >&2
        return 1
    fi
    grep -E "built in|up to date" "$log" | sed 's/^/cr-test: /'
}

case_hello() {
    dir=$out/hello
    rm -rf "$dir"
    mkdir -p "$dir"
    tools/cr/toolchain.sh clang cmake > "$dir/toolchain.log" 2>&1 ||
        { tail -5 "$dir/toolchain.log"; echo "FAIL (the toolchain, see $dir/toolchain.log)" > "$dir/result"; return; }
    if ! build_ports "$dir/build.log" cr-hello; then
        echo "FAIL (the build, see $dir/build.log)" > "$dir/result"
        return
    fi
    prog=build/ports/cr-hello/stage/bin/cr-hello
    cp "$prog" "$dir/cr-hello"
    size=$(wc -c < "$prog" | tr -d ' ')
    say "hello: bin/cr-hello is $((size / 1024)) KB"
    if ! python3 tools/cr/syscall-scan.py "$prog" > "$dir/scan.txt" 2>&1; then
        cat "$dir/scan.txt"
        echo "FAIL (the syscall scan, see $dir/scan.txt)" > "$dir/result"
        return
    fi
    sed 's/^/cr-test: /' "$dir/scan.txt"

    # The build host's own build of the same source must print expected.txt.
    src=ports/cr-hello/files
    host_cxx=${CXX_FOR_BUILD:-c++}
    if ! "$host_cxx" -std=c++20 -O2 -pthread -o "$dir/cr-hello-host" "$src/main.cc" \
         "$src/throw.cc" "$src/ctors.cc" > "$dir/host-build.log" 2>&1; then
        echo "FAIL (the build host's $host_cxx, see $dir/host-build.log)" > "$dir/result"
        return
    fi
    "$dir/cr-hello-host" "$dir/host-scratch" > "$dir/host-output.txt" 2> "$dir/host-stderr.txt"
    st=$?
    if [ $st != 0 ] || ! cmp -s "$dir/host-output.txt" "$src/expected.txt"; then
        diff "$src/expected.txt" "$dir/host-output.txt" | head -20
        echo "FAIL (the build host's run differs from expected.txt: status $st)" > "$dir/result"
        return
    fi
    rm -f "$dir/cr-hello-host"
    say "hello: the build host's $host_cxx build prints expected.txt"

    # On Jam OS: from /data, the same output.
    if tools/ports-test.sh "$dir/qemu" cr-hello > "$dir/ports-test.out" 2>&1; then
        echo PASS > "$dir/result"
    else
        grep "ports-test: cr-hello:" "$dir/ports-test.out" | tail -1
        echo "FAIL (on Jam OS: see $dir/qemu/cr-hello/)" > "$dir/result"
    fi
}

case_rust() {
    dir=$out/rust
    rm -rf "$dir"
    mkdir -p "$dir"
    tools/cr/toolchain.sh clang cmake rust gperf > "$dir/toolchain.log" 2>&1 ||
        { tail -5 "$dir/toolchain.log"; echo "FAIL (the toolchain, see $dir/toolchain.log)" > "$dir/result"; return; }
    if ! build_ports "$dir/build.log" cr-rust-hello; then
        echo "FAIL (the build, see $dir/build.log)" > "$dir/result"
        return
    fi
    prog=build/ports/cr-rust-hello/stage/bin/cr-rust-hello
    cp "$prog" "$dir/cr-rust-hello"
    size=$(wc -c < "$prog" | tr -d ' ')
    say "rust: bin/cr-rust-hello is $((size / 1024)) KB"
    if ! python3 tools/cr/syscall-scan.py "$prog" > "$dir/scan.txt" 2>&1; then
        cat "$dir/scan.txt"
        echo "FAIL (the syscall scan, see $dir/scan.txt)" > "$dir/result"
        return
    fi
    sed 's/^/cr-test: /' "$dir/scan.txt"

    # The build host's own build of the same source (the same rustc, its
    # standard library for the host) must print expected.txt, and abort in
    # panic mode (status 134, a signal 6) with expected-panic.txt.
    src=ports/cr-rust-hello/files
    rustc=$(tools/cr/toolchain.sh --dir rust)/bin/rustc
    if ! "$rustc" --edition 2021 -O -C panic=abort -o "$dir/cr-rust-hello-host" "$src/src/main.rs" \
         > "$dir/host-build.log" 2>&1; then
        echo "FAIL (the build host's rustc, see $dir/host-build.log)" > "$dir/result"
        return
    fi
    "$dir/cr-rust-hello-host" "$dir/host-scratch" > "$dir/host-output.txt" 2> "$dir/host-stderr.txt"
    st=$?
    if [ $st != 0 ] || ! cmp -s "$dir/host-output.txt" "$src/expected.txt"; then
        diff "$src/expected.txt" "$dir/host-output.txt" | head -20
        echo "FAIL (the build host's run differs from expected.txt: status $st)" > "$dir/result"
        return
    fi
    # (in a subshell with no core file, so the shell's "Aborted" note is not shown)
    ( ulimit -c 0; "$dir/cr-rust-hello-host" panic > "$dir/host-panic.txt" 2> "$dir/host-panic-stderr.txt" ) 2> /dev/null
    st=$?
    if [ $st != 134 ] || ! cmp -s "$dir/host-panic.txt" "$src/expected-panic.txt"; then
        diff "$src/expected-panic.txt" "$dir/host-panic.txt" | head -20
        echo "FAIL (the build host's panic run differs from expected-panic.txt: status $st)" > "$dir/result"
        return
    fi
    rm -f "$dir/cr-rust-hello-host"
    say "rust: the build host's build prints expected.txt and aborts in panic mode"

    # On Jam OS: from /data, the same output and the same abort.
    if tools/ports-test.sh "$dir/qemu" cr-rust-hello > "$dir/ports-test.out" 2>&1; then
        echo PASS > "$dir/result"
    else
        grep "ports-test: cr-rust-hello:" "$dir/ports-test.out" | tail -1
        echo "FAIL (on Jam OS: see $dir/qemu/cr-rust-hello/)" > "$dir/result"
    fi
}

case_base() {
    dir=$out/base
    rm -rf "$dir"
    mkdir -p "$dir"
    cr=${CR_DIR:-$HOME/cr}
    version=$(awk '$1 == "version" { print $2 }' ports/chromium/recipe)
    stage=$cr/ports/chromium/stage
    srcdir=$cr/$version/src/chromium-$version
    for f in "$stage/lib/chromium/base_unittests" "$stage/lib/chromium/test_child_process" "$srcdir/base/test/data"; do
        [ -e "$f" ] || { echo "FAIL (no $f: tools/cr/build.sh start builds it)" > "$dir/result"; return; }
    done
    image=${PORTS_TEST_IMAGE:-build/jamos.img}
    [ -f "$image" ] || { echo "FAIL (no $image: make image first)" > "$dir/result"; return; }
    stick=$dir/base-stick.img
    # An ext /data (the default image's) is filled by jamext.
    if [ "$(python3 tools/mkimage.py --data-fs "$image")" = ext ] &&
       ! make -s build/host/jamext > "$dir/jamext.out" 2>&1; then
        cat "$dir/jamext.out"
        echo "FAIL (no build/host/jamext, see $dir/jamext.out)" > "$dir/result"
        return
    fi
    if ! python3 tools/cr/base-stick.py --data-mib "${CR_BASE_DATA_MIB:-6144}" "$image" "$stick" \
         "$stage" "$srcdir" "$srcdir/out/jamos" > "$dir/stick.out" 2>&1; then
        cat "$dir/stick.out"
        echo "FAIL (the stick, see $dir/stick.out)" > "$dir/result"
        return
    fi
    sed 's/^/cr-test: /' "$dir/stick.out"
    # CR_BASE_ENV: more shell variables for the program ("TMPDIR=/data/tmp"),
    # CR_BASE_ONLY: only suites matching a glob, CR_BASE_LISTED=1: only suites
    # with a test on the skip list, CR_BASE_ARGS: more arguments
    # for base_unittests ("--v=1"), CR_BASE_ALL=1: the skip list's tests run
    # too (a failure of one is a known failure).
    extra=
    for e in ${CR_BASE_ENV-TMPDIR=/data/cr/tmp TEST_TMPDIR=/data/cr/tmp}; do extra="$extra --env $e"; done
    # shellcheck disable=SC2086
    if python3 tools/cr/base-run.py --shard "${CR_BASE_SHARD:-0/1}" --mem "${QEMU_MEM:-4G}" \
       --death-style "${CR_BASE_DEATH:-threadsafe}" ${CR_BASE_ALL:+--all} ${CR_BASE_LISTED:+--listed} ${CR_BASE_ONLY:+--only "$CR_BASE_ONLY"} ${CR_BASE_ARGS:+--args " $CR_BASE_ARGS"} ${CR_BASE_TIMEOUT:+--suite-timeout "$CR_BASE_TIMEOUT"} $extra \
       "$dir/run" "$stick" > "$dir/base-run.out" 2>&1; then
        echo PASS > "$dir/result"
    else
        echo "FAIL ($(grep 'base-run: [0-9]* new failures' "$dir/base-run.out" | sed 's/^base-run: //'), see $dir/run/)" > "$dir/result"
    fi
    sed 's/^/cr-test: /' "$dir/base-run.out"
    rm -f "$stick"
}

# shell_run <dir> <page> <script> <name> <QEMU's memory> <seconds>:
# content_shell (the kept tree's out/jamos program: a relink, tools/cr/build.sh
# start content_shell, leaves it there) with its pak and Inter on a 2 GiB
# stick, <page> served by name (www.jam:8000, netpeer's DNS and relay to an
# http.server here) as page.html, and QEMU booted from the stick with a USB
# keyboard and mouse, <script> fed to the shell (the log and screenshots in
# <dir>/run/<name>.*). Returns qemu-test.sh's status, or 2 with <dir>/result
# written when a piece is missing. The shell-wl, multi, jit, webgl and bench cases;
# the web server is $serve (default serve_web: the page's directory) on the
# port it is given.
serve_web() {
    python3 -m http.server "$1" --bind 127.0.0.1 --directory "$dir/web"
}

shell_run() {
    dir=$1
    cr=${CR_DIR:-$HOME/cr}
    version=$(awk '$1 == "version" { print $2 }' ports/chromium/recipe)
    srcdir=$cr/$version/src/chromium-$version
    for f in "$srcdir/out/jamos/content_shell" "$srcdir/out/jamos/content_shell.pak"; do
        [ -f "$f" ] || { echo "FAIL (no $f: tools/cr/build.sh start content_shell builds it)" > "$dir/result"; return 2; }
    done
    image=${PORTS_TEST_IMAGE:-build/jamos.img}
    [ -f "$image" ] || { echo "FAIL (no $image: make image first)" > "$dir/result"; return 2; }
    if [ "$(python3 tools/mkimage.py --data-fs "$image")" = ext ] &&
       ! make -s build/host/jamext > "$dir/jamext.out" 2>&1; then
        echo "FAIL (no build/host/jamext, see $dir/jamext.out)" > "$dir/result"
        return 2
    fi
    # Inter and a fontconfig file naming it, beside the program; the page
    # in a directory of its own, served by name (www.jam, netpeer's DNS).
    mkdir -p "$dir/add/out/jamos/fonts" "$dir/web"
    cp third_party/inter/Inter-Regular.ttf "$dir/add/out/jamos/fonts/"
    printf '%s\n' '<?xml version="1.0"?>' '<!DOCTYPE fontconfig SYSTEM "fonts.dtd">' \
        '<fontconfig><dir>/data/cr/out/jamos/fonts</dir><cachedir>/data/cr/tmp/fccache</cachedir></fontconfig>' \
        > "$dir/add/out/jamos/fonts/fonts.conf"
    cp "$2" "$dir/web/page.html"
    stick=$dir/shell-stick.img
    if ! python3 tools/cr/base-stick.py --data-mib 2048 --programs content_shell \
         --program-dir "$srcdir/out/jamos" --out-files content_shell.pak --source-files "" \
         --add "$dir/add" "$image" "$stick" "$srcdir/out/jamos" "$srcdir" "$srcdir/out/jamos" \
         > "$dir/stick.out" 2>&1; then
        cat "$dir/stick.out"
        echo "FAIL (the stick, see $dir/stick.out)" > "$dir/result"
        return 2
    fi
    sed 's/^/cr-test: /' "$dir/stick.out"
    # shellcheck disable=SC2046 # one word, the port
    set -- "$@" $(python3 tools/netpeer.py --free-ports 1)
    ${serve:-serve_web} "$7" > "$dir/http.log" 2>&1 &
    hpid=$!
    QEMU_IMAGE=$stick QEMU_INPUT=$3 QEMU_TIMEOUT=${QEMU_TIMEOUT:-$6} \
        QEMU_MEM=${QEMU_MEM:-$5} QEMU_NET=1 QEMU_NET_PEER="--tcp-relay 8000:$7" \
        QEMU_USB="-device usb-kbd,bus=xhci.0,port=2 -device usb-mouse,bus=xhci.0,port=3" \
        tools/qemu-test.sh "$dir/run" "$4" shell > "$dir/qemu.out" 2>&1
    qrc=$?
    kill "$hpid" 2> /dev/null
    rm -f "$stick"
    return $qrc
}

case_shell_wl() {
    dir=$out/shell-wl
    rm -rf "$dir"
    mkdir -p "$dir"
    shell_run "$dir" tools/cr/shell-wl.html tools/cr/shell-wl.txt shell-wl 4G 300
    qrc=$?
    [ $qrc = 2 ] && [ -f "$dir/result" ] && return
    # what the page said (of the text box's lines, one a key, the last)
    grep -a -o 'cb4: [^"]*' "$dir/run/shell-wl.log" 2> /dev/null | uniq |
        awk '!/^cb4: input / || /Hello Jam$/' | sed 's/^/cr-test: page said: /'
    if [ $qrc -ne 0 ]; then
        echo "FAIL (the script: $(grep 'serial-feed: .*: no ' "$dir/qemu.out" | tail -1 | sed 's/^serial-feed: //'), see $dir/run/)" > "$dir/result"
        return
    fi
    if python3 tools/cr/shell-wl-check.py "$dir/run" > "$dir/check.out" 2>&1; then
        echo PASS > "$dir/result"
    else
        echo "FAIL (the screenshots, see $dir/check.out)" > "$dir/result"
    fi
    sed 's/^/cr-test: /' "$dir/check.out"
}

case_headless() {
    dir=$out/headless
    rm -rf "$dir"
    mkdir -p "$dir"
    cr=${CR_DIR:-$HOME/cr}
    version=$(awk '$1 == "version" { print $2 }' ports/chromium/recipe)
    srcdir=$cr/$version/src/chromium-$version
    # The kept tree's own out/jamos: a relink (tools/cr/build.sh start
    # headless_shell) leaves the program and its paks there.
    paks="headless_lib_data.pak headless_lib_strings.pak headless_command_resources.pak"
    for f in "$srcdir/out/jamos/headless_shell" $(for p in $paks; do echo "$srcdir/out/jamos/$p"; done); do
        [ -f "$f" ] || { echo "FAIL (no $f: tools/cr/build.sh start headless_shell builds it)" > "$dir/result"; return; }
    done
    command -v openssl > /dev/null || { echo "FAIL (no openssl on the build host)" > "$dir/result"; return; }
    image=${PORTS_TEST_IMAGE:-build/jamos.img}
    [ -f "$image" ] || { echo "FAIL (no $image: make image first)" > "$dir/result"; return; }
    # The files the guest wrote are read out of its saved stick by jamext.
    if [ "$(python3 tools/mkimage.py --data-fs "$image")" != ext ]; then
        echo "FAIL (the image's /data is not ext: the case reads its files out with jamext)" > "$dir/result"
        return
    fi
    if ! make -s build/host/jamext > "$dir/jamext.out" 2>&1; then
        echo "FAIL (no build/host/jamext, see $dir/jamext.out)" > "$dir/result"
        return
    fi
    # What goes on /data beside the program: Inter and a fontconfig file
    # naming it, the page and its picture. The same page goes to the web
    # directory twice, its picture from a second name (mac.jam), once for
    # each scheme.
    mkdir -p "$dir/add/out/jamos/fonts" "$dir/web"
    cp third_party/inter/Inter-Regular.ttf "$dir/add/out/jamos/fonts/"
    printf '%s\n' '<?xml version="1.0"?>' '<!DOCTYPE fontconfig SYSTEM "fonts.dtd">' \
        '<fontconfig><dir>/data/cr/out/jamos/fonts</dir><cachedir>/data/cr/tmp/fccache</cachedir></fontconfig>' \
        > "$dir/add/out/jamos/fonts/fonts.conf"
    sed -e 's|@BASE@||' -e 's|@WHERE@|local|' tools/cr/headless.html > "$dir/add/out/jamos/page.html"
    sed -e 's|@BASE@|http://mac.jam:8000/|' -e 's|@WHERE@|http|' tools/cr/headless.html > "$dir/web/page-http.html"
    sed -e 's|@BASE@|https://mac.jam:8443/|' -e 's|@WHERE@|https|' tools/cr/headless.html > "$dir/web/page-https.html"
    cp tools/cr/headless-pic.svg "$dir/add/out/jamos/pic.svg"
    cp tools/cr/headless-pic.svg "$dir/web/pic.svg"
    # The certificate, for this run only: made now, for the two names, and
    # trusted by the browser through its public key's hash (base64 of the
    # SHA-256 of the SubjectPublicKeyInfo, what the flag takes).
    if ! openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 2 \
         -keyout "$dir/key.pem" -out "$dir/cert.pem" -subj /CN=www.jam \
         -addext subjectAltName=DNS:www.jam,DNS:mac.jam > "$dir/openssl.out" 2>&1; then
        cat "$dir/openssl.out"
        echo "FAIL (openssl could not make the certificate, see $dir/openssl.out)" > "$dir/result"
        return
    fi
    spki=$(openssl x509 -in "$dir/cert.pem" -pubkey -noout | openssl pkey -pubin -outform der |
           openssl dgst -sha256 -binary | openssl base64 -A)
    if [ ${#spki} != 44 ]; then
        echo "FAIL (the certificate's SPKI hash is '$spki', not 44 characters)" > "$dir/result"
        return
    fi
    say "headless: the test certificate's SPKI hash is $spki"
    sed "s|@SPKI@|$spki|" tools/cr/headless.txt > "$dir/headless.txt"
    # The shell cuts a command line at 240 characters (USING.md): a longer
    # line would run something else.
    if awk '/^send / && length($0) > 245 { print "cr-test: headless: line too long (" length($0) - 5 "): " $0; bad = 1 } END { exit bad }' "$dir/headless.txt"; then
        :
    else
        echo "FAIL (a send line of tools/cr/headless.txt is over the shell's 240 characters)" > "$dir/result"
        return
    fi
    stick=$dir/headless-stick.img
    if ! python3 tools/cr/base-stick.py --data-mib 2048 --programs headless_shell \
         --program-dir "$srcdir/out/jamos" --out-files "$(echo $paks | tr ' ' ',')" --source-files "" \
         --add "$dir/add" "$image" "$stick" "$srcdir/out/jamos" "$srcdir" "$srcdir/out/jamos" \
         > "$dir/stick.out" 2>&1; then
        cat "$dir/stick.out"
        echo "FAIL (the stick, see $dir/stick.out)" > "$dir/result"
        return
    fi
    sed 's/^/cr-test: /' "$dir/stick.out"
    rm -rf "$dir/add"
    # the guest's reach: 8000 is the HTTP server, 8443 the HTTPS one (the
    # pages name those ports)
    # shellcheck disable=SC2046 # two words, the ports
    set -- $(python3 tools/netpeer.py --free-ports 2)
    python3 tools/cr/headless-server.py "$dir/web" "$1" "$dir/http-server.log" > "$dir/http-server.out" 2>&1 &
    hpid=$!
    python3 tools/cr/headless-server.py "$dir/web" "$2" "$dir/https-server.log" --tls "$dir/cert.pem" "$dir/key.pem" \
        > "$dir/https-server.out" 2>&1 &
    spid=$!
    QEMU_SAVE=$dir/saved.img QEMU_IMAGE=$stick QEMU_INPUT=$dir/headless.txt QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} \
        QEMU_MEM=${QEMU_MEM:-4G} QEMU_NET=1 QEMU_NET_PEER="--tcp-relay 8000:$1,8443:$2" \
        tools/qemu-test.sh "$dir/run" headless shell > "$dir/qemu.out" 2>&1
    qrc=$?
    kill "$hpid" "$spid" 2> /dev/null
    rm -f "$stick" "$dir/key.pem"
    # what the guest wrote, out of the saved stick
    for f in dom-local.html dom-http.html dom-https.html dom-nocert.html local.png http.png https.png; do
        build/host/jamext -q --part 2 get "$dir/saved.img" "/cr/tmp/$f" "$dir/$f" > /dev/null 2>&1 ||
            say "headless: no $f on the stick"
    done
    rm -f "$dir/saved.img"
    if [ $qrc -ne 0 ]; then
        echo "FAIL (the script: $(grep 'serial-feed: .*: no ' "$dir/qemu.out" | tail -1 | sed 's/^serial-feed: //'), see $dir/run/)" > "$dir/result"
        return
    fi
    if python3 tools/cr/headless-check.py "$dir" > "$dir/check.out" 2>&1; then
        echo PASS > "$dir/result"
    else
        echo "FAIL (see $dir/check.out)" > "$dir/result"
    fi
    sed 's/^/cr-test: /' "$dir/check.out"
}

case_multi() {
    dir=$out/multi
    rm -rf "$dir"
    mkdir -p "$dir"
    # CR_MULTI_ARGS: one more argument for content_shell ("--v=1"), on its run
    # line; a script with `ps -t <pid>` lines (a hang's threads) gets them
    # named in <dir>/threads.txt.
    sed "s|@ARGS@|${CR_MULTI_ARGS:---v=0}|" tools/cr/multi.txt > "$dir/multi.txt"
    shell_run "$dir" tools/cr/multi.html "$dir/multi.txt" multi "${CR_MULTI_MEM:-6G}" 600
    qrc=$?
    [ $qrc = 2 ] && [ -f "$dir/result" ] && return
    grep -a -o 'cb5: [^"]*' "$dir/run/multi.log" 2> /dev/null | uniq | sed 's/^/cr-test: page said: /'
    if grep -a -q "threads: process" "$dir/run/multi.log" 2> /dev/null; then
        python3 tools/symbolize-threads.py "$dir/run/multi.log" "$srcdir/out/jamos/content_shell" \
            > "$dir/threads.txt" 2>&1
    fi
    if [ $qrc -ne 0 ]; then
        echo "FAIL (the script: $(grep 'serial-feed: .*: no ' "$dir/qemu.out" | tail -1 | sed 's/^serial-feed: //'), see $dir/run/)" > "$dir/result"
        return
    fi
    if python3 tools/cr/multi-check.py "$dir/run" > "$dir/check.out" 2>&1; then
        echo PASS > "$dir/result"
    else
        echo "FAIL (the checks, see $dir/check.out)" > "$dir/result"
    fi
    sed 's/^/cr-test: /' "$dir/check.out"
}

case_jit() {
    dir=$out/jit
    rm -rf "$dir"
    mkdir -p "$dir"
    shell_run "$dir" tools/cr/jit.html tools/cr/jit.txt jit "${CR_MULTI_MEM:-6G}" 900
    qrc=$?
    [ $qrc = 2 ] && [ -f "$dir/result" ] && return
    grep -a -o 'crjit: [^"]*' "$dir/run/jit.log" 2> /dev/null | uniq | sed 's/^/cr-test: page said: /'
    if [ $qrc -ne 0 ]; then
        echo "FAIL (the script: $(grep 'serial-feed: .*: no ' "$dir/qemu.out" | tail -1 | sed 's/^serial-feed: //'), see $dir/run/)" > "$dir/result"
        return
    fi
    if python3 tools/cr/jit-check.py "$dir/run" > "$dir/check.out" 2>&1; then
        echo PASS > "$dir/result"
    else
        echo "FAIL (the checks, see $dir/check.out)" > "$dir/result"
    fi
    sed 's/^/cr-test: /' "$dir/check.out"
}

case_webgl() {
    dir=$out/webgl
    rm -rf "$dir"
    mkdir -p "$dir"
    shell_run "$dir" tools/cr/webgl.html tools/cr/webgl.txt webgl "${CR_MULTI_MEM:-6G}" 600
    qrc=$?
    [ $qrc = 2 ] && [ -f "$dir/result" ] && return
    grep -a -o 'crgl: [^"]*' "$dir/run/webgl.log" 2> /dev/null | uniq | sed 's/^/cr-test: page said: /'
    if [ $qrc -ne 0 ]; then
        echo "FAIL (the script: $(grep 'serial-feed: .*: no ' "$dir/qemu.out" | tail -1 | sed 's/^serial-feed: //'), see $dir/run/)" > "$dir/result"
        return
    fi
    if python3 tools/cr/webgl-check.py "$dir/run" > "$dir/check.out" 2>&1; then
        echo PASS > "$dir/result"
    else
        echo "FAIL (the checks, see $dir/check.out)" > "$dir/result"
    fi
    sed 's/^/cr-test: /' "$dir/check.out"
}

# bench: the benchmarks, served with the reporter (bench-server.py).
serve_bench() {
    python3 tools/cr/bench-server.py "$bench_js" "$bench_sp" "$1" "$dir/bench.log"
}

case_bench() {
    dir=$out/bench
    rm -rf "$dir"
    mkdir -p "$dir"
    runs=${CR_BENCH_RUNS:-jetstream-js:jit jetstream-js:jitless jetstream-wasm:jit speedometer:jit speedometer:jitless}
    if ! tools/cr/toolchain.sh jetstream speedometer > "$dir/fetch.log" 2>&1; then
        echo "FAIL (the benchmarks' fetch, see $dir/fetch.log)" > "$dir/result"
        return
    fi
    bench_js=$(tools/cr/toolchain.sh --dir jetstream)/JetStream-$(awk '$1 == "jetstream" { print $2 }' tools/cr/toolchain.pins)
    bench_sp=$(tools/cr/toolchain.sh --dir speedometer)/Speedometer-$(awk '$1 == "speedometer" { print $2 }' tools/cr/toolchain.pins)
    # The script: jit.txt's start, then each run the way jit.txt runs one.
    {
        sed -n '/^wait 120 Jam OS shell/,/^send H=/p' tools/cr/jit.txt
        echo "wait {prompt}"
        n=0
        for r in $runs; do
            n=$((n + 1))
            name=${r%%:*}
            flags=
            [ "${r#*:}" = jitless ] && flags=--js-flags=--jitless
            echo "send run \$H --no-sandbox --no-zygote --disable-gpu --enable-logging=stderr $flags http://www.jam:8000/run/$name &"
            echo "wait 5400 crbench: done"
            echo "sleep 2"
            echo "shot bench-$n-$name-${r#*:}"
            printf '%s\n' "pointer 1000 600" "monitor mouse_button 1" "monitor mouse_button 0" "sleep 1" \
                "monitor sendkey meta_l-q" "sleep 20" "pointer 300 400" "monitor mouse_button 1" \
                "monitor mouse_button 0" "sleep 1" "send jobs" "wait {prompt}"
        done
        printf '%s\n' "send reboot -f" "wait 60 reboot: resetting"
    } > "$dir/bench.txt"
    serve=serve_bench shell_run "$dir" tools/cr/jit.html "$dir/bench.txt" bench "${CR_MULTI_MEM:-6G}" \
        "${CR_BENCH_TIMEOUT:-14400}"
    qrc=$?
    [ $qrc = 2 ] && [ -f "$dir/result" ] && return
    grep -a -o 'crbench: [^"]*' "$dir/run/bench.log" 2> /dev/null | uniq | sed 's/^/cr-test: page said: /'
    # shellcheck disable=SC2086 # the runs are words
    python3 tools/cr/bench-check.py "$dir/run/bench.log" $runs > "$dir/check.out" 2>&1
    crc=$?
    if [ $qrc -ne 0 ]; then
        echo "FAIL (the script: $(grep 'serial-feed: .*: no ' "$dir/qemu.out" | tail -1 | sed 's/^serial-feed: //'), see $dir/run/)" > "$dir/result"
    elif [ $crc -eq 0 ]; then
        echo PASS > "$dir/result"
    else
        echo "FAIL (the scores, see $dir/check.out)" > "$dir/result"
    fi
    sed 's/^/cr-test: /' "$dir/check.out"
}

for c in "$@"; do
    case $c in
    hello) case_hello ;;
    rust) case_rust ;;
    base) case_base ;;
    shell-wl) case_shell_wl ;;
    headless) case_headless ;;
    multi) case_multi ;;
    jit) case_jit ;;
    webgl) case_webgl ;;
    bench) case_bench ;;
    *) echo "cr-test: no case $c (hello, rust, base, shell-wl, headless, multi, jit, bench)" >&2
       mkdir -p "$out/$c"
       echo "FAIL (no such case)" > "$out/$c/result" ;;
    esac
    r=$(cat "$out/$c/result" 2> /dev/null || echo "FAIL (no result)")
    say "$c: $r"
    [ "$r" = PASS ] || rc=1
done
exit $rc
