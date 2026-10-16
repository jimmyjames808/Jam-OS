#!/usr/bin/env python3
"""base_unittests on Jam OS, suite by suite (docs/CR-PLAN.md, track CB2;
tools/cr-test.sh's `base` case calls it).

    base-run.py [options] <outdir> <stick>

<stick> is tools/cr/base-stick.py's stick (the programs, their test files
and approvals under /data/cr). The run is a few boots of it in QEMU
(tools/qemu-test.sh, the serial shell fed by tools/serial-feed.py):

  1. the listing: `base_unittests --gtest_list_tests`, every test of every
     suite (minus the DISABLED_ ones);
  2. a run of the shard's suites, each a program of its own
     (`--single-process-tests --gtest_filter=<suite>.*`: one suite's crash
     or abort ends only that suite, and a program starts in about a second
     from its pages);
  3. for each suite that crashed: the same suite again without the test that
     crashed (as many rounds as it takes, --rounds at most), and, if the
     serial script times out on a hung test, the suites not yet run.

Tests named in the skip list (tools/cr/base_unittests.skip: a
pattern, the layer that owns the failure, the reason) are not run, unless
--all: then they run, and a failure of one is a known failure, not a new
one. When the stick's /data is FAT, tools/cr/base_unittests.fat.skip (the
tests only FAT fails) is added. --shard I/N takes every N-th suite from I (a
shard has a listing boot of its own). The environment variables of the shell
(JAMOS_ENOSYS_LOG=1, TMPDIR) are --env NAME=value. The stick's /data is a FAT
or an ext volume (its listing is read back with mtools or jamext); with
QEMU_BOOT_NVME=1 in the environment the boots are from an installed NVMe drive.

Writes to <outdir>: results.txt (one line per test: its status and the
suite's run), failures/<test>.txt (the test's output, and the kernel log
lines that came with it: the ENOSYS log's, a kill's), enosys.txt (every
distinct ENOSYS line of the run), summary.txt. Exit status 0 when every
test that ran passed (or is a known failure) and nothing is missing."""
import argparse
import fnmatch
import hashlib
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import zlib

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")
KERNEL = re.compile(r"^\[\s*\d+\.\d+\]")            # a kernel log line: "[    2.53] ..."
PROGRAM = "/data/cr/out/jamos/base_unittests"
JAMEXT = os.path.join(TOP, "build", "host", "jamext")
LINE_MAX = 200                                       # the shell takes 240 characters a line


def read_log(path):
    """The serial log as text lines, without colours or carriage returns."""
    try:
        data = open(path, "rb").read()
    except OSError:
        return []
    text = ANSI.sub(b"", data).replace(b"\r", b"").decode("utf-8", "replace")
    return text.split("\n")


class Boot:
    """One QEMU boot of a copy of the stick, fed a script."""

    def __init__(self, args, name, script_lines, timeout, keep=False):
        self.keep = keep   # leave the stick (with what the guest wrote) for the caller
        self.dir = os.path.join(args.outdir, name)
        self.name = name
        os.makedirs(self.dir, exist_ok=True)
        self.script = os.path.join(self.dir, "script.txt")
        with open(self.script, "w") as f:
            f.write("\n".join(script_lines) + "\n")
        self.timeout = timeout
        self.args = args
        # QEMU's sockets are named after the run (build/.qemu-<name>.ser):
        # runs of one checkout at once need names of their own.
        self.qname = "%s-%s" % (args.tag, name)
        self.log = os.path.join(self.dir, self.qname + ".log")
        self.feed_failed = None

    def run(self):
        a = self.args
        img = self.img = os.path.join(self.dir, name_img(self.qname))
        shutil.copyfile(a.stick, img)
        env = dict(os.environ, QEMU_IMAGE=img, QEMU_INPUT=self.script,
                   QEMU_TIMEOUT=str(self.timeout), QEMU_MEM=a.mem)
        self.saved = os.path.join(self.dir, self.qname + "-saved.img")
        if self.keep:
            env["QEMU_SAVE"] = self.saved   # without it the guest's writes are thrown away
        qout = os.path.join(self.dir, "qemu.out")
        feed = os.path.join(self.dir, self.qname + ".feed")   # serial-feed's stderr, as it goes
        with open(qout, "w") as out:
            p = subprocess.Popen(["tools/qemu-test.sh", self.dir, self.qname, "shell"],
                                 stdout=out, stderr=subprocess.STDOUT, env=env, cwd=TOP,
                                 start_new_session=True)
            t0 = time.time()
            while p.poll() is None:
                time.sleep(2)
                # The serial script gave up (a wait timed out): QEMU stays up until
                # its own limit, so end the run here (the group is ours).
                why = grep_file(feed, r"serial-feed: .*: no '.*' within")
                if why and not self.feed_failed:
                    self.feed_failed = why
                    time.sleep(3)
                    os.killpg(p.pid, signal.SIGTERM)
                    p.wait(timeout=60)
                    break
                if time.time() - t0 > self.timeout * 2 + 60:
                    os.killpg(p.pid, signal.SIGTERM)
                    p.wait(timeout=60)
                    self.feed_failed = "the run's time limit"
                    break
            self.rc = p.returncode
        if not self.feed_failed:
            self.feed_failed = grep_file(feed, r"serial-feed: .*: no '.*' within")
        if not self.keep:
            try:
                os.unlink(img)
            except OSError:
                pass
        return read_log(self.log)


