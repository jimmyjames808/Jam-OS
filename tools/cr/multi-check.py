#!/usr/bin/env python3
"""The multi case's run of cr-test.sh (docs/CR-PLAN.md, track CB5): its
screenshots, looked at where the page (tools/cr/multi.html) put things,
and its serial log, read for what the processes did:

    multi-check.py <dir>

<dir> has the script's (tools/cr/multi.txt) multi-page.png, multi-killed.png
and multi-reload.png (1280x800; the page's top-left corner at (647, 92),
content_shell's window on the right half, as shell-wl-check.py has it)
and multi.log:

    page    the red box (40, 40) 320x140 at three points, the ground beside
            it: the renderer's frame, drawn by the GPU process's compositor
    killed  the box is gone: the renderer was killed (chrome://kill) and
            the browser dropped its frame
    reload  the box again: a renderer started again for Refresh
    log     the page said "loaded ... visit 1", then the renderer said it
            was terminating itself, then the page said "visit 2"; the first
            `ps` listed the browser and at least four children (GPU,
            network, storage, renderer: every process content_shell); after
            `jobs` said content_shell was "done (exit 0)", a `ps` with none left

Prints a line for each check and PASS or FAIL; exit 0 if all passed."""
import os
import re
import sys

from PIL import Image

X0, Y0 = 647, 92      # the page's (0, 0) on the desktop
TOLERANCE = 12
RED = (233, 69, 96)        # #e94560
GROUND = (15, 76, 92)      # #0f4c5c


def near(got, want):
    return all(abs(g - w) <= TOLERANCE for g, w in zip(got, want))


def not_red(p):
    return not near(p, RED)


# (shot, what, page x, page y, test, its words)
POINTS = [
    ("page", "box's top-left", 43, 43, RED, None),
    ("page", "box's middle", 199, 108, RED, None),
    ("page", "box's bottom-right", 356, 176, RED, None),
    ("page", "ground right of the box", 453, 108, GROUND, None),
    ("killed", "box's middle", 199, 108, not_red, "not the box (no frame)"),
    ("reload", "box's top-left", 43, 43, RED, None),
    ("reload", "box's middle", 199, 108, RED, None),
    ("reload", "ground right of the box", 453, 108, GROUND, None),
]

PS_LINE = re.compile(r"^ *\d+ +\d+ +\d+ +\w+ +[0-9:.]+ +[0-9.]+ \w+ +(\S+)\s*$")


def ps_blocks(text):
    """Each `ps` listing in the log: the names it lists, in order."""
    blocks, cur = [], None
    for line in text.splitlines():
        line = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", line).rstrip("\r")
        m = PS_LINE.match(line)
        if m:
            if cur is None:
                cur = []
                blocks.append(cur)
            cur.append(m.group(1))
        elif re.match(r"^\d+ processes", line):
            cur = None
    return blocks


def check_log(path):
    ok = True
    try:
        with open(path, "rb") as f:
            text = f.read().decode("utf-8", "replace")
    except OSError:
        print("multi-check: log: no %s" % path)
        return False
    marks = [r"cb5: loaded 623x695 visit 1", r"Intentionally terminating current process",
             r"cb5: loaded 623x695 visit 2", r"\[1\] +\d+ +done \(exit 0\) +content_shell"]
    at, done = -1, -1
    for m in marks:
        found = re.compile(m).search(text, at + 1)
        good = found is not None
        print("multi-check: log: \"%s\"%s: %s" % (m, " after the last" if at >= 0 else "",
                                                 "ok" if good else "WRONG"))
        ok &= good
        if good:
            at = done = found.start()
    before = ps_blocks(text[:done] if done >= 0 else text)
    after = ps_blocks(text[done:]) if done >= 0 else []
    counts = [b.count("content_shell") for b in before]
    good = bool(counts) and counts[0] >= 5
    print("multi-check: log: content_shell processes listed while it ran: %s (want 5 or more "
          "first): %s" % (", ".join(map(str, counts)) or "none", "ok" if good else "WRONG"))
    ok &= good
    left = after[0].count("content_shell") if after else -1
    good = left == 0
    print("multi-check: log: content_shell processes after it exited: %s: %s"
          % (left if left >= 0 else "no listing", "ok" if good else "WRONG"))
    return ok and good


def main(argv):
    if len(argv) != 2:
        print(__doc__.split("\n\n")[1].strip(), file=sys.stderr)
        return 2
    shots = {}
    ok = True
    for shot, what, x, y, test, words in POINTS:
        if shot not in shots:
            path = os.path.join(argv[1], "multi-%s.png" % shot)
            if not os.path.isfile(path):
                print("multi-check: %s: no %s" % (shot, path))
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
        print("multi-check: %s: %s at (%d, %d): #%02x%02x%02x, want %s: %s"
              % (shot, what, X0 + x, Y0 + y, p[0], p[1], p[2], want, "ok" if good else "WRONG"))
        ok &= good
    ok &= check_log(os.path.join(argv[1], "multi.log"))
    print("multi-check: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
