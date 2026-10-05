#!/bin/sh
# `update`'s stick write in QEMU: init writes a fetched build to the
# stick's ESP, and the stick boots it from cold. Build A is this build with a throwaway test
# key (tools/update-test-key.sh); build B is A's kernel with another version
# string and A's boot image with `build.txt` saying git b0b0b0b and one
# more file, update-marker.txt (as tools/update-net-test.sh makes them),
# signed with the test key; build C the same with another version string
# and git c0c0c0c. QEMU runs on one stick image, each a cold boot of the
# image the last one left (QEMU_SAVE, QEMU_IMAGE):
#   1. wfail: `run updtest writefail`: B offered to be written with a
#      test's failure at each step (room; once the older previous build is
#      removed; half way through the new kernel; between the two renames
#      of the new build): each answered "not written", the stick left
#      booting A (renamed back after the swap began; the second failure
#      leaves no previous build, so the fourth write copies A as it first).
#      Then a firmware reset (`reboot -f`, the power cycle). On the Mac the
#      ESP holds A as jamos.elf/bootfs.img and as the previous build, and no
#      .new or .old file.
#   1b. wstop (on a copy of run 1's stick): B and C written in turn
#      (`run updtest writestop <n> b|c`), each stopped dead after change n
#      of names of its swap, n = 1 to 8, as a power cut there, and after
#      each `run updtest espcheck`: one of the stick's two entries holds a
#      whole build (A, B or C). A build is written again after a stop
#      before its change 4 (it isn't in yet), the other one after; then a
#      last write of C with no stop. Cold boots of that stick: its default
#      entry runs C, its previous-build entry B.
#   2. wnet: a cold boot runs A; `update -m` (QEMU_NET, the peer serving B,
#      signed): B fetched, checked and loaded into memory only, no reboot;
#      `reboot` (kexec) starts B (`version`, the marker), the stick's
#      kernel still A's; then `update -w` (the old spelling of plain
#      `update`), B being the running build: B written to the stick, no
#      reboot. On the Mac the ESP holds B, and A as the previous build.
#   3. wcold: a cold boot of that stick runs B (the marker is there).
#   4. wprev: a cold boot of the boot menu's "Jam OS (previous build)"
#      (QEMU_BOOT_PREV) runs A (no marker).
# Every frame of run 2 is tagged VLAN 21 (the peer's and the pcap's checks).
# Usage: tools/update-write-test.sh <outdir> (after `make -s image`); exit 0 on PASS.
set -u
out=$1
mkdir -p "$out"
fails=0

fail() {
    echo "update-write-test: FAILED: $1"
    fails=$((fails + 1))
}

img="$out/wtest.img"
tools/update-test-key.sh "$out" build/jamos.img "$img" ||
    { echo "update-write-test: can't make the test key's stick"; exit 1; }
key="$out/testkey/key1/update.key"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\nnet.host = 10.2.21.174\n' \
    > "$out/wtest.settings"
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/wtest.settings" ::/etc/settings ||
    { echo "update-write-test: can't write the stick's settings"; exit 1; }

# Build B, and its signed manifest on /data/update for updtest.
va=$(python3 tools/update-server.py --manifest build/jamos.elf build/bootfs.img |
     sed -n 's/^version //p')
vb=$(python3 - build/jamos.elf "$out/jamos-B.elf" "$va" <<'EOF'
import sys
data, old = open(sys.argv[1], "rb").read(), sys.argv[3].encode()
new = old[:-1] + (b"C" if old.endswith(b"B") else b"B")
assert data.count(old + b"\0") >= 1, "no version string in the kernel"
open(sys.argv[2], "wb").write(data.replace(old + b"\0", new + b"\0"))
print(new.decode())
EOF
) || { echo "update-write-test: can't make build B's kernel"; exit 1; }
marker="update-marker: build B written $$"
printf '%s\n' "$marker" > "$out/wtest-marker.txt"
printf 'git b0b0b0b\n%s\n' "$(sed -n 's/^\(net .*\)$/\1/p' build/build.txt)" > "$out/wtest-build.txt"
python3 tools/bootfs-edit.py "$out/testkey/bootfs-key.img" "$out/bootfs-B.img" \
    "update-marker.txt=$out/wtest-marker.txt" "build.txt=$out/wtest-build.txt" ||
    { echo "update-write-test: can't make build B's boot image"; exit 1; }
