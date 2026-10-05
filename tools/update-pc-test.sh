#!/bin/sh
# `update -w` as on the owner's PC, where it first looked like a hang
# (2026-10-02, build b050c6a): a real USB 2 boot stick, whose every write
# command costs milliseconds, and a second stick at /usb0 (the SanDisk: an
# MBR and one FAT32 partition, type 0b). The boot stick here is QEMU's
# with throttled writes (QEMU_STICK_THROTTLE); the network peer serves
# build B, signed with the test key (as tools/update-write-test.sh makes
# it), and the boot menu boot/limine.conf; the stick's own menu is that
# with a line more (an older menu), and every run keeps it
# (QEMU_MENU_AT=efi). Four runs, each a cold boot of the stick image A
# (`again`: of the one `fast` left):
#   fast    100 write commands a second and 10 MB/s (a cheap stick's
#           small-write speed; PC_THROTTLE overrides it, "" for none):
#           `update -w` answers "written" within PC_WAIT seconds (150);
#           the stick holds B, with A as the previous build, no .new file,
#           and the new menu, the old as limine.conf.prev;
#   again   that stick boots B, and `update -w` of B again writes nothing
#           ("the stick has this build already", "... this boot menu
#           already") and answers "written";
#   stuck   3 write commands a second: the write can't finish in its time
#           (espwrite.c WRITE_LIMIT), and `update -w` still answers, "not
#           written", within 280 s; /esp is back read-only, and the stick
#           still boots A, with its old menu, no .new file left;
#   reboot  the same slow stick; `update -w` stopped with Ctrl+C while init
#           writes, then `reboot`: the write doesn't hold the reboot up
#           (kexec within 60 s), and the next boot runs B (the stored
#           kernel); the stick's default entry still boots A.
# init's progress lines (`init: update: write: ...`) are printed.
# PC_RUNS picks runs ("fast" alone: about 2 minutes; all four about 9;
# `again` needs `fast` before it);
# PC_SANDISK=0 leaves the second stick out.
# Usage: tools/update-pc-test.sh <outdir> (after `make -s image`); exit 0 on PASS.
set -u
out=$1
mkdir -p "$out"
fails=0
limit=${PC_WAIT:-150}

fail() {
    echo "update-pc-test: FAILED: $1"
    fails=$((fails + 1))
}

img="$out/pctest-stick.img"
tools/update-test-key.sh "$out" build/jamos.img "$img" ||
    { echo "update-pc-test: can't make the test key's stick"; exit 1; }
key="$out/testkey/key1/update.key"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\nnet.host = 10.2.21.174\n' \
    > "$out/pctest.settings"
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/pctest.settings" ::/etc/settings ||
    { echo "update-pc-test: can't write the stick's settings"; exit 1; }

# Build B: A's kernel with another version string, A's boot image with
# another build.txt (as tools/update-write-test.sh makes it).
va=$(python3 tools/update-server.py --manifest build/jamos.elf build/bootfs.img |
     sed -n 's/^version //p')
vb=$(python3 - build/jamos.elf "$out/jamos-B.elf" "$va" <<'EOF'
import sys
data, old = open(sys.argv[1], "rb").read(), sys.argv[3].encode()
new = old[:-1] + (b"C" if old.endswith(b"B") else b"B")
assert data.count(old + b"\0") >= 1, "no version string in the kernel"
open(sys.argv[2], "wb").write(data.replace(old + b"\0", new + b"\0"))
print(new.decode())
EOF
) || { echo "update-pc-test: can't make build B's kernel"; exit 1; }
printf 'git b0b0b0b\n%s\n' "$(sed -n 's/^\(net .*\)$/\1/p' build/build.txt)" \
    > "$out/pctest-build.txt"
python3 tools/bootfs-edit.py "$out/testkey/bootfs-key.img" "$out/bootfs-B.img" \
    "build.txt=$out/pctest-build.txt" ||
    { echo "update-pc-test: can't make build B's boot image"; exit 1; }
# The boot menus: the stick's (boot/limine.conf with a line more, as an
# older menu), and the one served with B (boot/limine.conf).
cp boot/limine.conf "$out/pctest-new.conf"
{ cat boot/limine.conf; echo "# the stick's menu before the update"; } > "$out/pctest-old.conf"
mcopy -o -i "$img@@1M" "$out/pctest-old.conf" ::/boot/limine/limine.conf
cat > "$out/pctest.spec.json" <<EOF
{"kernel": "$out/jamos-B.elf", "bootfs": "$out/bootfs-B.img", "key": "$key",
 "menu": "$out/pctest-new.conf", "plan": []}
EOF
mcopy -o -i "$img@@1M" ::/boot/bootfs.img "$out/bootfs-A.img"
echo "update-pc-test: build A $va, build B $vb"

# The second stick: not a Jam OS stick, one FAT32 partition with a file.
usb=
seen_usb="# no second stick"
if [ "${PC_SANDISK:-1}" = 1 ]; then
    sd="$out/pctest-sandisk.img"
    python3 tools/mkstick.py "$sd" 256 0b
    mformat -i "$sd@@1M" -T $((255 * 2048)) -F -v SANDISK ::
    echo "the owner's other stick" > "$out/pctest-hello.txt"
    mcopy -i "$sd@@1M" "$out/pctest-hello.txt" ::/hello.txt
    usb="-drive if=none,id=sandisk,format=raw,file=$sd"
    usb="$usb -device usb-storage,bus=xhci.0,port=2,drive=sandisk"
    seen_usb="seen 60 init: /usb0 mounted"
