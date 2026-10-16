#!/usr/bin/env python3
"""Type into a QEMU guest's serial port (the shell tests).

    serial-feed.py <unix socket> <script>

Connects to QEMU's serial chardev socket (tools/qemu-test.sh with
QEMU_INPUT=<script>: `-chardev socket,...,server=on,wait=on`, so QEMU waits
for us before it starts the guest), follows what the guest writes, and runs
the script, one command per line:

    wait [<seconds>] <text>   until <text> appears in the output after the
                              point the last wait matched (default 60 s)
    seen [<seconds>] <text>   the same, but anywhere in the output so far
                              (for lines whose order against the last wait
                              isn't fixed); doesn't move that point
                              In <text> of either, {prompt} stands for the
                              shell's prompt, `jam:<cwd>>` with its colours
                              (user/services/shell/main.c), whatever the
                              current directory: `wait {prompt}` waits for
                              the next one. Any other {name} is a number:
                              one not seen yet is captured from the match
                              (e.g. `seen jamjar: next at {nx},{ny}`), one
                              captured before stands for its value.
                              A <text> that begins "re:" is a regular
                              expression (Python's, over bytes) whose
                              named groups, which must be numbers, are
                              captured the same way, e.g.
                              `seen re:nvme(?P<d>[0-9]+) +QEMU`: a disk
                              whose name isn't known beforehand.
    send <text>               <text> and Enter (CR), one byte at a time; a {name}
                              captured earlier stands for its number
    type <text>               <text> without Enter; \\e \\r \\n \\t \\xNN escapes
    sleep <seconds>
    shot <name>               a screenshot: <name>.png next to the log
                              (through QEMU's monitor, $QEMU_MON / $SHOT_DIR)
    shots <name> <n> <s>      n screenshots <s> seconds apart (made PNGs
                              after the last): <name>-01.png, -02 ...
    monitor <command>         a QEMU monitor command ($QEMU_MON), e.g.
                              `monitor device_del kbd1`; a device_add is
                              sent again until QEMU takes it (up to 10 s:
                              it refuses a drive whose last device it
                              hasn't freed yet)
    usbkeys <text>            <text> typed on QEMU's keyboards (monitor
                              `sendkey`, one key at a time: the USB keyboard
                              path); \r (or \n) is Enter; a-z 0-9 space
                              - . / and : only
    pointer <x> <y>           QEMU's mouse to pixel (x, y): pushed into the
                              top-left corner (where it is clamped), then
                              moved in steps of at most 6 counts, which the
                              compositor's (and libfun's) acceleration
                              takes 1:1, each its own monitor command; x
                              and y are sums of numbers and captured
                              {name}s, e.g. 640-{lx}+{nx} (a window's
                              place plus a point in it)
    # comment, blank lines ignored

After the script it keeps reading until QEMU closes the socket (a `reboot`
with -no-reboot ends QEMU), so the guest never blocks on a full socket.
Exit status 0 if every wait matched; on a timeout it says which and exits 1
(and stops typing, but still drains until QEMU goes).

Test verdicts. Whatever the script waits for, a test program's own failure
line anywhere in the output fails the script (FAILED_LINES below: a
summary "N passed, M FAILED" of utest, usbtest, hdatest, mixtest, nettest
or ktest, a failed check, posixtest's FAIL), and so does a `utest only` or
`utest except` that said it was running and never gave its summary (it
crashed or was killed). A script's own `wait utest: 3 passed` is a prefix of
"utest: 3 passed, 1 FAILED" and could not tell. The run stops at the first
failure line (QEMU is told to quit) and says which line it was.

    serial-feed.py --selftest      the line patterns, on sample lines
    serial-feed.py --scan <log>... the failure lines a saved log would have
                                   made this script fail on"""
import re
import socket
import sys
import threading
import time

