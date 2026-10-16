#!/bin/sh
# extfs on a Jam OS NVMe drive, with Linux's e2fsprogs as the judge
# (docs/M13.6-PLAN.md, U9). Each case is a shard of its own: give each its
# own output directory to run them side by side. A case boots a drive made
# on the host (tools/mkgpt.py's jamos layout: the ESP, and a data partition
# that is an ext volume Linux's mke2fs made, as tools/nvme-test.sh's and
# tools/pager-test.sh's jam cases make one), which Jam OS mounts at /jam,
# types commands into the shell, and then Linux's e2fsck and debugfs read
# the volume. These need e2fsprogs and mtools: the test PC's WSL has them.
#
#     tools/extfs-test.sh <outdir> <case>
#
# Cases:
#   basic    files and directories of every size made by mke2fs read by Jam
#            OS (hashes the host's), and made by Jam OS (copied, replaced,
#            renamed, removed, 300 files removed with `rm -r`, 100 synced
#            files); a second boot finds it all; debugfs reads what Jam OS
#            wrote (hashes the host's) and e2fsck -fn calls the volume clean.
#   links    symbolic links (relative, to a directory, chains, dangling, a
#            loop, one whose target is too long to sit in the inode) and
#            hard links made by mke2fs, followed and changed by Jam OS;
#            debugfs sees the links as they should be, e2fsck -fn clean.
#   case     names that differ only by case (Makefile, makefile, MAKEFILE),
#            UTF-8 names, spaces and a 255-byte name: kept apart by Jam OS
#            on /jam, made and renamed there; on /data (FAT) the same pair
#            of names is one file.
#   big      `exttest big` writes 6 GiB and reads it back, `exttest sparse`
#            a file with data at 0, at 5 GiB and at 6 GiB (holes between,
#            one at the end): the hashes Jam OS prints are the host's
#            (tools/extbigsum.py), the file read out of the volume by
#            debugfs is too, the sparse file holds no more than its data;
#            a second boot hashes both again with the shell's sha256sum.
#   cut      QEMU killed (kill -9) at 20 points of a `disktest` storm of
#            synced files, each boot cut at the Nth write of the trace of
#            the NVMe drive (tools/nvme-write-test.sh's cutstorm, on ext);
#            after each: Linux's `e2fsck -fy` of the cut image (it replays
#            the journal) is clean and holds the files Jam OS's recovery
#            then finds (the same hashes), `disktest check` after Jam OS's
#            recovery finds every synced file whole, and e2fsck -fn of the
#            recovered image is clean.
#   storm    `storm` copies a file of random bytes within /jam (64 MiB) while
#            extfs-jam is killed 0, 1, 10 and 100 times a second: every copy
#            MATCHes (both files hashed again from the disk), extfs carries
#            on each time (never starting fresh), no start replays more than
#            24 journal blocks (the journal's tail stays close behind: it
#            was 100 to 250, and at 10 kills a second a start took longer
#            than the instance before it lived), e2fsck -fn clean.
#   interop  three volumes mke2fs made (Jam OS's feature set with 4 KiB
#            blocks, Linux's default ext4 with 1 KiB blocks, ext3's with
#            2 KiB and block-mapped files), each written by Jam OS (a tree
#            copied from the stick's /data: 60 small files, a 8 MiB one,
#            deep and empty directories, some removed): e2fsck -fn clean,
#            debugfs's rdump the tree, byte for byte.
#   foreign  other people's volumes (Q9): a Linux ext4 stick at /usb0, a
#            Linux GPT drive whose first volume has a journal to replay and
#            whose second has a feature Jam OS doesn't know, a second Jam OS
#            drive: all read-only (hashes the host's), no write or `mount
#            -w` lets through, every image unchanged to the byte; /jam
#            beside them writable.
#
# Into <outdir>: each boot's log (extfs-<case>[-n].log, the run up to the
# `reboot -f` in .run.log), the host's judges' outputs (*.fsck, *.out) and
# the cases' generated shell scripts (*.in). Exit 0 when the case passed.
# QEMU_SMP, QEMU_MEM and QEMU_TIMEOUT pass through.
# EXTFS_STORM_RATES, EXTFS_CUT_POINTS name the rates and the cuts (writes
# after the storm began) of those cases, e.g. EXTFS_CUT_POINTS="1 5 21".
set -eu
out=${1:?usage: tools/extfs-test.sh <outdir> <case>}
c=${2:?usage: tools/extfs-test.sh <outdir> <case>}
export PATH=/usr/sbin:/sbin:$PATH LC_ALL=C
mkdir -p "$out"
ok=1
name=extfs-$c

count() { grep -acE -- "$1" "$2" || true; }
# What no run's log may say (the kernel's "0 failed" is no failure).
bad="can't |MISMATCH|DIFFERENT|[^0-9] failed"
# want <how many times exactly> <text (a regex)> <log>
want() {
    n=$(count "$2" "$3")
    [ "$n" -eq "$1" ] || { echo "$name: \"$2\" $n times in $(basename "$3"), wanted $1"; ok=0; }
}
never() {   # never <text> <log>
    n=$(count "$1" "$2")
    [ "$n" -eq 0 ] || { echo "$name: \"$1\" $n times in $(basename "$2")"; ok=0; }
}
fail() { echo "$name: $*"; ok=0; }

# The tools the cases need, or say what is missing.
ext_tools() {
    for t in mke2fs debugfs e2fsck mcopy mformat sha256sum python3 truncate; do
        command -v $t > /dev/null 2>&1 && continue
        echo "$name: no $t here (needs e2fsprogs and mtools: the test PC's WSL has them)"
        exit 2
    done
}

# ---- host files -------------------------------------------------------------------------

# seeded <file> <MiB> <seed>: that many MiB of the same bytes every time.
seeded() {
    python3 -c "import random, sys; sys.stdout.buffer.write(random.Random($3).randbytes($2 << 20))" > "$1"
}

# sha <file>: its SHA-256.
sha() { sha256sum "$1" | cut -d' ' -f1; }

# sums <dir> <rel>...: the lines `sha256sum <prefix>/<rel>...` prints in the
# guest (hash, two spaces, path), for the files of dir, into $sums. The
# prefix is $guest.
sums_add() {
    _d=$1
    shift
    for _r in "$@"; do
        echo "$(sha "$_d/$_r")  $guest/$_r" >> "$sums"
    done
}
# sums_seen <log>: every line of $sums is in the log.
sums_seen() {
    while read -r _h _p; do
        _n=$(count "$_h  *$_p" "$1")
        [ "$_n" -ge 1 ] || fail "the guest's sha256sum of $_p isn't $_h (the host's)"
    done < "$sums"
}

# ---- the volumes ------------------------------------------------------------------------

JAM_FEATURES=none,has_journal,extent,flex_bg,64bit,sparse_super,large_file,huge_file,dir_nlink,extra_isize,metadata_csum,filetype
LINUX_FEATURES=none,has_journal,ext_attr,resize_inode,dir_index,filetype,extent,flex_bg,sparse_super,large_file,huge_file,dir_nlink,extra_isize,64bit,metadata_csum
PLAIN_FEATURES=none,has_journal,ext_attr,resize_inode,dir_index,filetype,extent,flex_bg,sparse_super,large_file,huge_file,dir_nlink,extra_isize,metadata_csum
OLD_FEATURES=none,has_journal,ext_attr,resize_inode,dir_index,filetype,sparse_super,large_file,uninit_bg

