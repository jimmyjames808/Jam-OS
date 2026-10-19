#!/usr/bin/env python3
"""Check every user program's list (<wants.h>) before the boot image is
packed: the build's approval of what each boot-image program is given.

    checkwants.py <source dir>=<ELF> ...
    checkwants.py --selftest     check_text against lists it must take
                                 and lists it must refuse (make check)

Each ELF's PT_NOTE segments are searched for the note "JamOS" type 1, as
libos's wants_read does (user/lib/wants.c), and its text must follow the
same rules: printable ASCII lines, each one of

    svc <name>            a service init publishes (<os.h> SVC_*)
    svc net listen        /svc/net and the listen permission
                          (/svc/net-listen, never named on its own)
    svc net listen low    the same on ports below 1024 too
                          (/svc/net-low, never named on its own)
    mount <point> r|rw    <point>: /boot /esp /data /jam /pkg /usb* * (/pkg: r only,
                          init's read-only view of the packages' prefix)
    mount /data rw pkg    /data writable, the packages' prefix too
    mount /jam rw pkg     the same on /jam (the Jam OS drive's volume)
    right <name>          klog sysinfo clock debug jit
    posix                 a POSIX program (once; it grants /tmp, writable)

and, the build's own policy, the services that kill drivers and services
(devmgr-ctl, init), the disks' and mounts' (volumes) and the file server's (serve: with it a program could
put any file it can read on the network without the listen permission)
only for a program whose source is under user/tests/,
and netstack's and the resolver's reserves for the network's own services
(net-sys, dns-sys), the desktop's notices (notify) and the low ports (`svc net listen low`: the system's
ports, where a program could pose as a service) only for one under
user/services/ (the low ports also for the SSH server, ports/tinyssh or
user/ports/tinyssh: a port that init runs as a service, on port 22), and
init's sessions (session: a whole shell) only for one under user/services/,
ports/ (a recipe's directory: tinyssh's sshd), user/ports/ (the make
fragments not yet recipes) or user/tests/, and JIT memory (`right jit`:
memory it may write and run, docs/CR-PLAN.md section 12) only for a
package (ports/, user/ports/) or a test (user/tests/), never for the boot
image's system programs, and the packages' prefix writable (`mount /data
rw pkg`: bin/pkg's) only for one under user/services/, and the ESP
writable (`mount /esp rw`, or `mount * rw`, every mount, which takes it
in: what the next boot runs, docs/M13.7-REVIEW.md PR1) only for a service
or a test.
A program without a list is fine (it gets its terminal only). Prints how
many lists it checked (-v: each one); exits 1, naming each problem, if any.

Run from the repository root (the Makefile does)."""
import re
import struct
import sys

OS_H = "user/include/os.h"
POINTS = {"/boot", "/esp", "/data", "/jam", "/pkg", "/usb*", "*"}
BOOT_POINTS = {"/esp", "*"}   # writable: the ESP (`*`, every mount, takes it in)
RIGHTS = {"klog", "sysinfo", "clock", "debug", "jit"}
JIT_DIRS = ("ports/", "user/ports/", "user/tests/")   # `right jit`: packages and tests only
TESTS_ONLY = {"devmgr-ctl", "init", "serve", "volumes"}
SERVICES_ONLY = {"net-sys", "dns-sys", "notify"}
SESSION = "session"   # a whole shell: services, ports and tests only
SESSION_DIRS = ("user/services/", "ports/", "user/ports/", "user/tests/")
LISTEN = {"net-listen", "net-low"}   # given only as `svc net listen [low]` (<wants.h>)
LOW_PORTS = {"user/ports/tinyssh"}   # ports init runs as services: may listen below 1024
TEXT_MAX = 1024
WANTS_MAX = 24
PT_NOTE = 4


def services():
    with open(OS_H) as f:
        return set(re.findall(r'#define SVC_[A-Z_]+ +"([a-z0-9-]+)"', f.read()))


def notes(data):
    """The JamOS type-1 notes' text (bytes, NUL dropped)."""
    if data[:4] != b"\x7fELF" or data[4] != 2:
        raise ValueError("not a 64-bit ELF file")
    phoff, = struct.unpack_from("<Q", data, 32)
    phentsize, phnum = struct.unpack_from("<HH", data, 54)
    out = []
    for i in range(phnum):
        ptype, _, off, _, _, filesz = struct.unpack_from("<IIQQQQ", data, phoff + i * phentsize)
        if ptype != PT_NOTE:
            continue
        at = off
        while at + 12 <= off + filesz:
            namesz, descsz, ntype = struct.unpack_from("<III", data, at)
            name_pad, desc_pad = (namesz + 3) & ~3, (descsz + 3) & ~3
            name = data[at + 12:at + 12 + namesz]
            desc = data[at + 12 + name_pad:at + 12 + name_pad + descsz]
            if name == b"JamOS\0" and ntype == 1:
                if not desc.endswith(b"\0"):
                    raise ValueError("the list's text has no NUL")
                out.append(desc[:-1])
            at += 12 + name_pad + desc_pad
    return out


