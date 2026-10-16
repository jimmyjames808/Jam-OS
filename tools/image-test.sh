#!/bin/sh
# tools/image-test.sh <outdir>
#
# The stick's image and the flashing tool's data step (M13.6's U11a, "Ext by
# default"), no QEMU, on the Mac or the PC (where Linux's e2fsck and debugfs
# are installed they check the volumes too). Needs `make` done (the kernel
# and the boot image); the images are made under <outdir> with
# `make image IMAGE=...`, so build/jamos.img and the runs that read it are
# not touched.
#   ext     the default image (DATA_FS=ext) with a DATA_DIR tree: partition 2
#           is MBR type 0x83, mkimage.py --data-fs says ext, jamext check is
#           clean and reads the tree back byte for byte, the ESP still has
#           Limine, the kernel and the boot image, e2fsck -fn is clean;
#   same    the same inputs make the same bytes in the data partition (the
#           image's own MBR id and the ESP's serial are the only random
#           parts), and a DATA_DIR that isn't there leaves the volume empty;
#   fat     DATA_FS=fat: type 0x0C, a FAT volume mtools reads at --data-at,
#           and jamext says it is no ext volume;
#   bogus   DATA_FS=nonsense is refused;
#   stick   what `make usb` does after dd: the image on a bigger "stick"
#           (a sparse file), tools/mbr-grow.py, tools/stick-data.sh: the
#           volume is as big as the grown partition, holds the tree, is clean
#           (jamext, e2fsck), and the ESP is byte for byte what it was;
#   dflt    build/jamos.img, if it is an ext image, is clean.
# Exit 0 when every case passes.
set -eu

out=${1:?usage: tools/image-test.sh <outdir>}
export PATH=/usr/sbin:/sbin:$PATH LC_ALL=C
unset MAKEFLAGS MFLAGS MAKELEVEL   # `make check` runs this; its makes are their own
JX=build/host/jamext
rm -rf "$out"
mkdir -p "$out"
fails=0

fail() {
    echo "image-test: FAIL: $*" >&2
    fails=$((fails + 1))
}
pass() { echo "image-test: ok: $*"; }
have() { command -v "$1" > /dev/null 2>&1; }

make -s "$JX" || { echo "image-test: can't build jamext"; exit 1; }

# The data partition of an image (entry 2), as a file.
data_of() {
    off=$(python3 tools/mkimage.py --data-at "$1")
    dd if="$1" of="$2" bs=1048576 skip="${off%M}" count="$3" 2> /dev/null
}

# A DATA_DIR tree: a key, a 3 MiB file, a deeper name, an empty one.
mkdir -p "$out/tree/etc" "$out/tree/sub/deeper"
printf 'ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAItest key@example\n' > "$out/tree/etc/authorized_keys"
dd if=/dev/urandom of="$out/tree/sub/big.bin" bs=1048576 count=3 2> /dev/null
printf 'deep\n' > "$out/tree/sub/deeper/file.txt"
: > "$out/tree/empty"

# The tree comes back byte for byte from <image> (partition 2).
tree_ok() {
    for f in etc/authorized_keys sub/big.bin sub/deeper/file.txt empty; do
        "$JX" --part 2 get "$1" "/$f" "$out/got" > /dev/null 2>&1 &&
            cmp -s "$out/got" "$out/tree/$f" || { fail "$2: /$f is not what DATA_DIR held"; return 1; }
    done
    rm -f "$out/got"
}

# e2fsck -fn of a volume held in a file at <offset> bytes, size <bytes>.
fsck_part() {
    have e2fsck || { echo "image-test: no e2fsck here: $2 not checked by it"; return 0; }
    dd if="$1" of="$out/part.img" bs=1048576 skip=$(($3 >> 20)) count=$(($4 >> 20)) 2> /dev/null
    if e2fsck -fn "$out/part.img" > "$out/$2.fsck" 2>&1; then
        pass "$2: e2fsck -fn clean"
    else
        fail "$2: e2fsck -fn: $(tail -3 "$out/$2.fsck" | tr '\n' ' ')"
    fi
    if have debugfs; then
        debugfs -R "cat /etc/authorized_keys" "$out/part.img" 2> /dev/null |
            cmp -s - "$out/tree/etc/authorized_keys" ||
            fail "$2: debugfs doesn't read the key back"
    fi
    rm -f "$out/part.img"
}