# make_volume <file> <MiB> <tree> <label> [<variant>]: an ext volume in
# <file> of that many MiB holding the tree's files, every file owned by
# Jam OS's one user (uid and gid 1000, so the permission bits that count
# are the owner's). The variant is mke2fs's: jam4k (default: Jam OS's
# feature set, 4 KiB blocks), linux4k (Linux's default ext4), linux1k (the
# same with 1 KiB blocks), plain2k (ext4 without 64bit, 2 KiB
# blocks), old2k (ext3's: 2 KiB blocks, block maps, uninit_bg).
make_volume() {
    truncate -s "$2M" "$1"
    case ${5:-jam4k} in
    jam4k) bs=4096 feat=$JAM_FEATURES pct=0 ;;
    linux4k) bs=4096 feat=$LINUX_FEATURES pct=5 ;;
    linux1k) bs=1024 feat=$LINUX_FEATURES pct=5 ;;
    plain2k) bs=2048 feat=$PLAIN_FEATURES pct=5 ;;
    old2k) bs=2048 feat=$OLD_FEATURES pct=5 ;;
    esac
    MKE2FS_CONFIG=/dev/null mke2fs -q -F -o Linux -b $bs -I 256 -m $pct -J size=4 -O "$feat" \
        -E lazy_itable_init=0,lazy_journal_init=0,root_owner=1000:1000 \
        -L "$4" -d "$3" "$1" > "$1.mke2fs" 2>&1 ||
        { cat "$1.mke2fs"; echo "$name: mke2fs failed"; exit 1; }
    (cd "$3" && find . -mindepth 1) | sed 's|^\./||' | while read -r p; do
        echo "sif \"/$p\" uid 1000"
        echo "sif \"/$p\" gid 1000"
    done > "$1.owners"
    debugfs -w -f "$1.owners" "$1" > "$1.debugfs" 2>&1 ||
        { cat "$1.debugfs"; echo "$name: debugfs failed"; exit 1; }
    rm -f "$1.mke2fs" "$1.owners" "$1.debugfs"
}

# make_drive <drive> <volume> [<type>]: <drive> is a Jam OS NVMe drive (the
# ESP a FAT volume, the data partition at 513 MiB holding <volume>).
make_drive() {
    vm=$(( ($(stat -c %s "$2") + (1 << 20) - 1) >> 20 ))
    esp="$1.esp"
    truncate -s 128M "$esp"
    mformat -i "$esp" -T $((128 * 2048)) -F -v JAMOS-ESP :: || { echo "$name: mformat"; exit 1; }
    python3 tools/mkgpt.py "$1" --layout jamos --size $((513 + vm + 16))M --fill "1=$esp" > /dev/null
    dd if="$2" of="$1" bs=1M seek=513 conv=notrunc,sparse status=none
    rm -f "$esp"
}

# volume_of <drive> <MiB> <file>: the data partition's first <MiB> MiB.
volume_of() {
    dd if="$1" of="$3" bs=1M iflag=skip_bytes skip=$((513 << 20)) count="$2" conv=sparse status=none
}

# clean <drive> <MiB> <what>: the ext volume in the data partition (its
# first <MiB>) is what e2fsck -fn calls clean.
clean() {
    vol="$out/$name-check.img"
    volume_of "$1" "$2" "$vol"
    if ! e2fsck -fn "$vol" > "$out/$name-$3.fsck" 2>&1; then
        fail "e2fsck doesn't call the volume clean $3 (see $out/$name-$3.fsck)"
    fi
    rm -f "$vol"
}

# ---- one boot ---------------------------------------------------------------------------

# boot <run> <script> <drive>...: the shell script typed into a boot with
# these NVMe drives; also $bootimg as the boot stick's image, $usb as more
# QEMU devices, $throttle and $qextra. Fails the case if the run did; the
# run's log up to the `reboot -f` at the end is $run.
boot() {
    rn=$1 script=$2
    shift 2
    if ! QEMU_TIMEOUT=${QEMU_TIMEOUT:-900} QEMU_INPUT="$script" QEMU_NVME="$*" \
         QEMU_IMAGE="${bootimg:-}" QEMU_USB="${usb:-}" QEMU_NVME_THROTTLE="${throttle:-}" \
         QEMU_EXTRA="${qextra:-}" tools/qemu-test.sh "$out" "$rn" shell > "$out/$rn.out" 2>&1; then
        fail "the shell script FAILED (see $out/$rn.log)"
        grep "serial-feed: .*no '" "$out/$rn.out" || true
    fi
    run="$out/$rn.run.log"
    sed '/jam[^>]*>.* reboot -f/q' "$out/$rn.log" > "$run"
}

# ---- the shell scripts ------------------------------------------------------------------

# A marker built from $T (set to k) never appears in the typed line, only
# when the command before it ran.
g_top() {
    echo "wait 120 Jam OS shell"
    echo "wait {prompt}"
    echo "seen 60 init: /data mounted"
    echo "send T=k"
    echo "wait {prompt}"
    echo "seen 60 init: /jam mounted"
}
# g <tag> <command...>: type it, wait for its status ($gwait s at most).
g() {
    t=$1
    shift
    echo "send $*; echo $t-\$T=\$?"
    echo "wait ${gwait:-120} $t-k="
    echo "wait {prompt}"
}
g_end() {
    echo "send sync"
    echo "wait {prompt}"
    echo "send reboot -f"
    echo "wait reboot: resetting"
}

# status <tag> <value>: the command g gave <tag> said <value> in the log.
status() {
    want 1 "$1-k=$2" "$run"
}
# refused <tag>: it ended, and not with 0.
refused() {
    want 1 "$1-k=" "$run"
    want 0 "$1-k=0" "$run"
}

# ---- basic -------------------------------------------------------------------------------

basic_tree() {
    t=$1
    mkdir -p "$t/dir/sub" "$t/sizes" "$t/many"
    printf 'hello from mke2fs\n' > "$t/hello.txt"
    seeded "$t/dir/a.bin" 1 21
    seeded "$t/dir/b.bin" 3 22
    head -c 17 /dev/zero | tr '\0' 'x' >> "$t/dir/b.bin"
    printf 'deep\n' > "$t/dir/sub/deep.txt"
    : > "$t/sizes/s0"
    printf 'x' > "$t/sizes/s1"
    for n in 4095 4096 4097 65536; do head -c $n "$t/dir/a.bin" > "$t/sizes/s$n"; done
    for i in $(seq 1 300); do printf 'file %d\n' $i > "$t/many/f$(printf %03d $i)"; done
}

