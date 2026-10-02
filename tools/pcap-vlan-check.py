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
    pcap-vlan-check.py [--vlan N] --pc MAC file.pcap
    pcap-vlan-check.py --selftest

--expect-none: the file must have no frame at all (the vlan=off run).
--min N: at least N frames (a run that should have sent something).
--exclude-src: ignore frames from this source MAC (a dump that has both
directions: the peer's own frames). Prints one line per bad frame (the
first 8) and a summary line ending PASS or FAIL; exits 0 on PASS.

--pc MAC: a capture taken on the Mac's end of a cable straight to the PC
(docs/M9-PLAN.md, "R1 progress"; pcap or pcapng). Every frame from the
PC's MAC must keep the rule and be sane (padded to 64 bytes, a well-formed
ARP, IPv4 and ICMP checksums right); it also counts the PC's ARP probes
and the replies to them, the Mac's pings and which the PC answered (a
+/- pattern and the missing sequence numbers), and says whether the Mac's
own frames show their tags (if none does, the capture can't see tags).
PASS needs at least one frame from the PC and none bad."""
import argparse
import os
import struct
import sys
import tempfile

LINKTYPE_ETHERNET = 1
MAGICS = {0xA1B2C3D4: "<", 0xD4C3B2A1: ">", 0xA1B23C4D: "<", 0x4D3CB2A1: ">"}
PCAPNG_SHB = 0x0A0D0D0A


def frames(path):
    """The frames of a pcap file (raises ValueError if it isn't one of
    Ethernet frames)."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 24:
        raise ValueError("too short for a pcap header")
    if struct.unpack_from("<I", data, 0)[0] == PCAPNG_SHB:
        return frames_pcapng(data)
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


def frames_pcapng(data):
    """The frames of a pcapng file (macOS's tcpdump writes one with -P):
    its enhanced and simple packet blocks, every interface Ethernet."""
    e = {0x1A2B3C4D: "<", 0x4D3C2B1A: ">"}.get(struct.unpack_from("<I", data, 8)[0])
    if not e:
        raise ValueError("a pcapng file with a bad byte-order magic")
    out, off, links = [], 0, []
    while off + 12 <= len(data):
        btype, blen = struct.unpack_from(e + "II", data, off)
        if blen < 12 or blen % 4 or off + blen > len(data):
            raise ValueError("a pcapng block of %d bytes at %d" % (blen, off))
        body = data[off + 8:off + blen - 4]
        if btype == 1:                                   # interface description
            links.append(struct.unpack_from(e + "H", body, 0)[0])
        elif btype == 6:                                 # enhanced packet
            ifid, _, _, incl, orig = struct.unpack_from(e + "IIIII", body, 0)
            if ifid >= len(links) or links[ifid] != LINKTYPE_ETHERNET:
                raise ValueError("a packet on an interface that isn't Ethernet")
            if incl != orig:
                raise ValueError("a frame was cut short in the capture (%d of %d)" % (incl, orig))
            out.append(body[20:20 + incl])
        elif btype == 3:                                 # simple packet
            orig = struct.unpack_from(e + "I", body, 0)[0]
            out.append(body[4:4 + orig])
        off += blen
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


# ---- --pc: a capture from the Mac's end of a direct cable to the PC -------------


def be16(b, off):
    return struct.unpack_from(">H", b, off)[0]


def csum(b):
    """The Internet checksum of b (0 when b holds its own correct one)."""
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack(">%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def inner(frame):
    """(EtherType, payload) of a frame, past one 802.1Q tag if it has one."""
    if len(frame) >= 18 and be16(frame, 12) == 0x8100:
        return be16(frame, 16), frame[18:]
    return (be16(frame, 12), frame[14:]) if len(frame) >= 14 else (0, b"")


def garbled(frame):
    """Why a frame from the PC that kept the tag rule is not a sane frame,
    or None: the driver pads every frame to 64 bytes (60 without the FCS a
    capture leaves off); its ARP must be well formed and from its own MAC;
    an IPv4 header must check out, and an ICMP message too."""
    if len(frame) < 60:
        return "short (%d bytes, the driver pads to 64)" % len(frame)
    et, p = inner(frame)
    if et == 0x0806:
        if (len(p) < 28 or be16(p, 0) != 1 or be16(p, 2) != 0x0800 or p[4] != 6 or
                p[5] != 4 or be16(p, 6) not in (1, 2) or p[8:14] != frame[6:12]):
            return "a malformed ARP: %s" % p[:28].hex()
        return None
    if et != 0x0800:
        return None
    if len(p) < 20 or p[0] >> 4 != 4 or (p[0] & 15) < 5:
        return "a malformed IPv4 header"
    hl, total = (p[0] & 15) * 4, be16(p, 2)
    if total < hl or total > len(p) or csum(p[:hl]):
        return "an IPv4 header with a bad length or checksum"
    if p[9] == 1 and csum(p[hl:total]):
        return "an ICMP message with a bad checksum"
    return None


def pc_report(fs, pc, vlan, out):
    """The direct-cable capture: every frame from the PC's MAC `pc` must be
    tagged `vlan` and sane; pings to it and its ARP probes are counted, and
    the Mac's own frames say whether the capture shows tags at all."""
    bad = odd = probes = probe_replies = 0
    mine_tagged = mine_untagged = 0
    asked, answered, order = {}, set(), []
    for i, f in enumerate(fs):
        if len(f) < 14:
            continue
        et, p = inner(f)
        if f[6:12] == pc:
            rule = problem(f, vlan)
            why = rule or garbled(f)
            bad += rule is not None
            odd += rule is None and why is not None
            if why and bad + odd <= 8:
                print("pcap-vlan-check: frame %d from the PC: %s: %s" % (i, why, f[:48].hex()),
                      file=out)
            if et == 0x0806 and len(p) >= 28 and be16(p, 6) == 1 and p[14:18] == b"\0" * 4:
                probes += 1
            if et == 0x0800 and len(p) >= 28 and p[9] == 1 and p[(p[0] & 15) * 4] == 0:
                hl = (p[0] & 15) * 4
                answered.add((p[16:20], be16(p, hl + 4), be16(p, hl + 6)))
            continue
        mine_tagged += be16(f, 12) == 0x8100
        mine_untagged += be16(f, 12) != 0x8100
        if et == 0x0806 and len(p) >= 28 and be16(p, 6) == 2 and p[18:24] == pc:
            probe_replies += 1
        if et == 0x0800 and f[0:6] == pc and len(p) >= 28 and p[9] == 1:
            hl = (p[0] & 15) * 4
            if p[hl] == 8:
                key = (p[12:16], be16(p, hl + 4), be16(p, hl + 6))
                if key not in asked:
                    asked[key] = len(order)
                    order.append(key)
    pattern = "".join("+" if k in answered else "-" for k in order)
    missing = [str(k[2]) for k in order if k not in answered]
    print("pcap-vlan-check: from the PC: %d frame(s), %d NOT tagged %d, %d garbled; ARP probes "
          "%d, replies to it %d" % (sum(f[6:12] == pc for f in fs), bad, vlan, odd, probes,
                                    probe_replies), file=out)
    print("pcap-vlan-check: pings to the PC: %d, answered %d %s%s" %
          (len(order), len(order) - len(missing), pattern[:100],
           "; no reply to seq " + " ".join(missing[:30]) if missing else ""), file=out)
    if mine_untagged and not mine_tagged:
        print("pcap-vlan-check: WARNING: none of the Mac's own %d frame(s) shows a tag: this "
              "capture may not show tags at all (the adapter adds them)" % mine_untagged, file=out)
    else:
        print("pcap-vlan-check: the Mac's own frames: %d tagged, %d untagged" %
              (mine_tagged, mine_untagged), file=out)
    return bad == 0 and odd == 0


def check_pc(path, pc, vlan=21, out=sys.stdout):
    """True if every frame from `pc` in the capture at path is tagged and sane."""
    try:
        fs = frames(path)
    except (OSError, ValueError) as err:
        print("pcap-vlan-check: %s: %s: FAIL" % (path, err), file=out)
        return False
    ok = pc_report(fs, pc, vlan, out)
    ok = ok and any(f[6:12] == pc for f in fs)
    print("pcap-vlan-check: %s: %d frames, %s" % (os.path.basename(path), len(fs),
                                                 "PASS" if ok else "FAIL"), file=out)
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
            fails += selftest_pc(d, null)
    print("pcap-vlan-check selftest: %s" % ("PASS" if not fails else "FAIL"))
    return 1 if fails else 0


def ipv4_icmp(src, dst, icmp_type, ident, seq):
    """An IPv4 ICMP echo (request 8, reply 0) with correct checksums."""
    icmp = struct.pack(">BBHHH", icmp_type, 0, 0, ident, seq) + b"x" * 32
    icmp = icmp[:2] + struct.pack(">H", csum(icmp)) + icmp[4:]
    ip = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + len(icmp), 1, 0, 64, 1, 0, src, dst)
    return ip[:10] + struct.pack(">H", csum(ip)) + ip[12:] + icmp


def write_pcapng(path, fs):
    def block(btype, body):
        body += b"\0" * (-len(body) % 4)
        n = len(body) + 12
        return struct.pack("<II", btype, n) + body + struct.pack("<I", n)
    with open(path, "wb") as f:
        f.write(block(PCAPNG_SHB, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1)))
        f.write(block(1, struct.pack("<HHI", LINKTYPE_ETHERNET, 0, 65535)))
        for fr in fs:
            f.write(block(6, struct.pack("<IIIII", 0, 0, 0, len(fr), len(fr)) + fr))


