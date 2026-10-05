#!/usr/bin/env python3
"""Check the compositor's test scene on QEMU's screen (tools/comp-test.sh).

    comp-check.py <serial log> <shot prefix>

The log has the scene, one line per command the compositor ran
("compositor: testscene: <command>", user/services/compositor/testscene.c),
and a line for each step it held on the screen ("holding N ms after paint
S"); <prefix>-<S>.ppm is QEMU's screenshot of step S (saved again as .png
for a look). Each screenshot is compared with the scene as the commands
leave it at that step, painted here from scratch: the background, each
window's pixels (testscene.h's pattern, premultiplied alpha blended with
px_over's rounding), the title bars and borders in their colours, the
arrow. Pixels whose value this script doesn't model (the title's text, the
close box's X) are skipped, and the text is checked to be there instead.
Every third pixel each way is compared; exit 0 if all match."""
import re
import sys

from PIL import Image

BG = 0x1E1A1D                     # SPLASH_BG
TITLE_H, BORDER_W, TEXT_PAD = 24, 2, 8
BAR_F, BAR = 0x4A3D66, 0x2E2833   # paint.h's title colours
TEXT_F, TEXT = 0xF2EAF6, 0x9D93A3
BORDER_F, BORDER = 0x7A66A8, 0x3D3542
GARBAGE = None                    # a pixel this script doesn't model

ARROW = [   # libfun's pointer_arrow (fun.h): '#' black, 'o' white
    "#           ", "##          ", "#o#         ", "#oo#        ", "#ooo#       ",
    "#oooo#      ", "#ooooo#     ", "#oooooo#    ", "#ooooooo#   ", "#oooooooo#  ",
    "#ooooooooo# ", "#oooooo#####", "#ooo#oo#    ", "#oo# #oo#   ", "#o#  #oo#   ",
    "##    #oo#  ", "#     #oo#  ", "       ##   ",
]


def pattern(rgb, x, y, solid):
    return rgb if solid else rgb ^ (((x * 7 + y * 13) & 0x3F) * 0x010101)


def pm(rgb, a):
    out = a << 24
    for sh in (0, 8, 16):
        out |= ((rgb >> sh & 0xFF) * a + 127) // 255 << sh
    return out


def over(dst, src):
    inv, out = 255 - (src >> 24), 0
    for sh in (0, 8, 16):
        x = (dst >> sh & 0xFF) * inv + 128
        c = (src >> sh & 0xFF) + ((x + (x >> 8)) >> 8)
        out |= min(c, 255) << sh
    return out


class Win:
    def __init__(self, n, x, y, w, h, argb, flags):
        self.n, self.x, self.y, self.w, self.h = n, x, y, w, h
        self.argb, self.flags, self.mapped = argb, flags, True
        self.deco = (TITLE_H, BORDER_W) if "t" in flags else (0, 0)

    def frame(self):
        top, side = self.deco
        return self.x - side, self.y - top, self.x + self.w + side, self.y + self.h + side

    def pixel(self, px, py, below):
        """The window's pixel at (px, py) over `below`, or `below` if it isn't there."""
        x1, y1, x2, y2 = self.frame()
        if not (x1 <= px < x2 and y1 <= py < y2):
            return below
        if self.x <= px < self.x + self.w and self.y <= py < self.y + self.h:
            a, rgb = self.argb >> 24, pattern(self.argb & 0xFFFFFF, px - self.x, py - self.y,
                                             "s" in self.flags)
            if a == 0xFF:
                return rgb
            if "o" in self.flags:
                return pm(rgb, a) & 0xFFFFFF
            return None if below is None else over(below, pm(rgb, a))
        focused = "f" in self.flags
        if py < self.y:   # the title bar
            close = x2 - TITLE_H
            if px >= close or x1 + TEXT_PAD <= px < close - TEXT_PAD:
                return GARBAGE   # the text and the close box: checked apart
            return BAR_F if focused else BAR
        return BORDER_F if focused else BORDER


