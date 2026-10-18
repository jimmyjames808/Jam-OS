#!/bin/sh
# POSIX mapped files and programs on /data (docs/M13-PLAN.md section 12 and
# "F1b notes"): posixtest's mmap sections from the shell. The cases named
# in MMAP_CASES (default: all of them, one after the other; give each its
# own output directory to run them side by side), each a shell boot
# (tools/shell-tests/posix-mmap-<case>.txt) and a look at its log:
#   mmap      posixtest's mmap section from the shell (/data has a pager
#             there): shared and private mappings on the file's pages,
#             mprotect adding write, msync, 4 MiB read without a copy.
#   big       a 100 MiB file on a second stick (made here, its 64-bit
#             words' sum computed here) mapped shared and private by
#             `posixtest mmap-big`: the sum right both times, and the job's
#             pages not the file's (no copy: a copy would add 25,600).
#   pull      `posixtest --child-mmap-pull` maps a file on the second stick
#             and touches every page; the stick is pulled (QEMU's monitor
#             device_del); then its copy from the mapping through a pipe is
#             EIO and it lives, and its touch of the mapping ends it (the
#             kernel's line: the file's pager is gone, or couldn't read
#             it); the shell and /data go on.
#   coherent  `posixtest mmap-coherent`: a write through the file seen by
#             a shared mapping and by read; a private mapping's touched page
#             keeps its bytes, an untouched one follows the file. Needs
#             M12.5's PF2 (fat keeps mappings coherent with writes and maps
#             a file open for writing): FAILS without it.
#   wshared   `posixtest mmap-wshared`: writable shared mappings (docs/
#             WRITEMAP-PLAN.md section 3). On an ext /data: a store seen by
#             read, pread, a second mapping and a child; write and stores
#             on one page both kept; mprotect adding write on an O_RDWR
#             mapping (EACCES on O_RDONLY); truncate's tail and a write's
#             gap zero; msync MS_ASYNC and MS_SYNC return 0 (MS_SYNC over
#             stored pages waits for extfs's write-back: WE2's; until then
#             it ends EIO after the pager's answer time). A second boot,
#             on a twin of the image with a FAT /data (tools/datafs.sh):
#             the same section there (ENODEV with PROT_WRITE, EACCES on a
#             read-only descriptor, a read-only mapping of an O_RDWR one)
#             and the mmap and mmap-coherent sections as they were.
#   spawn     a copy of posixtest on /data: posix_spawn of it is EACCES
#             while it isn't allowed; then `allow`ed, it runs (init's
#             /svc/exec seals and starts it) and exits 7; a forked child's
#             execve of it gets our pipe and our SIGTERM (forwarded): exit 9.
#             Then `posixtest spawn-wmap` (WRITEMAP-PLAN's WT1): with a
#             writable shared mapping of it alive (an O_RDWR descriptor's,
#             open and then closed) a spawn and a forked child's execve are
#             ETXTBSY, and once it is unmapped the program runs again.
# The image's /data is ext (extfs serves it, its pager behind every mapping
# there, docs/M13.6-PLAN.md's U10); the second sticks are FAT. MMAP_DATA=ext
# makes it an ext volume Linux's mke2fs made instead (tools/ext-stick.sh), which
# needs e2fsprogs and mtools (the test PC's WSL).
# QEMU_SMP and QEMU_MEM pass through.
# Usage: tools/posix-mmap-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-posix-mmap}
cases=${MMAP_CASES:-mmap big pull coherent wshared spawn}
mkdir -p "$out"
ok=1
image=${QEMU_IMAGE:-build/jamos.img}
if [ "${MMAP_DATA:-fat}" = ext ]; then
    image="$out/$name-ext.img"
    cp build/jamos.img "$image"
    tools/ext-stick.sh "$image" > "$out/$name-ext.out" 2>&1 ||
        { echo "$name: the ext stick couldn't be made (see $out/$name-ext.out)"; exit 1; }
fi

count() { grep -acE -- "$1" "$2" || true; }
want() {   # want <case> <how many times exactly> <text (a regex)> <log>
    n=$(count "$3" "$4")
    [ "$n" -eq "$2" ] || { echo "$name: $1: \"$3\" $n times in $(basename "$4"), wanted $2"; ok=0; }
}
never() {   # never <case> <text> <log>
    n=$(count "$2" "$3")
    [ "$n" -eq 0 ] || { echo "$name: $1: \"$2\" $n times in $(basename "$3")"; ok=0; }
}

