#!/bin/sh
# `update`'s check in init (user/services/init/update.c), fed from files
# instead of the network (bin/updtest, user/tests/updtest): the half of
# M9's update that works before sockets exist. One QEMU run, three boots,
# then a second run on a build without a key (4, below):
#   1. `run updtest bad`: every damaged offer refused, each for its own
#      reason (a changed byte, a wrong length, a short VMO, a garbage, cut,
#      short-signed or future manifest, an unsigned one, one changed after
#      signing, one signed by another key, one with another manifest's
#      signature, a malformed offer, two files that match their signed
#      manifest but are no kernel), and the good build offered check only
#      (accepted, not loaded); then `reboot` (kexec): the stored kernel was
#      left alone, so the next boot is the stick's build (no
#      /boot/update-marker.txt).
#   2. `run updtest good`: the build accepted and stored; `reboot` reads
#      nothing from /esp ("/esp unchanged") and starts it.
#   3. the fetched build runs: /boot/update-marker.txt is there.
# The stick's build has a throwaway test key's public half
# (tools/update-test-key.sh: never the owner's). The fetched build is this
# build's kernel and a copy of that boot image with one more file
# (update-marker.txt), its manifest made by tools/update-server.py
# --manifest and signed with the test key (and once more with a second
# key), all put on the stick's /data (update/) with mtools before the boot.
# Usage: tools/update-test.sh <outdir>; exit 0 on PASS.
set -u
out=$1
mkdir -p "$out"
fails=0

fail() {
    echo "update-test: FAILED: $1"
    fails=$((fails + 1))
}

stick="$out/update-stick.img"
tools/update-test-key.sh "$out" build/jamos.img "$stick" ||
    { echo "update-test: can't make the test key's stick"; exit 1; }
key1="$out/testkey/key1/update.key" key2="$out/testkey/key2/update.key"
marker="update-marker: the fetched build $$"
printf '%s\n' "$marker" > "$out/update-marker.txt"
# The boot image again, with the marker.
python3 tools/bootfs-edit.py "$out/testkey/bootfs-key.img" "$out/bootfs-marked.img" \
    "update-marker.txt=$out/update-marker.txt" || { echo "update-test: can't pack"; exit 1; }
python3 tools/update-server.py --manifest build/jamos.elf "$out/bootfs-marked.img" \
    --key "$key1" > "$out/manifest" &&
    python3 tools/update-server.py --manifest build/jamos.elf "$out/bootfs-marked.img" \
        --key "$key2" > "$out/manifest-otherkey" || { echo "update-test: no manifest"; exit 1; }
# Two files that are no kernel (updtest makes the same bytes), signed.
python3 -c 'import sys; open(sys.argv[1], "wb").write(b"\x55" * 8192);
open(sys.argv[2], "wb").write(b"\xaa" * 4096)' "$out/nak.elf" "$out/nak.img" &&
    python3 tools/update-server.py --manifest "$out/nak.elf" "$out/nak.img" \
        --version not-a-kernel --git 0000000 --key "$key1" > "$out/nak.manifest" ||
    { echo "update-test: no manifest for the files that are no kernel"; exit 1; }
mmd -i "$stick@@64M" ::/update &&
    mcopy -i "$stick@@64M" "$out/manifest" "$out/manifest-otherkey" "$out/nak.manifest" \
        ::/update/ &&
    mcopy -i "$stick@@64M" build/jamos.elf ::/update/jamos.elf &&
    mcopy -i "$stick@@64M" "$out/bootfs-marked.img" ::/update/bootfs.img ||
    { echo "update-test: can't write the stick's /data"; exit 1; }

