#!/bin/sh
# `storm` in QEMU (docs/HISTORY.md, M11.6;
# user/services/shell/cmd/storm.c): tools/shell-tests/storm.txt copies a
# file of random bytes from a second stick (/usb0, made writable) to the
# boot stick's /data at 0, 1, 10 and 100 kills a second, killing fat-usb0
# and fat-data in turn (/data holds one copy at a time: each but the last
# is removed after its line), with one more at 100 stopped by Ctrl+C; then
# `storm mixer 2 10` while the music player plays a WAV from /data/music.
# Checked:
#   - every copy's line says MATCH, and its two SHA-256 lines are the
#     host's for the file it made; the kills were done (none refused, each
#     answered after), more of them the higher the rate; Ctrl+C said, 130;
#   - on the host afterwards (mtools), the last copy on the boot stick's
#     /data is the file, byte for byte;
#   - tools/fatcheck.py on both disks: /data (partition 2 of the boot
#     stick) and the second stick: clean, no lost clusters, and marked
#     clean (--require-clean: everything synced at the reboot);
#   - the mixer: one restart line per kill, each with the output running
#     and the least lead left above the mixer's guard (256 frames); no
#     late period, in storm's line, in the log, and in the music stream's
#     own close line.
# Prints the four storm lines and the mixer's, and fsck_msdos's verdict on
# both disks too where the Mac has it (read-only attach; not part of PASS:
# mtools' volume label alone makes it warn).
# The kills must land DURING the copies, or the test proves nothing: storm
# starts its kill thread before the copy and stops it right after (the
# read-back hashing runs with no kills), so the kills in a storm line are
# the copy's own, and the first of them comes one period in. A copy of 32
# MiB took 1.8 s on the PC (18 MB/s), so at 1 kill a second it was killed
# once, at the very end. Now the file is 64 MiB (about 3.6 s; about 7 s at
# 100 kills a second) and each rate's line is checked: at least so many
# kills (1 a second: 2, so both services die mid-copy; 10: 10; 100: 50) and
# at least 40% of the kills the rate asks for over the copy's seconds; the
# Ctrl+C copy must stop with the file unfinished and kills done. A machine
# too fast for the file fails with the numbers: raise STORM_MIB.
# QEMU_SMP passes through; STORM_MIB sets the file's size (default 64; the
# boot stick's /data and the second stick are made big enough for it).
# Usage: tools/fat-storm-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
mib=${STORM_MIB:-64}
# The image's /data (64 MiB) and a 64 MiB stick are too small for a copy
# that size: the boot image's data partition grows to the file, the WAV
# (6 MiB) and room for the logs, the stick to the file and some.
esp_mib=64
data_mib=$((mib + 32))
boot_mib=$((esp_mib + data_mib))
stick_mib=$((mib + 32))
boot="$out/storm-boot.img"       # the boot stick, with the WAV on /data
saved="$out/storm-boot-after.img"  # ... as the guest left it
stick="$out/storm-usb0.img"       # the second stick, /usb0
tmp="$out/storm-files"
rm -rf "$tmp" "$saved"
mkdir -p "$tmp"
ok=1

python3 - "$tmp" "$mib" <<'PY'
import math, random, struct, sys
out, mib = sys.argv[1], int(sys.argv[2])
open("%s/big.bin" % out, "wb").write(random.Random(11).randbytes(mib << 20))
rate, secs, n = 48000, 30, 48000 * 30   # a 440 Hz tone, a quarter of full scale, stereo
frames = b"".join(struct.pack("<hh", v, v) for v in
                  (int(8191 * math.sin(2 * math.pi * 440 * k / rate)) for k in range(n)))
fmt = struct.pack("<HHIIHH", 1, 2, rate, rate * 4, 4, 16)
body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", len(frames)) + frames
open("%s/tone.wav" % out, "wb").write(b"RIFF" + struct.pack("<I", len(body)) + body)
PY
want=$(shasum -a 256 "$tmp/big.bin" | cut -c1-64)

python3 tools/mkstick.py "$stick" "$stick_mib" 0c
mformat -i "$stick@@1M" -T $(((stick_mib - 1) * 2048)) -F -v STORM ::
mcopy -i "$stick@@1M" "$tmp/big.bin" ::/big.bin
# The boot image with its data partition grown (as tools/write-usb.sh does)
# and the volume made here as the Makefile makes it (mbr-grow's wipe is
# not needed: nothing is on it yet).
cp build/jamos.img "$boot"
python3 -c "import sys; open(sys.argv[1], 'r+b').truncate(int(sys.argv[2]) << 20)" "$boot" "$boot_mib"
python3 tools/mbr-grow.py "$boot" $((boot_mib * 2048))
mformat -i "$boot@@${esp_mib}M" -T $((data_mib * 2048)) -F -v JAMOS-DATA ::
python3 tools/fat-label.py "$boot" "$esp_mib" --fix
mmd -i "$boot@@64M" ::/music
mcopy -i "$boot@@64M" "$tmp/tone.wav" "::/music/tone.wav"

