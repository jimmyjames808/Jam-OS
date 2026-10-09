#!/bin/sh
# volumes (user/services/volumes, M12's storage split) started again, and
# what is under it, in QEMU on the boot stick: tools/shell-tests/
# volumes-restart.txt kills volumes, then devmgr, then the boot stick's
# usb-storage; each time /data and /esp go and come back, and logd
# carries on in its file. Checked afterwards: no "Jam OS stick removed"
# notice (the stick never went), nothing given up on, no job left
# unclean; on the stick (read on the host with mtools), the file written
# before the kills and appended after them, and one boot log that holds
# every kill (logd never started a second file). Of tools/fat-restart-
# test.sh's family. QEMU_SMP passes through.
# Usage: tools/volumes-restart-test.sh <outdir>; exit 0 on PASS.
set -eu
out=$1
mkdir -p "$out"
ok=1
log="$out/vr.log"
QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/volumes-restart.txt \
    QEMU_SAVE="$out/vr-stick.img" \
    tools/qemu-test.sh "$out" vr shell > "$out/vr.out" 2>&1 ||
    { echo "volumes-restart: the script failed"; grep "serial-feed: .*no '" "$out/vr.out" || true; ok=0; }

count() { grep -ac -- "$1" "$2" || true; }
need() {   # need <min> <text>
    n=$(count "$2" "$log")
    [ "$n" -ge "$1" ] || { echo "volumes-restart: \"$2\" $n times, wanted $1 or more"; ok=0; }
}
never() {   # never <text>
    n=$(count "$1" "$log")
    [ "$n" -eq 0 ] || { echo "volumes-restart: \"$1\" $n times, wanted none"; ok=0; }
}
need 1 "init: volumes and its filesystems are gone"
need 1 "init: devmgr and its drivers are gone"
need 1 "volumes: disk [0-9]*: its driver ended"
need 4 "init: /data mounted"
need 4 "init: /esp mounted"
never "Jam OS stick removed"
never "giving up"
never "did not end cleanly"
# The stick, on the host: the file, and the one boot log with every kill.
img="$out/vr-stick.img"
if [ -f "$img" ]; then
    mtype -i "$img@@64M" ::/vr.txt > "$out/vr-file.txt" 2>/dev/null || true
    grep -q "before the kills-v" "$out/vr-file.txt" && grep -q "after the kills-v" "$out/vr-file.txt" ||
        { echo "volumes-restart: the stick's /vr.txt doesn't hold what was written"; ok=0; }
    mtype -i "$img@@64M" ::/logs/boot-0001.txt > "$out/vr-boot.txt" 2>/dev/null || true
    for line in "init: volumes and its filesystems are gone" "init: devmgr and its drivers are gone" \
                "init: kill usb-storage-1:0: OK"; do
        grep -q "$line" "$out/vr-boot.txt" ||
            { echo "volumes-restart: boot-0001.txt on the stick lacks \"$line\""; ok=0; }
    done
    ! mdir -i "$img@@64M" ::/logs/boot-0002.txt > /dev/null 2>&1 ||
        { echo "volumes-restart: logd started a second boot log"; ok=0; }
    rm -f "$img"
else
    echo "volumes-restart: no stick image came back"
    ok=0
fi
[ "$ok" = 1 ] && echo "volumes-restart: PASS" && exit 0
echo "volumes-restart: FAIL"
exit 1