case_basic() {
    tree="$out/$name-tree"
    drive="$out/$name-drive.img"
    rm -rf "$tree" "$drive"
    basic_tree "$tree"
    make_volume "$out/$name-data.img" 256 "$tree" jamdata
    make_drive "$drive" "$out/$name-data.img"
    rm -f "$out/$name-data.img"
    guest=/jam sums="$out/$name.sums"
    : > "$sums"
    sums_add "$tree" hello.txt dir/a.bin dir/b.bin dir/sub/deep.txt sizes/s0 sizes/s1 sizes/s4095 \
        sizes/s4096 sizes/s4097 sizes/s65536
    # `exttest digest /jam/many`: the SHA-256 of "hash  name" lines in byte order.
    many=$(cd "$tree/many" && ls | LC_ALL=C sort | while read -r f; do
        echo "$(sha256sum < "$f" | cut -d' ' -f1)  $f"
    done | sha256sum | cut -d' ' -f1)
    sb=$(sha "$tree/dir/b.bin")
    s4097=$(sha "$tree/sizes/s4097")
    first=$(printf 'first line\nsecond line\n' | sha256sum | cut -d' ' -f1)
    {
        echo "# generated by tools/extfs-test.sh: basic, the first boot"
        g_top
        g ls1 ls /jam
        g ls2 ls -l /jam/sizes
        g cat1 cat /jam/hello.txt
        g sum1 sha256sum /jam/hello.txt /jam/dir/a.bin /jam/dir/b.bin /jam/dir/sub/deep.txt
        g sum2 sha256sum /jam/sizes/s0 /jam/sizes/s1 /jam/sizes/s4095 /jam/sizes/s4096 /jam/sizes/s4097 /jam/sizes/s65536
        g cnt1 exttest digest /jam/many
        g df1 df
        g mk1 mkdir -p /jam/w/a/b/c
        g cp1 cp /jam/dir/a.bin /jam/w/a1.bin
        g cp2 cp /jam/dir/b.bin /jam/w/a/b/c/b1.bin
        g cp3 cp /jam/sizes/s4097 /jam/w
        g wr1 write /jam/w/t.txt first line
        g wr2 write -a /jam/w/t.txt second line
        g cat2 cat /jam/w/t.txt
        g cp4 cp /jam/dir/b.bin /jam/w/a1.bin
        g mv1 mv /jam/w/a1.bin /jam/w/a/moved.bin
        g mv2 mv /jam/w/a /jam/w2
        g rm1 rm /jam/dir/a.bin
        g rm2 rm -r /jam/many
        g ls3 ls /jam/many
        g mk2 mkdir /jam/many2
        g dt1 disktest /jam/many2 write 100 8
        g dt2 disktest /jam/many2 check
        g sum3 sha256sum /jam/w2/moved.bin /jam/w2/b/c/b1.bin /jam/w/s4097 /jam/w/t.txt
        g df2 df
        g_end
    } > "$out/$name-1.in"
    boot "$name-1" "$out/$name-1.in" "$drive"
    want 1 "^hello from mke2fs" "$run"
    sums_seen "$run"
    want 1 "exttest: digest /jam/many: 300 files, sha256 $many" "$run"
    status cp4 0
    status rm2 0
    refused ls3
    status dt1 0
    want 1 "disktest: check /jam/many2: files=100 whole=100 damaged=0 missing=0 prefix=100" "$run"
    want 1 "^first line" "$run"
    want 1 "^second line" "$run"
    want 1 "^$sb  /jam/w2/moved.bin" "$run"
    want 1 "^$sb  /jam/w2/b/c/b1.bin" "$run"
    want 1 "^$s4097  /jam/w/s4097" "$run"
    want 1 "^$first  /jam/w/t.txt" "$run"
    never "$bad" "$run"
    clean "$drive" 256 "after the first boot"
    vol="$out/$name-1.img"
    volume_of "$drive" 256 "$vol"
    # What debugfs reads is what Jam OS wrote.
    for f in w2/moved.bin w2/b/c/b1.bin; do
        [ "$(debugfs -R "cat /$f" "$vol" 2> /dev/null | sha256sum | cut -d' ' -f1)" = "$sb" ] ||
            fail "debugfs reads /$f, and it isn't the file Jam OS copied"
    done
    [ "$(debugfs -R "cat /w/s4097" "$vol" 2> /dev/null | sha256sum | cut -d' ' -f1)" = "$s4097" ] ||
        fail "debugfs reads /w/s4097, and it isn't the file Jam OS copied"
    [ "$(debugfs -R "cat /w/t.txt" "$vol" 2> /dev/null | sha256sum | cut -d' ' -f1)" = "$first" ] ||
        fail "debugfs reads /w/t.txt, and it isn't what Jam OS wrote"
    debugfs -R "stat /dir/a.bin" "$vol" 2>&1 | grep -q "File not found" || fail "/dir/a.bin is still there"
    debugfs -R "stat /many" "$vol" 2>&1 | grep -q "File not found" || fail "/many is still there"
    n=$(debugfs -R "ls -p /many2" "$vol" 2> /dev/null | grep -c "/dt-")
    [ "$n" -eq 100 ] || fail "/many2 holds $n of the 100 files"
    rm -f "$vol"
    # A second boot finds everything.
    {
        echo "# generated by tools/extfs-test.sh: basic, the second boot"
        g_top
        g sum4 sha256sum /jam/w2/moved.bin /jam/w2/b/c/b1.bin /jam/w/s4097 /jam/w/t.txt
        g dt3 disktest /jam/many2 check
        g ls4 ls /jam/w2
        g ls5 ls /jam/dir
        g_end
    } > "$out/$name-2.in"
    boot "$name-2" "$out/$name-2.in" "$drive"
    want 1 "^$sb  /jam/w2/moved.bin" "$run"
    want 1 "^$sb  /jam/w2/b/c/b1.bin" "$run"
    want 1 "^$first  /jam/w/t.txt" "$run"
    want 1 "disktest: check /jam/many2: files=100 whole=100 damaged=0 missing=0 prefix=100" "$run"
    never "$bad" "$run"
    clean "$drive" 256 "after the second boot"
    rm -rf "$tree" "$drive"
}

# ---- storm -------------------------------------------------------------------------------

case_storm() {
    tree="$out/$name-tree"
    drive="$out/$name-drive.img"
    rates=${EXTFS_STORM_RATES:-0 1 10 100}
    rm -rf "$tree" "$drive"
    mkdir -p "$tree"
    seeded "$tree/src.bin" 64 13
    make_volume "$out/$name-data.img" 256 "$tree" jamdata
    make_drive "$drive" "$out/$name-data.img"
    rm -rf "$tree" "$out/$name-data.img"
    {
        echo "# generated by tools/extfs-test.sh: storm"
        echo "# storm copies /jam/src.bin (64 MiB of random bytes, put on the volume by the host's"
        echo "# mke2fs) to a new file on /jam while extfs-jam is killed $rates times a"
        echo "# second; each copy must MATCH (both files hashed again from the disk)."
        g_top
        for k in $rates; do
            gwait=900 g s$k storm /jam/src.bin /jam/d$k.bin $k extfs-jam
            g r$k rm /jam/d$k.bin
        done
        g_end
    } > "$out/$name.in"
    boot "$name" "$out/$name.in" "$drive"
    for k in $rates; do
        want 1 "\\[shell\\] storm: $k kills/s: .*; MATCH" "$run"
        status s$k 0
    done
    n=$(count "extfs /jam: restart \\(killed" "$run")
    [ "$n" -ge 2 ] || fail "$n restarts of extfs /jam carried on, wanted 2 or more"
    never "extfs /jam: restart .*starting fresh|giving up" "$run"
    never "DIFFERENT|MISMATCH" "$run"
    # The journal's tail stays close behind: no start replayed more than a
    # few transactions (with the tail moving every 64 commits it was 100 to
    # 250 blocks, and at 10 kills a second a start took longer than the
    # instance before it lived: the copy never got anywhere).
    most=$(sed -n 's/.*the journal: [0-9]* transaction(s), \([0-9]*\) block(s) replayed.*/\1/p' "$run" |
           sort -n | tail -1)
    [ "${most:-0}" -le 24 ] || fail "a start replayed $most blocks, wanted 24 at most"
    clean "$drive" 256 "after the run"
    rm -f "$drive"
}

# ---- links -------------------------------------------------------------------------------

links_tree() {
    t=$1
    mkdir -p "$t/d/e" "$t/dd"
    printf 'file f\n' > "$t/f.txt"
    printf 'file g\n' > "$t/d/g.txt"
    printf 'file h\n' > "$t/d/e/h.txt"
    printf 'hard\n' > "$t/hl-a"
    ln "$t/hl-a" "$t/hl-b"
    ln "$t/hl-a" "$t/hl-c"
    ln "$t/hl-a" "$t/d/hl-d"
    ln -s f.txt "$t/sl-rel"
    ln -s d "$t/sl-dir"
    ln -s d/e/h.txt "$t/sl-deep"
    ln -s d/../d/g.txt "$t/sl-dd"
    ln -s ../f.txt "$t/d/up"
    i=1
    while [ $i -lt 20 ]; do
        ln -s chain-$((i + 1)) "$t/chain-$i"
        i=$((i + 1))
    done
    ln -s f.txt "$t/chain-20"
    ln -s no-such-file "$t/dangling"
    ln -s loop-b "$t/loop-a"
    ln -s loop-a "$t/loop-b"
    p=$(printf 'p%.0s' $(seq 1 100))
    q=$(printf 'q%.0s' $(seq 1 100))
    mkdir -p "$t/$p"
    printf 'long target\n' > "$t/$p/$q"
    ln -s "$p/$q" "$t/long"   # 201 bytes: too long to sit in the inode
    ln -s ../d "$t/dd/to-d"
}

