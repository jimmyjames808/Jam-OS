#!/bin/sh
# A stick pulled and pushed back in quickly, many times: a shell boot with
# a second stick (MBR, one FAT32 partition, a -blockdev node, like the boot
# stick) on xhci.0 port 2, which a driver here (python, on QEMU's monitor
# and the serial log) pulls with device_del and plugs back with
# device_add, <rounds> times (default 81). Each round varies two gaps:
# from usb-bus's "attached" line to the pull (0, 30 or 150 ms: the pull
# lands while usb-storage, volumes and fat are still starting, with bulk
# transfers in flight, or after), and from usb-bus's "detached" line to
# the replug (0, 1, 2, 5, 10, 20, 50 or 100 ms; or the replug sent right
# after the pull, before the guest has seen it). A replug whose "attached" line
# doesn't come within 4 s is LOST: counted, then the stick is plugged
# again so the rounds go on. Passes with 0 lost and every pull's
# "detached" line seen; prints the counts ("replug: N rounds, L lost").
#
# QEMU frees a device_del'd stick's drive only when its RCU callbacks run,
# some 50 to 100 ms after the pull, and refuses a device_add of the same
# drive until then ("Permission conflict on node ..."; it drains RCU
# before it says so, so the next try works). The driver tries again until
# QEMU takes it, as tools/serial-feed.py does. With `once` it sends each
# device_add once, as serial-feed did before (it never saw the refusal):
# the replugs QEMU refused are then LOST, as tools/screen-test.sh's "stick
# is back" sometimes was, and the test fails.
# QEMU_SMP and QEMU_XHCI pass through.
# Usage: tools/replug-test.sh <outdir> [name] [rounds] [once]; exit 0 on PASS.
set -eu
out=$1 name=${2:-replug} rounds=${3:-81} mode=${4:-retry}
mkdir -p "$out"
a="$out/$name-a.img"
python3 tools/mkstick.py "$a" 32 0c
mformat -i "$a@@1M" -T $((31 * 2048)) -F -v REPLUG ::
feed="$out/$name.txt"
cat > "$feed" <<'EOF'
wait 120 Jam OS shell
wait {prompt}
seen 60 init: /data mounted
EOF

log="$out/$name.log" mon="build/.qemu-$name.sock"
rm -f "$out/$name.driver"
QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} QEMU_INPUT="$feed" \
QEMU_USB="-blockdev driver=file,node-name=rfile,filename=$a -blockdev driver=raw,node-name=rimg,file=rfile " \
    tools/qemu-test.sh "$out" "$name" shell > "$out/$name.out" 2>&1 &
qpid=$!
ok=1
python3 - "$log" "$mon" "$rounds" "$mode" > "$out/$name.driver" 2>&1 <<'PY' || ok=0
import socket, sys, time
log, mon, n, once = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4] == "once"
DEV = "usb-storage,id=rs,bus=xhci.0,port=2,drive=rimg"
ATTACHED, DETACHED = "[usb-bus] attached: port 2 ", "usb 2: 46f4:0001 detached"


def say(*a):
    print(*a, flush=True)


data, pos, lf = b"", 0, None   # the log so far, how far it has been looked at


def more():
    global data, lf
    if lf is None:
        try:
            lf = open(log, "rb")
        except OSError:
            return
    data += lf.read()


def find(text, timeout):
    """Wait for text in the log after pos; move pos past it."""
    global pos
    end = time.time() + timeout
    while True:
        more()
        i = data.find(text.encode(), pos)
        if i >= 0:
            pos = i + len(text)
            return True
        if time.time() > end:
            return False
        time.sleep(0.0005)


def mark():
    """pos to the end of the log: only what comes from now counts."""
    global pos
    more()
    pos = len(data)


s = None
for _ in range(600):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(mon)
        break
    except OSError:
        s = None
        time.sleep(0.1)
if s is None:
    sys.exit("replug: no monitor at " + mon)
