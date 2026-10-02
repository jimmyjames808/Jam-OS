#!/bin/sh
# SNTP in QEMU: bin/sntp sets the clock from the network peer's SNTP
# server (tools/netpeer.py --ntp: 2031-02-27 23:06:40 UTC from the peer's
# start), three plain boots with QEMU's e1000e and the RTC at 2026-01-15,
# each passing the peer's and the pcap's VLAN checks (tools/qemu-test.sh):
#   sntp        no settings (tools/shell-tests/sntp.txt): sntp asks the
#               gateway (the DHCP lease's router); every answer comes after
#               a forged one (--ntp-forge: its origin a bit off, its time a
#               year later), which must be ignored; the step of five years
#               needs a second reply and gets one; `date -u` shows 2031 and
#               `date -r` the network; a restarted sntp moves the clock by
#               less than a second. Checked afterwards: the kernel's clock
#               line says the network set it, near the peer's time and
#               never a year past it; the forged replies were said to be
#               ignored; the first step was five years, the second under a
#               second;
#   sntp-name   `ntp.server = router.jam`: the name through /svc/dns, the
#               time from its address;
#   sntp-off    `ntp = off`: no sntp, and not one SNTP request at the peer.
# QEMU_SMP passes through. Usage: tools/sntp-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
ok=1
T=1930000000   # the peer's time at its start: 2031-02-27 23:06:40 UTC
RTC="-rtc base=2026-01-15T01:02:03"

peer_count() {
    python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get(sys.argv[2], -1))" \
        "$1" "$2" 2>/dev/null || echo -1
}

# A copy of the image with these settings: settings_image <name> <text>
settings_image() {
    img="$out/$1.base.img"
    cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
    printf '%s\n' "$2" > "$out/$1.settings"
    mmd -i "$img@@64M" ::/etc 2>/dev/null || true
    mcopy -o -i "$img@@64M" "$out/$1.settings" ::/etc/settings
}

# --- the gateway, forged replies, the jump ---
QEMU_TIMEOUT=${QEMU_TIMEOUT:-200} QEMU_NET=1 QEMU_NET_PEER="--ntp $T --ntp-forge" \
    QEMU_EXTRA="$RTC" QEMU_INPUT=tools/shell-tests/sntp.txt \
    tools/qemu-test.sh "$out" sntp shell || ok=0
log="$out/sntp.log"
set=$(grep -a "clock: set to .* from the network" "$log" | head -1 |
      sed -n 's/.*set to \([0-9]*\) s since.*/\1/p')
echo "sntp: the kernel's first network set: ${set:-none} (the peer started at $T)"
if [ -z "$set" ] || [ "$set" -lt $T ] || [ "$set" -gt $((T + 600)) ]; then
    echo "sntp: want the clock set to the peer's time (not the forged year later)"
    ok=0
fi
grep -aq "sntp: 1 reply ignored (the last: the origin doesn't match what we sent)" "$log" ||
    { echo "sntp: the forged replies weren't said to be ignored"; ok=0; }
steps=$(grep -a "sntp: the clock was" "$log" |
        sed -n "s/.*clock was \([-+][0-9]*\)\..*/\1/p" | tr -d + | tr '\n' ' ')
echo "sntp: the steps, in whole seconds: $steps"
set -- $steps
if [ $# -lt 2 ] || [ "$1" -lt 150000000 ] || [ "$2" -lt -1 ] || [ "$2" -gt 0 ]; then
    echo "sntp: want a first step of about five years and a second under a second"
    ok=0
fi
forged=$(peer_count "$out/sntp.peer.json" ntp_forged)
answered=$(peer_count "$out/sntp.peer.json" ntp_answered)
echo "sntp: the peer answered $answered requests, each after a forged reply ($forged)"
[ "$answered" -ge 3 ] || { echo "sntp: want at least 3 (the confirmation and the restart)"; ok=0; }
if grep -a "sntp: the clock was" "$log" | grep -q "NOT set"; then
    echo "sntp: a set failed"
    ok=0
fi

# --- a server by name ---
settings_image sntp-name "ntp.server = router.jam"
QEMU_IMAGE="$out/sntp-name.base.img" QEMU_NET=1 QEMU_NET_PEER="--ntp $T" QEMU_EXTRA="$RTC" \
    QEMU_INPUT=tools/shell-tests/sntp-name.txt tools/qemu-test.sh "$out" sntp-name shell || ok=0
rm -f "$out/sntp-name.base.img"

# --- off ---
settings_image sntp-off "ntp = off"
QEMU_IMAGE="$out/sntp-off.base.img" QEMU_NET=1 QEMU_NET_PEER="--ntp $T" QEMU_EXTRA="$RTC" \
    QEMU_INPUT=tools/shell-tests/sntp-off.txt tools/qemu-test.sh "$out" sntp-off shell || ok=0
rm -f "$out/sntp-off.base.img"
queries=$(peer_count "$out/sntp-off.peer.json" ntp_queries)
echo "sntp-off: the peer saw $queries SNTP requests"
[ "$queries" = 0 ] || { echo "sntp-off: SNTP requests with ntp = off"; ok=0; }
if grep -a "\[sntp\]" "$out/sntp-off.log"; then
    echo "sntp-off: sntp ran"
    ok=0
fi

if [ $ok = 1 ]; then
    echo "sntp-test: PASS"
    exit 0
fi
echo "sntp-test: FAIL (logs in $out)"
exit 1