fi

esp() {
    rm -f "$out/pctest-got"
    mcopy -i "$1@@1M" "::/boot/$2" "$out/pctest-got" 2>/dev/null && cmp -s "$out/pctest-got" "$3"
}

# stick <run> <build> <prev> <new>: the run's stick image boots <build> (A
# or B) from its default entry and, if <prev> is A, A from its previous-
# build entry; with <new> = none, no .new or .old file is left.
stick() {
    i="$out/$1-done.img" k=build/jamos.elf b="$out/bootfs-A.img"
    [ "$2" = B ] && k="$out/jamos-B.elf" b="$out/bootfs-B.img"
    esp "$i" jamos.elf "$k" && esp "$i" bootfs.img "$b" || fail "$1: the stick's build isn't $2"
    if [ "$3" = A ]; then
        esp "$i" prev-jamos.elf build/jamos.elf && esp "$i" prev-bootfs.img "$out/bootfs-A.img" ||
            fail "$1: the previous build isn't A"
    fi
    if [ "$4" = none ] && mdir -i "$i@@1M" ::/boot ::/boot/limine 2>/dev/null |
            grep -qiE "\.(new|old)"; then
        fail "$1: a .new or .old file is left"
    fi
    m=old
    [ "$2" = B ] && m=new
    esp "$i" limine/limine.conf "$out/pctest-$m.conf" || fail "$1: the boot menu isn't the $m one"
    if [ $m = new ]; then
        esp "$i" limine/limine.conf.prev "$out/pctest-old.conf" ||
            fail "$1: limine.conf.prev isn't the stick's old menu"
    fi
    rm -f "$i"
}

# boot <run> <throttle> [<image>]: the stick (A, or <image>) booted with
# the script <run>.txt.
boot() {
    QEMU_IMAGE="${3:-$img}" QEMU_SAVE="$out/$1-done.img" QEMU_NET=1 QEMU_USB="$usb" \
        QEMU_MENU_AT=efi \
        QEMU_STICK_THROTTLE="$2" QEMU_NET_PEER="--update $out/pctest.spec.json" \
        QEMU_TIMEOUT=$((limit + 420)) QEMU_INPUT="$out/$1.txt" \
        tools/qemu-test.sh "$out" "$1" shell > "$out/$1.out" 2>&1 ||
        fail "$1: the script, or the VLAN checks (see $out/$1.out, $out/$1.log)"
    grep -a "update: \|init: update: \|devmgr: /esp\|kexec: starting" "$out/$1.log" |
        sed "s/^/update-pc-test: $1: /"
}

begin() {
    cat > "$out/$1.txt" <<EOF
wait 120 Jam OS shell
$seen_usb
seen 60 netstack: address 10.2.21.5/24
wait jam>
send update -w
EOF
}

slow="${PC_THROTTLE-x-iops-write=100,x-bps-write=10485760}"
for run in ${PC_RUNS:-fast again stuck reboot}; do
    begin $run
    case $run in
    fast|again)
        cat >> "$out/$run.txt" <<EOF
wait $limit stored and written to the stick (-w)
wait jam>
send reboot -f
wait reboot: resetting
EOF
        if [ $run = fast ]; then
            boot fast "$slow"
            cp "$out/fast-done.img" "$out/again-stick.img"
        else
            boot again "$slow" "$out/again-stick.img"
            rm -f "$out/again-stick.img"
            grep -aq "init: update: write: the stick has this build already" "$out/again.log" ||
                fail "again: the stick's build was written again"
            grep -aq "init: update: write: the stick has this boot menu already" "$out/again.log" ||
                fail "again: the stick's boot menu was written again"
        fi
        [ $run = again ] ||
            grep -aq "init: update: its boot menu written" "$out/$run.log" ||
            fail "$run: init didn't say it wrote the boot menu"
        grep -aq "init: update: .* and stored, and written to the stick" "$out/$run.log" ||
            fail "$run: init didn't say it wrote the stick"
        stick $run B A none ;;
    stuck)
        cat >> "$out/stuck.txt" <<EOF
wait 280 is stored, but init couldn't write it to the stick
wait jam>
seen 30 init: update: write: the ESP read-only again in
send ls /esp/boot
wait jamos.elf
wait jam>
send reboot -f
wait reboot: resetting
EOF
        boot stuck x-iops-write=3
        grep -aq "init: update: write: no time left" "$out/stuck.log" ||
            fail "stuck: the write didn't stop at its time limit"
        stick stuck A - none ;;
    reboot)
        cat >> "$out/reboot.txt" <<EOF
wait 120 init: update: write: making room: OK
type \x03
wait jam>
send reboot
wait 60 kexec: starting the stored kernel
wait 120 init: the shell is up
wait jam>
send version
wait Jam OS $vb, git
wait jam>
send reboot -f
wait reboot: resetting
EOF
        boot reboot x-iops-write=3
        stick reboot A - any ;;
    esac
done
rm -f "$img" "$out/pctest-got" "$out/again-stick.img"
if [ $fails -eq 0 ]; then
    echo "update-pc-test: PASS"
    exit 0
fi
echo "update-pc-test: FAIL ($fails)"
exit 1
