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
#                 it was signed); install of bbb, which runs aaa, one question
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
#   4-7. pkgbig, pkgbigcold, pkgbigcut, pkgbigsettle (M13.7's PO13; PKG_TEST_BIG=0
#                 leaves them out): the same with a program as big as Chromium's,
#                 on an emulated NVMe Jam OS drive whose ext volume is /jam (the
#                 packages' prefix there is /jam/pkg): see the comment before
#                 `make_jam` below
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

# The big package's program (a 300 MiB POSIX program, generated, never kept in
# the tree) and the host tool that makes the Jam OS drive's ext volume.
# PKG_TEST_BIG=0 leaves the big cases out.
big=${PKG_TEST_BIG:-1}
bigprog=
if [ "$big" = 1 ]; then
    if make -s build/bigpkg/bigpkg build/host/jamext > "$out/pkgbig-make.log" 2>&1; then
        bigprog=build/bigpkg/bigpkg
    else
        fail "can't make build/bigpkg/bigpkg (see $out/pkgbig-make.log)"
        big=0
    fi
fi

# The repositories, served on a free port.
python3 tools/pkg-test-repo.py serve "$out/repo" --port 0 ${bigprog:+--bigprog "$bigprog"} \
    > "$out/pkg.http.log" 2>&1 &
hpid=$!
trap 'kill $hpid 2>/dev/null || true' EXIT
j=0
while [ $j -lt 100 ] && ! grep -q "serving on" "$out/pkg.http.log"; do sleep 0.1; j=$((j + 1)); done
hport=$(sed -n 's/^serving on //p' "$out/pkg.http.log")
[ -n "$hport" ] || { echo "pkg-test: the repository server didn't start"; cat "$out/pkg.http.log"; exit 1; }

