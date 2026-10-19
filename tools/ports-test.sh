#!/bin/sh
# Test ported programs on Jam OS (docs/M13.7-PLAN.md section 14, PO8): each
# named port is built (tools/ports.py), preinstalled into a copy of the
# stick image's data partition from this side (tools/ports-preinstall.py:
# mtools, the files under /data/pkg, their signed manifests in db/, the
# approval lines in /data/etc/allow and the page tables, as `pkg` and init
# will have made them; a test image only), booted in QEMU, and the port's own
# test run through the serial shell. The image's /boot/ports.pub is the ports
# vectors' public test key (as tools/pkg-test.sh has it: its secret is public,
# no real image carries it), which signed the manifests, so `pkg list`, `pkg
# info` and `pkg check` in the guest see the packages as installed.
#
#   tools/ports-test.sh <outdir> <port>...
#
# A port's test is ports/<port>/test: a script in the shell tests' format
# (tools/serial-feed.py; the examples are in tools/shell-tests/). It starts
# at the shell's prompt with
#       T=x                     the letter the test builds its answers from
#       PK=/pkg                 the packages' prefix as a program sees it: /pkg,
#                               init's read-only view, when the image's /data is
#                               ext; /data/pkg (where they are) when it is FAT,
#                               which has no views (a program that reads its
#                               package's files asks for `mount /pkg r`, or has
#                               `mount /data r` beside it for this case)
#       P=$PK/bin               where the port's programs are (PATH has /pkg/bin,
#                               after /boot/bin and /boot/usr/bin: a program of a
#                               name the boot image has too is run as
#                               `run $P/<program>`)
#       S=/boot/usr/bin         sbase's tools: the shell's own commands (seq,
#                               cat, sort, ...) win by name and only POSIX
#                               programs take < and >, so `run $S/seq 1 9 > f`
#       the current directory /data
# and ends at a prompt. It passes when every `wait` matched. Text a test
# waits for should be built from $T, so it never appears in the typed line
# (`... && echo ok-$T-gz` then `wait ok-x-gz`). The ports a port `runs` are
# installed with it; its `-test` subpackage (a test program, never in the
# repository) is a `file` of the same stage, so it is installed too.
#
# One QEMU run per port, each in <outdir>/<port>/ (so a failing port's
# serial log, screenshot and image stay there); ports run side by side,
# PORTS_TEST_JOBS (default 3) at a time. Prints "ports-test: <port>: PASS"
# or "FAIL" per port; exit 0 if all passed.
#
# Environment: PORTS_TEST_JOBS, PORTS_TEST_IMAGE (default build/jamos.img:
# `make image` first, which tools/remote-test.sh does), PORTS_TEST_SCRIPT (a
# script, run after the same start in place of each port's own `test`: the
# shell's `pkg` against the installed packages, tools/shell-tests/pkg.txt),
# PORTS_TEST_DATA_MIB (each stick's /data made this many MiB before the
# packages go in: the image's own is 64, too small for fonts-noto-extra's 45 MB
# and fonts-jamos's 34 MB together; fonts-noto-extra's test wants 256),
# QEMU_TIMEOUT (here default 300), and the tools/qemu-test.sh variables. Build output is in
# build/ports. On the test host:
#   JAMOS_REMOTE=jamos-pc JAMOS_REMOTE_ONLY=1 tools/remote-test.sh \
#       tools/ports-test.sh build/test/ports zlib xz
# (one remote-test call per port takes a slot of the machine for each, as
# the other area tests do: use that for many ports at once.)
set -u
. tools/datafs.sh
if [ $# -lt 2 ]; then
    echo "usage: tools/ports-test.sh <outdir> <port>..." >&2
    exit 2
fi
out=$1
shift
ports="$*"
image=${PORTS_TEST_IMAGE:-build/jamos.img}
jobs=${PORTS_TEST_JOBS:-3}
export QEMU_TIMEOUT=${QEMU_TIMEOUT:-300}

[ -f "$image" ] || { echo "ports-test: $image: no image (make image first)" >&2; exit 2; }
for p in $ports; do
    [ -f "ports/$p/recipe" ] || { echo "ports-test: $p: no such port" >&2; exit 2; }
    [ -f "${PORTS_TEST_SCRIPT:-ports/$p/test}" ] ||
        { echo "ports-test: $p: no ${PORTS_TEST_SCRIPT:-ports/$p/test}" >&2; exit 2; }
done
mkdir -p "$out"

# What a test needs installed (the named ports and what they run), built.
all=$(python3 tools/ports-preinstall.py closure $ports) || exit 2
if ! python3 tools/ports.py --out build/ports build $all > "$out/build.log" 2>&1; then
    tail -20 "$out/build.log"
    for p in $ports; do echo "ports-test: $p: FAIL (the build, see $out/build.log)"; done
    exit 1
fi
grep -E "built in|up to date" "$out/build.log" | sed 's/^/ports-test: /'
true

# One shard: a port's image, script and QEMU run; leaves $dir/result.
shard() {
    p=$1
    dir=$out/$p
    rm -rf "$dir"
    mkdir -p "$dir"
    img=$dir/ports-$p-stick.img
    fail() { echo "FAIL ($1)" > "$dir/result"; }
    # the image with the test key as /boot/ports.pub (tools/pkg-test.sh does the same)
    if ! mcopy -i "$image@@1M" ::/boot/bootfs.img "$dir/bootfs-in.img" ||
       ! python3 tools/bootfs-edit.py "$dir/bootfs-in.img" "$dir/bootfs-key.img" \
            "ports.pub=ports/vectors/test.pub" > "$dir/bootfs-edit.out" 2>&1; then
        fail "the test key: can't read the boot image or add the key (see $dir/bootfs-edit.out)"
        return
    fi
    cp "$image" "$img"
    if [ -n "${PORTS_TEST_DATA_MIB:-}" ] && ! data_grow "$img" "$PORTS_TEST_DATA_MIB" > "$dir/grow.out" 2>&1; then
        fail "growing /data to $PORTS_TEST_DATA_MIB MiB: $(tail -1 "$dir/grow.out")"
        return
    fi
    pk=/data/pkg
    [ "$(python3 tools/mkimage.py --data-fs "$img" 2> /dev/null)" = ext ] && pk=/pkg
    if ! mcopy -o -i "$img@@1M" "$dir/bootfs-key.img" ::/boot/bootfs.img; then
        fail "the test key: can't write the boot image"
        return
    fi
    rm -f "$dir/bootfs-in.img" "$dir/bootfs-key.img"
    if ! python3 tools/ports-preinstall.py install --feed "$dir/allow-check.txt" "$img" $p \
         > "$dir/preinstall.out" 2>&1; then
        fail "the preinstall: $(tail -1 "$dir/preinstall.out")"
        return
    fi
    {
        echo "# generated by tools/ports-test.sh for $p: the header, ports/$p/test, the end"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "seen 60 init: /data mounted"
        [ "$pk" = /pkg ] && echo "seen 60 init: /pkg: a view of /data/pkg, read-only"
        echo "send T=x"
        echo "wait {prompt}"
        echo "send PK=$pk"
        echo "wait {prompt}"
        echo "send P=\$PK/bin"
        echo "wait {prompt}"
        echo "send S=/boot/usr/bin"
        echo "wait {prompt}"
        echo "send cd /data"
        echo "wait {prompt}"
        cat "$dir/allow-check.txt"
        echo "# ---- ${PORTS_TEST_SCRIPT:-ports/$p/test} ----"
        cat "${PORTS_TEST_SCRIPT:-ports/$p/test}"
        echo "# ---- the end ----"
        echo "send echo ports-test-done-\$T"
        echo "wait ports-test-done-x"
        echo "wait {prompt}"
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$dir/script.txt"
    if QEMU_IMAGE="$img" QEMU_INPUT="$dir/script.txt" \
       tools/qemu-test.sh "$dir" "ports-$p" shell > "$dir/qemu.out" 2>&1; then
        echo PASS > "$dir/result"
        rm -f "$img" "$dir/ports-$p.img"   # kept only for a failure
    else
        why=$(grep -E "serial-feed: .*(no '|within|timed out)" "$dir/qemu.out" | head -1)
        [ -n "$why" ] || why=$(grep "serial-feed: " "$dir/qemu.out" | tail -1)
        fail "${why:-QEMU, see $dir/qemu.out and $dir/ports-$p.log}"
    fi
}

# jobs at a time (a plain `wait` for the batch: sh has no `wait -n`)
n=0
for p in $ports; do
    shard "$p" &
    n=$((n + 1))
    if [ "$n" -ge "$jobs" ]; then
        wait
        n=0
    fi
done
wait

rc=0
for p in $ports; do
    r=$(cat "$out/$p/result" 2> /dev/null || echo "FAIL (no result)")
    echo "ports-test: $p: $r"
    [ "$r" = PASS ] || rc=1
done
[ $rc = 0 ] && echo "ports-test: PASS" || echo "ports-test: FAIL"
exit $rc
