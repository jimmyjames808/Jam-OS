#!/usr/bin/env python3
"""Check a pcap of the guest's frames: every one tagged 802.1Q with the
VLAN (docs/M9-PLAN.md "Testing without the real NIC").

QEMU's `-object filter-dump,...,queue=rx` on the NIC's netdev writes
every frame the guest's NIC sends to a pcap file (tools/qemu-test.sh does
it when QEMU_NET is set). This reads that file on its own, sharing no code
with tools/netpeer.py, so the rule has two independent checks: a frame
fails if it is a runt (under 18 bytes), untagged, tagged with another
TPID (QinQ 0x88a8, 0x9100), priority-tagged (VLAN 0), tagged with
another VLAN, or longer than 1518 bytes.

    pcap-vlan-check.py [--vlan N] [--expect-none] [--min N]
                       [--exclude-src MAC] file.pcap
    pcap-vlan-check.py --selftest

--expect-none: the file must have no frame at all (the vlan=off run).
--min N: at least N frames (a run that should have sent something).
--exclude-src: ignore frames from this source MAC (a dump that has both
directions: the peer's own frames). Prints one line per bad frame (the
first 8) and a summary line ending PASS or FAIL; exits 0 on PASS."""
import argparse
import os
import struct
import sys
import tempfile

LINKTYPE_ETHERNET = 1
MAGICS = {0xA1B2C3D4: "<", 0xD4C3B2A1: ">", 0xA1B23C4D: "<", 0x4D3CB2A1: ">"}


def frames(path):
    """The frames of a pcap file (raises ValueError if it isn't one of
    Ethernet frames)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 24:
        raise ValueError("too short for a pcap header")
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic not in MAGICS:
        raise ValueError("not a pcap file (magic %08x)" % magic)
    e = MAGICS[magic]
    linktype = struct.unpack_from(e + "I", data, 20)[0]
    if linktype != LINKTYPE_ETHERNET:
        raise ValueError("link type %d, not Ethernet" % linktype)
    out, off = [], 24
    while off + 16 <= len(data):
        incl, orig = struct.unpack_from(e + "II", data, off + 8)
        off += 16
        if off + incl > len(data):
            raise ValueError("a record runs past the end of the file")
        if incl != orig:
            raise ValueError("a frame was cut short in the capture (%d of %d)" % (incl, orig))
        out.append(data[off:off + incl])
        off += incl
    if off != len(data):
        raise ValueError("%d stray bytes at the end" % (len(data) - off))
    return out


def problem(frame, vlan):
    """Why frame breaks the rule, or None."""
    if len(frame) < 18:
        return "runt (%d bytes)" % len(frame)
    if len(frame) > 1518:
        return "too long (%d bytes)" % len(frame)
    tpid, tci = struct.unpack_from(">HH", frame, 12)
    if tpid != 0x8100:
        if tpid in (0x88A8, 0x9100):
            return "outer tag %04x" % tpid
        return "untagged (EtherType %04x)" % tpid
    vid = tci & 0x0FFF
    if vid != vlan:
        return "priority-tagged (VLAN 0)" if vid == 0 else "VLAN %d" % vid
    return None


def check(path, vlan=21, expect_none=False, minimum=0, exclude=None, out=sys.stdout):
    """True if the pcap at path keeps the rule (and the counts asked for)."""
    try:
        fs = frames(path)
    except (OSError, ValueError) as err:
        print("pcap-vlan-check: %s: %s: FAIL" % (path, err), file=out)
        return False
    if exclude:
        fs = [f for f in fs if f[6:12] != exclude]
    bad = 0
    for i, f in enumerate(fs):
        why = problem(f, vlan)
        if why:
            bad += 1
            if bad <= 8:
                print("pcap-vlan-check: frame %d: %s: %s" % (i, why, f[:32].hex()), file=out)
    ok = bad == 0 and not (expect_none and fs) and len(fs) >= minimum
    extra = ""
    if expect_none and fs:
        extra = " (none expected)"
    elif len(fs) < minimum:
        extra = " (at least %d expected)" % minimum
    print("pcap-vlan-check: %s: %d frames, %d not tagged %d%s: %s" %
          (os.path.basename(path), len(fs), bad, vlan, extra, "PASS" if ok else "FAIL"), file=out)
    return ok


def write_pcap(path, fs):
    with open(path, "wb") as f:
        f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, LINKTYPE_ETHERNET))
        for i, fr in enumerate(fs):
            f.write(struct.pack("<IIII", i, 0, len(fr), len(fr)) + fr)


def selftest():
    hdr = b"\xff" * 6 + bytes.fromhex("525400123456")
    arp = b"\x08\x06" + b"\0" * 28
    good = hdr + b"\x81\x00\x00\x15" + arp
    cases = [
        ([good, good], {}, True),
        ([], {"expect_none": True}, True),
        ([good], {"expect_none": True}, False),
        ([], {"minimum": 1}, False),
        ([good, hdr + arp], {}, False),                                  # untagged
        ([hdr + b"\x81\x00\x00\x0a" + arp], {}, False),                  # VLAN 10
        ([hdr + b"\x81\x00\xa0\x00" + arp], {}, False),                  # VLAN 0
        ([hdr + b"\x88\xa8\x00\x15\x81\x00\x00\x15" + arp], {}, False),  # QinQ
        ([good[:16]], {}, False),                                        # runt
        ([good + b"\0" * 1500], {}, False),                              # too long
        ([hdr + b"\x81\x00\x00\x16" + arp], {"vlan": 22}, True),
        ([hdr + arp, good], {"exclude": hdr[6:12]}, True),
    ]
    fails = 0
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "t.pcap")
        with open(os.devnull, "w") as null:
            for i, (fs, kw, want) in enumerate(cases):
                write_pcap(p, fs)
                if check(p, out=null, **kw) != want:
                    print("pcap-vlan-check selftest: case %d: want %s" % (i, want))
                    fails += 1
            with open(p, "wb") as f:
                f.write(b"not a pcap at all, really")
            if check(p, out=null):
                print("pcap-vlan-check selftest: a file that isn't a pcap passed")
                fails += 1
    print("pcap-vlan-check selftest: %s" % ("PASS" if not fails else "FAIL"))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pcap", nargs="?")
    ap.add_argument("--vlan", type=int, default=21)
    ap.add_argument("--expect-none", action="store_true")
    ap.add_argument("--min", type=int, default=0)
    ap.add_argument("--exclude-src")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.pcap:
        ap.error("a pcap file is needed")
    exclude = bytes.fromhex(a.exclude_src.replace(":", "")) if a.exclude_src else None
    return 0 if check(a.pcap, a.vlan, a.expect_none, a.min, exclude) else 1


if __name__ == "__main__":
    sys.exit(main())
