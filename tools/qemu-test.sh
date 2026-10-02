#!/bin/sh
# Boot build/jamos.img headless in QEMU with a given kernel command line,
# wait until it halts, then save the serial log and a screenshot.
# QEMU_IMAGE picks another image (e.g. build/noktests/jamos.img).
# QEMU_XHCI adds qemu-xhci properties (e.g. "msi=on,msix=off": an MSI-only
# xHCI like many Intel PCH controllers).
# QEMU_INPUT=<script> types into the serial port (the shell tests): the
# serial port becomes a socket chardev (still logged to <name>.log) that
# tools/serial-feed.py drives with the script (its format is in that file).
# The run ends when QEMU does (e.g. the script's `reboot`: -no-reboot) or
# at QEMU_TIMEOUT; it passes if every `wait` in the script matched and QEMU
# ended by itself.
# QEMU_TIMEOUT: seconds before a run is given up (150: a cap, a run ends
# as soon as the kernel halts or QEMU goes; the `init` run and the full
# ktest take about 30 s each, more on a busy machine).
# QEMU_USB adds USB devices after the boot stick (which takes xhci.0 port
# 1), e.g. "-device usb-hub,bus=xhci.0,port=2 -device usb-kbd,bus=xhci.0,port=2.1"
# (give every device a port=, or QEMU picks the next free one).
# QEMU_EXTRA: more QEMU arguments, e.g. "-rtc base=2026-01-15T01:02:03" (the
# shell test sets the real-time clock with it).
# QEMU_MONITOR names a script run against the QEMU monitor while it boots,
# one command per line:
#     expect <text>    wait (up to the timeout) until the serial log has <text>
#     send <command>   a monitor command: sendkey a, device_del kbd2, ...
#     sleep <seconds>
# QEMU_SAVE=<file> keeps the run's stick image, with what the guest wrote
# to it, as <file> (for a later run's QEMU_IMAGE: a second boot of the
# same stick). QEMU_BOOT_PREV=1 boots the stick's previous build
# (boot/prev-jamos.elf and prev-bootfs.img, as the boot menu's "Jam OS
# (previous build)" does) instead of boot/jamos.elf and bootfs.img.
# The stick is the device "stick" on the block node "usbstick" (a
# -blockdev, which outlives the device): a script pulls it with the monitor
# command `device_del stick` and plugs it back with
# `device_add usb-storage,id=stick,bus=xhci.0,port=1,drive=usbstick`.
# QEMU_STICK_THROTTLE makes the stick as slow as a real one, with QEMU's
# throttle-group limits, e.g. "x-iops-write=100,x-bps-write=10485760":
# every write command waits its turn (100 a second), whatever its size, as
# a cheap USB 2 stick's do (tools/update-pc-test.sh).
# A panic starts the kernel's stored copy (kexec): a run without
# QEMU_INPUT ends there (PANIC), a scripted one goes on into that boot.
# The boot splash (a plain boot's animation) is left out with the boot
# word `nosplash`, so the tests see the text log as before; QEMU_SPLASH=1
# keeps it (tools/splash-test.sh).
# The machine has no network card (-nic none) unless QEMU_NET asks for one;
# it always has a spare MSI-X function with no driver, a virtio-rng,
# first so it takes 00:02.0, where q35's default network card was (the
# other functions keep their addresses; utest's driver_handle_limits
# tries a driver's handles on it).
# QEMU_NET=1: QEMU's e1000e (8086:10d3; no option ROM, so the firmware
# never sends on it) on a `-netdev dgram` to tools/netpeer.py, which this
# script starts on two free UDP ports and stops at the end (its log in
# <outdir>/<name>.peer.log, its summary in <name>.peer.json); every frame
# the guest sends is also dumped to <outdir>/<name>.pcap and checked by
# tools/pcap-vlan-check.py. The run fails if either finds a frame from the
# guest that breaks the network's rule: QEMU_NET_VLAN (default 21) is the
# VLAN every frame must be tagged with, or `none`: every frame untagged,
# none tagged. The guest gets the same as its boot word (vlan=21,
# vlan=none) unless the command line has a vlan word already or
# QEMU_NET_WORD=0 (a run that boots with the build's default: the caller
# then sets QEMU_NET_VLAN to that default).
# QEMU_NET_NONE=1: no frame at all may leave (the vlan=off run).
# QEMU_NET_PEER: more netpeer flags (e.g. "--noise 2").
# QEMU_NET=<peer port>:<qemu port>: the same card and pcap, with a peer
# the caller runs (and checks) itself.
# Usage: tools/qemu-test.sh <outdir> <name> [cmdline...]
set -eu
out=$1 name=$2
shift 2
cmdline="$*"
[ "${QEMU_SPLASH:-0}" = 1 ] || cmdline="$cmdline nosplash"
net_vlan=${QEMU_NET_VLAN:-21}
if [ -n "${QEMU_NET:-}" ] && [ "${QEMU_NET_WORD:-1}" != 0 ]; then
    case " $cmdline" in
    *" vlan"*) ;;   # the caller chose the mode
    *) cmdline="$cmdline vlan=$net_vlan" ;;
    esac
