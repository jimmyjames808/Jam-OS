#!/bin/sh
# VT-d in QEMU: the boot-time probe (kernel/dev/vtd_probe.c) in three boots
# of the `pcilist` entry (PCI, the probe, the Devices list, no user space),
# then the units themselves in two ktest boots:
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
#   3. without an IOMMU: one line, "no DMAR table";
#   4. and 5. the units started with the boot word iommu=on (kernel/dev/
#      vtd_unit.c, vtd_qi.c, vtd_fault.c) and their ktests (ktest=vtd),
#      with caching mode on (QEMU_IOMMU=1) and off (cm0, the PC's case):
#      the "started" line, every vtd_unit_* test passed (every
#      invalidation kind, the vtd_pt and vtd_ir callbacks, a refused
#      descriptor reported and the queue going on, the queue wrapping,
#      every CPU at once, QI off and on); interrupt remapping on
#      (kernel/dev/vtd_irq.c) and every vtd_irq_* test passed (COM1's pin
#      remapped, an MSI through its own entry, entries freed on close, a
#      foreign, freed or out-of-range entry refused and recorded, the
#      interrupt window written by edu's DMA blocked, the timer, IPIs and
#      COM1 unaffected, remapping off and on again); nothing in the RESULTS
#      box but the refusal and the interrupt faults the tests provoke
#      (edu's, 00:04.0, and its DMA's, which QEMU sends with no requester
#      id: ff:1f.7);
#   6. a shell boot with iommu=on, `reboot` (kexec): the next kernel takes
#      the queue over (drained, off, its own set up), starts the unit and
#      turns interrupt remapping on again; nothing reported.
# Run 1 has no iommu word: it must start nothing (no write traced).
# VTD_TEST_INIT=1 adds the `init` run (utest, usbtest) twice, with the
# intel-iommu present: left off, and started with iommu=on (its queue and
# fault interrupt running: translation is not on yet), so every device's
# DMA and interrupts still work around it (about a minute more).
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
if grep -qF "started: invalidation queue" "$out/vtd-iommu.log"; then
    echo "vtd-iommu: a unit was started without iommu=on"
    ok=0
fi
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

# on <name> <QEMU_IOMMU>: the units started, their ktests.
on() {
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_IOMMU=$2 \
        tools/qemu-test.sh "$out" "$1" ktest=vtd iommu=on > "$out/$1.out" 2>&1 ||
        { echo "$1: QEMU run failed (see $out/$1.out)"; ok=0; }
    have "$1" "vtd:         unit 0: started: invalidation queue at" \
        "vtd:         iommu=on: 1 of 1 unit started" \
        "ktest: vtd_unit_every_invalidation_completes ok" \
        "ktest: vtd_unit_callbacks_invalidate    ok" \
        "ktest: vtd_unit_refused_descriptor_reported ok" \
        "ktest: vtd_unit_queue_wraps             ok" \
        "ktest: vtd_unit_many_cpus_at_once       ok" \
        "ktest: vtd_unit_queue_off_and_on        ok" \
        "ktest: vtd_unit_registers_kept          ok" \
        "vtd:         interrupt remapping on: 1 unit, one table at" \
        "ktest: vtd_irq_check_units_pure         ok" \
        "ktest: vtd_irq_on_and_ioapic            ok" \
        "ktest: vtd_irq_msi_through_entry        ok" \
        "ktest: vtd_irq_entries_freed_on_close   ok" \
        "ktest: vtd_irq_foreign_and_stale_source_refused ok" \
        "ktest: vtd_irq_window_write_blocked     ok" \
        "ktest: vtd_irq_timer_ipis_and_com1_unaffected ok" \
        "ktest: vtd_irq_off_and_on_again         ok" \
        "run complete: no problems"
    if grep -E "vtd: [^ ]" "$out/$1.log" | grep -vF "the queue refused descriptor" |
        grep -vE "vtd: fault: unit 0: (00:04\.0|ff:1f\.7) interrupt, index"; then
        echo "$1: a VT-d problem reported (above)"
        ok=0
    fi
    if grep -q "skipped" "$out/$1.log"; then
        echo "$1: a test skipped itself"
        ok=0
    fi
}
on vtd-on 1
on vtd-on-cm0 cm0

# A reboot (kexec) with iommu=on: the next kernel finds the queue on,
# drains it, turns it off and sets its own up (6.5.2's takeover).
printf '%s\n' "wait 180 init: the shell is up" "wait jam>" "send reboot" \
    "wait 60 kexec: starting the stored kernel" "wait 60 queued invalidation was on" \
    "wait 60 iommu=on: 1 of 1 unit started" "wait 60 interrupt remapping on: 1 unit" \
    "wait 180 init: the shell is up" "wait jam>" \
    "send reboot -f" "wait reboot: resetting" > "$out/vtd-kexec.txt"
QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_IOMMU=1 QEMU_INPUT="$out/vtd-kexec.txt" \
    tools/qemu-test.sh "$out" vtd-kexec shell iommu=on > "$out/vtd-kexec.out" 2>&1 ||
    { echo "vtd-kexec: QEMU run failed (see $out/vtd-kexec.out)"; ok=0; }
if grep -E "vtd: [^ ]" "$out/vtd-kexec.log"; then
    echo "vtd-kexec: a VT-d problem reported (above)"
    ok=0
fi

if [ "${VTD_TEST_INIT:-0}" = 1 ]; then
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_EXTRA="$iommu" \
        tools/qemu-test.sh "$out" vtd-init init > "$out/vtd-init.out" 2>&1 ||
        { echo "vtd-init: QEMU run failed"; ok=0; }
    have vtd-init "vtd:         handover: 1 of 1 unit answered"
    clean vtd-init
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_EXTRA="$iommu" \
        tools/qemu-test.sh "$out" vtd-init-on init iommu=on > "$out/vtd-init-on.out" 2>&1 ||
        { echo "vtd-init-on: QEMU run failed"; ok=0; }
    have vtd-init-on "vtd:         iommu=on: 1 of 1 unit started"
    clean vtd-init-on
fi

if [ $ok = 1 ]; then
    echo "vtd-test: PASS ($reads register reads, $writes writes)"
else
    echo "vtd-test: FAIL"
    exit 1
fi
