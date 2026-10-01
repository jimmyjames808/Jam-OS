#!/bin/sh
# The HD Audio probe (drivers/hda) in QEMU, two boots:
#   1. the shell (tools/shell-tests/hda.txt): each controller's dump at
#      boot, `hda` from the shell, `kill hda` and devmgr's restart;
#   2. the `init` run (utest, usbtest) with the same devices: devmgr stops
#      every driver at the end, and each hda must end cleanly (its
#      controller back in reset, exit 0, nothing left in init's root job).
# The devices: intel-hda (ICH6, 8086:2668) with hda-duplex (codec 0) and
# hda-output (codec 1), and ich9-intel-hda (8086:293e) with hda-micro.
# Each driver runs the path and jack self-tests on its fixtures (the jack
# one: the ALC897's jack table, the RIRB's demultiplexer, the debounce,
# unsolicited responses and the polling fallback against a fake codec,
# which also checks that jack code sends no verb but 0x708, 0x709 and
# GET_PIN_SENSE), logs each codec's
# path (DAC 02 -> pin 03 on all three) and sets the best one up, muted.
# The codecs log every verb they get (their debug=3), and every one must
# be a GET (0xf00-0xfff, or the 4-bit 0xa / 0xb) or one of the SETs that
# setting a path up muted uses: power D0 (0x705 payload 0), connection
# select (0x701), pin control with the output and headphone bits clear
# (0x707), and amp gain/mute with the mute bit set (the 4-bit 0x3, which
# QEMU prints as 0x300); tools/hda-verbs.awk sorts them. Besides those,
# the output stage (pin control with the output bits, EAPD on: the
# awk's "power") once the path's amps are muted, which the driver leaves
# on while it runs; on these codecs an amp mutes the path, so it lets
# nothing out. So no amp was unmuted; and since nothing opens a stream
# here, no converter format or stream tag was set either.
# (QEMU's codecs ignore the pin control: their output pins read back 0x40,
# output on, whatever is set, and the driver's log says so. The trace is
# what shows the driver asked for the output off.)
# QEMU_SMP passes through. Usage: tools/hda-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
devs="-audiodev none,id=snd0 \
-device intel-hda,id=hda0 -device hda-duplex,bus=hda0.0,cad=0,audiodev=snd0,debug=3 \
-device hda-output,bus=hda0.0,cad=1,audiodev=snd0,debug=3 \
-device ich9-intel-hda,id=hda1 -device hda-micro,bus=hda1.0,audiodev=snd0,debug=3"
ok=1

QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_EXTRA="$devs" QEMU_INPUT=tools/shell-tests/hda.txt \
    tools/qemu-test.sh "$out" hda-shell shell > "$out/hda-shell.out" 2>&1 ||
    { echo "hda-shell: the script failed"; grep "serial-feed: .*no '" "$out/hda-shell.out"; ok=0; }
log="$out/hda-shell.log"
for want in "controller 8086:2668" "controller 8086:293e" \
            "codec 0: 1af4:0022" "codec 1: 1af4:0012" "codec 0: 1af4:0032" \
            "commands through CORB/RIRB (256/256 entries)" \
            "c0 03   cfg 00004010: jack ext green line-out" "c0 03 pin      caps 00400101 2ch conn 02" \
            "0 verb(s) timed out, 0 unsolicited response(s)" \
            "path self-test: 8 of 8 fixture(s) passed" \
            "codec 1 path: dac 02 -> pin 03 (line-out)" "codec 0 path: dac 02 -> pin 03 (speaker)" \
            "path: codec 0 dac 02 -> pin 03 (line-out), muted: afg D0; dac 02 out m0; pin 03 ctl 40" \
            "path: codec 0 dac 02 -> pin 03 (speaker), muted: afg D0; dac 02 out m0; pin 03 ctl 40" \
            "path: pin 03 kept its output on (the codec ignores its pin control)" \
            "path: the output stage (pin 03's output, EAPD) on with every amp muted" \
            "hda: path: codec 0 dac 02 -> pin 03 (line-out), muted:" \
            "commands through CORB/RIRB, " "path 02-03 muted" \
            "jack self-test: passed (table, RIRB, debounce, unsolicited and polled)" \
            "jacks: none with presence detection: nothing to watch"; do
    grep -qF -- "$want" "$log" || { echo "hda-shell: no line with \"$want\""; ok=0; }