# make_stick <stick> <MiB> <file name> <file MiB> <seed>: a FAT32 stick
# (one partition, type 0c) holding that many MiB of seeded bytes; the
# 64-bit little-endian words' sum (mod 2^64, hex) into <stick>.sum.
make_stick() {
    python3 tools/mkstick.py "$1" "$2" 0c
    mformat -i "$1@@1M" -T $((($2 - 1) * 2048)) -F -v MMAP-A ::
    python3 - "$1.data" "$4" "$5" "$1.sum" <<'EOF'
import random, sys
from array import array
path, mib, seed, sum_path = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
data = random.Random(seed).randbytes(mib << 20)
open(path, "wb").write(data)
words = array("Q")
words.frombytes(data)
if sys.byteorder != "little":
    words.byteswap()
open(sum_path, "w").write("%016x\n" % (sum(words) % (1 << 64)))
EOF
    mcopy -i "$1@@1M" "$1.data" "::/$3"
    rm -f "$1.data"
}

# run_case <case> <script> [stick]: the shell script (with the stick as the
# drive aimg); its log up to the `reboot -f` at the end into $run.
run_case() {
    usb=""
    [ -z "${3:-}" ] || usb="-drive if=none,id=aimg,format=raw,file=$3"
    if ! QEMU_TIMEOUT=${QEMU_TIMEOUT:-600} QEMU_INPUT="$2" QEMU_USB="$usb" QEMU_IMAGE="$image" \
         tools/qemu-test.sh "$out" "$name-$1" shell > "$out/$name-$1.out" 2>&1; then
        echo "$name: $1: the shell script FAILED (see $out/$name-$1.log)"
        grep "serial-feed: .*no '" "$out/$name-$1.out" || true
        ok=0
    fi
    run="$out/$name-$1.run.log"
    sed '/jam[^>]*>.* reboot -f/q' "$out/$name-$1.log" > "$run"
    # On an ext /data (the default), extfs served it, with its pager.
    [ "$(python3 tools/mkimage.py --data-fs "$image")" != ext ] || want "$1" 1 "volumes: extfs-data: pager made: " "$run"
}

# Nothing went wrong with a page or a program on the way.
no_errors() {
    never "$1" "posixtest: FAILED|MISMATCH|doesn't know|can't detach|can't fail" "$run"
    never "$1" "did not end cleanly|left [0-9]+ units of job kind" "$run"
}

case_mmap() {
    run_case mmap tools/shell-tests/posix-mmap.txt
    want mmap 1 "posixtest: mmap: ok" "$run"
    want mmap 1 "posixtest: mmap: 4 MiB mapped and read \(the file's pages\)" "$run"
    never mmap "has no pager here" "$run"
    never mmap "killed: page fault" "$run"
    no_errors mmap
}

case_big() {
    stick="$out/$name-big-a.img"
    make_stick "$stick" 160 big.bin 100 21
    sum=$(cat "$stick.sum")
    sed "s/@SUM@/$sum/" tools/shell-tests/posix-mmap-big.txt > "$out/$name-big.txt"
    run_case big "$out/$name-big.txt" "$stick"
    want big 1 "posixtest: mmap-big: /usb0/big.bin shared: 104857600 bytes, sum $sum \(right\)" "$run"
    want big 1 "posixtest: mmap-big: /usb0/big.bin private: 104857600 bytes, sum $sum \(right\)" "$run"
    want big 1 "posixtest: mmap-big: ok" "$run"
    grep -a "posixtest: mmap-big: /usb0" "$run" || true   # the pages and the times, for the report
    no_errors big
    rm -f "$stick" "$stick".*
}

case_pull() {
    stick="$out/$name-pull-a.img"
    make_stick "$stick" 64 pull.bin 2 22
    run_case pull tools/shell-tests/posix-mmap-pull.txt "$stick"
    want pull 1 "posixtest: mmap-pull: mapped /usb0/pull.bin \(2097152 bytes" "$run"
    want pull 1 "posixtest: mmap-pull: a copy from the mapping: EIO" "$run"
    want pull 1 "process \"posixtest\" killed: page fault: the file's pager (is gone|couldn't read it)" "$run"
    want pull 1 "^pull-p=137" "$run"   # the shell's status for a killed program
    want pull 1 "^ls-p=0" "$run"
    never pull "touched: byte" "$run"
    never pull "did not end cleanly|left [0-9]+ units of job kind" "$run"
    rm -f "$stick" "$stick".*
}

case_coherent() {
    run_case coherent tools/shell-tests/posix-mmap-coherent.txt
    want coherent 1 "posixtest: mmap-coherent: ok" "$run"
    want coherent 1 "^coh-c=0" "$run"
    never coherent "mmap of a file open for writing" "$run"
    no_errors coherent
}

