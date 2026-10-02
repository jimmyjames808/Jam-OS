#!/usr/bin/env python3
"""The web server tools/fetch-test.sh fetches from (through the network
peer's --tcp-relay): well-behaved answers and hostile ones, for Jam OS's
`fetch`.

    httptest.py --port P [--size BYTES]     serve until killed
    httptest.py --sha NAME [--size BYTES]   print the SHA-256 of what NAME serves

Paths:
  /big.bin        --size bytes (a fixed pseudo-random pattern), Content-Length
  /chunked.bin    the same bytes, chunked: chunks of 1 to 30000 bytes, some
                  with extensions, and a trailer
  /close.bin      300000 bytes, no length: the body ends with the connection
  /small.txt      190 bytes of text
  /redir          301 -> /r/2 -> (302, relative) r3 -> (307, absolute) /small.txt
  /redir4         four redirects: one too many
  /to-https       301 to https://example.com/ (refused: no TLS)
  /giant          a header line of 20000 bytes (refused: a head over 16 KiB)
  /endless        header lines forever, 200 a second (refused at 16 KiB)
  /drip           the head one byte a second, never ending (a 20 s timeout)
  /cut            Content-Length 100000, 5000 bytes, then the connection closes
  /badchunk       a chunk size that isn't hex
  anything else   404"""
import argparse
import hashlib
import random
import socketserver
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SIZE = 6 * 1024 * 1024
HOST = "10.2.21.174:8000"   # the Mac as the guest sees it (fetch-test.sh's relay)


def body(name, size):
    if name in ("big.bin", "chunked.bin"):
        return random.Random(0x4A414D).randbytes(size)
    if name == "close.bin":
        return random.Random(0x434C53).randbytes(300000)
    if name == "small.txt":
        return b"hello from the Mac\n" * 10
    return None


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    size = SIZE
    cache = {}

    def log_message(self, fmt, *args):
        sys.stderr.write("httptest: %s %s\n" % (self.address_string(), fmt % args))

    def raw(self, data):
        self.wfile.write(data)
        self.wfile.flush()

    def data(self, name):
        if name not in self.cache:
            self.cache[name] = body(name, self.size)
        return self.cache[name]

    def plain(self, name):
        b = self.data(name)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.raw(b)

    def chunked(self):
        b, rng, at = self.data("chunked.bin"), random.Random(7), 0
        self.send_response(200)
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        while at < len(b):
            n = min(rng.randint(1, 30000), len(b) - at)
            ext = ";x=%d" % n if rng.random() < 0.2 else ""
            self.wfile.write(b"%x%s\r\n" % (n, ext.encode()) + b[at:at + n] + b"\r\n")
            at += n
        self.raw(b"0\r\nX-Trailer: done\r\n\r\n")

    def redirect(self, code, where):
        self.send_response(code)
        self.send_header("Location", where)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def close_bin(self):
        self.close_connection = True
        self.raw(b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + self.data("close.bin"))

    def hostile(self, path):
        self.close_connection = True
        if path == "/giant":
            self.raw(b"HTTP/1.1 200 OK\r\nX-Big: " + b"a" * 20000 + b"\r\n\r\n")
        elif path == "/endless":
            self.raw(b"HTTP/1.1 200 OK\r\n")
            for i in range(100000):   # until fetch hangs up (a write fails)
                self.raw(b"X-%d: y\r\n" % i)
                time.sleep(0.005)
        elif path == "/drip":
            for c in b"HTTP/1.1 200 OK\r\nX-Slow: " + b"z" * 1000:
                self.raw(bytes([c]))
                time.sleep(1.0)
        elif path == "/cut":
            self.raw(b"HTTP/1.1 200 OK\r\nContent-Length: 100000\r\n\r\n" + b"c" * 5000)
        elif path == "/badchunk":
            self.raw(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n")

    def do_GET(self):
        p = self.path
        try:
            if p in ("/big.bin", "/small.txt"):
                self.plain(p[1:])
            elif p == "/chunked.bin":
                self.chunked()
            elif p == "/close.bin":
                self.close_bin()
            elif p == "/redir":
                self.redirect(301, "/r/2")
            elif p == "/r/2":
                self.redirect(302, "r3")
            elif p == "/r/r3":
                self.redirect(307, "http://%s/small.txt" % HOST)
            elif p.startswith("/redir4"):
                n = int(p[7:] or "0")
                self.redirect(302, "/redir4%d" % (n + 1) if n < 4 else "/small.txt")
            elif p == "/to-https":
                self.redirect(301, "https://example.com/")
            elif p in ("/giant", "/endless", "/drip", "/cut", "/badchunk"):
                self.hostile(p)
            else:
                self.send_error(404)
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--size", type=int, default=SIZE)
    ap.add_argument("--sha", help="print the SHA-256 of what this path's name serves")
    a = ap.parse_args()
    if a.sha:
        print(hashlib.sha256(body(a.sha, a.size)).hexdigest())
        return 0
    Handler.size = a.size
    socketserver.TCPServer.allow_reuse_address = True
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    srv.daemon_threads = True
    print("httptest: serving on 127.0.0.1:%d" % srv.server_address[1], flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
