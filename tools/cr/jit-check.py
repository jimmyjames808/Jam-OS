#!/usr/bin/env python3
"""The jit case's run of cr-test.sh (docs/CR-PLAN.md, CR-e): its serial
log, read for what tools/cr/jit.html said in each of its two loads
(tools/cr/jit.txt), and its screenshots:

    jit-check.py <dir>

<dir> has jit.log, jit-on.png and jit-off.png. The page's lines
("crjit: ...", console lines the browser logs):

    on      js and wasm results the loop's own value, a load past the
            module's memory a RuntimeError, then done; a copy of the loop
            optimized by TurboFan (--allow-natives-syntax: the page asks V8
            to, and V8's status of it says TurboFan) gives the same value;
            the tier-up's function (called often, the event loop between
            its batches) optimized in the background by Maglev or TurboFan
    off     (--jitless) the same js result, "wasm none", done
    faster  the hot loop at least 3 times faster with the JIT than without
    marks   `ps` after each load (the processes as a tree: the browser and
            at least four children): with the JIT one or two children marked
            [jit] (the renderers: the page's and a spare), never the browser
            and never every child (the GPU process, the network and storage
            services gave the power up); --jitless none
    shots   the page's text drawn in both (its yellow on the dark ground)

Prints a line for each check, the timings (the figures BENCH.md takes),
and PASS or FAIL; exit 0 if all passed."""
import os
import re
import sys

from PIL import Image

N = 30000000
X0, Y0 = 647, 92            # the page's (0, 0) on the desktop (multi-check.py's)
GROUND = (27, 27, 58)       # #1b1b3a
TEXT = (245, 215, 110)      # #f5d76e


def expected():
    acc = 0
    for i in range(N):
        acc = (acc * 31 + i) & 0xffffffff
    return acc - (1 << 32) if acc >= 1 << 31 else acc


def runs(log):
    """The page's lines, split into loads at each "done"."""
    out, cur = [], []
    for m in re.finditer(r'crjit: ([^"\r\n]*)', log):
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
    order: a listing starts at its header line and ends at the next prompt;
    a process's depth in the tree is the indent of its name."""
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


def text_drawn(png):
    try:
        im = Image.open(png).convert("RGB")
    except OSError:
        return False
    hits = 0
    for y in range(Y0 + 40, Y0 + 160, 2):
        for x in range(X0 + 40, X0 + 400, 2):
            p = im.getpixel((x, y))
            if all(abs(a - b) <= 40 for a, b in zip(p, TEXT)):
                hits += 1
    ground = im.getpixel((X0 + 560, Y0 + 400))
    return hits >= 20 and all(abs(a - b) <= 12 for a, b in zip(ground, GROUND))


def main(argv):
    if len(argv) != 2:
        print("usage: jit-check.py <dir>", file=sys.stderr)
        return 2
    d = argv[1]
    try:
        log = open(os.path.join(d, "jit.log"), errors="replace").read()
    except OSError as e:
        print("jit: no log: %s" % e)
        return 1
    want = str(expected())
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        print("jit: %s: %s%s" % (name, "ok" if cond else "FAILED", (" (" + detail + ")") if detail else ""))
        ok &= bool(cond)

    loads = runs(log)
    check("two loads", len(loads) == 2, "%d" % len(loads))
    on = loads[0] if loads else []
    off = loads[1] if len(loads) > 1 else []
    js_on, wasm_on, trap = field(on, "js"), field(on, "wasm"), field(on, "trap")
    print("jit: the page was told it is %s" % " and ".join(field(on, "page") or ["?"]))
    js_off, wasm_off = field(off, "js"), field(off, "wasm")
    check("on: js", js_on and js_on[2] == want, " ".join(js_on or []))
    check("on: wasm", wasm_on and len(wasm_on) == 3 and wasm_on[2] == want, " ".join(wasm_on or []))
    check("on: trap", trap and trap[0] == "RuntimeError:", " ".join(trap or []))
    tf = field(on, "turbofan")
    tier = field(on, "tier")
    check("on: TurboFan", tf and len(tf) == 4 and tf[2] == want and int(tf[3]) & (1 << 5),
          " ".join(tf or []))
    names = ((4, "Maglev"), (5, "TurboFan"), (14, "Sparkplug"), (9, "optimizing in the background"))

    def tiers(bits):
        return ", ".join(n for b, n in names if bits & (1 << b)) or "interpreted"
    if tier and len(tier) == 2:
        print("jit: on: hot() a second after its runs: status %s (%s)" % (tier[0], tiers(int(tier[0]))))
        check("on: tier-up in the background", int(tier[1]) & (3 << 4),
              "chunk(): status %s (%s)" % (tier[1], tiers(int(tier[1]))))
    else:
        check("on: tier status", False)
    for name, run in (("on", on), ("off", off)):
        t = field(run, "tierup")
        check("%s: tier-up batches" % name, t and len(t) == 2, " ".join(t or []))
        if t and len(t) == 2:
            print("jit: %s: tier-up: first batch %s ms, last %s ms" % (name, t[0], t[1]))
    check("off: js", js_off and js_off[2] == want, " ".join(js_off or []))
    check("off: wasm none", wasm_off == ["none"], " ".join(wasm_off or []))
    if js_on and js_off:
        speed = float(js_off[0]) / max(float(js_on[0]), 0.1)
        check("faster", speed >= 3, "%.1fx" % speed)
        print("jit: js loop of %d: best %s ms (5 runs %s ms) with the JIT, best %s ms "
              "(5 runs %s ms) --jitless: %.1fx" % (N, js_on[0], js_on[1], js_off[0], js_off[1], speed))
    if wasm_on and len(wasm_on) == 3:
        print("jit: wasm loop of %d: best %s ms (5 runs %s ms)" % (N, wasm_on[0], wasm_on[1]))
    tables = ps_tables(log)
    check("ps after each load", len(tables) == 2, "%d" % len(tables))
    if len(tables) == 2:
        for name, t, lo, hi in (("on", tables[0], 1, 2), ("off", tables[1], 0, 0)):
            top = min((d for d, _ in t), default=0)
            browser = [m for d, m in t if d == top]
            kids = [m for d, m in t if d > top]
            marked = sum(1 for _, m in t if m)
            check("%s: [jit] marks" % name, browser == [False] and len(kids) >= 4 and
                  lo <= marked <= hi and not all(kids),
                  "%d processes, %d marked" % (len(t), marked))
    for shot in ("jit-on", "jit-off"):
        check("%s drawn" % shot, text_drawn(os.path.join(d, shot + ".png")))
    print("jit: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
