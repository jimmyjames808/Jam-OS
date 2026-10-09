#!/usr/bin/env python3
"""Check QEMU's trace of every NVMe command against the stage's opcode list.

    nvme-trace-check.py [--stage 1|2] [--allow-admin 0a,0c] [--allow-io ..]
                        [--require-io 02] [--require-admin 06] <trace file>

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
of the opcodes seen. Exit 0 if the trace is clean."""
import argparse
import re
import sys

ADMIN = {0x00: "Delete I/O SQ", 0x01: "Create I/O SQ", 0x02: "Get Log Page",
         0x04: "Delete I/O CQ", 0x05: "Create I/O CQ", 0x06: "Identify",
         0x08: "Abort", 0x09: "Set Features"}
IO = {1: {0x02: "Read"}, 2: {0x00: "Flush", 0x01: "Write", 0x02: "Read"}}
LINE = re.compile(r"pci_nvme_(admin|io)_cmd\b.*?\bopc 0x([0-9a-fA-F]+)")


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
    for n, line, why in bad[:20]:
        print("nvme-trace: FAILED: line %d: %s: %s" % (n, why, line))
        ok = False
    if len(bad) > 20:
        print("nvme-trace: ... and %d more" % (len(bad) - 20))
    print("nvme-trace: %d commands, %s" % (total, "clean" if ok else "NOT clean"))
    return 0 if ok else 1


sys.exit(main())
