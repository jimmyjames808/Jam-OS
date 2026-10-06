#!/bin/sh
# `update`'s stick write and the boot menu (user/services/init/espmenu.c, <update.h>'s
# `menu` line), in QEMU. The stick's build A has a test key
# (tools/update-test-key.sh) and an OLD boot menu: boot/limine.conf without
# its two IOMMU entries (Developer > "Jam OS (no IOMMU)" and "IOMMU checks":
# the owner's stick had neither). Build B is A's kernel
# with another version string and A's boot image with build.txt saying git
# b0b0b0b and one more file (as tools/update-write-test.sh makes it). The
# NEW menu is boot/limine.conf; a BAD one is it with "Jam OS (previous
# build)" booting /boot/jamos.elf (the PC's check refuses it). Manifests for
# B, signed with the test key: without a menu (an older server's), and with
# each menu. Every QEMU run keeps the stick's own menu (QEMU_MENU_AT=efi:
# the run's one-entry menu goes to EFI/BOOT/limine.conf, which Limine reads
# first); each is a cold boot of the stick image a run before it left.
#   1. menus: `run updtest menucheck` before anything (Limine reads the old
#      menu); `menubad` (a menu byte changed: refused, its SHA-256; the
#      menu longer than its manifest says; its VMO missing; a menu with a
#      manifest that names none; a menu length with none; check-only and
#      RAM-only offers with the menu: accepted, the menu untouched); B with
#      the BAD menu written (the build written, the menu refused by the
#      check, the old one kept); B with no menu line (written: the build was
#      there; the menu untouched); B with the NEW menu (written, the old one
#      kept as limine.conf.prev); the same again (nothing written: "the
#      stick has this boot menu already"). On the Mac: the stick's menu is
#      the new one, limine.conf.prev the old, no limine.conf.new, no spare
#      (boot/limine.conf), the build B, the previous build A.
#   2. stops: on run 1's stick, the menu's swap stopped dead after each of
#      its changes (UPDATE_OFFER_STOP 9 to 14, as a power cut there), the
#      menu offered being the one the stick doesn't have, and after each
#      `menucheck`: Limine reads a whole menu, the old or the new (from 12,
#      the spare); then one with no stop. On the Mac: the old menu in its
#      place, no leftovers.
#   3. cut: on run 1's stick, a write stopped after change 12 (the stick's
#      menu renamed limine.conf.prev, the new one not yet in its place),
#      then a firmware reset. On the Mac: no boot/limine/limine.conf, the
#      spare is the stick's menu (the new one). Then that stick booted with
#      NO menu of the test's: Limine finds only the spare, and its default
#      entry starts build B (the kernel's `loader:` line).
#   4. settle: that stick in QEMU again: the next write undoes the cut swap
#      ("an earlier boot menu write stopped in its swap: undoing it") and
#      writes its menu. On the Mac: the old menu in place, the new as .prev.
#   5. net: a fresh stick (A, the old menu), QEMU_NET with the peer serving
#      B and the NEW menu (tools/netpeer.py --update): `update -n` (checked,
#      the menu untouched); `update -w` with the menu damaged on the way
#      (refused: "a file's SHA-256 isn't the manifest's: menu"); plain
#      `update` from a server without menus (the build written, the menu
#      untouched; no reboot: `reboot` starts B); `update -w` again with the
#      menu (the build there already and running, the menu written; then
#      `reboot`). On the Mac: the new menu, the old as .prev, B, A as the
#      previous build.
#   6. entries: that stick, with nothing of the test's on it, booted with
#      Limine's own pick (its menu as written: the default entry, B), then
#      each entry of its menu that boots (a copy of the menu with
#      `default_entry` and `timeout: 0`, its directory open, at
#      EFI/BOOT/limine.conf; the entries' own lines as they are): each
#      starts the kernel its entry names (B; A for "Jam OS (previous
#      build)") with its command line. Each boot is stopped at the kernel's
#      `loader:` line.
# MENU_RUNS picks runs (2-4 need 1; 6 needs 5). About 15 minutes for all.
# Usage: tools/update-menu-test.sh <outdir> (after `make -s image`); exit 0 on PASS.
set -u
out=$1
mkdir -p "$out"
fails=0
runs=${MENU_RUNS:-menus stops cut settle net entries}