# The lines a failed test prints (the programs' result lines: user/include/check.h,
# utest, usbtest, hdatest, mixtest, nettest, dnstest, posixtest, the kernel's ktest, the shell's soak).
FAILED_LINES = [
    # "utest: 5 passed, 1 FAILED", usbtest's "..., 1 FAILED, 2 skipped", ktest's "..., 0 skipped, 1 FAILED"
    re.compile(rb"\b[0-9]+ passed, (?:[0-9]+ skipped, )?[1-9][0-9]* FAILED"),
    re.compile(rb"\bdnstest: [0-9]+ passed, [1-9][0-9]* failed"),
    re.compile(rb"\butest: FAILED tests:"),
    re.compile(rb"\bFAILED at line [0-9]+"),   # a failed check (user/include/check.h)
    re.compile(rb"\bktest: FAILED "),
    re.compile(rb"\bposixtest: FAIL(?:ED)?\b"),
    re.compile(rb"\bposixtest: .*: FAIL(?:ED)?$"),   # a section's "name: FAILED", a spawn test's "...: FAIL"
    re.compile(rb"\bshell: soak: FAILED"),
    re.compile(rb"\b[0-9]+ run\(s\), [1-9][0-9]* FAILED"),   # the soak's utest runs between loops
]
UTEST_RUNNING = re.compile(rb"\butest: running [0-9]+ of [0-9]+ tests\b")
UTEST_RESULT = re.compile(rb"\butest: [0-9]+ passed\b")
ESCAPES = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")


class Verdict:
    """The guest's output, line by line, for the failure lines above.
    failed: the lines that matched (the first ones); running: the line of a
    `utest only` that has not given its summary yet, else None."""
    KEEP = 10

    def __init__(self, say=None):
        self.at = 0
        self.failed = []
        self.running = None
        self.crashed = None   # a utest that was running when the next one began
        self.say = say

    def feed(self, buf):
        """Look at the complete lines of buf that were not looked at yet."""
        while True:
            nl = buf.find(b"\n", self.at)
            if nl < 0:
                return
            self.line(bytes(buf[self.at:nl]))
            self.at = nl + 1

    def line(self, raw):
        text = ESCAPES.sub(b"", raw).replace(b"\r", b"").strip()
        if UTEST_RUNNING.search(text):
            if self.running is not None and self.crashed is None:
                self.crashed = self.running
            self.running = text
        elif UTEST_RESULT.search(text):
            self.running = None
        if any(p.search(text) for p in FAILED_LINES):
            if len(self.failed) < self.KEEP:
                self.failed.append(text.decode(errors="replace"))
            if self.say:
                self.say("serial-feed: the guest reports a failed test: "
                         + text.decode(errors="replace"))

    def problems(self, at_end):
        """Why the script fails: what the guest printed, and (at the end of
        the run, or when another began) a utest that never gave its summary."""
        out = list(self.failed)
        no_result = self.crashed or (self.running if at_end else None)
        if no_result is not None and not self.failed:   # (else the run was cut short)
            out.append("a utest began and gave no summary (it crashed, was killed or had not "
                       "finished): " + no_result.decode(errors="replace"))
        return out


