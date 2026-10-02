#!/usr/bin/env python3
"""Network throughput between the Mac and Jam OS (the other end of
Jam OS's `speed`, bin/speed; user/apps/speed/speed.h has the protocol).
Plain Python 3, nothing to install.

    python3 tools/speed.py server [--port 5201]
        Wait for Jam OS's `speed <mac address>` (it sends), `speed ... -r`
        (this sends) and `speed ... -u` (UDP datagrams to here), on TCP and
        UDP port 5201, until Ctrl+C. Each test is one line here too.
    python3 tools/speed.py client <jam os address> [--port 5201] [-r] [-t seconds]
        Connect to Jam OS's `speed -l` and send for 5 s (-t); -r: Jam OS
        sends, this receives.

Speeds are MB/s (10^6 bytes a second) and Mbit/s, each side's own count:
the sender's is what went into its socket, the receiver's what arrived
from its first byte to the sender's end."""
import argparse
import os
import socket
import struct
import sys
import threading
import time

PORT = 5201
HELLO = struct.Struct("!4sBBHII")    # "JSPD", version, mode, 0, seconds, 0
REPORT = struct.Struct("!4sIQ")      # "JSPR", ms, bytes
UDP_DATA = struct.Struct("!4sIII")   # "JSPU", run, seq, 0
UDP_END = struct.Struct("!4sIII")    # "JSPE", run, sent, 0
UDP_REPORT = struct.Struct("!4sIIIQ")   # "JSPR", run, received, ms, bytes
CHUNK = 65536
MAX_SECONDS = 60


def rate(what, n, secs):
    secs = max(secs, 1e-6)
    return "speed.py: %s %.1f MB in %.2f s: %.1f MB/s, %.1f Mbit/s" % (
        what, n / 1e6, secs, n / 1e6 / secs, n * 8 / 1e6 / secs)


def recv_exact(s, n):
    out = b""
    while len(out) < n:
        b = s.recv(n - len(out))
        if not b:
            raise ConnectionError("the connection ended early")
        out += b
    return out


def send_for(s, seconds):
    """Bytes for `seconds`, our end, then the receiver's report."""
    buf = os.urandom(CHUNK)
    t0 = time.monotonic()
    end, sent = t0 + seconds, 0
    while time.monotonic() < end:
        sent += s.send(buf)
    t1 = time.monotonic()
    s.shutdown(socket.SHUT_WR)
    print(rate("sent", sent, t1 - t0), flush=True)
    tag, ms, got = REPORT.unpack(recv_exact(s, REPORT.size))
    if tag != b"JSPR":
        raise ValueError("not a report")
    print(rate("the other side got", got, ms / 1000.0), flush=True)


def receive_all(s):
    """Bytes to the sender's end, counted; our report back."""
    got, t0, t1 = 0, None, None
    s.settimeout(30)
    while True:
        b = s.recv(CHUNK * 4)
        if not b:
            break
        if t0 is None:
            t0 = time.monotonic()
        got += len(b)
        t1 = time.monotonic()
    t0 = t0 or time.monotonic()
    t1 = t1 or t0
    print(rate("received", got, t1 - t0), flush=True)
    s.sendall(REPORT.pack(b"JSPR", int((t1 - t0) * 1000), got))
    s.shutdown(socket.SHUT_WR)


def serve_tcp(conn, peer):
    with conn:
        try:
            conn.settimeout(10)
            tag, ver, mode, _, secs, _ = HELLO.unpack(recv_exact(conn, HELLO.size))
            if tag != b"JSPD" or ver != 1 or mode > 1 or not 1 <= secs <= MAX_SECONDS:
                print("speed.py: %s: not a speed test" % peer[0], flush=True)
                return
            print("speed.py: %s: %s for %d s over TCP" % (
                peer[0], "receiving" if mode == 0 else "sending", secs), flush=True)
            conn.settimeout(None)
            if mode == 0:
                receive_all(conn)
            else:
                send_for(conn, secs)
            conn.settimeout(10)
            while conn.recv(4096):   # to its end
                pass
        except (OSError, ValueError) as e:
            print("speed.py: %s: %s" % (peer[0], e), flush=True)


class UdpRuns:
    """UDP tests by run id: datagrams counted until the end comes."""

    def __init__(self):
        self.runs = {}   # run -> [count, bytes, first, last]

    def datagram(self, sock, data, peer):
        if len(data) >= UDP_DATA.size and data[:4] == b"JSPU":
            _, run, _, _ = UDP_DATA.unpack_from(data)
            r = self.runs.setdefault(run, [0, 0, time.monotonic(), 0.0])
            r[0] += 1
            r[1] += len(data)
            r[3] = time.monotonic()
        elif len(data) >= UDP_END.size and data[:4] == b"JSPE":
            _, run, sent, _ = UDP_END.unpack_from(data)
            r = self.runs.get(run, [0, 0, time.monotonic(), time.monotonic()])
            ms = int(max(r[3] - r[2], 0) * 1000)
            sock.sendto(UDP_REPORT.pack(b"JSPR", run, r[0], ms, r[1]), peer)
            if run in self.runs:
                print("speed.py: %s: UDP: %d of %d datagrams arrived (%d lost); %s" % (
                    peer[0], r[0], sent, max(sent - r[0], 0),
                    rate("received", r[1], ms / 1000.0)[len("speed.py: "):]), flush=True)
                del self.runs[run]


def server(port):
    t = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    t.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    t.bind(("0.0.0.0", port))
    t.listen(8)
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
    u.bind(("0.0.0.0", port))
    runs = UdpRuns()

    def udp_loop():
        while True:
            data, peer = u.recvfrom(65536)
            runs.datagram(u, data, peer)

    threading.Thread(target=udp_loop, daemon=True).start()
    print("speed.py: waiting on TCP and UDP port %d (Jam OS: speed <this Mac's address> "
          "[-r | -u]); Ctrl+C stops it" % port, flush=True)
    while True:
        conn, peer = t.accept()
        threading.Thread(target=serve_tcp, args=(conn, peer), daemon=True).start()


def client(host, port, receive, seconds):
    s = socket.create_connection((host, port), timeout=10)
    s.settimeout(None)
    print("speed.py: connected to %s port %d: %s for %d s over TCP" % (
        host, port, "receiving" if receive else "sending", seconds), flush=True)
    s.sendall(HELLO.pack(b"JSPD", 1, 1 if receive else 0, 0, seconds, 0))
    if receive:
        receive_all(s)
    else:
        send_for(s, seconds)
    s.settimeout(10)
    while s.recv(4096):
        pass
    s.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sv = sub.add_parser("server", help="wait for Jam OS's speed")
    sv.add_argument("--port", type=int, default=PORT)
    cl = sub.add_parser("client", help="test against Jam OS's speed -l")
    cl.add_argument("host")
    cl.add_argument("--port", type=int, default=PORT)
    cl.add_argument("-r", action="store_true", help="Jam OS sends, this receives")
    cl.add_argument("-t", type=int, default=5, help="seconds (1..60)")
    a = ap.parse_args()
    try:
        if a.cmd == "server":
            server(a.port)
        else:
            if not 1 <= a.t <= MAX_SECONDS:
                ap.error("-t: 1..60")
            client(a.host, a.port, a.r, a.t)
    except KeyboardInterrupt:
        return 0
    except (OSError, ValueError, ConnectionError) as e:
        print("speed.py: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
