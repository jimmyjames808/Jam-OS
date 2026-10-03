#!/bin/sh
# The mixer killed while it plays is not seen by its clients
# (docs/M11.6-PLAN.md, "The demonstration and its tests";
# user/services/mixer/adopt.c). Two boots of tools/shell-tests/mixer-restart.txt,
# each with intel-hda and an hda-output codec (mixer=off) whose samples
# QEMU writes to a WAV file: one with the warm spare, one with `nospare`.
# In each, bin/mixramp plays a 16-bit ramp (frame k is k + 1, both
# channels) at 0 dB while it has init kill the mixer 20 times, 300 ms
# apart; then:
#   - mixramp says every call it made succeeded (no client saw the kills);
#   - the log has one "mixer: restart (killed" line per kill, each saying
#     how much written-ahead audio was left (the lead), and no restart that
#     fell back to a fresh start, no output closed or reopened;
#   - the WAV is the ramp, whole: from 1 to the ramp's last frame, every
#     frame one more than the last, both channels the same: no frame
#     missing, repeated or silent, except where QEMU's own trace
#     (hda_audio_overrun) says it dropped a buffer of 2048 frames on a busy
#     host (each such drop is a jump of exactly 2048 frames, counted).
# mixramp also keeps the mixer busy with other calls (volume, position, a
# stream opened and closed), so some kills land while a request is in
# progress; each restart's line says what it found and did, and every call
# must still succeed. Then it prints the lead left at the restarts,
# kill-to-first-answer (mixramp's own, and init's probe) with and without
# the spare, and what the restarts found in progress.
# QEMU_SMP passes through. Usage: tools/mixer-restart-test.sh <outdir>;
# exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
kills=20
ok=1

run() {   # run <name> [boot words]: one boot; its WAV is $out/<name>.wav
    name=$1
    shift
    wav="$out/$name.wav"
    rm -f "$wav"
    devs="-audiodev wav,id=snd0,path=$wav,out.frequency=48000,out.channels=2,out.format=s16 \
-device intel-hda,id=hda0 -device hda-output,bus=hda0.0,cad=0,audiodev=snd0,mixer=off \
-trace hda_audio_overrun"
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_EXTRA="$devs" \
        QEMU_INPUT=tools/shell-tests/mixer-restart.txt \
        tools/qemu-test.sh "$out" "$name" shell "$@" > "$out/$name.out" 2>&1 ||
        { echo "mixer-restart: $name: the script failed"; grep "serial-feed: .*no '" "$out/$name.out" || true; ok=0; }
}

count() { grep -ac -- "$1" "$2" || true; }
need() {   # need <n> <text> <log>: exactly n lines
    n=$(count "$2" "$3")
    [ "$n" -eq "$1" ] || { echo "mixer-restart: $(basename "$3"): \"$2\" $n times, wanted $1"; ok=0; }
}
never() {   # never <text> <log>
    n=$(count "$1" "$2")
    [ "$n" -eq 0 ] || { echo "mixer-restart: $(basename "$2"): \"$1\" $n times, wanted none"; ok=0; }
}
stats() {   # stats <label> <pattern before the number> <unit> <log>: median and worst
    grep -ao -- "$2 [0-9]* $3" "$4" | awk '{print $(NF-1)}' | sort -n |
        awk -v l="$1" -v u="$3" '{v[NR]=$1} END {if (NR) printf "mixer-restart: %s: %d samples, least %d %s, median %d %s, worst %d %s\n", l, NR, v[1], u, v[int((NR+1)/2)], u, v[NR], u}'
}

