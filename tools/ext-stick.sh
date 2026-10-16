#!/bin/sh
# Ext volumes for the tests of extfs as a pager (docs/M13.6-PLAN.md's U10),
# made by Linux's e2fsprogs (the test PC's WSL has them, and mtools):
#
#   tools/ext-stick.sh <image>
#       a stick image (`make image`'s, or a copy) with its data partition,
#       FAT, made again as an ext volume holding the same files: extfs then
#       serves the stick's /data (volumes looks for an ext superblock on the
#       boot disk's data partition, whatever its type). The image changes in
#       place; its partition table and ESP don't.
#   tools/ext-stick.sh --volume <file> <tree> <label>
#       an ext volume in file (its size kept) holding the tree's files.
#
# Either way the volume has Jam OS's feature set (as tools/nvme-test.sh's
# jam case makes one: a journal, extents, 64-bit, metadata checksums, 4 KiB
# blocks) and every file and directory is owned by Jam OS's one user (uid
# and gid 1000), so the permission bits that count are the owner's.
# Exit 0 when it is made.
set -eu
export PATH=/usr/sbin:/sbin:$PATH
for t in mke2fs debugfs mcopy mdir; do
    command -v $t > /dev/null 2>&1 ||
        { echo "ext-stick: no $t here (needs e2fsprogs and mtools)"; exit 2; }
done

# volume <file> <tree> <label>
volume() {
    MKE2FS_CONFIG=/dev/null mke2fs -q -F -o Linux -b 4096 -I 256 -m 0 -J size=4 \
        -O none,has_journal,extent,flex_bg,64bit,sparse_super,large_file,huge_file,dir_nlink,extra_isize,metadata_csum,filetype \
        -E lazy_itable_init=0,lazy_journal_init=0,root_owner=1000:1000 \
        -L "$3" -d "$2" "$1" > "$1.mke2fs" 2>&1 ||
        { cat "$1.mke2fs"; echo "ext-stick: mke2fs failed"; exit 1; }
    (cd "$2" && find . -mindepth 1) | sed 's|^\./||' | while read -r p; do
        echo "sif \"/$p\" uid 1000"
        echo "sif \"/$p\" gid 1000"
    done > "$1.owners"
    debugfs -w -f "$1.owners" "$1" > "$1.debugfs" 2>&1 ||
        { cat "$1.debugfs"; echo "ext-stick: debugfs failed"; exit 1; }
    rm -f "$1.mke2fs" "$1.owners" "$1.debugfs"
}

if [ "${1:-}" = --volume ] && [ $# -eq 4 ]; then
    volume "$2" "$3" "$4"
    exit 0
fi
[ $# -eq 1 ] || { echo "usage: tools/ext-stick.sh <image> | --volume <file> <tree> <label>"; exit 2; }
img=$1
# The data partition: the MBR's entry 2 (tools/mkimage.py), in bytes.
set -- $(python3 -c "import struct, sys; d = open(sys.argv[1], 'rb').read(512)
lba, n = struct.unpack_from('<II', d, 446 + 16 + 8)
print(lba * 512, n * 512)" "$img")
at=$1 bytes=$2
tree="$img.tree" vol="$img.data"
rm -rf "$tree" "$vol"
mkdir -p "$tree"
if mdir -b -i "$img@@$at" :: 2> /dev/null | grep -q .; then
    mcopy -s -n -i "$img@@$at" "::/*" "$tree/"
fi
truncate -s "$bytes" "$vol"
volume "$vol" "$tree" JAMOS-DATA
dd if="$vol" of="$img" bs=512 seek=$((at / 512)) conv=notrunc status=none
rm -rf "$tree" "$vol"
echo "ext-stick: $img's data partition ($((bytes >> 20)) MiB at $((at >> 20)) MiB) is an ext volume"
