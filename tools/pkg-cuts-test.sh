#!/bin/sh
# bin/pkg's journal cut at each of its change points, in QEMU (docs/M13.7-PLAN.md
# section 9; PO8 part 2, built by PO14). tools/pkg-test.sh has the end-to-end run
# and one cut, in a download; this one stops the machine, as a power cut would,
# with the journal at EVERY state it passes through while a package is
# installed, upgraded and removed (tools/pkg-cuts.py lists them: each of the
# operation's steps is on the disk three ways, `<step>:new`, `<step>:alone` and
# `<step>`, as the journal is replaced; then `done:new` and `done`: 17, 20 and 14
# points). One chain of boots for each operation on one stick, a boot for each
# point:
#   boot k  init's settle line for what cut k-1 left ("install aaa undone at
#           begin", or "finished at approved": the commit point), `pkg check`
#           clean, the package there or not as it must be (files, approvals,
#           record), no journal, no `.new`; then the starting state brought
#           back; then `pkg -H <point> <op> aaa`, which stands still at the
#           point and says "pkg: held at <point>", and QEMU is quit.
#   the last boot only checks.
# The package is the smallest of tools/pkg-test-repo.py's: aaa, 1.0 in `good`,
# 2.0 in `next` (the starting state of an upgrade is 1.0, brought back with
# `pkg update -f` since the index of `next` is the newer).
#
#   tools/pkg-cuts-test.sh <outdir> [install|upgrade|remove]...   (all three)
#
# Needs `make -s image`; PC only (QEMU). The image's /boot/ports.pub is the ports
# vectors' test key, its settings point `pkg.repo` at `good` through the network
# peer's TCP relay (10.2.21.174:8000), as tools/pkg-test.sh has it. Prints
# "pkg-cuts-test: <op>: PASS" or FAIL (the point, and the log) for each; exit 0
# if all passed.
set -u
if [ $# -lt 1 ]; then
    echo "usage: tools/pkg-cuts-test.sh <outdir> [install|upgrade|remove]..." >&2
    exit 2
fi
out=$1
shift
ops=${*:-install upgrade remove}
for op in $ops; do
    case $op in install | upgrade | remove) ;; *) echo "pkg-cuts-test: $op: not an operation" >&2; exit 2 ;; esac
done
mkdir -p "$out"
base=${QEMU_IMAGE:-build/jamos.img}
[ -f "$base" ] || { echo "pkg-cuts-test: $base: no image (make image first)" >&2; exit 2; }
repo=http://10.2.21.174:8000

# The image: the test key's public half as /boot/ports.pub, and the settings.
img="$out/cuts.base.img"
rm -f "$out/bootfs-in.img"
mcopy -i "$base@@1M" ::/boot/bootfs.img "$out/bootfs-in.img" ||
    { echo "pkg-cuts-test: can't read the boot image"; exit 1; }
python3 tools/bootfs-edit.py "$out/bootfs-in.img" "$out/bootfs-key.img" \
    "ports.pub=ports/vectors/test.pub" || { echo "pkg-cuts-test: can't add the key"; exit 1; }
cp "$base" "$img"
mcopy -o -i "$img@@1M" "$out/bootfs-key.img" ::/boot/bootfs.img ||
    { echo "pkg-cuts-test: can't write the boot image"; exit 1; }
rm -f "$out/bootfs-in.img" "$out/bootfs-key.img"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\npkg.repo = %s/good/\n' "$repo" \
    > "$out/pkg.settings"
if [ "$(python3 tools/mkimage.py --data-fs "$img")" = ext ]; then
    make -s build/host/jamext > "$out/jamext-make.log" 2>&1 ||
        { echo "pkg-cuts-test: can't make build/host/jamext (see $out/jamext-make.log)"; exit 1; }
    rm -rf "$out/seed"
    mkdir -p "$out/seed/etc"
    cp "$out/pkg.settings" "$out/seed/etc/settings"
    build/host/jamext -q --part 2 import "$img" "$out/seed" ||
        { echo "pkg-cuts-test: can't write the stick's settings"; exit 1; }
    ext=1
else
    DATA_AT=$(python3 tools/mkimage.py --data-at "$img") || exit 1
    mmd -i "$img@@$DATA_AT" ::/etc 2> /dev/null || true
    mcopy -o -i "$img@@$DATA_AT" "$out/pkg.settings" ::/etc/settings ||
        { echo "pkg-cuts-test: can't write the stick's settings"; exit 1; }
    ext=0
fi

# The repositories, served on a free port.
python3 tools/pkg-test-repo.py serve "$out/repo" --port 0 > "$out/pkg.http.log" 2>&1 &
hpid=$!
trap 'kill $hpid 2>/dev/null || true' EXIT
j=0
while [ $j -lt 100 ] && ! grep -q "serving on" "$out/pkg.http.log"; do sleep 0.1; j=$((j + 1)); done
hport=$(sed -n 's/^serving on //p' "$out/pkg.http.log")
[ -n "$hport" ] || { echo "pkg-cuts-test: the repository server didn't start"; cat "$out/pkg.http.log"; exit 1; }

# One boot: image, script, name, the image it leaves.
guest() {
    QEMU_IMAGE=$1 QEMU_NET=1 QEMU_TIMEOUT=${PKG_CUTS_TIMEOUT:-240} QEMU_SAVE=$4 \
        QEMU_NET_PEER="--tcp-relay 8000:$hport" QEMU_INPUT=$2 \
        tools/qemu-test.sh "$out" "$3" shell
}

fails=0
for op in $ops; do
    points=$(python3 tools/pkg-cuts.py points "$op") || exit 1
    n=$(echo "$points" | wc -l | tr -d ' ')
    cur="$out/cuts.$op.0.img"
    cp "$img" "$cur"
    k=0
    why=
    while [ $k -le "$n" ]; do
        point=$(echo "$points" | sed -n "$((k + 1))p")
        script="$out/cuts-$op-$k.txt"
        python3 tools/pkg-cuts.py script "$op" "$k" "$repo" > "$script" || { why="can't make the script"; break; }
        next="$out/cuts.$op.$((k + 1)).img"
        if ! guest "$cur" "$script" "cut-$op-$k" "$next" > "$out/cut-$op-$k.out" 2>&1; then
            if [ $k -eq 0 ]; then
                why="the first boot, cut at $point (see $out/cut-$op-$k.log)"
            elif [ $k -eq "$n" ]; then
                why="the last check, after the cut at $(echo "$points" | sed -n "${k}p") (see $out/cut-$op-$k.log)"
            else
                why="the check after the cut at $(echo "$points" | sed -n "${k}p"), or the cut at $point (see $out/cut-$op-$k.log)"
            fi
            break
        fi
        rm -f "$cur"
        cur=$next
        k=$((k + 1))
    done
    if [ -z "$why" ] && [ $ext = 1 ]; then   # the volume the cuts left
        build/host/jamext check --replay --part 2 "$cur" > "$out/cuts-$op-check.txt" 2>&1 ||
            why="the stick's ext volume isn't clean after the last boot (see $out/cuts-$op-check.txt)"
    fi
    rm -f "$out"/cuts.$op.*.img
    if [ -z "$why" ]; then
        echo "pkg-cuts-test: $op: PASS ($n points)"
    else
        echo "pkg-cuts-test: $op: FAIL ($why)"
        fails=$((fails + 1))
    fi
done
rm -f "$img"
if [ $fails -eq 0 ]; then
    echo "pkg-cuts-test: PASS"
    exit 0
fi
echo "pkg-cuts-test: FAIL ($fails)"
exit 1
