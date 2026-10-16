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
#           stick with a 640 MiB /data (tools/cr/base-stick.py: no package,
#           the format holds no file over 64 MiB), run suite by suite with
#           the ENOSYS log on (tools/cr/base-run.py); the tests in
#           tools/cr/base_unittests.skip are not run. CR_BASE_SHARD=i/n,
#           CR_BASE_ALL=1, CR_BASE_ONLY=<glob>, CR_BASE_ENV="NAME=value ..."
#           (default TMPDIR and TEST_TMPDIR=/data/cr/tmp; CR_BASE_ENV= empty
#           leaves both unset and the programs use init's /tmp); the stick's
#           /data is the image's: ext, or FAT when JAMOS_DATA_FS=fat made the
#           image (tools/cr/base-stick.py fills an ext one with jamext;
#           QEMU_BOOT_NVME=1 boots the stick as an installed NVMe drive); results in
#           <outdir>/base/run/ (results.txt, failures/, enosys.txt, summary.txt)
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
    if ! python3 tools/cr/base-stick.py --data-mib "${CR_BASE_DATA_MIB:-640}" "$image" "$stick" \
         "$stage" "$srcdir" "$srcdir/out/jamos" > "$dir/stick.out" 2>&1; then
        cat "$dir/stick.out"
        echo "FAIL (the stick, see $dir/stick.out)" > "$dir/result"
        return
    fi
    sed 's/^/cr-test: /' "$dir/stick.out"
    # CR_BASE_ENV: more shell variables for the program ("TMPDIR=/data/tmp"),
    # CR_BASE_ONLY: only suites matching a glob, CR_BASE_ARGS: more arguments
    # for base_unittests ("--v=1"), CR_BASE_ALL=1: the skip list's tests run
    # too (a failure of one is a known failure).
    extra=
    for e in ${CR_BASE_ENV-TMPDIR=/data/cr/tmp TEST_TMPDIR=/data/cr/tmp}; do extra="$extra --env $e"; done
    # shellcheck disable=SC2086
    if python3 tools/cr/base-run.py --shard "${CR_BASE_SHARD:-0/1}" --mem "${QEMU_MEM:-4G}" \
       ${CR_BASE_ALL:+--all} ${CR_BASE_ONLY:+--only "$CR_BASE_ONLY"} ${CR_BASE_ARGS:+--args " $CR_BASE_ARGS"} $extra \
       "$dir/run" "$stick" > "$dir/base-run.out" 2>&1; then
        echo PASS > "$dir/result"
    else
        echo "FAIL ($(grep 'base-run: [0-9]* new failures' "$dir/base-run.out" | sed 's/^base-run: //'), see $dir/run/)" > "$dir/result"
    fi
    sed 's/^/cr-test: /' "$dir/base-run.out"
    rm -f "$stick"
}

for c in "$@"; do
    case $c in
    hello) case_hello ;;
    rust) case_rust ;;
    base) case_base ;;
    *) echo "cr-test: no case $c (hello, rust, base)" >&2
       mkdir -p "$out/$c"
       echo "FAIL (no such case)" > "$out/$c/result" ;;
    esac
    r=$(cat "$out/$c/result" 2> /dev/null || echo "FAIL (no result)")
    say "$c: $r"
    [ "$r" = PASS ] || rc=1
done
exit $rc
