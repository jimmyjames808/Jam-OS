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
# Cases (milestone CR-a now; CT2 adds `rust`, the CB tracks theirs):
#   hello   ports/cr-hello, the C++ program (track CT1): built by
#           tools/jamos-clang++ with LLVM's runtimes (ports/llvm-runtimes,
#           ports/linux-uapi); the syscall scan on it (tools/cr/syscall-scan.py);
#           the same source built by the build host's own c++ prints
#           ports/cr-hello/files/expected.txt; then on Jam OS from /data it
#           prints the same, byte for byte (ports/cr-hello/test)
#
# The toolchain is fetched first if it isn't there (tools/cr/toolchain.sh:
# clang, cmake). The ports build at low priority with CR_JOBS jobs (8;
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

for c in "$@"; do
    case $c in
    hello) case_hello ;;
    *) echo "cr-test: no case $c (hello)" >&2
       mkdir -p "$out/$c"
       echo "FAIL (no such case)" > "$out/$c/result" ;;
    esac
    r=$(cat "$out/$c/result" 2> /dev/null || echo "FAIL (no result)")
    say "$c: $r"
    [ "$r" = PASS ] || rc=1
done
exit $rc
