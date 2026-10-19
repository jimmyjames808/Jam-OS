#!/usr/bin/env python3
"""What cr-test.sh's `fonts` case (docs/CR-PLAN.md, FONTS notes) got from
headless_shell:

    fonts-check.py <dir>
    fonts-check.py --has-pkg <program>

<dir> has the files the guest wrote (tools/cr/fonts.txt; the case copies them
out of the saved stick): dom-sys.html, dom-psn.html and dom-ctl.html
(--dump-dom), sys.png, own2.png and ctl.png (--screenshot,
--window-size=1000,1400) and the guest's terminal log
run/fonts.log. The page is tools/cr/fonts.html: a line of text in each script
and its own check (its script draws every grapheme cluster of each line on a
canvas and compares the pixels with a character no font has, which Chromium
draws as the primary font's box, and with nothing). The sys files are the
system fonts found by default (no FONTCONFIG_FILE: /etc/fonts/fonts.conf, the
packages' /pkg/share/fonts/fonts.conf); the ctl files are the control, a
fonts.conf of Inter alone. Checked:

    every run's exit status, and no complaint of fontconfig's in the log
    sys DOM: every line of the page has no missing cluster (the 29 rows of
        fonts.html: English, Greek, Cyrillic, Chinese both ways, Japanese,
        Korean, Arabic, Hebrew, Hindi, Thai, the other Indic scripts, Georgian,
        Armenian, Ethiopic, Khmer, Lao, Myanmar, Sinhala, Odia, math, symbols,
        emoji), the reference box has pixels, and each language's CJK fallback
        draws the same pixels as the regional face asked for by name (ja the
        JP face, ko KR, zh-Hans and zh-CN SC, zh-Hant and zh-TW TC, zh-HK HK)
        while the four regional faces draw some of the seven test characters
        differently (so the match is not all four faces being one)
    ctl DOM (the checker's own proof that it can fail): the English line
        (Inter has Latin) has no missing cluster, every other script's line
        (Greek and Cyrillic too: the Inter beside the program is cut to
        Latin) has
    sys and ctl PNGs: 1000x1400, each row of the sys page has ink and, for the
        scripts Inter lacks, differs from the control's row
    what the system's configuration reads (docs/SANDBOX-PLAN.md, SB1): the
        stick has a planted /data/.config/fontconfig/fonts.conf that puts
        Inter first for every family, and the sys DOM's faces by name (Noto
        Sans and Serif, Liberation Sans and Serif, Inter) are five different
        faces with sans-serif and serif drawn as one of theirs; no cache was
        written on /data by the system runs ($HOME/.cache/fontconfig,
        /data/.cache/fontconfig); a run with poison.conf (a scan rule that
        renames every face of the packages' jamos directory, its cache in
        /data/.cache/fontconfig) wrote its cache files there, and the system
        run after it (dom-psn.html, the planted file gone) drew the faces as
        themselves and everything as dom-sys.html did; own2.png (a cache of the program's own, read) is
        the same picture as sys.png
    the times the shell's `time` gave (printed, not checked): rc2, the system
        fonts with no cache (a scan at every start), rcA a run that scans and
        writes a cache of its own, rcB the run that reads it

Prints a line for each check and PASS or FAIL; exit 0 if all passed.

--has-pkg <program>: exit 0 if the program's list (its JamOS note,
tools/checkwants.py) has `mount /pkg r`: a headless_shell or content_shell
linked after the FONTS track, whose libc serves /etc/fonts/fonts.conf too
(cr-test.sh's `fonts` case asks before it builds a stick)."""
import importlib.util
import os
import re
import sys

from PIL import Image

# the page's rows, in order (fonts.html's ROWS)
ROWS = ["english", "greek", "cyrillic", "chinese-simplified", "chinese-traditional",
        "japanese", "korean", "arabic", "hebrew", "hindi", "thai", "bengali", "tamil",
        "telugu", "gujarati", "gurmukhi", "kannada", "malayalam", "georgian",
        "armenian", "ethiopic", "khmer", "lao", "myanmar", "sinhala", "odia", "math",
        "symbols", "emoji"]
