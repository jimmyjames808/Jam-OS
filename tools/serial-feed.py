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
                              the next one.
    send <text>               <text> and Enter (CR), one byte at a time
    type <text>               <text> without Enter; \\e \\r \\n \\t \\xNN escapes
    sleep <seconds>
    shot <name>               a screenshot: <name>.png next to the log
                              (through QEMU's monitor, $QEMU_MON / $SHOT_DIR)
    shots <name> <n> <s>      n screenshots <s> seconds apart (made PNGs
                              after the last): <name>-01.png, -02 ...
    monitor <command>         a QEMU monitor command ($QEMU_MON), e.g.
                              `monitor device_del kbd1`
    usbkeys <text>            <text> typed on QEMU's keyboards (monitor
                              `sendkey`, one key at a time: the USB keyboard
                              path); \r (or \n) is Enter; a-z 0-9 space
                              - . / and : only
    pointer <x> <y>           QEMU's mouse to pixel (x, y): pushed into the
                              top-left corner (where it is clamped), then
                              moved in steps of at most 6 counts, which the
                              compositor's (and libfun's) acceleration
                              takes 1:1, each its own monitor command
    # comment, blank lines ignored

After the script it keeps reading until QEMU closes the socket (a `reboot`
with -no-reboot ends QEMU), so the guest never blocks on a full socket.
Exit status 0 if every wait matched; on a timeout it says which and exits 1
(and stops typing, but still drains until QEMU goes)."""
import re
import socket
import sys
import threading
import time

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
            lock.notify_all()


threading.Thread(target=reader, daemon=True).start()


# {prompt} in a wait or seen: the shell's prompt as the serial port has it,
# "jam" and the directory each in their colour (ESC [ ... m) and plain ":"
# and ">": jam, colours, ':', the directory (and colours), '>'.
PROMPT_RE = rb"jam(?:\x1b\[[0-9;]*m)*:[^\r\n>]*>"


def needle_of(text):
    """What a wait or seen looks for: the text's bytes, or a regular
    expression when it has {prompt} in it."""
    if "{prompt}" not in text:
        return text.encode()
    return re.compile(PROMPT_RE.join(re.escape(p.encode()) for p in text.split("{prompt}")))


def find(needle, start):
    """(start, end) of needle's first match in buf from start, or None."""
    if isinstance(needle, bytes):
        i = buf.find(needle, start)
        return (i, i + len(needle)) if i >= 0 else None
    m = needle.search(buf, start)
    return m.span() if m else None


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


def send(data):
    for b in data:
        s.sendall(bytes([b]))
        time.sleep(0.002)   # a person types slower than 115200 baud anyway


mark = 0
ok = True
for lineno, raw in enumerate(open(script), 1):
    line = raw.rstrip("\n")
    if not line.strip() or line.lstrip().startswith("#"):
        continue
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
        send(arg.encode() + b"\r")
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
        px, py = (int(v) for v in arg.split())
        pointer(px, py)
    else:
        sys.exit(f"serial-feed: {script}:{lineno}: unknown command '{cmd}'")

if mon_sock is not None:
    mon_sock.close()   # qemu-test.sh's screendump needs the monitor
with lock:
    while not closed:
        lock.wait(1)
sys.exit(0 if ok else 1)
