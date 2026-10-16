#!/bin/sh
# Mapped files and the pagers behind them (docs/M12.5-PLAN.md). The cases
# named in PAGER_CASES (default: all of them, one after the other; give each
# its own output directory to run them side by side), each a shell boot
# (tools/shell-tests/pager-<case>.txt) and a look at its log:
#   plumb  what volumes and libos do: one pager a mount (made with the
#          mount, the same one across fat-data's restart and the spare's
#          promotion, closed with the stick), the budget a sixteenth of
#          the memory, `pagertest self` over plain VMOs, and file.map
#          answered by fat through the kept-channel machinery, also after
#          fat-data and fat-usb0 were killed.
#   map    a file on the second stick and its copy on /data, hashed
#          through a mapping twice (the second pass asks fat for nothing:
#          its "gone" line counts the requests) and through a VMO of our
#          own (FILE_MAP_EXEC, read with vmo_read): every hash the host's
#          sha256sum; a touch on a cue asks for its 64 KiB alone.
#   budget a file half again as big as the mount's budget (QEMU_MEM 512M:
#          a budget of 32 MiB, a 48 MiB file), hashed through a mapping
#          twice: right both times, and the second pass asks for every
#          page again (the first's were evicted, oldest first).
#   kill   that file's pages, twice, while fat-usb0 is killed again and
#          again: the successors take the requests the dead ones left
#          (PAGER_NEXT_AGAIN), the mapper sees the right bytes and no page
#          fails.
#   exec   programs on /data run from their pages, in two boots of one
#          stick (pager-exec.txt, then pager-exec2.txt; their headers say
#          every step): boot 1 allows copies of bin/pxtest (one page table,
#          named by its hash, for them all), runs one touching none of its
#          table (fat's "gone" line: at most half its pages fetched), two
#          at once (one VMO: one "gone" line, no page fetched twice), one
#          whose table was removed (the whole read once, which writes it
#          again); then this script changes a byte of three copies on the
#          image, as the Mac would (px's table, py's data segment, pz's ELF
#          header page) and boot 2 runs them: px dies at the changed page,
#          py and pz are refused, the shell lives; pq dies when the stick is
#          pulled under it, and runs again from the replugged one.
#   write  files that change under their mappings: pagertest's own
#          writes and truncates of every kind, on /data and the stick,
#          each seen through the mapping; the shell's `write` emptying a
#          mapped file (a copy past the new end refused, a load there a
#          bus error); `rm` of a mapped file (cut off: a copy ERR_IO, a
#          load killed "the file's pager is gone").
#   pull   the stick pulled under a mapping (QEMU's device_del): a copy
#          from a page read before and one never read both fail ERR_IO in
#          under a second, a load is killed "the file's pager is gone";
#          /data and the shell still answer; plugged back, the file maps
#          and hashes right again.
# extfs as the pager (docs/M13.6-PLAN.md's U10; these need e2fsprogs and
# mtools: the test PC's WSL):
#   jam    map and write on /jam (pager-jam.txt): a Jam OS NVMe drive whose
#          data partition is an ext volume Linux's mke2fs made holding
#          pager.bin (as tools/nvme-test.sh's jam case makes it), mounted
#          by extfs at /jam with its pager: pager.bin hashed through a
#          mapping twice (the second pass asks for nothing) and through a
#          VMO of our own, every hash the host's; pagertest's writes and
#          truncates; the shell's `write` emptying a mapped file (a copy
#          past the new end refused, a load a bus error); `rm` of a mapped
#          file: on ext its mapping reads on, by a copy and by a load (the
#          inode kept until the mapping goes). Then on the host Linux's
#          e2fsck -fn calls the volume clean: no orphan left behind.
#   jstorm storm with a mapper (pager-jstorm.txt): on such a drive, a file
#          half again as big as the budget hashed through a mapping six
#          times (every pass asks extfs for every page) while `storm`
#          copies another file on /jam and kills extfs-jam twice a
#          second: the copy MATCHes, the hashes are the host's, the
#          restarts carry on (none fresh), no page fails; e2fsck clean.
#   xexec  the exec case (both boots, the same steps and checks) with the
#          stick's /data an ext volume: the image's FAT data partition made
#          again by mke2fs from its files, so extfs serves /data; between
#          the boots the three bytes are changed in the files' blocks on
#          the image (debugfs's bmap finds them). e2fsck -fn after each.
# QEMU_SMP and QEMU_MEM pass through (budget sets its own QEMU_MEM).
# Usage: tools/pager-test.sh <outdir> [name]; exit 0 on PASS.
set -eu
out=$1 name=${2:-pager}
cases=${PAGER_CASES:-plumb map budget kill exec write pull jam jstorm xexec}
mkdir -p "$out"
ok=1