fi
ovmf=$(brew --prefix qemu)/share/qemu
mkdir -p "$out"

img="$out/$name.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
kfile=jamos.elf bfile=bootfs.img
if [ "${QEMU_BOOT_PREV:-0}" = 1 ]; then   # the boot menu's "Jam OS (previous build)"
    kfile=prev-jamos.elf bfile=prev-bootfs.img
fi
{
    printf 'timeout: 0\n/test\n    protocol: limine\n    path: boot():/boot/%s\n' "$kfile"
    printf '    module_path: boot():/boot/%s\n' "$bfile"
    printf '    module_path: boot():/boot/%s\n    cmdline: %s\n' "$kfile" "$cmdline"
} > "$out/$name.conf"
mcopy -o -i "$img@@1M" "$out/$name.conf" ::/boot/limine/limine.conf
cp "$ovmf/edk2-i386-vars.fd" "$out/$name.vars"

log="$out/$name.log" mon="build/.qemu-$name.sock"   # unix socket paths max out at 104 bytes
nic="-nic none"
ppid= pcap="$out/$name.pcap"
if [ -n "${QEMU_NET:-}" ]; then
    rm -f "$pcap" "$out/$name.peer.log" "$out/$name.peer.json" "$out/$name.peer.ready"
    case $QEMU_NET in
    *:*) pport=${QEMU_NET%%:*} qport=${QEMU_NET#*:} ;;
    *)
        set -- $(python3 tools/netpeer.py --free-ports 2)
        pport=$1 qport=$2
        none=
        [ "${QEMU_NET_NONE:-0}" = 1 ] && none=--expect-none
        python3 tools/netpeer.py --listen "$pport" --qemu "$qport" --vlan "$net_vlan" \
            $none ${QEMU_NET_PEER:-} --log "$out/$name.peer.log" \
            --summary "$out/$name.peer.json" --ready "$out/$name.peer.ready" \
            > "$out/$name.peer.out" 2>&1 &
        ppid=$!
        j=0
        while [ $j -lt 50 ] && [ ! -s "$out/$name.peer.ready" ]; do sleep 0.1; j=$((j + 1)); done
        [ -s "$out/$name.peer.ready" ] || { echo "$name: the network peer didn't start"; exit 1; } ;;
    esac
    nic="-netdev dgram,id=net0,local.type=inet,local.host=127.0.0.1,local.port=$qport"
    nic="$nic,remote.type=inet,remote.host=127.0.0.1,remote.port=$pport"
    nic="$nic -device e1000e,netdev=net0,romfile="
    nic="$nic -object filter-dump,id=dump0,netdev=net0,queue=rx,file=$pcap"
fi
# The stick's block nodes: the file, then raw on it as "usbstick", or raw
# then a throttle filter as "usbstick" (QEMU_STICK_THROTTLE).
stick="-blockdev driver=raw,node-name=usbstick,file=stickfile"
if [ -n "${QEMU_STICK_THROTTLE:-}" ]; then
    stick="-object throttle-group,id=stickslow,$QEMU_STICK_THROTTLE"
    stick="$stick -blockdev driver=raw,node-name=stickraw,file=stickfile"
    stick="$stick -blockdev driver=throttle,node-name=usbstick,throttle-group=stickslow,file=stickraw"
fi
ser="build/.qemu-$name.ser"
rm -f "$log" "$mon" "$ser"
if [ -n "${QEMU_INPUT:-}" ]; then
    serial="-chardev socket,id=ser0,path=$ser,server=on,wait=on,logfile=$log -serial chardev:ser0"
else
    serial="-serial file:$log"
