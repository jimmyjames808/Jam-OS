#!/bin/sh
# M12.7's QEMU test: drv/nvme on emulated NVMe drives: read-only on every
# drive that isn't Jam OS's (stage 1's cases), and writes to a Jam OS
# drive only (stage 2's `write` and `diskro`). Every case keeps a
# Windows-like drive attached (the PC-like image; `4k` and `damaged` its
# 4096-byte and broken-header forms) whose SHA-256 must be the same at the
# end.
# For each case it makes the drive image(s) (tools/nvme-img.py: a
# Windows-like GPT disk like the PC's, a second namespace, 4096-byte
# blocks, a damaged primary GPT), boots the shell with them attached
# (tools/qemu-test.sh's QEMU_NVME; the images are opened writable, so a
# write would land) with QEMU's trace of every NVMe command, and checks:
#   1. the image's SHA-256, taken before and after the run, is identical
#      (the guest wrote nothing);
#   2. the trace (tools/nvme-trace-check.py): every I/O opcode is Read
#      (02h) and every admin opcode is in the stage's list, whoever sent it
#      (OVMF's driver, then drv/nvme); an empty trace fails;
#   3. what the guest says it found, up to the LEVEL of the driver stack
#      that is built (below).
# Cases (default: windows; "all" runs every one in turn; run them in
# parallel with a separate <outdir> each):
#   windows  the PC-like disk: found, listed (4 partitions), the ESP mounted
#            at /nvme0 read-only and a file on it read (its SHA-256 the
#            host's), `disk digest`'s region lines equal
#            tools/disk-regions.py's on the image, `mount -w` refused
#   twons    two namespaces: the second is logged "not served"
#   4k       4096-byte blocks: listed and digested, not mounted (fat is 512 only)
#   damaged  the primary GPT header broken: read from the backup, listed as damaged
#   kill     drv/nvme killed right after a read: its mounts go with it (volumes: "its driver
#            ended"), then devmgr's restart is attached again and /nvme0 and the file are back
#   iommu    the windows case behind QEMU's VT-d (QEMU_IOMMU=1, iommu=on)
#   plain    the windows case booted with no nvmetest= word at all, as the PC's
#            everyday entry boots (devmgr binds drv/nvme on every boot since
#            M12.7's N4): the same checks
#   block    the windows disk under utest's NVMe cases (user/tests/utest/
#            nvme.c: storage, block, 32 reads in flight, a client that leaves
#            with reads out), and their storage.digest region lines equal
#            tools/disk-regions.py's on the image (from level 2 on)
#   write    stage 2: the Windows-like drive and a blank one (640 MiB), with
#            utest's write cases straight at the driver (`utest only
#            nvmew_`, user/tests/utest/nvmewrite.c: volumes bypassed, so
#            every refusal is the driver's): every writable open, write and
#            create_table on the first refused; the second prepared
#            (create_table), written and read back. Then a second boot of
#            the same images: the driver finds the second drive Jam OS's
#            from its own table, create_table refused, the first boot's
#            blocks there, written again. After each boot: the Windows-like
#            image's hash unchanged; the Jam OS image's table right, its
#            written blocks in their places and nothing outside its table and
#            partitions (tools/nvme-written.py); QEMU's trace holds stage 2's
#            opcodes, and every Write lies inside the Jam OS drive's table or
#            partitions (`-trace pci_nvme_write`, --write-ranges)
#   wiommu   the write case behind QEMU's VT-d (QEMU_IOMMU=1, iommu=on): the
#            device reads what it writes through the pins, and no fault
#   diskro   the Windows-like drive, a Jam OS one (tools/mkgpt.py's layout)
#            and a blank one, booted with `diskro`: every drive refuses a
#            writable open and create_table (`utest only nvmero_`), the
#            trace holds stage 1's list only (no Write, no Flush), and all
#            three images are unchanged
#   checker  no QEMU: tools/nvme-trace-check.py on made-up traces (a clean
#            one passes; a Write, a Flush, a Format NVM, a Dataset Management,
#            an unparsable line and a silent trace each fail in stage 1; in
#            stage 2 a Write outside the ranges, or one whose place isn't
#            traced, fails), and tools/disk-regions.py refusing a file
#            without --image ("all" runs it first)
# LEVEL (JAMOS_NVME_LEVEL; by default what the tree has):
#   0  no driver yet: the drive is on the PCI bus (class NVMe), nothing
#      sent it a write, the trace is clean (the firmware's own reads and
#      admin commands)
#   1  drivers/nvme (N1): the controller line is logged, Identify worked,
#      and the boot word nvmetest=selftest makes it read (nothing else
#      reads yet): the namespace's ends are hashed
#   2  + the partition library and serving storage (N3): "GPT, 4
#      partitions, read-only"; the `block` case
#   3  + volumes and the shell (N4: the shell's `disks`): disks, /nvme0,
#      disk digest, mount -w, disk smart, as the plan's P1 types them
# The log lines it greps for are the plan's examples (docs/M12.7-PLAN.md,
# "The design"); when a driver's wording differs, change the patterns in
# this file's L1, L2 and L3 functions (and the typed commands in the
# generated script), nothing else.
# A QEMU other than the PC's 10.0.2 is fine but its trace event format may
# differ: the checker fails on a silent or unparsable trace, never passes it.
# QEMU_TIMEOUT, QEMU_SMP pass through. Nothing here writes to anything but
# files under <outdir>.
# Usage: tools/nvme-test.sh <outdir> [case...]; exit 0 on PASS.
set -eu
out=$1
shift
cases=${*:-windows}
[ "$cases" = all ] && cases="checker windows plain twons 4k damaged kill iommu block write wiommu diskro"
mkdir -p "$out"

