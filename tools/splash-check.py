#!/usr/bin/env python3
"""Check tools/splash-test.sh's screenshots and sound captures against the
boot splash's own file (boot/splash.mpg, decoded here with ffmpeg).

    splash-check.py quiet <png>             the whole screen is #1E1A1D
    splash-check.py frames <png> <png>      two screenshots of the animation,
                                            a second apart: each matches a
                                            frame of the video (placed as
                                            bin/splash places it: scaled to
                                            fit, centred, the rest #1E1A1D),
                                            in order
    splash-check.py frame <png> <width>     one screenshot: a frame of the
                                            video, drawn that wide
    splash-check.py text <png>              the console's text screen: its
                                            background with text on it
    splash-check.py red <png>               a panic screen (dark red)
    splash-check.py alpha <png>             `run splash --alpha`: the seven
                                            drupelets' colours at their
                                            centres, the middle one's edge
                                            partly covered (anti-aliased)
    splash-check.py sound <wav> <join ms>   the capture is the video's sound
                                            from <join ms> on (aligned to the
                                            video by the file's time stamps),
                                            to its end, 30 dB above the
                                            difference (two MP2 decoders,
                                            pl_mpeg's and ffmpeg's, differ a
                                            little)
    splash-check.py skipped <wav> <join ms> the same, cut short, faded out

Prints one line per check; exits 1 on a failure."""
import subprocess
import sys

import numpy as np
from PIL import Image

MPG = "boot/splash.mpg"
BG = np.array([0x1E, 0x1A, 0x1D])
RATE = 48000
SMALL = (320, 180)   # frames are compared at this size


def fail(msg):
    print("splash-check: FAIL: " + msg)
    sys.exit(1)


def ok(msg):
    print("splash-check: " + msg)


def shot(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(np.int16)


def ref_frames():
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", MPG, "-f", "rawvideo", "-pix_fmt",
                          "rgb24", "-vf", "scale=%d:%d:flags=area" % SMALL, "-"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, SMALL[1], SMALL[0], 3).astype(np.int16)


def video_size():
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                          "stream=width,height", "-of", "csv=p=0", MPG],
                         capture_output=True, check=True, text=True).stdout
    w, h = out.strip().split(",")[:2]
    return int(w), int(h)