INTER_HAS = ("english",)   # the control's Inter (third_party/inter, cut to Latin) has this and nothing else
CONTROL_BOXES = [r for r in ROWS if r not in INTER_HAS and r not in ("math", "symbols", "emoji")]
TOP, ROW = 40, 46          # the page's geometry, pixels (fonts.html)
SIZE = (1000, 1400)
X0, X1 = 120, 990          # the text part of a row (the tag is to the left)
INK = 160                  # a pixel darker than this is ink
INK_MIN = 150              # pixels of ink a row must have
DIFF_MIN = 150             # pixels of a row that must differ from the control's
FACES = {"ja": "JP", "ko": "KR", "zh-Hans": "SC", "zh-CN": "SC", "zh-Hant": "TC",
         "zh-TW": "TC", "zh-HK": "HK"}

ok = True


def report(good, what):
    global ok
    ok &= bool(good)
    print("fonts-check: %s: %s" % (what, "ok" if good else "WRONG"))


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace")
    except OSError:
        return None


def parse(dom):
    """The page's result lines: (refs, rows by name, cjk lines)."""
    refs, rows, cjk = [], {}, []
    for line in (dom or "").splitlines():
        # the first line follows the <pre> tag that holds them
        m = re.search(r"(?:^|>)((?:ref|row|cjk)\|.*)$", line.strip())
        if not m:
            continue
        p = m.group(1).split("|")
        if p[0] == "ref" and len(p) == 4:
            refs.append((p[1], int(p[2]), p[3]))
        elif p[0] == "row" and len(p) >= 5:
            rows[p[1]] = (p[2], int(p[3]), int(p[4]), p[5] if len(p) > 5 else "")
        elif p[0] == "cjk" and len(p) == 6:
            cjk.append((p[1], p[2], p[3], p[4] == "1", p[5]))
    return refs, rows, cjk


def looks(dom):
    """The page's face and gen lines: {(kind, name): (ink, hash)}."""
    got = {}
    for line in (dom or "").splitlines():
        m = re.search(r"(?:^|>)((?:face|gen)\|[^<]*)", line.strip())
        if not m:
            continue
        p = m.group(1).split("|")
        if len(p) == 4 and p[2].isdigit():
            got[(p[0], p[1])] = (int(p[2]), p[3])
    return got


SANS, SERIF = ("Noto Sans", "Liberation Sans"), ("Noto Serif", "Liberation Serif")


def check_faces(what, got):
    """In one run the faces by name are themselves (five different drawings:
    a planted file putting one face first, or a cache renaming the faces,
    makes them one) and the generic families are drawn with the faces the
    packages' lists give."""
    faces = {n: got.get(("face", n)) for n in SANS + SERIF + ("Inter",)}
    if not all(faces.values()):
        report(False, "%s has the face lines (%s)" % (what, ", ".join(n for n, v in faces.items() if not v)))
        return
    report(all(v[0] > 0 for v in faces.values()) and len({v[1] for v in faces.values()}) == len(faces),
           "%s: the faces by name are %d different drawings: %s"
           % (what, len(faces), ", ".join("%s %s" % (n, v[1]) for n, v in faces.items())))
    for gen, want in (("sans-serif", SANS), ("serif", SERIF)):
        g = got.get(("gen", gen))
        is_ = [n for n in want if g and faces[n][1] == g[1]]
        report(bool(is_), "%s: %s is drawn with %s (one of %s)"
               % (what, gen, is_[0] if is_ else "another face (%s)" % (g[1] if g else "no line"), ", ".join(want)))


