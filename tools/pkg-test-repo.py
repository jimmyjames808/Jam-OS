#!/usr/bin/env python3
"""The repositories tools/pkg-test.sh serves to `pkg` in QEMU: small signed
packages made with the ports builder's own writers (tools/portslib), signed
with the ports vectors' public test key (the one tools/pkg-test.sh puts in
the test image as /boot/ports.pub; its secret is public and no real image
carries it).

    pkg-test-repo.py make DIR            write the repositories under DIR/web/
    pkg-test-repo.py serve DIR [--port N]  make, then serve DIR/web/ on N (0: any)
                                          and print "serving on <port>"

Each directory under web/ is a repository (INDEX, VULNS, pkg/), reached as
http://<host>:<port>/<name>/:

    good      serial 1001: aaa 1.0 (a program, data, a licence) and bbb 1.0,
              which runs aaa
    next      serial 1002: aaa 2.0 (its program and readme changed, old.txt
              gone, new.txt added), bbb 1.0 as it was, ccc 1.0; VULNS names aaa
              before 2.0
    old       serial 900: good's packages (an index replayed from the past)
    badpkg    serial 901: aaa's package has a byte changed (the index names the
              hash of the bytes before)
    otherkey  serial 902: aaa's manifest signed with another key
    missing   serial 903: aaa's archive lacks a file its manifest lists
    refuse    serial 904: aaa's program asks for `right debug`, which no
              package may
    badidx    serial 905: the index changed by a byte after it was signed
    rebuild   serial 1004: aaa 2.0 again with another readme (same version and
              revision, another hash: newer by its hash)
    slow      serial 1005: next's packages, but a package's body stalls halfway
              for SLOW_S seconds (a power cut in the middle of a download)

The serials rise in the order a test uses them (an index below the last
accepted is refused, so the defects come first with the lowest, then good,
next, rebuild, slow).
"""
import argparse
import hashlib
import http.server
import os
import socketserver
import sys
import time

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(TOP, "tools"))
sys.dont_write_bytecode = True

from portslib import package as packagemod  # noqa: E402
from portslib import pkgformat as fmt  # noqa: E402
from portslib import sign as signmod  # noqa: E402
from portslib import ustar  # noqa: E402
from portslib import vectors  # noqa: E402

ABI = vectors.ABI
MADE = "2026-10-12T00:00:00Z"
LIST = b"posix\nmount /data r\n"


def package(name, version, runs=(), readme=None, extra=None, list_=LIST, rev=1):
    """A package: a program, a readme, a licence, and the extra files."""
    r = vectors.FakeRecipe(name, version, rev)
    r.values["summary"] = "test package %s" % name
    p = packagemod.Package(name, r)
    p.runs = [(d, v) for d, v in runs]
    p.built_with = [["musl", "1.2.6"]]
    length = 4096 + (sum(version.encode()) % 40) * 8
    p.add("bin/" + name, 0o755, vectors.tiny_elf(list_, length))
    p.lists["bin/" + name] = list_
    p.add("share/%s/readme.txt" % name, 0o644, (readme or "%s %s\n" % (name, version)).encode())
    p.add("share/licences/%s/LICENSE" % name, 0o644, b"MIT, for a test package\n")
    for path, data in (extra or {}).items():
        p.add(path, 0o644, data)
    return p


def archive_of(p, key):
    text = packagemod.manifest(p, ABI)
    return packagemod.archive(p, signmod.sign(text, fmt.PKG_FIRST, key))


def index_text(entries, serial, key, vulns=None):
    """INDEX and VULNS for entries: [(Package, jpk bytes)]."""
    lines = ["serial %d" % serial, "made " + MADE, "abi " + ABI]
    ents = sorted(entries, key=lambda e: e[0].name.encode())
    for p, data in ents:
        lines.append("package %s %s %d %d %s %s" % (p.name, p.version, p.revision, len(data),
                                                    hashlib.sha256(data).hexdigest(), p.name))
        lines += ["runs %s %s%s" % (p.name, d, " " + v if v else "") for d, v in p.runs]
    for p, _ in ents:
        lines.append("src %s %s %d 100 %s" % (p.name, p.version, p.revision, "11" * 32))
    out = lambda ls, first: signmod.sign(
        ("\n".join([first] + ls + ["signature"]) + "\n").encode("ascii"), first, key)
    index = out(lines, fmt.INDEX_FIRST)
    v = ["serial %d" % serial, "made " + MADE] + (vulns or [])
    return index, out(v, fmt.VULNS_FIRST)


