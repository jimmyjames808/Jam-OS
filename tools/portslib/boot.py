"""The boot image's side of the ports (docs/M13.7-PLAN.md section 1, "system
ports"): the recipes with `place boot`, and what of their stages goes into
bootfs (tools/mkbootfs.py).

A boot port is built like any other (build.py) and lands under
build/ports/<name>/stage/ with the prefix /boot/usr, so a program the
recipe calls bin/ls is bootfs's usr/bin/ls. What goes in:

  program, file   usr/<path>
  multicall       usr/<dir of the program>/<name>, the program's own bytes
                  (mkbootfs stores identical files once, and the kernel's
                  bootfs check accepts exactly that sharing)
  licence         share/licences/<port>/<file>: the notices of the whole
                  image are under /boot/share/licences, not /boot/usr
                  (the stage holds them under share/licences/ already)

Libraries are not in the image. `bootlist` returns the entries as
(bootfs name, file, SHA-256); `write_list` makes the file the Makefile
gives mkbootfs (`@file`: one `name=path` a line, the hash after a `#` so
a change of any file changes the list and make packs bootfs again).
"""
import os

from . import stage as stagemod


def boot_ports(tree):
    """The names of the recipes that go in the boot image, sorted."""
    return sorted(n for n, r in tree.recipes.items() if r.place == "boot")


def bootlist(out, names):
    """[(bootfs name, path of the file, sha256)] for the stages of the
    named boot ports under out, in order; ValueError when two ports (or
    two lines) want one name."""
    entries, seen = [], {}

    def add(name, path, digest, port):
        if name in seen:
            raise ValueError("%s: %s is also %s's" % (port, name, seen[name]))
        seen[name] = port
        entries.append((name, path, digest))

    for port in names:
        stage = os.path.join(out, port, "stage")
        lines = stagemod.read_list(os.path.join(stage, "stage.txt"))
        files = {}
        for w in lines:
            if w[0] in ("program", "file", "licence"):
                files[w[1]] = w[3]
        for w in lines:
            kind = w[0]
            if kind in ("program", "file"):
                add("usr/" + w[1], os.path.join(stage, w[1]), w[3], port)
            elif kind == "licence":
                add(w[1], os.path.join(stage, w[1]), w[3], port)
            elif kind == "multicall":
                prog, name = w[1], w[2]
                add("usr/" + os.path.join(os.path.dirname(prog), name),
                    os.path.join(stage, prog), files[prog], port)
    return entries


def write_list(entries, f):
    for name, path, digest in entries:
        f.write("%s=%s  # %s\n" % (name, path, digest))