def layout(sw, sh):
    """Where bin/splash puts the video on a sw x sh screen (video.c's rules):
    an integer scale up when it fits, else down to fit (an integer n as a
    box, any other ratio bilinear). (x, y, width, height)."""
    vw, vh = video_size()
    if vw <= sw and vh <= sh:
        k = min(sw // vw, sh // vh, 8)
        ow, oh = vw * k, vh * k
    else:
        s = min((sw << 16) // vw, (sh << 16) // vh)
        ow, oh = (vw * s) >> 16, (vh * s) >> 16
        n = vw // max(ow, 1)
        if (n >= 2 and vw % n == 0 and vh % n == 0 and vw // n <= sw and vh // n <= sh and
                vw // n >= ow - 1 and vh // n >= oh - 1):
            ow, oh = vw // n, vh // n
    return (sw - ow) // 2, (sh - oh) // 2, ow, oh


def video_part(img):
    """The picture where bin/splash puts it, at SMALL; the rest; its width."""
    h, w = img.shape[:2]
    x, y, ow, oh = layout(w, h)
    pic = img[y:y + oh, x:x + ow]
    small = np.asarray(Image.fromarray(pic.astype(np.uint8)).resize(SMALL, Image.BOX))
    mask = np.ones((h, w), bool)
    mask[y:y + oh, x:x + ow] = False
    return small.astype(np.int16), img[mask], ow


def match(frames, img):
    small, rest, k = video_part(img)
    err = np.abs(frames - small[None]).mean(axis=(1, 2, 3))
    i = int(err.argmin())
    border = float(np.abs(rest - BG).max()) if rest.size else 0.0
    return i, float(err[i]), border, k


def check_quiet(path):
    d = int(np.abs(shot(path) - BG).max())
    if d > 2:
        fail("%s: not all #1E1A1D (a pixel %d off)" % (path, d))
    ok("%s: the whole screen is the splash's background" % path)


def check_frames(a, b):
    frames = ref_frames()
    ia, ea, ba, k = match(frames, shot(a))
    ib, eb, bb, _ = match(frames, shot(b))
    ok("%s: frame %d (mean error %.2f), %s: frame %d (%.2f), %d wide" % (a, ia, ea, b, ib, eb, k))
    if ea > 4 or eb > 4:
        fail("a screenshot doesn't look like any frame of the video")
    if ba > 4 or bb > 4:
        fail("the screen around the video isn't #1E1A1D")
    num, den = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                               "stream=r_frame_rate", "-of", "csv=p=0", MPG], capture_output=True,
                              check=True, text=True).stdout.split()[0].strip(",").split("/")
    fps = float(num) / float(den)
    # QEMU's emulated CPU decodes a 2560x1440 frame in about 40 ms, slower
    # than the 33 ms it has: the picture runs behind the sound there (the PC
    # decodes one in a few ms). So: moving on, in order, not to the minute.
    if not 0.1 * fps <= ia <= 2.5 * fps or not 0.3 * fps <= ib - ia <= 3 * fps:
        fail("frames %d and %d: want the first 0.1-2.5 s in and the second 0.3-3 s later"
             % (ia, ib))


def check_frame(path, width):
    i, err, border, k = match(ref_frames(), shot(path))
    ok("%s: frame %d (mean error %.2f), %d wide" % (path, i, err, k))
    if err > 4 or border > 4 or k != width:
        fail("%s: not a frame of the video %d wide" % (path, width))


def check_text(path):
    img = shot(path)
    bg = np.array([0x10, 0x10, 0x18])
    share = float((np.abs(img - bg).max(axis=2) <= 2).mean())
    text = int((img.max(axis=2) > 128).sum())
    ok("%s: %.0f %% console background, %d bright pixels" % (path, share * 100, text))
    if share < 0.6 or text < 200:
        fail("%s: not the console's text screen" % path)


def check_red(path):
    img = shot(path)
    red = float(((img[:, :, 0] > 100) & (img[:, :, 1] < 40) & (img[:, :, 2] < 40)).mean())
    ok("%s: %.0f %% panic red" % (path, red * 100))
    if red < 0.5:
        fail("%s: not a panic screen" % path)


def check_alpha(path):
    img = shot(path)
    h, w = img.shape[:2]
    k = max(1, min(w // 1280, h // 720))
    ox, oy = (w - 1280 * k) / 2, (h - 720 * k) / 2
    cx, cy, r, d = ox + 456 * k, oy + 360 * k, 44 * k, 91 * k
    want = [0x8e1b3b, 0xa9234a, 0xc8274d, 0xd99f31, 0xc8274d, 0xa9234a, 0xc8274d]
    pts = [(cx, cy)] + [(cx + d * np.sin(i * np.pi / 3), cy - d * np.cos(i * np.pi / 3))
                        for i in range(6)]
    for (x, y), c in zip(pts, want):
        got = img[int(y), int(x)]
        rgb = np.array([c >> 16, c >> 8 & 0xff, c & 0xff])
        if np.abs(got - rgb).max() > 3:
            fail("%s: the drupelet at (%d, %d) is %s, want #%06x" % (path, x, y, got, c))
    edge = [int(img[int(cy), int(cx + r) + dx][0]) for dx in (-1, 0, 1)]
    ok("%s: seven drupelets; the middle one's edge, red: %s" % (path, edge))
    if not any(60 < e < 130 for e in edge):
        fail("%s: no partly covered pixel on the edge (not anti-aliased)" % path)


def wav_frames(path):
    data = open(path, "rb").read()
    i = data.find(b"data")
    if data[:4] != b"RIFF" or i < 0:
        fail("%s is not a WAV file" % path)
    pcm = np.frombuffer(data[i + 8:], np.int16)
    return pcm[:len(pcm) // 2 * 2].reshape(-1, 2).astype(np.int32)


def first_pts(stream):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", stream, "-show_entries",
                          "packet=pts_time", "-read_intervals", "%+#1", "-of", "csv=p=0", MPG],
                         capture_output=True, check=True, text=True).stdout
    return float(out.split()[0])


def ref_sound():
    """The video's sound with frame j at media time j / RATE: the decoder's
    delay (the stamps start that much before the video's) cut off, as the
    splash does."""
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", MPG, "-vn", "-f", "s16le", "-ac", "2",
                          "-ar", str(RATE), "-"], capture_output=True, check=True).stdout
    pcm = np.frombuffer(raw, np.int16).reshape(-1, 2).astype(np.int32)
    shift = int(round((first_pts("v") - first_pts("a")) * RATE))
    return pcm[shift:] if shift >= 0 else np.vstack([np.zeros((-shift, 2), np.int32), pcm])


