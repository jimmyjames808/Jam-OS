#!/usr/bin/env python3
"""Check tools/splash-test.sh's screenshots and sound captures against the
boot splash's own file (boot/splash.mpg, decoded here with ffmpeg).

    splash-check.py quiet <png>             the whole screen is #1E1A1D
    splash-check.py frames <png> <png>      two screenshots of the animation,
                                            a known time apart: each matches
                                            a frame of the video (at the
                                            integer scale that fits, centred,
                                            the rest #1E1A1D), in order
    splash-check.py frame <png> <scale>     one screenshot: a frame of the
                                            video at that integer scale
    splash-check.py text <png>              the console's text screen: its
                                            background with text on it
    splash-check.py red <png>               a panic screen (dark red)
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


def video_part(img):
    """The 1280x720 picture at its integer scale, centred; and the rest."""
    h, w = img.shape[:2]
    k = max(1, min(w // 1280, h // 720))
    x, y = (w - 1280 * k) // 2, (h - 720 * k) // 2
    pic = img[y:y + 720 * k, x:x + 1280 * k]
    small = np.asarray(Image.fromarray(pic.astype(np.uint8)).resize(SMALL, Image.BOX))
    mask = np.ones((h, w), bool)
    mask[y:y + 720 * k, x:x + 1280 * k] = False
    return small.astype(np.int16), img[mask], k


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
    ok("%s: frame %d (mean error %.2f), %s: frame %d (%.2f), at %dx" % (a, ia, ea, b, ib, eb, k))
    if ea > 4 or eb > 4:
        fail("a screenshot doesn't look like any frame of the video")
    if ba > 4 or bb > 4:
        fail("the screen around the video isn't #1E1A1D")
    if not 15 <= ia <= 75 or not 40 <= ib - ia <= 90:
        fail("frames %d and %d: want the first 0.5-2.5 s in and the second 1.3-3 s later"
             % (ia, ib))


def check_frame(path, scale):
    i, err, border, k = match(ref_frames(), shot(path))
    ok("%s: frame %d (mean error %.2f) at %dx" % (path, i, err, k))
    if err > 4 or border > 4 or k != scale:
        fail("%s: not a frame of the video at %dx" % (path, scale))


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
    seg = r[at:at + len(probe)]
    best = float(np.dot(seg, probe)) / (np.linalg.norm(seg) * np.linalg.norm(probe) + 1e-9)
    start_ms = at * 1000.0 / RATE
    length = min(len(cap), len(ref) - at)
    x, y = cap[:length].astype(np.float64), ref[at:at + length].astype(np.float64)
    gain = float((x * y).sum() / ((y * y).sum() + 1e-9))
    snr = 10 * np.log10((y * y).sum() / (((x - gain * y) ** 2).sum() + 1e-9))
    ok("%s: starts at %.1f ms of the sound (the splash said %d ms; match %.4f), %.0f ms long, "
       "%.1f dB above the difference (gain %.3f)" % (path, start_ms, join_ms, best,
                                                     len(cap) * 1000.0 / RATE, snr, gain))
    if best < 0.99 or start_ms < join_ms - 1 or start_ms > join_ms + 400:
        fail("the capture isn't the video's sound from where the splash joined it")
    played_to = (at + len(cap)) * 1000.0 / RATE
    end_ms = len(ref) * 1000.0 / RATE
    if skipped:
        if played_to > end_ms - 500:
            fail("skipped, but it played on to %.0f ms of %.0f" % (played_to, end_ms))
        ok("%s: stopped at %.0f ms of %.0f" % (path, played_to, end_ms))
    else:
        if snr < 30:
            fail("the sound isn't the video's (%.1f dB)" % snr)
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
    elif what in ("sound", "skipped"):
        check_sound(args[0], int(args[1]), what == "skipped")
    else:
        sys.exit(__doc__)


main()