case_links() {
    tree="$out/$name-tree"
    drive="$out/$name-drive.img"
    rm -rf "$tree" "$drive"
    links_tree "$tree"
    make_volume "$out/$name-data.img" 64 "$tree" jamdata
    make_drive "$drive" "$out/$name-data.img"
    vol="$out/$name-before.img"
    cp "$out/$name-data.img" "$vol"
    rm -f "$out/$name-data.img"
    # What Linux says before Jam OS touches the volume: three names for the
    # hard-linked file, a link to the long path.
    n=$(debugfs -R "stat /hl-a" "$vol" 2> /dev/null | sed -n 's/.*Links: \([0-9]*\).*/\1/p')
    [ "$n" = 4 ] || { echo "$name: the host's volume has $n links to hl-a, wanted 4"; exit 1; }
    debugfs -R "stat /long" "$vol" 2> /dev/null | grep -q "Type: symlink" ||
        { echo "$name: the host's volume has no symlink /long"; exit 1; }
    guest=/jam sums="$out/$name.sums"
    : > "$sums"
    sums_add "$tree" sl-rel sl-dir/g.txt sl-deep sl-dd d/up chain-1 chain-20 long hl-b hl-c d/hl-d
    h=$(sha "$tree/d/g.txt")
    {
        echo "# generated by tools/extfs-test.sh: links"
        g_top
        g r1 sha256sum /jam/sl-rel /jam/sl-dir/g.txt /jam/sl-deep /jam/sl-dd /jam/d/up
        g r2 sha256sum /jam/chain-1 /jam/chain-20 /jam/long /jam/hl-b /jam/hl-c /jam/d/hl-d
        g r3 ls /jam/sl-dir
        g r4 sha256sum /jam/dangling
        g r5 sha256sum /jam/loop-a
        g r6 sha256sum /jam/chain-9
        g h1 write -a /jam/hl-b more
        g h2 sha256sum /jam/hl-c /jam/d/hl-d
        g h3 rm /jam/hl-a
        g h4 cat /jam/hl-c
        g w1 write /jam/sl-rel changed
        g w2 cat /jam/f.txt
        g m1 mv /jam/sl-deep /jam/sl-moved
        g m2 sha256sum /jam/sl-moved /jam/d/e/h.txt
        g d1 rm /jam/sl-dir
        g d2 sha256sum /jam/d/g.txt
        g d3 rm -r /jam/dd
        g d4 sha256sum /jam/d/g.txt /jam/d/e/h.txt
        g c1 cp /jam/sl-moved /jam/copy-of-h
        g c2 sha256sum /jam/copy-of-h
        g_end
    } > "$out/$name.in"
    boot "$name" "$out/$name.in" "$drive"
    sums_seen "$run"
    status r3 0
    refused r4
    refused r5
    status r6 0
    status w1 0
    status d1 0
    status d3 0
    status c1 0
    hh=$(sha "$tree/d/e/h.txt")
    want 2 "^$h  /jam/d/g.txt" "$run"
    want 2 "^$hh  /jam/d/e/h.txt" "$run"
    want 1 "^$hh  /jam/sl-moved" "$run"
    want 1 "^$hh  /jam/copy-of-h" "$run"
    want 1 "^changed" "$run"
    # The one thing Jam OS can't read is the loop.
    want 1 "can't read it" "$run"
    want 1 "loop-a: can't read it \\(ERR_LINK_LOOP\\)" "$run"
    never "MISMATCH|DIFFERENT|[^0-9] failed" "$run"
    # Linux's view: the same names, the links as they should be.
    clean "$drive" 64 "after the run"
    volume_of "$drive" 64 "$vol"
    stat_of() { debugfs -R "stat $1" "$vol" 2>&1; }
    n=$(stat_of /hl-b | sed -n 's/.*Links: \([0-9]*\).*/\1/p')
    [ "$n" = 3 ] || fail "/hl-b has $n links after hl-a was removed, wanted 3"
    [ "$(debugfs -R "cat /hl-c" "$vol" 2> /dev/null | tr '\n' ' ')" = "hard more " ] ||
        fail "/hl-c isn't \"hard\" and \"more\" (the file hl-b's append went to)"
    [ "$(debugfs -R "cat /f.txt" "$vol" 2> /dev/null)" = changed ] ||
        fail "/f.txt isn't \"changed\": the write through sl-rel went elsewhere"
    stat_of /sl-moved | grep -q 'Fast link dest: "d/e/h.txt"' || fail "/sl-moved isn't a link to d/e/h.txt"
    stat_of /sl-deep | grep -q "File not found" || fail "/sl-deep is still there after the mv"
    stat_of /sl-dir | grep -q "File not found" || fail "/sl-dir is still there after rm"
    stat_of /dd | grep -q "File not found" || fail "/dd is still there after rm -r"
    for f in d/g.txt d/e/h.txt; do
        stat_of /$f | grep -q "Type: regular" || fail "/$f was removed: rm followed a link"
    done
    stat_of /copy-of-h | grep -q "Type: regular" || fail "/copy-of-h isn't a regular file (cp copied the link)"
    stat_of /dangling | grep -q 'Fast link dest: "no-such-file"' || fail "/dangling isn't what it was"
    stat_of /loop-a | grep -q 'Fast link dest: "loop-b"' || fail "/loop-a isn't what it was"
    rm -f "$vol"
    rm -rf "$tree" "$drive"
}

# ---- case --------------------------------------------------------------------------------

# The names with bytes over 127, as printf's octal: the shell's input takes
# ASCII only, so Jam OS reads them with `exttest sums` and copies them with
# `exttest copy`.
NFC='caf\303\251.txt'
NFD='cafe\314\201.txt'
JA='\346\227\245\346\234\254.txt'

case_tree() {
    t=$1
    printf 'upper M\n' > "$t/Makefile"
    printf 'lower m\n' > "$t/makefile"
    printf 'all caps\n' > "$t/MAKEFILE"
    printf 'README upper\n' > "$t/README"
    printf 'readme lower\n' > "$t/readme"
    printf 'with space\n' > "$t/a b.txt"
    printf 'hidden\n' > "$t/.hidden"
    printf 'dash\n' > "$t/-dash"
    printf 'nfc\n' > "$t/$(printf "$NFC")"
    printf 'nfd\n' > "$t/$(printf "$NFD")"
    printf 'nihon\n' > "$t/$(printf "$JA")"
    printf 'two hundred\n' > "$t/$(printf 'o%.0s' $(seq 1 200))"
    printf 'the longest name\n' > "$t/$(printf 'n%.0s' $(seq 1 255))"
    printf 'the longest path\n' > "$t/$(printf 'p%.0s' $(seq 1 250))"
}