def write_repo(web, name, entries, serial, key, vulns=None, index_edit=None, files=None):
    index, vtext = index_text(entries, serial, key, vulns)
    if index_edit:
        index = index_edit(index)
    d = os.path.join(web, name)
    os.makedirs(os.path.join(d, "pkg"), exist_ok=True)
    for p, data in entries:
        rel = fmt.pkg_file(p.name, p.version, p.revision, hashlib.sha256(data).hexdigest())
        served = (files or {}).get(p.name, data)
        with open(os.path.join(d, rel), "wb") as f:
            f.write(served)
    for fname, data in (("INDEX", index), ("VULNS", vtext)):
        with open(os.path.join(d, fname), "wb") as f:
            f.write(data)


def entry(p, key):
    return p, archive_of(p, key)


def make(out):
    web = os.path.join(out, "web")
    key, _pub = signmod.test_key(os.path.join(out, "keys"), "ports vectors")
    key2, _ = signmod.test_key(os.path.join(out, "keys2"), "ports vectors, another")
    aaa1 = package("aaa", "1.0", extra={"share/aaa/old.txt": b"old\n"})
    bbb1 = package("bbb", "1.0", runs=[("aaa", "1.0")])
    aaa2 = package("aaa", "2.0", readme="aaa 2.0, again\n", extra={"share/aaa/new.txt": b"new\n"})
    ccc1 = package("ccc", "1.0")
    good = [entry(aaa1, key), entry(bbb1, key)]
    write_repo(web, "good", good, 1001, key)
    write_repo(web, "next", [entry(aaa2, key), good[1], entry(ccc1, key)], 1002, key,
               vulns=["vuln aaa - 2.0 - CVE-2026-1234 aaa before 2.0 has a test hole"])
    write_repo(web, "old", good, 900, key)
    write_repo(web, "badidx", good, 905, key,
               index_edit=lambda b: b.replace(b"package aaa 1.0 1 ", b"package aaa 1.0 2 ", 1))
    a_ok = entry(aaa1, key)
    bad = bytearray(a_ok[1])
    bad[-1500] ^= 0x40
    write_repo(web, "badpkg", [a_ok], 901, key, files={"aaa": bytes(bad)})
    write_repo(web, "otherkey", [entry(aaa1, key2)], 902, key)
    m = packagemod.manifest(aaa1, ABI)
    paths = sorted(aaa1.files, key=lambda s: s.encode())
    members = [("+MANIFEST", 0o644, signmod.sign(m, fmt.PKG_FIRST, key))]
    members += [(x, aaa1.files[x][0], aaa1.files[x][1]) for x in paths
                if x != "share/aaa/readme.txt"]
    members += [("+TABLES/bin/aaa", 0o644, packagemod.pagetable.make(aaa1.files["bin/aaa"][1]))]
    write_repo(web, "missing", [(aaa1, ustar.archive(members))], 903, key)
    write_repo(web, "refuse", [entry(package("aaa", "1.0", list_=b"posix\nright debug\n"), key)],
               904, key)
    aaa2b = package("aaa", "2.0", readme="aaa 2.0, rebuilt\n", extra={"share/aaa/new.txt": b"new\n"})
    write_repo(web, "rebuild", [entry(aaa2b, key), good[1], entry(ccc1, key)], 1004, key)
    write_repo(web, "slow", [entry(aaa2, key), good[1], entry(ccc1, key)], 1005, key)
    return web


SLOW_S = 25


def handler_for(web):
    root = os.path.realpath(web)

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def log_message(self, fmt_, *args):
            sys.stderr.write("pkg-test-repo: %s\n" % (fmt_ % args))

        def do_GET(self):
            path = self.path.split("?")[0]
            full = os.path.realpath(os.path.join(root, path.lstrip("/")))
            if not full.startswith(root + os.sep) or not os.path.isfile(full):
                self.send_error(404)
                return
            with open(full, "rb") as f:
                data = f.read()
            self.send_response(200)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            try:
                if path.startswith("/slow/pkg/"):
                    half = len(data) // 2
                    self.wfile.write(data[:half])
                    self.wfile.flush()
                    time.sleep(SLOW_S)
                    self.wfile.write(data[half:])
                else:
                    self.wfile.write(data)
            except (BrokenPipeError, ConnectionResetError):
                pass   # the client has what it wanted (a head), or is gone

    return Handler


def serve(web, port):
    with socketserver.ThreadingTCPServer(("127.0.0.1", port), handler_for(web)) as srv:
        srv.daemon_threads = True
        print("serving on %d" % srv.server_address[1], flush=True)
        srv.serve_forever()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("cmd", choices=("make", "serve"))
    ap.add_argument("dir")
    ap.add_argument("--port", type=int, default=0)
    a = ap.parse_args()
    web = make(a.dir)
    if a.cmd == "serve":
        serve(web, a.port)
    else:
        print("pkg-test-repo: repositories in %s" % web)


if __name__ == "__main__":
    main()