# ---- ext ------------------------------------------------------------------

make -s image IMAGE="$out/ext.img" DATA_FS=ext DATA_DIR="$out/tree" > "$out/ext.make" 2>&1 ||
    { tail -5 "$out/ext.make"; fail "ext: make image failed"; }
if [ -f "$out/ext.img" ]; then
    [ "$(python3 tools/mkimage.py --data-fs "$out/ext.img")" = ext ] || fail "ext: --data-fs isn't ext"
    mbr_type=$(python3 -c "import sys; d = open(sys.argv[1], 'rb').read(512); print('%02x %02x' % (d[450], d[466]))" "$out/ext.img")
    [ "$mbr_type" = "ef 83" ] || fail "ext: the MBR's types are $mbr_type, not ef 83"
    "$JX" --part 2 check "$out/ext.img" > "$out/ext.check" 2>&1 ||
        fail "ext: jamext check: $(cat "$out/ext.check")"
    tree_ok "$out/ext.img" ext && pass "ext: the tree is in /data, byte for byte"
    esp=$(mdir -b -i "$out/ext.img@@1M" ::/boot ::/EFI/BOOT 2> /dev/null | tr '\n' ' ')
    case $esp in
    *jamos.elf*bootfs.img*) pass "ext: the ESP holds the kernel and the boot image" ;;
    *) fail "ext: the ESP lacks the kernel or the boot image: $esp" ;;
    esac
    case $esp in
    *BOOTX64.EFI*) ;;
    *) fail "ext: the ESP lacks BOOTX64.EFI" ;;
    esac
    off=$(python3 tools/mkimage.py --data-at "$out/ext.img")
    fsck_part "$out/ext.img" ext $((${off%M} << 20)) $((64 << 20))
fi

# ---- same ------------------------------------------------------------------

make -s image IMAGE="$out/ext2.img" DATA_FS=ext DATA_DIR="$out/tree" > "$out/ext2.make" 2>&1 ||
    fail "same: the second make image failed"
if [ -f "$out/ext.img" ] && [ -f "$out/ext2.img" ]; then
    data_of "$out/ext.img" "$out/d1" 64
    data_of "$out/ext2.img" "$out/d2" 64
    if cmp -s "$out/d1" "$out/d2"; then
        pass "same: the same DATA_DIR makes the same data partition"
    else
        fail "same: two images from the same inputs differ in /data"
    fi
    rm -f "$out/d1" "$out/d2"
fi
make -s image IMAGE="$out/none.img" DATA_FS=ext DATA_DIR="$out/no-such-dir" > "$out/none.make" 2>&1 ||
    fail "same: make image without a DATA_DIR failed"
if [ -f "$out/none.img" ]; then
    names=$("$JX" --part 2 ls -R "$out/none.img" 2> /dev/null | grep -c . || true)
    # "/:", "lost+found" and "/lost+found:" : nothing of ours
    if "$JX" --part 2 ls "$out/none.img" 2> /dev/null | grep -qv lost+found; then
        fail "same: an image without a DATA_DIR has files in /data ($names lines)"
    else
        pass "same: without a DATA_DIR /data holds only lost+found"
    fi
fi

# ---- fat -------------------------------------------------------------------

make -s image IMAGE="$out/fat.img" DATA_FS=fat > "$out/fat.make" 2>&1 ||
    { tail -5 "$out/fat.make"; fail "fat: make image DATA_FS=fat failed"; }
if [ -f "$out/fat.img" ]; then
    [ "$(python3 tools/mkimage.py --data-fs "$out/fat.img")" = fat ] || fail "fat: --data-fs isn't fat"
    off=$(python3 tools/mkimage.py --data-at "$out/fat.img")
    if mdir -i "$out/fat.img@@$off" :: 2> /dev/null | grep -q JAMOS-DATA; then
        pass "fat: DATA_FS=fat makes a FAT32 JAMOS-DATA at $off"
    else
        fail "fat: mtools can't read the data partition at $off"
    fi
    if "$JX" --part 2 check "$out/fat.img" > /dev/null 2>&1; then
        fail "fat: jamext check took a FAT volume"
    fi
