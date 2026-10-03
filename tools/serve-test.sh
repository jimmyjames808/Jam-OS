#!/bin/sh
# serve end to end in QEMU (tools/shell-tests/serve.txt): two files put on
# the stick's /data, served by bin/serve in the guest on ports 8080 and
# 8081, fetched by curl on the Mac through the network peer's TCP relay
# (tools/netpeer.py --tcp-forward: two Mac ports to the guest's two), with
# tools/servecheck.py checking every answer: two downloads at once with
# their SHA-256, HEAD, a range, the second file's type, a request that
# isn't HTTP and a head too big, a silent client that holds up no other,
# nothing served after `serve stop`. The guest's script checks the
# shell's side, the request lines and a program without the listen
# permission refused. The run must pass the peer's and the pcap's VLAN
# checks (tools/qemu-test.sh). Then a boot with no network card:
# `serve` says there is no network address yet (serve-nonet.txt).
# Usage: tools/serve-test.sh <outdir>
set -eu
out=$1
mkdir -p "$out"
img="$out/serve.base.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\n' > "$out/serve.settings"
python3 -c "import random,sys; sys.stdout.buffer.write(random.Random(5).randbytes(5 << 20))" \
    > "$out/serve.bin"
printf '<html><body>served by jam os</body></html>\n' > "$out/page.html"   # 43 bytes
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/serve.settings" ::/etc/settings
mcopy -o -i "$img@@64M" "$out/serve.bin" ::/serve.bin
mcopy -o -i "$img@@64M" "$out/page.html" ::/page.html
set -- $(python3 tools/netpeer.py --free-ports 2)
p1=$1 p2=$2
rm -f "$out/serve.log"
python3 tools/servecheck.py --log "$out/serve.log" --port "$p1" --port2 "$p2" \
    --file "$out/serve.bin" --page "$out/page.html" --out "$out" > "$out/servecheck.log" 2>&1 &
cpid=$!
trap 'kill $cpid 2>/dev/null || true' EXIT
ok=1
QEMU_IMAGE="$img" QEMU_NET=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} \
    QEMU_NET_PEER="--tcp-forward $p1:10.2.21.5:8080,$p2:10.2.21.5:8081" \
    QEMU_INPUT=tools/shell-tests/serve.txt tools/qemu-test.sh "$out" serve shell || ok=0
wait $cpid || ok=0
cp "${QEMU_IMAGE:-build/jamos.img}" "$out/serve-nonet.base.img"   # no net.address: no address
QEMU_IMAGE="$out/serve-nonet.base.img" QEMU_INPUT=tools/shell-tests/serve-nonet.txt \
    tools/qemu-test.sh "$out" serve-nonet shell || ok=0
rm -f "$out/serve-nonet.base.img"
sed 's/^/serve-test: /' "$out/servecheck.log"
grep -a "serve: 10\.2\.21\." "$out/serve.log" | tr -d '\r' | sed 's/^.*serve:/serve-test: guest: serve:/' || true
if [ $ok = 1 ]; then
    echo "serve-test: PASS"
else
    echo "serve-test: FAIL"
    exit 1
fi