case_wshared() {
    fat_image=$image
    if [ "$(python3 tools/mkimage.py --data-fs "$image")" = ext ]; then
        run_case wshared tools/shell-tests/posix-mmap-wshared.txt
        want wshared 1 "posixtest: mmap-wshared: /data is ext" "$run"
        want wshared 1 "posixtest: mmap-wshared: a store seen by pread, read, a second mapping and a child: ok" "$run"
        want wshared 1 "posixtest: mmap-wshared: write and stores on one page both kept; growth by write: ok" "$run"
        want wshared 1 "posixtest: mmap-wshared: mprotect adding write: ok" "$run"
        want wshared 1 "posixtest: mmap-wshared: truncate's tail and a write's gap are zero: ok" "$run"
        want wshared 1 "posixtest: mmap-wshared: msync where nothing is dirty: ok" "$run"
        want wshared 1 "posixtest: mmap-wshared: msync: ok" "$run"
        want wshared 1 "posixtest: mmap-wshared: ok" "$run"
        want wshared 1 "^ws-w=0" "$run"
        grep -a "posixtest: mmap-wshared: msync MS_SYNC" "$run" || true   # its time, for the report
        never wshared "killed: page fault" "$run"
        no_errors wshared
        # The second boot: a twin of the image with a FAT /data.
        . tools/datafs.sh
        fat_image="$out/$name-wshared-fat.img"
        data_fat_image "$fat_image" ||
            { echo "$name: wshared: no FAT boot image"; ok=0; return; }
    fi
    saved=$image
    image=$fat_image
    run_case wshfat tools/shell-tests/posix-mmap-wshared-fat.txt
    image=$saved
    want wshfat 1 "posixtest: mmap-wshared: /data is not ext" "$run"
    want wshfat 1 "posixtest: mmap-wshared: fat: ENODEV, EACCES, a read-only mapping: ok" "$run"
    want wshfat 1 "posixtest: mmap: ok" "$run"
    want wshfat 1 "posixtest: mmap-coherent: ok" "$run"
    want wshfat 1 "posixtest: mmap-wshared: ok" "$run"
    want wshfat 1 "^wf-f=0" "$run"
    never wshfat "killed: page fault" "$run"
    no_errors wshfat
    [ "$fat_image" = "$image" ] || rm -f "$fat_image"
}

case_spawn() {
    run_case spawn tools/shell-tests/posix-mmap-spawn.txt
    # The refused run: EACCES, its section ok, posixtest's status 0.
    n=$(count "posixtest: spawn-data: /data/pt-prog: Permission denied" "$run")
    [ "$n" -ge 1 ] || { echo "$name: spawn: no EACCES for the program not allowed"; ok=0; }
    want spawn 1 "^r-s=0" "$run"
    want spawn 1 "allow: /data/pt-prog may run" "$run"
    # The allowed run: sealed and started by init's /svc/exec, it exits 7.
    want spawn 1 "posixtest: spawn-data: /data/pt-prog: started, exited 7" "$run"
    # A forked child's execve of it: our SIGTERM forwarded, its exit 9 the child's.
    want spawn 1 "posixtest: spawn-data: /data/pt-prog fork-exec: wait status 0x900" "$run"
    # Its descriptors, asked of the waiting child, are the program's (PR).
    want spawn 1 "posixtest: spawn-data: /data/pt-prog fork-exec fds: PASS" "$run"
    want spawn 1 "^a-s=0" "$run"
    # With a writable shared mapping of the file alive it is ETXTBSY, to a
    # spawn (the descriptor open, then closed) and a forked child's execve;
    # unmapped, it runs again.
    want spawn 1 "posixtest: spawn-wmap: /data/pt-prog mapped, the descriptor open: spawn Text file busy" "$run"
    want spawn 1 "posixtest: spawn-wmap: a forked child's execve: wait status 0x6100" "$run"
    want spawn 1 "posixtest: spawn-wmap: the descriptor closed, the mapping alive: spawn Text file busy" "$run"
    want spawn 1 "posixtest: spawn-wmap: unmapped: spawn ran \(code 7\) after" "$run"
    want spawn 1 "^wm-s=0" "$run"
    never spawn "init: exec: .* didn't start" "$run"
    no_errors spawn
}

for c in $cases; do
    case $c in
    mmap) case_mmap ;;
    big) case_big ;;
    pull) case_pull ;;
    coherent) case_coherent ;;
    wshared) case_wshared ;;
    spawn) case_spawn ;;
    *) echo "$name: no case called $c"; ok=0 ;;
    esac
done
[ "${MMAP_DATA:-fat}" != ext ] || rm -f "$image"
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL"
exit 1
