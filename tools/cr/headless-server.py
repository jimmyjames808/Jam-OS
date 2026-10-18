#!/usr/bin/env python3
"""The web server of cr-test.sh's `headless` case (docs/CR-PLAN.md, track
CB3): the files of a directory over HTTP, or over TLS with a certificate,
and one line in a log for each request it answered:

    headless-server.py <dir> <port> <log> [--tls <cert.pem> <key.pem>]

A log line is `<method> <path> host=<Host header> ua=<User-Agent>`, and over
TLS `tls=<version>` too, so the check (tools/cr/headless-check.py) can say
which name the browser asked for and that it really spoke TLS. A client
whose handshake fails (a browser that doesn't trust the certificate) gets a
`handshake failed: <why>` line. The handshake runs in the connection's own
thread, so a client that stalls holds up only itself.

Runs on the build host; the guest reaches it through netpeer's
`--tcp-relay <guest port>:<this port>` (the guest asks for a name that
netpeer's DNS answers, www.jam or mac.jam, at the relay's address)."""
import functools
import http.server
import socketserver
import ssl
import sys
import threading


class Log:
    def __init__(self, path):
        self.f = open(path, "a", buffering=1)
        self.lock = threading.Lock()

    def line(self, text):
        with self.lock:
            self.f.write(text + "\n")


class Handler(http.server.SimpleHTTPRequestHandler):
    log = None
    tls = False

    def setup(self):
        if self.tls:
            try:
                self.request.do_handshake()
            except (ssl.SSLError, OSError) as e:
                self.log.line("handshake failed: %s" % (getattr(e, "reason", None) or e))
                raise
        super().setup()

    def log_request(self, code="-", size="-"):
        words = "%s %s host=%s ua=%s" % (
            self.command, self.path, self.headers.get("Host", "-"),
            (self.headers.get("User-Agent") or "-").replace(" ", "_"))
        if self.tls:
            words += " tls=%s" % self.request.version()
        self.log.line(words + " status=%s" % getattr(code, "value", code))

    def log_message(self, *args):
        pass

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    context = None

    def get_request(self):
        sock, addr = self.socket.accept()
        if self.context:
            sock = self.context.wrap_socket(sock, server_side=True, do_handshake_on_connect=False)
        return sock, addr

    def handle_error(self, request, client_address):
        pass     # a failed handshake or a closed connection: logged by the handler


def main(argv):
    args = argv[1:]
    tls = None
    if "--tls" in args:
        i = args.index("--tls")
        tls = args[i + 1:i + 3]
        del args[i:i + 3]
        if len(tls) != 2:
            args = []
    if len(args) != 3:
        print(__doc__.split("\n\n")[1].strip(), file=sys.stderr)
        return 2
    directory, port, path = args[0], int(args[1]), args[2]
    Handler.log = Log(path)
    Handler.tls = tls is not None
    server = Server(("127.0.0.1", port), functools.partial(Handler, directory=directory))
    if tls:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(tls[0], tls[1])
        server.context = ctx
    print("serving %s on %d%s" % (directory, port, " (TLS)" if tls else ""), flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