sha256() { if command -v sha256sum > /dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }

level=${JAMOS_NVME_LEVEL:-}
if [ -z "$level" ]; then
    level=0
    [ -f drivers/nvme/main.c ] && level=1
    [ $level = 1 ] && [ -f drivers/lib/parttable.c ] && level=2
    [ $level = 2 ] && grep -qs '"disks"' user/services/shell/sh_table.c && level=3
fi

fails=0
checker() {
    name=nvme-checker
    dir="$out/nvme-checker"
    rm -rf "$dir"
    mkdir -p "$dir"
    ok=1
    io() { echo "pci_nvme_io_cmd cid 7 nsid 0x1 sqid 1 opc 0x$1 opname 'X'"; }
    adm() { echo "pci_nvme_admin_cmd cid 7 sqid 0 opc 0x$1 opname 'X'"; }
    { adm 6; adm 5; adm 1; adm 9; adm 2; adm 8; adm 4; adm 0; io 2; io 2; } > "$dir/clean.txt"
    python3 tools/nvme-trace-check.py --require-io 02 --require-admin 06 "$dir/clean.txt" > "$dir/clean.out" ||
        { echo "$name: FAIL: a clean trace failed"; ok=0; }
    # Write, Flush, Dataset Management, Write Zeroes, Format NVM, Security
    # Send, Sanitize, Firmware Commit, Namespace Management, Get Features,
    # Async Event Request
    for bad_one in "io 1" "io 0" "io 9" "io 8" "adm 80" "adm 81" "adm 84" "adm 10" "adm d" "adm c" "adm a"; do
        { adm 6; io 2; $bad_one; } > "$dir/bad.txt"
        if python3 tools/nvme-trace-check.py "$dir/bad.txt" > "$dir/bad.out"; then
            echo "$name: FAIL: '$bad_one' passed the check"
            ok=0
        fi
    done
    { adm 6; echo "pci_nvme_io_cmd cid 7 something new"; } > "$dir/bad.txt"
    ! python3 tools/nvme-trace-check.py "$dir/bad.txt" > "$dir/bad.out" ||
        { echo "$name: FAIL: an unparsable command line passed"; ok=0; }
    : > "$dir/bad.txt"
    ! python3 tools/nvme-trace-check.py "$dir/bad.txt" > "$dir/bad.out" ||
        { echo "$name: FAIL: a silent trace passed"; ok=0; }
    { adm 6; } > "$dir/bad.txt"
    ! python3 tools/nvme-trace-check.py --require-io 02 "$dir/bad.txt" > "$dir/bad.out" ||
        { echo "$name: FAIL: a trace without the Read the test requires passed"; ok=0; }
    # Stage 2: Writes inside the ranges pass; one outside, or one whose
    # place the trace doesn't say, fails.
    wr() { echo "pci_nvme_write cid 7 opname 'NVME_NVM_CMD_WRITE' nsid 1 nlb $2 len 512 lba $1"; }
    { adm 6; io 2; io 1; wr 0x0 1; io 1; wr 0x800 8; io 0; } > "$dir/w.txt"
    python3 tools/nvme-trace-check.py --stage 2 --write-ranges 0-34,2048-4096 "$dir/w.txt" \
        > "$dir/w.out" || { echo "$name: FAIL: Writes inside the ranges failed"; ok=0; }
    { adm 6; io 1; wr 0x7fc 8; } > "$dir/bad.txt"
    ! python3 tools/nvme-trace-check.py --stage 2 --write-ranges 0-34,2048-4096 "$dir/bad.txt" \
        > "$dir/bad.out" || { echo "$name: FAIL: a Write across a range's edge passed"; ok=0; }
    { adm 6; io 1; io 1; wr 0x0 1; } > "$dir/bad.txt"
    ! python3 tools/nvme-trace-check.py --stage 2 --write-ranges 0-34 "$dir/bad.txt" \
        > "$dir/bad.out" || { echo "$name: FAIL: a Write with no place traced passed"; ok=0; }
    { adm 6; io 1; wr 0x0 1; } > "$dir/bad.txt"
    ! python3 tools/nvme-trace-check.py "$dir/bad.txt" > "$dir/bad.out" ||
        { echo "$name: FAIL: a Write passed stage 1"; ok=0; }
    python3 tools/nvme-img.py "$dir/f.img" small
    ! python3 tools/disk-regions.py "$dir/f.img" > "$dir/bad.out" 2>&1 ||
        { echo "$name: FAIL: disk-regions.py read a file without --image"; ok=0; }
    rm -f "$dir/f.img"
    if [ $ok = 1 ]; then echo "$name: PASS"; else fails=$((fails + 1)); fi
}
# ---- stage 2: write and diskro ------------------------------------------------------------

