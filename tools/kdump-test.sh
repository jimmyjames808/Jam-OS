#!/bin/sh
# The crash kernel (docs/M8.5-PLAN.md): a panic starts it, it saves the
# crashed kernel's log to the stick and halts or reboots. Each case is a
# fresh boot of a stick image; the image is read afterwards with mtools,
# as the Mac reads the real stick.
#   save      a plain boot, `crash panic yes` once logd writes boot-0001:
#             the crash kernel boots (on one CPU), saves
#             /data/logs/boot-0001-crash.txt (the crashed boot's own
#             number) holding the panic and the lines before it, says so
#             on the screen and in its RESULTS box, and halts
#   reboot    panic_reboot=3: the crash kernel counts down and resets
#             (-no-reboot ends QEMU there); the same stick then boots into
#             the normal kernel, which lists the crash file and takes the
#             next number for its own log
#   bad       `crash kexecbad yes`: a byte of the loaded crash kernel
#             changed, so the panic refuses it (checksum mismatch) and halts
#             as it did before crash kernels; no second kernel boots
#   inner     crashtest=panic: the crash kernel itself panics at boot; it
#             halts on its own panic screen and never tries another kernel
#   nostick   the stick pulled before the panic: the crash kernel says the
#             log was not saved and why, and halts
# QEMU_SMP and QEMU_XHCI pass through. Usage: tools/kdump-test.sh <outdir>
# [case ...]; exit 0 on PASS.
set -u
out=$1
shift
cases=${*:-save reboot bad inner nostick}
mkdir -p "$out"
fails=0

fail() {
    echo "kdump $1: FAILED: $2"
    fails=$((fails + 1))
}

# Count of lines matching an extended regex in a log.
count() {
    grep -cE -- "$2" "$1" 2>/dev/null || true
}

# boot <case> <cmdline> <script lines...>: one QEMU run typed into by the
# script; the stick image is kept as $out/kdump-<case>-stick.img.
boot() {
    c=$1 cmdline=$2
    shift 2
    printf '%s\n' "$@" > "$out/kdump-$c.txt"
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_SAVE="$out/kdump-$c-stick.img" \
        QEMU_INPUT="$out/kdump-$c.txt" tools/qemu-test.sh "$out" "kdump-$c" $cmdline \
        > "$out/kdump-$c.out" 2>&1
}

# The crashed boot's log reached the shell's prompt and logd's file.
UP="wait 120 Jam OS shell|wait jam>|wait 60 logd: writing /data/logs/boot-0001.txt"

case_save() {
    IFS='|'
    set -- $UP "send crash panic yes" "wait 60 KERNEL PANIC" \
        "wait 30 crash kernel: checking it, then starting it" \
        "wait 60 loader:      Jam OS kexec (crash kernel)" \
        "wait 120 crash: the crashed kernel's log is saved as /data/logs/boot-0001-crash.txt" \
        "wait 60 THE KERNEL BEFORE THIS ONE PANICKED" "wait 30 test panic requested" \
        "wait 30 RESULTS" "wait 30 saved as /data/logs/boot-0001-crash.txt" "wait 30 system halted"
    unset IFS
    boot save shell "$@" || { fail save "the script (see $out/kdump-save.log)"; return; }
    log="$out/kdump-save.log"
    [ "$(count "$log" 'smp: 1 of 1 CPUs online')" -ge 1 ] || fail save "the crash kernel ran on more than one CPU"
    f="$out/kdump-save-crash.txt"
    mtype -i "$out/kdump-save-stick.img@@64M" ::/logs/boot-0001-crash.txt > "$f" 2>/dev/null ||
        { fail save "no logs/boot-0001-crash.txt on the stick"; return; }
    mdir -i "$out/kdump-save-stick.img@@64M" ::/logs/boot-0001.txt > /dev/null 2>&1 ||
        fail save "the crashed boot's own log boot-0001.txt is not on the stick"
    for want in "Jam OS crash log: the end of the kernel log of boot-0001" \
                "JAM OS KERNEL PANIC" "test panic requested (crash test)" \
                "dbgcmd: crash panic" "logd: writing /data/logs/boot-0001.txt" \
                "crash kernel: checking it, then starting it"; do
        grep -qF -- "$want" "$f" || fail save "the crash log lacks '$want'"
    done
    # The lines before the panic come before it in the file.
    line_before=$(grep -nF "dbgcmd: crash panic" "$f" | head -1 | cut -d: -f1)
    line_panic=$(grep -nF "JAM OS KERNEL PANIC" "$f" | head -1 | cut -d: -f1)
    [ -n "$line_before" ] && [ -n "$line_panic" ] && [ "$line_before" -lt "$line_panic" ] ||
        fail save "the lines before the panic are not before it"
    python3 tools/fat-label.py "$out/kdump-save-stick.img" 64 > /dev/null 2>&1 ||
        fail save "the data volume's label is damaged"
}

