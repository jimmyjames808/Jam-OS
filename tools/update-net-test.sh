#!/bin/sh
# `update` over the network in QEMU: the shell's command, bin/update's
# fetch through netstack and QEMU's e1000e, init's check and kexec_load.
# tools/netpeer.py answers port 5022 with tools/update-server.py's
# PlannedServer, serving build B: this build's kernel with another version
# string (its last character changed in the ELF file) and its boot image
# with build.txt saying git b0b0b0b and one more file, update-marker.txt.
# One QEMU run (net.address and net.host = 10.2.21.174 in the stick's
# settings), each `update` a new client of the server, which gets the next
# plan:
#   1. damage     a kernel byte changed after the manifest: init refuses it
#   2. wronghash  the manifest's boot image SHA-256 wrong: init refuses it
#   3. truncated  the boot image served is half the manifest's size: the
#                 fetch fails
#   4. gone       the server stops answering mid-fetch: the fetch fails
#   5. `update -n`: build B fetched and checked, old -> new said, nothing
#                 loaded
#   6. `update`: build B stored and the shell reboots into it (kexec); the
#      next boot's `version` is B's, and /boot/update-marker.txt is there.
# The running build stays untouched by 1-5: exactly one kexec_load (the
# last), `version` still A's before it. Every frame the guest sent is tagged
# VLAN 21 (the peer's and the pcap's checks, tools/qemu-test.sh).
# Usage: tools/update-net-test.sh <outdir> (after `make -s image`); exit 0 on PASS.
set -u
out=$1
mkdir -p "$out"
fails=0

fail() {
    echo "update-net-test: FAILED: $1"
    fails=$((fails + 1))
}

img="$out/updnet.base.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\nnet.host = 10.2.21.174\n' \
    > "$out/updnet.settings"
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/updnet.settings" ::/etc/settings ||
    { echo "update-net-test: can't write the stick's settings"; exit 1; }

# Build B.
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
) || { echo "update-net-test: can't make build B's kernel"; exit 1; }
marker="update-marker: build B $$"
printf '%s\n' "$marker" > "$out/updnet-marker.txt"
printf 'git b0b0b0b\n' > "$out/updnet-build.txt"
python3 tools/bootfs-edit.py build/bootfs.img "$out/bootfs-B.img" \
    "update-marker.txt=$out/updnet-marker.txt" "build.txt=$out/updnet-build.txt" ||
    { echo "update-net-test: can't make build B's boot image"; exit 1; }
cat > "$out/updnet.spec.json" <<EOF
{"kernel": "$out/jamos-B.elf", "bootfs": "$out/bootfs-B.img",
 "plan": ["damage", "wronghash", "truncated", "gone"]}
EOF
echo "update-net-test: build A $va, build B $vb"

cat > "$out/updnet.txt" <<EOF
wait 120 Jam OS shell
seen 60 netstack: address 10.2.21.5/24
wait jam>
send version
wait Jam OS $va, git
wait jam>
send update
wait 120 update: init refused it: a file's SHA-256 isn't the manifest's: kernel
wait jam>
send update
wait 120 update: init refused it: a file's SHA-256 isn't the manifest's: bootfs
wait jam>
send update
wait 120 update: the server's answers don't match its manifest
wait jam>
send update
wait 120 update: the server stopped answering: the fetch failed
wait jam>
send update -n
wait 120 -> $vb (b0b0b0b): checked by init in
wait not loaded (-n)
wait jam>
send version
wait Jam OS $va, git
wait jam>
send update
wait 120 -> $vb (b0b0b0b): checked by init in
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
send reboot -f
wait reboot: resetting
EOF
QEMU_IMAGE="$img" QEMU_NET=1 QEMU_NET_PEER="--update $out/updnet.spec.json" \
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-360} QEMU_INPUT="$out/updnet.txt" \
    tools/qemu-test.sh "$out" updnet shell > "$out/updnet.out" 2>&1 ||
    fail "the script or the VLAN checks (see $out/updnet.out, $out/updnet.log)"
log="$out/updnet.log"
[ "$(grep -ac "init: update: refused: a file's SHA-256 isn't the manifest's" "$log")" -eq 2 ] ||
    fail "not 2 SHA-256 refusals logged by init"
[ "$(grep -ac "kexec: kexec_load from init: OK" "$log")" -eq 1 ] ||
    fail "not exactly one build loaded (the last one)"
grep -aq "init: update: .* and not loaded (check only)" "$log" ||
    fail "init didn't say the -n check loaded nothing"
for plan in damage wronghash truncated gone good; do
    grep -aq "gets the plan '$plan'" "$out/updnet.peer.log" ||
        fail "the server never served the plan $plan"
done
[ "$(grep -ac "$marker" "$log")" -ge 1 ] || fail "build B didn't run (no marker)"
[ "$(grep -ac "reboot: resetting" "$log")" -eq 1 ] ||
    fail "a firmware reset happened before the last one"
grep -a "update: fetched\|init: update: .*checked in" "$log" | sed 's/^/update-net-test: /'
tail -1 "$out/updnet.out"
rm -f "$img" "$out/updnet.img"
if [ $fails -eq 0 ]; then
    echo "update-net-test: PASS"
    exit 0
fi
echo "update-net-test: FAIL ($fails)"
exit 1