python3 tools/update-server.py --manifest "$out/jamos-B.elf" "$out/bootfs-B.img" --key "$key" \
    > "$out/manifest" || { echo "update-write-test: no manifest"; exit 1; }
# Build C: A's kernel with a third version string, A's boot image with git c0c0c0c.
vc=$(python3 - build/jamos.elf "$out/jamos-C.elf" "$va" <<'EOF'
import sys
data, old = open(sys.argv[1], "rb").read(), sys.argv[3].encode()
new = old[:-1] + (b"E" if old.endswith(b"D") else b"D")
open(sys.argv[2], "wb").write(data.replace(old + b"\0", new + b"\0"))
print(new.decode())
EOF
) || { echo "update-write-test: can't make build C's kernel"; exit 1; }
printf 'git c0c0c0c\n%s\n' "$(sed -n 's/^\(net .*\)$/\1/p' build/build.txt)" > "$out/wtest-build-C.txt"
python3 tools/bootfs-edit.py "$out/testkey/bootfs-key.img" "$out/bootfs-C.img" \
    "build.txt=$out/wtest-build-C.txt" ||
    { echo "update-write-test: can't make build C's boot image"; exit 1; }
python3 tools/update-server.py --manifest "$out/jamos-C.elf" "$out/bootfs-C.img" --key "$key" \
    > "$out/manifest-C" || { echo "update-write-test: no manifest for C"; exit 1; }
mmd -i "$img@@64M" ::/update &&
    mcopy -i "$img@@64M" "$out/manifest" ::/update/manifest &&
    mcopy -i "$img@@64M" "$out/jamos-B.elf" ::/update/jamos.elf &&
    mcopy -i "$img@@64M" "$out/bootfs-B.img" ::/update/bootfs.img &&
    mmd -i "$img@@64M" ::/update/c &&
    mcopy -i "$img@@64M" "$out/manifest-C" ::/update/c/manifest &&
    mcopy -i "$img@@64M" "$out/jamos-C.elf" ::/update/c/jamos.elf &&
    mcopy -i "$img@@64M" "$out/bootfs-C.img" ::/update/c/bootfs.img &&
    mmd -i "$img@@64M" ::/update/a &&
    mcopy -i "$img@@1M" ::/boot/jamos.elf "$out/jamos-A.elf" &&
    mcopy -i "$img@@1M" ::/boot/bootfs.img "$out/bootfs-A0.img" &&
    mcopy -i "$img@@64M" "$out/jamos-A.elf" ::/update/a/jamos.elf &&
    mcopy -i "$img@@64M" "$out/bootfs-A0.img" ::/update/a/bootfs.img &&
    cp "$out/manifest" "$out/manifest-A0" && mcopy -i "$img@@64M" "$out/manifest-A0" ::/update/a/manifest ||
    { echo "update-write-test: can't write the stick's /data"; exit 1; }
cat > "$out/wtest.spec.json" <<EOF
{"kernel": "$out/jamos-B.elf", "bootfs": "$out/bootfs-B.img", "key": "$key", "plan": []}
EOF
echo "update-write-test: build A $va, build B $vb, build C $vc"
sha_a=$(shasum -a 256 build/jamos.elf | cut -d' ' -f1)

# esp <image> <name> <file>: does the image's ESP have boot/<name>, the
# same bytes as <file>?
esp() {
    rm -f "$out/wtest-got"
    mcopy -i "$1@@1M" "::/boot/$2" "$out/wtest-got" 2>/dev/null && cmp -s "$out/wtest-got" "$3"
}
# no_new <image>: no .new or .old file left on its ESP.
no_new() {
    ! mdir -i "$1@@1M" ::/boot 2>/dev/null | grep -qiE "\.(new|old)"
}
mcopy -i "$img@@1M" ::/boot/bootfs.img "$out/bootfs-A.img"

# Run 1: the failures, then a power cycle.
cat > "$out/wfail.txt" <<EOF
wait 120 Jam OS shell
wait jam>
seen 60 init: /data mounted
send run updtest writefail
wait 900 updtest: writefail:
wait jam>
send reboot -f
wait reboot: resetting
EOF
QEMU_IMAGE="$img" QEMU_SAVE="$out/wtest-1.img" QEMU_TIMEOUT=${QEMU_TIMEOUT:-1200} \
    QEMU_INPUT="$out/wfail.txt" tools/qemu-test.sh "$out" wfail shell > "$out/wfail.out" 2>&1 ||
    fail "run 1, the script (see $out/wfail.log)"