case_reboot() {
    IFS='|'
    set -- $UP "send crash panic yes" "wait 60 KERNEL PANIC" \
        "wait 60 loader:      Jam OS kexec (crash kernel)" \
        "wait 120 crash: the crashed kernel's log is saved as /data/logs/boot-0001-crash.txt" \
        "wait 60 rebooting in" "wait 30 reboot: resetting"
    unset IFS
    boot reboot "shell panic_reboot=3" "$@" || { fail reboot "the script (see $out/kdump-reboot.log)"; return; }
    grep -qF 'crash_reboot=3' "$out/kdump-reboot.log" || fail reboot "the crash kernel didn't get crash_reboot=3"
    # The same stick boots again, into the normal kernel.
    printf '%s\n' "wait 120 Jam OS shell" "wait jam>" "wait 60 logd: writing /data/logs/boot-0002.txt" \
        "send ls /data/logs" "wait boot-0001-crash.txt" "wait jam>" "send reboot -f" \
        "wait reboot: resetting" > "$out/kdump-reboot2.txt"
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_IMAGE="$out/kdump-reboot-stick.img" \
        QEMU_INPUT="$out/kdump-reboot2.txt" tools/qemu-test.sh "$out" kdump-reboot2 shell \
        > "$out/kdump-reboot2.out" 2>&1 ||
        fail reboot "the next boot (see $out/kdump-reboot2.log)"
    grep -qF 'loader:      Limine' "$out/kdump-reboot2.log" || fail reboot "the next boot didn't come from Limine"
}

case_bad() {
    IFS='|'
    set -- $UP "send crash kexecbad yes" "wait 60 KERNEL PANIC" \
        "wait 30 crash kernel: checksum mismatch" "wait 30 system halted"
    unset IFS
    boot bad shell "$@" || { fail bad "the script (see $out/kdump-bad.log)"; return; }
    [ "$(count "$out/kdump-bad.log" 'Jam OS kexec')" -eq 0 ] || fail bad "a crash kernel started anyway"
    mdir -i "$out/kdump-bad-stick.img@@64M" ::/logs/boot-0001-crash.txt > /dev/null 2>&1 &&
        fail bad "a crash log was saved"
}

case_inner() {
    IFS='|'
    set -- $UP "send crash panic yes" "wait 60 KERNEL PANIC" \
        "wait 60 loader:      Jam OS kexec (crash kernel)" "wait 60 test panic requested" \
        "wait 60 system halted"
    unset IFS
    boot inner "shell crashtest=panic" "$@" || { fail inner "the script (see $out/kdump-inner.log)"; return; }
    log="$out/kdump-inner.log"
    [ "$(count "$log" 'JAM OS KERNEL PANIC')" -ge 2 ] || fail inner "the crash kernel didn't panic"
    # Only an armed kernel says this before it jumps: the crash kernel never does.
    [ "$(count "$log" 'crash kernel: checking it')" -eq 1 ] || fail inner "the crash kernel tried another"
    tail -3 "$log" | grep -q "system halted" || fail inner "it didn't end halted on its panic"
}

case_nostick() {
    IFS='|'
    set -- $UP "monitor device_del stick" "wait 30 init: /data is gone" \
        "send crash panic yes" "wait 60 KERNEL PANIC" \
        "wait 60 loader:      Jam OS kexec (crash kernel)" \
        "wait 120 crash: the log was NOT saved: no /data after 30 s" "wait 60 system halted"
    unset IFS
    boot nostick shell "$@" || fail nostick "the script (see $out/kdump-nostick.log)"
}

for c in $cases; do
    had=$fails
    "case_$c"
    [ $fails -eq $had ] && echo "kdump $c: OK"
    rm -f "$out/kdump-$c-stick.img"
done
if [ $fails -eq 0 ]; then
    echo "kdump: PASS"
    exit 0
fi
echo "kdump: FAIL ($fails)"
exit 1
