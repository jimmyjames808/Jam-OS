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
    mount <point> r|rw    <point>: /boot /esp /data /usb* *
    right <name>          klog sysinfo clock debug

and, the build's own policy, the services that kill drivers and services
(devmgr-ctl, init) only for a program whose source is under user/tests/,
and netstack's reserve for the network's own services (net-sys) only for
one under user/services/.
A program without a list is fine (it gets its terminal only). Prints how
many lists it checked (-v: each one); exits 1, naming each problem, if any.

Run from the repository root (the Makefile does)."""
import re
import struct
import sys

OS_H = "user/include/os.h"
POINTS = {"/boot", "/esp", "/data", "/usb*", "*"}
RIGHTS = {"klog", "sysinfo", "clock", "debug"}
TESTS_ONLY = {"devmgr-ctl", "init"}
SERVICES_ONLY = {"net-sys"}
LISTEN = "net-listen"   # given only as `svc net listen` (<wants.h>)
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


def check_text(text, in_tests, svcs, in_services=False):
    """The problems of one list's text; its wants as shown."""
    problems, shown, n = [], [], 0
    if len(text) + 1 > TEXT_MAX:
        problems.append("the list is longer than %d bytes" % (TEXT_MAX - 1))
    if any(c != 0x0a and not 0x20 <= c <= 0x7e for c in text):
        return ["the list has a byte that isn't printable ASCII"], shown
    for line in text.decode().split("\n"):
        if not line:
            continue
        w = line.split(" ")
        if len(w) == 3 and w[:3] == ["svc", "net", "listen"]:
            n += 2   # /svc/net and /svc/net-listen
        elif len(w) == 2 and w[0] == "svc":
            if w[1] == LISTEN:
                problems.append("'%s': write it `svc net listen`" % line)
            elif w[1] not in svcs:
                problems.append("'%s': no such service (os.h's SVC_*)" % line)
            elif w[1] in TESTS_ONLY and not in_tests:
                problems.append("'%s': only a test program (user/tests/) may ask for it" % line)
            elif w[1] in SERVICES_ONLY and not in_services:
                problems.append("'%s': only a service (user/services/) may ask for it" % line)
            n += 1
        elif len(w) == 3 and w[0] == "mount" and w[1] in POINTS and w[2] in ("r", "rw"):
            n += 1
        elif len(w) == 2 and w[0] == "right" and w[1] in RIGHTS:
            pass
        else:
            problems.append("'%s': not a want (svc <name>, svc net listen, mount <point> r|rw, "
                            "right <name>)" % line)
            continue
        shown.append(line)
    if n > WANTS_MAX:
        problems.append("more than %d services and mounts" % WANTS_MAX)
    return problems, shown


def selftest():
    """check_text on lists it must take and lists it must refuse; 0 on PASS."""
    svcs = services()
    take = [b"svc net\n", b"svc net listen\n", b"svc net listen\nsvc dns\nmount /data rw\n",
            b"right clock\n", b"svc init\n"]
    refuse = [b"svc net-listen\n", b"svc dns listen\n", b"svc net listen now\n",
              b"svc net Listen\n", b"svc nope\n", b"right listen\n", b"svc init\n",
              b"svc net-sys\n",
              b"mount /data rw\n" * 21 + b"svc net listen\nsvc net listen\n"]   # 25 wants
    fails = []
    for t in take:
        problems, _ = check_text(t, True, svcs)
        if problems:
            fails.append("refused %r: %s" % (t, problems))
    for t in refuse:
        problems, _ = check_text(t, False, svcs)
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
                                     src.startswith("user/services/"))
        for p in problems:
            print("checkwants: %s (%s): %s" % (name, src, p))
        bad += len(problems)
        if not problems and verbose:
            print("checkwants: %s: %s" % (name, "; ".join(shown) or "nothing"))
    print("checkwants: %d program lists%s" % (lists, ", %d problem(s)" % bad if bad else " ok"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