log="$out/wfail.log"
grep -aq "updtest: writefail: PASS" "$log" || fail "run 1: a failed write didn't end as it should"
[ "$(grep -ac "init: update: the stick write fails here, as the test asked" "$log")" -eq 4 ] ||
    fail "run 1: not 4 failures injected"
[ "$(grep -ac "the old build is back as the stick's" "$log")" -eq 2 ] ||
    fail "run 1: the two failures in the swap weren't put back"
[ "$(grep -ac "init: update: .* stored, but the stick write failed" "$log")" -eq 4 ] ||
    fail "run 1: not 4 writes said to have failed"
grep -a "updtest: write\|init: update: .*stick" "$log" | sed 's/^/update-write-test: /'
esp "$out/wtest-1.img" jamos.elf build/jamos.elf &&
    esp "$out/wtest-1.img" bootfs.img "$out/bootfs-A.img" ||
    fail "run 1: the stick's build isn't A any more"
esp "$out/wtest-1.img" prev-jamos.elf build/jamos.elf &&
    esp "$out/wtest-1.img" prev-bootfs.img "$out/bootfs-A.img" ||
    fail "run 1: the previous build isn't A (copied again after the failure that removed it)"
no_new "$out/wtest-1.img" || fail "run 1: a .new or .old file is left on the ESP"

# Run 1b: a stop at each change of the swap, on a copy of run 1's stick.
{
    printf 'wait 120 Jam OS shell\nwait jam>\nseen 60 init: /data mounted\n'
    for step in 1:b 2:b 3:b 4:b 5:c 6:b 7:c 8:b 0:c; do
        printf 'send run updtest writestop %s %s\nwait 300 updtest: writestop:\nwait jam>\n' \
            "${step%:*}" "${step#*:}"
        printf 'send run updtest espcheck\nwait 120 updtest: espcheck:\nwait jam>\n'
    done
    printf 'send reboot -f\nwait reboot: resetting\n'
} > "$out/wstop.txt"
QEMU_IMAGE="$out/wtest-1.img" QEMU_SAVE="$out/wtest-stop.img" QEMU_TIMEOUT=${QEMU_TIMEOUT:-1500} \
    QEMU_INPUT="$out/wstop.txt" tools/qemu-test.sh "$out" wstop shell > "$out/wstop.out" 2>&1 ||
    fail "run 1b, the script (see $out/wstop.log)"
[ "$(grep -ac "updtest: writestop: PASS" "$out/wstop.log")" -eq 9 ] ||
    fail "run 1b: a write wasn't answered as it should"
[ "$(grep -ac "updtest: espcheck: PASS" "$out/wstop.log")" -eq 9 ] ||
    fail "run 1b: a stop left neither entry with a whole build"
grep -a "updtest: [bc] written\|updtest: the default entry" "$out/wstop.log" | tr -d '\r' |
    sed 's/^.*updtest/update-write-test: updtest/'
esp "$out/wtest-stop.img" jamos.elf "$out/jamos-C.elf" &&
    esp "$out/wtest-stop.img" prev-jamos.elf "$out/jamos-B.elf" ||
    fail "run 1b: the stick isn't C with B as the previous build"
no_new "$out/wtest-stop.img" || fail "run 1b: a .new or .old file is left on the ESP"
for run in wstopcold wstopprev; do
    want=$vc prev=0
    [ $run = wstopprev ] && want=$vb prev=1
    printf 'wait 120 Jam OS shell\nwait jam>\nsend version\nwait Jam OS %s, git\nwait jam>\nsend reboot -f\nwait reboot: resetting\n' \
        "$want" > "$out/$run.txt"
    QEMU_IMAGE="$out/wtest-stop.img" QEMU_BOOT_PREV=$prev QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} \
        QEMU_INPUT="$out/$run.txt" tools/qemu-test.sh "$out" $run shell > "$out/$run.out" 2>&1 ||
        fail "$run: the stopped-and-written stick didn't boot build $want (see $out/$run.log)"
done