count() { grep -acE -- "$1" "$2" || true; }
# want <case> <how many times exactly> <text (a regex)> <log>
want() {
    n=$(count "$3" "$4")
    [ "$n" -eq "$2" ] || { echo "$name: $1: \"$3\" $n times in $(basename "$4"), wanted $2"; ok=0; }
}
never() {   # never <case> <text> <log>
    n=$(count "$2" "$3")
    [ "$n" -eq 0 ] || { echo "$name: $1: \"$2\" $n times in $(basename "$3")"; ok=0; }
}

# put_file <stick> <name> <MiB> <seed>: that many MiB of the same bytes
# every time, as ::/<name>; their SHA-256 into <stick>.<name>.sha.
put_file() {
    python3 -c "import random, sys; sys.stdout.buffer.write(random.Random($4).randbytes($3 << 20))" \
        > "$1.$2"
    sha256sum "$1.$2" | cut -d' ' -f1 > "$1.$2.sha"
    mcopy -i "$1@@1M" "$1.$2" "::/$2"
    rm -f "$1.$2"
}

# make_stick <stick> [MiB]: a 64 MiB stick with one FAT32 partition (type
# 0c), as sticks-test.sh makes its sticks, holding pager.bin (2 MiB) and,
# with MiB, big.bin of that many.
make_stick() {
    python3 tools/mkstick.py "$1" 64 0c
    mformat -i "$1@@1M" -T $((63 * 2048)) -F -v PAGER-A ::
    put_file "$1" pager.bin 2 11
    [ -z "${2:-}" ] || put_file "$1" big.bin "$2" 12
}

# run_case <case> <stick>: the shell script pager-<case>.txt with the stick
# as the drive aimg; its log up to the `reboot -f` at the end (the shutdown
# closes everything) into $run.
run_case() {
    if ! QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT="tools/shell-tests/pager-$1.txt" \
         QEMU_USB="-blockdev driver=file,node-name=aimgf,filename=$2 -blockdev driver=raw,node-name=aimg,file=aimgf" \
         tools/qemu-test.sh "$out" "$name-$1" shell > "$out/$name-$1.out" 2>&1; then
        echo "$name: $1: the shell script FAILED (see $out/$name-$1.log)"
        grep "serial-feed: .*no '" "$out/$name-$1.out" || true
        ok=0
    fi
    run="$out/$name-$1.run.log"
    sed '/jam[^>]*>.* reboot -f/q' "$out/$name-$1.log" > "$run"
}

# hashed <case> <path> <sha> <how many>: pagertest's result lines for path,
# each a match with the host's hash (through the mapping or our own VMO,
# and through file_read alike).
hashed() {
    lines=$(grep -a "pagertest: $2: [0-9]* bytes: " "$run" || true)
    n=$(printf '%s\n' "$lines" | grep -c "sha256 $3: read the same" || true)
    [ "$n" -eq "$4" ] ||
        { echo "$name: $1: $n of $4 hashes of $2 are the host's ($3)"; echo "$lines"; ok=0; }
}

# Nothing went wrong with a page, in fat or in a mapper.
no_page_errors() {
    never "$1" "MISMATCH|page fault|doesn't know|can't detach|can't read|can't fail" "$run"
    never "$1" "pages not supplied|can't wait on the pager|don't match the approved" "$run"
    never "$1" "did not end cleanly|left [0-9]+ units of job kind" "$run"
}

