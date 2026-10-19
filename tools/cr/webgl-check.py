#!/usr/bin/env python3
"""The webgl case's run of cr-test.sh (docs/CR-PLAN.md, CR-e): its serial
log, read for what tools/cr/webgl.html said in each of its two loads
(tools/cr/webgl.txt), and its screenshots:

    webgl-check.py <dir>

<dir> has webgl.log, webgl-on.png and webgl-off.png. The page's lines
("crgl: ...", console lines the browser logs):

    on      (--enable-unsafe-swiftshader) a WebGL context, its renderer
            SwiftShader's, the three pixels read back as drawn, the draws
            timed, done
    off     no WebGL context, done
    marks   `ps` after each load (the processes as a tree: the browser and
            at least four children): the browser never marked [jit], never
            every child, and one child more marked with SwiftShader than
            without it (the GPU process, which keeps the JIT power only when
            it runs SwiftShader; the others marked are the renderers)
    shots   the band drawn on the page in the first (its red half, its
            green half and the cleared ground above it, at the canvas's
            place on the desktop), and the page's ground there in the second

Prints a line for each check, SwiftShader's renderer and the draws' time,
and PASS or FAIL; exit 0 if all passed."""
import os
import re
import sys

from PIL import Image

X0, Y0 = 647, 92            # the page's (0, 0) on the desktop (multi-check.py's)
CANVAS = (40, 40)           # the canvas's place on the page (webgl.html)
GROUND = (27, 27, 58)       # the page's #1b1b3a
# (x, y) on the canvas and the colour drawn there
POINTS = (((80, 120), (255, 0, 0)), ((240, 120), (0, 255, 0)), ((160, 20), (51, 102, 204)))


def runs(log):
    """The page's lines, split into loads at each "done"."""
    out, cur = [], []
    for m in re.finditer(r'crgl: ([^"\r\n]*)', log):
        line = m.group(1).strip()
        if cur and cur[-1] == line:
            continue   # the same console line twice (the log and its echo)
        cur.append(line)
        if line == "done":
            out.append(cur)
            cur = []
    return out


def field(run, name):
    for line in run:
        w = line.split()
        if w and w[0] == name:
            return w[1:]
    return None


def ps_tables(log):
    """Each `ps` listing's content_shell processes, [(depth, marked)] in its
    order (jit-check.py's)."""
    tables, cur = [], None
    for line in log.splitlines():
        if re.match(r"\s*PID\s+JOB\s+THR\s+STATE", line):
            cur = []
            tables.append(cur)
            continue
        m = re.match(r"\s*\d+\s+\d+\s+\d+\s+\w+\s+[0-9:.]+\s+[0-9.]+ \w+( +)content_shell"
                     r"( \[jit\])?\s*$", line)
        if cur is not None and m:
            cur.append((len(m.group(1)), bool(m.group(2))))
        elif cur is not None and re.search(r"> ", line):
            cur = None   # the next command
    return tables


def near(p, want, tol=12):
    return all(abs(a - b) <= tol for a, b in zip(p, want))


def shot(png, drawn):
    """The band and its ground where the canvas is (drawn), or the page's
    ground there (not drawn); and the ground beside the canvas either way."""
    try:
        im = Image.open(png).convert("RGB")
    except OSError:
        return False, "no picture"
    seen = []
    ok = near(im.getpixel((X0 + 560, Y0 + 200)), GROUND)
    for (x, y), colour in POINTS:
        p = im.getpixel((X0 + CANVAS[0] + x, Y0 + CANVAS[1] + y))
        seen.append("%d,%d,%d" % p)
        ok &= near(p, colour if drawn else GROUND)
    return ok, " ".join(seen)


def main(argv):
    if len(argv) != 2:
        print("usage: webgl-check.py <dir>", file=sys.stderr)
        return 2
    d = argv[1]
    try:
        log = open(os.path.join(d, "webgl.log"), errors="replace").read()
    except OSError as e:
        print("webgl: no log: %s" % e)
        return 1
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        print("webgl: %s: %s%s" % (name, "ok" if cond else "FAILED", (" (" + detail + ")") if detail else ""))
        ok &= bool(cond)

    loads = runs(log)
    check("two loads", len(loads) == 2, "%d" % len(loads))
    on = loads[0] if loads else []
    off = loads[1] if len(loads) > 1 else []
    ctx = field(on, "context")
    check("on: a WebGL context", ctx and ctx[0] == "webgl", " ".join(ctx or []))
    renderer = " ".join(field(on, "renderer") or [])
    check("on: SwiftShader's", "SwiftShader" in renderer, renderer)
    pixels = field(on, "pixels")
    check("on: drawn", field(on, "drawn") == ["ok"], " ".join(pixels or []))
    frames = field(on, "frames")
    check("on: draws timed", frames and len(frames) == 2, " ".join(frames or []))
    if frames and len(frames) == 2:
        print("webgl: on: %s draws of the band (320x120) and a finish: %s ms" % tuple(frames))
    ctx = field(off, "context")
    check("off: no WebGL", ctx and ctx[0] == "none", " ".join(ctx or []))
    tables = ps_tables(log)
    check("ps after each load", len(tables) == 2, "%d" % len(tables))
    if len(tables) == 2:
        marks = []
        for name, t in (("on", tables[0]), ("off", tables[1])):
            top = min((d for d, _ in t), default=0)
            browser = [m for d, m in t if d == top]
            kids = [m for d, m in t if d > top]
            marks.append(sum(1 for m in kids if m))
            check("%s: [jit] marks" % name, browser == [False] and len(kids) >= 4 and
                  not all(kids), "%d processes, marked: %s" %
                  (len(t), " ".join("jit" if m else "-" for m in kids)))
        check("the GPU process marked with SwiftShader only", marks[0] == marks[1] + 1,
              "%d children marked with it, %d without" % tuple(marks))
    for name, drawn in (("webgl-on", True), ("webgl-off", False)):
        good, seen = shot(os.path.join(d, name + ".png"), drawn)
        check("%s %s" % (name, "drawn" if drawn else "not drawn"), good, seen)
    print("webgl: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