fail() {
    echo "update-menu-test: FAILED: $1"
    fails=$((fails + 1))
}

img="$out/mtest.img"
tools/update-test-key.sh "$out" build/jamos.img "$img" ||
    { echo "update-menu-test: can't make the test key's stick"; exit 1; }
key="$out/testkey/key1/update.key"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\nnet.host = 10.2.21.174\n' \
    > "$out/mtest.settings"
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/mtest.settings" ::/etc/settings ||
    { echo "update-menu-test: can't write the stick's settings"; exit 1; }

# The menus: new (the repo's), old (without the IOMMU entries), bad.
cp boot/limine.conf "$out/menu-new.conf"
python3 - boot/limine.conf "$out/menu-old.conf" "$out/menu-bad.conf" <<'EOF' ||
import sys
text = open(sys.argv[1]).read()
blocks, cur = [], []
for line in text.split("\n")[:-1]:
    if line.startswith("/") and cur:
        blocks.append(cur)
        cur = []
    cur.append(line)
blocks.append(cur)
old = [b for b in blocks if b[0] not in ("//Jam OS (no IOMMU)", "//IOMMU checks")]
assert len(old) == len(blocks) - 2, "boot/limine.conf has no IOMMU entries to leave out"
open(sys.argv[2], "w").write("\n".join(l for b in old for l in b) + "\n")
bad = text.replace("path: boot():/boot/prev-jamos.elf\n    module_path: boot():/boot/prev-bootfs.img",
                   "path: boot():/boot/jamos.elf\n    module_path: boot():/boot/bootfs.img")
assert bad != text
open(sys.argv[3], "w").write(bad)
EOF
    { echo "update-menu-test: can't make the menus"; exit 1; }
for m in new old; do
    build/host/menucheck "$out/menu-$m.conf" /boot/jamos.elf /boot/bootfs.img \
        /boot/prev-jamos.elf /boot/prev-bootfs.img > /dev/null ||
        { echo "update-menu-test: the $m menu fails the PC's check"; exit 1; }
done
mcopy -o -i "$img@@1M" "$out/menu-old.conf" ::/boot/limine/limine.conf

# Build B and its manifests: none, new, old, bad (as folders on /data/update).
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
) || { echo "update-menu-test: can't make build B's kernel"; exit 1; }
printf 'update-marker: build B %s\n' "$$" > "$out/mtest-marker.txt"
printf 'git b0b0b0b\n%s\n' "$(sed -n 's/^\(net .*\)$/\1/p' build/build.txt)" > "$out/mtest-build.txt"
python3 tools/bootfs-edit.py "$out/testkey/bootfs-key.img" "$out/bootfs-B.img" \
    "update-marker.txt=$out/mtest-marker.txt" "build.txt=$out/mtest-build.txt" ||
    { echo "update-menu-test: can't make build B's boot image"; exit 1; }
mcopy -i "$img@@1M" ::/boot/bootfs.img "$out/bootfs-A.img"
put() {   # put <file> <path on /data>
    mcopy -o -i "$img@@64M" "$1" "::$2" || { echo "update-menu-test: can't write $2"; exit 1; }
}
mmd -i "$img@@64M" ::/update ::/update/mnew ::/update/mold ::/update/mbad
python3 tools/update-server.py --manifest "$out/jamos-B.elf" "$out/bootfs-B.img" --key "$key" \
    > "$out/manifest-none" || { echo "update-menu-test: no manifest"; exit 1; }