devs="-audiodev wav,id=snd0,path=$out/storm.wav,out.frequency=48000,out.channels=2,out.format=s16 \
-device intel-hda,id=hda0 -device hda-output,bus=hda0.0,cad=0,audiodev=snd0"
QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} QEMU_IMAGE="$boot" QEMU_SAVE="$saved" QEMU_EXTRA="$devs" \
    QEMU_USB="-drive if=none,id=s0img,format=raw,file=$stick " \
    QEMU_INPUT=tools/shell-tests/storm.txt \
    tools/qemu-test.sh "$out" storm shell > "$out/storm.out" 2>&1 ||
    { echo "fat-storm: the script failed"; grep "serial-feed: .*no '" "$out/storm.out" || true; ok=0; }
log="$out/storm.log"
clean="$out/storm.clean.log"
tr -d '\r' < "$log" > "$clean"

count() { grep -ac -- "$1" "$2" || true; }
never() {   # never <text> <log>
    n=$(count "$1" "$2")
    [ "$n" -eq 0 ] || { echo "fat-storm: \"$1\" $n times, wanted none"; ok=0; }
}

# ---- the copies ------------------------------------------------------------------------------
# Did the kills land during the copy? storm's kills are the copy's own (see
# the top): `landed <rate> <least kills> <storm line>` is true when the line
# has at least that many, and at least 40% of rate x the copy's seconds
# (they come evenly from the copy's start, so that is a copy killed all
# along, not just at its very end). Says what it found when not.
landed() {
    k=$(echo "$3" | grep -ao "; [0-9]* kills" | grep -ao "[0-9]*" || echo 0)
    secs=$(echo "$3" | grep -ao " bytes in [0-9]*\.[0-9]* s" | grep -ao "[0-9]*\.[0-9]*" || echo 0)
    awk -v k="${k:-0}" -v r="$1" -v m="$2" -v s="${secs:-0}" \
        'BEGIN { exit !(k >= m && k >= 0.4 * r * s) }' && return 0
    echo "fat-storm: $1 kills/s: ${k:-0} kills in a ${secs:-0} s copy, wanted at least $2 and" \
         "$(awk -v r="$1" -v s="${secs:-0}" 'BEGIN { printf "%.1f", 0.4 * r * s }')" \
         "(the kills didn't land during the copy: raise STORM_MIB, now $mib)"
    return 1
}
# A storm line is on the screen and in the kernel log: both reach the
# serial port, so each is there twice; the first is taken.
prev=-1
for rate in 0 1 10 100; do
    line=$(grep -a "^storm: $rate kills/s: [0-9]* bytes" "$clean" | head -1)   # not the Ctrl+C one
    echo "fat-storm: ${line:-no line at $rate kills/s}"
    case $line in
    *"; MATCH") ;;
    *) echo "fat-storm: the copy at $rate kills/s doesn't say MATCH"; ok=0 ;;
    esac
    got=$(grep -ao "^[0-9a-f]\{64\}  /data/s$rate.bin\$" "$clean" | head -1 | cut -c1-64)
    [ "$got" = "$want" ] ||
        { echo "fat-storm: $rate kills/s: /data/s$rate.bin's SHA-256 is \"$got\", the file's $want"; ok=0; }
    kills=$(echo "$line" | grep -ao "; [0-9]* kills" | grep -ao "[0-9]*" || echo 0)
    if [ "$rate" = 0 ]; then
        echo "$line" | grep -q "; no kills;" || { echo "fat-storm: 0 kills/s killed something"; ok=0; }
    elif [ "${kills:-0}" -lt 1 ] || [ "${kills:-0}" -le "$prev" ]; then
        echo "fat-storm: $rate kills/s: ${kills:-0} kills (the rate before: $prev)"
        ok=0
    fi
    [ "$rate" = 0 ] || echo "$line" | grep -q "kill to first answer median" ||
        { echo "fat-storm: $rate kills/s: no kill to first answer"; ok=0; }
    case $rate in
    1) landed 1 2 "$line" || ok=0 ;;
    10) landed 10 10 "$line" || ok=0 ;;
    100) landed 100 50 "$line" || ok=0 ;;
    esac
    [ "$rate" = 0 ] || prev=${kills:-0}
done
srcs=$(grep -ac "^[0-9a-f]\{64\}  /usb0/big.bin\$" "$clean" || true)
others=$(grep -a "^[0-9a-f]\{64\}  /usb0/big.bin\$" "$clean" | grep -vc "^$want" || true)
[ "$srcs" -ge 4 ] && [ "$others" = 0 ] ||
    { echo "fat-storm: /usb0/big.bin hashed $srcs times, $others of them not the file's"; ok=0; }
never "storm: .* refused" "$clean"
never "not answered after" "$clean"
never "DIFFERENT" "$clean"
never "can't read it back" "$clean"
cutline=$(grep -a "^storm: 100 kills/s: the copy: stopped by Ctrl+C" "$clean" | head -1)
if [ -z "$cutline" ]; then
    echo "fat-storm: the Ctrl+C line is missing"
    ok=0
