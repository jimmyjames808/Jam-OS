#!/usr/bin/env python3
"""The checks of tools/wl-port-test.sh: a group's wlprobe cases from the
serial log, and the screenshots the group took, at known spots.

    wl-port-check.py <group> <outdir> <serial log>

Prints one line per check, "<name>: PASS|FAIL|XFAIL|XPASS <detail>", and
exits 1 if any is FAIL or XPASS. A case wlprobe reports FAIL that the
compositor is known to fail is an XFAIL (XFAIL below, with the reason, and
the evidence in the track's report); one on that list that passes is an
XPASS, which fails the group so the entry is taken off.

The screenshots (PNG, 1280x800 at OVMF's size) were taken by the group's
script at points wlprobe's "shot" lines announce; the pixel checks find the
window by colours the client drew (never by where the layout puts it), then
check pixels at offsets from it."""
import glob
import os
import re
import sys

sys.dont_write_bytecode = True

# The cases each group must report, by name.
CASES = {
    "proto": ["globals", "bind", "shm", "output", "keymap", "toplevel", "keyless", "deco-server",
              "frames", "geometry", "geometry-error", "limits-error", "serial-error", "parent",
              "popup", "popup-nested", "popup-reposition", "popup-flip", "popup-tooltip",
              "popup-parent", "big-buffer", "resize-storm", "reuse", "clipboard-serial", "maximize",
              "fullscreen", "minimize", "title", "shm-argb", "damage", "csd-geometry",
              "states-tiled", "subcompositor"],
    "floating": ["states-floating"],
    "paint": ["title", "shm-argb", "damage", "csd-geometry"],
    "pointer": ["pointer", "keyless-click", "popup-click"],
    "keyboard": ["keyboard", "close"],
    "cursor": ["cursor-shape", "cursor-surface"],
    "clip": ["clipboard-in", "clipboard-out"],
    "hold": ["hold"],
    "hostile": ["hostile-sub-storm", "hostile-desync-flood", "hostile-popup-cap", "hostile-deco",
                "hostile-handles", "hostile-never-reads", "hostile-regrab", "toplevel"],
}

# Cases the compositor fails today, and why (the track's report has the evidence).
XFAIL = {
}

ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")
# A result line, wherever on its line it starts: the kernel's log lines and a terminal's
# echo share the serial port and land in front of it, or behind it.
RESULT = re.compile(r"(PASS|FAIL) ([a-z0-9-]+): (.*)$")
KLOG = re.compile(r"\[(?:\s*\d+\.\d+|[a-z][a-z0-9-]*)\] .*$")

results = []   # (name, status, detail)


def report(name, status, detail=""):
    results.append((name, status, detail))
    print("%s: %s%s" % (name, status, " " + detail if detail else ""))


def read_log(path):
    try:
        text = open(path, "rb").read().decode("utf-8", "replace")
    except OSError as e:
        report("log", "FAIL", "%s: %s" % (path, e))
        return []
    return [ANSI.sub("", l).rstrip("\r") for l in text.split("\n")]


def wlprobe_cases(lines):
    """name -> (PASS or FAIL, detail), the last report of each."""
    out = {}
    for line in lines:
        m = RESULT.search(line)
        if m:
            out[m.group(2)] = (m.group(1), KLOG.sub("", m.group(3)).strip())
    return out


def check_cases(group, lines):
    seen = wlprobe_cases(lines)
    for name in CASES.get(group, []):
        if name not in seen:
            report(name, "FAIL", "wlprobe printed no PASS or FAIL line for it")
            continue
        status, detail = seen[name]
        if name in XFAIL:
            if status == "FAIL":
                report(name, "XFAIL", "%s (%s)" % (detail, XFAIL[name]))
            else:
                report(name, "XPASS", "passes now: %s (take it off XFAIL in tools/wl-port-check.py)" % detail)
        else:
            report(name, status, detail)


# ---- screenshots -------------------------------------------------------------------------