# s2_boot <dir> <name> <utest prefix> <words> <images>: one shell boot that
# runs `utest only <prefix>` with the drives attached and every command
# traced, the Writes with their blocks.
s2_boot() {
    d=$1 bn=$2
    {
        echo "# generated by tools/nvme-test.sh: $bn"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "send utest only $3; echo nvme-utest-\$?"
        echo "wait 240 nvme-utest-"
        echo "wait {prompt}"
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$d/$bn.in"
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-240} QEMU_NVME="$5" QEMU_INPUT="$d/$bn.in" \
        QEMU_EXTRA="-trace pci_nvme_pci_reset -trace pci_nvme_admin_cmd -trace pci_nvme_io_cmd -trace pci_nvme_write -D $d/$bn.trace" \
        QEMU_WORDS="$4" QEMU_IOMMU=${s2_iommu:-} tools/qemu-test.sh "$d" "$bn" shell > "$d/$bn.out" 2>&1 ||
        { echo "$bn: FAIL: the run failed (see $d/$bn.out)"; grep -a "serial-feed: .*no '" "$d/$bn.out" || true; return 1; }
}

# s2_want <log> <regex>...: each line there, or a FAIL line and 1.
s2_want() {
    f=$1
    shift
    r=0
    for re in "$@"; do
        grep -aqE -- "$re" "$f" || { echo "nvme-$c: FAIL: no line matching '$re' in $f"; r=1; }
    done
    return $r
}

# s2_trace <trace> <stage> [--write-ranges ...]: the trace's opcodes (and Writes).
s2_trace() {
    t=$1 stage=$2
    shift 2
    grep -aq "^pci_nvme_pci_reset" "$t" 2> /dev/null || { echo "nvme-$c: FAIL: the trace is dead: $t"; return 1; }
    python3 tools/nvme-trace-check.py --stage "$stage" --require-admin 06 --require-io 02 "$@" "$t" \
        > "$t.check" || { echo "nvme-$c: FAIL: the trace (see $t.check)"; return 1; }
    grep "^nvme-trace: [0-9]" "$t.check" | sed "s/^/nvme-$c: /"
}

