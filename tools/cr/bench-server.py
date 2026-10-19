#!/usr/bin/env python3
"""The web server of cr-test.sh's `bench` case (docs/CR-PLAN.md, CR-e):
JetStream 2.2 and Speedometer 3.1 as they come (tools/cr/toolchain.sh
fetches them, pinned), served unchanged but for one line added to each one's
index.html as it goes out, which loads /crbench.js: a script of ours that
says the benchmark's results as console lines ("crbench: ...", which the
browser logs) once its page shows them.

    bench-server.py <jetstream dir> <speedometer dir> <port> <log>

Paths: /jetstream/... and /speedometer/... are the two directories,
/crbench.js the reporter, /run/<name> a redirect to one of RUNS (below:
the parts of each benchmark the case runs, which make URLs too long for a
shell line). JetStream, started with ?report=true, also POSTs its results
(JSON) to /report: kept beside the log, <log>.report-<n>.json. One line in
the log for each request. Runs on the build host; the guest reaches it
through netpeer's `--tcp-relay <guest port>:<this port>`."""
import http.server
import os
import socketserver
import sys
import threading
import urllib.parse

# The reporter: polls the page for the benchmark's result and says it, the
# score first ("crbench: <bench> score <n>"), then each part's figure,
# then "crbench: done". An error on the page (a part that throws stops
# JetStream) is said too, and ends it the same way.
REPORTER = r"""(function () {
    var t0 = Date.now(), said = false;
    function say(s) { console.log("crbench: " + s); }
    function secs() { return ((Date.now() - t0) / 1000).toFixed(0); }
    function end(what) { if (!said) { said = true; say(what); say("done"); } }
    addEventListener("error", function (e) { end("error " + e.message); });
    addEventListener("unhandledrejection", function (e) {
        end("error " + (e.reason && e.reason.message || e.reason));
    });
    function jetstream() {
        var s = document.querySelector("#result-summary .score");
        if (!s || !s.textContent)
            return false;
        say("jetstream score " + s.textContent + " in " + secs() + " s");
        (JetStream.benchmarks || []).forEach(function (b) {
            say("jetstream " + b.name + " " + Number(b.score).toFixed(3));
        });
        end("jetstream finished");
        return true;
    }
    function speedometer() {
        var c = globalThis.benchmarkClient, r = document.getElementById("result-number");
        if (!c || !c._hasResults || !r)
            return false;
        say("speedometer score " + r.textContent + " in " + secs() + " s");
        var m = c._metrics || {};
        Object.keys(m).forEach(function (k) {
            if (k.indexOf("/") < 0 && k.indexOf("Iteration-") != 0 && m[k].mean !== undefined)
                say("speedometer " + k + " " + Number(m[k].mean).toFixed(2) + " " + (m[k].unit || ""));
        });
        end("speedometer finished");
        return true;
    }
    function poll() {
        if (said)
            return;
        var p = location.pathname;
        if ((p.indexOf("/jetstream/") == 0 && jetstream()) ||
            (p.indexOf("/speedometer/") == 0 && speedometer()))
            return;
        setTimeout(poll, 2000);
    }
    say("started " + location.pathname + location.search);
    setTimeout(poll, 2000);
})();
"""


# The parts run (docs/BENCH.md says why these): JetStream's JavaScript
# tests that run both with V8's compilers and --jitless (the Octane classics
# and a few of its newer ones), its WebAssembly tests (with the JIT only:
# --jitless has no WebAssembly), and six of Speedometer's suites, 5 times.
JS_TESTS = ("richards", "delta-blue", "crypto", "raytrace", "earley-boyer", "regexp", "splay",
            "navier-stokes", "Box2D", "Babylon", "UniPoker", "hash-map", "ai-astar",
            "json-parse-inspector")
WASM_TESTS = ("HashSet-wasm", "tsf-wasm", "quicksort-wasm", "gcc-loops-wasm", "richards-wasm")
SUITES = ("TodoMVC-JavaScript-ES5", "TodoMVC-React", "TodoMVC-Vue", "TodoMVC-jQuery",
          "Editor-CodeMirror", "Charts-chartjs")
RUNS = {
    "jetstream-js": "/jetstream/index.html?report=true&" + "&".join("test=" + t for t in JS_TESTS),
    "jetstream-wasm": "/jetstream/index.html?report=true&" +
                      "&".join("test=" + t for t in WASM_TESTS),
    "speedometer": "/speedometer/index.html?startAutomatically&iterationCount=5&suites=" +
                   ",".join(SUITES),
}


class Handler(http.server.SimpleHTTPRequestHandler):
    roots = {}
    log = None
    lock = threading.Lock()
    reports = 0

    def note(self, text):
        with self.lock:
            self.log.write(text + "\n")
            self.log.flush()

    def translate_path(self, path):
        p = urllib.parse.unquote(urllib.parse.urlsplit(path).path)
        for prefix, root in self.roots.items():
            if p == "/" + prefix or p.startswith("/" + prefix + "/"):
                rest = os.path.normpath(p[len(prefix) + 1:]).lstrip("/")
                if rest.startswith(".."):
                    break
                return os.path.join(root, rest)
        return "/nonexistent"

    def send_bytes(self, data, kind):
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        p = urllib.parse.urlsplit(self.path).path
        if p == "/crbench.js":
            return self.send_bytes(REPORTER.encode(), "text/javascript")
        if p.startswith("/run/") and p[5:] in RUNS:
            self.send_response(302)
            self.send_header("Location", RUNS[p[5:]])
            self.send_header("Content-Length", "0")
            self.end_headers()
            return None
        if p in ("/jetstream/", "/jetstream/index.html", "/speedometer/", "/speedometer/index.html"):
            f = os.path.join(self.translate_path(p), "index.html") if p.endswith("/") else \
                self.translate_path(p)
            page = open(f, "rb").read()
            tag = b'<script src="/crbench.js"></script>'
            page = page.replace(b"</body>", tag + b"</body>", 1) if b"</body>" in page else page + tag
            return self.send_bytes(page, "text/html; charset=utf-8")
        return super().do_GET()

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        if urllib.parse.urlsplit(self.path).path == "/report":
            with self.lock:
                Handler.reports += 1
                k = Handler.reports
            with open("%s.report-%d.json" % (self.log.name, k), "wb") as f:
                f.write(body)
        self.send_bytes(b"ok", "text/plain")

    def log_request(self, code="-", size="-"):
        self.note("%s %s status=%s" % (self.command, self.path, getattr(code, "value", code)))

    def log_message(self, *args):
        pass


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main(argv):
    if len(argv) != 5:
        print("usage: bench-server.py <jetstream dir> <speedometer dir> <port> <log>", file=sys.stderr)
        return 2
    Handler.roots = {"jetstream": argv[1], "speedometer": argv[2]}
    Handler.log = open(argv[4], "a")
    Server(("127.0.0.1", int(argv[3])), Handler).serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