put "$out/manifest-none" /update/manifest
put "$out/jamos-B.elf" /update/jamos.elf
put "$out/bootfs-B.img" /update/bootfs.img
put "$out/menu-old.conf" /update/menu-old.conf
put "$out/menu-new.conf" /update/menu-new.conf
for m in new old bad; do
    python3 tools/update-server.py --manifest "$out/jamos-B.elf" "$out/bootfs-B.img" \
        --menu "$out/menu-$m.conf" --key "$key" > "$out/manifest-$m" ||
        { echo "update-menu-test: no manifest with the $m menu"; exit 1; }
    put "$out/manifest-$m" "/update/m$m/manifest"
    put "$out/menu-$m.conf" "/update/m$m/limine.conf"
done
echo "update-menu-test: build A $va, build B $vb"

# esp <image> <path> <file>: does the image's ESP have <path>, the bytes of <file>?
esp() {
    rm -f "$out/mtest-got"
    mcopy -i "$1@@1M" "::$2" "$out/mtest-got" 2>/dev/null && cmp -s "$out/mtest-got" "$3"
}
# no_esp <image> <path>: the ESP has no <path>.
no_esp() {
    ! mcopy -i "$1@@1M" "::$2" "$out/mtest-got" 2>/dev/null
}
# menus <run> <image> <menu> <prev>: the image's menu is <menu> (old, new or
# none), limine.conf.prev is <prev> (old, new, none, any), no .new, no spare.
menus() {
    if [ "$3" = none ]; then
        no_esp "$2" /boot/limine/limine.conf || fail "$1: the stick has a menu in its place"
    else
        esp "$2" /boot/limine/limine.conf "$out/menu-$3.conf" || fail "$1: the menu isn't the $3 one"
    fi
    case $4 in
    none) no_esp "$2" /boot/limine/limine.conf.prev || fail "$1: a limine.conf.prev is left" ;;
    any) ;;
    *) esp "$2" /boot/limine/limine.conf.prev "$out/menu-$4.conf" ||
           fail "$1: limine.conf.prev isn't the $4 menu" ;;
    esac
    no_esp "$2" /boot/limine/limine.conf.new || fail "$1: limine.conf.new is left"
    [ "$3" = none ] || no_esp "$2" /boot/limine.conf || fail "$1: the spare is left"
}
# run <name> <image> <save> [net]: the script <name>.txt on a cold boot of <image>.
run() {
    QEMU_IMAGE="$2" QEMU_SAVE="$3" QEMU_MENU_AT=efi QEMU_TIMEOUT=${QEMU_TIMEOUT:-1200} \
        QEMU_INPUT="$out/$1.txt" tools/qemu-test.sh "$out" "$1" shell > "$out/$1.out" 2>&1 ||
        fail "$1: the script (see $out/$1.log)"
    grep -a "updtest: menu\|init: update: write: .*menu\|init: update: .*boot menu" \
        "$out/$1.log" | tr -d '\r' | sed "s/^/update-menu-test: $1: /"
}
start='wait 120 Jam OS shell
wait {prompt}
seen 60 init: /data mounted'
mcheck='send run updtest menucheck /data/update/menu-old.conf /data/update/menu-new.conf
wait 60 updtest: menucheck:'
# write <dir> [<stop>]: a menuwrite, its answer line waited for.
write() {
    printf 'send run updtest menuwrite /data/update/%s %s\nwait 300 updtest: menuwrite:\nwait {prompt}\n' \
        "$1" "${2:-0}"
}
# expect <run> <count> <text>: the run's log has <text> exactly <count> times.
expect() {
    n=$(tr -d '\r' < "$out/$1.log" | grep -ac -- "$3")
    [ "$n" -eq "$2" ] || fail "$1: \"$3\" $n times, not $2"
}

