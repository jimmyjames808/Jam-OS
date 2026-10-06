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
supersampled circles), the arrow (where it is all fill: cursors.c's set,
docs/design/cursors.svg). Pixels whose value this script doesn't
model (the title's text, the circles' edges, the arrow's outline and
shadow) are skipped; the text is
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

# The arrow (cursors.c, docs/design/cursors.svg): its outline in units (a
# pixel each), its picture CURSOR_IMG square with CURSOR_PAD round, its hot
# spot (5, 2.5) rounded down; filled #14161c, outlined 0.55 either side of
# each edge, sampled 5 x 5 a pixel.
ARROW_PTS = [(5, 2.5), (5, 18.7), (9.1, 15), (11.8, 21.2), (14.5, 20), (11.8, 13.9), (17.4, 13.9)]
CURSOR_IMG, CURSOR_PAD, CURSOR_FILL = 28, 2, 0x14161C
ARROW_HOT = (5 + CURSOR_PAD, 2 + CURSOR_PAD)


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
        if x1 + BTNS_W <= px < x2 - BTNS_W and y1 + 2 <= py < y1 + TITLE_H - 1:
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


def seg_d2(x, y, a, b):
    vx, vy, qx, qy = b[0] - a[0], b[1] - a[1], x - a[0], y - a[1]
    k = max(0.0, min(1.0, (qx * vx + qy * vy) / (vx * vx + vy * vy)))
    return (qx - k * vx) ** 2 + (qy - k * vy) ** 2


def in_arrow(x, y):
    inside, n = False, len(ARROW_PTS)
    for i in range(n):
        (xi, yi), (xj, yj) = ARROW_PTS[i], ARROW_PTS[i - 1]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            inside = not inside
    return inside


def arrow_fill(i, j):
    """Is the arrow's picture's pixel (i, j) all fill: every sample inside,
    none within the outline's 0.55 (or near enough to it to round either way)?"""
    for t in range(5):
        for s in range(5):
            x, y = i - CURSOR_PAD + (s + 0.5) / 5, j - CURSOR_PAD + (t + 0.5) / 5
            if not in_arrow(x, y):
                return False
            if min(seg_d2(x, y, ARROW_PTS[k], ARROW_PTS[k - 1]) for k in range(7)) <= 0.6 ** 2:
                return False
    return True


ARROW_FILL = {(i, j) for j in range(CURSOR_IMG) for i in range(CURSOR_IMG) if arrow_fill(i, j)}


def expected(px, py, shown, cursor, scale, wall):
    if cursor:
        i, j = px - cursor[0] + ARROW_HOT[0], py - cursor[1] + ARROW_HOT[1]
        if 0 <= i < CURSOR_IMG and 0 <= j < CURSOR_IMG:
            return CURSOR_FILL if (i, j) in ARROW_FILL else GARBAGE
    v = wall.at(px, py)
    for w in shown:
        v = w.pixel(px, py, v)
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
                if cursor and 0 <= x - cursor[0] + ARROW_HOT[0] < CURSOR_IMG and \
                        0 <= y - cursor[1] + ARROW_HOT[1] < CURSOR_IMG:
                    continue   # under the arrow
                for above in shown[shown.index(w) + 1:]:
                    colour = above.shadowed(x, y, colour)   # a shadow from above falls on it
                if rgb(pix, x, y) != colour:
                    print("comp-check: step %d: window %d's circle %d at (%d, %d) is %06x, want %06x"
                          % (step, w.n, b, x, y, rgb(pix, x, y), colour))
                    bad += 1
    return bad


