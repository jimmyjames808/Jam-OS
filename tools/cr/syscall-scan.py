#!/usr/bin/env python3
"""The syscall scan (docs/CR-PLAN.md section 1, "Inline system calls";
track CT1): fail on a program that issues a system call instruction of
its own.

On Jam OS a program reaches the kernel only through Jam OS's own call
stubs (user/lib/syscalls.S: one `jam_<call>` each, with Jam OS's numbers)
and the few hand-written assembly routines of the POSIX layer that end a
thread (musl's __unmapself override); every Linux call goes through
musl's wrappers into the call layer. Code that executes `syscall` itself
(linux_syscall_support.h in Chromium's sandbox and breakpad, a
`syscall` in inline assembly) would run a Jam OS call with Linux's
number and arguments: wrong at best. This disassembles each program and
lists every `syscall`, `sysenter` and `int $0x80` outside those routines.

    tools/cr/syscall-scan.py [--objdump CMD] <program>...

The allowed routines are read from the tree, not listed here: every
global label of an assembly file under user/ that holds a `syscall`
line. A program without its symbol table can't be checked (each site
must be put in a function) and fails. Exit 0: every program clean;
1: a site outside the stubs (each listed: program, function, address);
2: usage, or a program objdump can't read.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys

TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SYSCALL_LINE = re.compile(r"^\s*syscall\s*(#.*|/\*.*)?$")
GLOBAL = re.compile(r"^\s*\.glob(?:a)?l\s+([A-Za-z_.$][\w.$]*)")
FUNC = re.compile(r"^([0-9a-f]+) <(.+)>:$")
# An instruction line of `objdump -d --no-show-raw-insn`: "  401000:\tsyscall"
INSN = re.compile(r"^\s*([0-9a-f]+):\s+(.*)$")
BAD = re.compile(r"^(syscall|sysenter|int\s+\$0x80)\b")


def allowed_routines(top):
    """The global labels of the assembly files under user/ that issue a
    system call: Jam OS's stubs and the layer's own routines."""
    names = set()
    for d, _, files in os.walk(os.path.join(top, "user")):
        for f in files:
            if not f.endswith((".S", ".s")):
                continue
            with open(os.path.join(d, f), errors="replace") as fh:
                text = fh.read().split("\n")
            if any(SYSCALL_LINE.match(line) for line in text):
                names.update(m.group(1) for m in map(GLOBAL.match, text) if m)
    return names


def objdump_cmd(given):
    if given:
        return given
    cross = os.environ.get("CROSS", "x86_64-elf-")
    for cand in (cross + "objdump", "llvm-objdump", "objdump"):
        if shutil.which(cand):
            return cand
    return None


def scan(objdump, path, allowed):
    """(sites, bad, functions seen, error): sites = every system call
    instruction, bad = those outside the allowed routines, as
    (function, address, instruction)."""
    proc = subprocess.Popen([objdump, "-d", "-w", "--no-show-raw-insn", path],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            universal_newlines=True, errors="replace")
    func, funcs, sites, bad = None, 0, 0, []
    for line in proc.stdout:
        m = FUNC.match(line.rstrip("\n"))
        if m:
            func = m.group(2)
            funcs += 1
            continue
        m = INSN.match(line)
        if not m or not BAD.match(m.group(2).strip()):
            continue
        sites += 1
        name = func.split("+")[0] if func else None
        if name not in allowed:
            bad.append((func or "(no symbol)", m.group(1), m.group(2).strip()))
    err = proc.stderr.read()
    rc = proc.wait()
    if rc != 0:
        return sites, bad, funcs, (err.strip() or "objdump exited %d" % rc)
    return sites, bad, funcs, None


SELFTEST_GOOD = r"""
#include <unistd.h>
int main(void) { return getpid() > 0 ? 0 : 1; }
"""
SELFTEST_BAD = r"""
#include <unistd.h>
__attribute__((noinline)) long rogue(long n) { long r; __asm__ volatile("syscall" : "=a"(r) : "a"(n) : "rcx", "r11", "memory"); return r; }
int main(void) { return (int)rogue(39) + (getpid() > 0 ? 0 : 1); }
"""


def selftest(objdump, top):
    """Two programs built with tools/jamos-cc against build/sysroot: one
    that calls through libc (clean), one with a `syscall` of its own in
    rogue() (caught, by name). make check runs it."""
    import tempfile
    allowed = allowed_routines(top)
    for want in ("jam_process_exit", "__unmapself"):
        if want not in allowed:
            print("syscall-scan: selftest: %s is not among the allowed routines" % want)
            return 1
    with tempfile.TemporaryDirectory() as tmp:
        out = {}
        for name, text in (("good", SELFTEST_GOOD), ("bad", SELFTEST_BAD)):
            src = os.path.join(tmp, name + ".c")
            with open(src, "w") as f:
                f.write(text)
            exe = os.path.join(tmp, name)
            r = subprocess.run([os.path.join(top, "tools", "jamos-cc"), "-O2", "-o", exe, src],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if r.returncode != 0:
                print("syscall-scan: selftest: jamos-cc failed:\n" + r.stdout.decode())
                return 1
            out[name] = scan(objdump, exe, allowed)
    sites, bad, _, err = out["good"]
    if err or bad or sites == 0:
        print("syscall-scan: selftest: the clean program: %d sites, %s, %s" % (sites, bad, err))
        return 1
    sites, bad, _, err = out["bad"]
    if err or [b[0] for b in bad] != ["rogue"]:
        print("syscall-scan: selftest: the rogue program: %s, %s" % (bad, err))
        return 1
    print("syscall-scan: selftest: ok (a clean program passes, rogue()'s syscall is caught; "
          "%d routines allowed)" % len(allowed))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    if sys.argv[1:] == ["--selftest"]:
        objdump = objdump_cmd(None)
        return selftest(objdump, TOP) if objdump else 2
    ap.add_argument("--objdump", help="the disassembler (default: $CROSS objdump, "
                    "llvm-objdump or objdump)")
    ap.add_argument("--top", default=TOP, help="the Jam OS tree (the allowed routines)")
    ap.add_argument("program", nargs="+")
    args = ap.parse_args()
    objdump = objdump_cmd(args.objdump)
    if not objdump:
        print("syscall-scan: no objdump (x86_64-elf-objdump, llvm-objdump, objdump)",
              file=sys.stderr)
        return 2
    allowed = allowed_routines(args.top)
    if not allowed:
        print("syscall-scan: no call stubs found under %s/user" % args.top, file=sys.stderr)
        return 2
    rc = 0
    for path in args.program:
        sites, bad, funcs, err = scan(objdump, path, allowed)
        if err:
            print("syscall-scan: %s: %s" % (path, err), file=sys.stderr)
            rc = max(rc, 2)
            continue
        if funcs == 0:
            print("syscall-scan: %s: FAIL: no symbols (scan a build with its symbol table)"
                  % path)
            rc = max(rc, 1)
            continue
        if bad:
            print("syscall-scan: %s: FAIL: %d of %d system call instructions outside Jam "
                  "OS's call stubs:" % (path, len(bad), sites))
            for func, addr, insn in bad:
                print("  %s in %s at 0x%s" % (insn, func, addr))
            rc = max(rc, 1)
        else:
            print("syscall-scan: %s: ok (%d system call instructions, all in Jam OS's "
                  "call stubs)" % (path, sites))
    return rc


if __name__ == "__main__":
    sys.exit(main())
