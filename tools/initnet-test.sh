#!/bin/sh
# The regression run (`init`, init.cfg's programs) with the network: its log
# reaches the Mac (user/services/init/regnet.c). QEMU's e1000e and the
# peer of tools/netpeer.py answering netlog (port 5021) into a folder, the
# stick's settings with net.host = 10.2.21.174 and, for `static`,
# net.address (for `dhcp` the peer's DHCP server gives the address). The
# plain `init` boot runs utest, usbtest and posixtest as always, with
# netstack, the DHCP client, logd and netlog alongside; the run must pass
# QEMU's own checks (run complete: no problems, so the root job is clean
# after the network services were stopped; every frame tagged VLAN 21), and
# then:
#   - one file from the receiver, from the boot's first line, line for
#     line what the serial port shows up to "init: the run is over" (init's
#     last line before netlog flushes: the results of every program, the
#     stop of volumes), the line itself included, with no gap and no
#     receiver note;
#   - the file has every program's result line ("init: bin/...  exited with
#     code 0") and utest's summary;
#   - the stick's /data/logs/boot-NNNN.txt (logd, QEMU_SAVE) starts at the
#     boot's first line, has the same lines in order up to the last
#     program's result, and has no lines the serial port lacks;
#   - the kernel's own end-of-run lines ("init: exited with code", the RESULTS
#     box) are NOT in the file: the root job must be empty before the kernel
#     prints them (docs/TESTING.md, "The regression run's network");
#   - netlog said a handful of lines of its own.
# And a plain run is unchanged: no network without net.host (the same
# QEMU_NET run with settings that lack it starts no netstack, no netlog and
# no logd, and says why in one line).
# Usage: tools/initnet-test.sh <outdir> [static|dhcp|nohost|off]   (static)
set -eu
out=$1
mode=${2:-static}
mkdir -p "$out"
name=initnet-$mode
rm -rf "$out/$name.recv" "$out/$name.stick.img"
img="$out/$name.base.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
case $mode in
static) printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\nnet.host = 10.2.21.174\n' ;;
dhcp) printf 'net.host = 10.2.21.174\n' ;;
nohost) printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\n' ;;
off) printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\nnet.host = 10.2.21.174\nnetlog = off\n' ;;
*) echo "initnet-test: want static, dhcp, nohost or off"; exit 2 ;;
esac > "$out/$name.settings"
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/$name.settings" ::/etc/settings
ok=1
QEMU_IMAGE="$img" QEMU_NET=1 QEMU_SAVE="$out/$name.stick.img" \
    QEMU_NET_PEER="--netlog $out/$name.recv" QEMU_TIMEOUT=${QEMU_TIMEOUT:-400} \
    tools/qemu-test.sh "$out" "$name" init > "$out/$name.out" 2>&1 || ok=0
cat "$out/$name.out"
rm -f "$img"
python3 - "$out" "$name" "$mode" <<'EOF' || ok=0
import json, os, re, subprocess, sys
out, name, mode = sys.argv[1:4]
fails = []

def check(what, cond):
    print("initnet-test: %s: %s" % (what, "ok" if cond else "FAILED"))
    if not cond:
        fails.append(what)

ts = re.compile(r"^\[ *\d+\.\d{6}\] ")
esc = re.compile(r"\x1b\[[0-9;=?]*[A-Za-z]")
text = esc.sub("", open(os.path.join(out, name + ".log"), encoding="utf-8",
                        errors="replace").read()).replace("\r", "")
serial = [l for l in text.split("\n") if ts.match(l)]
check("a boot on the serial port", bool(serial) and "] Jam OS 0." in serial[0])
folder = os.path.join(out, name + ".recv")
files = sorted(n for n in os.listdir(folder) if n.endswith(".txt")) if os.path.isdir(folder) else []
peer = json.load(open(os.path.join(out, name + ".peer.json")))

def lines_with(s):
    return [i for i, l in enumerate(serial) if s in l]

