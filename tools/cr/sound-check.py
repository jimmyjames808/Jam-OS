#!/usr/bin/env python3
"""cr-test.sh's sound check (docs/CR-PLAN.md section 11, CA1): what
tools/cr/sound.html played through Jam OS's mixer, as QEMU's intel-hda
wrote it into a WAV file (48 kHz 16-bit stereo, the codec's mixer off, so
the file holds exactly what the mixer wrote), and the run's log.

    tools/cr/sound-check.py <wav> <log> <qemu output>

- The pattern (the page's xorshift32 values, 4 s of stereo at 48 kHz, through
  Web Audio): its frames are found in the capture by pairs of frames
  (unique in the pattern); none may come twice or out of order, the first
  and the last must be there, and between them the only break allowed is
  one jump forward (the context's suspend: Chromium stops the stream and
  what was queued is dropped) besides QEMU's own drops (hda_audio_overrun
  in QEMU's output: 2048 frames gone with no silence, a busy host's),
  CR_SOUND_LATE (20) buffers of silence in place of the same number of the
  pattern's frames (the audio service's sync reader waits 95 % of a buffer
  for the renderer and plays silence for a late one, Chromium's way: on a
  busy host the renderer's thread is sometimes late) and CR_SOUND_GAPS (0)
  runs of silence inside it (the mixer ran short).
- The tones (the capture's other sounds, cut at 50 ms of silence): one at
  440 Hz (the OscillatorNode) of exactly 1.5 s at a quarter of full scale
  (within 3 %), and the <audio> file's 1000 Hz at a quarter of full scale
  (within 10 %: Opus), in two pieces (the pause) that make up the file's
  3 s, none of it twice (within 20 ms).
- The log: the context ran at 48 kHz; the oscillator was heard within
  600 ms of its start() (by the context's output timestamp, which is the
  back end's delay); every one of the back end's lines (--vmodule) came from
  a process that isn't the browser (the console's lines are the browser's),
  so the audio service ran out of process; each of the mixer's "chromium"
  streams closed with CR_SOUND_UNDERRUNS (2) underruns at most.
Prints what it found; exit 0 if all is as above.
"""
import os
import re
import sys

import numpy as np

RATE = 48000
FRAMES = 4 * RATE
AMP = 8192


def pattern():
    x = 0x9E3779B9
    out = np.empty(2 * FRAMES, np.int32)
    for j in range(2 * FRAMES):
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        v = (x >> 17) - 16384
        out[j] = v if v else 1
    return out.reshape(FRAMES, 2)


def pair_keys(f):
    """A key for each frame and the one after it."""
    u = (f.astype(np.int64) & 0xFFFF).astype(np.uint64)
    one = (u[:, 0] << np.uint64(16)) | u[:, 1]
    return (one[:-1] << np.uint64(32)) | one[1:]


