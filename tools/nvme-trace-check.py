#!/usr/bin/env python3
"""Check QEMU's trace of every NVMe command against the stage's opcode list.

    nvme-trace-check.py [--stage 1|2] [--allow-admin 0a,0c] [--allow-io ..]
                        [--require-io 02] [--require-admin 06]
                        [--write-ranges a-b,c-d,...] <trace file>

The trace is what QEMU wrote for `-trace pci_nvme_admin_cmd -trace
pci_nvme_io_cmd` (QEMU 10.0.2; tools/qemu-test.sh's QEMU_EXTRA with
`-D <file>`): one line per command the emulated controller was handed,
whoever sent it (OVMF's driver before Jam OS boots, then drv/nvme), e.g.
    pci_nvme_io_cmd cid 1 nsid 0x1 sqid 1 opc 0x2 opname 'NVME_NVM_CMD_READ'
    pci_nvme_admin_cmd cid 3 sqid 0 opc 0x6 opname 'NVME_ADMIN_CMD_IDENTIFY'

The list (docs/M12.7-PLAN.md, "The read-only guarantee"):
  stage 1  I/O: Read (02h) only.
           admin: Delete I/O SQ 00h, Create I/O SQ 01h, Get Log Page 02h,
           Delete I/O CQ 04h, Create I/O CQ 05h, Identify 06h, Abort 08h,
           Set Features 09h.
  stage 2  adds Flush (00h) and Write (01h) to the I/O list.
Any other opcode fails the check, with its line. It also fails if no
command was traced at all, or a line that names a command isn't in the
format above (QEMU's trace format changes between versions: a silent trace
must not pass), and with --require-io / --require-admin if an opcode the
test expects never appeared (the guest really did read). Prints a summary
of the opcodes seen.

--write-ranges (stage 2): every Write must lie inside one of the block
ranges given (b exclusive: a Jam OS drive's table and partitions,
tools/nvme-written.py --ranges). It reads QEMU's
    pci_nvme_write cid 3 opname 'NVME_NVM_CMD_WRITE' nsid 1 nlb 8 len 4096 lba 0x800
(`-trace pci_nvme_write`), which says where; it fails on a Write outside
them, on a Write line it can't parse, and when the Writes the I/O lines
count aren't as many as those lines (a trace that doesn't say where
mustn't pass). The trace doesn't say which drive a Write went to: the
test hashes every other drive's image as well.
Exit 0 if the trace is clean."""
import argparse
import re
import sys

ADMIN = {0x00: "Delete I/O SQ", 0x01: "Create I/O SQ", 0x02: "Get Log Page",
         0x04: "Delete I/O CQ", 0x05: "Create I/O CQ", 0x06: "Identify",
         0x08: "Abort", 0x09: "Set Features"}
IO = {1: {0x02: "Read"}, 2: {0x00: "Flush", 0x01: "Write", 0x02: "Read"}}
LINE = re.compile(r"pci_nvme_(admin|io)_cmd\b.*?\bopc 0x([0-9a-fA-F]+)")
WRITE = re.compile(r"\bpci_nvme_write\b.*?\bnlb (\d+)\b.*?\blba 0x([0-9a-fA-F]+)\b")


def ranges(s):
    out = []
    for r in s.split(","):
        a, b = r.split("-")
        out.append((int(a), int(b)))
    return out


def check_writes(path, allowed, io_writes):
    """Every pci_nvme_write line inside one of `allowed`; as many as the
    Write commands traced. Returns the failures."""
    bad, n = [], 0
    with open(path, errors="replace") as f:
        for k, line in enumerate(f, 1):
            if not re.search(r"\bpci_nvme_write\b", line):
                continue
            m = WRITE.search(line)
            if not m:
                bad.append("line %d: a Write line it can't parse: %s" % (k, line.strip()))
                continue
            n += 1
            nlb, lba = int(m.group(1)), int(m.group(2), 16)
            if not nlb or not any(a <= lba and lba + nlb <= b for a, b in allowed):
                bad.append("line %d: a Write of blocks %d..%d, outside every range given" %
                           (k, lba, lba + nlb - 1))
    if n != io_writes:
        bad.append("%d Write commands traced, but %d Write lines say where" % (io_writes, n))
    print("nvme-trace: %d Writes, each inside the ranges given" % n if not bad else
          "nvme-trace: Writes outside the ranges given")
    return bad


def hexlist(s):
    return [int(x, 16) for x in s.split(",") if x]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace")
    ap.add_argument("--stage", type=int, default=1, choices=[1, 2])
    ap.add_argument("--allow-admin", default="", help="extra admin opcodes, hex, comma-separated")
    ap.add_argument("--allow-io", default="", help="extra I/O opcodes, hex, comma-separated")
    ap.add_argument("--require-io", default="", help="I/O opcodes that must appear")
    ap.add_argument("--require-admin", default="", help="admin opcodes that must appear")
    ap.add_argument("--write-ranges", default="", help="blocks a Write may touch: a-b,c-d")
    a = ap.parse_args()
    admin_ok = set(ADMIN) | set(hexlist(a.allow_admin))
    io_ok = set(IO[a.stage]) | set(hexlist(a.allow_io))
    seen = {"admin": {}, "io": {}}
    bad = []
    with open(a.trace, errors="replace") as f:
        for n, line in enumerate(f, 1):
            if "pci_nvme_" not in line or "_cmd" not in line:
                continue
            m = LINE.search(line)
            if not m:
                if re.search(r"pci_nvme_(admin|io)_cmd", line):
                    bad.append((n, line.strip(), "unparsable"))
                continue
            kind, opc = m.group(1), int(m.group(2), 16)
            seen[kind][opc] = seen[kind].get(opc, 0) + 1
            if opc not in (admin_ok if kind == "admin" else io_ok):
                bad.append((n, line.strip(), "opcode 0x%02x is not allowed in stage %d" % (opc, a.stage)))
    total = sum(sum(v.values()) for v in seen.values())
    for kind, names in (("admin", ADMIN), ("io", IO[a.stage])):
        for opc, c in sorted(seen[kind].items()):
            print("nvme-trace: %-5s opcode 0x%02x %-14s %6d" % (kind, opc, names.get(opc, "?"), c))
    ok = True
    if total == 0:
        print("nvme-trace: FAILED: no NVMe command in the trace (the trace is off, or QEMU's "
              "event names changed)")
        ok = False
    for kind, req in (("io", a.require_io), ("admin", a.require_admin)):
        for opc in hexlist(req):
            if opc not in seen[kind]:
                print("nvme-trace: FAILED: no %s opcode 0x%02x in the trace" % (kind, opc))
                ok = False
    if a.write_ranges:
        for why in check_writes(a.trace, ranges(a.write_ranges), seen["io"].get(0x01, 0)):
            print("nvme-trace: FAILED: %s" % why)
            ok = False
    for n, line, why in bad[:20]:
        print("nvme-trace: FAILED: line %d: %s: %s" % (n, why, line))
        ok = False
    if len(bad) > 20:
        print("nvme-trace: ... and %d more" % (len(bad) - 20))
    print("nvme-trace: %d commands, %s" % (total, "clean" if ok else "NOT clean"))
    return 0 if ok else 1


sys.exit(main())
