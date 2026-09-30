#!/bin/sh
# The HD Audio probe (drivers/hda) in QEMU, two boots:
#   1. the shell (tools/shell-tests/hda.txt): each controller's dump at
#      boot, `hda` from the shell, `kill hda` and devmgr's restart;
#   2. the `init` run (utest, usbtest) with the same devices: devmgr stops
#      every driver at the end, and each hda must end cleanly (its
#      controller back in reset, exit 0, nothing left in init's root job).
# The devices: intel-hda (ICH6, 8086:2668) with hda-duplex (codec 0) and
# hda-output (codec 1), and ich9-intel-hda (8086:293e) with hda-micro. The
# codecs log every verb they get (their debug=3), and every one must be a
# GET (0xf00-0xfff, or the 4-bit 0xa / 0xb): the probe sets nothing.
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
            "0 verb(s) timed out, 0 unsolicited response(s)"; do
    grep -qF -- "$want" "$log" || { echo "hda-shell: no line with \"$want\""; ok=0; }
done
n=$(grep -c "codec(s) answered" "$log" || true)
[ "$n" -ge 5 ] || { echo "hda-shell: $n full dump(s), want 5 (2 at boot, 2 from hda, 1 restart)"; ok=0; }

# Every verb the codecs got (their debug lines on QEMU's stderr) is a GET.
verbs=$(grep -c "hda_audio_command: nid" "$out/hda-shell.out" || true)
bad=$(grep "hda_audio_command: nid" "$out/hda-shell.out" | grep -vE "verb 0x(f[0-9a-f]{2}|[ab][0-9a-f]{2})," || true)
[ "$verbs" -gt 100 ] || { echo "hda-shell: only $verbs codec verb(s) traced"; ok=0; }
if [ -n "$bad" ]; then
    echo "hda-shell: verbs that are not GETs:"
    echo "$bad" | head -10
    ok=0
fi

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
# The run's own verdict is reported, not required: it ends "with problems"
# about one run in three on main too, from a fat-esp shutdown race
# (docs/ROADMAP.md, smaller follow-ups).
if grep -q "run complete: no problems" "$log"; then
    echo "hda-init: run complete: no problems"
else
    echo "hda-init: note: the run had problems:"
    grep "devmgr: .* did not end cleanly" "$log" | head -3
fi

if [ $ok = 1 ]; then
    echo "hda: PASS ($verbs verbs, all GETs)"
    exit 0
fi
echo "hda: FAIL (see $out/hda-shell.log, $out/hda-init.log)"
exit 1