s2_write() {
    dir=$1 ok=1 words="nvmetest=on${s2_iommu:+ iommu=on}"
    win="$dir/windows.img" jam="$dir/jamos.img"
    python3 tools/nvme-img.py "$win" windows
    python3 tools/mkgpt.py "$jam" --layout blank --size 640M > /dev/null
    before=$(sha256 "$win" | cut -d' ' -f1)
    for run in 1 2; do
        bn=nvme-write-$run log="$dir/nvme-write-$run.log"
        s2_boot "$dir" $bn nvmew_ "$words" "$win $jam" || ok=0
        if [ -n "${s2_iommu:-}" ] && grep -aq "vtd: fault" "$log"; then echo "nvme-$c: FAIL: a VT-d fault"; ok=0; fi
        [ "$(sha256 "$win" | cut -d' ' -f1)" = "$before" ] || { echo "nvme-$c: FAIL: run $run: the Windows-like image changed"; ok=0; }
        s2_want "$log" "nvme-utest-0" "utest: 4 passed" \
            "nvmew_refuse: drive [0-9]+ \(someone else's, 4 partitions\): every write refused" \
            "nvmew_write: drive [0-9]+: 288 blocks written in both partitions and read back" \
            "GPT, 4 partitions, read-only" || ok=0
        if [ $run = 1 ]; then
            s2_want "$log" "blank, blank: create_table may write its table" \
                "nvmew_prepare: drive [0-9]+ is Jam OS's now" "nvmew_check: drive [0-9]+: nothing written yet" \
                "create_table: Jam OS's table written and read back: GPT, 2 partitions, a Jam OS disk" || ok=0
        else
            s2_want "$log" "GPT, 2 partitions, a Jam OS disk: its two partitions writable" \
                "nvmew_prepare: drive [0-9]+ is Jam OS's already: create_table refused" \
                "nvmew_check: drive [0-9]+: the blocks written by an earlier boot are there" || ok=0
        fi
        python3 tools/nvme-written.py "$jam" --pattern all --ranges > "$dir/written-$run.txt" ||
            { echo "nvme-$c: FAIL: run $run: $(tail -1 "$dir/written-$run.txt")"; ok=0; continue; }
        head -1 "$dir/written-$run.txt" | sed "s/^/nvme-$c: run $run: /"
        s2_trace "$dir/$bn.trace" 2 --write-ranges "$(tail -1 "$dir/written-$run.txt")" || ok=0
    done
    if [ $ok = 1 ]; then rm -f "$win" "$jam"; echo "nvme-$c: PASS"; else echo "nvme-$c: FAIL (see $dir)"; fails=$((fails + 1)); fi
}

s2_diskro() {
    dir=$1 ok=1
    win="$dir/windows.img" jam="$dir/jamos.img" blank="$dir/blank.img"
    python3 tools/nvme-img.py "$win" windows
    python3 tools/mkgpt.py "$jam" --layout jamos --size 640M > /dev/null
    python3 tools/mkgpt.py "$blank" --layout blank --size 640M > /dev/null
    sums=$(sha256 "$win" "$jam" "$blank" | cut -d' ' -f1)
    log="$dir/nvme-diskro.log"
    s2_boot "$dir" nvme-diskro nvmero_ "nvmetest=on diskro" "$win $jam $blank" || ok=0
    [ "$(sha256 "$win" "$jam" "$blank" | cut -d' ' -f1)" = "$sums" ] || { echo "nvme-diskro: FAIL: an image changed"; ok=0; }
    s2_want "$log" "nvme-utest-0" "utest: 1 passed" "nvmero_refuse: drive [0-9]+ \(someone else's\): read-only" \
        "nvmero_refuse: drive [0-9]+ \(blank\): read-only" "nvmero_refuse: drive [0-9]+ \(Jam OS's\): read-only" \
        "the drive is read-only \(the ro word\)" || ok=0
    [ "$(grep -ac 'the drive is read-only (the ro word)' "$log")" -ge 3 ] || { echo "nvme-diskro: FAIL: not every drive said ro"; ok=0; }
    s2_trace "$dir/nvme-diskro.trace" 1 || ok=0
    if [ $ok = 1 ]; then rm -f "$win" "$jam" "$blank"; echo "nvme-diskro: PASS"; else echo "nvme-diskro: FAIL (see $dir)"; fails=$((fails + 1)); fi
}

