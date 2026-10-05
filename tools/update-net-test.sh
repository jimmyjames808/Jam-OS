#!/bin/sh
# `update` over the network in QEMU: the shell's command, bin/update's
# fetch through netstack and QEMU's e1000e, init's check and kexec_load.
# tools/netpeer.py answers port 5022 with tools/update-server.py's
# PlannedServer, serving build B: this build's kernel with another version
# string (its last character changed in the ELF file) and its boot image
# with build.txt saying git b0b0b0b (and this build's network default)
# and one more file, update-marker.txt.
# One QEMU run (net.address and net.host = 10.2.21.174 in the stick's
# settings), each `update` a new client of the server, which gets the next
# plan:
#   1. damage     a kernel byte changed after the manifest: init refuses it
#   2. wronghash  the manifest's boot image SHA-256 wrong: init refuses it
#   3. truncated  the boot image served is half the manifest's size: the
#                 fetch fails
#   4. gone       the server stops answering mid-fetch: the fetch fails
#   5. badsig     the manifest changed after it was signed: init refuses
#                 the signature
#   6. othernet   the manifest (signed) says the other network default
#                 (vlan21 for an untagged build, untagged for a VLAN one):
#                 init refuses it
#   7. othernet   again, with `update -f -n`: taken when forced, checked,
#                 nothing loaded
#   8. mustknow   the manifest (signed) has a must-understand line no build
#                 knows: init refuses it, the build needs a newer one
#   9. extension  the manifest (signed) has an extension line no build
#                 knows, with `update -n`: skipped, B checked
#  10. `update -n`: build B fetched and checked, old -> new said, nothing
#                 loaded
#  11. `update -m`: build B loaded into memory only: no reboot (`version`
#      still A's), the stick's kernel still A's (sha256sum /esp/boot/jamos.elf)
#  12. `update`: build B loaded and written to the stick, and NO reboot:
#      `version` still A's, the stick's kernel B's; then `reboot` (kexec)
#      reads nothing from /esp and starts B: `version` is B's, and
#      /boot/update-marker.txt is there; then `reboot -f`.
# Then a second run, `updcold`: a cold boot of the stick that run left
# (what `reboot -f` starts, through the firmware): B, from the stick.
# Build A (the stick's) has a throwaway test key's public half
# (tools/update-test-key.sh), and the server signs every manifest with it;
# build B has it too. The running build stays untouched by 1-10: exactly
# two kexec_loads (11 and 12), `version` still A's until the `reboot`.
# Every frame the guest sent is tagged VLAN 21 (the peer's and the pcap's
# checks, tools/qemu-test.sh: the run boots with vlan=21, whatever this
# build's default).
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
tools/update-test-key.sh "$out" "${QEMU_IMAGE:-build/jamos.img}" "$img" ||
    { echo "update-net-test: can't make the test key's stick"; exit 1; }
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
printf 'git b0b0b0b\n%s\n' "$(sed -n 's/^\(net .*\)$/\1/p' build/build.txt)" \
    > "$out/updnet-build.txt"
python3 tools/bootfs-edit.py "$out/testkey/bootfs-key.img" "$out/bootfs-B.img" \
    "update-marker.txt=$out/updnet-marker.txt" "build.txt=$out/updnet-build.txt" ||
    { echo "update-net-test: can't make build B's boot image"; exit 1; }
cat > "$out/updnet.spec.json" <<EOF
{"kernel": "$out/jamos-B.elf", "bootfs": "$out/bootfs-B.img",
 "key": "$out/testkey/key1/update.key",
 "plan": ["damage", "wronghash", "truncated", "gone", "badsig", "othernet", "othernet",
          "mustknow", "extension"]}
