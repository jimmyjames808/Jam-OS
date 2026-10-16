#!/usr/bin/env python3
"""ports-server: the Mac's package repository over HTTP, for `pkg` on the
PC (docs/M13.7-PLAN.md section 8): build/repo/ as `make ports` made it
(tools/portslib/repo.py), served read-only.

    ports-server.py [--repo DIR] [--port 8080] [--bind ADDR] [--client ADDR]
    ports-server.py --self-test

GET and HEAD of a file under the repository: /INDEX, /VULNS,
/pkg/<file>.jpk, /src/<file>.tar. Nothing else: no listing, no query, no
'%' escapes, no part starting with '.', nothing a symlink leads out to,
only regular files; every other request is answered 400, 404 or 405 and
the connection closed. Each answer is one snapshot of the file: read
whole into memory and checked unchanged (size, change time, inode) from
open to the last byte, read again if it changed meanwhile (a `make
ports` writing in place), so a client never gets half of one version
and half of another; a repository written by repo.py replaces files by
rename, so an open file never changes at all. Every answer has its
Content-Length and Connection: close (the PC's HTTP client, user/lib/
http.c, takes either framing).

Nothing here is trusted by the PC: the index, VULNS and every package
are signed with the ports key, and pkg checks the signatures, not the
transport (as apt and FreeBSD's pkg over plain HTTP). --client answers
only that address (the PC's), as tools/update-server.py does.
"""
import argparse
import http.client
import http.server
import os
import re
import stat
import sys
import tempfile
import threading

PORT = 8080
REPO = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "build", "repo")
PART = re.compile(r"[A-Za-z0-9_+~-][A-Za-z0-9._+~-]{0,254}\Z")
DEPTH = 2           # /INDEX, /pkg/<file>
TRIES = 5           # reads of a file that keeps changing before 503
CHUNK = 1 << 16
FILE_MAX = 4 << 30  # the largest file served (tools/portslib/pkgformat.py ARCHIVE_MAX)


class Busy(Exception):
    pass


def resolve(root, target):
    """The file a request target names under root, or None: the HTTP
    status to answer instead (400 or 404) as an int."""
    if not target.startswith("/") or "?" in target or "%" in target or "#" in target:
        return 400
    parts = target[1:].split("/")
    if not 0 < len(parts) <= DEPTH or not all(PART.match(p) for p in parts):
        return 404
    path = os.path.join(root, *parts)
    real, top = os.path.realpath(path), os.path.realpath(root)
    if not real.startswith(top + os.sep) or real != os.path.join(top, *parts):
        return 404   # a symlink anywhere on the way
    return path


def snapshot(path, hook=None):
    """path's bytes, unchanged from open to the end of the read; None if
    it isn't a regular file; Busy if it kept changing."""
    for _ in range(TRIES):
        try:
            fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
        except OSError:
            return None
        try:
            before = os.fstat(fd)
            if not stat.S_ISREG(before.st_mode) or before.st_size > FILE_MAX:
                return None
            chunks, got = [], 0
            while got <= before.st_size:   # one byte past the size: it grew
                b = os.read(fd, min(CHUNK, before.st_size + 1 - got))
                if not b:
                    break
                chunks.append(b)
                got += len(b)
                if hook:
                    hook(path)
            after = os.fstat(fd)
        finally:
            os.close(fd)
        data = b"".join(chunks)
        same = (before.st_size, before.st_mtime_ns, before.st_ctime_ns, before.st_ino) == \
            (after.st_size, after.st_mtime_ns, after.st_ctime_ns, after.st_ino)
        if same and len(data) == before.st_size:
            return data
    raise Busy(path)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "jamos-ports"
    sys_version = ""

    def answer(self, code, body=b"", head_only=False):
        self.send_response(code)
        self.send_header("Content-Type", "application/octet-stream" if code == 200 else
                         "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        if not head_only:
            self.wfile.write(body)

    def serve(self, head_only):
        srv = self.server
        if srv.client and self.client_address[0] != srv.client:
            self.close_connection = True
            return
        path = resolve(srv.root, self.path)
        if isinstance(path, int):
            self.answer(path, b"no\n", head_only)
            return
        try:
            data = snapshot(path, srv.hook)
        except Busy:
            self.answer(503, b"the file keeps changing: try again\n", head_only)
            return
        if data is None:
            self.answer(404, b"no such file\n", head_only)
            return
        self.answer(200, data, head_only)

    def do_GET(self):   # noqa: N802 (http.server's names)
        self.serve(False)

    def do_HEAD(self):   # noqa: N802
        self.serve(True)

    def refuse(self):
        self.answer(405, b"GET or HEAD only\n")

    do_POST = do_PUT = do_DELETE = do_PATCH = do_OPTIONS = refuse   # noqa: N815

    def log_message(self, fmt, *args):
        if not self.server.quiet:
            sys.stderr.write("ports-server: %s %s\n" % (self.client_address[0], fmt % args))


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, addr, root, client=None, hook=None, quiet=False):
        super().__init__(addr, Handler)
        self.root, self.client, self.hook, self.quiet = root, client, hook, quiet