check() {   # check <name>: the log and the WAV of one boot
    name=$1
    log="$out/$name.log"
    tr -d '\r' < "$log" | grep -a "mixramp: " || true
    need 1 "mixramp: every call succeeded" "$log"
    need "$kills" "mixer: restart (killed" "$log"
    need "$kills" "mixer: restart (killed.*output running, lead left" "$log"
    never "mixer: restart (crashed" "$log"
    never "mixer: the saved numbers are refused" "$log"
    never "svcstate: the state of service" "$log"
    never "what the keeper kept couldn't be taken back" "$log"
    never "the driver's stream can't be taken over" "$log"
    never "mixer: output closed (the driver" "$log"
    never "keep: refused" "$log"
    never "can't be handed over" "$log"
    # One output for the whole ramp: opened once, closed once at its end.
    need 1 "mixer: output open:" "$log"
    stats "$name: lead left at the restarts" "lead left" "frames" "$log"
    grep -ao "lead left [0-9]* frames ([0-9]* ms)" "$log" | awk '{print $5}' | tr -d '(' |
        sort -n | awk -v l="$name" '{v[NR]=$1} END {if (NR) printf "mixer-restart: %s: lead left in ms: least %d, median %d, most %d\n", l, v[1], v[int((NR+1)/2)], v[NR]}'
    stats "$name: init's kill to first answer" "kill to first answer" "us" "$log"
    echo "mixer-restart: $name: what the restarts found in progress:"
    tr -d '\r' < "$log" | grep -ao "mixer: restart (killed.*request in progress: [^;]*" |
        sed 's/.*request in progress: //' | sort | uniq -c | sed 's/^/    /'
    never "mixer: a request (ordinal" "$log"
    frames=$(grep -ao "mixramp: ramp of [0-9]* frames" "$log" | head -1 | awk '{print $4}')
    drops=$(grep -ac "hda_audio_overrun" "$out/$name.out" || true)
    [ "$drops" = 0 ] || echo "mixer-restart: $name: QEMU dropped its codec buffer $drops time(s) (the host was late)"
    [ -n "$frames" ] || { echo "mixer-restart: $name: mixramp didn't say how long its ramp was"; ok=0; return; }
    python3 - "$out/$name.wav" "$frames" "$drops" "$name" <<'PY' || ok=0
import struct, sys
path, frames, drops, name = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
data = open(path, "rb").read()
i = data.find(b"data")
if data[:4] != b"RIFF" or i < 0:
    sys.exit("mixer-restart: %s: %s is not a WAV file" % (name, path))
pcm = data[i + 8:]
n = len(pcm) // 4
fr = struct.unpack_from("<%dh" % (2 * n), pcm)
left, right = fr[0::2], fr[1::2]
if left != right:
    k = next(k for k in range(n) if left[k] != right[k])
    sys.exit("mixer-restart: %s: the channels differ first at frame %d" % (name, k))
start = next((k for k in range(n) if left[k]), None)
if start is None:
    sys.exit("mixer-restart: %s: the WAV is silent" % name)
last = (frames + 32768) % 65536 - 32768        # the ramp's last frame, as an s16
faults, skipped, k = [], 0, start + 1
while k < n:
    d = (left[k] - left[k - 1]) % 65536
    if d == 1:
        k += 1
        continue
    if left[k - 1] == last and not any(left[k:k + 4800]):
        break                                   # the ramp's end, then silence
    if d == 2049 and skipped < drops:
        skipped += 1                            # a QEMU drop: 2048 frames skipped
    else:
        faults.append((k, left[k - 1], left[k]))
    k += 1
seen = k - start
fails = []
if left[start] != 1:
    fails.append("the ramp starts at %d, not 1" % left[start])
if k >= n:
    fails.append("the ramp's end (%d) was never reached" % last)
if faults:
    fails.append("%d break(s) in the ramp, the first at frame %d (%d then %d)"
                 % ((len(faults),) + faults[0]))
want = frames - 2048 * skipped
if not fails and seen != want:
    fails.append("%d frames of ramp, want %d" % (seen, want))
if fails:
    sys.exit("mixer-restart: %s: %s" % (name, "; ".join(fails)))
print("mixer-restart: %s: the WAV holds the whole ramp, exact: %d frames (%.2f s)%s"
      % (name, seen, seen / 48000.0,
         ", %d QEMU drop(s) of 2048 frames" % skipped if skipped else ""))
PY
}

run spare
run nospare nospare
check spare
check nospare
need 1 "init: nospare: no warm spare" "$out/nospare.log"
never "spare promoted" "$out/nospare.log"
need "$kills" "init: bin/mixer: spare promoted" "$out/spare.log"

[ "$ok" = 1 ] && echo "mixer-restart: PASS" && exit 0
echo "mixer-restart: FAIL (see $out/*.log, $out/*.out, $out/*.wav)"
exit 1
