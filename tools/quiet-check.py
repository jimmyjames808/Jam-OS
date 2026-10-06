#!/usr/bin/env python3
"""Count the text rows a terminal window shows (the quiet boot's check).

    quiet-check.py <screenshot.png> [--rows N | --min-rows N]

A QEMU screenshot of the desktop at 1280x800 with the first terminal
tiled alone (the default: it fills x 6..1274, y 46..794 under the strip;
user/services/compositor/look.h) in the smooth font (cells 9x21, 10 pixels
of padding: user/services/console/cells.h). A row of cells has text if any
pixel in it is bright (a channel over 90: the terminal's background and
its border are darker; its text, the prompt's colours and the cursor are
not). Prints the rows with text; --rows N: exit 1 unless exactly the first
N rows have text and none after them (the quiet boot: the banner and the
prompt, N = 2); --min-rows N: exit 1 unless at least N rows have text (a
`verbose` boot: the log is on the screen)."""
import sys
from PIL import Image

X0, X1 = 20, 1260          # inside the window's padding, clear of its border
Y0, CELL_H, Y_END = 56, 21, 790


def rows_with_text(path):
    img = Image.open(path).convert("RGB")
    if img.size != (1280, 800):
        sys.exit(f"quiet-check: {path} is {img.size[0]}x{img.size[1]}, not 1280x800")
    px = img.load()
    rows = []
    r = 0
    while Y0 + (r + 1) * CELL_H <= Y_END:
        y0 = Y0 + r * CELL_H
        lit = any(max(px[x, y]) > 90 for y in range(y0 + 2, y0 + CELL_H - 2)
                  for x in range(X0, X1, 2))
        if lit:
            rows.append(r)
        r += 1
    return rows


def main():
    if len(sys.argv) not in (2, 4):
        sys.exit(__doc__)
    rows = rows_with_text(sys.argv[1])
    print(f"quiet-check: {sys.argv[1]}: text on rows {rows}")
    if len(sys.argv) == 2:
        return 0
    want = int(sys.argv[3])
    if sys.argv[2] == "--rows":
        ok = rows == list(range(want))
    elif sys.argv[2] == "--min-rows":
        ok = len(rows) >= want
    else:
        sys.exit(__doc__)
    print(f"quiet-check: {'PASS' if ok else 'FAIL'} ({sys.argv[2]} {want})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