# The compositor's colours (user/services/compositor/look.h) and the terminal's
# background (tools/clip-test.sh), the pixels the checks expect around windows.
BAR_FOCUSED, BAR = 0x30363e, 0x262b31
OUTLINE_FOCUSED, OUTLINE = 0x4b535d, 0x363c44
TILE_FOCUSED, TILE = 0x7f77dd, 0x363c44
TERM_BG = 0x101018
MAGENTA = 0xff00ff        # wlprobe's marker block at its surface's top left
CORNER = 12               # pixels at a window's corners the compositor rounds

# wl-window-test's bands (ports/wayland/override/wl-window-test.c, `still`)
BANDS = [0xc2185b, 0xf2a541, 0x4a2c5e, 0xf6efe6]


def numpy():
    import numpy as n
    return n


def load(outdir, name):
    from PIL import Image
    path = os.path.join(outdir, name + ".png")
    try:
        return numpy().array(Image.open(path).convert("RGB"), dtype=numpy().int32)
    except OSError:
        report(name, "FAIL", "no screenshot %s (the group's script stopped before it?)" % path)
        return None


def rgb(c):
    return ((c >> 16) & 255, (c >> 8) & 255, c & 255)


def px(img, x, y):
    if x < 0 or y < 0 or y >= img.shape[0] or x >= img.shape[1]:
        return None
    r, g, b = img[y, x]
    return int(r) << 16 | int(g) << 8 | int(b)


def hexc(c):
    return "none" if c is None else "#%06x" % c


def bbox_of(img, colour):
    """(x0, y0, x1, y1) of the pixels exactly this colour, or None."""
    r, g, b = rgb(colour)
    m = (img[:, :, 0] == r) & (img[:, :, 1] == g) & (img[:, :, 2] == b)
    ys, xs = numpy().nonzero(m)
    if len(xs) == 0:
        return None
    return int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())


def find_row(img, pattern):
    """Where the run of pixels `pattern` (an Nx3 array) occurs in a row of
    img: (x, y) of the first hit, or None."""
    want = pattern.astype("uint8").tobytes()
    for y in range(img.shape[0]):
        row = img[y].astype("uint8").tobytes()
        i = row.find(want)
        while i >= 0 and i % 3:
            i = row.find(want, i + 1)
        if i >= 0:
            return i // 3, y
    return None


def marker_origin(img, offset=(0, 0)):
    """Where wlprobe's surface starts: its magenta marker block (16x16 at the
    surface's top left, or elsewhere by `offset`: the block's place in the
    surface)."""
    bb = bbox_of(img, MAGENTA)
    if bb is None:
        return None
    return bb[0] - offset[0], bb[1] - offset[1]


def check_pixels(name, what, img, points, tol=0):
    """points: [(x, y, colour or a set of colours)]"""
    bad = []
    for x, y, want in points:
        got = px(img, x, y)
        wants = list(want) if isinstance(want, (set, tuple, list)) else [want]
        if got is None or not any(all(abs(a - b) <= tol for a, b in zip(rgb(got), rgb(w))) for w in wants):
            bad.append("(%d,%d) is %s, want %s" % (x, y, hexc(got), "/".join(hexc(w) for w in wants)))
    if bad:
        report(name, "FAIL", "%s: %s" % (what, "; ".join(bad[:4])))
    else:
        report(name, "PASS", "%s (%d pixels)" % (what, len(points)))
    return not bad


