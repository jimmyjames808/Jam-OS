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
#      every CPU at once, QI off and on), and nothing in the RESULTS box
#      but the refusal the test provokes;
#      With them DMA translation is on (kernel/dev/vtd_domain.c,
#      vtd_boot.c): every function in its home (blocking) domain, and every
#      vtd_domain_* test passed (edu blocked and its fault seen, passed
#      through, in a domain of its own, muted, the handover in flight);
#      the only problems reported are edu's own (00:04.0) faults;
#   6. the same tests with pass-through off (pt=off): drivers' devices get
#      an identity map of all RAM instead;
#   7. a shell boot with iommu=on, `reboot` (kexec): the jump turns
#      translation and the queue off, and the next kernel finds them off,
#      starts the unit and turns translation on again; nothing reported;
#   8. the same with a panic (`crash panic yes`): the panic's jump does the
#      same, and the next kernel comes up with translation on.
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

# domain_ok <name>: every vtd_domain_* test passed in <name>'s log.
domain_ok() {
    have "$1" "vtd:         unit 0: 00:04.0 1234:11e8 class 00ff: blocking, domain 1" \
        "ktest: vtd_domain_entry_bits_literal    ok" \
        "ktest: vtd_domain_rmrr_carve            ok" \
        "ktest: vtd_domain_did_alloc             ok" \
        "ktest: vtd_domain_translation_on        ok" \
        "ktest: vtd_domain_blocked_dma_faults    ok" \
        "ktest: vtd_domain_pass_dma_lands        ok" \
        "ktest: vtd_domain_own_domain            ok" \
        "ktest: vtd_domain_mute_after_faults     ok" \
        "ktest: vtd_domain_handover_while_on     ok"
}

# problems <name>: the VT-d problems reported, but for the refusal and
# edu's faults the tests provoke.
problems() {
    grep -E "vtd: [^ ]" "$out/$1.log" | grep -vF "the queue refused descriptor" |
        grep -vE "vtd: fault: unit 0: 00:04\.0 "
}

# on <name> <QEMU_IOMMU>: the units started, translation on, their ktests.
on() {
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_IOMMU=$2 \
        tools/qemu-test.sh "$out" "$1" ktest=vtd iommu=on > "$out/$1.out" 2>&1 ||
        { echo "$1: QEMU run failed (see $out/$1.out)"; ok=0; }
    domain_ok "$1"
    have "$1" "vtd:         unit 0: translation on (it was off): " "domain 2 (pass-through)"
    have "$1" "vtd:         unit 0: started: invalidation queue at" \
        "vtd:         iommu=on: 1 of 1 unit started" \
        "ktest: vtd_unit_every_invalidation_completes ok" \
        "ktest: vtd_unit_callbacks_invalidate    ok" \
        "ktest: vtd_unit_refused_descriptor_reported ok" \
        "ktest: vtd_unit_queue_wraps             ok" \
        "ktest: vtd_unit_many_cpus_at_once       ok" \
        "ktest: vtd_unit_queue_off_and_on        ok" \
        "ktest: vtd_unit_registers_kept          ok" \
        "run complete: no problems"
    if problems "$1"; then
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

# Without pass-through (pt=off): the identity domain of all RAM.
QEMU_TIMEOUT=${QEMU_TIMEOUT:-150} QEMU_EXTRA="-device intel-iommu,intremap=on,caching-mode=on,pt=off" \
    tools/qemu-test.sh "$out" vtd-nopt ktest=vtd_domain iommu=on > "$out/vtd-nopt.out" 2>&1 ||
    { echo "vtd-nopt: QEMU run failed (see $out/vtd-nopt.out)"; ok=0; }
domain_ok vtd-nopt
have vtd-nopt "pt 0," "driven ones go to domain 2 (identity (all RAM))" "run complete: no problems"
if problems vtd-nopt; then
    echo "vtd-nopt: a VT-d problem reported (above)"
    ok=0
fi

# jump <name> <command> <what the old kernel says>: a shell boot with
# iommu=on, then <command> into the stored kernel (kexec). The jump turned
# translation and the queue off, so the next kernel finds the unit as a
# cold boot leaves it, starts it and turns translation on again; the
# devices (the stick, the shell) work in both kernels.
jump() {
    printf '%s\n' "wait 180 init: the shell is up" "wait jam>" "send $2" "wait 60 $3" \
        "wait 60 loader:      Jam OS kexec" "wait 60 iommu=on: 1 of 1 unit started" \
        "wait 60 unit 0: translation on (it was off)" "wait 180 init: the shell is up" \
        "wait jam>" "send reboot -f" "wait reboot: resetting" > "$out/$1.txt"
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_IOMMU=1 QEMU_INPUT="$out/$1.txt" \
        tools/qemu-test.sh "$out" "$1" shell iommu=on > "$out/$1.out" 2>&1 ||
        { echo "$1: QEMU run failed (see $out/$1.out)"; ok=0; }
    n=$(grep -c "unit 0 status: translation off, interrupt remapping off, queued invalidation off" \
        "$out/$1.log" || true)
    [ "$n" -eq 2 ] || { echo "$1: $n boots found the unit all off, want 2"; ok=0; }
    if grep -E "vtd: [^ ]" "$out/$1.log"; then
        echo "$1: a VT-d problem reported (above)"
        ok=0
    fi
}
jump vtd-kexec reboot "kexec: starting the stored kernel"
jump vtd-panic "crash panic yes" "KERNEL PANIC"

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