case_plumb() {
    stick="$out/$name-plumb-a.img"
    make_stick "$stick"
    run_case plumb "$stick"
    # One pager a mount, made with its first instance and kept: fat-data was
    # killed once (a spare promoted) and fat-usb0 once, and neither made
    # another.
    want plumb 1 "volumes: fat-data: pager made: " "$run"
    want plumb 1 "volumes: fat-esp: pager made: " "$run"
    want plumb 1 "volumes: fat-usb0: pager made: " "$run"
    want plumb 1 "volumes: fat-usb0: pager closed" "$run"
    never plumb "starts without a pager" "$run"
    never plumb "no pager power" "$run"
    never plumb "fat-data: pager closed" "$run"
    # The budget is a sixteenth of the memory (QEMU_MEM, 2G by default).
    pages=$(sed -n 's/.*volumes: fat-data: pager made: \([0-9]*\) pages.*/\1/p' "$run" | head -1)
    memarg=${QEMU_MEM:-2G}
    case $memarg in
    *G) mib=$(( ${memarg%G} * 1024 )) ;;
    *M) mib=${memarg%M} ;;
    *) mib=2048 ;;
    esac
    mem=$((mib * 256))   # pages in the machine, about
    if [ -z "$pages" ] || [ "$pages" -lt $((mem / 20)) ] || [ "$pages" -gt $((mem / 14)) ]; then
        echo "$name: plumb: the budget is ${pages:-none} pages, wanted about $((mem / 16))"
        ok=0
    fi
    want plumb 3 "answers within 60 s" "$run"
    hashed plumb /data/pager.txt "$(printf 'a file to map\n' | sha256sum | cut -d' ' -f1)" 1
    no_page_errors plumb
    rm -f "$stick" "$stick".*
}

case_map() {
    stick="$out/$name-map-a.img"
    make_stick "$stick"
    run_case map "$stick"
    sha=$(cat "$stick.pager.bin.sha")
    hashed map /usb0/pager.bin "$sha" 2
    hashed map /data/pager.bin "$sha" 2
    want map 1 "^$sha  */usb0/pager.bin" "$run"
    # Each mapping hashed twice asked for each 64 KiB once, the second pass
    # for nothing (both passes right); each VMO of our own the same once.
    want map 2 "pagertest: pass [12] of /usb0/pager.bin: 2097152 bytes at 0 in [0-9]+ ms: sha256 $sha" "$run"
    want map 2 "pagertest: pass [12] of /data/pager.bin: 2097152 bytes at 0 in [0-9]+ ms: sha256 $sha" "$run"
    want map 2 "fat /usb0: mapped file [0-9a-f]+ gone: 32 page requests, 512 pages supplied" "$run"
    want map 2 "fat /data: mapped file [0-9a-f]+ gone: 32 page requests, 512 pages supplied" "$run"
    # The touch at 4096 asked for the 64 KiB from there.
    want map 1 "fat /data: mapped file [0-9a-f]+ gone: 1 page requests, 16 pages supplied" "$run"
    no_page_errors map
    rm -f "$stick" "$stick".*
}

case_budget() {
    stick="$out/$name-budget-a.img"
    make_stick "$stick" 48
    QEMU_MEM=512M run_case budget "$stick"
    sha=$(cat "$stick.big.bin.sha")
    pages=$(sed -n 's/.*volumes: fat-usb0: pager made: \([0-9]*\) pages.*/\1/p' "$run" | head -1)
    if [ -z "$pages" ] || [ "$pages" -ge $((48 * 256)) ]; then
        echo "$name: budget: the budget is ${pages:-none} pages: not smaller than the file"
        ok=0
    fi
    hashed budget /usb0/big.bin "$sha" 1
    want budget 2 "pagertest: pass [12] of /usb0/big.bin: 50331648 bytes at 0 in [0-9]+ ms: sha256 $sha" "$run"
    # Both passes asked for all of it: 768 requests of 16 pages each.
    want budget 1 "fat /usb0: mapped file [0-9a-f]+ gone: 1536 page requests, 24576 pages supplied" "$run"
    no_page_errors budget
    rm -f "$stick" "$stick".*
}