if mode in ("nohost", "off"):
    # nothing changes: no network service, no file, the reason in one line
    want = ("no net.host in the settings" if mode == "nohost"
            else "netlog = off in the settings")
    check("one line says why there is no log on the Mac (%s)" % want, len(lines_with(want)) == 1)
    # (utest starts a netstack and a logd of its own for its tests: look for init's and netlog's)
    check("init started no network service, and netlog sent nothing", not any(
        ("alongside this run's programs" in l or "netlog: sending" in l) for l in serial))
    check("nothing reached the receiver", not files and peer.get("netlog_in", 0) == 0)
    sys.exit(1 if fails else 0)

done = lines_with("init: the run is over")
check("init said the run is over once", len(done) == 1)
check("the run ended with no problems", bool(lines_with("run complete: no problems")))
check("the root job was clean after the network services stopped",
      any("init: root job afterwards" in l and "(clean)" in l for l in serial))
check("init started the network and netlog", bool(lines_with("alongside this run's programs"))
      and bool(lines_with("netlog: sending this boot's log")))
if mode == "static":
    check("netstack took the static address", bool(lines_with("netstack: address 10.2.21.5/24")))
else:
    check("the DHCP client got a lease", bool(lines_with("dhcp: ")) and
          bool(lines_with("netstack: address 10.2.21.")))
check("one file from the receiver (%s)" % ", ".join(files), len(files) == 1)
if len(files) == 1 and len(done) == 1:
    got = open(os.path.join(folder, files[0]), encoding="utf-8",
               errors="replace").read().split("\n")[:-1]
    ref = serial[:done[0] + 1]
    check("the file starts at the boot's first line", bool(got) and got[0] == serial[0])
    k = 0
    for l in got:
        if k < len(ref) and ref[k] == l:
            k += 1
    check("every line up to 'the run is over' is in the file, in order (%d of %d)" %
          (k, len(ref)), k == len(ref))
    check("every line of the file is on the serial port", all(l in set(serial) for l in got))
    check("no receiver note (no gap, nothing lost)", not any("[netlog-recv:" in l for l in got))
    results = [l for l in serial[:done[0]] if re.search(r"init: bin/\S+ exited with code", l)]
    check("%d program result lines, all in the file" % len(results),
          len(results) >= 3 and all(l in got for l in results))
    check("utest's summary is in the file",
          any("utest:" in l and "passed" in l for l in got))
    check("volumes' stop line is in the file", any("init: volumes exited with code 0" in l for l in got))
    check("the kernel's own end-of-run lines are not (the root job must be empty first)",
          not any("init: exited with code" in l for l in got))
    own = [l for l in got if "] netlog: " in l]
    check("netlog said %d lines of its own (state changes only)" % len(own), len(own) <= 6)
    check("netlog sent %d datagrams (live: they follow the log as it grows)" %
          peer.get("netlog_in", 0), 0 < peer.get("netlog_in", 0) < 5000)
    # logd's file on the stick
    stick = os.path.join(out, name + ".stick.img")
    ls = subprocess.run(["mdir", "-b", "-i", stick + "@@64M", "::/logs"], capture_output=True,
                        text=True).stdout.split()
    logs = [p for p in ls if re.search(r"boot-\d+\.txt$", p)]
    check("logd saved one /data/logs/boot-NNNN.txt (%s)" % ", ".join(logs), len(logs) == 1)
    if len(logs) == 1:
        raw = subprocess.run(["mtype", "-i", stick + "@@64M", logs[0]], capture_output=True).stdout
        lg = raw.decode("utf-8", "replace").replace("\r", "").split("\n")
        lg = [l for l in lg if ts.match(l)]
        check("logd's file starts at the boot's first line", bool(lg) and lg[0] == serial[0])
        last = max(i for i, l in enumerate(serial[:done[0]])
                   if re.search(r"init: bin/\S+ exited with code", l))
        miss = [l for l in serial[:last + 1] if l not in set(lg)]
        for l in miss[:5]:
            print("initnet-test:   logd lacks: " + l[:150])
        check("logd's file has every line up to the last program's result (%d missing)" %
              len(miss), not miss)
        check("... and nothing the serial port lacks", all(l in set(serial) for l in lg))
sys.exit(1 if fails else 0)
EOF
if [ $ok = 1 ]; then
    echo "initnet-test: PASS"
else
    echo "initnet-test: FAIL"
    exit 1
fi
