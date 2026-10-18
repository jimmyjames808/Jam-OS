#!/usr/bin/env python3
"""The screenshots of cr-test.sh's shell-wl case (docs/CR-PLAN.md, track
CB4), looked at where the page (tools/cr/shell-wl.html) put things:

    shell-wl-check.py <dir>

<dir> has the script's shell-wl-page.png, shell-wl-list.png and
shell-wl-wheel.png (tools/cr/shell-wl.txt, 1280x800). The page's top-left
corner is at (647, 92) on the desktop (the tiling layout: content_shell's
window on the right half, under its toolbar). Each point is checked
against the colour the page drew there, give or take TOLERANCE in each
channel:

    page   the red box (40, 40) 320x140 at three points; the page's
           ground right of it and where the yellow band will come (the
           band is at 700 to 900, below the window)
    list   the list's popup under it: its first row (One, the selected
           one) highlighted, the other two light
    wheel  the band where the ground was (the page scrolled some 360
           pixels: three notches), the box still where it was (fixed)

Prints a line for each point and PASS or FAIL; exit 0 if all passed."""
import os
import sys

from PIL import Image

X0, Y0 = 647, 92      # the page's (0, 0) on the desktop
TOLERANCE = 12
RED = (233, 69, 96)        # #e94560
GROUND = (15, 76, 92)      # #0f4c5c
BAND = (245, 215, 110)     # #f5d76e


def near(got, want):
    return all(abs(g - w) <= TOLERANCE for g, w in zip(got, want))


def highlighted(p):
    """A selected row: Chromium's blue."""
    return p[2] > 150 and p[0] < 100


def light(p):
    """An unselected row of the popup: near white."""
    return min(p) > 200


# (shot, what, page x, page y, test, its words)
POINTS = [
    ("page", "box's top-left", 43, 43, RED, None),
    ("page", "box's middle", 199, 108, RED, None),
    ("page", "box's bottom-right", 356, 176, RED, None),
    ("page", "ground right of the box", 453, 108, GROUND, None),
    ("page", "ground where the band will come", 453, 508, GROUND, None),
    ("list", "popup row One, selected", 183, 433, highlighted, "highlighted"),
    ("list", "popup row Two", 183, 465, light, "light"),
    ("list", "popup row Three", 183, 497, light, "light"),
    ("wheel", "the band, scrolled up into the window", 453, 508, BAND, None),
    ("wheel", "box's middle, fixed", 199, 108, RED, None),
]


def main(argv):
    if len(argv) != 2:
        print(__doc__.split("\n\n")[1].strip(), file=sys.stderr)
        return 2
    shots = {}
    ok = True
    for shot, what, x, y, test, words in POINTS:
        if shot not in shots:
            path = os.path.join(argv[1], "shell-wl-%s.png" % shot)
            if not os.path.isfile(path):
                print("shell-wl-check: %s: no %s" % (shot, path))
                ok = False
                shots[shot] = None
                continue
            shots[shot] = Image.open(path).convert("RGB")
        im = shots[shot]
        if im is None:
            continue
        p = im.getpixel((X0 + x, Y0 + y))
        good = test(p) if callable(test) else near(p, test)
        want = words or "#%02x%02x%02x" % test
        print("shell-wl-check: %s: %s at (%d, %d): #%02x%02x%02x, want %s: %s"
              % (shot, what, X0 + x, Y0 + y, p[0], p[1], p[2], want, "ok" if good else "WRONG"))
        ok &= good
    print("shell-wl-check: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
