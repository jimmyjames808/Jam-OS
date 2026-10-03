#!/usr/bin/env python3
"""The Mac's side of tools/serve-test.sh: once the guest's log says it
serves, fetch from Jam OS's `serve` through the network peer's
--tcp-forward ports, as programs on the Mac would, and check every answer.

    servecheck.py --log SERIAL_LOG --port P (the guest's 8080) --port2 P2 (its 80)
                  --file FILE --page PAGE

With one connection left open and silent the whole time (a slow client
must hold up no other): curl twice at once (two paths: the same file),
the SHA-256 of each must be FILE's; a HEAD (curl -I); a range (curl -r);
the second file on the second port and its Content-Type; a request that
isn't HTTP (400) and a head over the server's 8 KiB (431). Then, once the
log says everything was stopped, a fetch that must fail. One line per
check, "servecheck: PASS" or "servecheck: FAIL" last."""
import argparse
import hashlib
import socket
import subprocess
import sys
import threading
import time

problems = []


def say(msg):
    print("servecheck: " + msg, flush=True)


def check(cond, what):
    say(("ok: " if cond else "FAILED: ") + what)
    if not cond:
        problems.append(what)


def wait_log(path, text, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            with open(path, "rb") as f:
                if text.encode() in f.read():
                    return True
        except OSError:
            pass
        time.sleep(0.2)
    say("FAILED: no '%s' in the guest's log in %d s" % (text, timeout))
    problems.append("log: " + text)
    return False


def curl(*args):
    p = subprocess.run(["curl", "-s", "--max-time", "120"] + list(args),
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return p.returncode, p.stdout


def sha(path):
    try:
        with open(path, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()
    except OSError:
        return None


def raw(port, data, timeout=15):
    """Send data on a new connection; what came back before the server closed."""
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.sendall(data)
    got = b""
    try:
        while True:
            b = s.recv(65536)
            if not b:
                break
            got += b
    except OSError:
        pass
    s.close()
    return got


def downloads(a, want):
    """Two curls at once, each a path of its own: the same file."""
    outs = [a.out + "/c1.bin", a.out + "/c2.bin"]
    urls = ["http://127.0.0.1:%d/" % a.port, "http://127.0.0.1:%d/any/thing?x=1" % a.port]
    codes = [None, None]
    t0 = time.monotonic()

    def get(i):
        codes[i], _ = curl("-o", outs[i], urls[i])

    ts = [threading.Thread(target=get, args=(i,)) for i in range(2)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    secs = time.monotonic() - t0
    for i in range(2):
        check(codes[i] == 0 and sha(outs[i]) == want,
              "curl %s: exit %s, sha256 %s" % (urls[i], codes[i], sha(outs[i])))
    say("two downloads at once: %.1f MB in %.2f s (%.1f MB/s in all; QEMU's)" % (
        2 * len(open(a.file, "rb").read()) / 1e6, secs,
        2 * len(open(a.file, "rb").read()) / 1e6 / max(secs, 1e-6)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--log", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--port2", type=int, required=True)
    ap.add_argument("--file", required=True)
    ap.add_argument("--page", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    data = open(a.file, "rb").read()
    want = hashlib.sha256(data).hexdigest()
    if not wait_log(a.log, "serve: serving /data/page.html", 300):
        return 1
    idle = socket.create_connection(("127.0.0.1", a.port), timeout=10)   # silent all along
    downloads(a, want)
    code, head = curl("-I", "http://127.0.0.1:%d/" % a.port)
    check(code == 0 and b"200 OK" in head and b"Content-Length: %d" % len(data) in head,
          "HEAD: 200 and the length (%r)" % head[:60])
    code, part = curl("-r", "100-199", "http://127.0.0.1:%d/" % a.port)
    check(code == 0 and part == data[100:200], "a range (100-199): %d bytes, right" % len(part))
    code, head = curl("-sD", "-", "-o", a.out + "/page.got", "http://127.0.0.1:%d/" % a.port2)
    check(code == 0 and b"text/html" in head and sha(a.out + "/page.got") == sha(a.page),
          "the second file on the second port, text/html")
    got = raw(a.port, b"HELLO THERE\r\n\r\n")
    check(got.startswith(b"HTTP/1.1 400 "), "not HTTP: 400 (%r)" % got[:40])
    got = raw(a.port, b"GET / HTTP/1.1\r\nHost: h\r\nX-Long: " + b"a" * 9000 + b"\r\n\r\n")
    check(got.startswith(b"HTTP/1.1 431 "), "a head over 8 KiB: 431 (%r)" % got[:40])
    idle.close()
    if wait_log(a.log, "serve: nothing was served", 300):
        code, body = curl("-o", a.out + "/after.bin", "http://127.0.0.1:%d/" % a.port)
        check(code != 0 and sha(a.out + "/after.bin") != want,
              "after serve stop: nothing served (curl exit %d)" % code)
    say("PASS" if not problems else "FAIL")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
