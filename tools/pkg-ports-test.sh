#!/bin/sh
# The first real packages through `pkg` in QEMU (docs/M13.7-PLAN.md, PO9): the
# libarchive chain (zlib, bzip2, xz and libarchive, built by tools/ports.py and
# signed with the ports vectors' public test key, whose public half is the
# image's /boot/ports.pub as in tools/pkg-test.sh) installed from a repository
# the test host serves, not preinstalled as tools/ports-test.sh does:
#   tools/shell-tests/pkg-ports.txt: `pkg install libarchive` asks once about
#   four packages and installs the three it runs first; every program (the
#   multi-call names too) is approved and runs; a -test subpackage is not in the
#   repository; a package that runs another can't leave it removed; `remove -r`
#   and an install of the missing part again; a file that another package has
#   is refused before the question; `pkg check` throughout.
# The repositories are tools/pkg-test-repo.py's (--ports libarchive), reached
# over the network peer's TCP relay as tools/pkg-test.sh does.
# Usage: tools/pkg-ports-test.sh <outdir> (after `make -s image`); exit 0 on PASS.
set -u
. tools/datafs.sh
out=$1
mkdir -p "$out"

base=${QEMU_IMAGE:-build/jamos.img}
img="$out/pkgports-stick.img"
rm -f "$out/bootfs-in.img"
mcopy -i "$base@@1M" ::/boot/bootfs.img "$out/bootfs-in.img" ||
    { echo "pkg-ports-test: can't read the boot image"; exit 1; }
python3 tools/bootfs-edit.py "$out/bootfs-in.img" "$out/bootfs-key.img" \
    "ports.pub=ports/vectors/test.pub" || { echo "pkg-ports-test: can't add the key"; exit 1; }
cp "$base" "$img"
mcopy -o -i "$img@@1M" "$out/bootfs-key.img" ::/boot/bootfs.img ||
    { echo "pkg-ports-test: can't write the boot image"; exit 1; }
rm -f "$out/bootfs-in.img" "$out/bootfs-key.img"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\npkg.repo = http://10.2.21.174:8000/real/\n' \
    > "$out/pkgports.settings"
data_put "$img" "$out/pkgports.settings" etc/settings ||
    { echo "pkg-ports-test: can't write the stick's settings"; exit 1; }

# The repositories (the real packages are built here), served on a free port.
python3 tools/pkg-test-repo.py serve "$out/repo" --port 0 --ports libarchive \
    > "$out/pkgports.http.log" 2>&1 &
hpid=$!
trap 'kill $hpid 2>/dev/null || true' EXIT
j=0
while [ $j -lt 3000 ] && kill -0 $hpid 2>/dev/null && ! grep -q "serving on" "$out/pkgports.http.log"; do
    sleep 0.1
    j=$((j + 1))
done
hport=$(sed -n 's/^serving on //p' "$out/pkgports.http.log")
[ -n "$hport" ] || { echo "pkg-ports-test: the repository server didn't start"; tail -5 "$out/pkgports.http.log"; exit 1; }

if QEMU_IMAGE="$img" QEMU_NET=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-420} \
   QEMU_NET_PEER="--tcp-relay 8000:$hport" QEMU_INPUT=tools/shell-tests/pkg-ports.txt \
   tools/qemu-test.sh "$out" pkgports shell; then
    rm -f "$img"
    echo "pkg-ports-test: PASS"
    exit 0
fi
echo "pkg-ports-test: FAIL (see $out/pkgports.log)"
exit 1