else   # stopped in the copy, not after it: part of the file, with kills done
    cutb=$(echo "$cutline" | grep -ao "after [0-9]* bytes" | grep -ao "[0-9]*" || echo 0)
    cutk=$(echo "$cutline" | grep -ao "; [0-9]* kills" | grep -ao "[0-9]*" || echo 0)
    if [ "${cutb:-0}" -le 0 ] || [ "${cutb:-0}" -ge $((mib << 20)) ]; then
        echo "fat-storm: Ctrl+C: ${cutb:-0} of $((mib << 20)) bytes copied: not in the middle of the copy"
        ok=0
    fi
    [ "${cutk:-0}" -ge 20 ] ||
        { echo "fat-storm: Ctrl+C: ${cutk:-0} kills before it, wanted 20 or more"; ok=0; }
fi
never "starting fresh" "$clean"
never "giving up" "$clean"
# From /usb0 made writable (its remount takes it away once) to the reboot.
sed -n '/mount: \/usb0 is now read-write/,/jam[^>]*>.* reboot -f/p' "$clean" > "$out/storm.run.log"
never "init: /data is gone" "$out/storm.run.log"
never "init: /usb0 is gone" "$out/storm.run.log"
echo "fat-storm: $(count 'fat /data: restart (killed' "$clean") restarts of fat /data and $(count 'fat /usb0: restart (killed' "$clean") of fat /usb0 carried on"

# ---- the disks afterwards -------------------------------------------------------------------
rm -f "$tmp/back.bin"
mcopy -o -i "$saved@@64M" "::/s100.bin" "$tmp/back.bin" 2>/dev/null &&
    cmp -s "$tmp/back.bin" "$tmp/big.bin" ||
    { echo "fat-storm: /data/s100.bin on the stick isn't the file"; ok=0; }
for f in s0.bin s1.bin s10.bin cut.bin; do
    ! mdir -i "$saved@@64M" "::/$f" > /dev/null 2>&1 ||
        { echo "fat-storm: /data/$f is still on the stick (it was removed)"; ok=0; }
done
for disk in "data:$saved:2" "usb0:$stick:1"; do
    name=${disk%%:*} rest=${disk#*:}
    img=${rest%:*} part=${rest##*:}
    python3 tools/fatcheck.py "$img" -p "$part" --require-clean > "$out/storm-fatcheck-$name.txt" 2>&1
    st=$?
    sed "s/^/fat-storm: \/$name: /" "$out/storm-fatcheck-$name.txt" | grep -v "chains walked" || true
    [ "$st" = 0 ] || { echo "fat-storm: fatcheck on /$name: exit $st"; ok=0; }
done
if command -v hdiutil > /dev/null && command -v fsck_msdos > /dev/null; then
    for disk in "data:$saved:2" "usb0:$stick:1"; do
        name=${disk%%:*} rest=${disk#*:}
        img=${rest%:*} part=${rest##*:}
        dev=$(hdiutil attach -readonly -nomount -imagekey diskimage-class=CRawDiskImage "$img" |
              awk 'NR == 1 {print $1}')
        [ -n "$dev" ] || continue
        fsck_msdos -n "${dev}s$part" > "$out/storm-fsck-$name.txt" 2>&1 && fst=0 || fst=$?
        hdiutil detach "$dev" > /dev/null || true
        echo "fat-storm: /$name: fsck_msdos -n exit $fst:" \
             "$(grep -a "Warning" "$out/storm-fsck-$name.txt" | grep -v "files, .* free" | tr '\n' ' ')"
    done
fi

# ---- the mixer ------------------------------------------------------------------------------
mline=$(grep -a "^storm: mixer: 2 kills/s" "$clean" | head -1)
echo "fat-storm: ${mline:-no storm mixer line}"
mk=$(echo "$mline" | grep -ao "; [0-9]* kills" | grep -ao "[0-9]*" || echo 0)
restarts=$(count "mixer: restart (killed" "$clean")
[ "${mk:-0}" -ge 5 ] || { echo "fat-storm: storm mixer: ${mk:-0} kills, wanted 5 or more"; ok=0; }
[ "$restarts" -ge "${mk:-0}" ] ||
    { echo "fat-storm: $restarts mixer restart lines for ${mk:-0} kills"; ok=0; }
echo "$mline" | grep -q "; 0 late period(s)$" || { echo "fat-storm: storm mixer saw late periods"; ok=0; }
least=$(echo "$mline" | grep -ao "least lead left [0-9]* frames" | grep -ao "[0-9]*" || echo 0)
[ "${least:-0}" -gt 256 ] ||
    { echo "fat-storm: the least lead left was ${least:-0} frames, the guard is 256"; ok=0; }
never "mixer: late:" "$clean"
never "frames late: the driver played silence" "$clean"
never "music: the mixer stream failed" "$clean"
close=$(grep -a "mixer: stream .*(music.*) closed" "$clean" | head -1)
echo "fat-storm: ${close:-no music stream close line}"
echo "$close" | grep -q " 0 late period(s)" || { echo "fat-storm: the music stream was late"; ok=0; }

rm -f "$boot" "$tmp/back.bin"
[ "$ok" = 1 ] && echo "fat-storm: PASS" && exit 0
echo "fat-storm: FAIL (see $out/storm.log)"
exit 1