def bands_array(w, h):
    n = numpy()
    yy, xx = n.mgrid[0:h, 0:w]
    idx = ((xx + yy) // 48) % 4
    out = n.zeros((h, w, 3), dtype=n.int32)
    for i, c in enumerate(BANDS):
        out[idx == i] = rgb(c)
    return out


def configured_size(lines):
    for l in lines:
        m = re.match(r"^configured (\d+)x(\d+)", l.strip())
        if m:
            return int(m.group(1)), int(m.group(2))
    return None


def mask_corners(diff):
    diff[:CORNER, :CORNER] = diff[:CORNER, -CORNER:] = False
    diff[-CORNER:, :CORNER] = diff[-CORNER:, -CORNER:] = False
    return diff


def check_window_test(outdir, lines, shot, layout):
    """wl-window-test's window in the screenshot: its bands at every pixel but
    the rounded corners, and its decoration (the border of a tiled window, the
    title bar and outline of a floating one)."""
    n = numpy()
    img = load(outdir, shot)
    if img is None:
        return
    size = configured_size(lines)
    if size is None:
        report(shot, "FAIL", "wl-window-test printed no 'configured WxH'")
        return
    w, h = size
    # the surface: the bounding box of the pixels that are one of the bands' colours
    # (the bands repeat along the anti-diagonal, so no run of them says where it starts)
    mask = numpy().zeros(img.shape[:2], dtype=bool)
    for c in BANDS:
        mask |= (img == numpy().array(rgb(c))).all(axis=2)
    ys, xs = n.nonzero(mask)
    if len(xs) == 0:
        report(shot, "FAIL", "no pixel of wl-window-test's bands is on the screen")
        return
    x0, y0 = int(xs.min()), int(ys.min())
    bw, bh = int(xs.max()) - x0 + 1, int(ys.max()) - y0 + 1
    if (bw, bh) != (w, h):
        report(shot + " size", "FAIL", "the bands cover %dx%d at %d,%d; wl-window-test said it was %dx%d" %
               (bw, bh, x0, y0, w, h))
        return
    exp = bands_array(w, h)
    got = img[y0:y0 + h, x0:x0 + w]
    if got.shape[:2] != (h, w):
        report(shot, "FAIL", "window at %d,%d, %dx%d does not fit the screen" % (x0, y0, w, h))
        return
    diff = mask_corners((got != exp).any(axis=2))
    bad = int(diff.sum())
    if bad:
        ys, xs = n.nonzero(diff)
        report(shot + " bands", "FAIL", "window %dx%d at %d,%d: %d of %d pixels differ (first at %d,%d: %s, want %s)" %
               (w, h, x0, y0, bad, w * h, int(xs[0]), int(ys[0]), hexc(px(img, x0 + int(xs[0]), y0 + int(ys[0]))),
                hexc(int(exp[ys[0], xs[0], 0]) << 16 | int(exp[ys[0], xs[0], 1]) << 8 | int(exp[ys[0], xs[0], 2]))))
    else:
        report(shot + " bands", "PASS", "window %dx%d at %d,%d: every pixel but the corners is the bands'" %
               (w, h, x0, y0))
    mid_y, mid_x = y0 + h // 2, x0 + w // 2
    if layout == "tiled":
        check_pixels(shot + " border", "the tile's border round the surface (focused or not)", img,
                     [(x0 - 1, mid_y, {TILE_FOCUSED, TILE}), (x0 + w, mid_y, {TILE_FOCUSED, TILE}),
                      (mid_x, y0 - 1, {TILE_FOCUSED, TILE}), (mid_x, y0 + h, {TILE_FOCUSED, TILE})])
    else:
        check_pixels(shot + " frame", "title bar above, outline round the surface (focused or not)", img,
                     [(x0 + w - 30, y0 - 6, {BAR_FOCUSED, BAR}), (x0 + w - 30, y0 - 24, {BAR_FOCUSED, BAR}),
                      (x0 - 1, mid_y, {OUTLINE_FOCUSED, OUTLINE}), (x0 + w, mid_y, {OUTLINE_FOCUSED, OUTLINE}),
                      (mid_x, y0 + h, {OUTLINE_FOCUSED, OUTLINE})])


def check_cat(outdir, shot, layout):
    """hello-wayland's cat: the picture (build/ports/hello-wayland/work/*/cat.png)
    found by its pixels, matching inside the rounded corners; no title bar above
    it (it never bound the decoration manager)."""
    from PIL import Image
    img = load(outdir, shot)
    if img is None:
        return
    files = glob.glob("build/ports/hello-wayland/work/*/cat.png")
    if not files:
        report(shot, "FAIL", "build/ports/hello-wayland/work/*/cat.png is not there")
        return
    cat = numpy().array(Image.open(files[0]).convert("RGB"), dtype=numpy().int32)
    ch, cw = cat.shape[:2]
    row = ch // 2
    hit = find_row(img, cat[row, 16:cw - 16])
    if hit is None:
        report(shot, "FAIL", "the cat's middle row is nowhere on the screen")
        return
    x0, y0 = hit[0] - 16, hit[1] - row
    got = img[y0:y0 + ch, x0:x0 + cw]
    diff = mask_corners((got != cat).any(axis=2))
    bad = int(diff.sum())
    if bad:
        report(shot + " cat", "FAIL", "cat at %d,%d: %d of %d pixels differ" % (x0, y0, bad, cw * ch))
    else:
        report(shot + " cat", "PASS", "the %dx%d cat at %d,%d, every pixel but the corners" % (cw, ch, x0, y0))
    above = [px(img, x0 + cw // 2, y0 - d) for d in (4, 10, 20)]
    if layout == "floating":
        ok = all(c not in (BAR_FOCUSED, BAR) for c in above)
        report(shot + " bare", "PASS" if ok else "FAIL",
               "no title bar above the cat (%s)" % " ".join(hexc(c) for c in above))


def check_tiled(outdir, lines):
    if not any("wl-window-test: ok" in l for l in lines):
        report("wl-window-test", "FAIL", "it did not end with 'wl-window-test: ok'")
    check_window_test(outdir, lines, "wl-tiled", "tiled")
    check_cat(outdir, "cat-tiled", "tiled")


def check_floating(outdir, lines):
    if not any("wl-window-test: ok" in l for l in lines):
        report("wl-window-test", "FAIL", "it did not end with 'wl-window-test: ok'")
    check_window_test(outdir, lines, "wl-floating", "floating")
    check_cat(outdir, "cat-floating", "floating")


def check_title(outdir):
    """The title bar changes with set_title; the surface does not."""
    n = numpy()
    a, b = load(outdir, "title-a"), load(outdir, "title-b")
    if a is None or b is None:
        return
    o = marker_origin(a)
    if o is None:
        report("title bar", "FAIL", "no marker in title-a.png")
        return
    x0, y0 = o
    bar_a = a[y0 - 26:y0 - 2, x0 + 90:x0 + 380]
    bar_b = b[y0 - 26:y0 - 2, x0 + 90:x0 + 380]
    plain = (bar_a == n.array(rgb(BAR_FOCUSED))).all(axis=2) | (bar_a == n.array(rgb(BAR))).all(axis=2)
    ink_a = int((~plain).sum())
    changed = int((bar_a != bar_b).any(axis=2).sum())
    body_same = bool((a[y0:y0 + 200, x0:x0 + 300] == b[y0:y0 + 200, x0:x0 + 300]).all())
    ok = ink_a > 30 and changed > 100 and body_same
    report("title bar", "PASS" if ok else "FAIL",
           "title A drew %d pixels of text in the bar, title B changed %d; the surface %s" %
           (ink_a, changed, "did not change" if body_same else "changed"))


def check_argb(outdir):
    """Alpha: blocks of a wl_shm ARGB8888 buffer over the terminal's background
    (the script cleared the terminal, so the window is over its plain ground)."""
    img = load(outdir, "shm-argb")
    if img is None:
        return
    o = marker_origin(img)
    if o is None:
        report("shm-argb", "FAIL", "no marker in shm-argb.png")
        return
    x0, y0 = o
    bg = rgb(TERM_BG)

    def over(src, a):
        return sum(min(255, s + (b * (255 - a)) // 255) << sh for s, b, sh in zip(src, bg, (16, 8, 0)))

    check_pixels("shm-argb opaque", "an opaque block is its own colour", img,
                 [(x0 + 64, y0 + 64, 0x3366cc), (x0 + 40, y0 + 40, 0x3366cc)])
    check_pixels("shm-argb transparent", "a block of alpha 0 shows what is behind (the terminal)", img,
                 [(x0 + 224, y0 + 64, TERM_BG), (x0 + 200, y0 + 40, TERM_BG)], tol=1)
    check_pixels("shm-argb 50%", "alpha 128, premultiplied red, over the terminal", img,
                 [(x0 + 144, y0 + 64, over((0x80, 0, 0), 0x80))], tol=2)
    check_pixels("shm-argb 25%", "alpha 64, premultiplied red, over the terminal", img,
                 [(x0 + 64, y0 + 144, over((0x40, 0, 0), 0x40))], tol=2)
    check_pixels("shm-argb 75%", "alpha 192, premultiplied grey, over the terminal", img,
                 [(x0 + 144, y0 + 144, over((0x60, 0x60, 0x60), 0xc0))], tol=2)


def check_damage(outdir):
    d0, d1, d2 = (load(outdir, "damage-%d" % i) for i in range(3))
    if d0 is None or d1 is None or d2 is None:
        return
    o = marker_origin(d0)
    if o is None:
        report("damage", "FAIL", "no marker in damage-0.png")
        return
    x0, y0 = o
    g = 0x204060
    check_pixels("damage 0", "the whole buffer is the ground colour", d0,
                 [(x0 + 80, y0 + 70, g), (x0 + 225, y0 + 145, g), (x0 + 350, y0 + 230, g)])
    check_pixels("damage 1", "two damage_buffer rectangles show, the rest is as it was", d1,
                 [(x0 + 80, y0 + 70, 0xe0a030), (x0 + 225, y0 + 145, 0x30a0e0),
                  (x0 + 150, y0 + 20, g), (x0 + 350, y0 + 230, g), (x0 + 39, y0 + 70, g),
                  (x0 + 120, y0 + 70, g)])
    check_pixels("damage 2", "a wl_surface.damage rectangle shows too, the others stay", d2,
                 [(x0 + 350, y0 + 230, 0xa0e060), (x0 + 80, y0 + 70, 0xe0a030),
                  (x0 + 225, y0 + 145, 0x30a0e0), (x0 + 150, y0 + 20, g)])


def check_csd(outdir):
    """A client-side window whose geometry is smaller than its surface gets no
    corners of the compositor's (GC1b); the same window with the whole surface
    as geometry does."""
    ca, cb = load(outdir, "csd-a"), load(outdir, "csd-b")
    if ca is None or cb is None:
        return
    oa, ob = marker_origin(ca, (24, 24)), marker_origin(cb, (24, 24))
    if oa is None or ob is None:
        report("csd-geometry", "FAIL", "no marker in csd-a.png or csd-b.png")
        return
    w, h = 400, 300
    orange = 0xff8000
    spots = ((0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1))
    a_px = [px(ca, oa[0] + dx, oa[1] + dy) for dx, dy in spots]
    clipped = sum(1 for c in a_px if c != orange)
    report("csd-geometry a", "PASS" if clipped >= 3 else "FAIL",
           "no geometry: the compositor rounds the corners (%d of 4 corner pixels are not the client's: %s)" %
           (clipped, " ".join(hexc(c) for c in a_px)))
    check_pixels("csd-geometry b", "geometry smaller than the surface: the client's corner pixels show whole",
                 cb, [(ob[0] + dx, ob[1] + dy, orange) for dx, dy in spots])


def check_paint(outdir, lines):
    check_title(outdir)
    check_argb(outdir)
    check_damage(outdir)
    check_csd(outdir)


def check_pointer(outdir, lines):
    img = load(outdir, "pointer-win")
    if img is None:
        return
    o = marker_origin(img)
    if o is None:
        report("pointer place", "FAIL", "no marker in pointer-win.png")
        return
    clicks = {}
    for l in lines:
        m = re.search(r"wlprobe: pointer: click ([AB]) at (-?\d+),(-?\d+)", l)
        if m:
            clicks[m.group(1)] = (int(m.group(2)), int(m.group(3)))
    if "A" not in clicks:
        report("pointer place", "FAIL", "wlprobe printed no click A")
        return
    want = (960 - o[0], 420 - o[1])
    ok = abs(clicks["A"][0] - want[0]) <= 1 and abs(clicks["A"][1] - want[1]) <= 1
    report("pointer place", "PASS" if ok else "FAIL",
           "the surface starts at screen %d,%d; the click at screen 960,420 was reported at %d,%d, want %d,%d" %
           (o[0], o[1], clicks["A"][0], clicks["A"][1], want[0], want[1]))


def check_cursor(outdir, lines):
    check_cursor_surface(outdir)
    shots = [load(outdir, s) for s in ("cursor-default", "cursor-text", "cursor-hand")]
    if any(s is None for s in shots):
        return
    # the probe's window is under the pointer in each (its ground, 0x202830): the shots
    # were taken while it held still, not after it ended
    for name, shot in zip(("cursor-default", "cursor-text", "cursor-hand"), shots):
        check_pixels(name + " window", "the probe's window is on the screen", shot, [(1000, 500, 0x202830)])
    crops = [s[420 - 20:420 + 28, 960 - 20:960 + 28] for s in shots]
    names = ("default", "text", "hand")
    for i, j in ((0, 1), (0, 2), (1, 2)):
        d = int((crops[i] != crops[j]).any(axis=2).sum())
        report("cursor %s/%s" % (names[i], names[j]), "PASS" if d >= 20 else "FAIL",
               "%d pixels around the pointer differ" % d)


def check_cursor_surface(outdir):
    """The client's own cursor surface is drawn at the pointer with its hotspot at
    the pointer's place, and none draws nothing."""
    red, green, ground = 0xff0000, 0x00ff00, 0x202830
    a, b = load(outdir, "cursor-surface"), load(outdir, "cursor-hidden")
    if a is not None:
        check_pixels("cursor-surface drawn", "red cursor, green centre on the pointer (hotspot 12,12)", a,
                     [(960, 420, green), (951, 411, red), (969, 429, red), (951, 429, red)])
    if b is not None:
        check_pixels("cursor-surface hidden", "set_cursor(NULL) draws no cursor", b,
                     [(960, 420, ground), (951, 411, ground), (969, 429, ground)])


def check_clip(outdir, lines):
    for want in ("console: terminal 1: copied 12 bytes", "console: terminal 2: pasted 21 bytes"):
        ok = any(want in l for l in lines)
        report("clip log", "PASS" if ok else "FAIL", want if ok else "no '%s' in the log" % want)


def check_hold(outdir, lines):
    seen = wlprobe_cases(lines).get("hold")
    if seen and seen[0] == "PASS":
        m = re.match(r"(\d+) frames", seen[1])
        frames = int(m.group(1)) if m else 0
        report("hold frames", "PASS" if frames >= 300 else "FAIL", "%d frames in 20 s" % frames)
    img = load(outdir, "hold")
    if img is not None:
        got = {px(img, x, y) for x in range(700, 760, 4) for y in range(300, 360, 4)}
        report("hold bands", "PASS" if got & set(BANDS) else "FAIL", "the bands are on the screen")


JOB_LINE = re.compile(r"job \d+: \d+ pages, (\d+) handles, \d+ threads?, (\d+) message bytes")
COMP_LINE = re.compile(r"process \d+ compositor\b")
HANDLES_SLACK, BYTES_SLACK = 4, 16384   # the terminal's own pools and unread events may move


def compositor_jobs(lines):
    """(handles, message bytes) of the compositor's job in each `ps -k` listing,
    in order: the job line just above its process line."""
    out, last = [], None
    for line in lines:
        m = JOB_LINE.search(line)
        if m:
            last = (int(m.group(1)), int(m.group(2)))
        elif COMP_LINE.search(line) and last:
            out.append(last)
            last = None
    return out


def check_hostile(outdir, lines):
    """The compositor's job after the hostile clients is where it was before
    them: a client gone, however it went, leaves no handle or message of its
    own charged to the compositor (X1's leak, G1)."""
    jobs = compositor_jobs(lines)
    if len(jobs) < 2:
        report("hostile leak", "FAIL", "the compositor's job is in %d ps -k listing(s), wanted 2" % len(jobs))
        return
    (h0, b0), (h1, b1) = jobs[0], jobs[-1]
    ok = h1 <= h0 + HANDLES_SLACK and b1 <= b0 + BYTES_SLACK
    report("hostile leak", "PASS" if ok else "FAIL",
           "the compositor's handles %d -> %d, message bytes %d -> %d" % (h0, h1, b0, b1))


def main():
    group, outdir, logpath = sys.argv[1:4]
    lines = read_log(logpath)
    check_cases(group, lines)
    checker = globals().get("check_" + group.replace("-", "_"))
    if checker:
        checker(outdir, lines)
    bad = [r for r in results if r[1] in ("FAIL", "XPASS")]
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