case_case() {
    tree="$out/$name-tree"
    drive="$out/$name-drive.img"
    rm -rf "$tree" "$drive"
    mkdir -p "$tree"
    case_tree "$tree"
    make_volume "$out/$name-data.img" 64 "$tree" jamdata
    make_drive "$drive" "$out/$name-data.img"
    rm -f "$out/$name-data.img"
    # What `exttest sums /jam` must print: every file with a short name (the
    # log breaks a long line), the host's hash; and `exttest digest /jam`,
    # one line, every file but the one whose path is over 255 bytes
    # (FS_PATH_MAX), which it can't open.
    guest=/jam sums="$out/$name.sums"
    (cd "$tree" && find . -maxdepth 1 -type f | sed 's|^\./||' | while IFS= read -r f; do
        if [ ${#f} -le 100 ]; then
            echo "$(sha256sum < "$f" | cut -d' ' -f1)  /jam/$f"
        fi
    done) > "$sums"
    (cd "$tree" && ls -A) > "$out/$name.files"
    dall=$(cd "$tree" && ls -A | LC_ALL=C sort | while IFS= read -r f; do
        if [ ${#f} -le 250 ]; then
            echo "$(sha256sum < "$f" | cut -d' ' -f1)  $f"
        fi
    done | sha256sum | cut -d' ' -f1)
    o200=$(printf 'o%.0s' $(seq 1 200))
    m200=$(printf 'm%.0s' $(seq 1 200))
    n255=$(printf 'n%.0s' $(seq 1 255))
    ho=$(sha "$tree/$o200")
    one=$(printf 'one\n' | sha256sum | cut -d' ' -f1)
    two=$(printf 'two\n' | sha256sum | cut -d' ' -f1)
    three=$(printf 'three\n' | sha256sum | cut -d' ' -f1)
    sp=$(sha "$tree/a b.txt")
    {
        echo "# generated by tools/extfs-test.sh: case"
        g_top
        g a1 sha256sum /jam/Makefile /jam/makefile /jam/MAKEFILE /jam/README /jam/readme /jam/.hidden /jam/-dash
        g a2 sha256sum '"/jam/a b.txt"'
        g a3 exttest sums /jam
        g a3b exttest digest /jam
        g a4 sha256sum /jam/$o200
        g a5 ls /jam
        g b1 write /jam/Foo one
        g b2 write /jam/foo two
        g b3 write /jam/FOO three
        g b4 sha256sum /jam/Foo /jam/foo /jam/FOO
        g b5 mv /jam/foo /jam/Bar
        g b6 mv /jam/Bar /jam/bar
        g b7 rm /jam/FOO
        g b8 sha256sum /jam/Foo /jam/bar
        g b9 sha256sum /jam/foo
        g b10 write /jam/$m200 made here
        g b11 sha256sum /jam/$m200
        g b12 exttest copy /jam copy-
        g f1 write /data/CaseT one
        g f2 write /data/caset two
        g f3 sha256sum /data/CaseT
        g f4 ls /data \| grep -ic caset
        g f5 rm /data/caset
        g_end
    } > "$out/$name.in"
    boot "$name" "$out/$name.in" "$drive"
    sums_seen "$run"
    status a3 0
    want 1 "exttest: digest /jam: 14 files, sha256 $dall" "$run"
    want 1 "^$sp  /jam/a b.txt" "$run"
    want 1 "^$ho  /jam/$o200" "$run"
    want 1 "$n255" "$run"
    want 2 "^$one  /jam/Foo" "$run"
    want 1 "^$two  /jam/foo" "$run"
    want 1 "^$three  /jam/FOO" "$run"
    want 1 "^$two  /jam/bar" "$run"
    refused b9
    status b10 0
    want 1 "^$(printf 'made here\n' | sha256sum | cut -d' ' -f1)  /jam/$m200" "$run"
    # A path is 255 bytes at most: the name of 255 bytes is listed and can't
    # be opened (sums, copy), the one of 250 can't take a prefix; the other
    # fifteen are copied.
    want 3 "a name of [0-9]+ bytes: the path is too long" "$run"
    want 1 "exttest: copy /jam: 15 made, 2 refused" "$run"
    # FAT: Foo and foo are one file, the second write's.
    want 1 "^$two  /data/CaseT" "$run"
    want 1 "^1[[:space:]]*\$" "$run"
    never "$bad" "$run"
    clean "$drive" 64 "after the run"
    vol="$out/$name-after.img"
    volume_of "$drive" 64 "$vol"
    debugfs -R "ls -p /" "$vol" 2> /dev/null | awk -F/ '{ print $6 }' | sort > "$out/$name.names"
    for f in foo FOO Bar; do
        ! grep -qxF -- "$f" "$out/$name.names" || fail "debugfs lists \"$f\", which was removed or renamed"
    done
    # Linux sees every name exactly (each made by mke2fs or by Jam OS), and a
    # copy beside each, byte for byte.
    dump="$out/$name-dump"
    rm -rf "$dump"
    mkdir -p "$dump"
    debugfs -R "rdump / $dump" "$vol" > /dev/null 2>&1
    base=$(dirname "$(find "$dump" -name Makefile | head -1)")
    { cat "$out/$name.files"; printf 'Foo\nbar\n%s\n' "$m200"; } > "$out/$name.all"
    while IFS= read -r f; do
        grep -qxF -- "$f" "$out/$name.names" || fail "debugfs doesn't list \"$f\""
        [ ${#f} -ge 250 ] && continue
        cmp -s "$base/$f" "$base/copy-$f" || fail "debugfs: \"copy-$f\" isn't a copy of \"$f\""
    done < "$out/$name.all"
    rm -rf "$vol" "$dump"
    rm -rf "$tree" "$drive"
}

# ---- big ---------------------------------------------------------------------------------

case_big() {
    drive="$out/$name-drive.img"
    rm -rf "$drive" "$out/$name-tree"
    mkdir -p "$out/$name-tree"
    printf 'big\n' > "$out/$name-tree/README"
    big=$(python3 tools/extbigsum.py big 6144)
    sparse=$(python3 tools/extbigsum.py sparse)
    make_volume "$out/$name-data.img" 8192 "$out/$name-tree" jamdata
    make_drive "$drive" "$out/$name-data.img"
    rm -rf "$out/$name-data.img" "$out/$name-tree"
    {
        echo "# generated by tools/extfs-test.sh: big, the first boot"
        g_top
        gwait=1500 g b1 exttest big /jam/big.bin 6144
        gwait=1500 g b2 exttest sparse /jam/sparse.bin
        g d1 df
        g_end
    } > "$out/$name-1.in"
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-2400} boot "$name-1" "$out/$name-1.in" "$drive"
    status b1 0
    status b2 0
    want 1 "exttest: big /jam/big.bin: wrote sha256 $big" "$run"
    want 1 "exttest: big /jam/big.bin: read sha256 $big" "$run"
    want 1 "exttest: sparse /jam/sparse.bin: wanted sha256 $sparse" "$run"
    want 1 "exttest: sparse /jam/sparse.bin: read sha256 $sparse" "$run"
    want 1 "exttest: sparse /jam/sparse.bin: PASS" "$run"
    {
        echo "# generated by tools/extfs-test.sh: big, the second boot"
        g_top
        gwait=1500 g c1 sha256sum /jam/big.bin /jam/sparse.bin
        g_end
    } > "$out/$name-2.in"
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-2400} boot "$name-2" "$out/$name-2.in" "$drive"
    want 1 "^$big  /jam/big.bin" "$run"
    want 1 "^$sparse  /jam/sparse.bin" "$run"
    never "$bad" "$run"
    vol="$out/$name-after.img"
    volume_of "$drive" 8192 "$vol"
    if e2fsck -fn "$vol" > "$out/$name-after.fsck" 2>&1; then
        echo "$name: e2fsck: clean"
    else
        fail "e2fsck doesn't call the volume clean (see $out/$name-after.fsck)"
    fi
    # Linux reads the files out of the volume: the host's hashes.
    [ "$(debugfs -R "cat /big.bin" "$vol" 2> /dev/null | sha256sum | cut -d' ' -f1)" = "$big" ] ||
        fail "debugfs reads /big.bin, and its hash isn't the host's"
    [ "$(debugfs -R "cat /sparse.bin" "$vol" 2> /dev/null | sha256sum | cut -d' ' -f1)" = "$sparse" ] ||
        fail "debugfs reads /sparse.bin, and its hash isn't the host's"
    debugfs -R "stat /sparse.bin" "$vol" > "$out/$name-sparse.stat" 2>&1
    sz=$(sed -n 's/.*Size: \([0-9]*\).*/\1/p' "$out/$name-sparse.stat" | head -1)
    [ "$sz" = $((6 * 1073741824 + 1048576 + 8192)) ] || fail "/sparse.bin is $sz bytes in debugfs's view"
    held=$(sed -n 's/.*Blockcount: \([0-9]*\).*/\1/p' "$out/$name-sparse.stat" | head -1)
    [ "${held:-0}" -lt 32768 ] || fail "/sparse.bin holds ${held:-?} units of 512 bytes: the holes aren't holes"
    rm -f "$vol" "$drive"
}

# ---- cut ---------------------------------------------------------------------------------

# find_qemu <run>: the pid of the QEMU of the run (tools/qemu-test.sh names
# its monitor socket build/.qemu-<run>.sock): the process with that on its
# command line whose working directory is this checkout's; empty if it isn't
# there (yet).
find_qemu() {
    here=$(pwd -P)
    for p in $(pgrep -f "qemu-system-x86_64 .*-monitor unix:build/.qemu-$1.sock" || true); do
        [ "$(readlink "/proc/$p/cwd" 2> /dev/null)" = "$here" ] && { echo "$p"; return 0; }
    done
    return 0
}

# nwrites <trace>: the Writes QEMU has traced so far.
nwrites() {
    n=$(grep -ac "^pci_nvme_write " "$1" 2> /dev/null || true)
    echo "${n:-0}"
}

# boot_cut <run> <script> <drive> <marker> <delta>: boot, and kill -9 QEMU
# when the trace of the drive's commands has <delta> more Writes than it had
# when the log first showed <marker>. The drive is throttled (QEMU's
# throttle: tools/qemu-test.sh's QEMU_NVME_THROTTLE): without it five Writes
# take less time than the poll between two looks at the trace. A throttle's
# bucket holds a tenth of a second's operations, so at 2 a second it holds a
# fifth of one and every Write waits its turn: the kill lands on the
# point's Write (the first two pass at once, though: a cut at the 1st lands
# at the 2nd). That is for ten; further out the exact Write matters less
# than the time it takes to get there, and the drive takes 30 a second.
# Sets `cut` to the Writes traced when the kill was sent, or empty when the
# run ended first.
boot_cut() {
    rn=$1 script=$2 drv=$3 marker=$4 delta=$5
    cut=
    thr="x-iops-write=$([ "$delta" -le 10 ] && echo 2 || echo 30)"
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-420} QEMU_INPUT="$script" QEMU_NVME="$drv" QEMU_NVME_THROTTLE="$thr" \
        QEMU_EXTRA="-trace pci_nvme_write -D $out/$rn.trace" \
        tools/qemu-test.sh "$out" "$rn" shell > "$out/$rn.out" 2>&1 &
    bpid=$!
    log="$out/$rn.log" trace="$out/$rn.trace"
    limit=$(( ${QEMU_TIMEOUT:-420} * 100 ))   # polls of at least 5 ms
    i=0 base=0 target= qp=
    while [ $i -lt $limit ] && kill -0 $bpid 2> /dev/null; do
        [ -n "$qp" ] || qp=$(find_qemu "$rn")
        if [ -z "$target" ] && grep -aqF -- "$marker" "$log" 2> /dev/null; then
            base=$(nwrites "$trace")
            target=$((base + delta))
        fi
        if [ -n "$target" ] && [ -n "$qp" ] && [ "$(nwrites "$trace")" -ge "$target" ]; then
            cut=$(nwrites "$trace")
            kill -9 "$qp"
            break
        fi
        sleep 0.005
        i=$((i + 1))
    done
    wait $bpid || true
    [ -z "$cut" ] || echo "$rn: killed (kill -9) at Write $cut of the trace (the storm began at $base)"
}

# listing <volume>: the SHA-256 and name of every file in the volume's root
# (debugfs's rdump), sorted by name.
listing() {
    d="$1.dump"
    rm -rf "$d"
    mkdir -p "$d"
    debugfs -R "rdump / $d" "$1" > /dev/null 2>&1
    (cd "$d" && find . -type f | sort | while read -r f; do echo "$(sha256sum < "$f" | cut -d' ' -f1)  $f"; done)
    rm -rf "$d"
}

# verify <log>...: the logs in boot order. Every `disktest check` line has a
# prefix of whole files at least as long as the highest "synced N" before it,
# no file missing, at most one damaged. Needs one check.
verify() {
    cat "$@" | tr -d '\r' |
        grep -aoE "synced [0-9]+|check /jam: files=[0-9]+ whole=[0-9]+ damaged=[0-9]+ missing=[0-9]+ prefix=[0-9]+" |
        awk -F'[ =]' -v c="$name" '
            /^synced / { if ($2 + 0 > s) s = $2 + 0; next }
            { n++; why = ""
              if ($12 + 0 < s) why = why " prefix " $12 " below the " s " synced"
              if ($10 + 0 != 0) why = why " " $10 " missing"
              if ($8 + 0 > 1) why = why " " $8 " damaged"
              printf "%s: check %d: %d files, prefix %d, synced %d, damaged %d%s\n", c, n, $4, $12, s, $8, (why == "" ? "" : ": FAIL:" why)
              if (why != "") bad = 1 }
            END { if (!n) { print c ": FAIL: no check line"; bad = 1 } exit bad }'
}

case_cut() {
    drive="$out/$name-drive.img"
    points=${EXTFS_CUT_POINTS:-1 2 3 4 5 6 7 8 9 10 12 14 17 20 25 30 40 50 70 100}
    rm -rf "$drive" "$out/$name-tree"
    mkdir -p "$out/$name-tree"
    printf 'hello from mke2fs\n' > "$out/$name-tree/hello.txt"
    make_volume "$out/$name-data.img" 128 "$out/$name-tree" jamdata
    make_drive "$drive" "$out/$name-data.img"
    rm -rf "$out/$name-data.img" "$out/$name-tree"
    k=0 logs= cuts=0 replays=0 linux_replays=0
    for delta in $points; do
        k=$((k + 1))
        a="$name-a$k" b="$name-b$k"
        {
            echo "# generated by tools/extfs-test.sh: cut $k (the cut $delta Writes into the storm)"
            g_top
            echo "send disktest /jam write 400 64; echo cs-\$T=\$?"
            echo "wait 300 cs-k="
            g_end
        } > "$out/$a.in"
        boot_cut "$a" "$out/$a.in" "$drive" "disktest: writing /jam" "$delta"
        [ -n "$cut" ] || echo "$name: point $k: the storm ended before $delta Writes (not cut)"
        [ -z "$cut" ] || cuts=$((cuts + 1))
        # Linux recovers a copy of the cut volume: its journal replay, then a
        # full check, which must find nothing to put right.
        L="$out/$name-cut$k.img"
        volume_of "$drive" 128 "$L"
        st=0
        e2fsck -fy "$L" > "$out/$name-cut$k.fsck" 2>&1 || st=$?
        grep -q "recovering journal" "$out/$name-cut$k.fsck" && linux_replays=$((linux_replays + 1))
        [ $st = 0 ] || fail "point $k: Linux's e2fsck -fy of the cut image exits $st (see $out/$name-cut$k.fsck)"
        listing "$L" > "$out/$name-cut$k.list"
        # Jam OS's recovery: a boot that mounts it and checks every file.
        {
            echo "# generated by tools/extfs-test.sh: cut $k, the recovery"
            g_top
            g ck$k disktest /jam check
            g_end
        } > "$out/$b.in"
        boot "$b" "$out/$b.in" "$drive"
        r=$(sed -n 's/.*the journal: \([0-9]*\) transaction(s), \([0-9]*\) block(s) replayed.*/\1 transactions, \2 blocks/p' "$run" | head -1)
        [ -z "$r" ] || replays=$((replays + 1))
        echo "$name: point $k: cut at write ${cut:-none}; Jam OS replayed ${r:-nothing}; Linux ${st} ($(grep -c 'recovering journal' "$out/$name-cut$k.fsck") replays)"
        J="$out/$name-rec$k.img"
        volume_of "$drive" 128 "$J"
        e2fsck -fn "$J" > "$out/$name-rec$k.fsck" 2>&1 || fail "point $k: e2fsck -fn after Jam OS's recovery isn't clean (see $out/$name-rec$k.fsck)"
        listing "$J" > "$out/$name-rec$k.list"
        cmp -s "$out/$name-cut$k.list" "$out/$name-rec$k.list" ||
            fail "point $k: Linux's recovery and Jam OS's left different files (diff $out/$name-cut$k.list $out/$name-rec$k.list)"
        rm -f "$L" "$J"
        logs="$logs $out/$a.log $out/$b.log"
    done
    {
        echo "# generated by tools/extfs-test.sh: cut, the last boot (no cut)"
        g_top
        g ca disktest /jam check
        gwait=300 g cb disktest /jam write 10 64
        g cc disktest /jam check
        g_end
    } > "$out/$name-last.in"
    boot "$name-last" "$out/$name-last.in" "$drive"
    logs="$logs $out/$name-last.log"
    status cb 0
    status cc 0
    # shellcheck disable=SC2086
    verify $logs || ok=0
    echo "$name: $cuts cuts of $k points; Jam OS's recovery replayed a journal after $replays, Linux's after $linux_replays"
    [ "$cuts" -ge $((k * 3 / 4)) ] || fail "only $cuts of the $k points were cut"
    clean "$drive" 128 "at the end"
    rm -f "$drive"
}

# ---- interop -----------------------------------------------------------------------------

interop_src() {
    t=$1
    mkdir -p "$t/deep/a/b/c" "$t/many" "$t/emptydir"
    printf 'small\n' > "$t/small.txt"
    head -c 4096 /dev/zero | tr '\0' 'e' > "$t/exact.bin"
    seeded "$t/big.bin" 8 33
    head -c 307200 "$t/big.bin" > "$t/m.bin"
    printf 'deepest\n' > "$t/deep/a/b/c/d.txt"
    : > "$t/empty"
    for i in $(seq 1 60); do
        printf 'n%03d: %s\n' $i "$(printf 'x%.0s' $(seq 1 $((i * 3))))" > "$t/many/n$(printf %03d $i)"
    done
}

# interop_one <variant>: a volume of that kind made by mke2fs, written by
# Jam OS, read by Linux.
interop_one() {
    v=$1
    src="$out/$name-$v-src"
    expect="$out/$name-$v-expect"
    drive="$out/$name-$v-drive.img"
    rm -rf "$src" "$expect" "$drive" "$out/$name-$v-tree"
    mkdir -p "$out/$name-$v-tree"
    printf 'made by mke2fs (%s)\n' "$v" > "$out/$name-$v-tree/seed.txt"
    interop_src "$src"
    cp -R "$src" "$expect"
    for i in $(seq 30 40); do rm -f "$expect/many/n0$i"; done
    make_volume "$out/$name-$v-data.img" 128 "$out/$name-$v-tree" "jam-$v" "$v"
    make_drive "$drive" "$out/$name-$v-data.img"
    volume_of "$drive" 128 "$out/$name-$v-before.img"
    dumpe2fs -h "$out/$name-$v-before.img" 2> /dev/null | sed -n 's/^Filesystem features: *//p' | tr ' ' '\n' | sort > "$out/$name-$v.features-before"
    rm -rf "$out/$name-$v-data.img" "$out/$name-$v-tree" "$out/$name-$v-before.img"
    # The stick's /data holds the source tree.
    bootimg="$out/$name-$v-boot.img"
    cp build/jamos.img "$bootimg"
    at=$(python3 tools/mkimage.py --data-at build/jamos.img)
    mmd -i "$bootimg@@$at" ::/src
    mcopy -s -i "$bootimg@@$at" "$src"/* ::/src/
    {
        echo "# generated by tools/extfs-test.sh: interop $v"
        g_top
        g m1 mkdir -p /jam/t/deep/a/b/c /jam/t/many /jam/t/emptydir
        for f in small.txt exact.bin m.bin big.bin empty deep/a/b/c/d.txt; do
            gwait=300 g "cp-$(echo "$f" | tr -c 'a-z0-9\n' '-')" cp /data/src/$f /jam/t/$f
        done
        for i in $(seq 1 60); do
            f=many/n$(printf %03d $i)
            g "cm$i" cp /data/src/$f /jam/t/$f
        done
        for i in $(seq 30 40); do g "rm$i" rm /jam/t/many/n0$i; done
        g m2 cp /jam/t/big.bin /jam/t/big2.bin
        g m3 rm /jam/t/big2.bin
        g m4 cp /jam/t/small.txt /jam/t/small.txt.bak
        g m5 mv /jam/t/small.txt.bak /jam/t/deep/small.moved
        g m6 rm /jam/t/deep/small.moved
        g df1 df
        g_end
    } > "$out/$name-$v.in"
    boot "$name-$v" "$out/$name-$v.in" "$drive"
    bootimg=
    never "$bad" "$run"
    want 1 "extfs /jam: mounted, 128 MiB" "$run"
    clean "$drive" 128 "$v: after the run"
    vol="$out/$name-$v-after.img"
    volume_of "$drive" 128 "$vol"
    # debugfs reads the tree, byte for byte.
    dump="$out/$name-$v-dump"
    rm -rf "$dump"
    mkdir -p "$dump"
    debugfs -R "rdump /t $dump" "$vol" > /dev/null 2>&1
    if diff -r "$expect" "$dump/t" > "$out/$name-$v.diff" 2>&1; then
        echo "$name: $v: debugfs's rdump of /t is the tree Jam OS was given"
    else
        fail "$v: debugfs's rdump of /t differs from the tree (see $out/$name-$v.diff)"
    fi
    dumpe2fs -h "$vol" 2> /dev/null | sed -n 's/^Filesystem features: *//p' | tr ' ' '\n' | sort > "$out/$name-$v.features-after"
    # Jam OS leaves the volume's features as they were (it sets needs_recovery
    # while it is mounted, as Linux does).
    grep -v needs_recovery "$out/$name-$v.features-before" > "$out/$name-$v.fb"
    grep -v needs_recovery "$out/$name-$v.features-after" > "$out/$name-$v.fa"
    cmp -s "$out/$name-$v.fb" "$out/$name-$v.fa" ||
        fail "$v: the volume's features changed (see $out/$name-$v.features-before and -after)"
    rm -rf "$src" "$expect" "$dump" "$vol" "$drive" "$out/$name-$v-boot.img"
}

case_interop() {
    for v in ${EXTFS_INTEROP_VARIANTS:-jam4k linux1k plain2k}; do
        interop_one "$v"
    done
}

# ---- foreign -----------------------------------------------------------------------------

# A Linux GPT drive of three partitions, 64 MiB each, from the volumes
# <file1>, <file2> and <file3>.
linux_drive() {
    python3 tools/mkgpt.py "$1" --layout none --size 240M \
        --part 0FC63DAF-8483-4772-8E79-3D69D8477DE4:2048:131072:dirtyj \
        --part 0FC63DAF-8483-4772-8E79-3D69D8477DE4:133120:131072:bigalloc \
        --part 0FC63DAF-8483-4772-8E79-3D69D8477DE4:264192:131072:ext3p \
        --fill "1=$2" --fill "2=$3" --fill "3=$4" > /dev/null
}

case_foreign() {
    tree="$out/$name-tree"
    rm -rf "$tree"
    mkdir -p "$tree/dir"
    printf 'hello from Linux\n' > "$tree/hello.txt"
    seeded "$tree/dir/a.bin" 1 41
    printf 'upper\n' > "$tree/Makefile"
    printf 'lower\n' > "$tree/makefile"
    ln -s hello.txt "$tree/lnk"
    # 1. a USB stick: Linux's default ext4 (4 KiB blocks), one type-83 partition.
    stick="$out/$name-stick.img"
    python3 tools/mkstick.py "$stick" 96 83 > /dev/null
    make_volume "$out/$name-a.img" 80 "$tree" linux-usb linux4k
    dd if="$out/$name-a.img" of="$stick" bs=1M seek=1 conv=notrunc,sparse status=none
    # 2. a Linux GPT drive. Its first volume was cut in the middle of the
    # journal's life: a transaction that rewrites hello.txt's block is
    # committed in the journal and not replayed (Jam OS mounts it read-only
    # and reads what is home).
    mkdir -p "$out/$name-tree2"
    printf 'old state\n' > "$out/$name-tree2/hello.txt"
    make_volume "$out/$name-b.img" 64 "$out/$name-tree2" linux-dirty linux4k
    blk=$(debugfs -R "bmap /hello.txt 0" "$out/$name-b.img" 2> /dev/null | tail -1)
    python3 -c "import sys; sys.stdout.buffer.write(b'new state (from the journal)\n'.ljust(4096, b'\0'))" > "$out/$name-new.blk"
    printf 'jo\njw -b %s %s\njc\n' "$blk" "$out/$name-new.blk" > "$out/$name-jw.cmds"
    debugfs -w -f "$out/$name-jw.cmds" "$out/$name-b.img" > "$out/$name-jw.out" 2>&1 ||
        { cat "$out/$name-jw.out"; echo "$name: debugfs can't write a journal transaction"; exit 1; }
    dumpe2fs -h "$out/$name-b.img" 2> /dev/null | grep -q "needs_recovery" ||
        { echo "$name: the dirty volume doesn't need recovery"; exit 1; }
    # Its second: a feature Jam OS doesn't know (bigalloc).
    truncate -s 64M "$out/$name-c.img"
    MKE2FS_CONFIG=/dev/null mke2fs -q -F -o Linux -b 4096 -m 0 -O none,has_journal,extent,bigalloc,sparse_super,large_file,filetype \
        -C 16384 -J size=4 -L linux-bigalloc "$out/$name-c.img" > "$out/$name-c.mke2fs" 2>&1 ||
        { cat "$out/$name-c.mke2fs"; echo "$name: mke2fs -O bigalloc failed"; exit 1; }
    # Its third: ext3's, with block maps (a 3 MiB file reaches its double
    # indirect block) and uninit_bg: read, never written.
    mkdir -p "$out/$name-tree5"
    seeded "$out/$name-tree5/ind.bin" 3 43
    printf 'ext3\n' > "$out/$name-tree5/hello.txt"
    make_volume "$out/$name-f.img" 64 "$out/$name-tree5" linux-ext3 old2k
    ind=$(sha "$out/$name-tree5/ind.bin")
    ldrive="$out/$name-linux.img"
    linux_drive "$ldrive" "$out/$name-b.img" "$out/$name-c.img" "$out/$name-f.img"
    # 3. two Jam OS drives with the same files: the first one's data is /jam
    # (writable), the other's another disk's, read-only. Which is which
    # depends on the order the drivers answer in.
    mkdir -p "$out/$name-tree3"
    printf 'mine\n' > "$out/$name-tree3/mine.txt"
    printf 'the second drive\n' > "$out/$name-tree3/second.txt"
    make_volume "$out/$name-d.img" 64 "$out/$name-tree3" jamdata
    j1="$out/$name-j1.img" j2="$out/$name-j2.img"
    make_drive "$j1" "$out/$name-d.img"
    make_volume "$out/$name-g.img" 64 "$out/$name-tree3" jamdata   # another volume: its own UUID
    make_drive "$j2" "$out/$name-g.img"
    rm -rf "$out/$name"-?.img "$out/$name-tree2" "$out/$name-tree3" "$out/$name-tree5"
    before=$(sha256sum "$stick" "$ldrive" "$j1" "$j2" | cut -d' ' -f1 | tr '\n' ' ')
    guest=/usb0 sums="$out/$name.sums"
    : > "$sums"
    sums_add "$tree" hello.txt dir/a.bin lnk Makefile makefile
    old=$(printf 'old state\n' | sha256sum | cut -d' ' -f1)
    second=$(printf 'the second drive\n' | sha256sum | cut -d' ' -f1)
    mine=$(printf 'mine\n' | sha256sum | cut -d' ' -f1)
    usb="-blockdev driver=file,node-name=aimgf,filename=$stick -blockdev driver=raw,node-name=aimg,file=aimgf"
    {
        echo "# generated by tools/extfs-test.sh: foreign"
        g_top
        echo "monitor device_add usb-storage,id=sa,bus=xhci.0,port=2,drive=aimg"
        echo "wait 60 init: /usb0 mounted"
        echo "seen 60 re:volumes: nvme[0-9]+ partition 3 is /nvme[0-9]+, read-only"
        echo "send sleep 2"
        echo "wait {prompt}"
        echo "send disks"
        echo "seen re:dirtyj +/nvme(?P<dj>[0-9]+) \\(ext\\)"
        echo "seen re:ext3p +/nvme(?P<e3>[0-9]+) \\(ext\\)"
        echo "seen re:Jam OS data +[0-9.]+ [A-Za-z]+ +Jam OS data +/nvme(?P<jx>[0-9]+) \\(ext\\)"
        echo "wait {prompt}"
        g u1 sha256sum /usb0/hello.txt /usb0/dir/a.bin /usb0/lnk /usb0/Makefile /usb0/makefile
        g u2 ls /usb0
        g u3 write /usb0/new.txt x
        g u4 rm /usb0/hello.txt
        g u5 mkdir /usb0/newdir
        g u6 mv /usb0/hello.txt /usb0/h2.txt
        g u7 mount -w /usb0
        g u8 write /usb0/new.txt x
        g d1 sha256sum /nvme{dj}/hello.txt
        g d2 write /nvme{dj}/new.txt x
        g d3 mount -w /nvme{dj}
        g e1 sha256sum /nvme{e3}/ind.bin /nvme{e3}/hello.txt
        g e2 write /nvme{e3}/new.txt x
        g e3 mount -w /nvme{e3}
        g j1 sha256sum /nvme{jx}/second.txt /jam/second.txt
        g j2 write /nvme{jx}/new.txt x
        g j3 mount -w /nvme{jx}
        g p1 write /jam/mine2.txt ok
        g p2 sha256sum /jam/mine.txt /jam/mine2.txt
        g df1 df
        g mnt mount
        g_end
    } > "$out/$name.in"
    boot "$name" "$out/$name.in" "$j1" "$j2" "$ldrive"
    usb=
    sums_seen "$run"
    for t in u3 u4 u5 u6 u7 u8 d2 d3 e2 e3 j2 j3; do
        refused $t
    done
    want 1 "^$old  /nvme[0-9]+/hello.txt" "$run"
    want 1 "^$second  /nvme[0-9]+/second.txt" "$run"
    want 1 "^$second  /jam/second.txt" "$run"
    want 1 "^$mine  /jam/mine.txt" "$run"
    want 1 "^$ind  /nvme[0-9]+/ind.bin" "$run"
    want 1 "bigalloc +\\(no ext volume\\)" "$run"
    never "MISMATCH|DIFFERENT" "$run"
    # Someone else's volumes: every byte as it was. Of the two Jam OS
    # drives, the one that was /jam changed and the other didn't.
    after=$(sha256sum "$stick" "$ldrive" | cut -d' ' -f1 | tr '\n' ' ')
    [ "$after" = "$(echo "$before" | cut -d' ' -f1-2) " ] ||
        fail "an image of someone else's volume changed: Jam OS wrote to it"
    changed=0
    for d in "$j1" "$j2"; do
        a=$(sha256sum "$d" | cut -d' ' -f1)
        case $before in *"$a"*) ;; *) changed=$((changed + 1)); mine_drive=$d ;; esac
        clean "$d" 64 "of $(basename "$d") after the run"
    done
    [ $changed = 1 ] || fail "$changed of the two Jam OS drives changed, wanted 1 (the one at /jam)"
    if [ $changed = 1 ]; then
        vol="$out/$name-jam.img"
        volume_of "$mine_drive" 64 "$vol"
        [ "$(debugfs -R "cat /mine2.txt" "$vol" 2> /dev/null)" = ok ] || fail "/jam's mine2.txt isn't what Jam OS wrote"
        rm -f "$vol"
    fi
    rm -rf "$stick" "$ldrive" "$j1" "$j2" "$tree" "$out/$name-new.blk"
}

ext_tools
case $c in
basic) case_basic ;;
links) case_links ;;
case) case_case ;;
big) case_big ;;
cut) case_cut ;;
storm) case_storm ;;
interop) case_interop ;;
foreign) case_foreign ;;
*) echo "$name: no case called $c"; exit 2 ;;
esac
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL"
exit 1