# One QEMU run: image, script (a path), name, saved image; the Jam OS drive
# $nvme (an emulated NVMe drive: /jam), if one is set; QEMU_TIMEOUT for the
# run is $timeout.
nvme=
timeout=${QEMU_TIMEOUT:-360}
guest_in() {
    QEMU_IMAGE=$1 QEMU_NET=1 QEMU_TIMEOUT=$timeout QEMU_SAVE=$4 QEMU_NVME=$nvme \
        QEMU_NET_PEER="--tcp-relay 8000:$hport" QEMU_INPUT=$2 \
        tools/qemu-test.sh "$out" "$3" shell
}
guest() {
    guest_in "$1" tools/shell-tests/$2 "$3" "$4"
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

# A Jam OS drive: a GPT of the Jam OS layout, a FAT ESP and an empty ext volume
# (the data partition: jamext's mkfs), as one image for an emulated NVMe drive.
make_jam() {
    esp="$out/esp.img"
    rm -f "$1" "$esp"
    truncate -s 128M "$esp" &&
        mformat -i "$esp" -T $((128 * 2048)) -F -v JAMOS-ESP :: &&
        python3 tools/mkgpt.py "$1" --layout jamos --size 1536M --fill "1=$esp" > /dev/null &&
        build/host/jamext -q mkfs --part 2 --label jamdata "$1" 1000M
    rc=$?
    rm -f "$esp"
    return $rc
}
# One of the scripts of the big cases with the program's numbers in it.
big_script() {
    sed -e "s/@PAGES@/$big_pages/g" -e "s/@HASH@/$big_hash/g" -e "s/@HASH5@/$big_hash5/g" \
        "tools/shell-tests/$1" > "$out/$1.in"
}

# 4-7. A program as big as Chromium's, on the Jam OS drive's ext volume (PO13).
#   pkgbig       the stick boots, the emulated NVMe drive is /jam: `pkg install bigpkg`
#                (300 MiB: downloaded, unpacked in pieces, read and hashed by init in
#                pieces), listed, run from its pages (its hash is the table's), the
#                guard on /jam/pkg, `pkg check`
#   pkgbigcold   from cold: still installed, runs, `pkg remove` takes it away
#   pkgbigcut    a fresh drive: the same package from the repository whose body stalls
#                halfway, and QEMU is quit during the stall (150 MiB staged on /jam)
#   pkgbigsettle the boot after the cut: init undoes the install (its log line), the drive
#                holds no `.new` and no journal, the package installs whole
# Then the volume on the host: no `.new`, no journal, the volume clean, the program's bytes
# the generated file's.
if [ "$big" = 1 ]; then
    big_pages=${BIGPROG_PAGES:-76800}
    big_hash=$(python3 tools/bigpkg-hash.py "$big_pages" 16)
    big_hash5=$(python3 tools/bigpkg-hash.py "$big_pages" 5)
    for s in pkg-big.txt pkg-bigcold.txt pkg-bigcut.txt pkg-bigsettle.txt; do big_script $s; done
    timeout=${PKG_TEST_BIG_TIMEOUT:-400}
    jam="$out/jam1.img"
    if make_jam "$jam"; then
        nvme="$jam"
        if guest_in "$img" "$out/pkg-big.txt.in" pkgbig "$out/pkg.bigA.img"; then
            guest_in "$out/pkg.bigA.img" "$out/pkg-bigcold.txt.in" pkgbigcold "$out/pkg.bigB.img" ||
                fail "the cold boot with the big package (see $out/pkgbigcold.log)"
        else
            fail "the big package's install (see $out/pkgbig.log)"
        fi
        build/host/jamext check --replay --part 2 "$jam" > "$out/pkgbig-check1.txt" 2>&1 ||
            fail "the Jam OS drive's volume isn't clean after the first two boots (see $out/pkgbig-check1.txt)"
        build/host/jamext ls -lR --part 2 "$jam" /pkg > "$out/pkgbig-ls1.txt" 2>&1
        grep -aq "bigpkg" "$out/pkgbig-ls1.txt" && fail "bigpkg is still on /jam after pkg remove (see $out/pkgbig-ls1.txt)"
    else
        fail "can't make the Jam OS drive"
    fi
    jam="$out/jam2.img"
    if make_jam "$jam"; then
        nvme="$jam"
        guest_in "$img" "$out/pkg-bigcut.txt.in" pkgbigcut "$out/pkg.bigcut.img" ||
            fail "the big package's cut download (see $out/pkgbigcut.log)"
        # The drive as the cut left it, on a copy: half of bigpkg's program under its `.new` name.
        cp --sparse=always "$jam" "$out/jam2.cut.img"
        build/host/jamext check --replay --part 2 "$out/jam2.cut.img" > "$out/pkgbig-check2.txt" 2>&1
        build/host/jamext ls -lR --part 2 "$out/jam2.cut.img" /pkg > "$out/pkgbig-ls2.txt" 2>&1
        grep -aq "bigpkg\.new" "$out/pkgbig-ls2.txt" ||
            fail "the cut left no half-staged bigpkg.new on /jam: the power went at the wrong time (see $out/pkgbig-ls2.txt)"
        rm -f "$out/jam2.cut.img"
        if [ -s "$out/pkg.bigcut.img" ]; then
            guest_in "$out/pkg.bigcut.img" "$out/pkg-bigsettle.txt.in" pkgbigsettle "$out/pkg.bigafter.img" ||
                fail "the boot after the big cut (see $out/pkgbigsettle.log)"
            grep -aq "init: pkg: settle: install bigpkg undone" "$out/pkgbigsettle.log" ||
                fail "init did not undo the big install that was cut (no settle line in pkgbigsettle.log)"
        fi
        build/host/jamext check --replay --part 2 "$jam" > "$out/pkgbig-check3.txt" 2>&1 ||
            fail "the Jam OS drive's volume isn't clean after the cut and the settle (see $out/pkgbig-check3.txt)"
        build/host/jamext ls -lR --part 2 "$jam" /pkg > "$out/pkgbig-ls3.txt" 2>&1
        if grep -aq "\.new\|journal" "$out/pkgbig-ls3.txt"; then
            fail "something of the cut install is left on /jam (see $out/pkgbig-ls3.txt)"
        fi
        rm -f "$out/pkgbig-got"
        if build/host/jamext get --part 2 "$jam" /pkg/bin/bigpkg "$out/pkgbig-got" > /dev/null 2>&1; then
            cmp -s "$out/pkgbig-got" "$bigprog" || fail "/jam/pkg/bin/bigpkg isn't the generated program's bytes"
        else
            fail "/jam/pkg/bin/bigpkg is not on the drive after the last install"
        fi
        rm -f "$out/pkgbig-got"
    else
        fail "can't make the second Jam OS drive"
    fi
    nvme=
    rm -f "$out/jam1.img" "$out/jam2.img" "$out/pkg.bigA.img" "$out/pkg.bigB.img" \
        "$out/pkg.bigcut.img" "$out/pkg.bigafter.img"
    rm -rf "$out/repo/web/big" "$out/repo/web/bigslow"   # 300 MiB each: not carried back
fi
grep -a "init: pkg: " "$out"/pkgmain.log "$out"/pkgsettle.log "$out"/pkgbig*.log 2>/dev/null | sed 's/^/pkg-test: /' | head -40
rm -f "$out/pkg.base.img" "$out/pkg.main.img" "$out/pkg.cut.img" "$out/pkg.after.img"
if [ $fails -eq 0 ]; then
    echo "pkg-test: PASS"
    exit 0
fi
echo "pkg-test: FAIL ($fails)"
exit 1
