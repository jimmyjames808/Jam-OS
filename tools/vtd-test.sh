#!/bin/sh
# The boot-time VT-d probe (kernel/dev/vtd_probe.c) in QEMU, three boots
# of the `pcilist` entry (PCI, the probe, the Devices list, no user space):
#   1. with QEMU's intel-iommu (interrupt remapping on, caching mode on, as
#      M11's tests will run it): the DMAR table's lines (one unit at
#      fed90000, its I/O APIC scope matched with the MADT's, an endpoint
#      scope per PCI function named by its ids), the unit's registers
#      decoded (version, caching mode 1, queued invalidation, interrupt
#      remapping), translation and interrupt remapping off as the firmware
#      left them, the handover line, and nothing in the RESULTS box. QEMU's
#      own trace of the unit's registers proves the probe only READ them:
#      some vtd_reg_read lines and not one vtd_reg_write;
#   2. the same with eim=on: the unit takes x2APIC destination ids;
#   3. without an IOMMU: one line, "no DMAR table".
# VTD_TEST_INIT=1 adds a fourth: the `init` run (utest, usbtest) with the
# intel-iommu present and left off, so every device's DMA and interrupts
# still work around it (about 30 s more).
# The parser's own tests are the ktests dmar_* and vtd_* (kernel/test/test_dmar.c).
# QEMU_SMP passes through. Usage: tools/vtd-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
ok=1
iommu="-device intel-iommu,intremap=on,caching-mode=on"
trace="-trace vtd_reg_read -trace vtd_reg_write"

# have <name> <fixed string>...: every string is in <name>'s log.
have() {
    name=$1
    shift
    for want in "$@"; do
        grep -qF -- "$want" "$out/$name.log" || { echo "$name: no line with \"$want\""; ok=0; }
    done
}

# clean <name>: the run ended well and reported nothing about VT-d (the
# probe's report lines, which go to the RESULTS box, start "vtd: " and a
# word; its log lines are "vtd:" and spaces).
clean() {
    have "$1" "run complete: no problems"
    if grep -E "vtd: [^ ]" "$out/$1.log"; then
        echo "$1: the probe reported a problem (above)"
        ok=0
    fi
}

run() {
    name=$1
    shift
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-90} QEMU_EXTRA="$*" \
        tools/qemu-test.sh "$out" "$name" pcilist > "$out/$name.out" 2>&1 ||
        { echo "$name: QEMU run failed (see $out/$name.out)"; ok=0; }
}

run vtd-iommu "$iommu $trace"
have vtd-iommu \
    "vtd:         DMAR rev 1," "flags 1 (interrupt remapping yes, x2APIC opt-out no" \
    "vtd:         unit 0: registers fed90000 (1 page), segment 0" \
    "unit 0 scope 0: IOAPIC 0, requester id ff:00.0 (the MADT's I/O APIC at fec00000" \
    "endpoint, bus 00 path 03.0 = 00:03.0 1b36:000d class 0c03" \
    "endpoint, bus 00 path 04.0 = 00:04.0 1234:11e8" \
    "vtd:         unit 0: version 1.0, cap " \
    "mgaw 48 bits, cm 1," "qi 1, ir 1, eim 0, pt 1" \
    "vtd:         unit 0 status: translation off, interrupt remapping off, queued invalidation off" \
    "vtd:         bus mastering on at the probe:" \
    "vtd:         CPUs: highest APIC id" \
    "vtd:         handover: 1 of 1 unit answered; translation on in 0, interrupt remapping on in 0"
clean vtd-iommu
reads=$(grep -c "vtd_reg_read" "$out/vtd-iommu.out" || true)
writes=$(grep -c "vtd_reg_write" "$out/vtd-iommu.out" || true)
[ "$reads" -ge 8 ] || { echo "vtd-iommu: $reads register reads traced, want 8 or more"; ok=0; }
[ "$writes" -eq 0 ] || { echo "vtd-iommu: $writes register WRITES traced, want none"; ok=0; }

run vtd-eim "-device intel-iommu,intremap=on,eim=on,caching-mode=on"
have vtd-eim "qi 1, ir 1, eim 1, pt 1"
clean vtd-eim

run vtd-none ""
have vtd-none "vtd:         no DMAR table: the firmware reports no VT-d remapping hardware"
clean vtd-none
if grep -E "vtd:         (DMAR|unit)" "$out/vtd-none.log"; then
    echo "vtd-none: unit lines without a DMAR table"
    ok=0
fi

if [ "${VTD_TEST_INIT:-0}" = 1 ]; then
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_EXTRA="$iommu" \
        tools/qemu-test.sh "$out" vtd-init init > "$out/vtd-init.out" 2>&1 ||
        { echo "vtd-init: QEMU run failed"; ok=0; }
    have vtd-init "vtd:         handover: 1 of 1 unit answered"
    clean vtd-init
fi

if [ $ok = 1 ]; then
    echo "vtd-test: PASS ($reads register reads, $writes writes)"
else
    echo "vtd-test: FAIL"
    exit 1
fi