def check_looks(sys_dom, psn_dom):
    """The system's configuration and caches are the packages' alone: the sys
    run (the planted fonts.conf on the stick) and the run after the poisoned
    cache was written each draw the faces as themselves, and both the same."""
    s, p = looks(sys_dom), looks(psn_dom)
    check_faces("sys DOM (a fonts.conf planted)", s)
    check_faces("psn DOM (a cache written to /data/.cache/fontconfig)", p)
    diff = sorted("%s %s" % k for k in s if p.get(k) != s[k])
    report(s and not diff, "the psn run draws the faces and generic families as the sys run%s"
           % (" (not: " + ", ".join(diff) + ")" if diff else ""))


def check_sys(dom):
    refs, rows, cjk = parse(dom)
    if not rows:
        report(False, "sys DOM has the page's result lines (the script did not run?)")
        return
    report(len(refs) >= 3 and len({h for _, _, h in refs}) == 1 and refs[0][1] > 0,
           "the missing-character reference is one box with pixels (%s)"
           % ", ".join("%s: %d px" % (n, i) for n, i, _ in refs))
    for name in ROWS:
        r = rows.get(name)
        if r is None:
            report(False, "sys row %s: not in the result" % name)
            continue
        lang, total, missing, what = r
        report(total >= 5 and missing == 0,
               "sys row %s (%s): %d clusters, %d missing%s"
               % (name, lang, total, missing, " (" + what + ")" if missing else ""))
    by_lang = {}
    for lang, ch, face, same, h in cjk:
        by_lang.setdefault(lang, []).append(same)
    for lang in sorted(FACES):
        got = by_lang.get(lang, [])
        report(len(got) == 7 and all(got),
               "sys CJK %s: its fallback draws the same as the %s face by name (%d of %d characters)"
               % (lang, FACES[lang], sum(got), len(got)))
    # the regional faces are not one: the hashes of the faces' drawing of the 7 characters
    tuples = {}
    for lang, ch, face, same, h in cjk:
        tuples.setdefault(face, {})[ch] = h
    distinct = {tuple(sorted(d.items())) for f, d in tuples.items() if f in ("JP", "KR", "SC", "TC")}
    report(len(distinct) >= 3,
           "sys CJK: the JP, KR, SC and TC faces draw the test characters in %d different ways (3 or more)"
           % len(distinct))


def check_ctl(dom):
    refs, rows, cjk = parse(dom)
    if not rows:
        report(False, "control DOM has the page's result lines")
        return
    for name in INTER_HAS:
        r = rows.get(name)
        report(r and r[2] == 0, "control row %s: Inter has it, %s missing" % (name, r[2] if r else "no row"))
    for name in CONTROL_BOXES:
        r = rows.get(name)
        report(r and r[2] > 0, "control row %s: boxes (%s of %s clusters missing)"
               % (name, r[2] if r else "no row", r[1] if r else "?"))


def ink_count(im, y0, y1):
    px = im.load()
    n = 0
    for y in range(y0, y1):
        for x in range(X0, X1):
            if px[x, y] < INK:
                n += 1
    return n


def diff_count(a, b, y0, y1):
    pa, pb = a.load(), b.load()
    n = 0
    for y in range(y0, y1):
        for x in range(X0, X1):
            if abs(pa[x, y] - pb[x, y]) > 24:
                n += 1
    return n


def check_png(dir):
    ims = {}
    for name in ("sys", "ctl"):
        path = os.path.join(dir, name + ".png")
        if not os.path.isfile(path):
            report(False, "%s screenshot: no %s" % (name, path))
            return
        im = Image.open(path).convert("L")
        report(im.size == SIZE, "%s screenshot is %dx%d (%dx%d)" % ((name,) + SIZE + im.size))
        if im.size != SIZE:
            return
        ims[name] = im
    for i, name in enumerate(ROWS):
        y0, y1 = TOP + ROW * i + 4, TOP + ROW * (i + 1) - 4
        ink = ink_count(ims["sys"], y0, y1)
        report(ink >= INK_MIN, "screenshot row %s has ink: %d pixels (%d or more)" % (name, ink, INK_MIN))
        if name in CONTROL_BOXES:
            d = diff_count(ims["sys"], ims["ctl"], y0, y1)
            report(d >= DIFF_MIN, "screenshot row %s differs from the control's boxes: %d pixels (%d or more)"
                   % (name, d, DIFF_MIN))
    # a cache of the program's own gives the same fonts as the scan
    own = os.path.join(dir, "own2.png")
    if not os.path.isfile(own):
        report(False, "own-cache screenshot: no %s" % own)
        return
    same = Image.open(own).convert("RGB").tobytes() == Image.open(os.path.join(dir, "sys.png")).convert("RGB").tobytes()
    report(same, "own2.png (the program's own cache, read) is the same picture as sys.png (a scan)")