# ---- the self-test ----------------------------------------------------------------------

FAILS = []


def expect(cond, what):
    if not cond:
        FAILS.append(what)


def fetch(port, target, method="GET"):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
    c.putrequest(method, target, skip_accept_encoding=True)
    c.endheaders()
    r = c.getresponse()
    body = r.read()
    out = (r.status, r.getheader("Content-Length"), r.getheader("Connection"), body)
    c.close()
    return out


def self_test():
    with tempfile.TemporaryDirectory(prefix="ports-server.") as d:
        root = os.path.join(d, "repo")
        os.makedirs(os.path.join(root, "pkg"))
        big = bytes(range(256)) * (12 << 10)   # 3 MiB
        files = {"INDEX": b"jamos-ports 1\nserial 1\n", "pkg/a-1-r1-0123456789abcdef.jpk": big,
                 "pkg/.hidden": b"x", "VULNS": b"old version\n" * 1000}
        for rel, data in files.items():
            with open(os.path.join(root, rel), "wb") as f:
                f.write(data)
        with open(os.path.join(d, "secret"), "wb") as f:
            f.write(b"outside the repository\n")
        os.symlink(os.path.join(d, "secret"), os.path.join(root, "pkg", "link.jpk"))
        os.makedirs(os.path.join(root, "src"))
        changed = {"n": 0}

        def hook(path):
            """Rewrite VULNS in place once, mid-read: the answer must be one
            version whole; CHURN changes on every read: 503."""
            if path.endswith("VULNS") and changed["n"] == 0:
                changed["n"] = 1
                with open(path, "r+b") as f:
                    f.write(b"NEW" * 5000)
            elif path.endswith("CHURN"):
                with open(path, "ab") as f:
                    f.write(b"+")
        srv = Server(("127.0.0.1", 0), root, hook=hook, quiet=True)
        port = srv.server_address[1]
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        try:
            run_cases(port, root, files, big)
        finally:
            srv.shutdown()
            srv.server_close()
    for f in FAILS:
        print("ports-server self-test: " + f)
    print("ports-server self-test: %s" % ("FAIL" if FAILS else "PASS"))
    return 1 if FAILS else 0


def run_cases(port, root, files, big):
    st, n, conn, body = fetch(port, "/INDEX")
    expect(st == 200 and body == files["INDEX"] and n == str(len(body)) and conn == "close",
           "GET /INDEX: %s %s %s" % (st, n, conn))
    st, n, _, body = fetch(port, "/pkg/a-1-r1-0123456789abcdef.jpk")
    expect(st == 200 and body == big, "GET of a 3 MiB package: %s, %d bytes" % (st, len(body)))
    st, n, _, body = fetch(port, "/pkg/a-1-r1-0123456789abcdef.jpk", "HEAD")
    expect(st == 200 and n == str(len(big)) and body == b"", "HEAD: %s %s" % (st, n))
    for target, want in [("/nothere", 404), ("/../secret", 404), ("/pkg/%2e%2e/secret", 400),
                         ("/pkg/.hidden", 404), ("/", 404), ("/pkg", 404), ("/pkg/", 404),
                         ("/INDEX?x=1", 400), ("/pkg/link.jpk", 404), ("/src", 404),
                         ("/a/b/c", 404), ("INDEX", 400)]:
        st, _, conn, body = fetch(port, target)
        expect(st == want and b"outside" not in body, "GET %s: %s, want %s" % (target, st, want))
    st, _, _, _ = fetch(port, "/INDEX", "POST")
    expect(st == 405, "POST: %s" % st)
    old, new = files["VULNS"], b"NEW" * 5000 + files["VULNS"][15000:]
    st, n, _, body = fetch(port, "/VULNS")
    expect(st == 200 and body == new and n == str(len(new)),
           "a file changed mid-read: %s, %d bytes, %s" % (
               st, len(body), "the old" if body == old else "the new" if body == new else "a mix"))
    path = os.path.join(root, "CHURN")
    with open(path, "wb") as f:
        f.write(b"x" * (3 * CHUNK))
    st, _, _, _ = fetch(port, "/CHURN")
    expect(st == 503, "a file that keeps changing: %s, want 503" % st)
    # Replaced by rename mid-read (as repo.py writes): the open file is
    # one snapshot, whichever the client gets.
    tmp = os.path.join(root, "INDEX.tmp")
    with open(tmp, "wb") as f:
        f.write(b"jamos-ports 1\nserial 2\n")
    os.replace(tmp, os.path.join(root, "INDEX"))
    st, _, _, body = fetch(port, "/INDEX")
    expect(st == 200 and body == b"jamos-ports 1\nserial 2\n", "the renamed INDEX: %s" % body)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--repo", default=REPO)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--bind", default="0.0.0.0")
    ap.add_argument("--client", help="answer only this address (the PC's)")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if not os.path.isfile(os.path.join(a.repo, "INDEX")):
        print("ports-server: no %s/INDEX: run `make ports` first" % a.repo)
        return 1
    srv = Server((a.bind, a.port), os.path.abspath(a.repo), a.client)
    print("ports-server: serving %s on %s:%d" % (a.repo, a.bind, a.port))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