# boot_entry <image> <name> <entry>: a cold boot of the image's own ESP,
# stopped at the kernel's loader line: <entry> 0 is Limine's own pick (no
# menu of the test's), N its menu's entry N (a copy at EFI/BOOT/limine.conf).
# Prints the kernel's version and command line.
boot_entry() {
    i="$out/$2.img"
    cp "$1" "$i"
    mdel -i "$i@@1M" ::/EFI/BOOT/limine.conf 2>/dev/null
    if [ "$3" != 0 ]; then
        mcopy -i "$i@@1M" ::/boot/limine/limine.conf "$out/$2.conf" 2>/dev/null ||
            mcopy -i "$i@@1M" ::/boot/limine.conf "$out/$2.conf"
        python3 - "$out/$2.conf" "$3" <<'EOF'
import sys
lines = [l for l in open(sys.argv[1]).read().split("\n")[:-1] if not l.startswith("timeout:")]
entries = [i for i, l in enumerate(lines) if l.startswith("/")]
for i, j in zip(entries, entries[1:]):   # a / entry followed by a // one: a directory, opened
    if not lines[i].startswith(("//", "/+")) and lines[j].startswith("//"):
        lines[i] = "/+" + lines[i][1:]
open(sys.argv[1], "w").write("timeout: 0\ndefault_entry: %s\n" % sys.argv[2] + "\n".join(lines) + "\n")
EOF
        mcopy -o -i "$i@@1M" "$out/$2.conf" ::/EFI/BOOT/limine.conf
    fi
    ovmf=$(brew --prefix qemu)/share/qemu
    cp "$ovmf/edk2-i386-vars.fd" "$out/$2.vars"
    rm -f "$out/$2.log"
    qemu-system-x86_64 -M q35 -m 2G -smp 2 -cpu max -snapshot \
        -drive if=pflash,format=raw,readonly=on,file="$ovmf/edk2-x86_64-code.fd" \
        -drive if=pflash,format=raw,file="$out/$2.vars" \
        -device qemu-xhci,id=xhci -drive if=none,id=stick,format=raw,file="$i" \
        -device usb-storage,bus=xhci.0,port=1,drive=stick,bootindex=0 \
        -serial file:"$out/$2.log" -display none -no-reboot -nic none 2> "$out/$2.qemu" &
    q=$!
    j=0
    while [ $j -lt 120 ] && ! grep -aq "loader:" "$out/$2.log" 2>/dev/null; do
        sleep 0.5
        j=$((j + 1))
    done
    kill $q 2>/dev/null
    wait $q 2>/dev/null
    rm -f "$i" "$out/$2.vars"
    v=$(tr -d '\r' < "$out/$2.log" | sed -n 's/^.*\] Jam OS \([^ ]*\)$/\1/p' | head -1)
    c=$(tr -d '\r' < "$out/$2.log" | sed -n 's/^.*loader: *Limine, cmdline "\(.*\)"$/\1/p' | head -1)
    echo "$v|$c"
}

for r in $runs; do
case $r in
menus)
    {
        echo "$start"
        echo "$mcheck"; echo "wait {prompt}"
        printf 'send run updtest menubad /data/update/mnew/\nwait 300 updtest: menubad:\nwait {prompt}\n'
        echo "$mcheck"; echo "wait {prompt}"
        write mbad/; echo "$mcheck"; echo "wait {prompt}"
        write ""; echo "$mcheck"; echo "wait {prompt}"
        write mnew/; echo "$mcheck"; echo "wait {prompt}"
        write mnew/
        printf 'send reboot -f\nwait reboot: resetting\n'
    } > "$out/m1.txt"
    run m1 "$img" "$out/m1-saved.img"
    expect m1 1 "updtest: menubad: PASS"
    expect m1 4 "Limine reads boot/limine/limine.conf: the old menu (prev: none): as expected"
    expect m1 1 "Limine reads boot/limine/limine.conf: the new menu (prev: the old menu): as expected"
    expect m1 1 "menuwrite /data/update/mbad/ stop 0: build accepted (OK), done, menu refused (OK): line .*: \"Jam OS (previous build)\" boots another kernel"
    expect m1 1 "menuwrite /data/update/ stop 0: build accepted (OK), done, menu none (OK)"
    expect m1 1 "menuwrite /data/update/mnew/ stop 0: build accepted (OK), done, menu written (OK)"
    expect m1 1 "menuwrite /data/update/mnew/ stop 0: build accepted (OK), done, menu same (OK)"
    expect m1 1 "init: update: write: the stick has this boot menu already: nothing to write"
    expect m1 3 "init: update: write: the stick has this build already"
    menus m1 "$out/m1-saved.img" new old
    esp "$out/m1-saved.img" /boot/jamos.elf "$out/jamos-B.elf" &&
        esp "$out/m1-saved.img" /boot/prev-jamos.elf build/jamos.elf || fail "m1: the stick isn't B with A before it"
    ;;