def segments(c):
    """Runs of sound: [a, b) separated by 50 ms or more of zeros."""
    loud = np.flatnonzero(np.any(c != 0, axis=1))
    if len(loud) == 0:
        return []
    cuts = np.flatnonzero(np.diff(loud) > RATE // 20)
    starts = np.concatenate(([loud[0]], loud[cuts + 1]))
    ends = np.concatenate((loud[cuts] + 1, [loud[-1] + 1]))
    return list(zip(starts.tolist(), ends.tolist()))


def tone(x):
    """The strongest frequency of x and its amplitude (a Hann window, the
    peak's bin refined by a parabola, then the amplitude at it)."""
    n = len(x)
    w = np.hanning(n)
    size = 1 << (8 * n - 1).bit_length()
    spec = np.abs(np.fft.rfft(x * w, size))
    k = int(np.argmax(spec[1:])) + 1
    a, b, c = np.log(spec[k - 1:k + 2] + 1e-9)
    k += 0.5 * (a - c) / (a - 2 * b + c)
    hz = k * RATE / size
    t = np.arange(n) / RATE
    amp = 2 * abs(np.sum(x * w * np.exp(-2j * np.pi * hz * t))) / np.sum(w)
    return hz, amp


def main():
    wav, log, qout = sys.argv[1:4]
    gaps_ok = int(os.environ.get("CR_SOUND_GAPS", "0"))
    under_ok = int(os.environ.get("CR_SOUND_UNDERRUNS", "2"))
    late_ok = int(os.environ.get("CR_SOUND_LATE", "20"))
    ok = True

    def bad(why):
        nonlocal ok
        print("sound-check: FAIL: " + why)
        ok = False

    data = open(wav, "rb").read()
    at = data.find(b"data")
    if data[:4] != b"RIFF" or at < 0:
        print("sound-check: FAIL: %s is not a WAV file" % wav)
        return 1
    pcm = np.frombuffer(data[at + 8:at + 8 + (len(data) - at - 8) // 4 * 4], dtype="<i2")
    cap = pcm.reshape(-1, 2).astype(np.int32)
    text = open(log, "rb").read().decode("utf-8", "replace")
    drops = open(qout, "rb").read().decode("utf-8", "replace").count("hda_audio_overrun")
    print("sound-check: WAV: %.2f s; QEMU dropped its buffer %d time(s)" % (len(cap) / RATE, drops))

    # The pattern.
    pat = pattern()
    pk = pair_keys(pat)
    order = np.argsort(pk, kind="stable")
    sk = pk[order]
    assert len(np.unique(sk)) == len(sk), "the pattern's pairs repeat"
    ck = pair_keys(cap)
    pos = np.searchsorted(sk, ck).clip(0, len(sk) - 1)
    hit = sk[pos] == ck
    ci = np.flatnonzero(hit)
    kk = order[pos[hit]]
    in_pattern = np.zeros(len(cap), bool)
    if len(ci) == 0:
        bad("none of the pattern's frames reached the capture")
    else:
        in_pattern[ci[0]:ci[-1] + 2] = True
        twice = len(kk) - len(np.unique(kk))
        print("sound-check: pattern: %d of %d frames heard, from frame %d to %d"
              % (len(kk) + 1, FRAMES, kk[0], kk[-1] + 1))
        if twice:
            bad("%d of the pattern's frames were played twice" % twice)
        if np.any(np.diff(kk) <= 0):
            bad("the pattern's frames were played out of order")
        if kk[0] != 0 or kk[-1] != FRAMES - 2:
            bad("the pattern's first or last frame is missing")
        # A break: the pair across it matches nothing, so frame k (the last
        # before it) is followed by `quiet` zeros and then frame k + 1 +
        # `skipped`.
        di, dk = np.diff(ci), np.diff(kk)
        skips, gaps, late, dropped = [], [], [], 0
        for j in np.flatnonzero((di != 1) | (dk != 1)):
            k, skipped, quiet = int(kk[j]) + 1, int(dk[j]) - 2, int(di[j]) - 2
            if skipped == 2048 and quiet == 0 and dropped < drops:
                dropped += 1
            elif 0 < skipped == quiet <= 1024:
                late.append((k, quiet))
            elif skipped > 0:
                skips.append((k, skipped, quiet))
            elif quiet > 0:
                gaps.append((k, quiet))
        for k, skipped, quiet in skips:
            print("sound-check: pattern: after frame %d, %d frames (%.0f ms) not heard, %d of silence"
                  % (k, skipped, skipped * 1000 / RATE, quiet))
        if dropped:
            print("sound-check: pattern: %d of QEMU's drops (2048 frames each) inside it" % dropped)
        for k, quiet in late:
            print("sound-check: pattern: after frame %d, a buffer of silence (%d frames) in place of "
                  "the renderer's (it was late)" % (k, quiet))
        if len(late) > late_ok:
            bad("the renderer was late %d times (CR_SOUND_LATE: %d)" % (len(late), late_ok))
        if len(skips) > 1:
            bad("%d jumps in the pattern (one is the suspend's)" % len(skips))
        for k, quiet in gaps:
            print("sound-check: pattern: %d frames of silence after frame %d" % (quiet, k))
        if len(gaps) > gaps_ok:
            bad("%d runs of silence inside the pattern (the mixer ran short)" % len(gaps))

    # The tones.
    osc, audio = [], []
    for a, b in segments(cap):
        if in_pattern[a:b].mean() > 0.5:
            continue
        n = b - a
        x = cap[a:b, 0].astype(float)
        trim = max(n // 10, 240) if n > 4800 else 0
        hz, amp = tone(x[trim:n - trim] if trim else x)
        side = int(np.max(np.abs(cap[a:b, 0] - cap[a:b, 1])))
        print("sound-check: a sound at %.1f s: %.3f s of %.1f Hz, amplitude %.0f, channels apart by %d"
              % (a / RATE, n / RATE, hz, amp, side))
        if abs(hz - 440) < 5:
            osc.append((n, hz, amp, side))
        elif abs(hz - 1000) < 5:
            audio.append((n, hz, amp, side))
        else:
            bad("a sound that is neither tone (%.1f Hz)" % hz)
    if len(osc) != 1:
        bad("%d pieces of the oscillator's 440 Hz, not one" % len(osc))
    else:
        n, hz, amp, side = osc[0]
        if abs(n - 72000) > 48 or abs(hz - 440) > 1 or abs(amp - AMP) > 0.03 * AMP or side > 2:
            bad("the oscillator: %d frames (72000), %.2f Hz (440), amplitude %.0f (%d), channels apart %d"
                % (n, hz, amp, AMP, side))
    if not 1 <= len(audio) <= 2:
        bad("%d pieces of the <audio> file's 1000 Hz, not two" % len(audio))
    else:
        total = sum(p[0] for p in audio) / RATE
        print("sound-check: <audio>: %d piece(s), %.3f s in all (the file: 3 s)" % (len(audio), total))
        if not 2.9 <= total <= 3.02:
            bad("the <audio> file's pieces make %.3f s, not its 3 s" % total)
        for n, hz, amp, side in audio:
            if abs(hz - 1000) > 2 or abs(amp - AMP) > 0.1 * AMP:
                bad("the <audio> file's tone: %.2f Hz (1000), amplitude %.0f (%d)" % (hz, amp, AMP))

    # The log.
    said = re.findall(r"crsnd: ([^\"\r\n]*)", text)
    for s in dict.fromkeys(said):
        print("sound-check: page said: " + s)
    m = re.search(r"crsnd: context (\d+) Hz (\w+)", text)
    if not m or m.group(1) != "48000" or m.group(2) != "running":
        bad("the context did not run at 48000 Hz")
    m = re.search(r"crsnd: osc latency (-?\d+) ms", text)
    if not m or not 0 <= int(m.group(1)) <= 600:
        bad("the oscillator's start latency: %s ms (0 to 600)" % (m.group(1) if m else "none"))
    console = set(re.findall(r"\[(\d+):\d+:[^\]]*:CONSOLE[^\]]*\] \"crsnd", text))
    backend = re.findall(r"\[(\d+):\d+:[^\]]*audio_output_stream_jamos\.cc[:(]\d+\)?\] (jamos audio: [^\r\n]*)",
                         text)
    for pid, line in backend:
        print("sound-check: process %s: %s" % (pid, line))
    if not backend:
        bad("no line of the back end's (--vmodule)")
    elif console & {p for p, _ in backend}:
        bad("a stream in the browser's process (%s): the audio service ran in it" % ", ".join(console))
    else:
        print("sound-check: the streams were in process %s, the browser is %s"
              % (", ".join(sorted({p for p, _ in backend})), ", ".join(console)))
    closed = re.findall(r"mixer: stream \d+ \(chromium\) closed[^:]*: (\d+) frames taken, (\d+) underrun", text)
    for frames, under in closed:
        print("sound-check: a chromium stream closed: %s frames taken, %s underrun(s)" % (frames, under))
    if not closed:
        bad("the mixer logged no chromium stream's close")
    elif any(int(u) > under_ok for _, u in closed):
        bad("a stream ran short more than %d times" % under_ok)

    print("sound-check: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