def selftest_pc(d, null):
    """--pc over hand-made captures; returns the number of failures."""
    pc, mac = bytes.fromhex("020000000001"), bytes.fromhex("020000000002")
    tag = b"\x81\x00\x00\x15"
    ip_pc, ip_mac = bytes([10, 2, 21, 240]), bytes([10, 2, 21, 1])
    probe = (b"\xff" * 6 + pc + tag + b"\x08\x06" + struct.pack(">HHBBH", 1, 0x0800, 6, 4, 1) +
             pc + b"\0" * 4 + b"\0" * 6 + ip_mac).ljust(64, b"\0")
    reply = (pc + mac + tag + b"\x08\x06" + struct.pack(">HHBBH", 1, 0x0800, 6, 4, 2) + mac +
             ip_mac + pc + b"\0" * 4).ljust(64, b"\0")
    ask = pc + mac + tag + b"\x08\x00" + ipv4_icmp(ip_mac, ip_pc, 8, 7, 1)    # Mac -> PC
    ans = mac + pc + tag + b"\x08\x00" + ipv4_icmp(ip_pc, ip_mac, 0, 7, 1)    # PC -> Mac
    ask2 = pc + mac + tag + b"\x08\x00" + ipv4_icmp(ip_mac, ip_pc, 8, 7, 2)   # no answer
    badsum = bytearray(ans)
    badsum[18 + 10] ^= 1                          # the IPv4 header checksum
    cases = [
        ([probe, reply, ask, ans, ask2], True),
        ([probe, probe[:12] + b"\x81\x00\x00\x0a" + probe[16:]], False),   # VLAN 10
        ([probe, probe[:12] + probe[16:]], False),   # untagged from the PC
        ([probe[:50]], False),                       # short: not padded
        ([bytes(badsum)], False),
        ([reply, ask], False),                       # nothing from the PC
    ]
    fails = 0
    p = os.path.join(d, "pc.pcap")
    for i, (fs, want) in enumerate(cases):
        write_pcap(p, fs)
        if check_pc(p, pc, out=null) != want:
            print("pcap-vlan-check selftest: --pc case %d: want %s" % (i, want))
            fails += 1
    write_pcapng(p, cases[0][0])
    if not check_pc(p, pc, out=null):
        print("pcap-vlan-check selftest: --pc over pcapng failed")
        fails += 1
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("pcap", nargs="?")
    ap.add_argument("--vlan", type=int, default=21)
    ap.add_argument("--expect-none", action="store_true")
    ap.add_argument("--min", type=int, default=0)
    ap.add_argument("--exclude-src")
    ap.add_argument("--pc")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not a.pcap:
        ap.error("a pcap file is needed")
    if a.pc:
        return 0 if check_pc(a.pcap, bytes.fromhex(a.pc.replace(":", "")), a.vlan) else 1
    exclude = bytes.fromhex(a.exclude_src.replace(":", "")) if a.exclude_src else None
    return 0 if check(a.pcap, a.vlan, a.expect_none, a.min, exclude) else 1


if __name__ == "__main__":
    sys.exit(main())
