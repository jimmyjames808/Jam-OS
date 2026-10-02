#!/bin/sh
# netlog end to end in QEMU (tools/shell-tests/netlog.txt): QEMU's e1000e,
# net.address and net.host = 10.2.21.174 in a copy of the image's
# settings, and tools/netpeer.py hosting tools/netlog-recv.py's receiver
# (--netlog), started 5 s late (--netlog-late) and paused for 6 s once it
# has 6000 bytes (--netlog-pause). One run, two boots: netlog killed and
# restarted, netstack killed and restarted, then a panic (`crash panic
# yes`) and the next boot by kexec. The run must pass the script and the
# peer's and the pcap's VLAN checks (tools/qemu-test.sh: every frame
# tagged 21), and then the receiver's files are compared with the serial
# log:
#   - three files: each boot's log and the panicked boot's (-lastcrash);
#   - each boot's file is that boot's kernel log from its first line, line
#     for line, as far as it goes, with no gap and nothing lost (no
#     receiver note); the first boot's reaches past netstack's restart;
#   - the -lastcrash file overlaps the first boot's and has the panic;
#   - netlog's own lines are a handful (state changes, not one per
#     datagram), and the receiver dropped datagrams (late and paused).
# Usage: tools/netlog-test.sh <outdir>
set -eu
out=$1
mkdir -p "$out"
rm -rf "$out/netlog"
img="$out/netlog.base.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\nnet.host = 10.2.21.174\n' \
    > "$out/netlog.settings"
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/netlog.settings" ::/etc/settings
ok=1
QEMU_IMAGE="$img" QEMU_NET=1 QEMU_INPUT=tools/shell-tests/netlog.txt \
    QEMU_NET_PEER="--netlog $out/netlog --netlog-late 5 --netlog-pause 6000:6" \
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-240} tools/qemu-test.sh "$out" netlog shell || ok=0
python3 - "$out" <<'EOF' || ok=0
import json, os, re, sys
out = sys.argv[1]
folder = os.path.join(out, "netlog")
fails = []

def check(what, cond):
    print("netlog-test: %s: %s" % (what, "ok" if cond else "FAILED"))
    if not cond:
        fails.append(what)

ts = re.compile(r"^\[ *\d+\.\d{6}\] ")
esc = re.compile(r"\x1b\[[0-9;=?]*[A-Za-z]")
serial = esc.sub("", open(os.path.join(out, "netlog.log"), encoding="utf-8",
                          errors="replace").read()).replace("\r", "")
boots, cur = [], None
for line in serial.split("\n"):
    if not ts.match(line):
        continue
    if "] Jam OS 0." in line and (cur is None or len(cur) > 1):
        cur = []
        boots.append(cur)
    if cur is not None:
        cur.append(line)
check("two boots on the serial port", len(boots) == 2)
names = sorted(n for n in os.listdir(folder) if n.endswith(".txt")) if os.path.isdir(folder) else []
check("three files from the receiver (%s)" % ", ".join(names), len(names) == 3)
text = {n: open(os.path.join(folder, n), encoding="utf-8", errors="replace").read()
        for n in names}
