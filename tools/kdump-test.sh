#!/bin/sh
# A panic starts the stored kernel (docs/history/M8.5-PLAN.md, "Revision 2"): a
# fresh copy of Jam OS whose boot is a normal one, except that logd first
# saves the panicked boot's log and the shell prints one line about it.
# Each case is a fresh boot of a stick image; the image is read afterwards
# with mtools, as the Mac reads the real stick.
#   save     a plain boot, `crash panic yes` once logd writes boot-0001:
#            no panic screen, the next boot comes up on every CPU, its
#            shell says "the last boot panicked: ... (saved as
#            /data/logs/boot-0001-crash.txt)", and that file (the panicked
#            boot's own number) holds the panic and the lines before it;
#            the new boot logs to boot-0002
#   loop     crashtest=lockorder: the next boot panics at once (its test
#            word), within 30 s of a start after a panic: it halts on its
#            panic screen (red), saying it is a crash loop; no third kernel
#   bad      `crash kexecbad yes`: a byte of the stored kernel changed, so
#            the panic refuses it (checksum) and halts on its red screen;
#            no second kernel, no crash file
#   nostick  the stick pulled before the panic: the next boot comes up
#            without it, and its shell says the log was not saved and why
#   screen   with the splash (QEMU_SPLASH=1): the screen as the next
#            kernel starts is all the splash background (no panic screen,
#            no text), and the next boot plays the splash before the
#            shell's line
# QEMU_SMP and QEMU_XHCI pass through. Usage: tools/kdump-test.sh <outdir>
# [case ...]; exit 0 on PASS.
set -u
out=$1
shift
cases=${*:-save loop bad nostick screen}
mkdir -p "$out"
fails=0
cpus=${QEMU_SMP:-4}

fail() {
    echo "kdump $1: FAILED: $2"
    fails=$((fails + 1))
}

