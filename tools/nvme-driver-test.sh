#!/bin/sh
# drv/nvme's controller core in QEMU (`-device nvme`), one boot per case,
# all at once. tools/nvme-test.sh is the read-only stage's whole test (the
# partitions, the mounts, the PC-like image); this one is the driver's own:
# its test reads (the boot word nvmetest=..., drivers/nvme/selftest.c) on a
# drive image of random bytes this script makes, so any byte read wrong
# changes a digest.
#   init     the `init` run (utest, usbtest, ...) with an NVMe drive and
#            nvmetest=selftest: the first and last MiB read through the
#            I/O queue and hashed (equal to sha256sum of the image's
#            ends); at the end devmgr stops the driver, which sends the
#            normal shutdown notification and ends cleanly
#   iommu    the same behind QEMU's VT-d unit (QEMU_IOMMU=1): no fault
#   ns       a shell boot, the controller with two namespaces: 1 has 8
#            bytes of metadata per block (not served), 2 has 4096-byte
#            blocks (served: its digests)
#   reset    nvmetest=reset: the controller reset with the reads out; they
#            are sent again after it and hash the same
#   abort    nvmetest=abort: the first read is past its deadline at once;
#            the Abort is answered and the reads hash the same
#   kill     nvmetest=loop (the ends read again and again): `kill nvme`
#            mid-read; devmgr restarts it, which disables the controller
#            the dead one left running and reads the same digests
#   nostop   nvmetest=nostop: the reset with the reads out takes the
#            controller as not stopped (CSTS.RDY still 1): bus mastering
#            goes off before the reads are answered (ended unfinished),
#            and the driver gives up (its restarts do the same: no digests)
# Every case: QEMU traces every admin and I/O command
# (pci_nvme_admin_cmd, pci_nvme_io_cmd) and the case fails on any command
# but the read-only list (admin 00 01 02 04 05 06 08 09, I/O 02 Read), or
# if none was traced at all (a QEMU whose trace events changed). The
# firmware's own NVMe driver runs first and is traced too: it must keep
# to the list as well. The image is opened writable (a read-only drive
# would turn a write into an error instead of showing it) and its SHA-256
# must be the same after the boot. No line may say FAILED.
# QEMU_SMP passes through. Usage: tools/nvme-driver-test.sh <outdir>
# [case...] (default: all); exit 0 on PASS.
set -eu
out=$1
shift
cases=${*:-init iommu ns reset abort kill nostop}
mkdir -p "$out"

# The script a shell case types: wait for the test reads (twice: the
# restarted driver's too, for kill), then reboot.
script() {
    f="$out/$1.txt"
    {
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        if [ "$1" = nostop ]; then
            echo "seen 90 bus mastering off (the controller didn't stop)"
            echo "seen 30 request(s) ended unfinished"
        else
            echo "seen 90 selftest: both ends read and hashed"
        fi
        if [ "$1" = kill ]; then
            echo "send sleep 1; kill nvme"
            echo "wait shell: killed process"
            echo "wait 60 restarted (restart 1 since boot)"
            echo "wait 60 selftest: both ends read and hashed"
            echo "send echo nv-\"\"1"
            echo "wait nv-1"
        fi
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$f"
    echo "$f"
}

trace="-trace pci_nvme_admin_cmd -trace pci_nvme_io_cmd"

# run <case>: the boot, in the background's own shell.
run() {
    c=$1
    img="$out/$c-disk.img" img2="$out/$c-disk2.img"
    head -c $((64 << 20)) /dev/urandom > "$img"
    sha256sum "$img" | cut -d' ' -f1 > "$out/$c.before"
    drives="-drive file=$img,if=none,id=nv,format=raw"
    dev="-device nvme,serial=jamtest-$c,drive=nv"
    words="nvmetest=selftest" mode=shell iommu= input=
    case $c in
    init) mode=init ;;
    iommu) mode=init iommu=1 ;;
    ns)
        head -c $((32 << 20)) /dev/urandom > "$img2"
        drives="$drives -drive file=$img2,if=none,id=nv2,format=raw"
        dev="-device nvme,id=nvc,serial=jamtest-ns -device nvme-ns,bus=nvc,drive=nv,nsid=1,ms=8"
        dev="$dev -device nvme-ns,bus=nvc,drive=nv2,nsid=2,logical_block_size=4096,physical_block_size=4096" ;;
    reset) words="nvmetest=reset" ;;
    abort) words="nvmetest=abort" ;;
    kill) words="nvmetest=loop" ;;
    nostop) words="nvmetest=nostop" ;;
    esac
    [ $mode = shell ] && input=$(script "$c")
    QEMU_IOMMU=$iommu QEMU_INPUT=$input QEMU_TIMEOUT=${QEMU_TIMEOUT:-200} \
        QEMU_EXTRA="$drives $dev $trace -D $out/$c.trace" \
        tools/qemu-test.sh "$out" "$c" $mode $words > "$out/$c.out" 2>&1 ||
        echo "the boot failed: $(tail -3 "$out/$c.out" | tr '\n' ' ')" > "$out/$c.fail"
    sha256sum "$img" | cut -d' ' -f1 > "$out/$c.after"
    [ ! -f "$img2" ] || sha256sum "$img2" | cut -d' ' -f1 > "$out/$c.ns2"
}