fi
qemu-system-x86_64 -M q35 -m "${QEMU_MEM:-2G}" -smp "${QEMU_SMP:-4}" -cpu "${QEMU_CPU:-max}" \
    -drive if=pflash,format=raw,readonly=on,file="$ovmf/edk2-x86_64-code.fd" \
    -drive if=pflash,format=raw,file="$out/$name.vars" \
    -device virtio-rng-pci,vectors=2 \
    -device qemu-xhci,id=xhci${QEMU_XHCI:+,$QEMU_XHCI} \
    -blockdev driver=file,node-name=stickfile,filename="$img" \
    $stick \
    -device usb-storage,id=stick,bus=xhci.0,port=1,drive=usbstick,bootindex=0 \
    ${QEMU_USB:-} ${QEMU_EXTRA:-} \
    -device edu,dma_mask=0xffffffff \
    $nic \
    $serial -display none -no-reboot \
    -monitor unix:"$mon",server,nowait &
qpid=$!
fpid=
if [ -n "${QEMU_INPUT:-}" ]; then
    QEMU_MON="$mon" SHOT_DIR="$out" \
        python3 tools/serial-feed.py "$ser" "$QEMU_INPUT" 2> "$out/$name.feed" &
    fpid=$!
fi

limit=$(( ${QEMU_TIMEOUT:-150} * 2 ))
if [ -n "${QEMU_MONITOR:-}" ]; then
    (
        while read -r what arg; do
            case $what in
            expect)
                j=0
                while [ $j -lt $((limit * 3)) ] && ! grep -qF -- "$arg" "$log" 2>/dev/null; do
                    sleep 0.2
                    j=$((j + 1))
                done ;;
            send) echo "$arg" | nc -U -w1 "$mon" >/dev/null 2>&1 || true ;;
            sleep) sleep "$arg" ;;
            esac
        done < "$QEMU_MONITOR"
    ) &
    mpid=$!
fi

# A run without a script also ends at a panic that starts the stored
# kernel (kexec): what comes after it is another boot.
ends="Halting|Idling|system halted"
[ -n "${QEMU_INPUT:-}" ] || ends="$ends|starting the stored kernel: the next boot"
i=0
while [ $i -lt $limit ] && kill -0 $qpid 2>/dev/null &&
      ! grep -qE "$ends" "$log" 2>/dev/null; do
    sleep 0.5
    i=$((i + 1))
done
sleep 0.3
echo "screendump $out/$name.ppm" | nc -U -w1 "$mon" >/dev/null || true
sleep 0.5
kill $qpid 2>/dev/null || true
wait $qpid 2>/dev/null || true
if [ -n "${mpid:-}" ]; then
    kill $mpid 2>/dev/null || true
    wait $mpid 2>/dev/null || true
fi
python3 -c "from PIL import Image; Image.open('$out/$name.ppm').save('$out/$name.png')" \
    2>/dev/null || true
if [ -n "${QEMU_SAVE:-}" ]; then
    cp "$img" "$QEMU_SAVE"
fi
rm -f "$img" "$out/$name.vars" "$out/$name.ppm" "$mon" "$ser"
# The network: the peer's verdict, then the pcap's (QEMU_NET).
net_ok=1
if [ -n "$ppid" ]; then
    kill $ppid 2>/dev/null || true
    wait $ppid 2>/dev/null || net_ok=0
    cat "$out/$name.peer.out"
fi
if [ -n "${QEMU_NET:-}" ]; then
    none=
    [ "${QEMU_NET_NONE:-0}" = 1 ] && none=--expect-none
    python3 tools/pcap-vlan-check.py --vlan "$net_vlan" $none "$pcap" || net_ok=0
fi
[ $net_ok = 1 ] || echo "$name: FAILED (network: a frame that breaks vlan=$net_vlan's rule, or one too many)"
if [ -n "$fpid" ]; then
    fst=0
    wait $fpid || fst=$?
    cat "$out/$name.feed"
    [ $fst -eq 0 ] && [ $i -lt $limit ] || { echo "$name: FAILED (input script)"; exit 1; }
    [ $net_ok = 1 ] || exit 1
    exit 0
fi
[ $net_ok = 1 ] || exit 1
grep -q "starting the stored kernel: the next boot" "$log" && { echo "$name: PANIC"; exit 1; }
grep -qE "Halting|Idling|system halted" "$log" || { echo "$name: TIMEOUT"; exit 1; }