EOF
echo "update-net-test: build A $va, build B $vb"
sha_a=$(shasum -a 256 build/jamos.elf | cut -d' ' -f1)
sha_b=$(shasum -a 256 "$out/jamos-B.elf" | cut -d' ' -f1)

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
send update
wait 120 update: init refused it: the signature isn't this build's key's
wait jam>
send update
wait 120 update: init refused it: its network default is
wait jam>
send update -f -n
wait 120 -> $vb (b0b0b0b): checked by init in
wait not loaded (-n)
wait jam>
send update
wait 120 update: init refused it: it needs a newer build than this one to take it (it has "!future-must"
wait jam>
send update -n
wait 120 -> $vb (b0b0b0b): checked by init in
wait not loaded (-n)
wait jam>
send update -n
wait 120 -> $vb (b0b0b0b): checked by init in
wait not loaded (-n)
wait jam>
send version
wait Jam OS $va, git
wait jam>
send update -m
wait 120 -> $vb (b0b0b0b): checked by init in
wait loaded into memory only (-m)
wait update: loaded into memory only:
wait jam>
send version
wait Jam OS $va, git
wait jam>
send sha256sum /esp/boot/jamos.elf
wait $sha_a
wait jam>
send update
wait 120 -> $vb (b0b0b0b): checked by init in
wait loaded and written to the stick
wait update: written to the stick and loaded:
wait jam>
send version
wait Jam OS $va, git
wait jam>
send sha256sum /esp/boot/jamos.elf
wait $sha_b
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
send reboot -f
wait reboot: resetting
EOF
QEMU_IMAGE="$img" QEMU_SAVE="$out/updnet-saved.img" QEMU_NET=1 \
    QEMU_NET_PEER="--update $out/updnet.spec.json" \
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-480} QEMU_INPUT="$out/updnet.txt" \
    tools/qemu-test.sh "$out" updnet shell > "$out/updnet.out" 2>&1 ||
    fail "the script or the VLAN checks (see $out/updnet.out, $out/updnet.log)"
log="$out/updnet.log"
[ "$(grep -ac "init: update: refused: a file's SHA-256 isn't the manifest's" "$log")" -eq 2 ] ||
    fail "not 2 SHA-256 refusals logged by init"
[ "$(grep -ac "kexec: kexec_load from init: OK" "$log")" -eq 2 ] ||
    fail "not exactly two builds loaded (update -m and update)"
[ "$(grep -ac "init: update: .* and stored in memory only" "$log")" -eq 1 ] ||
    fail "init didn't say update -m loaded B into memory only"
[ "$(grep -ac "init: update: .* and stored, and written to the stick" "$log")" -eq 1 ] ||
    fail "init didn't say update wrote B to the stick"
[ "$(grep -ac "init: update: .* and not loaded (check only)" "$log")" -eq 3 ] ||
    fail "init didn't say the three -n checks loaded nothing"
grep -aq "init: update: refused: it needs a newer build than this one to take it (it has \"!" \
    "$log" || fail "init didn't refuse the must-understand line"
[ "$(grep -ac "init: update: refused: its network default is" "$log")" -eq 1 ] ||
    fail "init didn't refuse the other network default"
grep -aq "init: update: its network default is .*: taken (forced)" "$log" ||
    fail "init didn't take the other network default with -f"
[ "$(grep -ac "init: update: refused: the signature isn't this build's key's" "$log")" -eq 1 ] ||
    fail "the changed manifest wasn't refused for its signature"
for plan in damage wronghash truncated gone badsig othernet mustknow extension good; do
    grep -aq "gets the plan '$plan'" "$out/updnet.peer.log" ||
        fail "the server never served the plan $plan"
done
[ "$(grep -ac "$marker" "$log")" -ge 1 ] || fail "build B didn't run (no marker)"
[ "$(grep -ac "reboot: resetting" "$log")" -eq 1 ] ||
    fail "a firmware reset happened before the last one"
grep -a "update: fetched\|init: update: .*checked in" "$log" | sed 's/^/update-net-test: /'
tail -1 "$out/updnet.out"

# What `reboot -f` starts: the stick, from cold, runs B.
cat > "$out/updcold.txt" <<EOF
wait 120 Jam OS shell
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
QEMU_IMAGE="$out/updnet-saved.img" QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} \
    QEMU_INPUT="$out/updcold.txt" tools/qemu-test.sh "$out" updcold shell \
    > "$out/updcold.out" 2>&1 ||
    fail "updcold: the written stick didn't boot B from cold (see $out/updcold.log)"
rm -f "$img" "$out/updnet.img" "$out/updnet-saved.img"
if [ $fails -eq 0 ]; then
    echo "update-net-test: PASS"
    exit 0
fi
echo "update-net-test: FAIL ($fails)"
exit 1