def selftest():
    """Sample lines against the patterns. Exit status 0 if all are right."""
    bad = [b"utest: 509 passed, 1 FAILED", b"usbtest: 4 passed, 1 FAILED, 2 skipped",
           b"hdatest: 3 passed, 2 FAILED", b"nettest rx: 10 passed, 1 FAILED",
           b"ktest: loop 1 of 2 done in 68 ms: 1 passed, 0 skipped, 2 FAILED",
           b"  kernel tests: 2 loop(s): 5 passed, 0 skipped, 1 FAILED",
           b"dnstest: 4 passed, 1 failed", b"utest: FAILED tests: fat_names session_hangup",
           b"utest: fat_names: FAILED at line 331: x is OK, want ERR",
           b"ktest: FAILED aspace_maps: x == 3 failed", b"posixtest: FAIL", b"posixtest: FAILED fds.c:9: x",
           b"posixtest: procfs: FAILED", b"posixtest: spawn-data: /data/p self-image: FAIL",
           b"shell: soak: FAILED after 40 s", b"utest between the loops: 3 run(s), 1 FAILED",
           b"\x1b[1m\x1b[91mutest: 3 passed, 1 FAILED\x1b[0m\r"]
    good = [b"utest: 11 passed", b"utest: running 3 of 960 tests (only)", b"utest: fat_names ok (12 ms)",
            b"usbtest: 12 passed", b"hdatest: 3 passed", b"dnstest: 5 passed, 0 failed",
            b"ktest: loop 1 of 2 done in 68 ms: 1 passed, 0 skipped, 0 FAILED",
            b"utest between the loops: 3 run(s), 0 FAILED", b"posixtest: PASS", b"posixtest: procfs: ok",
            b"posixtest: spawn-data: /data/p self-image: PASS", b"shell: soak: PASSED after 40 s",
            b"send utest only x; echo st-$T=$?", b"ktest (live system): 231 passed, 23 skipped; not checked: 4"]
    fails = []
    for l in bad:
        v = Verdict()
        v.feed(bytearray(l + b"\n"))
        if not v.failed:
            fails.append("not caught: " + l.decode())
    for l in good:
        v = Verdict()
        v.feed(bytearray(l + b"\n"))
        if v.failed:
            fails.append("a false alarm: " + l.decode())
    # utest's start and summary lines pair up; a start with none is a crash.
    v = Verdict()
    v.feed(bytearray(b"utest: running 1 of 9 tests (only)\nutest: x ok (3 ms)\nutest: 1 passed\n"))
    if v.problems(True):
        fails.append("a finished utest only is a problem")
    v = Verdict()
    v.feed(bytearray(b"utest: running 1 of 9 tests (only)\nrun: bin/utest was killed after 40 ms\n"))
    if v.problems(False) or not v.problems(True):
        fails.append("a utest only with no summary: a problem at the end, not before")
    v = Verdict()
    v.feed(bytearray(b"utest: running 1 of 9 tests (only)\nutest: running 1 of 9 tests (only)\nutest: 1 passed\n"))
    if not v.problems(False):
        fails.append("a utest only that began again without a summary")
    v = Verdict()
    v.feed(bytearray(b"utest: running 1 of 9 tests (only)\nutest: x: FAILED at line 3: y\n"))
    if len(v.problems(True)) != 1:
        fails.append("a failed check in a run that was cut short: one problem, not a second for the missing summary")
    for f in fails:
        print("serial-feed selftest: FAILED: " + f)
    print("serial-feed selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


if sys.argv[1:2] == ["--selftest"]:
    sys.exit(selftest())
if sys.argv[1:2] == ["--scan"]:
    status = 0
    for path in sys.argv[2:]:
        v = Verdict(say=lambda m, p=path: print(p + ": " + m))
        v.feed(bytearray(open(path, "rb").read() + b"\n"))
        for why in v.problems(True)[len(v.failed):]:
            print(path + ": " + why)
            status = 1
        status = status or (1 if v.failed else 0)
    sys.exit(status)

sock_path, script = sys.argv[1], sys.argv[2]

s = None
for _ in range(200):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(sock_path)
        break
    except OSError:
        s = None
        time.sleep(0.05)
if s is None:
    sys.exit(f"serial-feed: can't connect to {sock_path}")

buf = bytearray()
lock = threading.Condition()
closed = False
verdict = Verdict(say=lambda m: print(m, file=sys.stderr, flush=True))


def reader():
    global closed
    while True:
        try:
            d = s.recv(65536)
        except OSError:
            d = b""
        with lock:
            if not d:
                closed = True
                lock.notify_all()
                return
            buf.extend(d)
            verdict.feed(buf)
            lock.notify_all()


threading.Thread(target=reader, daemon=True).start()


# {prompt} in a wait or seen: the shell's prompt as the serial port has it,
# "jam" and the directory each in their colour (ESC [ ... m) and plain ":"
# and ">": jam, colours, ':', the directory (and colours), '>'.
PROMPT_RE = rb"jam(?:\x1b\[[0-9;]*m)*:[^\r\n>]*>"
FIELD = re.compile(r"\{([a-z][a-z0-9_]*)\}")
numbers = {}   # the {name}s captured so far


def needle_of(text):
    """What a wait or seen looks for: the text's bytes, or a regular
    expression when it has a {field} in it: {prompt}, a number captured
    before (its value), or one to capture (a named group)."""
    if text.startswith("re:"):
        return re.compile(text[3:].encode())
    if not FIELD.search(text):
        return text.encode()
    out, at, named = b"", 0, set()
    for m in FIELD.finditer(text):
        out += re.escape(text[at:m.start()].encode())
        name = m.group(1)
        if name == "prompt":
            out += PROMPT_RE
        elif name in numbers:
            out += re.escape(str(numbers[name]).encode())
        elif name in named:
            out += b"(?P=" + name.encode() + b")"
        else:
            out += b"(?P<" + name.encode() + b">-?[0-9]+)"
            if m.end() == len(text):
                # A number last in the text: the serial port may deliver it in two reads
                # ("127" as "12", then "7"), and the match must not end before the line
                # does: wait for the character after it.
                out += rb"(?=[^0-9])"
            named.add(name)
        at = m.end()
    return re.compile(out + re.escape(text[at:].encode()))


def find(needle, start):
    """(start, end) of needle's first match in buf from start, or None;
    the numbers it captures are kept."""
    if isinstance(needle, bytes):
        i = buf.find(needle, start)
        return (i, i + len(needle)) if i >= 0 else None
    m = needle.search(buf, start)
    if not m:
        return None
    numbers.update((k, int(v)) for k, v in m.groupdict().items())
    return m.span()


def number(expr):
    """A pointer coordinate: numbers and captured {name}s added and taken
    away, e.g. 640-{lx}+{nx}."""
    terms = re.findall(r"([+-]?)(\{[a-z][a-z0-9_]*\}|[0-9]+)", expr)
    if not terms or "".join(s + t for s, t in terms) != expr.replace(" ", ""):
        sys.exit(f"serial-feed: pointer: can't read '{expr}'")
    total = 0
    for sign, term in terms:
        if term.startswith("{"):
            if term[1:-1] not in numbers:
                sys.exit(f"serial-feed: pointer: {term} was never captured")
            v = numbers[term[1:-1]]
        else:
            v = int(term)
        total += -v if sign == "-" else v
    return total


def unescape(t):
    return t.encode().decode("unicode_escape").replace("\\e", "\x1b").encode("latin-1")


mon_sock = None


def monitor(command):
    """One QEMU monitor command, on a connection kept for the whole script
    (the monitor serves one client at a time; closed before we drain)."""
    global mon_sock
    import os
    if mon_sock is None:
        path = os.environ.get("QEMU_MON")
        if not path:
            sys.exit("serial-feed: monitor/usbkeys need $QEMU_MON")
        mon_sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        mon_sock.connect(path)
        mon_sock.settimeout(0.05)
    if command.startswith("device_add "):
        device_add(command)
        return
    mon_sock.sendall(command.encode() + b"\n")
    reply = b""
    try:   # the monitor's echo and prompt: not piled up; an error is said
        while True:
            got = mon_sock.recv(4096)
            if not got:
                break
            reply += got
    except OSError:
        pass
    for line in reply.decode(errors="replace").splitlines():
        if "Error" in line or "error" in line:
            print(f"serial-feed: monitor {command}: {line.strip()}", file=sys.stderr)


def monitor_answer(command, timeout):
    """Send a monitor command and read its whole answer, up to the prompt
    after it (or the timeout): the answer's lines, the echo left out."""
    try:   # what an earlier command's 50 ms read left behind
        while mon_sock.recv(4096):
            pass
    except OSError:
        pass
    mon_sock.sendall(command.encode() + b"\n")
    reply, end = b"", time.time() + timeout
    while time.time() < end:
        try:
            got = mon_sock.recv(4096)
        except OSError:
            got = b"."   # no data yet
        if not got:
            break
        if got != b".":
            reply += got
        nl = reply.find(b"\n")
        if nl >= 0 and b"(qemu) " in reply[nl:]:
            break
    text = reply.decode(errors="replace").replace("\r", "")
    return [l.strip() for l in text.split("\n")[1:] if l.strip() and "(qemu)" not in l]


def device_add(command):
    """A device_add, again until QEMU takes it (up to 10 s). QEMU frees a
    device_del'd device's drive only once its RCU callbacks run, some 50 to
    100 ms later (more on a loaded host): a device_add of the same drive
    before then is refused ("Permission conflict on node ..."), and the
    error comes after the RCU drain that a failed device_add does, past
    the 50 ms read above, so it went unseen and the guest never got its
    device (tools/screen-test.sh's "stick is back" waited for nothing;
    tools/replug-test.sh counts it). The drain makes the next try work."""
    t0, tries = time.time(), 0
    while True:
        tries += 1
        errors = [l for l in monitor_answer(command, 30) if "rror" in l]
        if not errors:
            if tries > 1:
                print(f"serial-feed: monitor {command}: QEMU refused it {tries - 1} time(s) "
                      f"(the last device on that drive not yet freed), took it after "
                      f"{int((time.time() - t0) * 1000)} ms", file=sys.stderr)
            return
        busy = any(w in errors[0] for w in ("Permission conflict", "Duplicate", "in use"))
        if not busy or time.time() - t0 > 10:
            print(f"serial-feed: monitor {command}: {errors[0]}"
                  + (" (refused for 10 s)" if busy else ""), file=sys.stderr)
            return
        time.sleep(0.05)


KEYNAMES = {" ": "spc", "\r": "ret", "\n": "ret", "-": "minus", ".": "dot", "/": "slash",
            ":": "shift-semicolon"}


def usbkeys(text):
    for ch in text.replace("\\r", "\r").replace("\\n", "\n"):
        name = KEYNAMES.get(ch, ch if ch.isalnum() and ch.isascii() else None)
        if name is None:
            sys.exit(f"serial-feed: usbkeys: no key for {ch!r}")
        monitor(f"sendkey {name.lower() if len(name) == 1 else name}")
        time.sleep(0.2)   # sendkey holds each key 100 ms


def pointer(x, y):
    """Each step is a report of its own: the monitor's round trip (its 50 ms
    reply wait) lets the guest poll the mouse in between, so QEMU never
    merges two steps into one bigger, accelerated move."""
    for _ in range(4):
        monitor("mouse_move -2000 -2000")
    time.sleep(0.5)
    while x > 0 or y > 0:
        dx, dy = min(x, 6), min(y, 6)
        monitor(f"mouse_move {dx} {dy}")
        x, y = x - dx, y - dy


def known(text):
    """text with the {name}s captured so far put in; any other left as it is."""
    return FIELD.sub(lambda m: str(numbers.get(m.group(1), m.group(0))), text)


def send(data):
    for b in data:
        s.sendall(bytes([b]))
        time.sleep(0.002)   # a person types slower than 115200 baud anyway


mark = 0
ok = True


def guest_failed():
    """The guest has printed a failure line, or a utest began over a utest
    that never finished."""
    return bool(verdict.failed or verdict.crashed)


for lineno, raw in enumerate(open(script), 1):
    line = raw.rstrip("\n")
    if not line.strip() or line.lstrip().startswith("#"):
        continue
    if guest_failed():
        print(f"serial-feed: {script}:{lineno}: stopped, the guest reported a failed test",
              file=sys.stderr)
        ok = False
        break
    cmd, _, arg = line.partition(" ")
    if cmd in ("wait", "seen"):
        timeout = 60.0
        first, _, rest = arg.partition(" ")
        try:
            timeout = float(first)
            arg = rest
        except ValueError:
            pass
        needle = needle_of(arg)
        end = time.time() + timeout
        with lock:
            while True:
                if guest_failed():
                    print(f"serial-feed: {script}:{lineno}: no '{arg}' before the guest "
                          "reported a failed test", file=sys.stderr)
                    ok = False
                    break
                hit = find(needle, mark if cmd == "wait" else 0)
                if hit:
                    if cmd == "wait":
                        mark = hit[1]
                    break
                left = end - time.time()
                if left <= 0 or closed:
                    print(f"serial-feed: {script}:{lineno}: no '{arg}' within {timeout:g} s"
                          + (" (QEMU ended)" if closed else ""), file=sys.stderr)
                    ok = False
                    break
                lock.wait(min(left, 0.5))
        if not ok:
            break
        print(f"serial-feed: saw '{arg}'", file=sys.stderr)
    elif cmd == "send":
        send(known(arg).encode() + b"\r")
    elif cmd == "type":
        send(unescape(arg.replace("\\e", "\x1b")))
    elif cmd == "sleep":
        time.sleep(float(arg))
    elif cmd in ("shot", "shots"):
        import os
        mon, d = os.environ.get("QEMU_MON"), os.environ.get("SHOT_DIR")
        if cmd == "shot":
            names, gap = [arg], 0.0
        else:
            base, n, gap = arg.split()
            names, gap = [f"{base}-{i + 1:02d}" for i in range(int(n))], float(gap)
        if mon and d:
            try:
                for i, name in enumerate(names):
                    if i:
                        time.sleep(gap)
                    monitor(f"screendump {os.path.join(d, name + '.ppm')}")
                time.sleep(1.0)
                from PIL import Image
                for name in names:
                    ppm = os.path.join(d, name + ".ppm")
                    Image.open(ppm).save(os.path.join(d, name + ".png"))
                    os.remove(ppm)
            except Exception as e:
                print(f"serial-feed: {cmd} {arg}: {e}", file=sys.stderr)
    elif cmd == "monitor":
        monitor(arg)
    elif cmd == "usbkeys":
        usbkeys(arg)
    elif cmd == "pointer":
        px, py = (number(v) for v in arg.split())
        pointer(px, py)
    else:
        sys.exit(f"serial-feed: {script}:{lineno}: unknown command '{cmd}'")

if guest_failed():
    try:   # nothing more to learn from this boot: don't wait for the run's timeout
        monitor("quit")
    except (SystemExit, OSError):
        pass
if mon_sock is not None:
    mon_sock.close()   # qemu-test.sh's screendump needs the monitor
with lock:
    while not closed:
        lock.wait(1)
problems = verdict.problems(True)
for why in problems:
    print("serial-feed: " + why, file=sys.stderr)
if problems:
    print(f"serial-feed: {script}: FAILED, the guest reported {len(problems)} failure(s)",
          file=sys.stderr)
sys.exit(0 if ok and not problems else 1)
