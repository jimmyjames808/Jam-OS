#!/bin/bash
# Make extcheck's fixture images (tools/extcheck.c) with Linux's own
# e2fsprogs: mke2fs makes each volume from a directory tree (-d), debugfs
# changes what -d can't make (other owners, device nodes, times past 2038,
# extended attributes, orphans), and tools/extfix/listing.py lists each
# image as debugfs reads it. What is committed (tools/extfix/) is each
# image gzipped (<name>.img.gz) and its listing (<name>.list); this script
# is the exact record of how they were made.
#
#     tools/remote-test.sh tools/extfix/make.sh build/extfix
#
# It runs on the test PC's WSL (Ubuntu 24.04, e2fsprogs 1.47.0, through
# tools/remote-test.sh, which copies build/extfix back), or on any Linux
# with e2fsprogs and python3; nothing is needed as root. Then copy
# build/extfix/*.img.gz and *.list into tools/extfix/.
#
# The images are made the same way every time: mke2fs reads no
# configuration file (MKE2FS_CONFIG=/dev/null: every choice is on its
# command line), the clock is fixed (E2FSPROGS_FAKE_TIME), the UUIDs and
# hash seeds are given, and the trees' bytes and times are fixed. Only the
# order in which -d finds a directory's names may differ between machines;
# each listing is made from its own image, so they always agree.
#
# The fixtures:
#   jam4k    Jam OS's own feature set (docs/M13.6-PLAN.md's subset), 4 KiB
#            blocks, 4 groups (flex_bg packs their bitmaps and tables).
#   linux1k  Linux's default ext4 (Ubuntu's mke2fs.conf: adds resize_inode,
#            dir_index and ext_attr), 1 KiB blocks: an indexed directory
#            (e2fsck -D), extended attributes in the inode and in a block,
#            an extent tree two levels deep, two inodes on the orphan list.
#   old2k    ext3 with uninit_bg (CRC16 group descriptors), 2 KiB blocks,
#            no extents: files mapped by direct, indirect, double and triple
#            indirect blocks. Mounted read-only by Jam OS.
set -eu
out=${1:?usage: tools/extfix/make.sh <outdir>}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
out=$(cd "$out" && pwd)
export PATH=/usr/sbin:/sbin:$PATH LC_ALL=C TZ=UTC
export MKE2FS_CONFIG=/dev/null E2FSPROGS_FAKE_TIME=1760000000
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
log=$out/make.log
: > "$log"

say() { echo "$*" | tee -a "$log"; }
run() { say "+ $*"; "$@" >> "$log" 2>&1; }