case_kill() {
    stick="$out/$name-kill-a.img"
    make_stick "$stick" 48
    run_case kill "$stick"
    sha=$(cat "$stick.big.bin.sha")
    hashed kill /usb0/big.bin "$sha" 2
    n=$(count "fat /usb0: restart \(killed" "$run")
    [ "$n" -ge 6 ] || { echo "$name: kill: $n restarts of fat /usb0 carried on, wanted 6 or more"; ok=0; }
    never kill "fat /usb0: restart .*starting fresh" "$run"
    no_page_errors kill
    rm -f "$stick" "$stick".*
}

# The data partition's offset in bytes in a stick image: partition 2 of
# its MBR (tools/mkimage.py), wherever the ESP ends.
data_at() {
    python3 -c "import struct, sys; d = open(sys.argv[1], 'rb').read(512)
print(struct.unpack_from('<I', d, 446 + 16 + 8)[0] * 512)" "$1"
}

# flip <file> <offset>: one byte of the file changed, its size kept.
flip() {
    python3 -c "import sys; p, o = sys.argv[1], int(sys.argv[2]); d = bytearray(open(p, 'rb').read())
d[o] ^= 0x5a; open(p, 'wb').write(d)" "$1" "$2"
}

# The offsets in pxtest's file of a byte in the middle of its table's page
# 40, and of the first byte of its data segment (its writable PT_LOAD).
px_offsets() {
    python3 -c "import struct, sys
d = open(sys.argv[1], 'rb').read()
phoff, = struct.unpack_from('<Q', d, 0x20)
phnum, = struct.unpack_from('<H', d, 0x38)
data = None
for i in range(phnum):
    kind, flags, off = struct.unpack_from('<IIQ', d, phoff + 56 * i)
    if kind == 1 and flags & 2 and data is None:
        data = off
print(d.index(b'PXTEST-TABLE-0123456789') + 40 * 4096 + 100, data)" "$1"
}

# gone_pages <from> <to>: the service's ($svc's) "gone" lines for /data's
# mapped files between the output lines <from> and <to> of $run, as the
# pages each supplied, one a line.
gone_pages() {
    sed -n "/$1/,/$2/p" "$run" |
        sed -n "s/.*$svc \/data: mapped file [0-9a-f]* gone: [0-9]* page requests, \([0-9]*\) pages supplied.*/\1/p"
}

# The tools the ext cases need, or say what is missing.
ext_tools() {
    PATH=/usr/sbin:/sbin:$PATH
    for t in mke2fs debugfs e2fsck mcopy mformat; do
        command -v $t > /dev/null 2>&1 && continue
        echo "$name: $1: no $t here (needs e2fsprogs and mtools: the test PC's WSL has them)"
        ok=0
        return 1
    done
}

# The data partition's size in bytes in a stick image (its MBR's entry 2).
data_bytes() {
    python3 -c "import struct, sys; d = open(sys.argv[1], 'rb').read(512)
print(struct.unpack_from('<I', d, 446 + 16 + 12)[0] * 512)" "$1"
}

# ext_clean <case> <image> <offset> <bytes> <what>: the ext volume there
# (copied out) is what Linux's e2fsck -fn calls clean.
ext_clean() {
    vol="$out/$name-$1-check.img"
    dd if="$2" of="$vol" bs=1M iflag=skip_bytes,count_bytes skip="$3" count="$4" status=none
    if ! e2fsck -fn "$vol" > "$out/$name-$1-$5.fsck" 2>&1; then
        echo "$name: $1: e2fsck doesn't call the volume clean $5 (see $out/$name-$1-$5.fsck)"
        ok=0
    fi
    rm -f "$vol"
}