def parse(log):
    """The output size, and each held step's windows (stacking order) and cursor."""
    size, wins, cursor, steps, paints = None, [], None, {}, {}
    for line in open(log, errors="replace"):
        m = re.search(r"compositor: testscene: (.*)", line)
        if not m:
            continue
        c = m.group(1).strip()
        if c.startswith("output "):
            w, h = c.split()[1].rstrip(",").split("x")
            size = (int(w), int(h))
        elif c.startswith("win=") or c.startswith("fullscreen="):
            if c.startswith("fullscreen="):
                c = "win=0,0,%d,%d,%s," % (size[0], size[1], c.split("=")[1])
            f = c[4:].split(",") + [""]
            wins.append(Win(len(wins) + 1, int(f[0]), int(f[1]), int(f[2]), int(f[3]),
                            int(f[4], 16), f[5]))
        elif c.startswith("move="):
            k, x, y = map(int, c[5:].split(","))
            w = next(w for w in wins if w.n == k)
            w.x, w.y = x, y
        elif c.startswith("raise="):
            w = next(w for w in wins if w.n == int(c[6:]))
            wins.remove(w)
            wins.append(w)
        elif c.startswith("map=") or c.startswith("unmap="):
            next(w for w in wins if w.n == int(c.split("=")[1])).mapped = c[0] == "m"
        elif c.startswith("cursor="):
            cursor = tuple(map(int, c[7:].split(",")))
        elif c == "nocursor":
            cursor = None
        elif c.startswith("paint "):
            n = int(c.split()[1].rstrip(":"))
            d = re.search(r"\((\d+) direct\)", c)
            paints[n] = int(d.group(1)) if d else 0
        elif c.startswith("holding "):
            step = int(c.split()[-1])
            steps[step] = ([(w.n, w.x, w.y, w.mapped) for w in wins], cursor)
    return size, wins, steps, paints


def expected(px, py, order, byn, cursor, scale):
    v = BG
    for n, x, y, mapped in order:
        w = byn[n]
        if mapped:
            w.x, w.y = x, y
            v = w.pixel(px, py, v)
    if cursor:
        cx, cy = cursor
        i, j = (px - cx) // scale, (py - cy) // scale
        if 0 <= i < 12 and 0 <= j < 18 and px >= cx and py >= cy and ARROW[j][i] != " ":
            v = 0 if ARROW[j][i] == "#" else 0xFFFFFF
    return v


def check_step(step, prefix, size, wins, state):
    order, cursor = state
    img = Image.open("%s-%d.ppm" % (prefix, step)).convert("RGB")
    img.save("%s-%d.png" % (prefix, step))
    if img.size != size:
        print("comp-check: step %d: the screenshot is %dx%d, the output %dx%d" %
              (step, img.size[0], img.size[1], size[0], size[1]))
        return False
    pix, byn = img.load(), {w.n: w for w in wins}
    scale = 2 if size[1] > 1100 else 1
    bad = checked = 0
    for py in range(0, size[1], 3):
        for px in range(0, size[0], 3):
            want = expected(px, py, order, byn, cursor, scale)
            if want is None:
                continue
            r, g, b = pix[px, py]
            got = r << 16 | g << 8 | b
            checked += 1
            if got != want:
                if bad < 5:
                    print("comp-check: step %d: pixel (%d, %d) is %06x, want %06x" %
                          (step, px, py, got, want))
                bad += 1
    # The titles' text is there, in the bars the steps show uncovered.
    for n, x, y, mapped in order:
        w = byn[n]
        if "t" not in w.flags or not mapped or step == 3:
            continue
        ink = TEXT_F if "f" in w.flags else TEXT
        tx = x - BORDER_W + TEXT_PAD
        found = sum(1 for yy in range(y - TITLE_H, y) for xx in range(tx, tx + 60)
                    if 0 <= xx < size[0] and 0 <= yy < size[1] and
                    expected(xx, yy, order, byn, None, scale) is None and
                    pix[xx, yy] == (ink >> 16, ink >> 8 & 0xFF, ink & 0xFF))
        if n == order[-1][0] and found < 20:
            print("comp-check: step %d: window %d's title has %d pixels of text" % (step, n, found))
            bad += 1
    print("comp-check: step %d: %d pixels compared, %d wrong" % (step, checked, bad))
    return bad == 0 and checked > 1000


def main():
    log, prefix = sys.argv[1], sys.argv[2]
    size, wins, steps, paints = parse(log)
    if not size or sorted(steps) != [1, 2, 3]:
        print("comp-check: the log has no output size or not the three steps: %s" % sorted(steps))
        return 1
    ok = all([check_step(s, prefix, size, wins, steps[s]) for s in (1, 2, 3)])
    if paints.get(3, 0) == 0:
        print("comp-check: step 3 (a full-screen window) copied no tile straight from its buffer")
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