cat > "$out/update.txt" <<EOF
wait 120 Jam OS shell
wait jam>
seen 60 init: kexec: noted /esp/boot/jamos.elf
seen 60 init: /data mounted
send run updtest bad
wait 300 updtest: bad:
wait jam>
send reboot
wait 30 init: kexec: /esp unchanged: the stored kernel, no files read
wait 60 kexec: starting the stored kernel
wait 60 kexec: started by a reboot
wait 120 init: the shell is up
wait jam>
send cat /boot/update-marker.txt; echo mk-""\$?
wait mk-1
wait jam>
send run updtest good
wait 300 updtest: good:
wait jam>
send reboot
wait 30 init: kexec: /esp unchanged: the stored kernel, no files read
wait 60 kexec: starting the stored kernel
wait 60 kexec: started by a reboot
wait 120 init: the shell is up
wait jam>
send cat /boot/update-marker.txt; echo mk-""\$?
wait mk-0
wait jam>
send reboot -f
wait reboot: resetting
EOF
QEMU_IMAGE="$stick" QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} QEMU_INPUT="$out/update.txt" \
    tools/qemu-test.sh "$out" update shell > "$out/update.out" 2>&1 ||
    fail "the script (see $out/update.log)"
log="$out/update.log"
grep -aq "updtest: bad: PASS" "$log" || fail "a damaged offer wasn't refused for its reason"
[ "$(grep -ac "init: update: refused: " "$log")" -eq 17 ] ||
    fail "not 17 refusals logged by init"
[ "$(grep -ac "init: update: refused: the signature isn't this build's key's" "$log")" -eq 3 ] ||
    fail "not 3 refusals for the signature"
[ "$(grep -ac "init: update: refused: the manifest is not signed" "$log")" -eq 1 ] ||
    fail "the unsigned manifest wasn't refused for that"
grep -aq "init: update: .* and not loaded (check only)" "$log" ||
    fail "init didn't check the check-only offer"
grep -aq "updtest: good: PASS" "$log" || fail "the good build wasn't accepted"
grep -aq "init: update: .* and stored: .reboot. starts it" "$log" ||
    fail "init didn't say it stored the build"
[ "$(grep -ac "$marker" "$log")" -ge 1 ] || fail "the fetched build didn't run (no marker)"
[ "$(grep -ac "reboot: resetting" "$log")" -eq 1 ] ||
    fail "a firmware reset happened before the last one"
grep -aq "init: kexec: .* read in\|reading /esp" "$log" && fail "a reboot read /esp's files"
[ "$(grep -ac "kexec: kexec_load from init: OK" "$log")" -eq 1 ] ||
    fail "not exactly one build loaded (the good one)"
rm -f "$stick"

# 4. A build without a key (its boot image has no update.pub): the shell's
#    `update` fetches nothing and says why; init refuses the build offered
#    straight to it (updtest nokey), plain and check-only.
nokey="$out/update-nokey.img"
tools/update-test-key.sh "$out" build/jamos.img "$nokey" nokey &&
    mmd -i "$nokey@@64M" ::/update &&
    mcopy -i "$nokey@@64M" "$out/manifest" build/jamos.elf ::/update/ &&
    mcopy -i "$nokey@@64M" "$out/bootfs-marked.img" ::/update/bootfs.img ||
    { echo "update-test: can't make the keyless stick"; exit 1; }
cat > "$out/nokey.txt" <<EOF
wait 120 Jam OS shell
wait jam>
seen 60 init: /data mounted
send update 10.2.21.174; echo nk-""\$?
wait 30 this build has no update key: updates are off
wait nk-1
wait jam>
send run updtest nokey
wait 120 updtest: nokey:
wait jam>
send reboot -f
wait reboot: resetting
EOF
QEMU_IMAGE="$nokey" QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} QEMU_INPUT="$out/nokey.txt" \
    tools/qemu-test.sh "$out" nokey shell > "$out/nokey.out" 2>&1 ||
    fail "the keyless script (see $out/nokey.log)"
grep -aq "updtest: nokey: PASS" "$out/nokey.log" || fail "the keyless build took an offer"
[ "$(grep -ac "init: update: refused: this build has no update key" "$out/nokey.log")" -eq 2 ] ||
    fail "not 2 keyless refusals logged by init"
grep -aq "update: asking" "$out/nokey.log" && fail "the keyless build fetched something"
rm -f "$nokey"
if [ $fails -eq 0 ]; then
    echo "update-test: PASS"
    exit 0
fi
echo "update-test: FAIL ($fails)"
exit 1