def check_titles(step, pix, shown, size):
    """The top window's title text is there: pixels nearer the text's colour
    than the bar's (it is anti-aliased), in its bar."""
    bad = 0
    for w in shown[-1:]:
        if w.look not in ("t", "m"):
            continue
        ink, bar = (TEXT_F, BAR_F) if w.focused else (TEXT, BAR)
        mid = ((ink >> 8 & 0xFF) + (bar >> 8 & 0xFF)) // 2
        x1, y1, x2, _ = w.frame()
        found = sum(1 for yy in range(y1, y1 + TITLE_H) for xx in range(x1 + BTNS_W, x2 - BTNS_W)
                    if 0 <= xx < size[0] and 0 <= yy < size[1] and
                    (rgb(pix, xx, yy) >> 8 & 0xFF) > mid)
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


# ---- the desktop (steps 4 and 5: desk.h, frost.c, stripdraw.c, testdesk.c) ----------------

STRIP_H, STRIP_BLUR, STRIP_SAT = 40, 33, 333
STRIP_TINT, STRIP_TINT_A, STRIP_LINE_A, ISLAND_A, ISLAND_R = 0x181B22, 107, 20, 20, 10
RASPBERRY, APRICOT, BLACKCURRANT, CHIP_FOCUS_A = 0xD4537E, 0xEF9F27, 0x7F77DD, 97
SHADOW_SIDE, SHADOW_ABOVE, SHADOW_BELOW = 30, 20, 40


def mix(a, b, c):
    """paint_mix: a over b by coverage c (0: a, 255: b)."""
    out = 0
    for sh in (0, 8, 16):
        x = (a >> sh & 0xFF) * (255 - c) + (b >> sh & 0xFF) * c + 128
        out |= ((x + (x >> 8)) >> 8) << sh
    return out


def box_pass(src, k):
    """One box blur of width k over a line, its ends repeated (frost.c's box_pass)."""
    n, h, out = len(src), k // 2, []
    sums = [0, 0, 0]
    for j in range(-h - 1, h):
        p = src[min(max(j, 0), n - 1)]
        for c in range(3):
            sums[c] += p >> (8 * c) & 0xFF
    for i in range(n):
        pin, pout = src[min(i + h, n - 1)], src[max(i - h - 1, 0)]
        px = 0
        for c in range(3):
            sums[c] += (pin >> (8 * c) & 0xFF) - (pout >> (8 * c) & 0xFF)
            px |= ((sums[c] + h) // k) << (8 * c)
        out.append(px)
    return out


def blur3(line, k):
    return box_pass(box_pass(box_pass(line, k), k), k)


def tdiv(a, b):
    """C's division: towards zero."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def strip_px(p):
    r, g, b = p >> 16 & 0xFF, p >> 8 & 0xFF, p & 0xFF
    lum, out = (r * 77 + g * 150 + b * 29) >> 8, 0
    for i, c in enumerate((r, g, b)):
        v = lum + tdiv((c - lum) * STRIP_SAT, 256)
        out |= min(max(v, 0), 255) << (16 - 8 * i)
    return mix(out, STRIP_TINT, STRIP_TINT_A)


def frost_strip(wall, w, h):
    """The strip's picture (frost.c's make_strip): rows of 0..STRIP_H - 1."""
    rows = min(STRIP_H + 3 * (STRIP_BLUR // 2), h)
    img = [blur3([wall.at(x, y) for x in range(w)], STRIP_BLUR) for y in range(rows)]
    cols = [blur3([img[y][x] for y in range(rows)], STRIP_BLUR) for x in range(w)]
    out = []
    for y in range(STRIP_H):
        row = [strip_px(cols[x][y]) for x in range(w)]
        if y == STRIP_H - 1:
            row = [mix(p, 0xFFFFFF, STRIP_LINE_A) for p in row]
        out.append(row)
    return out


def parse_desk(log):
    """Each held step's `desk:` lines (the last paint's before the hold), and the tops' colours."""
    steps, cur, tops = {}, [], []
    for line in open(log, errors="replace"):
        m = re.search(r"compositor: testscene: (.*)", line)
        if not m:
            continue
        c = m.group(1).strip()
        if c.startswith("paint "):
            cur = []
        elif c.startswith("desk: "):
            f = c.split()
            cur.append((f[1], int(f[2]), tuple(map(int, f[3:7])), int(f[7])))
        elif c.startswith("top="):
            tops.append(int(c[4:].split(",")[2], 16))
        elif c.startswith("holding "):
            steps[int(c.split()[-1])] = list(cur)
    return steps, tops