for c in $cases; do
    if [ $c = checker ]; then checker; continue; fi
    if [ $c = write ] || [ $c = wiommu ] || [ $c = diskro ]; then
        rm -rf "$out/nvme-$c"
        mkdir -p "$out/nvme-$c"
        s2_iommu=
        [ $c = wiommu ] && s2_iommu=1
        case $c in diskro) s2_diskro "$out/nvme-$c" ;; *) s2_write "$out/nvme-$c" ;; esac
        continue
    fi
    name=nvme-$c
    dir="$out/$name"
    rm -rf "$dir"
    mkdir -p "$dir"
    log="$dir/$name.log" trace="$dir/$name.trace"
    ok=1
    say() { echo "$name: $*"; }
    bad() { say "FAIL: $*"; ok=0; }
    want() { grep -aqE -- "$1" "$log" || bad "no line matching '$1'"; }   # want <regex>

    # ---- the images -------------------------------------------------------------------------
    bs=512 props= words= iommu= dmg=
    img="$dir/disk.img" imgs="$img"
    case $c in
    windows | kill | plain) ;;
    block) words="nvmetest=slow" ;;   # answers 20 ms apart: the drain rule is seen
    iommu) iommu=1 words="iommu=on" ;;
    twons) imgs="$img+$dir/ns2.img" ;;
    4k) bs=4096 props=",logical_block_size=4096,physical_block_size=4096" ;;
    damaged) dmg="--damage primary-header" ;;
    *) echo "nvme-test: no case '$c'"; exit 2 ;;
    esac
    # Level 1 has no client that reads: the driver's test word reads the
    # namespace's ends (drivers/nvme/selftest.c), so the trace has Reads.
    [ $level = 1 ] && words="${words:+$words }nvmetest=selftest"
    # nvmetest=on: no test words (drv/nvme binds on every boot; the plain case gives none).
    case " $words " in *" nvmetest="*) ;; *) [ $c = plain ] || words="${words:+$words }nvmetest=on" ;; esac
    # shellcheck disable=SC2086
    python3 tools/nvme-img.py "$img" windows --block-size $bs $dmg
    before2=
    if [ $c = twons ]; then
        python3 tools/nvme-img.py "$dir/ns2.img" small
        before2=$(sha256 "$dir/ns2.img" | cut -d' ' -f1)
    fi
    before=$(sha256 "$img" | cut -d' ' -f1)
    boot_hash=$(sha256 "$img.files/bootmgfw.efi" | cut -d' ' -f1)
    python3 tools/disk-regions.py --image --block-size $bs "$img" | grep -v '^#' > "$dir/regions-host.txt"

    # ---- the shell script the guest is given ------------------------------------------------
    in="$dir/$name.in"
    {
        echo "# generated by tools/nvme-test.sh for case $c, level $level"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        if [ $c = block ] && [ $level -ge 2 ]; then
            echo "send utest only nvme_; echo nvme-utest-\$?"
            echo "wait 240 passed"
            echo "wait {prompt}"
        fi
        if [ $level -ge 3 ] && [ $c != block ]; then
            echo "send disks"
            echo "wait nvme0"
            echo "wait {prompt}"
            if [ $c != 4k ]; then
                echo "send mount"
                echo "wait /nvme0"
                echo "wait {prompt}"
                echo "send ls /nvme0/EFI/Microsoft/Boot"
                echo "wait BCD"   # ls sorts upper case first: BCD, then bootmgfw.efi
                echo "wait bootmgfw.efi"
                echo "wait {prompt}"
                if [ $c = kill ]; then
                    # The driver's end takes the mounts with it (volumes: "its driver
                    # ended"); they are back once devmgr's restart has attached the
                    # disk again, a moment later.
                    echo "send ls /nvme0 > /dev/null; kill nvme; sleep 2; sha256sum /nvme0/EFI/Microsoft/Boot/bootmgfw.efi"
                else
                    echo "send sha256sum /nvme0/EFI/Microsoft/Boot/bootmgfw.efi"
                fi
                echo "wait $boot_hash"
                echo "wait {prompt}"
                echo "send mount -w /nvme0"
                echo "wait not on a Jam OS disk"
                echo "wait {prompt}"
            fi
            echo "send disk smart nvme0"
            echo "wait {prompt}"
            echo "send disk digest nvme0"
            echo "wait 120 disk digest: done"
            echo "wait {prompt}"
            echo "send disk smart nvme0"
            echo "wait {prompt}"
        fi
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$in"

    # ---- the run ----------------------------------------------------------------------------
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-240} QEMU_NVME="$imgs$props" QEMU_INPUT="$in" \
        QEMU_EXTRA="-trace pci_nvme_pci_reset -trace pci_nvme_admin_cmd -trace pci_nvme_io_cmd -D $trace" \
        QEMU_IOMMU=$iommu QEMU_WORDS="$words" \
        tools/qemu-test.sh "$dir" "$name" shell > "$dir/$name.out" 2>&1 ||
        { bad "the run failed (see $dir/$name.out)"; grep -a "serial-feed: .*no '" "$dir/$name.out" || true; }

    # ---- 1. the image is unchanged ----------------------------------------------------------
    after=$(sha256 "$img" | cut -d' ' -f1)
    [ "$before" = "$after" ] || bad "the drive image changed: $before -> $after"
    if [ $c = twons ] && [ "$before2" != "$(sha256 "$dir/ns2.img" | cut -d' ' -f1)" ]; then
        bad "the second namespace's image changed"
    fi

    # ---- 2. the trace has only allowed opcodes ----------------------------------------------
    # The device's reset at power-on is traced too: proof that the trace is
    # on and was written out, whether or not anything sent a command. Nothing
    # talks to the drive until drv/nvme runs (the firmware boots the stick and
    # leaves the controller alone): at level 0 no command is expected; from
    # level 1 on the checker fails on a trace with none (and with no Identify
    # and no Read).
    grep -aq "^pci_nvme_pci_reset" "$trace" 2> /dev/null || bad "the trace is dead: no device reset in $trace"
    req=
    [ $level -ge 1 ] && req="--require-admin 06 --require-io 02"
    if [ $level -ge 1 ] || grep -aqE "^pci_nvme_(admin|io)_cmd" "$trace" 2> /dev/null; then
        # shellcheck disable=SC2086
        python3 tools/nvme-trace-check.py --stage 1 $req "$trace" > "$dir/trace-check.txt" ||
            bad "the trace has an opcode stage 1 never sends, or no command (see $dir/trace-check.txt)"
        tail -1 "$dir/trace-check.txt" | sed "s/^/$name: /"
    else
        say "no NVMe command in the trace: nothing has talked to the drive (no driver yet)"
    fi

    # ---- 3. what the guest found ------------------------------------------------------------
    want "NVMe"   # level 0: the PCI report names the function
    L1() {
        want "nvme [0-9a-f:.]+: .*namespace"
        if [ $level = 1 ]; then want "selftest: both ends read and hashed"; fi
        if [ $c = twons ]; then want "namespace 2: not served"; fi
    }
    L2() {
        case $c in
        damaged) want "primary GPT damaged" ;;
        *) want "GPT, 4 partitions, read-only" ;;
        esac
    }
    L3() {
        if [ $c = 4k ]; then want "no FAT"; else want "EFI system"; fi
        if [ $c = plain ] && grep -aq "hello from ring 3.*nvmetest" "$log"; then bad "the plain case was given an nvmetest= word"; fi
        if [ $c = kill ]; then want "nvme0: its driver ended"; want "nvme0 partition 1 is /nvme0, read-only"; fi
        # `disk digest`'s lines against Linux's tool on the same image
        sed -nE 's/.*((gpt-head|part[0-9]+(-[a-z0-9]+)?|gpt-tail) lba [0-9]+ count [0-9]+ sha256 [0-9a-f]{64}).*/\1/p' \
            "$log" > "$dir/regions-guest.txt"
        cmp -s "$dir/regions-guest.txt" "$dir/regions-host.txt" ||
            bad "disk digest differs from tools/disk-regions.py (diff $dir/regions-guest.txt $dir/regions-host.txt)"
    }
    # utest's NVMe cases (user/tests/utest/nvme.c), and their region lines
    # against the host's.
    LB() {
        want "nvme-utest-0"
        want "utest: 4 passed"
        sed -nE 's/.*utest: nvme region ((gpt-head|part[0-9]+|gpt-tail) lba [0-9]+ count [0-9]+ sha256 [0-9a-f]{64}).*/\1/p' \
            "$log" > "$dir/regions-utest.txt"
        [ "$(wc -l < "$dir/regions-utest.txt")" -eq 4 ] || bad "not 4 region lines from utest"
        while read -r line; do
            grep -qxF "$line" "$dir/regions-host.txt" || bad "utest's '$line' isn't the host's"
        done < "$dir/regions-utest.txt"
        # the client that left with reads out: dropped, the partition drained
        want "partition 1: its client left with [1-9][0-9]* request\(s\) out"
        want "partition 1: the gone client's requests are done"
    }
    if [ $level -ge 1 ]; then L1; fi
    if [ $level -ge 2 ]; then L2; fi
    if [ $level -ge 2 ] && [ $c = block ]; then LB; fi
    if [ $level -ge 3 ] && [ $c != block ]; then L3; fi
    if [ $c = iommu ] && grep -aq "vtd: fault" "$log"; then bad "a VT-d fault"; fi

    if [ $ok = 1 ]; then
        rm -f "$img" "$dir/ns2.img"
        say "PASS (level $level)"
    else
        say "FAIL (see $dir)"
        fails=$((fails + 1))
    fi
done
[ $fails = 0 ]
