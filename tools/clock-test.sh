#!/bin/sh
# The real date and time, and the settings that survive a reboot: one
# stick image, two boots, the RTC at 2026-01-15 01:02:03.
#   boot 1  clock-1.txt: Sydney's time from the RTC (taken as local time,
#           the default), /data/etc/settings made with the defaults, a
#           file written, `vol master`, `music vol` and `date -z` kept in
#           the settings, then `reboot` by kexec: the next kernel's init
#           sets the clock in Perth's time, the volumes come back
#   boot 2  clock-2.txt: after `rtc = utc` is put into the settings from
#           this side, the clock reads the RTC as UTC
# Then, with mtools, as the Mac sees the stick: the file boot 1 wrote is
# dated 2026-01-15 01:02 (Sydney's local time: FAT keeps local time),
# boot-0001.txt starts with the line that dates it, and the settings file
# holds what was set (no settings.new left).
# QEMU_SMP passes through.
# Usage: tools/clock-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-clock}
mkdir -p "$out"
stick="$out/$name-stick.img"
data="$stick@@64M"
rtc="-rtc base=2026-01-15T01:02:03"
ok=1
if ! QEMU_TIMEOUT=${QEMU_TIMEOUT:-400} QEMU_EXTRA="$rtc" QEMU_SAVE="$stick" \
     QEMU_INPUT=tools/shell-tests/clock-1.txt tools/qemu-test.sh "$out" "$name-1" shell \
     > "$out/$name-1.out" 2>&1; then
    echo "$name: boot 1 FAILED (see $out/$name-1.log)"
    tail -2 "$out/$name-1.out"
    ok=0
fi
if [ $ok = 1 ]; then
    mdir -i "$data" ::/stamp.txt 2>/dev/null | grep -q "2026-01-15 *1:0[23]" ||
        { echo "$name: stamp.txt isn't dated 2026-01-15 1:02 on the stick:"; mdir -i "$data" ::/stamp.txt; ok=0; }
    mtype -i "$data" ::/logs/boot-0001.txt 2>/dev/null | head -1 |
        grep -q "^Jam OS boot log boot-0001.txt: the kernel started at Thu 15 Jan 2026 01:0[0-9]:[0-9][0-9] AEDT (UTC+11:00)" ||
        { echo "$name: boot-0001.txt doesn't start with its date:"; mtype -i "$data" ::/logs/boot-0001.txt | head -1; ok=0; }
    mtype -i "$data" ::/etc/settings > "$out/$name-settings.txt" 2>/dev/null || true
    for want in "timezone = Australia/Perth" "volume = -12.0" "music.volume = -6.0" "rtc = local"; do
        grep -qx "$want" "$out/$name-settings.txt" ||
            { echo "$name: the settings file has no line '$want'"; ok=0; }
    done
    ! mdir -i "$data" ::/etc/settings.new > /dev/null 2>&1 ||
        { echo "$name: settings.new was left on the stick"; ok=0; }
    sed 's/^rtc = local$/rtc = utc/' "$out/$name-settings.txt" > "$out/$name-settings2.txt"
    mcopy -o -i "$data" "$out/$name-settings2.txt" ::/etc/settings
fi
if [ $ok = 1 ] && ! QEMU_TIMEOUT=${QEMU_TIMEOUT:-400} QEMU_EXTRA="$rtc" QEMU_IMAGE="$stick" \
     QEMU_INPUT=tools/shell-tests/clock-2.txt tools/qemu-test.sh "$out" "$name-2" shell \
     > "$out/$name-2.out" 2>&1; then
    echo "$name: boot 2 FAILED (see $out/$name-2.log)"
    tail -2 "$out/$name-2.out"
    ok=0
fi
rm -f "$stick" "$out/$name-settings2.txt"
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL"
exit 1