# flip_at <file> <offset>: one byte of the file changed in place.
flip_at() {
    python3 -c "import sys; f = open(sys.argv[1], 'r+b'); o = int(sys.argv[2]); f.seek(o)
b = f.read(1); f.seek(o); f.write(bytes([b[0] ^ 0x5a]))" "$1" "$2"
}

# ext_flip <stick> <file> <offset>: a byte of /data's file changed where
# its block lies on the stick, as an edit on the Mac's side would (the
# inode and its times untouched, so only the bytes differ).
ext_flip() {
    at=$(data_at "$1") vol="$out/$name-xexec-bmap.img"
    dd if="$1" of="$vol" bs=1M iflag=skip_bytes,count_bytes skip="$at" count="$(data_bytes "$1")" status=none
    blk=$(debugfs -R "bmap /$2 $(($3 / 4096))" "$vol" 2> /dev/null | tail -1)
    rm -f "$vol"
    case $blk in
    '' | *[!0-9]* | 0) return 1 ;;
    esac
    flip_at "$1" $((at + blk * 4096 + $3 % 4096))
}

# case_exec [x]: the exec case; with x (xexec) on a stick whose /data is ext.
case_exec() {
    c=exec svc=fat from=
    prog=build/user/pxtest.bootfs
    if [ "${1:-}" = x ]; then
        c=xexec svc=extfs from="$out/$name-xexec-from.img"
        ext_tools $c || return
        cp build/jamos.img "$from"
        tools/ext-stick.sh "$from" > "$out/$name-$c-ext.out" 2>&1 ||
            { echo "$name: $c: the ext stick couldn't be made (see $out/$name-$c-ext.out)"; ok=0; return; }
    fi
    if [ ! -f "$prog" ]; then
        echo "$name: $c: no $prog (make builds it)"
        ok=0
        return
    fi
    sha=$(sha256sum "$prog" | cut -d' ' -f1)
    pages=$(( ($(wc -c < "$prog") + 4095) / 4096 ))
    stick="$out/$name-$c-stick.img"
    rm -f "$stick"
    if ! QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/pager-exec.txt \
         QEMU_SAVE="$stick" QEMU_IMAGE="${from:-${QEMU_IMAGE:-build/jamos.img}}" \
         tools/qemu-test.sh "$out" "$name-$c" shell > "$out/$name-$c.out" 2>&1; then
        echo "$name: $c: boot 1's shell script FAILED (see $out/$name-$c.log)"
        grep "serial-feed: .*no '" "$out/$name-$c.out" || true
        ok=0
        rm -f "$stick" ${from:+"$from"}
        return
    fi
    rm -f ${from:+"$from"}
    run="$out/$name-$c.run.log"
    sed '/jam[^>]*>.* reboot -f/q' "$out/$name-$c.log" > "$run"
    [ $c = exec ] || want $c 1 "volumes: extfs-data: pager made: " "$run"
    # The table: one file, named by the program's hash (listed twice).
    want $c 2 "$sha" "$run"
    # Its table untouched, it fetched some of its pages, not most.
    lazy=$(gone_pages mark-e-lazy0 mark-e-lazy1)
    if [ "$(printf '%s\n' "$lazy" | grep -c .)" -ne 1 ] || [ "$lazy" -gt $((pages / 2)) ]; then
        echo "$name: $c: the lazy run's pages supplied: '${lazy:-none}', wanted one VMO" \
             "and at most $((pages / 2)) of its $pages"
        ok=0
    fi
    # Two instances at once: one VMO, its table (128 pages) and its code
    # fetched once for both.
    shared=$(gone_pages mark-e-share0 mark-e-share1)
    if [ "$(printf '%s\n' "$shared" | grep -c .)" -ne 1 ] || [ "$shared" -lt 128 ] ||
       [ "$shared" -gt "$pages" ]; then
        echo "$name: $c: two instances' pages supplied: '$(echo $shared)', wanted one VMO" \
             "of 128 to $pages pages"
        ok=0
    fi
    want $c 1 "its approval had no page table: the whole file matched it" "$run"
    never $c "page fault|don't match the approved|doesn't match its approval" "$run"
    never $c "did not end cleanly|left [0-9]+ units of job kind|its job still holds" "$run"
    at=$(data_at "$stick")
    [ $c = exec ] || ext_clean $c "$stick" "$at" "$(data_bytes "$stick")" "after boot 1"
    # On this side, as the Mac would: px's table, py's data segment, pz's
    # ELF header page, each a byte; pq left as it was.
    set -- $(px_offsets "$prog")
    for f in px:$1 py:$2 pz:2048; do
        file=${f%%:*} off=${f#*:}
        if [ $c = xexec ]; then
            ext_flip "$stick" "$file" "$off" ||
                { echo "$name: $c: couldn't change /data/$file on the stick"; ok=0; }
            continue
        fi
        mcopy -o -i "$stick@@$at" "::/$file" "$out/$name-exec-$file" &&
            flip "$out/$name-exec-$file" "$off" &&
            mcopy -o -i "$stick@@$at" "$out/$name-exec-$file" "::/$file" ||
            { echo "$name: exec: couldn't change /data/$file on the stick"; ok=0; }
        rm -f "$out/$name-exec-$file"
    done
    after=   # (xexec: the stick kept after boot 2, its volume checked)
    [ $c = exec ] || after="$stick.after"
    if ! QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT=tools/shell-tests/pager-exec2.txt \
         QEMU_IMAGE="$stick" QEMU_SAVE="$after" tools/qemu-test.sh "$out" "$name-${c}2" shell \
         > "$out/$name-${c}2.out" 2>&1; then
        echo "$name: $c: boot 2's shell script FAILED (see $out/$name-${c}2.log)"
        grep "serial-feed: .*no '" "$out/$name-${c}2.out" || true
        ok=0
    fi
    run="$out/$name-${c}2.run.log"
    sed '/jam[^>]*>.* reboot -f/q' "$out/$name-${c}2.log" > "$run"
    want $c 1 'user: process "px" killed: page fault: a page doesn.t match the approved program' "$run"
    want $c 1 "run: can't start /data/py \(ERR_IO\)" "$run"
    want $c 1 "run: /data/pz changed since it was allowed" "$run"
    want $c 1 'user: process "pq" killed: page fault: the file.s pager is gone' "$run"
    # The service says so once for each changed file's VMO.
    n=$(count "$svc /data: mapped file [0-9a-f]+: the disk.s pages don.t match the approved" "$run")
    [ "$n" -ge 3 ] || { echo "$name: $c: $svc's mismatch line $n times, wanted 3 or more"; ok=0; }
    # The shell lived through all of it (it never maps a program's pages).
    never $c 'user: process "shell" killed' "$run"
    if [ $c = xexec ] && [ -f "$stick.after" ]; then
        ext_clean $c "$stick.after" "$at" "$(data_bytes "$stick.after")" "after boot 2"
    fi
    rm -f "$stick" "$stick.after"
}

case_write() {
    stick="$out/$name-write-a.img"
    make_stick "$stick"
    run_case write "$stick"
    want write 1 "pagertest: write /data/w.bin: PASS" "$run"
    want write 1 "pagertest: write /usb0/w.bin: PASS" "$run"
    want write 1 "pagertest: touch /data/t.bin at 1048576 by a copy: ERR_INVALID_ARGS" "$run"
    want write 1 "pagertest\" killed: bus error: page past the end of a shrunk VMO" "$run"
    want write 1 "pagertest: touch /data/u.bin at 4096 by a copy: ERR_IO" "$run"
    want write 1 "pagertest\" killed: page fault: the file's pager is gone" "$run"
    want write 2 "fat /data: mapped file [0-9a-f]+ cut off: its file was removed" "$run"
    never write "MISMATCH|doesn't know|can't detach|can't read|can't fail" "$run"
    never write "can't follow its size|can't drop its old pages|couldn't read it" "$run"
    never write "pages not supplied|can't wait on the pager|don't match the approved" "$run"
    never write "did not end cleanly|left [0-9]+ units of job kind" "$run"
    rm -f "$stick" "$stick".*
}

case_pull() {
    stick="$out/$name-pull-a.img"
    make_stick "$stick" 16
    run_case pull "$stick"
    sha=$(cat "$stick.big.bin.sha")
    # Both runs read the first half (8 MiB) before the pull, right.
    want pull 2 "pagertest: pull /usb0/big.bin: read 8388608 bytes: sha256 " "$run"
    for at in 0 8388608; do
        line=$(grep -a "pagertest: pull /usb0/big.bin: a copy at $at: " "$run" | head -1 || true)
        case $line in
        *": ERR_IO in "*" ms"*)
            ms=${line##*ERR_IO in }
            ms=${ms%% ms*}
            [ "$ms" -lt 1000 ] ||
                { echo "$name: pull: the copy at $at failed after $ms ms, wanted under 1000"; ok=0; } ;;
        *) echo "$name: pull: the copy at $at: \"${line:-no line}\", wanted ERR_IO"; ok=0 ;;
        esac
    done
    want pull 1 "pagertest\" killed: page fault: the file's pager is gone" "$run"
    never pull "still readable|couldn't read it|didn't answer" "$run"
    want pull 2 "volumes: fat-usb0: pager closed" "$run"
    hashed pull /usb0/big.bin "$sha" 1
    never pull "MISMATCH|can't detach|can't fail|pages not supplied|can't wait on the pager" "$run"
    rm -f "$stick" "$stick".*
}