def inside(b, x, y):
    return b[0] <= x < b[2] and b[1] <= y < b[3]


def in_corner(b, r, x, y):
    return (x < b[0] + r or x >= b[2] - r) and (y < b[1] + r or y >= b[3] - r)


def check_strip(step, pix, desk, strip, size):
    """The strip's pixels where nothing is on them: the frosting, the islands over it."""
    items = [b for kind, _, b, _ in desk if kind not in ("island", "top")]
    cards = [b for kind, _, b, _ in desk if kind in ("popover", "search", "note")]
    shadows = [(b[0] - SHADOW_SIDE, b[1] - SHADOW_ABOVE, b[2] + SHADOW_SIDE, b[3] + SHADOW_BELOW)
               for b in cards]
    islands = [b for kind, _, b, _ in desk if kind == "island"]
    bad = checked = 0
    for y in range(0, STRIP_H, 2):
        for x in range(0, size[0], 3):
            if any(inside(b, x, y) for b in items + shadows):
                continue
            want = strip[y][x]
            isl = [b for b in islands if inside(b, x, y)]
            if isl and in_corner(isl[0], ISLAND_R, x, y):
                continue
            if isl:
                want = mix(want, 0xFFFFFF, ISLAND_A)
            checked += 1
            if rgb(pix, x, y) != want:
                if bad < 5:
                    print("comp-check: step %d: strip pixel (%d, %d) is %06x, want %06x" %
                          (step, x, y, rgb(pix, x, y), want))
                bad += 1
    if checked < 500:
        print("comp-check: step %d: only %d strip pixels compared" % (step, checked))
        bad += 1
    return bad, checked