# Count of lines matching an extended regex in a log.
count() {
    grep -acE -- "$2" "$1" 2>/dev/null || true
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

# The panicking boot's log reached the shell's prompt and logd's file.
UP="wait 120 Jam OS shell|wait {prompt}|wait 60 logd: writing /data/logs/boot-0001.txt"

case_save() {
    IFS='|'
    set -- $UP "send crash panic yes" "wait 60 KERNEL PANIC" \
        "wait 30 starting the stored kernel: the next boot saves this log" \
        "wait 60 loader:      Jam OS kexec" \
        "wait 60 kexec: the previous kernel panicked after" \
        "wait 120 the last boot panicked: test panic requested (crash test) (saved as /data/logs/boot-0001-crash.txt)" \
        "wait {prompt}" "seen 60 logd: writing /data/logs/boot-0002.txt" \
        "send ls /data/logs" "wait boot-0002.txt" "wait {prompt}" \
        "send reboot -f" "wait reboot: resetting"
    unset IFS
    boot save shell "$@" || { fail save "the script (see $out/kdump-save.log)"; return; }
    log="$out/kdump-save.log"
    [ "$(count "$log" "smp: $cpus of $cpus CPUs online")" -ge 2 ] ||
        fail save "the next boot didn't bring up all $cpus CPUs"
    [ "$(count "$log" 'system halted')" -eq 0 ] || fail save "something halted"
    grep -aqE 'kexec: stored kernel armed, command line "shell nosplash( bootdisk=[0-9]+)?"' \
        "$log" ||
        fail save "the stored kernel's command line isn't the plain boot's"
    f="$out/kdump-save-crash.txt"
    data="$out/kdump-save-stick.img@@64M"
    mtype -i "$data" ::/logs/boot-0001-crash.txt > "$f" 2>/dev/null ||
        { fail save "no logs/boot-0001-crash.txt on the stick"; return; }
    for n in boot-0001.txt boot-0002.txt; do
        mdir -i "$data" ::/logs/$n > /dev/null 2>&1 || fail save "no logs/$n on the stick"
    done
    for want in "Jam OS crash log: the end of the kernel log of boot-0001, which panicked" \
                "The panic: test panic requested (crash test)" \
                "JAM OS KERNEL PANIC" "dbgcmd: crash panic" \
                "logd: writing /data/logs/boot-0001.txt" \
                "starting the stored kernel: the next boot saves this log"; do
        grep -qF -- "$want" "$f" || fail save "the crash log lacks '$want'"
    done
    # The lines before the panic come before it in the file.
    line_before=$(grep -nF "dbgcmd: crash panic" "$f" | head -1 | cut -d: -f1)
    line_panic=$(grep -nF "JAM OS KERNEL PANIC" "$f" | head -1 | cut -d: -f1)
    [ -n "$line_before" ] && [ -n "$line_panic" ] && [ "$line_before" -lt "$line_panic" ] ||
        fail save "the lines before the panic are not before it"
    mtype -i "$data" ::/logs/boot-0002.txt 2>/dev/null |
        grep -q "logd: the last boot's log is saved as /data/logs/boot-0001-crash.txt" ||
        fail save "boot-0002.txt doesn't say where the last boot's log went"
    python3 tools/fat-label.py "$out/kdump-save-stick.img" 64 > /dev/null 2>&1 ||
        fail save "the data volume's label is damaged"
}

case_loop() {
    IFS='|'
    set -- $UP "send crash panic yes" "wait 60 KERNEL PANIC" \
        "wait 60 loader:      Jam OS kexec" "wait 60 KERNEL PANIC" \
        "wait 30 no restart: this boot started after a panic less than 30 s ago (a crash loop)" \
        "wait 30 system halted"
    unset IFS
    boot loop "shell crashtest=lockorder" "$@" || { fail loop "the script (see $out/kdump-loop.log)"; return; }
    log="$out/kdump-loop.log"
    grep -aqE 'kexec: stored kernel armed, command line "shell nosplash testlockorder( bootdisk=[0-9]+)?"' \
        "$log" ||
        fail loop "the stored kernel didn't get the test word"
    [ "$(count "$log" 'JAM OS KERNEL PANIC')" -eq 2 ] || fail loop "not exactly two panics"
    [ "$(count "$log" 'loader:      Jam OS kexec')" -eq 1 ] || fail loop "a third kernel started"
    python3 tools/splash-check.py red "$out/kdump-loop.png" > /dev/null ||
        fail loop "the crash loop's screen isn't the red panic screen"
}

case_bad() {
    IFS='|'
    set -- $UP "send crash kexecbad yes" "wait 60 KERNEL PANIC" \
        "wait 30 no restart: the stored kernel's checksum no longer matches" \
        "wait 30 system halted"
    unset IFS
    boot bad shell "$@" || { fail bad "the script (see $out/kdump-bad.log)"; return; }
    [ "$(count "$out/kdump-bad.log" 'Jam OS kexec')" -eq 0 ] || fail bad "a second kernel started anyway"
    mdir -i "$out/kdump-bad-stick.img@@64M" ::/logs/boot-0001-crash.txt > /dev/null 2>&1 &&
        fail bad "a crash log was saved"
    python3 tools/splash-check.py red "$out/kdump-bad.png" > /dev/null ||
        fail bad "the screen isn't the red panic screen"
}

case_nostick() {
    IFS='|'
    set -- $UP "monitor device_del stick" "wait 30 init: /data is gone" \
        "send crash panic yes" "wait 60 KERNEL PANIC" "wait 60 loader:      Jam OS kexec" \
        "wait 120 the last boot panicked: test panic requested (crash test) (not saved: no /data within 20 s" \
        "wait {prompt}" "send reboot -f" "wait reboot: resetting"
    unset IFS
    boot nostick shell "$@" || fail nostick "the script (see $out/kdump-nostick.log)"
}

case_screen() {
    printf '%s\n' "wait 180 init: the shell is up" "wait {prompt}" "seen 60 logd: writing /data/logs/boot-0001.txt" \
        "sleep 1" "send crash panic yes" "wait 60 starting the stored kernel" \
        "wait 60 loader:      Jam OS kexec" "shot kdump-between" "wait 120 splash: first frame" \
        "wait 180 the last boot panicked: test panic requested" "wait 60 init: the shell is up" \
        "wait {prompt}" "send reboot -f" "wait reboot: resetting" > "$out/kdump-screen.txt"
    QEMU_SPLASH=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT="$out/kdump-screen.txt" \
        tools/qemu-test.sh "$out" kdump-screen shell > "$out/kdump-screen.out" 2>&1 ||
        { fail screen "the script (see $out/kdump-screen.log)"; return; }
    python3 tools/splash-check.py quiet "$out/kdump-between.png" ||
        fail screen "the screen between the kernels isn't all the splash background"
    grep -aq "quiet for the boot splash" "$out/kdump-screen.log" ||
        fail screen "the next boot didn't start quiet"
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