def follow(x, r, at):
    """The capture x against the reference r from frame `at`, 100 ms at a
    time. QEMU's audio backend on a busy host can lose a period, which is
    not the guest's doing: a window that doesn't match where the last one
    did is set aside, and the next windows are looked for within 4096
    frames (two of the mixer's periods). Returns the SNR (after the best
    gain) over the windows that matched, the jumps (capture position,
    frames), the windows set aside, and where in r the capture ends."""
    n, off, slips, bad = RATE // 10, at, [], 0
    xs, ys = [], []

    def fits(p, q):
        g = float(np.dot(p, q)) / (float(np.dot(q, q)) + 1e-9)
        return len(q) == n and ((p - g * q) ** 2).sum() * 1000 < (q * q).sum() + n * 64

    for pos in range(0, len(x) - n + 1, n):
        p = x[pos:pos + n]
        q = r[off + pos:off + pos + n]
        if len(q) < n:
            break
        if not fits(p, q):
            moved = [d for d in range(-4096, 4097, 2) if off + pos + d >= 0 and d and
                     fits(p, r[off + pos + d:off + pos + d + n])]
            if not moved:
                bad += 1
                continue
            slips.append((pos, moved[0]))
            off += moved[0]
            q = r[off + pos:off + pos + n]
        xs.append(p)
        ys.append(q)
    X, Y = np.concatenate(xs), np.concatenate(ys)
    gain = float((X * Y).sum() / ((Y * Y).sum() + 1e-9))
    snr = 10 * np.log10((Y * Y).sum() / (((X - gain * Y) ** 2).sum() + 1e-9))
    return snr, gain, slips, bad, off + len(x)


def check_sound(path, join_ms, skipped):
    cap, ref = wav_frames(path), ref_sound()
    loud = np.nonzero(np.abs(cap).max(axis=1) > 64)[0]
    if not len(loud):
        fail("%s: silent" % path)
    cap = cap[loud[0]:loud[-1] + 1]   # QEMU records silence while the output stays open
    # Where in the video's sound the capture starts: the peak of their
    # cross-correlation (by FFT, every lag at once) over its first second.
    probe = cap[:RATE, 0].astype(np.float64)
    r = ref[:, 0].astype(np.float64)
    size = 1 << int(np.ceil(np.log2(len(r) + len(probe))))
    xc = np.fft.irfft(np.fft.rfft(r, size) * np.conj(np.fft.rfft(probe, size)), size)
    energy = np.concatenate([[0.0], np.cumsum(r * r)])
    window = np.sqrt(energy[len(probe):] - energy[:-len(probe)])[:len(r) - len(probe)] + 1e-9
    at = int((xc[:len(r) - len(probe)] / window).argmax())  # normalised: loudness aside
    # The correlation's peak is broad on this music: settle the exact frame
    # by the least difference after the best gain, within 16 frames.
    def resid(o):
        q = r[o:o + len(probe)]
        g = float(np.dot(probe, q)) / (float(np.dot(q, q)) + 1e-9)
        return float(((probe - g * q) ** 2).sum())
    at = min(range(max(0, at - 16), min(len(r) - len(probe), at + 17)), key=resid)
    seg = r[at:at + len(probe)]
    best = float(np.dot(seg, probe)) / (np.linalg.norm(seg) * np.linalg.norm(probe) + 1e-9)
    start_ms = at * 1000.0 / RATE
    snr, gain, slips, bad, at_end = follow(cap[:, 0].astype(np.float64), r, at)
    ok("%s: starts at %.1f ms of the sound (the splash said %d ms; match %.4f), %.0f ms long, "
       "%.1f dB above the difference (gain %.3f), %d slip(s), %d window(s) of 100 ms set aside"
       % (path, start_ms, join_ms, best, len(cap) * 1000.0 / RATE, snr, gain, len(slips), bad))
    for pos, by in slips:
        ok("%s: at %.0f ms of the capture it jumps %+d frames (a period QEMU lost?)"
           % (path, pos * 1000.0 / RATE, by))
    if best < 0.99 or start_ms < join_ms - 1 or start_ms > join_ms + 400:
        fail("the capture isn't the video's sound from where the splash joined it")
    played_to = at_end * 1000.0 / RATE
    # the end of the sound: its last sample above the capture's trim level
    end_ms = (np.nonzero(np.abs(ref).max(axis=1) > 64)[0][-1] + 1) * 1000.0 / RATE
    if skipped:
        if played_to > end_ms - 500:
            fail("skipped, but it played on to %.0f ms of %.0f" % (played_to, end_ms))
        ok("%s: stopped at %.0f ms of %.0f" % (path, played_to, end_ms))
    else:
        if snr < 30 or len(slips) > 1 or bad > 2:
            fail("the sound isn't the video's (%.1f dB, %d slips, %d windows set aside)"
                 % (snr, len(slips), bad))
        if played_to < end_ms - 30:
            fail("it stopped at %.0f ms of %.0f" % (played_to, end_ms))


def main():
    what, args = sys.argv[1], sys.argv[2:]
    if what == "quiet":
        check_quiet(args[0])
    elif what == "frames":
        check_frames(args[0], args[1])
    elif what == "frame":
        check_frame(args[0], int(args[1]))
    elif what == "text":
        check_text(args[0])
    elif what == "red":
        check_red(args[0])
    elif what == "alpha":
        check_alpha(args[0])
    elif what in ("sound", "skipped"):
        check_sound(args[0], int(args[1]), what == "skipped")
    else:
        sys.exit(__doc__)


main()
