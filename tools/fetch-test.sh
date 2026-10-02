#!/bin/sh
# fetch end to end in QEMU (tools/shell-tests/fetch.txt): bin/fetch in the
# guest against tools/httptest.py on the Mac, joined by the network peer's
# TCP relay (tools/netpeer.py --tcp-relay 8000:<its port>: the guest
# connects to 10.2.21.174:8000) through e1000e on VLAN 21. The script has
# fetch save files, follow redirects, write into a pipe and refuse hostile
# servers; afterwards the files fetch saved are read off the stick image
# and their SHA-256 must be the server's. The run must pass the peer's and
# the pcap's VLAN checks (tools/qemu-test.sh).
# Usage: tools/fetch-test.sh <outdir>
set -eu
out=$1
mkdir -p "$out"
img="$out/fetch.base.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\n' > "$out/fetch.settings"
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/fetch.settings" ::/etc/settings
set -- $(python3 tools/netpeer.py --free-ports 1)
hport=$1
python3 tools/httptest.py --port "$hport" > "$out/fetch.http.log" 2>&1 &
hpid=$!
trap 'kill $hpid 2>/dev/null || true' EXIT
j=0
while [ $j -lt 50 ] && ! grep -q "serving on" "$out/fetch.http.log"; do sleep 0.1; j=$((j + 1)); done
ok=1
QEMU_IMAGE="$img" QEMU_NET=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-900} QEMU_SAVE="$out/fetch.after.img" \
    QEMU_NET_PEER="--tcp-relay 8000:$hport" \
    QEMU_INPUT=tools/shell-tests/fetch.txt tools/qemu-test.sh "$out" fetch shell || ok=0
grep -E "fetch: [0-9]+ bytes .* MB/s" "$out/fetch.log" | sed 's/^.*fetch:/fetch-test: guest: fetch:/' || true
# The files on the stick, against what the server sent.
for f in big.bin:big.bin chunked.bin:chunked.bin close.bin:close.bin name.txt:small.txt \
         redir.txt:small.txt; do
    file=${f%%:*} served=${f#*:}
    rm -f "$out/got-$file"
    mcopy -o -i "$out/fetch.after.img@@64M" "::/$file" "$out/got-$file" 2>/dev/null || true
    want=$(python3 tools/httptest.py --sha "$served")
    got=$(shasum -a 256 "$out/got-$file" 2>/dev/null | cut -d' ' -f1 || true)
    if [ "$got" = "$want" ]; then
        echo "fetch-test: /data/$file: sha256 $got, the server's"
    else
        echo "fetch-test: /data/$file: sha256 '${got:-none}', the server's is $want"
        ok=0
    fi
done
for f in x x.part y y.part; do
    if mdir -i "$out/fetch.after.img@@64M" "::/$f" >/dev/null 2>&1; then
        echo "fetch-test: /data/$f was left behind"
        ok=0
    fi
done
rm -f "$out/fetch.after.img"
if [ $ok = 1 ]; then
    echo "fetch-test: PASS"
else
    echo "fetch-test: FAIL"
    exit 1
fi
