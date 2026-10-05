#!/usr/bin/env python3
"""Check the compositor's test scene on QEMU's screen (tools/comp-test.sh).

    comp-check.py <serial log> <shot prefix>

The log has the scene, one line per command the compositor ran
("compositor: testscene: <command>", user/services/compositor/testscene.c),
and a line for each step it held on the screen ("holding N ms after paint
S"); <prefix>-<S>.ppm is QEMU's screenshot of step S (saved again as .png
for a look). Each screenshot is compared with the scene as the commands
leave it at that step, painted here from scratch with the look's numbers
(user/services/compositor/look.h): the wallpaper's formula, each window's
pixels (testscene.h's pattern, premultiplied alpha blended with px_over's
rounding), the shadows (a blur of three boxes, as shape.c makes it), the
title bars, outlines and borders, the rounded corners (mask.c's
supersampled circles), the arrow. Pixels whose value this script doesn't
model (the title's text, the circles' edges) are skipped; the text is
checked to be there instead, and each circle's colour (or its symbol,
under the pointer) where it is all circle. Every third pixel each way is
compared; exit 0 if all match."""
import re
import sys

from PIL import Image

# look.h
TITLE_H, OUTLINE, BORDER = 28, 1, 2
RADIUS, TILE_RADIUS = 10, 6
BAR_F, BAR = 0x30363E, 0x262B31
OUTLINE_F, OUTLINE_C = 0x4B535D, 0x363C44
TEXT_F, TEXT = 0xE6E9EC, 0x7F8892
TILE_F, TILE = 0x7F77DD, 0x363C44
BTN_D, BTN_LEFT, BTN_GAP, BTN_TOP, BTN_HIT = 12, 9, 6, 8, 3
BTN_COLOURS = (0xD4537E, 0xEF9F27, 0x7F77DD)
BTN_INKS = (0x5A1F35, 0x6B420E, 0x2D2958)
BTN_IDLE = 0x4A5058
BTNS_W, TEXT_PAD = BTN_LEFT + 3 * BTN_D + 2 * BTN_GAP, 8
SHADOWS = {False: (11, 13, 4, 77), True: (25, 30, 10, 128)}   # box, reach, dy, alpha
WALL_BASE = 0x161B26
GLOWS = ((0x4A2450, 150, 180, 620), (0x5A2C18, 880, 900, 520), (0x24203F, 520, 540, 480))
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


def div255(x):
    x += 128
    return (x + (x >> 8)) >> 8


def over(dst, src):
    inv, out = 255 - (src >> 24), 0
    for sh in (0, 8, 16):
        c = (src >> sh & 0xFF) + div255((dst >> sh & 0xFF) * inv)
        out |= min(c, 255) << sh
    return out


def darken(p, a):
    return sum(div255((p >> sh & 0xFF) * (255 - a)) << sh for sh in (0, 8, 16))


def corner_mix(below, edge, p, cov, ring):
    return sum(div255((below >> sh & 0xFF) * (255 - cov) + (edge >> sh & 0xFF) * ring +
                      (p >> sh & 0xFF) * (cov - ring)) << sh for sh in (0, 8, 16))


# ---- the wallpaper (wallpaper.c) ---------------------------------------------------------

def bayer(x, y):
    v = 0
    for s in range(3):
        v = v * 4 + ((0, 2), (3, 1))[(y >> s) & 1][(x >> s) & 1]
    return v


