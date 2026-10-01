#!/bin/sh
# The HD Audio output stream (drivers/hda, stream.c and irq.c) in QEMU:
# intel-hda (8086:2668) with an hda-output codec whose samples go to a
# WAV file (QEMU's wav audio backend at 48 kHz 16-bit stereo; the codec's
# mixer=off, so its amplifiers don't touch the samples). The shell runs
# `hdatest` (tools/shell-tests/hdastream.txt), which opens the stream,
# plays a known pattern, closes a running stream and kills the driver
# mid-stream. Then:
#   - hdatest's own checks passed (the position's rate, close, kill and
#     the DMA quarantine: its result line);
#   - the WAV file holds the pattern sample for sample after the leading
#     silence, then silence for most of a ring (the driver's clear-behind:
#     without it the ring's last 16384 frames would play again);
#   - the driver's log: the stream closed by the client, stopped at the
#     kill's restart ("was running: stopping it"), never found running
#     at an open;
#   - the codec got no SET verb but the silent ones of the path set up
#     muted, the converter's format (0x200) and stream (0x706), and the
#     path opened only while a stream runs and closed again (the state
#     tools/hda-verbs.awk follows).
# QEMU_SMP passes through. Usage: tools/hda-stream-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
wav="$out/hda-stream.wav"
rm -f "$wav"
devs="-audiodev wav,id=snd0,path=$wav,out.frequency=48000,out.channels=2,out.format=s16 \
-device intel-hda,id=hda0 -device hda-output,bus=hda0.0,cad=0,audiodev=snd0,mixer=off,debug=3"
ok=1

QEMU_TIMEOUT=${QEMU_TIMEOUT:-200} QEMU_EXTRA="$devs" QEMU_INPUT=tools/shell-tests/hdastream.txt \
    tools/qemu-test.sh "$out" hda-stream shell > "$out/hda-stream.out" 2>&1 ||
    { echo "hda-stream: the script failed"; grep "serial-feed: .*no '" "$out/hda-stream.out"; ok=0; }
log="$out/hda-stream.log"
grep -E "hdatest: [0-9]+ passed" "$log" | tail -1
grep -qE "hdatest: 4 passed$|hdatest: 4 passed\r" "$log" ||
    { echo "hda-stream: hdatest did not pass 4 of 4"; grep "FAILED" "$log" | head -5; ok=0; }
for want in "stream: open on descriptor 4, tag 1, format 0x0011 (48 kHz 16-bit stereo), converter 0/02" \
            "stream: closed (the client closed its channel)" \
            "stream 4 was running: stopping it" \
            "restarted (restart 1 since boot)"; do
    grep -qF -- "$want" "$log" || { echo "hda-stream: no line with \"$want\""; ok=0; }
done
if grep -q "was running at open" "$log"; then
    echo "hda-stream: a stream was still running when the next one opened"
    ok=0
fi

# The codec's verbs (tools/hda-verbs.awk): GETs, the silent SETs of the
# path set up muted at start, the converter's format and stream, and the
# path opened while hdatest's streams run: every open verb sent while the
# converter has the stream's tag, nothing open when the tag goes back to
# 0 (the close), and nothing open at the end (the kill mid-stream leaves
# the path open; the restarted driver's set-up mutes it).
trace=$(awk -f tools/hda-verbs.awk "$out/hda-stream.out")
sets=$(echo "$trace" | grep -cE "^conv nid 2 " || true)
[ "$sets" -ge 4 ] || { echo "hda-stream: only $sets converter SET(s) on node 2 traced"; ok=0; }
opens=$(echo "$trace" | grep -c "^open " || true)
[ "$opens" -ge 2 ] || { echo "hda-stream: only $opens verb(s) opened the path"; ok=0; }
bad=$(echo "$trace" | grep -E "^bad " || true)
if [ -n "$bad" ]; then
    echo "hda-stream: verbs that are not on the driver's allow-list:"
    echo "$bad" | head -10
    ok=0
fi
state=$(echo "$trace" | tail -1)
[ "$state" = "state untagged 0 released 0 left 0" ] ||
    { echo "hda-stream: the path was open outside a stream ($state)"; ok=0; }
echo "hda-stream: codec: $(echo "$trace" | tail -2 | head -1 | sed 's/^total //'); $state"

python3 - "$wav" <<'PY' || ok=0
import struct, sys
data = open(sys.argv[1], "rb").read()
# The data chunk; its size field may be 0 if QEMU did not finish the file.
i = data.find(b"data")
if data[:4] != b"RIFF" or i < 0:
    sys.exit("hda-stream: %s is not a WAV file" % sys.argv[1])
fmt = data.find(b"fmt ")
ch, rate, bits = struct.unpack_from("<H", data, fmt + 10)[0], \
    struct.unpack_from("<I", data, fmt + 12)[0], struct.unpack_from("<H", data, fmt + 22)[0]
if (ch, rate, bits) != (2, 48000, 16):
    sys.exit("hda-stream: WAV is %d ch %d Hz %d-bit, want 2/48000/16" % (ch, rate, bits))
pcm = data[i + 8:]
n = len(pcm) // 4
fr = struct.unpack_from("<%dh" % (2 * n), pcm)
PATTERN, RING = 48000, 16384
first = next((k for k in range(n) if fr[2 * k] or fr[2 * k + 1]), None)
if first is None:
    sys.exit("hda-stream: the WAV file (%d frames) is all silence" % n)
bad = 0
for k in range(PATTERN):
    j = first + k
    want = (1 + k % 30000, -1 - k % 20000)
    got = (fr[2 * j], fr[2 * j + 1]) if j < n else None
    if got != want:
        if bad < 3:
            print("hda-stream: frame %d is %s, want %s" % (k, got, want))
        bad += 1
if bad:
    sys.exit("hda-stream: %d of %d pattern frames differ (the pattern starts at frame %d)"
             % (bad, PATTERN, first))
end = first + PATTERN
quiet = 0
while end + quiet < n and not fr[2 * (end + quiet)] and not fr[2 * (end + quiet) + 1]:
    quiet += 1
print("hda-stream: WAV: %d frames of silence, the %d-frame pattern exact, then %d frames of "
      "silence" % (first, PATTERN, quiet))
if quiet < RING - 4096:
    sys.exit("hda-stream: only %d frames of silence after the pattern, want %d or more: the "
             "ring played again" % (quiet, RING - 4096))
PY

if [ $ok = 1 ]; then
    echo "hda-stream: PASS"
    exit 0
fi
echo "hda-stream: FAIL (see $out/hda-stream.log, $out/hda-stream.out, $wav)"
exit 1
