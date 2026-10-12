#!/bin/sh
# POSIX sockets end to end in QEMU (docs/M13-PLAN.md section 8): posixtest's
# socket and sockets sections (user/posix/test/socket.c, sockets.c) in the
# guest against the network peer's TCP side (tools/tcppeer.py, as
# tools/tcp-test.sh uses it) and its DNS server, on VLAN 21
# (tools/shell-tests/posix-net.txt). The peer's servers each read their
# BYTES to the guest's FIN, checked, then send theirs and a FIN:
#   5040  1 MiB each way (a blocking connection)
#   5041  64 KiB each way (a nonblocking connect, poll)
#   5042  100 bytes (EINTR, SO_RCVTIMEO, a second thread's send)
#   5043  100 bytes (EPIPE and SIGPIPE after shutdown)
#   5046  4 KiB on each of 32 connections polled at once
#   5047  4 KiB (the connection handed to a child)
# and it makes two connections to the guest's listener on 5045, 64 KiB
# each way each. PASS needs posixtest's PASS, the peer's 39 connections
# right and none wrong, and the run's VLAN checks (tools/qemu-test.sh).
# QEMU_NET_VLAN=none runs it untagged.
# Usage: tools/posix-net-test.sh <outdir>
set -eu
DATA_AT=$(python3 tools/mkimage.py --data-at build/jamos.img) || exit 1   # /data in the image: mtools' @@offset
out=$1
mkdir -p "$out"
img="$out/posix-net.base.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\n' > "$out/posix-net.settings"
mmd -i "$img@@$DATA_AT" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@$DATA_AT" "$out/posix-net.settings" ::/etc/settings
ok=1
serve="5040:1048576,5041:65536,5042:100,5043:100,5046:4096,5047:4096"
QEMU_IMAGE="$img" QEMU_NET=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} \
    QEMU_NET_PEER="--tcp-serve $serve --tcp-connect 10.2.21.5:5045:2:65536" \
    QEMU_INPUT=tools/shell-tests/posix-net.txt tools/qemu-test.sh "$out" posix-net shell || ok=0
rm -f "$img"
count() {
    python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get(sys.argv[2], 0))" \
        "$out/posix-net.peer.json" "$1" 2>/dev/null || echo 0
}
good=$(count tcp_ok)
bad=$(count tcp_bad)
echo "posix-net-test: the peer counted $good connections right, $bad wrong;" \
     "$(count tcp_bytes_in) bytes in, $(count tcp_bytes_out) out"
grep "posixtest: sockets: 1 MiB" "$out/posix-net.log" |
    sed 's/^.*posixtest/posix-net-test: posixtest/' || true
[ "$good" = 39 ] && [ "$bad" = 0 ] ||
    { echo "posix-net-test: want 39 connections right, 0 wrong"; ok=0; }
if [ $ok = 1 ]; then
    echo "posix-net-test: PASS"
else
    echo "posix-net-test: FAIL"
    exit 1
fi