def check_items(step, pix, desk, strip, tops, size):
    """The lit items: the current screen's pill, the focused chip's tint, a
    minimised chip's dot; the toplevels' pixels and that none is under the strip."""
    bad = 0
    island = [b for kind, _, b, _ in desk if kind == "island"]

    def want_at(x, y, colour, what):
        nonlocal bad
        if rgb(pix, x, y) != colour:
            print("comp-check: step %d: %s at (%d, %d) is %06x, want %06x" %
                  (step, what, x, y, rgb(pix, x, y), colour))
            bad += 1
    for kind, _, b, on in desk:
        mid = (b[1] + b[3]) // 2
        if kind == "dot" and on:
            want_at((b[0] + b[2]) // 2, 19, APRICOT, "the current screen's pill")
        if kind == "chip" and on:
            base = mix(strip[mid][b[0] + 3], 0xFFFFFF, ISLAND_A)
            want_at(b[0] + 3, mid, mix(base, RASPBERRY, CHIP_FOCUS_A), "the focused chip")
    tn = 0
    shown = [(i, b) for kind, i, b, m in desk if kind == "top" and not m]
    for kind, i, b, minimised in desk:
        if kind != "top":
            continue
        tn += 1
        if b[1] < STRIP_H + 6:
            print("comp-check: step %d: toplevel %d's frame at y %d: under the strip" %
                  (step, i, b[1]))
            bad += 1
        x, y = b[0] + 1 + 20, b[1] + TITLE_H + 20   # its surface's (20, 20)
        over = [o for j, o in shown if j > i and inside((o[0] - SHADOW_SIDE, o[1] - SHADOW_ABOVE,
                                                         o[2] + SHADOW_SIDE, o[3] + SHADOW_BELOW),
                                                        x, y)]
        if minimised or over:
            continue   # hidden, or under another's frame or shadow
        want_at(x, y, pattern(tops[i] & 0xFFFFFF, 20, 20, False), "toplevel %d's pixel" % i)
    if not island or tn != len(tops):
        print("comp-check: step %d: %d islands, %d of %d toplevels in the log" %
              (step, len(island), tn, len(tops)))
        bad += 1
    return bad


def count_near(pix, b, colour, size, tol=24):
    n = 0
    for y in range(max(b[1], 0), min(b[3], size[1])):
        for x in range(max(b[0], 0), min(b[2], size[0])):
            p = rgb(pix, x, y)
            if all(abs((p >> sh & 0xFF) - (colour >> sh & 0xFF)) <= tol for sh in (0, 8, 16)):
                n += 1
    return n


def check_cards(step, pix, desk, size):
    """The cards: the popover under the strip (2 pixels), its right edge on its
    icon's; today on the calendar; the notification's tile; the search box's rows."""
    bad = 0
    byk = {}
    for kind, i, b, on in desk:
        byk.setdefault(kind, []).append((i, b, on))
    for i, b, _ in byk.get("popover", []):
        opener = {1: "vol", 2: "net", 3: "clock"}[i]
        ob = byk[opener][0][1]
        if b[1] != STRIP_H + 2 or b[2] != ob[2]:
            print("comp-check: step %d: the popover at %s, its opener at %s" % (step, b, ob))
            bad += 1
        if i == 3 and count_near(pix, b, APRICOT, size, 4) < 40:
            print("comp-check: step %d: no apricot day on the calendar" % step)
            bad += 1
    for i, b, buttons in byk.get("note", []):
        x, y = b[0] + 12 + 2, b[1] + 10 + 14
        if rgb(pix, x, y) != APRICOT:
            print("comp-check: step %d: notification %d's tile at (%d, %d) is %06x" %
                  (step, i, x, y, rgb(pix, x, y)))
            bad += 1
        covered = any(not (p[2] <= b[0] or p[0] >= b[2] or p[3] <= b[1] or p[1] >= b[3])
                      for _, p, _ in byk.get("popover", []))
        if buttons and not covered and \
                count_near(pix, (b[0], b[3] - 40, b[2], b[3]), 0xFFB340, size, 40) < 20:
            print("comp-check: step %d: notification %d has no apricot button text" % (step, i))
            bad += 1
    for _, b, _ in byk.get("search", []):
        tiles = count_near(pix, b, RASPBERRY, size, 4) + count_near(pix, b, BLACKCURRANT, size, 4)
        if tiles < 400:
            print("comp-check: step %d: the search box has %d pixels of letter tiles" %
                  (step, tiles))
            bad += 1
    return bad


def check_desk(step, prefix, size, desk, tops, strip):
    img = Image.open("%s-%d.ppm" % (prefix, step)).convert("RGB")
    img.save("%s-%d.png" % (prefix, step))
    if img.size != size:
        print("comp-check: step %d: the screenshot is %dx%d" % (step, img.size[0], img.size[1]))
        return False
    pix = img.load()
    bad, checked = check_strip(step, pix, desk, strip, size)
    bad += check_items(step, pix, desk, strip, tops, size)
    bad += check_cards(step, pix, desk, size)
    print("comp-check: step %d (the desktop): %d strip pixels compared, %d wrong" %
          (step, checked, bad))
    return bad == 0


def main():
    log, prefix = sys.argv[1], sys.argv[2]
    size, wins, steps, paints = parse(log)
    if not size or sorted(steps) != [1, 2, 3, 4, 5]:
        print("comp-check: the log has no output size or not the five steps: %s" % sorted(steps))
        return 1
    wall = Wallpaper(*size)
    ok = all([check_step(s, prefix, size, wins, steps[s], wall) for s in (1, 2, 3)])
    desk, tops = parse_desk(log)
    strip = frost_strip(wall, *size)
    ok = all([check_desk(s, prefix, size, desk[s], tops, strip) for s in (4, 5)]) and ok
    if paints.get(3, 0) == 0:
        print("comp-check: step 3 (a full-screen window) copied no tile straight from its buffer")
        ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
