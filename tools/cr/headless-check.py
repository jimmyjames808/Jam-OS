#!/usr/bin/env python3
"""What cr-test.sh's `headless` case (docs/CR-PLAN.md, track CB3) got from
headless_shell:

    headless-check.py <dir>

<dir> has the files the guest wrote (tools/cr/headless.txt; the case copies
them out of the saved stick): dom-local.html, dom-http.html, dom-https.html
and dom-nocert.html (--dump-dom), local.png, http.png and https.png
(--screenshot, --window-size=800,600) and the guest's terminal log
run/headless.log; and the servers' logs http-server.log and
https-server.log (tools/cr/headless-server.py). Each page is
tools/cr/headless.html, with its picture beside it (local) or from a second
name (http, https). Checked:

    the exit status of each run (the log's `rcN-<status>`: 0 for the pages,
        whatever it was for the refused one), no complaint of the
        sandbox's libc interceptor (dlsym for localtime or getaddrinfo), and
        the shell's `host` answers for the two names
    each DOM: the page's title and the line its script wrote, which says the
        script ran and the picture arrived with its size (320x160)
    each PNG: 800x600, and the colours at the box, the picture, the circle
        and the ground, white pixels where the heading is and where the
        picture's own text is
    http: the server saw the page for www.jam:8000 and the picture for
        mac.jam:8000, twice each (the dump and the screenshot), from a
        HeadlessChrome
    https: the same on port 8443 over TLS; the run without the trust flag
        never got to ask (the page was asked for twice, not three times), the
        server saw its handshake fail, and the log says the page failed with
        a certificate error

Prints a line for each check and PASS or FAIL; exit 0 if all passed."""
import os
import re
import sys

from PIL import Image

TOLERANCE = 6
GROUND = (0x1e, 0x3a, 0x5f)
RED = (0xe9, 0x45, 0x60)
ORANGE = (0xfb, 0x8b, 0x24)
CIRCLE = (0xe3, 0x64, 0x14)

# (what, x, y, colour): the page's own coordinates (the window is 800x600)
POINTS = [
    ("ground", 700, 560, GROUND),
    ("box top-left", 45, 45, RED),
    ("box middle", 200, 110, RED),
    ("box bottom-right", 355, 175, RED),
    ("picture", 340, 220, ORANGE),
    ("picture's circle", 120, 280, CIRCLE),
]
# (what, x0, y0, x1, y1, brightest channel at least, pixels at least)
REGIONS = [
    ("white heading text", 40, 380, 400, 430, 235, 150),
    ("white text inside the picture", 190, 265, 330, 305, 235, 20),
]

ok = True


def report(good, what):
    global ok
    ok &= bool(good)
    print("headless-check: %s: %s" % (what, "ok" if good else "WRONG"))


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        return None


def near(got, want):
    return all(abs(g - w) <= TOLERANCE for g, w in zip(got[:3], want))


def check_dom(where, dir):
    path = os.path.join(dir, "dom-%s.html" % where)
    dom = read(path)
    if dom is None:
        report(False, "%s DOM: no %s" % (where, path))
        return
    report("<title>CB3 %s</title>" % where in dom, "%s DOM has its title" % where)
    report("js ok 42, picture 320x160" in dom,
           "%s DOM has the script's line (script ran, picture 320x160)" % where)


def check_png(where, dir):
    path = os.path.join(dir, "%s.png" % where)
    if not os.path.isfile(path):
        report(False, "%s screenshot: no %s" % (where, path))
        return
    im = Image.open(path).convert("RGB")
    report(im.size == (800, 600), "%s screenshot is 800x600 (%dx%d)" % ((where,) + im.size))
    if im.size != (800, 600):
        return
    for what, x, y, want in POINTS:
        p = im.getpixel((x, y))
        report(near(p, want), "%s screenshot, %s at (%d, %d): #%02x%02x%02x, want #%02x%02x%02x"
               % ((where, what, x, y) + tuple(p) + tuple(want)))
    for what, x0, y0, x1, y1, bright, need in REGIONS:
        n = sum(1 for y in range(y0, y1) for x in range(x0, x1) if min(im.getpixel((x, y))) >= bright)
        report(n >= need, "%s screenshot, %s: %d pixels of #%02x or more (want %d)" % (where, what, n, bright, need))


def count(log, pattern):
    return len(re.findall(pattern, log or "", re.M))


def check_server(where, dir, host_page, host_pic, tls):
    log = read(os.path.join(dir, "%s-server.log" % where))
    if log is None:
        report(False, "%s server: no log" % where)
        return
    page = "/page-%s.html" % where
    n = count(log, r"^GET %s host=%s " % (re.escape(page), re.escape(host_page)))
    report(n == 2, "%s server saw GET %s for %s %d times (want 2: the dump and the shot)" % (where, page, host_page, n))
    n = count(log, r"^GET /pic.svg host=%s " % re.escape(host_pic))
    report(n >= 2, "%s server saw GET /pic.svg for %s %d times (want 2 or more)" % (where, host_pic, n))
    report("ua=" in log and "HeadlessChrome/" in log, "%s server's client is a HeadlessChrome" % where)
    if tls:
        n = count(log, r" tls=TLSv1\.[23]\b")
        report(n >= 4, "%s server's requests were over TLS (%d)" % (where, n))
        n = count(log, r"^handshake failed: ")
        report(n >= 1, "%s server saw %d failed handshake(s) (the run without the trust flag)" % (where, n))
    else:
        report(" tls=" not in log, "%s server's requests were plain" % where)


def main(argv):
    if len(argv) != 2:
        print(__doc__.split("\n\n")[1].strip(), file=sys.stderr)
        return 2
    dir = argv[1]
    term = read(os.path.join(dir, "run", "headless.log"))
    if term is None:
        report(False, "no run/headless.log")
        term = ""
    for n in range(1, 7):
        m = re.search(r"^rc%d-(\d+)" % n, term, re.M)
        report(m and m.group(1) == "0", "run %d exit status %s" % (n, m.group(1) if m else "missing"))
    # the sandbox's libc interceptor looks up localtime and getaddrinfo with
    # dlsym(RTLD_NEXT): the layer answers, so neither complaint is logged
    report("dlsym" not in term and "Cannot find getaddrinfo" not in term,
           "the log has no dlsym complaint (localtime, getaddrinfo)")
    # the shell's `host` before the by-name runs: netpeer's DNS answered both names
    for name in ("www.jam", "mac.jam"):
        report(re.search(r"^%s has address [0-9.]+" % re.escape(name), term, re.M),
               "the guest's DNS answered %s" % name)
    m = re.search(r"^rc7-(\d+)", term, re.M)
    report(m, "run 7 (no trust flag) ended: status %s" % (m.group(1) if m else "missing"))
    m = re.search(r"Page load failed: (net::ERR_CERT_[A-Z_]+)", term)
    report(m, "run 7's log says %s" % (m.group(1) if m else "nothing about a certificate"))
    dom = read(os.path.join(dir, "dom-nocert.html")) or ""
    report("js ok" not in dom, "run 7 dumped no page (the script's line is not in dom-nocert.html)")
    for where in ("local", "http", "https"):
        check_dom(where, dir)
        check_png(where, dir)
    check_server("http", dir, "www.jam:8000", "mac.jam:8000", False)
    check_server("https", dir, "www.jam:8443", "mac.jam:8443", True)
    print("headless-check: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