def check_text(text, in_tests, svcs, in_services=False, src=""):
    """The problems of one list's text; its wants as shown. src: the
    program's source directory (for `svc session`)."""
    problems, shown, n, posix = [], [], 0, False
    if len(text) + 1 > TEXT_MAX:
        problems.append("the list is longer than %d bytes" % (TEXT_MAX - 1))
    if any(c != 0x0a and not 0x20 <= c <= 0x7e for c in text):
        return ["the list has a byte that isn't printable ASCII"], shown
    for line in text.decode().split("\n"):
        if not line:
            continue
        w = line.split(" ")
        if w == ["posix"]:
            if posix:
                problems.append("'posix' twice")
            posix = True
            n += 1   # /tmp, writable (wants.c)
        elif len(w) == 3 and w[:3] == ["svc", "net", "listen"]:
            n += 2   # /svc/net and /svc/net-listen
        elif len(w) == 4 and w == ["svc", "net", "listen", "low"]:
            if not in_services and src.rstrip("/") not in LOW_PORTS:
                problems.append("'%s': only a service (user/services/) may ask for it" % line)
            n += 2   # /svc/net and /svc/net-low
        elif len(w) == 2 and w[0] == "svc":
            if w[1] in LISTEN:
                problems.append("'%s': write it `svc net listen%s`" %
                                (line, " low" if w[1].endswith("low") else ""))
            elif w[1] not in svcs:
                problems.append("'%s': no such service (os.h's SVC_*)" % line)
            elif w[1] in TESTS_ONLY and not in_tests:
                problems.append("'%s': only a test program (user/tests/) may ask for it" % line)
            elif w[1] in SERVICES_ONLY and not in_services:
                problems.append("'%s': only a service (user/services/) may ask for it" % line)
            elif w[1] == SESSION and not (in_tests or in_services or src.startswith(SESSION_DIRS)):
                problems.append("'%s': only a service, a port or a test may ask for it" % line)
            n += 1
        elif len(w) == 3 and w[0] == "mount" and w[1] in POINTS and w[2] in ("r", "rw"):
            if w[1] == "/pkg" and w[2] == "rw":
                problems.append("'%s': /pkg is a read-only view (mount /pkg r)" % line)
            elif w[1] in BOOT_POINTS and w[2] == "rw" and not (in_tests or in_services):
                problems.append("'%s': only a service or a test may write the ESP (what the "
                                "next boot runs)" % line)
            n += 1
        elif w in (["mount", "/data", "rw", "pkg"], ["mount", "/jam", "rw", "pkg"]):
            if not in_services:
                problems.append("'%s': only a service (user/services/) may ask for it" % line)
            n += 1
        elif len(w) == 2 and w[0] == "right" and w[1] in RIGHTS:
            if w[1] == "jit" and not (in_tests or (src.rstrip("/") + "/").startswith(JIT_DIRS)):
                problems.append("'%s': only a package or a test may ask for it" % line)
        else:
            problems.append("'%s': not a want (svc <name>, svc net listen [low], "
                            "mount <point> r|rw, right <name>, posix)" % line)
            continue
        shown.append(line)
    if n > WANTS_MAX:
        problems.append("more than %d services and mounts" % WANTS_MAX)
    return problems, shown