stops)
    # The stick has the new menu: each stop offers the one it doesn't have
    # (after 13 the old is in place).
    {
        echo "$start"
        for s in 9:mold 10:mold 11:mold 12:mold 13:mold 14:mnew 0:mold; do
            write "${s#*:}/" "${s%:*}"; echo "$mcheck"; echo "wait {prompt}"
        done
        printf 'send reboot -f\nwait reboot: resetting\n'
    } > "$out/m2.txt"
    run m2 "$out/m1-saved.img" "$out/m2-saved.img"
    expect m2 6 "menu notwritten (ERR_CANCELED)"
    expect m2 1 "menuwrite /data/update/mold/ stop 0: build accepted (OK), done, menu written (OK)"
    expect m2 7 "updtest: menucheck: .*: as expected"
    expect m2 1 "Limine reads boot/limine.conf (the spare): the new menu"
    expect m2 1 "an earlier boot menu write stopped in its swap: undoing it"
    menus m2 "$out/m2-saved.img" old new
    ;;
cut)
    { echo "$start"; write mold/ 12; echo "$mcheck"; echo "wait {prompt}"
      printf 'send reboot -f\nwait reboot: resetting\n'; } > "$out/m3.txt"
    run m3 "$out/m1-saved.img" "$out/m3-saved.img"
    expect m3 1 "Limine reads boot/limine.conf (the spare): the new menu (prev: the new menu"
    no_esp "$out/m3-saved.img" /boot/limine/limine.conf || fail "m3: the cut left a menu in its place"
    esp "$out/m3-saved.img" /boot/limine.conf "$out/menu-new.conf" || fail "m3: the spare isn't the stick's menu"
    got=$(boot_entry "$out/m3-saved.img" m3boot 0)
    echo "update-menu-test: m3boot: the cut stick's own pick starts: $got"
    [ "$got" = "$vb|" ] || fail "m3boot: the cut stick doesn't boot B from the spare ($got)"
    ;;
settle)
    { echo "$start"; write mold/; echo "$mcheck"; echo "wait {prompt}"
      printf 'send reboot -f\nwait reboot: resetting\n'; } > "$out/m4.txt"
    run m4 "$out/m3-saved.img" "$out/m4-saved.img"
    expect m4 1 "an earlier boot menu write stopped in its swap: undoing it"
    expect m4 1 "menuwrite /data/update/mold/ stop 0: build accepted (OK), done, menu written (OK)"
    menus m4 "$out/m4-saved.img" old new
    ;;
net)
    cat > "$out/mtest.spec.json" <<EOF
{"kernel": "$out/jamos-B.elf", "bootfs": "$out/bootfs-B.img", "key": "$key",
 "menu": "$out/menu-new.conf", "plan": ["good", "menudamage", "nomenu"]}
EOF
    cat > "$out/m5.txt" <<EOF
