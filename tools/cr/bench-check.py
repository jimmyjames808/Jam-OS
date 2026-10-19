#!/usr/bin/env python3
"""The bench case's run of cr-test.sh (docs/CR-PLAN.md, CR-e): what
JetStream and Speedometer said in each load (tools/cr/bench-server.py's
reporter: "crbench: ..." console lines the browser logs), as a table:

    bench-check.py <log> <run>...

<run> is <name>:<mode> as the case ran them in order (jetstream-js:jit,
jetstream-js:jitless, ...). For each, its score and each part's figure
(JetStream: a score, higher is better; Speedometer: each suite's mean ms,
lower is better, and its score, higher is better), then, for a benchmark
run both ways, the JIT's score over --jitless's. Prints Markdown (the
lines docs/BENCH.md takes); exit 0 if every run said a score."""
import re
import sys


def loads(log):
    """The reporter's lines, one list per load (from "started" to "done")."""
    out, cur = [], None
    last = None
    for m in re.finditer(r'crbench: ([^"\r\n]*)', log):
        line = m.group(1).strip()
        if line == last:
            continue   # a console line seen twice
        last = line
        if line.startswith("started "):
            cur = [line]
            continue
        if cur is None:
            continue
        cur.append(line)
        if line == "done":
            out.append(cur)
            cur = None
    if cur:
        out.append(cur)   # a load that never ended
    return out


def parse(lines):
    score, secs, parts, error = None, None, [], None
    for line in lines:
        w = line.split()
        if len(w) >= 3 and w[1] == "score":
            score = w[2]
            secs = w[4] if len(w) >= 5 else None
        elif line.startswith("error "):
            error = line[6:]
        elif len(w) >= 3 and w[0] in ("jetstream", "speedometer") and w[1] != "score":
            parts.append((w[1], " ".join(w[2:])))
    return score, secs, parts, error


def main(argv):
    if len(argv) < 3:
        print("usage: bench-check.py <log> <name:mode>...", file=sys.stderr)
        return 2
    log = open(argv[1], errors="replace").read()
    runs = argv[2:]
    got = loads(log)
    ok = True
    scores = {}
    for i, run in enumerate(runs):
        lines = got[i] if i < len(got) else []
        score, secs, parts, error = parse(lines)
        name, _, mode = run.partition(":")
        if score is None:
            ok = False
            print("bench: %s: no score%s" % (run, (" (" + error + ")") if error else ""))
            continue
        try:
            scores[(name, mode)] = float(score.replace(",", ""))
        except ValueError:
            ok = False
            print("bench: %s: score %r" % (run, score))
            continue
        print("bench: %s: score %s in %s s" % (run, score, secs))
        print("")
        print("| %s, %s | |" % (name, mode))
        print("|---|---|")
        for part, value in parts:
            print("| %s | %s |" % (part, value))
        print("")
    for (name, mode), s in sorted(scores.items()):
        if mode == "jit" and (name, "jitless") in scores and scores[(name, "jitless")] > 0:
            print("bench: %s: the JIT's score %.2f, --jitless's %.2f: %.1fx" %
                  (name, s, scores[(name, "jitless")], s / scores[(name, "jitless")]))
    print("bench: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