def name_img(name):
    return name + "-stick.img"


def grep_file(path, pattern):
    rx = re.compile(pattern)
    try:
        for line in open(path, errors="replace"):
            if rx.search(line):
                return line.strip()
    except OSError:
        pass
    return None


HEADER = ["wait 120 Jam OS shell", "wait {prompt}", "seen 60 init: /data mounted",
          "send T=x", "wait {prompt}"]


def header(envs):
    lines = list(HEADER)
    for e in envs:
        lines += ["send export " + e, "wait {prompt}"]
    lines += ["send B=" + PROGRAM, "wait {prompt}", "send cd /data/cr/out/jamos", "wait {prompt}"]
    return lines


FOOTER = ["send reboot -f", "wait 60 reboot: resetting"]


def listing(args):
    """{suite: [test, ...]} in the order gtest lists them."""
    # The list goes to a file on /data and is read from the stick on this side
    # (QEMU_SAVE keeps what the guest wrote): ten thousand lines through the
    # serial line come out garbled.
    script = header(args.env) + [
        "send run $B --gtest_list_tests > /data/cr/tests.txt ; echo le-$T=$?",
        "wait 180 re:le-x=\\d+", "wait {prompt}"] + FOOTER
    b = Boot(args, "list", script, 300, keep=True)
    b.run()
    txt = os.path.join(b.dir, "tests.txt")
    try:
        if args.fs == "ext":   # an ext /data (a stick, or a GPT drive's second partition)
            cmd = [JAMEXT, "-q", "--part", "2", "get", b.saved, "/cr/tests.txt", txt]
        else:
            at = subprocess.run([sys.executable, "tools/mkimage.py", "--data-at", b.saved],
                                capture_output=True, text=True, cwd=TOP, check=True).stdout.strip()
            cmd = ["mcopy", "-n", "-i", "%s@@%s" % (b.saved, at), "::/cr/tests.txt", txt]
        r = subprocess.run(cmd, cwd=TOP, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("base-run: can't read the test list from the stick: %s said %r "
                             "(the stick is kept: %s; see %s)" %
                             (os.path.basename(cmd[0]), (r.stderr or r.stdout).strip()[-300:], b.saved, b.log))
    except (subprocess.CalledProcessError, OSError) as e:
        raise SystemExit("base-run: can't read the test list from the stick (%s; see %s)" % (e, b.log))
    for f in (b.img, b.saved):
        try:
            os.unlink(f)
        except OSError:
            pass
    suites, cur = {}, None
    for line in open(txt, errors="replace").read().split("\n"):
        line = line.rstrip("\r")
        m = re.match(r"^(\S+\.)(\s+#.*)?$", line)
        if m:
            cur = m.group(1)[:-1]
            suites.setdefault(cur, [])
            continue
        m = re.match(r"^  (\S+)(\s+#.*)?$", line)
        if m and cur is not None:
            suites[cur].append(m.group(1))
    if not suites:
        raise SystemExit("base-run: the listing boot listed nothing (see %s)" % b.log)
    return suites


def skip_list(path):
    """[(pattern, layer, reason)] from the skip file."""
    out = []
    group = None   # a "[layer] reason" line sets (layer, reason) for the patterns after it
    if not path or not os.path.isfile(path):
        return out
    for n, line in enumerate(open(path), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^\[(\S+)\]\s+(.+)$", line)
        if m:
            group = (m.group(1), m.group(2))
            continue
        w = line.split(None, 2)
        if len(w) == 1 and group:
            out.append((w[0], group[0], group[1]))
        elif len(w) == 3:
            out.append((w[0], w[1], w[2]))
        else:
            raise SystemExit("%s:%d: want '<pattern> <layer> <reason>', or a pattern after a "
                             "'[layer] reason' line" % (path, n))
    return out


def filter_for(suite, tests, neg):
    """The --gtest_filter for a suite, less the tests in neg (full names)."""
    f = suite + ".*"
    if neg:
        f += "-" + ":".join(sorted(neg))
    return f


def item_lines(i, flt, suite_timeout, extra=""):
    """The script lines for suite number i: split a long filter in pieces
    through the shell's variable F."""
    out = []
    if len(flt) > LINE_MAX - 60:
        # Pieces begin with ':' (or are the first), never with a letter: the
        # shell would read "$F" and the letters as one variable's name.
        parts, cur = [], ""
        for tok in re.split(r"(?=:)", flt):
            if len(cur) + len(tok) > LINE_MAX - 30:
                parts.append(cur)
                cur = ""
            cur += tok
        parts.append(cur)
        out += ["send F=" + parts[0], "wait {prompt}"]
        for p in parts[1:]:
            out += ["send F=$F" + p, "wait {prompt}"]
        flt = "$F"
    out += ["send run $B --single-process-tests%s --gtest_filter=%s ; echo se-$T-%d=$?" % (extra, flt, i),
            "wait %d re:se-x-%d=\\d+" % (suite_timeout, i), "wait {prompt}"]
    return out


class Result:
    def __init__(self):
        self.status = {}     # test -> OK FAILED CRASH HANG SKIPPED
        self.text = {}       # test -> [lines of its output and the kernel log with it]
        self.suite_note = {}  # suite -> a line about it (a crash at exit, no tests run)
        self.enosys = {}     # distinct "enosys: ..." text -> first test it came with


def parse_boot(lines, items, res, hung_last):
    """Fold a boot's log into res. items: [(i, suite, filter)]. Returns
    {suite: crashed-test or None} for the suites that ran to their end, and
    the first suite that did not end (a hang), or None."""
    pos = {}
    for n, line in enumerate(lines):
        m = re.search(r"se-x-(\d+)=(\d+)", line)
        if m and not line.startswith("send") and "$T" not in line:
            pos[int(m.group(1))] = (n, int(m.group(2)))
    start = 0
    crashed, hung = {}, None
    for i, suite, flt in items:
        if i in pos:
            end, code = pos[i]
        else:
            end, code = len(lines), None
        seg = lines[start:end + 1]
        start = end + 1
        cur, buf = None, []
        done = {}
        for line in seg:
            m = re.search(r"\[ RUN      \] (\S+)", line)
            if m:
                cur, buf = m.group(1), []
                continue
            # (a parameterized test that failed says ", where GetParam() = ..." first)
            m = re.search(r"\[\s+(OK|FAILED|SKIPPED)\s+\] (\S+?)(?:,\s+where .*?)? \(\d+ ms\)", line)
            if m and cur == m.group(2):
                st = {"OK": "OK", "FAILED": "FAILED", "SKIPPED": "SKIPPED"}[m.group(1)]
                res.status[cur] = st
                res.text[cur] = buf
                cur, buf = None, []
                continue
            if cur is not None:
                buf.append(line)
                m = re.search(r"enosys: (.*)$", line)
                if m and m.group(1) not in res.enosys:
                    res.enosys[m.group(1)] = cur
        for line in seg:
            m = re.search(r"enosys: (.*)$", line)
            if m and m.group(1) not in res.enosys:
                res.enosys[m.group(1)] = "(" + suite + ", between tests)"
        if i not in pos:
            if cur is not None:
                res.status[cur] = "HANG"
                res.text[cur] = buf
                crashed[suite] = cur
            hung = (i, suite)
            break
        finished = any(re.search(r"\[==========\] \d+ tests? from .* ran\.", l) for l in seg)
        if cur is not None and finished:
            # The program got to gtest's summary: the test's own result line
            # was lost (a kernel log line cut into it). Code 0 means it passed;
            # otherwise leave it without a result, so a later round runs it alone.
            if code == 0:
                res.status[cur] = "OK"
                res.text[cur] = buf
        elif cur is not None:
            res.status[cur] = "CRASH"
            res.text[cur] = buf + ["(the program ended with code %d)" % code]
            crashed[suite] = cur
        elif code not in (0, 1):
            res.suite_note[suite] = "exit code %d with every test finished" % code
        elif not any(t.startswith(suite + ".") for t in res.status):
            res.suite_note[suite] = "no test ran (exit code %d)" % code
    return crashed, hung


def main(argv):
    ap = argparse.ArgumentParser(prog="base-run.py", description=__doc__.split("\n\n")[0])
    ap.add_argument("--shard", default="0/1")
    ap.add_argument("--skip", default=os.path.join(TOP, "tools", "cr", "base_unittests.skip"))
    ap.add_argument("--skip-fat", default=os.path.join(TOP, "tools", "cr", "base_unittests.fat.skip"),
                    help="more tests to leave out when the stick's /data is FAT")
    ap.add_argument("--all", action="store_true", help="run the skip list's tests too")
    ap.add_argument("--rounds", type=int, default=20)
    ap.add_argument("--chunk", type=int, default=6, help="tests to a program after round 1")
    ap.add_argument("--suite-timeout", type=int, default=240)
    ap.add_argument("--mem", default="4G")
    ap.add_argument("--env", action="append", default=["JAMOS_ENOSYS_LOG=1"])
    ap.add_argument("--only", help="only suites matching this glob (several: separated by spaces or commas)")
    ap.add_argument("--args", default="", help="more arguments for base_unittests, e.g. ' --v=1' (a space first)")
    ap.add_argument("outdir")
    ap.add_argument("stick")
    args = ap.parse_args(argv)
    os.makedirs(args.outdir, exist_ok=True)
    si, sn = (int(x) for x in args.shard.split("/"))
    # QEMU's sockets and logs are named after the run (build/.qemu-<tag>-<boot>.ser,
    # in the checkout all of a batch's runs share): a name of this output directory's own.
    args.tag = "cb%s%d" % (hashlib.md5(os.path.abspath(args.outdir).encode()).hexdigest()[:6], si)
    # What /data is on the stick: the listing boot's file is read back with
    # mtools or jamext (a GPT image, from QEMU_BOOT_NVME, is told by its volume).
    r = subprocess.run([sys.executable, "tools/mkimage.py", "--data-fs", args.stick],
                       capture_output=True, text=True, cwd=TOP)
    args.fs = r.stdout.strip() if r.returncode == 0 and r.stdout.strip() in ("ext", "fat") else "fat"

    suites = listing(args)
    all_tests = [s + "." + t for s, ts in suites.items() for t in ts]
    skips = skip_list(args.skip)
    if args.fs == "fat":   # what only a FAT /data fails
        skips += skip_list(args.skip_fat)
    enabled = {}
    for s, ts in suites.items():
        if s.split("/")[-1].startswith("DISABLED_"):
            continue
        enabled[s] = [t for t in ts if not t.startswith("DISABLED_")]
    # A suite's shard is its name's checksum, not its place in the list, so
    # shards agree whatever each one's own listing boot saw.
    mine = [s for s in sorted(enabled) if zlib.crc32(s.encode()) % sn == si]
    if args.only:
        globs = args.only.replace(",", " ").split()
        mine = [s for s in mine if any(fnmatch.fnmatchcase(s, g) for g in globs)]

    def skipped(full):
        for pat, layer, reason in skips:
            if fnmatch.fnmatchcase(full, pat):
                return (layer, reason)
        return None

    excluded = set()   # full names not run: the skip list's, unless --all
    if not args.all:
        for s in mine:
            for t in enabled[s]:
                if skipped(s + "." + t):
                    excluded.add(s + "." + t)
    res = Result()

    def unrun(s):
        return [s + "." + t for t in enabled[s]
                if s + "." + t not in res.status and s + "." + t not in excluded]

    # Round 1 runs each suite whole. After that only what has no result is
    # run, a few named tests to a program: a crash or a hang there ends
    # only its own few, so a suite of crashers costs a round or two.
    pending = [s for s in mine if unrun(s)]
    rounds = 0
    while pending and rounds < args.rounds:
        rounds += 1
        items = []
        for s in pending:
            if rounds == 1:
                items.append((len(items), s, filter_for(s, [], {x for x in excluded if x.startswith(s + ".")})))
            else:
                u = unrun(s)
                # Six tests to a program in round 2, then fewer: what crashed
                # twice is probably the kind of test that crashes, and alone
                # it costs one program, not its neighbours.
                size = args.chunk if rounds == 2 else (2 if rounds <= 4 else 1)
                for k in range(0, len(u), size):
                    items.append((len(items), s, ":".join(u[k:k + size])))
        script = header(args.env)
        for i, s, flt in items:
            script += item_lines(i, flt, args.suite_timeout, args.args)
        script += FOOTER
        b = Boot(args, "run%d" % rounds, script, 6 * 3600)
        lines = b.run()
        before = len(res.status)
        crashed, hung = parse_boot(lines, items, res, None)
        print("base-run: round %d: %d programs, %d crashed, %s, %d new results" %
              (rounds, len(items), len(crashed), "stopped by a hang" if hung else "ran to the end",
               len(res.status) - before))
        pending = [s for s in mine if unrun(s)]
        if len(res.status) == before and rounds > 1:
            print("base-run: a round with no new result: the rest is NOT RUN")
            break

    # ---- the reconciliation and the reports ----
    ran = set(res.status)
    expected = [s + "." + t for s in mine for t in enabled[s]]
    known, new, missing = [], [], []
    counts = {}
    for t in expected:
        st = res.status.get(t)
        sk = skipped(t)
        label = st or ("on the skip list" if sk and not args.all else "NOT RUN")
        counts[label] = counts.get(label, 0) + 1
        if st is None:
            if not (sk and not args.all):
                missing.append(t)
        elif st in ("FAILED", "CRASH", "HANG"):
            (known if sk else new).append(t)
    with open(os.path.join(args.outdir, "results.txt"), "w") as f:
        for t in expected:
            f.write("%s\t%s\n" % (res.status.get(t, "skipped-by-list" if skipped(t) and not args.all
                                                  else "NOT RUN"), t))
    fd = os.path.join(args.outdir, "failures")
    os.makedirs(fd, exist_ok=True)
    for t in known + new:
        with open(os.path.join(fd, re.sub(r"[^A-Za-z0-9_.-]", "_", t) + ".txt"), "w") as f:
            f.write("%s: %s\n" % (t, res.status[t]))
            f.write("\n".join(res.text.get(t, [])[:200]) + "\n")
    with open(os.path.join(args.outdir, "enosys.txt"), "w") as f:
        for text, first in res.enosys.items():
            f.write("%s\t(first with %s)\n" % (text, first))
    lines = ["base-run: shard %s: %d suites, %d tests expected" % (args.shard, len(mine), len(expected))]
    lines.append("base-run: " + ", ".join("%d %s" % (n, k) for k, n in sorted(counts.items())))
    lines.append("base-run: %d new failures, %d known failures, %d missing, %d rounds" %
                 (len(new), len(known), len(missing), rounds))
    for s, note in sorted(res.suite_note.items()):
        lines.append("base-run: suite %s: %s" % (s, note))
    for t in new[:60]:
        lines.append("base-run: NEW %s %s" % (res.status[t], t))
    for t in missing[:30]:
        lines.append("base-run: MISSING %s" % t)
    with open(os.path.join(args.outdir, "summary.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0 if not new and not missing else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