fi

# ---- bogus -----------------------------------------------------------------

if make -s image IMAGE="$out/bogus.img" DATA_FS=nonsense > "$out/bogus.make" 2>&1; then
    fail "bogus: DATA_FS=nonsense was taken"
elif grep -q "DATA_FS is ext or fat" "$out/bogus.make"; then
    pass "bogus: DATA_FS=nonsense is refused"
else
    fail "bogus: refused, but not with the message: $(tail -2 "$out/bogus.make")"
fi

# ---- stick -----------------------------------------------------------------

if [ -f "$out/ext.img" ]; then
    cp "$out/ext.img" "$out/stick.img"
    gib=1
    python3 -c "import sys; open(sys.argv[1], 'r+b').truncate(int(sys.argv[2]) << 30)" "$out/stick.img" $gib
    # The ESP (1 MiB to the data partition's start) before, to compare after
    # (the MBR, in the first MiB, gets the new size).
    esp_mib=$(python3 tools/mkimage.py --data-at "$out/stick.img")
    esp_mib=${esp_mib%M}
    before=$(dd if="$out/stick.img" bs=1048576 skip=1 count=$((esp_mib - 1)) 2> /dev/null | cksum)
    python3 tools/mbr-grow.py "$out/stick.img" $((gib * 2048 * 1024)) > "$out/stick.grow" 2>&1 ||
        fail "stick: mbr-grow: $(cat "$out/stick.grow")"
    sh tools/stick-data.sh "$JX" "$out/stick.img" "$out/tree" > "$out/stick.data" 2>&1 ||
        fail "stick: stick-data.sh: $(cat "$out/stick.data")"
    after=$(dd if="$out/stick.img" bs=1048576 skip=1 count=$((esp_mib - 1)) 2> /dev/null | cksum)
    [ "$before" = "$after" ] && pass "stick: the ESP is what it was" || fail "stick: the ESP changed"
    [ "$(python3 tools/mkimage.py --data-fs "$out/stick.img")" = ext ] || fail "stick: not ext after"
    "$JX" --part 2 check "$out/stick.img" > "$out/stick.check" 2>&1 ||
        fail "stick: jamext check: $(cat "$out/stick.check")"
    mib=$("$JX" --part 2 info "$out/stick.img" 2> /dev/null | sed -n 's/^blocks: \([0-9]*\) .*/\1/p')
    want=$(((gib * 1024 - esp_mib) * 256))
    # one group less is allowed: a last group too small for its own metadata is left off
    if [ -n "$mib" ] && [ "$mib" -le "$want" ] && [ "$mib" -ge $((want - 32768)) ]; then
        pass "stick: the volume fills the grown partition ($mib blocks of $want)"
    else
        fail "stick: the volume has ${mib:-no} blocks, the partition $want"
    fi
    tree_ok "$out/stick.img" stick && pass "stick: the tree is in /data"
    fsck_part "$out/stick.img" stick $((esp_mib << 20)) $(((gib * 1024 - esp_mib) << 20))
    rm -f "$out/stick.img"
fi

# ---- dflt ------------------------------------------------------------------

if [ -f build/jamos.img ] && [ "$(python3 tools/mkimage.py --data-fs build/jamos.img)" = ext ]; then
    if "$JX" --part 2 check build/jamos.img > "$out/dflt.check" 2>&1; then
        pass "dflt: build/jamos.img is an ext image and clean"
    else
        fail "dflt: build/jamos.img: $(cat "$out/dflt.check")"
    fi
else
    echo "image-test: build/jamos.img is not an ext image here (DATA_FS=fat?): dflt skipped"
fi

if [ $fails = 0 ]; then
    echo "image-test: PASS"
else
    echo "image-test: $fails FAILED"
    exit 1
fi
