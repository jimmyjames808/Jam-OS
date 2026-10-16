#!/bin/sh
# `pkg` end to end in QEMU (docs/M13.7-PLAN.md sections 8 and 9, section 14):
# the shell's command, bin/pkg in the guest, init's approvals, the journal.
# The repositories are tools/pkg-test-repo.py's (small signed packages made
# with the ports builder's writers), served over HTTP on the test host and
# joined by the network peer's TCP relay (the guest connects to
# 10.2.21.174:8000), as tools/fetch-test.sh does. The image's /boot/ports.pub
# is the ports vectors' public test key (its secret is public: no real image
# has it); the stick's pkg.repo is the "good" repository.
#   1. pkgmain    tools/shell-tests/pkg-main.txt: nothing installed; every
#                 defect refused with nothing left (a byte changed in a
#                 package, another key's manifest, a file the archive lacks,
#                 a program asking for what none may have, an index changed after
#                 it was signed, an index and a package built for another ABI
#                 stamp: the repositories sign for this tree's stamp, which the
#                 image's /boot/build.txt has); install of bbb, which runs aaa, one question
#                 for both; list, check, files, info, allow -l with the package
#                 shown, allow -r refused over a package's program, remove
#                 refused while something runs it; an index from the past
#                 refused; the audit; an upgrade (a file gone, one added, the old
#                 approvals revoked); the same version rebuilt, newer by its
#                 hash; remove -r; installed again
#   2. pkgcold    the stick from cold: what was installed is there and checks
#                 out; then ccc from a repository whose packages stall
#                 halfway, and QEMU is quit during the download (a power cut with
#                 the journal at its first step and the package half staged)
#   3. pkgsettle  the stick after the cut: init's settle undid the install
#                 (its log line), pkg check passes; the host then reads the data
#                 partition: nothing of ccc, no journal, no `.new`
# Usage: tools/pkg-test.sh <outdir> (after `make -s image`); exit 0 on PASS.
set -u
DATA_AT=$(python3 tools/mkimage.py --data-at build/jamos.img) || exit 1   # /data in the image: mtools' @@offset
out=$1
mkdir -p "$out"
fails=0
fail() {
    echo "pkg-test: FAILED: $1"
    fails=$((fails + 1))
}

# The image: the test key's public half as /boot/ports.pub, and the stick's settings.
base=${QEMU_IMAGE:-build/jamos.img}
img="$out/pkg.base.img"
rm -f "$out/bootfs-in.img"
mcopy -i "$base@@1M" ::/boot/bootfs.img "$out/bootfs-in.img" ||
    { echo "pkg-test: can't read the boot image"; exit 1; }
python3 tools/bootfs-edit.py "$out/bootfs-in.img" "$out/bootfs-key.img" \
    "ports.pub=ports/vectors/test.pub" || { echo "pkg-test: can't add the key"; exit 1; }
cp "$base" "$img"
mcopy -o -i "$img@@1M" "$out/bootfs-key.img" ::/boot/bootfs.img ||
    { echo "pkg-test: can't write the boot image"; exit 1; }
rm -f "$out/bootfs-in.img" "$out/bootfs-key.img"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\npkg.repo = http://10.2.21.174:8000/good/\n' \
    > "$out/pkg.settings"
mmd -i "$img@@$DATA_AT" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@$DATA_AT" "$out/pkg.settings" ::/etc/settings ||
    { echo "pkg-test: can't write the stick's settings"; exit 1; }

# The repositories, served on a free port.
python3 tools/pkg-test-repo.py serve "$out/repo" --port 0 > "$out/pkg.http.log" 2>&1 &
hpid=$!
trap 'kill $hpid 2>/dev/null || true' EXIT
j=0
while [ $j -lt 100 ] && ! grep -q "serving on" "$out/pkg.http.log"; do sleep 0.1; j=$((j + 1)); done
hport=$(sed -n 's/^serving on //p' "$out/pkg.http.log")
[ -n "$hport" ] || { echo "pkg-test: the repository server didn't start"; cat "$out/pkg.http.log"; exit 1; }

# One QEMU run: image, script, name, saved image.
guest() {
    QEMU_IMAGE=$1 QEMU_NET=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-360} QEMU_SAVE=$4 \
        QEMU_NET_PEER="--tcp-relay 8000:$hport" QEMU_INPUT=tools/shell-tests/$2 \
        tools/qemu-test.sh "$out" "$3" shell
}
if guest "$img" pkg-main.txt pkgmain "$out/pkg.main.img"; then
    guest "$out/pkg.main.img" pkg-cold.txt pkgcold "$out/pkg.cut.img" ||
        fail "the cold boot, or the download that was cut (see $out/pkgcold.log)"
else
    fail "the first boot (see $out/pkgmain.log)"
fi
if [ -s "$out/pkg.cut.img" ]; then
    guest "$out/pkg.cut.img" pkg-settle.txt pkgsettle "$out/pkg.after.img" ||
        fail "the boot after the cut (see $out/pkgsettle.log)"
    grep -aq "init: pkg: settle: install ccc undone" "$out/pkgsettle.log" ||
        fail "init did not undo the install that was cut (no settle line in pkgsettle.log)"
    # The data partition, read on the host: aaa and bbb, nothing of ccc, no journal.
    rm -rf "$out/pkgfiles"
    mkdir -p "$out/pkgfiles"
    for d in pkg/bin pkg/db; do
        mdir -b -i "$out/pkg.after.img@@$DATA_AT" "::/$d" > "$out/pkgfiles/$(echo "$d" | tr / _).txt" 2>&1 ||
            fail "can't list /data/$d"
    done
    for want in bin/aaa bin/bbb; do
        grep -aiq "$(basename "$want")" "$out/pkgfiles/pkg_bin.txt" || fail "/data/pkg/$want is gone"
    done
    if grep -aiq "ccc\|\.new\|journal" "$out/pkgfiles/pkg_bin.txt" "$out/pkgfiles/pkg_db.txt"; then
        fail "something of the cut install is left (see $out/pkgfiles)"
    fi
fi
grep -a "init: pkg: " "$out"/pkgmain.log "$out"/pkgsettle.log 2>/dev/null | sed 's/^/pkg-test: /' | head -30
rm -f "$out/pkg.base.img" "$out/pkg.main.img" "$out/pkg.cut.img" "$out/pkg.after.img"
if [ $fails -eq 0 ]; then
    echo "pkg-test: PASS"
    exit 0
fi
echo "pkg-test: FAIL ($fails)"
exit 1