class Wallpaper:
    def __init__(self, w, h):
        side = max(w, h)
        self.glows = []
        for rgb, cx, cy, r in GLOWS:
            r = side * r // 1000
            self.glows.append((rgb, w * cx // 1000, h * cy // 1000, r * r, (1 << 48) // (r * r)))

    def at(self, x, y):
        acc = [(WALL_BASE >> (16 - 8 * c) & 0xFF) * 256 for c in range(3)]
        for rgb, cx, cy, r2, inv in self.glows:
            d2 = (x - cx) ** 2 + (y - cy) ** 2
            if d2 >= r2:
                continue
            q = 65536 - ((d2 * inv) >> 32)
            f = (q * q) >> 16
            for c in range(3):
                acc[c] += (f * ((rgb >> (16 - 8 * c) & 0xFF) - (WALL_BASE >> (16 - 8 * c) & 0xFF))) >> 8
        th = bayer(x & 7, y & 7) * 4 + 2
        return sum(min((min(max(acc[c], 0), 255 * 256) + th) >> 8, 255) << (16 - 8 * c)
                   for c in range(3))


# ---- shapes (mask.c, shape.c) ---------------------------------------------------------------

def coverage(i, j, r, rr):
    n = sum(1 for t in range(16) for s in range(16)
            if (32 * (r - i) - (2 * s + 1)) ** 2 + (32 * (r - j) - (2 * t + 1)) ** 2 <= 1024 * rr * rr)
    return (n * 255 + 128) // 256


def corner_table(r, ring):
    return {(i, j): (coverage(i, j, r, r), coverage(i, j, r, r) - coverage(i, j, r, r - ring))
            for i in range(r) for j in range(r)}


CORNERS = {"t": (RADIUS, corner_table(RADIUS, OUTLINE)), "g": (TILE_RADIUS, corner_table(TILE_RADIUS, BORDER))}


def edge_table(box, reach):
    k = [1] * box
    for _ in range(2):
        k = [sum(k[i - j] for j in range(box) if 0 <= i - j < len(k)) for i in range(len(k) + box - 1)]
    total, half, out, s = box ** 3, (3 * box - 2) // 2, {}, 0
    for i in range(half + reach):
        s += k[i]
        if i - half >= -reach:
            out[i - half] = (s * 65536 + total // 2) // total
    return out


EDGES = {f: edge_table(SHADOWS[f][0], SHADOWS[f][1]) for f in (False, True)}


def edge(focused, d):
    reach = SHADOWS[focused][1]
    return 0 if d < -reach else 65536 if d >= reach else EDGES[focused][d]


class Win:
    def __init__(self, n, x, y, w, h, argb, flags):
        self.n, self.x, self.y, self.w, self.h = n, x, y, w, h
        self.argb, self.flags, self.mapped = argb, flags, True
        self.look = "t" if "t" in flags else "g" if "g" in flags else "m" if "m" in flags else ""
        self.focused = "f" in flags

    def frame(self):
        top = {"t": TITLE_H, "m": TITLE_H, "g": BORDER}.get(self.look, 0)
        side = {"t": OUTLINE, "g": BORDER}.get(self.look, 0)
        return self.x - side, self.y - top, self.x + self.w + side, self.y + self.h + side

    def radius(self):
        if self.look not in CORNERS:
            return 0, None
        r, table = CORNERS[self.look]
        x1, y1, x2, y2 = self.frame()
        return (0, None) if x2 - x1 < 2 * r or y2 - y1 < 2 * r else (r, table)

    def corner(self, px, py):
        """(i, j) in the top-left corner's terms if (px, py) is in a corner square."""
        r, _ = self.radius()
        x1, y1, x2, y2 = self.frame()
        if not r or not (px < x1 + r or px >= x2 - r) or not (py < y1 + r or py >= y2 - r):
            return None
        return (px - x1 if px < x1 + r else x2 - 1 - px, py - y1 if py < y1 + r else y2 - 1 - py)

    def edge_colour(self):
        if self.look == "g":
            return TILE_F if self.focused else TILE
        return OUTLINE_F if self.focused else OUTLINE_C

    def button(self, b):
        x1, y1, x2, _ = self.frame()
        bx = x1 + BTN_LEFT + b * (BTN_D + BTN_GAP)
        if self.look not in ("t", "m") or bx + BTN_D > x2 - BTN_LEFT:
            return None
        return bx, y1 + BTN_TOP

    def shadowed(self, px, py, below):
        if self.look != "t" or below is None:
            return below
        x1, y1, x2, y2 = self.frame()
        if x1 <= px < x2 and y1 <= py < y2 and self.corner(px, py) is None:
            return below
        f, dy, alpha = self.focused, SHADOWS[self.focused][2], SHADOWS[self.focused][3]
        fy = edge(f, py - y1 - dy) - edge(f, py - y2 - dy)
        fx = edge(f, px - x1) - edge(f, px - x2)
        return darken(below, (alpha * fy * fx + (1 << 31)) >> 32)

    def body(self, px, py, below):
        """The window's own pixel at (px, py) in its frame, over `below`."""
        x1, y1, x2, y2 = self.frame()
        if self.x <= px < self.x + self.w and self.y <= py < self.y + self.h:
            a, rgb = self.argb >> 24, pattern(self.argb & 0xFFFFFF, px - self.x, py - self.y,
                                             "s" in self.flags)
            if a == 0xFF:
                return rgb
            if "o" in self.flags:
                return pm(rgb, a) & 0xFFFFFF
            return None if below is None else over(below, pm(rgb, a))
        if self.look == "g":
            return self.edge_colour()
        if self.look == "t" and (px in (x1, x2 - 1) or py == y1 or py == y2 - 1):
            return self.edge_colour()
        bx, by = x1 + BTN_LEFT, y1 + BTN_TOP
        if bx <= px < x1 + BTNS_W and by <= py < by + BTN_D:
            return GARBAGE   # the circles: checked apart
        if x1 + BTNS_W <= px < x2 - BTNS_W and y1 + 6 <= py < y1 + 22:
            return GARBAGE   # the title's text: checked apart
        return BAR_F if self.focused else BAR

    def pixel(self, px, py, below):
        """The window's pixel at (px, py) over `below`, its shadow included."""
        below = self.shadowed(px, py, below)
        x1, y1, x2, y2 = self.frame()
        if not (x1 <= px < x2 and y1 <= py < y2):
            return below
        p = self.body(px, py, below)
        c = self.corner(px, py)
        if c is None:
            return p
        cov, ring = self.radius()[1][c]
        if cov == 255 and ring == 0:
            return p
        if p is None or below is None:
            return GARBAGE
        return corner_mix(below, self.edge_colour(), p, cov, ring)


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


def place(order, byn):
    """The windows shown at a step, bottom to top, where the step has them."""
    shown = []
    for n, x, y, mapped in order:
        w = byn[n]
        if mapped:
            w.x, w.y = x, y
            shown.append(w)
    return shown


def hovered(shown, cursor):
    """The window whose circles the cursor is over (paint.c's hover), or None."""
    if not cursor:
        return None
    cx, cy = cursor
    for w in reversed(shown):
        x1, y1, x2, y2 = w.frame()
        if not (x1 <= cx < x2 and y1 <= cy < y2):
            continue
        if w.x <= cx < w.x + w.w and w.y <= cy < w.y + w.h:
            return None   # on its surface
        for b in range(3):
            at = w.button(b)
            if at and at[0] - BTN_HIT <= cx < at[0] + BTN_D + BTN_HIT and \
                    at[1] - BTN_HIT <= cy < at[1] + BTN_D + BTN_HIT:
                return w
        return None
    return None


def expected(px, py, shown, cursor, scale, wall):
    v = wall.at(px, py)
    for w in shown:
        v = w.pixel(px, py, v)
    if cursor:
        cx, cy = cursor
        i, j = (px - cx) // scale, (py - cy) // scale
        if 0 <= i < 12 and 0 <= j < 18 and px >= cx and py >= cy and ARROW[j][i] != " ":
            v = 0 if ARROW[j][i] == "#" else 0xFFFFFF
    return v


def rgb(pix, x, y):
    r, g, b = pix[x, y]
    return r << 16 | g << 8 | b


def topmost_at(shown, x, y):
    for w in reversed(shown):
        x1, y1, x2, y2 = w.frame()
        if x1 <= x < x2 and y1 <= y < y2:
            return w
    return None


def check_buttons(step, pix, shown, cursor, size):
    """Each circle's colour where it is all circle, and its symbol under the pointer."""
    bad, hov = 0, hovered(shown, cursor)
    for w in shown:
        lit = w.focused or w is hov
        for b in range(3):
            at = w.button(b)
            if not at:
                continue
            spots = [((6, 2), (2, 6), (9, 2))[b]]   # all circle, never symbol
            want = [BTN_COLOURS[b] if lit else BTN_IDLE]
            if w is hov:   # the x's crossing, the bar, an arrowhead: all symbol
                spots.append(((5, 5), (6, 5), (3, 3))[b])
                inks = (BTN_INKS[0], None, BTN_INKS[2])
                want.append(inks[b])
            for (i, j), colour in zip(spots, want):
                x, y = at[0] + i, at[1] + j
                if colour is None or not (0 <= x < size[0] and 0 <= y < size[1]):
                    continue
                if topmost_at(shown, x, y) is not w:
                    continue
                if cursor and cursor[0] <= x < cursor[0] + 24 and cursor[1] <= y < cursor[1] + 36:
                    continue   # under the arrow
                for above in shown[shown.index(w) + 1:]:
                    colour = above.shadowed(x, y, colour)   # a shadow from above falls on it
                if rgb(pix, x, y) != colour:
                    print("comp-check: step %d: window %d's circle %d at (%d, %d) is %06x, want %06x"
                          % (step, w.n, b, x, y, rgb(pix, x, y), colour))
                    bad += 1
    return bad


def check_titles(step, pix, shown, size):
    """The titles' text is there, in the bars the step shows uncovered."""
    bad = 0
    for w in shown:
        if w.look not in ("t", "m") or w is not shown[-1] and step != 1:
            continue
        ink = TEXT_F if w.focused else TEXT
        x1, y1, x2, _ = w.frame()
        found = sum(1 for yy in range(y1, y1 + TITLE_H) for xx in range(x1 + BTNS_W, x2 - BTNS_W)
                    if 0 <= xx < size[0] and 0 <= yy < size[1] and
                    topmost_at(shown, xx, yy) is w and rgb(pix, xx, yy) == ink)
        if found < 20:
            print("comp-check: step %d: window %d's title has %d pixels of text" % (step, w.n, found))
            bad += 1
    return bad


def check_step(step, prefix, size, wins, state, wall):
    order, cursor = state
    img = Image.open("%s-%d.ppm" % (prefix, step)).convert("RGB")
    img.save("%s-%d.png" % (prefix, step))
    if img.size != size:
        print("comp-check: step %d: the screenshot is %dx%d, the output %dx%d" %
              (step, img.size[0], img.size[1], size[0], size[1]))
        return False
    pix, byn = img.load(), {w.n: w for w in wins}
    shown = place(order, byn)
    scale = 2 if size[1] > 1100 else 1
    bad = checked = 0
    for py in range(0, size[1], 3):
        for px in range(0, size[0], 3):
            want = expected(px, py, shown, cursor, scale, wall)
            if want is None:
                continue
            got = rgb(pix, px, py)
            checked += 1
            if got != want:
                if bad < 5:
                    print("comp-check: step %d: pixel (%d, %d) is %06x, want %06x" %
                          (step, px, py, got, want))
                bad += 1
    bad += check_buttons(step, pix, shown, cursor, size)
    if step != 3:
        bad += check_titles(step, pix, shown, size)
    print("comp-check: step %d: %d pixels compared, %d wrong" % (step, checked, bad))
    return bad == 0 and checked > 1000


def main():
    log, prefix = sys.argv[1], sys.argv[2]
    size, wins, steps, paints = parse(log)
    if not size or sorted(steps) != [1, 2, 3]:
        print("comp-check: the log has no output size or not the three steps: %s" % sorted(steps))
        return 1
    wall = Wallpaper(*size)
    ok = all([check_step(s, prefix, size, wins, steps[s], wall) for s in (1, 2, 3)])
    if paints.get(3, 0) == 0:
        print("comp-check: step 3 (a full-screen window) copied no tile straight from its buffer")
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