$start
seen 60 netstack: address 10.2.21.5/24
send update -n
wait 300 not loaded (-n): the running build stays
wait {prompt}
$mcheck
wait {prompt}
send update -w
wait 300 init refused it: a file's SHA-256 isn't the manifest's: menu
wait {prompt}
send update
wait 300 loaded and written to the stick
wait update: written to the stick and loaded:
wait {prompt}
send reboot
wait 60 kexec: starting the stored kernel
wait 120 init: the shell is up
wait {prompt}
$mcheck
wait {prompt}
send update -w
wait 300 update: its boot menu written
wait update: this is the build running now, and the stick has it already
wait {prompt}
send reboot
wait 60 kexec: starting the stored kernel
wait 120 init: the shell is up
wait {prompt}
send version
wait Jam OS $vb, git b0b0b0b
wait {prompt}
$mcheck
wait {prompt}
send reboot -f
wait reboot: resetting
EOF
    QEMU_IMAGE="$img" QEMU_SAVE="$out/m5-saved.img" QEMU_MENU_AT=efi QEMU_NET=1 \
        QEMU_NET_PEER="--update $out/mtest.spec.json" QEMU_TIMEOUT=${QEMU_TIMEOUT:-1200} \
        QEMU_INPUT="$out/m5.txt" tools/qemu-test.sh "$out" m5 shell > "$out/m5.out" 2>&1 ||
        fail "m5: the script or the VLAN checks (see $out/m5.out, $out/m5.log)"
    grep -a "update: \|init: update: .*menu" "$out/m5.log" | tr -d '\r' |
        sed 's/^/update-menu-test: m5: /'
    expect m5 2 "Limine reads boot/limine/limine.conf: the old menu (prev: none): as expected"
    expect m5 1 "Limine reads boot/limine/limine.conf: the new menu (prev: the old menu): as expected"
    expect m5 1 "update: the server has .*, no boot menu"
    menus m5 "$out/m5-saved.img" new old
    esp "$out/m5-saved.img" /boot/jamos.elf "$out/jamos-B.elf" &&
        esp "$out/m5-saved.img" /boot/prev-jamos.elf build/jamos.elf || fail "m5: the stick isn't B with A before it"
    ;;
entries)
    got=$(boot_entry "$out/m5-saved.img" m6-0 0)
    echo "update-menu-test: m6: Limine's own pick starts: $got"
    [ "$got" = "$vb|" ] || fail "m6: the written stick's default isn't B with no words ($got)"
    mcopy -i "$out/m5-saved.img@@1M" ::/boot/limine/limine.conf "$out/m6.conf"
    python3 - "$out/m6.conf" > "$out/m6.entries" <<'EOF'
import sys
n = 0
cur = None
def out(c):
    if c and c["path"]:
        print("%d|%s|%s|%s" % (c["n"], c["name"], c["path"], c["cmdline"]))
for line in open(sys.argv[1]).read().split("\n"):
    if line.startswith("/"):
        out(cur)
        n += 1
        cur = {"n": n, "name": line.lstrip("/+"), "path": "", "cmdline": ""}
    elif cur and line.strip().startswith(("path:", "kernel_path:")):
        cur["path"] = line.split(": ", 1)[1]
    elif cur and line.strip().startswith(("cmdline:", "kernel_cmdline:")):
        cur["cmdline"] = line.split(": ", 1)[1]
out(cur)
EOF
    while IFS='|' read -r n name path words; do
        want=$vb
        [ "$path" = "boot():/boot/prev-jamos.elf" ] && want=$va
        got=$(boot_entry "$out/m5-saved.img" "m6-$n" "$n")
        echo "update-menu-test: m6: entry $n \"$name\" starts: $got"
        [ "$got" = "$want|$words" ] || fail "m6: entry $n \"$name\" didn't start $want with \"$words\""
    done < "$out/m6.entries"
    [ "$(wc -l < "$out/m6.entries")" -ge 16 ] || fail "m6: fewer than 16 entries boot"
    ;;
*)
    echo "update-menu-test: no run $r"; exit 2 ;;
esac
done
rm -f "$img" "$out"/m[1-5]-saved.img "$out/mtest-got"
if [ $fails -eq 0 ]; then
    echo "update-menu-test: PASS"
    exit 0
fi
echo "update-menu-test: FAIL ($fails)"
exit 1