# jam_drive <case> <MiB> <name:MiB:seed>...: $drive, a Jam OS NVMe drive
# (tools/mkgpt.py's jamos layout, 1 GiB: the ESP a FAT volume, the data
# partition at 513 MiB an ext volume of <MiB> MiB) holding a file of that
# many MiB of seeded bytes for each name; each file's SHA-256 in
# $drive.<name>.sha.
jam_drive() {
    c=$1 mib=$2
    shift 2
    drive="$out/$name-$c-drive.img" esp="$out/$name-$c-esp.img" vol="$out/$name-$c-data.img"
    tree="$out/$name-$c-tree"
    rm -rf "$drive" "$esp" "$vol" "$tree"
    mkdir -p "$tree"
    for f in "$@"; do
        file=${f%%:*} rest=${f#*:}
        python3 -c "import random, sys; sys.stdout.buffer.write(random.Random($((${rest#*:}))).randbytes($((${rest%%:*})) << 20))" \
            > "$tree/$file"
        sha256sum "$tree/$file" | cut -d' ' -f1 > "$drive.$file.sha"
    done
    truncate -s "${mib}M" "$vol"
    tools/ext-stick.sh --volume "$vol" "$tree" jamdata > "$out/$name-$c-ext.out" 2>&1 ||
        { echo "$name: $c: the ext volume couldn't be made (see $out/$name-$c-ext.out)"; return 1; }
    truncate -s 128M "$esp"
    mformat -i "$esp" -T $((128 * 2048)) -F -v JAMOS-ESP :: ||
        { echo "$name: $c: mformat"; return 1; }
    python3 tools/mkgpt.py "$drive" --layout jamos --size 1G --fill "1=$esp" --fill "2=$vol" \
        > /dev/null
    rm -rf "$esp" "$vol" "$tree"
}

# jam_run <case>: its shell script (pager-<case>.txt) with $drive as the
# NVMe drive; its log up to the `reboot -f` at the end into $run.
jam_run() {
    if ! QEMU_TIMEOUT=${QEMU_TIMEOUT:-300} QEMU_INPUT="tools/shell-tests/pager-$1.txt" \
         QEMU_NVME="$drive" tools/qemu-test.sh "$out" "$name-$1" shell \
         > "$out/$name-$1.out" 2>&1; then
        echo "$name: $1: the shell script FAILED (see $out/$name-$1.log)"
        grep "serial-feed: .*no '" "$out/$name-$1.out" || true
        ok=0
    fi
    run="$out/$name-$1.run.log"
    sed '/jam[^>]*>.* reboot -f/q' "$out/$name-$1.log" > "$run"
    want "$1" 1 "volumes: extfs-jam: pager made: " "$run"
}

case_jam() {
    ext_tools jam || return
    jam_drive jam 64 pager.bin:2:11 || { ok=0; return; }
    sha=$(cat "$drive.pager.bin.sha")
    jam_run jam
    hashed jam /jam/pager.bin "$sha" 2
    want jam 1 "^$sha  */jam/pager.bin" "$run"
    # The mapping hashed twice asked for each 64 KiB once, the second pass
    # for nothing; the VMO of our own the same once.
    want jam 2 "pagertest: pass [12] of /jam/pager.bin: 2097152 bytes at 0 in [0-9]+ ms: sha256 $sha" "$run"
    want jam 2 "extfs /jam: mapped file [0-9a-f]+ gone: 32 page requests, 512 pages supplied" "$run"
    want jam 1 "pagertest: write /jam/w.bin: PASS" "$run"
    want jam 1 "pagertest: touch /jam/t.bin at 1048576 by a copy: ERR_INVALID_ARGS" "$run"
    want jam 1 "pagertest\" killed: bus error: page past the end of a shrunk VMO" "$run"
    # Removed under its mappings: ext keeps the inode, so both read on.
    want jam 1 "pagertest: touch /jam/u.bin at 4096 by a copy: OK" "$run"
    want jam 1 "pagertest: touched /jam/u.bin at 4096: byte " "$run"
    never jam "cut off|the file's pager is gone" "$run"
    no_page_errors jam
    ext_clean jam "$drive" $((513 << 20)) $((64 << 20)) "after the run"
    rm -f "$drive" "$drive".*
}

# The jstorm case: storm with a mapper. A file half again as big as the
# pager's budget (QEMU_MEM 512M: 32 MiB, the file 48 MiB) hashed through a
# mapping six times, so every pass asks extfs for every page, while `storm`
# copies another file on /jam and kills extfs-jam twice a second: the
# copy MATCHes, every hash is the host's, extfs restarted (as a process:
# ext has no warm spare) and never started fresh, no page failed.
case_jstorm() {
    ext_tools jstorm || return
    jam_drive jstorm 256 big.bin:48:12 src.bin:64:13 || { ok=0; return; }
    sha=$(cat "$drive.big.bin.sha")
    QEMU_MEM=512M jam_run jstorm
    hashed jstorm /jam/big.bin "$sha" 1
    want jstorm 6 "pagertest: pass [1-6] of /jam/big.bin: 50331648 bytes at 0 in [0-9]+ ms: sha256 $sha" "$run"
    want jstorm 1 "\[shell\] storm: 2 kills/s: .*; MATCH" "$run"
    n=$(count "extfs /jam: restart \(killed" "$run")
    [ "$n" -ge 2 ] || { echo "$name: jstorm: $n restarts of extfs /jam carried on, wanted 2 or more"; ok=0; }
    never jstorm "extfs /jam: restart .*starting fresh|giving up" "$run"
    no_page_errors jstorm
    ext_clean jstorm "$drive" $((513 << 20)) $((256 << 20)) "after the run"
    rm -f "$drive" "$drive".*
}

for c in $cases; do
    case $c in
    plumb) case_plumb ;;
    map) case_map ;;
    budget) case_budget ;;
    kill) case_kill ;;
    exec) case_exec ;;
    write) case_write ;;
    pull) case_pull ;;
    jam) case_jam ;;
    jstorm) case_jstorm ;;
    xexec) case_exec x ;;
    *) echo "$name: no case called $c"; ok=0 ;;
    esac
done
if [ $ok = 1 ]; then
    echo "$name: PASS"
    exit 0
fi
echo "$name: FAIL"
exit 1