def has_pkg(path):
    top = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    spec = importlib.util.spec_from_file_location("checkwants", os.path.join(top, "tools", "checkwants.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    with open(path, "rb") as f:
        texts = mod.notes(f.read())
    return any(b"mount /pkg r" in t.splitlines() for t in texts)


def main(argv):
    if len(argv) == 3 and argv[1] == "--has-pkg":
        return 0 if has_pkg(argv[2]) else 1
    if len(argv) != 2:
        print(__doc__.split("\n\n")[1].strip(), file=sys.stderr)
        return 2
    dir = argv[1]
    term = read(os.path.join(dir, "run", "fonts.log"))
    if term is None:
        report(False, "no run/fonts.log")
        term = ""
    # What the shell printed, the log lines taken out: a kernel's or a
    # service's line may come anywhere in a line the shell echoes, and is
    # whole (tools/bigprog-test.sh's rule), so the echoed commands below are
    # found again once their pieces join.
    term = re.sub(r"\[ *\d+\.\d{6}\] [^\n]*\n", "", term)
    for n in ("1", "2", "A", "B", "P", "6", "3", "4"):
        m = re.search(r"^rc%s-(\d+)" % n, term, re.M)
        report(m and m.group(1) == "0", "run %s exit status %s" % (n, m.group(1) if m else "missing"))
    # fontconfig's own complaints (its "Fontconfig error: ..." lines) in the
    # runs that used the system fonts: the default config was found and parsed
    bad = [l for l in term.splitlines() if re.match(r"Fontconfig (error|warning)", l)]
    report(not bad, "no fontconfig complaint in the log%s" % (": " + bad[0][:120] if bad else ""))
    # fontconfig's cache files are named for a directory's hash: the system
    # runs wrote none on /data (the configuration names no cache there), and
    # the poison run wrote its own into /data/.cache/fontconfig
    cache = r"[0-9a-f]{32}-le64\.cache-\d+"
    m = re.search(r"fontconfig; echo ls1-\"x\"(.*?)^ls1-x", term, re.S | re.M)
    report(m and not re.search(cache, m.group(1)),
           "the system runs wrote no cache in $HOME/.cache/fontconfig or /data/.cache/fontconfig%s"
           % ("" if m else " (no ls1 output)"))
    m = re.search(r"fontconfig; echo ls2-\"x\"(.*?)^ls2-x", term, re.S | re.M)
    report(m and re.search(cache, m.group(1)), "the poison run wrote its cache in /data/.cache/fontconfig")
    # the times: `time` prints "time: <s> s (status <n>)" before the echo
    for n, what in (("2", "system fonts, no cache (a scan)"), ("A", "own cache, scanned and written"),
                    ("B", "own cache, read")):
        m = re.search(r"time: ([0-9.]+) s \(status \d+\)\s*rc%s-" % n, term)
        print("fonts-check: time of run %s (%s): %s s" % (n, what, m.group(1) if m else "?"))
    check_sys(read(os.path.join(dir, "dom-sys.html")))
    check_looks(read(os.path.join(dir, "dom-sys.html")), read(os.path.join(dir, "dom-psn.html")))
    check_ctl(read(os.path.join(dir, "dom-ctl.html")))
    check_png(dir)
    print("fonts-check: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
