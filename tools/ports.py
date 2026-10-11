#!/usr/bin/env python3
"""The ports builder: builds ported programs and libraries from their
recipes in ports/, on the Mac or the test host's Linux
(docs/M13.7-PLAN.md section 5).

    ports.py build [-f] [-v] [-j N] <port>...   build, in dependency order,
                                                what is out of date
    ports.py lint [<port>...]                   check recipes (all of them:
                                                make check runs it)
    ports.py fetch <port>...                    only download the sources
                                                (and fill the mirror)
    ports.py graph [<port>...]                  the build order, with what
                                                each needs and runs
    ports.py boot [-o FILE]                     build every `place boot` port
                                                and write the list of
                                                bootfs entries (make's)
    ports.py selftest                           the builder's own tests
                                                (make check runs them)
    ports.py package|repo|verify|vectors|abi    packages, the signed
                                                repository and its formats
                                                (tools/portslib/pkgcmd.py)

Options before the command: --ports DIR (default ports/), --out DIR
(default build/ports), --offline (no network: the cache and the mirror
only). Host tools (meson, ninja) a user keeps in ~/.cache/jamos/tools/bin
(or $JAMOS_TOOLS/bin) are put first on PATH. Each port builds in a clean tree that sees only the libraries it
declared, statically, with tools/jamos-cc and the sysroot (make builds
build/sysroot first); its result is build/ports/<name>/stage/ and the
stage list stage.txt there. Sources are fetched by size and SHA-256 into
~/.cache/jamos/distfiles, through the mirror (tools/portslib/fetch.py).

Not built yet: `audit` (CVEs).

The code is in tools/portslib/, a module per job: recipe (the format),
licence, tree (the ports tree and the order), fetch, source (unpack,
overrides), patch, toolchain (the sysroot and tools a build sees),
build, stage (keep, check), boot (the boot image's side), selftest.
"""
import argparse
import io
import os
import sys

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(TOP, "tools"))
sys.dont_write_bytecode = True   # no __pycache__ left in tools/

from portslib import boot as bootmod  # noqa: E402
from portslib import build as buildmod  # noqa: E402
from portslib import fetch as fetchmod  # noqa: E402
from portslib import pkgcmd  # noqa: E402
from portslib import tree as treemod  # noqa: E402


def local_tools():
    """Host tools kept in the user's home instead of installed on the machine
    (a newer Meson than the distribution's, say: pip into a venv at
    ~/.cache/jamos/tools, or wherever JAMOS_TOOLS says): put first on PATH,
    for the builds and for finding what a recipe's `host` line wants."""
    d = os.environ.get("JAMOS_TOOLS") or os.path.expanduser("~/.cache/jamos/tools")
    bin_dir = os.path.join(d, "bin")
    if os.path.isdir(bin_dir):
        os.environ["PATH"] = bin_dir + os.pathsep + os.environ.get("PATH", "/usr/bin:/bin")


def load(args):
    t, problems = treemod.load(args.ports)
    if problems:
        for p in problems:
            print("ports: " + p)
        sys.exit(1)
    return t


def cmd_lint(args):
    t, problems = treemod.load(args.ports)
    names = args.port or None
    for n in names or []:
        if n not in t.recipes:
            problems.append("no port %s" % n)
    if not problems:
        problems = treemod.lint(t, names)
    for p in problems:
        print("ports lint: " + p)
    print("ports lint: %d recipe%s, %s" % (len(names or t.recipes),
                                          "" if len(names or t.recipes) == 1 else "s",
                                          "%d problem(s)" % len(problems) if problems else "ok"))
    return 1 if problems else 0


def cmd_build(args):
    t = load(args)
    try:
        problems = treemod.lint(t, treemod.order(t, args.port))
    except (treemod.CycleError, KeyError) as e:
        problems = ["%s" % e if isinstance(e, treemod.CycleError) else "no port %s" % e]
    if problems:
        for p in problems:
            print("ports: " + p)
        return 1
    b = buildmod.Builder(t, TOP, args.out, args.jobs, args.port if args.force else (),
                         args.verbose)
    try:
        b.build(args.port)
    except buildmod.BuildError as e:
        print("ports: " + str(e))
        return 1
    return 0