# check <case>: its log, trace and image. Prints what is wrong.
check() {
    c=$1 log="$out/$1.log" tr="$out/$1.trace"
    [ ! -f "$out/$c.fail" ] || cat "$out/$c.fail"
    cmp -s "$out/$c.before" "$out/$c.after" || echo "the image changed"
    img="$out/$c-disk.img"
    [ $c != ns ] || img="$out/$c-disk2.img"
    first=$(head -c 1048576 "$img" | sha256sum | cut -d' ' -f1)
    last=$(tail -c 1048576 "$img" | sha256sum | cut -d' ' -f1)
    if [ $c != nostop ]; then   # nostop reads nothing to the end, and fails on purpose
        grep -q "selftest: first 1048576 bytes (lba 0) sha256 $first" "$log" ||
            echo "no right digest of the first MiB ($first)"
        grep -q "selftest: last 1048576 bytes (lba [0-9]*) sha256 $last" "$log" ||
            echo "no right digest of the last MiB ($last)"
        ! grep -E "nvme .*(FAILED|sha256)" "$log" | tr -d '\r' | grep -vE "sha256 ($first|$last)$" |
            grep . || echo "(lines above: a failure or a wrong digest)"
    fi
    adm=$(grep -c "pci_nvme_admin_cmd" "$tr" 2>/dev/null || true)
    [ "${adm:-0}" -gt 0 ] || echo "no admin command traced (QEMU's trace events?)"
    bad=$(grep -oE "pci_nvme_admin_cmd .* opc 0x[0-9a-f]+" "$tr" 2>/dev/null |
          grep -vE "opc 0x(0|1|2|4|5|6|8|9)$" || true)
    [ -z "$bad" ] || echo "admin commands outside the list: $(echo "$bad" | sort | uniq -c)"
    bad=$(grep -oE "pci_nvme_io_cmd .* opc 0x[0-9a-f]+" "$tr" 2>/dev/null |
          grep -vE "opc 0x2$" || true)
    [ -z "$bad" ] || echo "I/O commands other than Read: $(echo "$bad" | sort | uniq -c)"
    case $c in
    init|iommu)
        grep -q "shutdown notification: complete" "$log" ||
            echo "no completed shutdown notification" ;;
    ns)
        grep -q "namespace 1: not served" "$log" || echo "namespace 1 (metadata) served?"
        grep -q "namespace 2: 8192 blocks of 4096 bytes" "$log" || echo "namespace 2 not served" ;;
    reset)
        grep -q "reset 1 (the test word)" "$log" || echo "no reset" ;;
    abort)
        grep -q "timed out: abort" "$log" || echo "no abort sent"
        grep -q "abort: answered" "$log" || echo "the abort wasn't answered" ;;
    kill)
        n=$(grep -c "selftest: first 1048576 bytes (lba 0) sha256 $first" "$log" || true)
        [ "$n" -ge 2 ] || echo "the restarted driver didn't read the same ($n digest line(s))"
        awk '/restarted \(restart 1 since boot\)/ { r = 1 } r && /the controller was enabled/ { f = 1 }
             END { exit !f }' "$log" || echo "the restarted driver didn't find the controller enabled" ;;
    nostop)
        grep -q "test: the controller taken as not stopped" "$log" || echo "no reset taken as stuck"
        # The first driver's lines: bus mastering off, then the reads answered.
        awk '/bus mastering off \(the controller didn.t stop\): OK/ && !b { b = NR }
             /request\(s\) ended unfinished/ && !e { e = NR }
             END { exit !(b && e && b < e) }' "$log" ||
            echo "the reads out were answered before bus mastering was off" ;;
    esac
    [ $c != iommu ] || ! grep -q "vtd: fault" "$log" || echo "an IOMMU fault"
}

pids=
for c in $cases; do
    rm -f "$out/$c.fail"
    run "$c" &
    pids="$pids $!"
done
for p in $pids; do wait "$p" || true; done
ok=1
for c in $cases; do
    why=$(check "$c")
    if [ -n "$why" ]; then
        echo "nvme-driver $c: FAILED"
        echo "$why" | sed 's/^/    /'
        ok=0
    else
        ops=$(grep -oE "(admin|io)_cmd .* opc 0x[0-9a-f]+" "$out/$c.trace" | sed 's/_cmd.* opc / /' |
              sort | uniq -c | awk '{printf "%s %s x%s, ", $2, $3, $1}')
        echo "nvme-driver $c: PASS (traced: ${ops%, })"
    fi
done
rm -f "$out"/*-disk.img "$out"/*-disk2.img
[ $ok = 1 ]
