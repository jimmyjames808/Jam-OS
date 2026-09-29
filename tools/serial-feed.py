#!/usr/bin/env python3
"""Type into a QEMU guest's serial port (M7: the shell tests).

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
    send <text>               <text> and Enter (CR), one byte at a time
    type <text>               <text> without Enter; \\e \\r \\n \\t \\xNN escapes
    sleep <seconds>
    shot <name>               a screenshot: <name>.png next to the log
                              (through QEMU's monitor, $QEMU_MON / $SHOT_DIR)
    monitor <command>         a QEMU monitor command ($QEMU_MON), e.g.
                              `monitor device_del kbd1`
    usbkeys <text>            <text> typed on QEMU's keyboards (monitor
                              `sendkey`, one key at a time: the USB keyboard
                              path, M7); \r (or \n) is Enter; a-z 0-9 space
                              - . / and : only
    # comment, blank lines ignored

After the script it keeps reading until QEMU closes the socket (a `reboot`
with -no-reboot ends QEMU), so the guest never blocks on a full socket.
Exit status 0 if every wait matched; on a timeout it says which and exits 1
(and stops typing, but still drains until QEMU goes)."""
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
    try:   # the monitor's echo and prompt: not needed, just not piled up
        while mon_sock.recv(4096):
            pass
    except OSError:
        pass


KEYNAMES = {" ": "spc", "\r": "ret", "\n": "ret", "-": "minus", ".": "dot", "/": "slash",
            ":": "shift-semicolon"}


def usbkeys(text):
    for ch in text.replace("\\r", "\r").replace("\\n", "\n"):
        name = KEYNAMES.get(ch, ch if ch.isalnum() and ch.isascii() else None)
        if name is None:
            sys.exit(f"serial-feed: usbkeys: no key for {ch!r}")
        monitor(f"sendkey {name.lower() if len(name) == 1 else name}")
        time.sleep(0.2)   # sendkey holds each key 100 ms


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
        needle = arg.encode()
        end = time.time() + timeout
        with lock:
            while True:
                i = buf.find(needle, mark if cmd == "wait" else 0)
                if i >= 0:
                    if cmd == "wait":
                        mark = i + len(needle)
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
    elif cmd == "shot":
        import os
        mon, d = os.environ.get("QEMU_MON"), os.environ.get("SHOT_DIR")
        if mon and d:
            ppm = os.path.join(d, arg + ".ppm")
            try:
                monitor(f"screendump {ppm}")
                time.sleep(1.0)
                from PIL import Image
                Image.open(ppm).save(os.path.join(d, arg + ".png"))
                os.remove(ppm)
            except Exception as e:
                print(f"serial-feed: shot {arg}: {e}", file=sys.stderr)
    elif cmd == "monitor":
        monitor(arg)
    elif cmd == "usbkeys":
        usbkeys(arg)
    else:
        sys.exit(f"serial-feed: {script}:{lineno}: unknown command '{cmd}'")

if mon_sock is not None:
    mon_sock.close()   # qemu-test.sh's screendump needs the monitor
with lock:
    while not closed:
        lock.wait(1)
sys.exit(0 if ok else 1)