done
n=$(grep -c "codec(s) answered" "$log" || true)
[ "$n" -ge 5 ] || { echo "hda-shell: $n full dump(s), want 5 (2 at boot, 2 from hda, 1 restart)"; ok=0; }
# The RIRB interrupt is on (for unsolicited responses) while commands are
# still polled: the dumps from the shell must still be right (0 timeouts,
# above) and the interrupt must have been taken.
grep -qE "hda: jack 0 jack\(s\) .*unsolicited responses on \(0 received, [1-9][0-9]* RIRB" "$log" ||
    { echo "hda-shell: hda jacks shows no RIRB interrupt taken"; ok=0; }
if grep -E "jack self-test: [^p]" "$log"; then
    echo "hda-shell: the jack self-test failed"
    ok=0
fi
if grep -E "path self-test: [^8]|parses back to another path|path: none" "$log"; then
    echo "hda-shell: a path self-test or round trip failed"
    ok=0
fi

# Every verb the codecs got (their debug lines on QEMU's stderr) is a GET
# or a silent SET (tools/hda-verbs.awk); every path set-up sent at least
# the pin control and a mute.
trace=$(awk -v stage=1 -f tools/hda-verbs.awk "$out/hda-shell.out")
verbs=$(grep -c "hda_audio_command: nid" "$out/hda-shell.out" || true)
[ "$verbs" -gt 100 ] || { echo "hda-shell: only $verbs codec verb(s) traced"; ok=0; }
bad=$(echo "$trace" | grep -E "^(conv|open|bad) " || true)
if [ -n "$bad" ]; then
    echo "hda-shell: verbs that are neither GETs nor silent SETs:"
    echo "$bad" | head -10
    ok=0
fi
# QEMU's codecs have no pin with presence detection: no jack verb at all.
jackv=$(echo "$trace" | grep -c "^jack " || true)
[ "$jackv" = 0 ] ||
    { echo "hda-shell: $jackv jack verb(s) (0x708/0x709) sent to QEMU's codecs"; ok=0; }
# QEMU's codecs here keep their mixer, so an amp mutes the path: the
# output stage's verbs are "power" (-v stage=1), not "open".
pinctl=$(echo "$trace" | grep -c "^silent .* verb 0x707 " || true)
mutes=$(echo "$trace" | grep -c "^silent .* verb 0x300 " || true)
counts="$(echo "$trace" | tail -2 | head -1 | sed 's/^total //'); pinctl $pinctl mutes $mutes;"
counts="$counts $(echo "$trace" | tail -3 | head -1)"
# 3 path set-ups (2 at boot, 1 after the restart), each one pin control
# and at least one mute (the DAC's output amp).
[ "$pinctl" -ge 3 ] && [ "$mutes" -ge 3 ] || { echo "hda-shell: too few path SETs ($counts)"; ok=0; }

QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_EXTRA="$devs" \
    tools/qemu-test.sh "$out" hda-init init > "$out/hda-init.out" 2>&1 || true
log="$out/hda-init.log"
n=$(grep -c "\[hda\] stopped: controller back in reset (client closed)" "$log" || true)
[ "$n" = 2 ] || { echo "hda-init: $n clean stop(s), want 2"; ok=0; }
if grep -E "drv/hda (did not end cleanly|crashed|was killed)|process \"hda\" killed" "$log"; then
    echo "hda-init: the hda driver did not end cleanly"
    ok=0
fi
grep -q "root job afterwards: .*(clean)" "$log" || { echo "hda-init: init's root job not clean"; ok=0; }
if grep -q "run complete: no problems" "$log"; then
    echo "hda-init: run complete: no problems"
else
    echo "hda-init: the run had problems:"
    grep "devmgr: .* did not end cleanly" "$log" | head -3
    ok=0
fi

if [ $ok = 1 ]; then
    echo "hda: PASS ($verbs verbs: all GETs or silent SETs; $counts)"
    exit 0
fi
echo "hda: FAIL (see $out/hda-shell.log, $out/hda-init.log)"
exit 1