def selftest():
    """check_text on lists it must take and lists it must refuse; 0 on PASS."""
    svcs = services()
    take = [b"svc net\n", b"svc net listen\n", b"svc net listen\nsvc dns\nmount /data rw\n",
            b"right clock\n", b"svc init\n", b"svc volumes\n", b"posix\n",
            b"posix\nsvc net\nmount /data rw\n", b"svc session\n", b"mount /pkg r\n",
            b"posix\nmount /data r\nmount /pkg r\n", b"mount /esp rw\n", b"mount * rw\n",
            b"mount /esp r\nmount * r\n"]
    take_services = [b"svc net listen low\n", b"svc net-sys\n", b"mount /data rw pkg\n",
                     b"mount /jam rw pkg\n", b"mount /data rw pkg\nmount /jam rw pkg\n",
                     b"mount /esp rw\n", b"mount * rw\n"]
    refuse = [b"svc net-listen\n", b"svc dns listen\n", b"svc net listen now\n",
              b"svc net Listen\n", b"svc nope\n", b"right listen\n", b"svc init\n",
              b"svc net-sys\n", b"svc dns-sys\n", b"svc serve\n", b"svc volumes\n",
              b"svc net listen low\n",
              b"svc net-low\n", b"svc net listen lower\n", b"svc net listen low now\n",
              b"mount /data rw\n" * 21 + b"svc net listen\nsvc net listen\n",   # 25 wants
              b"posix\nposix\n", b"posix now\n", b"Posix\n", b"svc posix\n",
              b"svc session\n", b"right jit\n", b"right JIT\n", b"mount /data rw pkg\n",
              b"mount /data r pkg\n", b"mount * rw pkg\n", b"mount /usb* rw pkg\n",
              b"mount /data rw Pkg\n", b"mount /data rw pkg now\n", b"mount /jam rw pkg\n",
              b"mount /jam r pkg\n", b"mount /usb0 rw pkg\n", b"mount /pkg rw\n",
              b"mount /pkg r pkg\n", b"mount /pkg\n", b"mount /pkgx r\n", b"mount /pkg/bin r\n",
              b"mount /esp rw\n", b"mount * rw\n", b"posix\nmount /data rw\nmount * rw\n"]
    fails = []
    for t in take:
        problems, _ = check_text(t, True, svcs)
        if problems:
            fails.append("refused %r: %s" % (t, problems))
    for t in take_services:
        problems, _ = check_text(t, False, svcs, True)
        if problems:
            fails.append("refused %r for a service: %s" % (t, problems))
    for src in ("ports/tinyssh", "user/ports/sshd"):
        problems, _ = check_text(b"svc session\nposix\n", False, svcs, False, src)
        if problems:
            fails.append("refused svc session for a port (%s): %s" % (src, problems))
    problems, _ = check_text(b"posix\nsvc net listen low\nsvc session\n", False, svcs, False,
                             "user/ports/tinyssh")
    if problems:
        fails.append("refused the SSH server's list: %s" % problems)
    problems, _ = check_text(b"svc net listen low\n", False, svcs, False, "user/ports/sbase")
    if not problems:
        fails.append("took svc net listen low for a port that isn't the SSH server")
    for src, tests in (("ports/chromium", False), ("ports", False), ("user/ports/x", False),
                       ("user/tests/t", True)):
        problems, _ = check_text(b"right jit\nposix\n", tests, svcs, False, src)
        if problems:
            fails.append("refused right jit for %s: %s" % (src, problems))
    for src in ("user/services/x", "user/apps/x", "drivers/x", "user/posix/progs/sh",
                "user/posix/test", "portsx/y", "user/portsx"):
        problems, _ = check_text(b"right jit\n", False, svcs, src.startswith("user/services/"),
                                 src)
        if not problems:
            fails.append("took right jit for a system program (%s)" % src)
    for src in ("user/tests/t", "ports/x", "user/ports/x"):
        for want in (b"mount /data rw pkg\n", b"mount /jam rw pkg\n"):
            problems, _ = check_text(want, src.startswith("user/tests/"), svcs, False, src)
            if not problems:
                fails.append("took %s for %s" % (want.decode().strip(), src))
    for t in refuse:
        problems, _ = check_text(t, False, svcs, False, "user/apps/x")
        if not problems:
            fails.append("took %r" % t)
    for f in fails:
        print("checkwants --selftest: " + f)
    print("checkwants --selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0


def main(args):
    if args == ["--selftest"]:
        return selftest()
    svcs = services()
    verbose = args[:1] == ["-v"]
    args = args[1:] if verbose else args
    bad = lists = 0
    for arg in args:
        src, _, elf = arg.partition("=")
        name = src.rstrip("/").split("/")[-1]
        with open(elf, "rb") as f:
            data = f.read()
        try:
            texts = notes(data)
        except (ValueError, struct.error) as e:
            print("checkwants: %s: %s" % (elf, e))
            bad += 1
            continue
        if len(texts) > 1:
            print("checkwants: %s: more than one list" % name)
            bad += 1
            continue
        if not texts:
            continue
        lists += 1
        problems, shown = check_text(texts[0], src.startswith("user/tests/"), svcs,
                                     src.startswith("user/services/"), src)
        for p in problems:
            print("checkwants: %s (%s): %s" % (name, src, p))
        bad += len(problems)
        if not problems and verbose:
            print("checkwants: %s: %s" % (name, "; ".join(shown) or "nothing"))
    print("checkwants: %d program lists%s" % (lists, ", %d problem(s)" % bad if bad else " ok"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