def cmd_boot(args):
    """Build the boot ports and write the list mkbootfs packs (`@FILE`),
    only when it changed, so an unchanged boot image is packed anew by
    nobody."""
    t = load(args)
    args.port, args.force, args.verbose, args.jobs = bootmod.boot_ports(t), False, False, None
    if cmd_build(args):
        return 1
    try:
        entries = [(n, os.path.relpath(f, TOP), h) for n, f, h in
                   bootmod.bootlist(args.out, args.port)]
    except (ValueError, OSError) as e:
        print("ports: " + str(e))
        return 1
    if args.output:
        text = io.StringIO()
        bootmod.write_list(entries, text)
        try:
            with open(args.output) as f:
                same = f.read() == text.getvalue()
        except OSError:
            same = False
        if not same:
            with open(args.output + ".new", "w") as f:
                f.write(text.getvalue())
            os.replace(args.output + ".new", args.output)
    return 0


def cmd_fetch(args):
    t = load(args)
    try:
        names = treemod.order(t, args.port)
    except (treemod.CycleError, KeyError) as e:
        print("ports: %s" % e)
        return 1
    bad = 0
    for n in names:
        for s in t.recipes[n].sources:
            try:
                print("%s: %s" % (n, fetchmod.fetch(s)))
            except fetchmod.FetchError as e:
                print("ports: %s: %s" % (n, e))
                bad += 1
    return 1 if bad else 0


def cmd_graph(args):
    t = load(args)
    try:
        names = treemod.order(t, args.port or sorted(t.recipes), ("needs", "tool", "runs"))
    except (treemod.CycleError, KeyError) as e:
        print("ports: %s" % e)
        return 1
    for n in names:
        r = t.recipes[n]
        deps = [k + " " + d for k in ("needs", "tool", "runs") for d, _ in r.deps(k)]
        print("%-16s %-10s %-8s %s" % (n, r.version, r.place, ", ".join(deps)))
    return 0


def cmd_selftest(args):
    from portslib import selftest
    return selftest.main(TOP)


def cmd_later(args):
    print("ports: '%s' is not built yet (docs/M13.7-PLAN.md: PO12 for audit)" % args.command)
    return 2


def main(argv):
    ap = argparse.ArgumentParser(prog="ports.py", description=__doc__.split("\n\n")[0])
    ap.add_argument("--ports", default=os.path.join(TOP, "ports"))
    ap.add_argument("--out", default=os.path.join(TOP, "build", "ports"))
    ap.add_argument("--offline", action="store_true")
    sub = ap.add_subparsers(dest="command")
    b = sub.add_parser("build")
    b.add_argument("-f", "--force", action="store_true", help="build the named ports anyway")
    b.add_argument("-v", "--verbose", action="store_true")
    b.add_argument("-j", "--jobs", type=int)
    b.add_argument("port", nargs="+")
    for name in ("lint", "graph"):
        sub.add_parser(name).add_argument("port", nargs="*")
    sub.add_parser("fetch").add_argument("port", nargs="+")
    sub.add_parser("boot").add_argument("-o", "--output", metavar="FILE")
    sub.add_parser("selftest")
    sub.add_parser("audit").add_argument("port", nargs="*")
    pkgcmd.add_parsers(sub)
    args = ap.parse_args(argv)
    if not args.command:
        ap.print_help()
        return 2
    args.ports, args.out = os.path.abspath(args.ports), os.path.abspath(args.out)
    if args.offline:
        os.environ["JAMOS_OFFLINE"] = "1"
    local_tools()
    os.chdir(TOP)   # third_party/ and user/include/os.h are read from here
    run = {"lint": cmd_lint, "build": cmd_build, "fetch": cmd_fetch, "graph": cmd_graph,
           "selftest": cmd_selftest, "boot": cmd_boot, **pkgcmd.commands(TOP)}.get(args.command, cmd_later)
    return run(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
