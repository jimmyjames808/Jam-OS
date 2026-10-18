#!/usr/bin/env python3
"""Name the user frames of a captured thread listing (the kernel's
debug_command "threads", the shell's `ps -t <pid|name>`) from the program's
unstripped binary, on the host.

    symbolize-threads.py [--name NAME] [--base HEX] [--symbolizer PATH] <log> <program>

<log> is a serial log or a saved kernel log holding one or more listings
(each starts "threads: process <koid> "<name>": <n> threads"); --name
keeps only the listings of that process. <program> is the unstripped ELF
the process ran (Chromium's: out/jamos/<program> in the build tree; ours:
build/user/<prog> before strip). --base is where a PIE program was loaded
(0, the default, for a program linked at its address). The listing is
printed again with each `user #N` and `stack +off` line followed by its
function and source line (inlined frames indented under it), a futex's
address with the global it lies in ([name+offset]), and each `in system
call N` with the call's name from abi/syscalls.def.

The symbolizer: --symbolizer, else $LLVM_SYMBOLIZER, else llvm-symbolizer
on PATH, else the one in a Chromium tree's third_party/llvm-build under
~/cr (tools/cr/toolchain.sh's clang). Return addresses (every frame but
#0, and the stack words) are looked up one byte back, so the line is the
call's, not the one after it.
"""
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HEAD = re.compile(r'threads: process (\d+) "([^"]*)"')
FRAME = re.compile(r'^(\s*)user #(\d+) (0x[0-9a-f]+)')
STACK = re.compile(r'^(\s*)stack \+(0x[0-9a-f]+|0) (0x[0-9a-f]+)')
FUTEX = re.compile(r'waits for futex at (0x[0-9a-f]+)')
SYSCALL = re.compile(r'in system call (\d+)\b(?! \([a-z_]+\))')
STAMP = re.compile(r'\[\s*\d+\.\d{6}\] ')   # the kernel log's time stamp


def syscall_names():
    names = {}
    try:
        with open(os.path.join(TOP, "abi", "syscalls.def")) as f:
            for line in f:
                m = re.match(r'^(\d+)\s+(\w+)\s', line)
                if m:
                    names[int(m.group(1))] = m.group(2)
    except OSError:
        pass
    return names


def find_symbolizer(given):
    if given:
        return given
    if os.environ.get("LLVM_SYMBOLIZER"):
        return os.environ["LLVM_SYMBOLIZER"]
    on_path = shutil.which("llvm-symbolizer")
    if on_path:
        return on_path
    pattern = os.path.expanduser(
        "~/cr/*/src/chromium-*/third_party/llvm-build/Release+Asserts/bin/llvm-symbolizer")
    found = sorted(glob.glob(pattern))
    if found:
        return found[-1]
    sys.exit("symbolize-threads: no llvm-symbolizer (give --symbolizer)")


def symbolize(tool, program, addrs):
    """{address: [(function, place), ...]} (innermost inlined frame first)."""
    if not addrs:
        return {}
    order = sorted(set(addrs))
    query = "".join("0x%x\n" % a for a in order)
    out = subprocess.run([tool, "--obj=" + program, "--inlining", "--demangle",
                          "--functions=linkage"],
                         input=query, capture_output=True, text=True, check=False).stdout
    blocks = out.strip("\n").split("\n\n")
    names = {}
    for a, block in zip(order, blocks):
        lines = block.splitlines()
        if lines and lines[0].startswith("0x"):
            lines = lines[1:]   # newer symbolizers echo the address
        names[a] = [(lines[i], lines[i + 1] if i + 1 < len(lines) else "??")
                    for i in range(0, len(lines) - 1, 2)]
    return names


def symbolize_data(tool, program, addrs):
    """{address: "global+offset"} for the globals the addresses fall in."""
    if not addrs:
        return {}
    order = sorted(set(addrs))
    query = "".join("DATA 0x%x\n" % a for a in order)
    out = subprocess.run([tool, "--obj=" + program, "--demangle"], input=query,
                         capture_output=True, text=True, check=False).stdout
    names = {}
    for a, block in zip(order, out.strip("\n").split("\n\n")):
        lines = [l for l in block.splitlines() if not l.startswith("DATA ")]
        if len(lines) < 2 or lines[0] == "??":
            continue
        start = lines[1].split()[0]
        try:
            names[a] = "%s+%#x" % (lines[0], a - int(start, 0))
        except ValueError:
            names[a] = lines[0]
    return names


def listings(lines, name):
    """The lines of each listing: its head, then its indented lines up to
    debug_command's own end line ("dbgcmd: threads ... -> n"). Other kernel
    lines logged meanwhile are left out; in a serial log a kernel line may
    start in the middle of another's, after its time stamp."""
    blocks, cur = [], None
    for raw in lines:
        stamps = list(STAMP.finditer(raw))
        line = (raw[stamps[-1].end():] if stamps else raw).rstrip("\r\n")
        m = HEAD.search(line)
        if m:
            cur = [line] if name is None or m.group(2) == name else None
            if cur is not None:
                blocks.append(cur)
        elif cur is not None and line.startswith("dbgcmd: "):
            cur = None
        elif cur is not None and line.startswith("  "):
            cur.append(line)
    return blocks


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--name", help="only this process's listings")
    ap.add_argument("--base", default="0", help="a PIE program's load address (hex)")
    ap.add_argument("--symbolizer", help="llvm-symbolizer to use")
    ap.add_argument("log")
    ap.add_argument("program")
    a = ap.parse_args(argv)
    base = int(a.base, 16)
    with open(a.log, errors="replace") as f:
        blocks = listings(f, a.name)
    if not blocks:
        sys.exit("symbolize-threads: no thread listing in %s" % a.log)
    def code_key(line):
        """(indent, the address to look up) of a frame or stack line."""
        m = FRAME.match(line)
        if m:
            v = int(m.group(3), 16) - base
            return m.group(1), v if m.group(2) == "0" else v - 1
        m = STACK.match(line)
        if m:
            return m.group(1), int(m.group(3), 16) - base - 1
        return None, None

    def data_key(line):
        m = FUTEX.search(line)
        return int(m.group(1), 16) - base if m else None

    lines = [line for block in blocks for line in block]
    tool = find_symbolizer(a.symbolizer)
    code = symbolize(tool, a.program, [k for _, k in map(code_key, lines) if k is not None and k >= 0])
    data = symbolize_data(tool, a.program,
                          [k for k in map(data_key, lines) if k is not None and k >= 0])
    calls = syscall_names()
    for block in blocks:
        for line in block:
            line = SYSCALL.sub(lambda m: m.group(0) + (" (%s)" % calls[int(m.group(1))]
                                                       if int(m.group(1)) in calls else ""), line)
            k = data_key(line)
            if k in data:
                line += "  [%s]" % data[k]
            print(line)
            indent, k = code_key(line)
            for depth, (fn, place) in enumerate(code.get(k, []) if k is not None else []):
                print("%s    %s%s  %s" % (indent, "  " * depth, fn, place))
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