crash = [n for n in names if n.endswith("-lastcrash.txt")]
lives = [n for n in names if n not in crash]
if len(boots) == 2 and len(crash) == 1 and len(lives) == 2:
    # the second boot's file is the one that says the last boot panicked
    second = [n for n in lives if "the last boot panicked" in text[n]]
    first = [n for n in lives if n not in second]
    check("one file for each boot", len(first) == 1 and len(second) == 1)
    for name, boot, what in ((first[0], boots[0], "the first boot"),
                             (second[0], boots[1], "the second boot")):
        got = text[name].split("\n")[:-1]
        check("%s's file starts at its first line" % what,
              bool(got) and got[0] == boot[0] and "] Jam OS 0." in got[0])
        # The serial copy is the reference, but the shell's echo can land
        # in the middle of a kernel line there: every serial line up to the
        # file's last must be in the file, in order, and every line of the
        # file must be somewhere in the serial text.
        k = 0
        for line in got:
            if k < len(boot) and boot[k] == line:
                k += 1
        last = boot.index(got[-1]) + 1 if got and got[-1] in boot else len(boot)
        check("%s's file is its log, line for line (%d lines; %d of the serial's %d)" %
              (what, len(got), k, len(boot)),
              k >= last and all(l in serial for l in got))
        check("%s's file has no receiver note (no gap, nothing lost)" % what,
              "[netlog-recv:" not in text[name])
        own = [l for l in got if "] netlog: " in l]
        check("%s: netlog said %d lines (state changes only)" % (what, len(own)), len(own) <= 10)
    got1 = text[first[0]].split("\n")[:-1]
    check("the first boot's file reaches past netstack's restart",
          any("netlog: netstack has gone" in l for l in got1) and
          sum("netstack: address 10.2.21.5/24" in l for l in got1) >= 2)
    check("... and has both late and paused receivers' lines",
          sum("the Mac answers again" in l for l in got1) == 2)
    c = text[crash[0]]
    check("the -lastcrash file has the panic", "JAM OS KERNEL PANIC" in c)
    check("... and the first boot's lines", got1[-1] in c.split("\n") and got1[0] in c.split("\n"))
    check("the second boot's file says the last boot's log is sent",
          "netlog: the last boot's log is sent" in text[second[0]])
peer = json.load(open(os.path.join(out, "netlog.peer.json")))
check("the receiver dropped datagrams (late, then paused): %d of %d" %
      (peer.get("netlog_dropped", 0), peer.get("netlog_in", 0)),
      peer.get("netlog_dropped", 0) > 0 and peer.get("netlog_in", 0) > 0)
# a sender whose own sends made log lines would never stop sending
check("netlog went quiet: %d datagrams in all" % peer.get("netlog_in", 0),
      peer.get("netlog_in", 0) < 400)
# The Mac's own command line: every netlog datagram of the run (from the
# pcap), sent as they were to `netlog-recv.py --quiet FOLDER` on a port of
# 127.0.0.1, gives the same files.
import socket, struct, subprocess, time
pcap = open(os.path.join(out, "netlog.pcap"), "rb").read()
dgrams, at = [], 24
while at + 16 <= len(pcap):
    n = struct.unpack_from("<I", pcap, at + 8)[0]
    f = pcap[at + 16:at + 16 + n]
    at += 16 + n
    if len(f) < 46 or f[12:14] != b"\x81\x00" or f[16:18] != b"\x08\x00" or f[27] != 17:
        continue
    ip = f[18:]
    udp = ip[(ip[0] & 15) * 4:]
    if struct.unpack_from("!H", udp, 2)[0] == 5021:
        dgrams.append(udp[8:struct.unpack_from("!H", udp, 4)[0]])
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", 0))
port = s.getsockname()[1]
s.close()
mac = os.path.join(out, "netlog-mac")
subprocess.run(["rm", "-rf", mac])
rx = subprocess.Popen([sys.executable, "tools/netlog-recv.py", "--quiet", "--bind", "127.0.0.1",
                       "--port", str(port), mac], stderr=subprocess.DEVNULL)
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(2)
acks = 0
time.sleep(0.5)
for d in dgrams:
    s.sendto(d, ("127.0.0.1", port))
    try:
        acks += len(s.recv(64)) == 24
    except socket.timeout:
        pass
rx.send_signal(2)
rx.wait(5)
same = sorted(os.listdir(mac)) == sorted(os.listdir(folder)) and all(
    open(os.path.join(mac, n), "rb").read() == open(os.path.join(folder, n), "rb").read()
    for n in names)
check("netlog-recv.py on its own, fed the run's %d datagrams: %d acks, the same files" %
      (len(dgrams), acks), dgrams and acks == len(dgrams) and same)
sys.exit(1 if fails else 0)
EOF
if [ $ok = 1 ]; then
    echo "netlog-test: PASS"
else
    echo "netlog-test: FAIL"
    exit 1
fi