# `lines n`: n bytes of numbered lines (they compress well; every block
# differs).
lines() {
    python3 -c '
import sys
n = int(sys.argv[1])
s = b"".join(b"line %07d of the pattern\n" % i for i in range(n // 26 + 1))
sys.stdout.buffer.write(s[:n])' "$1"
}

# `block_at file bs at`: one block of bs bytes saying which block it is,
# written at block `at` of file (made if it isn't there; the rest holes).
block_at() {
    python3 -c '
import os, sys
path, bs, at = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
name = os.path.basename(path).encode()
with open(path, "r+b" if os.path.exists(path) else "wb") as f:
    f.seek(bs * at)
    f.write((b"block %d of %s\n" % (at, name)).ljust(bs, b"."))' "$1" "$2" "$3"
}

# The tree every fixture holds, in $1; $2 is the fixture's block size.
common_tree() {
    local d=$1 bs=$2
    mkdir -p "$d/a/b/c" "$d/sticky" "$d/many" "$d/emptydir"
    : > "$d/empty"
    printf 'hello, ext\n' > "$d/small"
    lines 4096 > "$d/block"
    lines 4097 > "$d/block1"
    lines 20000 > "$d/pattern"
    printf 'all: one\n' > "$d/Makefile"
    printf 'all: two\n' > "$d/makefile"
    printf 'deep\n' > "$d/a/b/c/deepfile"
    printf 'long name\n' > "$d/$(printf 'x%.0s' $(seq 250)).long"
    printf 'utf-8\n' > "$d/café-ü.txt"
    printf '#!/bin/sh\necho hi\n' > "$d/exec"
    cp "$d/exec" "$d/suid"
    cp "$d/exec" "$d/sgid"
    chmod 0600 "$d/small"
    chmod 0755 "$d/exec"
    chmod 04755 "$d/suid"
    chmod 02755 "$d/sgid"
    chmod 01777 "$d/sticky"
    chmod 0700 "$d/emptydir"
    ln "$d/small" "$d/small.hard"
    ln "$d/small" "$d/a/small.hard2"
    ln -s small "$d/fast"
    ln -s "$(printf 'a/b/c/../%.0s' $(seq 22))deepfile" "$d/slow"
    ln -s nowhere/at/all "$d/dangling"
    ln -s /etc/passwd "$d/abs"
    ln -s a "$d/toa"
    mkfifo "$d/pipe"
    for i in $(seq 0 149); do : > "$d/many/entry-$(printf %03d $i)-with-a-longer-name"; done
    # Blocks with holes between them: one extent each (a tree one level deep).
    for i in $(seq 0 39); do block_at "$d/holes" "$bs" $((2 * i)); done
    # A block at 5 GiB and one at the start: a sparse file past 4 GiB.
    block_at "$d/sparse5g" "$bs" 0
    block_at "$d/sparse5g" "$bs" $((5 * 1024 * 1024 * 1024 / bs))
    find "$d" -exec touch -h -d @1700000000.250000000 {} +
    touch -h -d @1000000000.123456789 "$d/small"
    touch -h -d @1234567890.987654321 "$d/a/b/c/deepfile"
}

# A listing of image $1 into $2 (listing.py), the header lines after it.
list() {
    local img=$1 lst=$2
    shift 2
    # The headers name the temporary directory as $work, so they read the
    # same on every run.
    python3 "$here/listing.py" "$img" "$lst" "${@//$work/\$work}" >> "$log" 2>&1
}

finish() {
    local name=$1
    gzip -9n -c "$work/$name.img" > "$out/$name.img.gz"
    say "$name: $(stat -c %s "$out/$name.img.gz") bytes gzipped"
}

# The feature sets: Jam OS's own (docs/M13.6-PLAN.md's subset), Linux's
# default ext4 (Ubuntu's mke2fs.conf), ext3 with uninit_bg.
JAM_FEATURES=none,has_journal,extent,flex_bg,64bit,sparse_super,large_file,huge_file
JAM_FEATURES+=,dir_nlink,extra_isize,metadata_csum,filetype
LINUX_FEATURES=none,has_journal,ext_attr,resize_inode,dir_index,filetype,extent,flex_bg
LINUX_FEATURES+=,sparse_super,large_file,huge_file,dir_nlink,extra_isize,64bit,metadata_csum
OLD_FEATURES=none,has_journal,ext_attr,resize_inode,dir_index,filetype,sparse_super
OLD_FEATURES+=,large_file,uninit_bg
LAZY=lazy_itable_init=0,lazy_journal_init=0

# ---- jam4k --------------------------------------------------------------------------
say "== jam4k"
mkdir "$work/jam4k"
common_tree "$work/jam4k" 4096
rm -f "$work/jam4k.img"
truncate -s 16M "$work/jam4k.img"
jam4k_mkfs=(mke2fs -q -F -o Linux -b 4096 -g 1024 -G 4 -I 256 -N 512 -m 0 -J size=4
    -O "$JAM_FEATURES" -U 4a414d34-0001-4000-8000-000000000001 -E "$LAZY"
    -L jam4k -d "$work/jam4k" "$work/jam4k.img")
run "${jam4k_mkfs[@]}"
jam4k_debugfs=(
    "write $work/jam4k/small rootfile"
    "sif rootfile uid 0" "sif rootfile gid 0"
    "write $work/jam4k/small bigid"
    "sif bigid uid 70000" "sif bigid gid 70001"
    "mknod chardev c 1 3" "mknod blkdev b 259 1000"
    "sif a/b/c/deepfile mtime @2147483647" "sif a/b/c/deepfile mtime_extra 0x1d6f3455"
    "sif a/b/c/deepfile atime @16" "sif a/b/c/deepfile atime_extra 0x00000003"
)
for c in "${jam4k_debugfs[@]}"; do run debugfs -w -R "$c" "$work/jam4k.img"; done
run e2fsck -fn "$work/jam4k.img"
list "$work/jam4k.img" "$out/jam4k.list" "${jam4k_mkfs[*]}" "${jam4k_debugfs[@]/#/debugfs -w -R }"
finish jam4k

# ---- linux1k ------------------------------------------------------------------------
say "== linux1k"
mkdir "$work/linux1k"
common_tree "$work/linux1k" 1024
# 400 one-block extents with holes between: more than a 1 KiB tree holds
# one level down (4 in the inode, 84 in a block), so two levels.
for i in $(seq 0 399); do block_at "$work/linux1k/deep" 1024 $((2 * i)); done
lines 600 > "$work/bigvalue"
rm -f "$work/linux1k.img"
truncate -s 32M "$work/linux1k.img"
linux1k_mkfs=(mke2fs -q -F -o Linux -b 1024 -I 256 -N 1024 -m 5 -J size=1
    -O "$LINUX_FEATURES" -U 4a414d34-0002-4000-8000-000000000002
    -E "$LAZY,hash_seed=4a414d34-0002-4000-8000-0000000000aa"
    -L linux1k -d "$work/linux1k" "$work/linux1k.img")
run "${linux1k_mkfs[@]}"
linux1k_debugfs=(
    "ea_set small user.note hello"
    "ea_set -f $work/bigvalue pattern user.big"
    "mknod chardev c 4 64"
)
for c in "${linux1k_debugfs[@]}"; do run debugfs -w -R "$c" "$work/linux1k.img"; done
# Index every directory of more than one block (many/), as Linux would
# have as it grew.
run e2fsck -fyD "$work/linux1k.img" || true
run e2fsck -fn "$work/linux1k.img"
# Two files unlinked while open, on the orphan list (last_orphan, then
# each inode's dtime to the next), as a cut would leave them.
run debugfs -w -R "write $work/linux1k/small orphan1" "$work/linux1k.img"
run debugfs -w -R "write $work/linux1k/pattern orphan2" "$work/linux1k.img"
o1=$(debugfs -R "ls -p /" "$work/linux1k.img" 2>/dev/null | awk -F/ '$6 == "orphan1" { print $2 }')
o2=$(debugfs -R "ls -p /" "$work/linux1k.img" 2>/dev/null | awk -F/ '$6 == "orphan2" { print $2 }')
linux1k_orphans=(
    "unlink orphan1" "unlink orphan2"
    "sif <$o1> links_count 0" "sif <$o2> links_count 0"
    "sif <$o2> dtime @$o1" "sif <$o1> dtime @0" "ssv last_orphan $o2"
)
for c in "${linux1k_orphans[@]}"; do run debugfs -w -R "$c" "$work/linux1k.img"; done
list "$work/linux1k.img" "$out/linux1k.list" "${linux1k_mkfs[*]}" \
    "${linux1k_debugfs[@]/#/debugfs -w -R }" "e2fsck -fyD" "debugfs -w -R write small orphan1" \
    "debugfs -w -R write pattern orphan2" "${linux1k_orphans[@]/#/debugfs -w -R }" \
    "orphans $o2 $o1"
finish linux1k

# ---- old2k --------------------------------------------------------------------------
say "== old2k"
mkdir "$work/old2k"
common_tree "$work/old2k" 2048
# Double indirect: block 1000 (past 12 direct and 512 single indirect).
block_at "$work/old2k/dbl" 2048 1000
rm -f "$work/old2k.img"
truncate -s 24M "$work/old2k.img"
old2k_mkfs=(mke2fs -q -F -o Linux -b 2048 -g 4096 -I 256 -N 768 -m 5 -J size=2
    -O "$OLD_FEATURES" -U 4a414d34-0003-4000-8000-000000000003
    -E "$LAZY,hash_seed=4a414d34-0003-4000-8000-0000000000bb"
    -L old2k -d "$work/old2k" "$work/old2k.img")
run "${old2k_mkfs[@]}"
run e2fsck -fyD "$work/old2k.img" || true
run e2fsck -fn "$work/old2k.img"
list "$work/old2k.img" "$out/old2k.list" "${old2k_mkfs[*]}" "e2fsck -fyD"
finish old2k

say "done: $out"