# Run 2: a cold boot (A), `update -m` and `reboot` (B from memory), then
# `update -w` over the network.
cat > "$out/wnet.txt" <<EOF
wait 120 Jam OS shell
seen 60 netstack: address 10.2.21.5/24
wait jam>
send version
wait Jam OS $va, git
wait jam>
send update -m
wait 600 -> $vb (b0b0b0b): checked by init in
wait loaded into memory only (-m)
wait jam>
send reboot
wait 30 init: kexec: /esp unchanged: the stored kernel, no files read
wait 60 kexec: starting the stored kernel
wait 60 kexec: started by a reboot
wait 120 init: the shell is up
wait jam>
send version
wait Jam OS $vb, git b0b0b0b
wait jam>
send cat /boot/update-marker.txt
wait $marker
wait jam>
send sha256sum /esp/boot/jamos.elf
wait $sha_a
wait jam>
send update -w
wait 600 -> $vb (b0b0b0b): checked by init in
wait loaded and written to the stick
wait update: written to the stick and loaded:
wait jam>
send version
wait Jam OS $vb, git b0b0b0b
wait jam>
send reboot -f
wait reboot: resetting
EOF
QEMU_IMAGE="$out/wtest-1.img" QEMU_SAVE="$out/wtest-2.img" QEMU_NET=1 \
    QEMU_NET_PEER="--update $out/wtest.spec.json" QEMU_TIMEOUT=${QEMU_TIMEOUT:-1200} \
    QEMU_INPUT="$out/wnet.txt" tools/qemu-test.sh "$out" wnet shell > "$out/wnet.out" 2>&1 ||
    fail "run 2, the script or the VLAN checks (see $out/wnet.out, $out/wnet.log)"
log="$out/wnet.log"
grep -aq "init: update: .* and stored in memory only" "$log" ||
    fail "run 2: init didn't say update -m loaded it into memory only"
grep -aq "init: update: .* and stored, and written to the stick" "$log" ||
    fail "run 2: init didn't say it wrote the stick"
[ "$(grep -ac "kexec: kexec_load from init: OK" "$log")" -eq 2 ] ||
    fail "run 2: not exactly two builds loaded (update -m, update -w)"
[ "$(grep -ac "kexec: starting the stored kernel" "$log")" -eq 1 ] ||
    fail "run 2: not exactly one kexec (the reboot after update -m: update -w rebooted?)"
[ "$(grep -ac "reboot: resetting" "$log")" -eq 1 ] ||
    fail "run 2: a firmware reset before the last one"
grep -a "update: fetched\|init: update: \|devmgr: /esp" "$log" | sed 's/^/update-write-test: /'
esp "$out/wtest-2.img" jamos.elf "$out/jamos-B.elf" &&
    esp "$out/wtest-2.img" bootfs.img "$out/bootfs-B.img" ||
    fail "run 2: the stick's build isn't B"
esp "$out/wtest-2.img" prev-jamos.elf build/jamos.elf &&
    esp "$out/wtest-2.img" prev-bootfs.img "$out/bootfs-A.img" ||
    fail "run 2: the previous build isn't A"
no_new "$out/wtest-2.img" || fail "run 2: a .new or .old file is left on the ESP"

# Runs 3 and 4: cold boots of the written stick, its default entry (B)
# and its previous-build entry (A).
for run in wcold wprev; do
    want=$vb mk=0 prev=0
    [ $run = wprev ] && want=$va mk=1 prev=1
    cat > "$out/$run.txt" <<EOF
wait 120 Jam OS shell
wait jam>
send version
wait Jam OS $want, git
wait jam>
send cat /boot/update-marker.txt; echo mk-""\$?
wait mk-$mk
wait jam>
send reboot -f
wait reboot: resetting
EOF
    QEMU_IMAGE="$out/wtest-2.img" QEMU_BOOT_PREV=$prev QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} \
        QEMU_INPUT="$out/$run.txt" tools/qemu-test.sh "$out" $run shell > "$out/$run.out" 2>&1 ||
        fail "$run: the stick didn't boot build $want from cold (see $out/$run.log)"
done
rm -f "$img" "$out/wtest-1.img" "$out/wtest-2.img" "$out/wtest-stop.img" "$out/wtest-got"
if [ $fails -eq 0 ]; then
    echo "update-write-test: PASS"
    exit 0
fi
echo "update-write-test: FAIL ($fails)"
exit 1