s.settimeout(30)
buf = b""


def answer():
    """The monitor's output up to its next prompt."""
    global buf
    while b"(qemu) " not in buf:
        d = s.recv(4096)
        if not d:
            sys.exit("replug: the monitor closed")
        buf += d
    i = buf.index(b"(qemu) ")
    said, buf = buf[:i], buf[i + 7:]
    return said.decode(errors="replace").replace("\r", "")


def monitor(cmd):
    """A monitor command: its error, or ""."""
    s.sendall(cmd.encode() + b"\n")
    return " / ".join(l.strip() for l in answer().split("\n") if "rror" in l)


answer()   # the banner
refused = 0       # device_adds QEMU refused
waited = []       # per refused replug: ms from the pull until QEMU took it
pulled_at = 0.0   # when the last device_del was sent


def plug(again):
    """device_add; with again, until QEMU takes it (up to 10 s). True if
    it took one."""
    global refused
    first = refused
    t0 = time.time()
    while True:
        err = monitor("device_add " + DEV)
        if not err:
            if refused > first:
                waited.append(int((time.time() - pulled_at) * 1000))
            return True
        refused += 1
        if not again:
            return False
        if time.time() - t0 > 10:
            say("QEMU refused the replug for 10 s: %s" % err)
            return False
        time.sleep(0.02)


def rounds():
    global pulled_at
    if not find("init: /data mounted", 300) or not find("usb-bus: ", 60):
        return fail("replug: the boot didn't get to the shell")
    time.sleep(3)   # past usb-bus's first list: "attached:" lines from here on
    mark()
    if not plug(True) or not find(ATTACHED, 30):
        return fail("replug: the stick was never attached the first time")
    lost = nodetach = lost_refused = 0
    GAP_ATTACHED = (0, 0.030, 0.150)
    GAP_DETACHED = (None, 0, 0.001, 0.002, 0.005, 0.010, 0.020, 0.050, 0.100)
    for r in range(n):
        ga = GAP_ATTACHED[r % len(GAP_ATTACHED)]
        gd = GAP_DETACHED[(r // len(GAP_ATTACHED)) % len(GAP_DETACHED)]
        time.sleep(ga)
        mark()
        pulled_at = time.time()
        err = monitor("device_del rs")
        if err:
            say("round %d: device_del: %s" % (r, err))
        if gd is not None:
            if not find(DETACHED, 5):
                nodetach += 1
                say("round %d: no '%s' line 5 s after the pull" % (r, DETACHED))
            time.sleep(gd)
        took = plug(not once)
        if took and find(ATTACHED, 4):
            continue
        lost += 1
        lost_refused += not took
        say("round %d: LOST: no 'attached' 4 s after the replug (%d ms after the attach, "
            "replug %s)%s" % (r, int(ga * 1000), "with the pull" if gd is None else
                              "%d ms after the detach" % int(gd * 1000),
                              ": QEMU had refused the device_add" if not took else ""))
        mark()
        monitor("device_del rs")
        if not plug(True) or not find(ATTACHED, 10):
            return fail("round %d: still not attached after another replug: stopping" % r)
    if waited:
        waited.sort()
        say("QEMU refused the first device_add in %d of %d rounds; it took one %d..%d ms "
            "(median %d) after the pull" % (len(waited), n, waited[0], waited[-1],
                                            waited[len(waited) // 2]))
    say("replug: %d rounds, %d lost (%d of them refused by QEMU), %d pulls without a "
        "'detached' line; %s" % (n, lost, lost_refused, nodetach,
                                 "each device_add sent once" if once else
                                 "device_add sent again until QEMU took it"))
    return 1 if lost or nodetach else 0


def fail(why):
    say(why)
    return 1


rc = rounds()
try:
    monitor("quit")
except (OSError, SystemExit):
    pass
sys.exit(rc)
PY
wait $qpid || true
cat "$out/$name.driver"
rm -f "$a"
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL (see $log)"
exit 1
