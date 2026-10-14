#!/bin/sh
# M12.7's stage-2 QEMU test: writes to a Jam OS disk, and only there
# (docs/M12.7-PLAN.md, W3; bin/disktest is the file storm). Beside
# tools/nvme-test.sh, which tests the driver's write path with utest
# (`write`, `diskro`), this runs the system the owner types at: volumes'
# refusals, `disk prepare`, `mount -w` on a Jam OS disk, a storm of synced
# files, power cuts and killed drivers. Every case keeps a Windows-like
# drive attached (tools/nvme-img.py's PC-like image) whose SHA-256 must be
# the same at the end (the owner's rule in place of reviews R1 and R2), and
# the QEMU trace of every command is checked: tools/nvme-trace-check.py,
# stage 1 where no write may happen, stage 2 with every Write inside the
# Jam OS drive's table and partitions.
# Cases (default: refuse; "all" runs every one in turn; run them in
# parallel with a separate <outdir> each):
#   refuse    the Windows-like, a blank and a Jam OS drive: `mount -w` on
#             Windows' ESP, writes by hand to it, `disk prepare` of Windows'
#             drive (shown as not empty, its erase asked for and answered
#             with a wrong serial tail) and of the Jam OS one ("a Jam OS
#             disk already"), of the blank one
#             with a wrong serial tail (refused), and the driver's own
#             refusals with volumes out of the way (utest's nvmew_refuse: a
#             writable open and create_table on Windows' drive, block.write
#             on a read-only channel). No Write ever reaches any drive
#             (trace stage 1), all three images unchanged
#   prepare   the Windows-like and a blank drive: `disk prepare` (the
#             serial's last four typed), `disks` and `mount` show the two
#             partitions, `mount -w`, a storm of 20 files and its check. A
#             second boot: it is a Jam OS disk from the start, `disk
#             prepare` refuses it, the 20 files are whole, 5 more are
#             written. The host then reads the image: Jam OS's table,
#             nothing outside it and the partitions (tools/nvme-written.py),
#             both partitions FAT32 without damage (tools/fatcheck.py)
#   cutstorm  a Jam OS drive (made on the host) and the Windows-like one;
#             QEMU killed with kill -9 at 10 points of a disktest storm and
#             booted again each time (one chain of boots on the same
#             images, the storm going on from the files already there): at
#             the point's Nth Write after the storm began (QEMU's trace of
#             every Write, flushed as it is made, says when; the drives are
#             throttled so that the kill lands within a Write of it). After
#             every cut: every file the log said was synced is whole, no
#             file below the highest is missing, at most the file being
#             written is damaged; the volume is mounted again and written
#             on. At the end the host's fatcheck finds no damage
#   cutprep   the same cut during `disk prepare` of a blank drive, at the
#             Nth Write of its table (1 to 5) and of the formats after it:
#             then a boot that finishes the preparation (`disk prepare`
#             again where the host sees a drive that isn't complete; none
#             where it is) and writes and checks a storm. The state the cut
#             left (table incomplete, formats missing, complete) is printed
#             per point
#   cut       cutstorm, then cutprep
#   cutdrv    the cut during create_table straight at the driver (no
#             volumes, no shell commands of W2's: utest's nvmew_prepare), at
#             each of its Writes; then utest's nvmew_prepare again on the
#             same image: the table is finished, or the drive was Jam OS's
#             already (create_table refused)
#   restart   a Jam OS drive (the first drive, so `kill nvme` finds it) and
#             the Windows-like one, booted with nvmetest=slow (a channel's
#             answers 20 ms apart, so a client that dies has requests out):
#             four rounds of a disktest storm in the background and, two
#             seconds in, `kill fat-nvme<N>` (the filesystem service: the
#             client of the driver's block channel dies with writes out;
#             its successor's channel must wait: the drain rule) or `kill
#             nvme` (the driver itself: its successor starts clean); then the
#             mount made writable again and every synced file checked whole.
#             The log must show a partition held for a gone client and
#             released (the drain rule engaged), the fatcheck of the image
#             no damage
# The disks' names (nvme0, ...) and their mounts (/nvme0, ...) are numbered
# in the order the drivers announce them, which varies from boot to boot: a
# script finds each drive by its QEMU serial in `disks` (tools/serial-feed.py's
# `seen re:` captures the numbers, and `send` puts them in).
# What the shell commands of W2 print (docs/M12.7-PLAN.md, P2) is in the
# NVW_ variables below; when a wording differs, change it there and nowhere
# else. NVW_POINTS, NVW_PREP_POINTS and NVW_DRV_POINTS name the cuts (Writes
# after the storm began, Writes of `disk prepare`, Writes of create_table),
# e.g. `tools/remote-test.sh env NVW_POINTS="1 5 21" tools/nvme-write-test.sh
# build/nw cutstorm` for a quick run (remote-test passes only some variables
# on by itself). QEMU_TIMEOUT, QEMU_SMP pass through. Nothing here writes to
# anything but files under <outdir>. QEMU is killed with kill -9 by its
# monitor socket's name and working directory (the checkout's), never by
# a guess: only the run's own.
# Usage: tools/nvme-write-test.sh <outdir> [case...]; exit 0 on PASS.
set -eu
out=$1
shift
cases=${*:-refuse}
[ "$cases" = all ] && cases="refuse prepare cutstorm cutprep cutdrv restart"
[ "$cases" = cut ] && cases="cutstorm cutprep"
mkdir -p "$out"

NVW_NOT_JAMOS=${NVW_NOT_JAMOS:-not on a Jam OS disk}     # mount -w on a disk that isn't
NVW_NOT_EMPTY=${NVW_NOT_EMPTY:-It is NOT empty}          # disk prepare on a drive that isn't empty
NVW_ASK=${NVW_ASK:-characters of its serial}             # disk prepare's request for the serial's end
NVW_WRONG=${NVW_WRONG:-not the end of}                   # and its answer to something else
NVW_ALREADY=${NVW_ALREADY:-is a Jam OS disk already}     # disk prepare on a Jam OS disk that needs nothing
NVW_BLANK_DISK=${NVW_BLANK_DISK:-nothing found on it}    # what `disks` says of a drive with no table
NVW_PREPARED=${NVW_PREPARED:-is a Jam OS disk now}       # and its answer when done
NVW_RW=${NVW_RW:-read-write}                             # mount -w: "is now read-write", "read-write already"
NVW_POINTS=${NVW_POINTS:-1 2 3 5 8 13 21 34 55 89}
NVW_PREP_POINTS=${NVW_PREP_POINTS:-1 3 4 5 6 8 12 20 35 50}   # a whole prepare is about 53 Writes
NVW_DRV_POINTS=${NVW_DRV_POINTS:-1 3 4 5 6}

sha256() { if command -v sha256sum > /dev/null 2>&1; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }

fails=0

# ---- the drives -------------------------------------------------------------------------

mk_win() { python3 tools/nvme-img.py "$1" windows; }
mk_blank() { python3 tools/mkgpt.py "$1" --layout blank --size 640M > /dev/null; }
# A Jam OS drive with both partitions formatted, as `disk prepare` leaves it:
# the ESP (512 MiB from 1 MiB) and the data partition (126 MiB from 513 MiB).
mk_jam() {
    python3 tools/mkgpt.py "$1" --layout jamos --size 640M > /dev/null
    mformat -i "$1@@1M" -T 1048576 -F -v JAMESP ::
    mformat -i "$1@@513M" -T 258048 -F -v JAMDATA ::
}
# The end of QEMU's serial number for the drive in slot $1 (tools/qemu-test.sh).
tail4() { printf '%s' "jamos-nvme$1" | tail -c 4; }

# ---- the lines of a script typed into the shell ----------------------------------------------

# The top of every script: the shell's prompt is up before the drivers have
# bound and init has handed out its channels (a command typed at once found
# "no devmgr control channel"), so wait a moment.
settle() {
    echo "wait 120 Jam OS shell"
    echo "wait {prompt}"
    echo "seen 60 init: /data mounted"
    echo "send sleep 4"
    echo "wait {prompt}"
}

# names <win slot> <blank slot> <jam slot>: `disks`, and from its answer the
# numbers of the drives in those QEMU slots (the order QEMU_NVME gave them;
# "" for a drive not wanted): {wd} {bd} {jd} their disks (nvme<N>), {we} the
# Windows drive's ESP mount, {je} and {jm} the Jam OS drive's ESP and data
# mounts (/nvme<N>), from anywhere in the output so far (`seen`: a drive
# that comes up as a Jam OS disk later is found by a second call).
names() {
    echo "send disks"
    if [ -n "${1:-}" ]; then
        printf '%s\n' "seen re:nvme(?P<wd>[0-9]+) +QEMU NVMe Ctrl +serial jamos-nvme$1 "
        printf '%s\n' "seen re:serial jamos-nvme$1 [^\r\n]*\r?\n +1 +EFI system [^\r\n]*?/nvme(?P<we>[0-9]+)"
    fi
    if [ -n "${2:-}" ]; then
        printf '%s\n' "seen re:nvme(?P<bd>[0-9]+) +QEMU NVMe Ctrl +serial jamos-nvme$2 "
    fi
    if [ -n "${3:-}" ]; then
        printf '%s\n' "seen re:nvme(?P<jd>[0-9]+) +QEMU NVMe Ctrl +serial jamos-nvme$3 "
        printf '%s\n' "seen re:serial jamos-nvme$3 [^\r\n]*\r?\n +1 +EFI system [^\r\n]*?/nvme(?P<je>[0-9]+)[^\r\n]*\r?\n +2 +Jam OS data [^\r\n]*?/nvme(?P<jm>[0-9]+)"
    fi
    echo "wait {prompt}"
}

# ---- one boot -----------------------------------------------------------------------------

# boot <dir> <name> <words> <images>: the shell script <dir>/<name>.in
# typed into the shell, every NVMe command traced (the Writes with their
# blocks). Fails if the run did.
boot() {
    QEMU_TIMEOUT=${QEMU_TIMEOUT:-420} QEMU_NVME="$4" QEMU_NVME_THROTTLE="${nvw_throttle:-}" QEMU_INPUT="$1/$2.in" \
        QEMU_EXTRA="-trace pci_nvme_pci_reset -trace pci_nvme_admin_cmd -trace pci_nvme_io_cmd -trace pci_nvme_write -D $1/$2.trace" \
        QEMU_WORDS="$3" tools/qemu-test.sh "$1" "$2" shell > "$1/$2.out" 2>&1 ||
        { echo "$2: FAIL: the run failed (see $1/$2.out)"; grep -a "serial-feed: .*no '" "$1/$2.out" || true; return 1; }
}

# nwrites <trace>: the Writes QEMU has traced so far.
nwrites() {
    n=$(grep -ac "^pci_nvme_write " "$1" 2> /dev/null || true)
    echo "${n:-0}"
}

# find_qemu <name>: the pid of the QEMU of run <name> (tools/qemu-test.sh
# names its monitor socket build/.qemu-<name>.sock): the process with that on
# its command line whose working directory is this checkout's; empty if it
# isn't there (yet).
find_qemu() {
    here=$(pwd -P)
    for p in $(pgrep -f "qemu-system-x86_64 .*-monitor unix:build/.qemu-$1.sock" || true); do
        [ "$(readlink "/proc/$p/cwd" 2> /dev/null)" = "$here" ] && { echo "$p"; return 0; }
    done
    return 0
}

# boot_cut <dir> <name> <words> <images> <marker> <delta>: boot, and kill
# -9 QEMU when the trace has <delta> more Writes than it had when the log
# first showed <marker> ("" for none: counted from the start). The drives
# are throttled (QEMU's throttle: tools/qemu-test.sh's QEMU_NVME_THROTTLE):
# without it five Writes take less time than the poll between two looks at
# the trace. A throttle's bucket holds a tenth of a second's operations, so
# at 2 a second it holds a fifth of one and every Write waits its turn: the
# kill lands on the point's Write (the first two pass at once, though: a cut
# at the 1st lands at the 2nd, and the line says where). That is for ten;
# further out the exact Write matters less than the time it takes to get
# there, and the drives take 30 a second (a Write or three of slack).
# Sets `cut` to the Writes traced when the kill was sent, or empty when the
# run ended first.
boot_cut() {
    d=$1 n=$2 marker=$5 delta=$6
    cut=
    nvw_throttle="x-iops-write=$([ "$delta" -le 10 ] && echo 2 || echo 30)"
    boot "$d" "$n" "$3" "$4" > "$d/$n.status" 2>&1 &
    bpid=$!
    nvw_throttle=
    log="$d/$n.log" trace="$d/$n.trace"
    limit=$(( ${QEMU_TIMEOUT:-420} * 100 ))   # polls of at least 5 ms
    i=0 base=0 target= qp= lastw=-1
    while [ $i -lt $limit ] && kill -0 $bpid 2> /dev/null; do
        [ -n "$qp" ] || qp=$(find_qemu "$n")
        if [ -z "$target" ] && { [ -z "$marker" ] || grep -aqF -- "$marker" "$log" 2> /dev/null; }; then
            base=$(nwrites "$trace")
            target=$((base + delta))
        fi
        if [ -n "$target" ] && [ -n "$qp" ]; then
            w=$(nwrites "$trace")
            [ "$w" = "$lastw" ] || { echo "$(date +%s.%N) Write $w" >> "$d/$n.polls"; lastw=$w; }
            if [ "$w" -ge "$target" ]; then
                kill -9 "$qp"
                cut=$w
                break
            fi
        fi
        sleep 0.005
        i=$((i + 1))
    done
    wait $bpid || true
    [ -z "$cut" ] || echo "$n: killed (kill -9) at Write $cut of the trace (the storm began at $base)"
}

# ---- checks -------------------------------------------------------------------------------

# s_want <log> <regex>...: each line there, or a FAIL line and 1.
s_want() {
    f=$1
    shift
    r=0
    for re in "$@"; do
        grep -aqE -- "$re" "$f" || { echo "nvme-write-$c: FAIL: no line matching '$re' in $f"; r=1; }
    done
    return $r
}

# s_trace <dir> <stage> [--write-ranges R]: every boot's trace of this case.
s_trace() {
    d=$1 stage=$2
    shift 2
    r=0
    for t in "$d"/nvw-*.trace; do
        grep -aq "^pci_nvme_pci_reset" "$t" 2> /dev/null || { echo "nvme-write-$c: FAIL: the trace is dead: $t"; r=1; continue; }
        python3 tools/nvme-trace-check.py --stage "$stage" --require-admin 06 --require-io 02 "$@" "$t" > "$t.check" ||
            { echo "nvme-write-$c: FAIL: the trace (see $t.check)"; r=1; }
    done
    return $r
}

# fatchk <label> <image@@offset> [--require-clean]: damage fails; lost
# clusters (fatcheck's 2: a cut leaves them, there is no fsck) are said.
fatchk() {
    st=0
    python3 tools/fatcheck.py "$2" ${3:-} > "$dir/fatcheck-$1.txt" 2>&1 || st=$?
    case $st in
    0) ;;
    2) echo "nvme-write-$c: $1: $(grep -a 'lost' "$dir/fatcheck-$1.txt" | head -1) (no damage)" ;;
    *) echo "nvme-write-$c: FAIL: fatcheck of $1: exit $st (see $dir/fatcheck-$1.txt)"; return 1 ;;
    esac
}

# verify <log>...: the logs in boot order. Every `disktest <dir> check` line
# has a prefix of whole files at least as long as the highest "synced N"
# before it, no file missing, at most one damaged. Needs one check.
verify() {
    cat "$@" | tr -d '\r' |
        grep -aoE "synced [0-9]+|check /nvme[0-9]+: files=[0-9]+ whole=[0-9]+ damaged=[0-9]+ missing=[0-9]+ prefix=[0-9]+" |
        awk -F'[ =]' -v c="nvme-write-$c" '
            /^synced / { if ($2 + 0 > s) s = $2 + 0; next }
            { n++; why = ""
              if ($12 + 0 < s) why = why " prefix " $12 " below the " s " synced"
              if ($10 + 0 != 0) why = why " " $10 " missing"
              if ($8 + 0 > 1) why = why " " $8 " damaged"
              printf "%s: check %d: %d files, prefix %d, synced %d, damaged %d%s\n", c, n, $4, $12, s, $8, (why == "" ? "" : ": FAIL:" why)
              if (why != "") bad = 1 }
            END { if (!n) { print c ": FAIL: no check line"; bad = 1 } exit bad }'
}

# written <image>: the Jam OS table is right and nothing is written outside
# it and the partitions; sets `ranges`.
written() {
    python3 tools/nvme-written.py "$1" --pattern none --ranges > "$dir/written.txt" 2>&1 ||
        { echo "nvme-write-$c: FAIL: $(tail -1 "$dir/written.txt")"; return 1; }
    head -1 "$dir/written.txt" | sed "s/^/nvme-write-$c: /"
    ranges=$(tail -1 "$dir/written.txt")
}

finish() {   # finish <ok>
    if [ "$1" = 1 ]; then
        rm -f "$dir"/*.img
        echo "nvme-write-$c: PASS"
    else
        echo "nvme-write-$c: FAIL (see $dir)"
        fails=$((fails + 1))
    fi
}

# ---- refuse -----------------------------------------------------------------------------

# Drives in QEMU's order: 0 Windows-like, 1 blank, 2 Jam OS.
refuse() {
    ok=1
    win="$dir/windows.img" blank="$dir/blank.img" jam="$dir/jamos.img"
    mk_win "$win"
    mk_blank "$blank"
    mk_jam "$jam"
    sums=$(sha256 "$win" "$blank" "$jam" | cut -d' ' -f1)
    {
        echo "# generated by tools/nvme-write-test.sh: refuse"
        settle
        names 0 1 2
        echo "send mount -w /nvme{we}"
        echo "wait $NVW_NOT_JAMOS"
        echo "wait {prompt}"
        echo "send touch /nvme{we}/jamos-refuse; echo rf-\"\"1 \$?"
        echo "wait rf-1 1"
        echo "wait {prompt}"
        echo "send rm /nvme{we}/EFI/Microsoft/Boot/BCD; echo rf-\"\"2 \$?"
        echo "wait rf-2 1"
        echo "wait {prompt}"
        echo "send mkdir /nvme{we}/jamos-refuse; echo rf-\"\"3 \$?"
        echo "wait rf-3 1"
        echo "wait {prompt}"
        # Windows' drive: shown as not empty, its erase asked for, and
        # answered with the wrong end of its serial: nothing is written.
        echo "send disk prepare nvme{wd}"
        echo "wait $NVW_NOT_EMPTY"
        echo "wait $NVW_ASK"
        echo "sleep 1"
        echo "send xxxx"
        echo "wait $NVW_WRONG"
        echo "wait {prompt}"
        echo "send disk prepare nvme{jd}"
        echo "wait $NVW_ALREADY"
        echo "wait {prompt}"
        # The blank drive, with the wrong end of its serial: it asks, it is
        # told no, and nothing is written.
        echo "send disk prepare nvme{bd}"
        echo "wait $NVW_ASK"
        echo "sleep 1"   # typed at once, the answer was once lost
        echo "send xxxx"
        echo "wait $NVW_WRONG"
        echo "wait {prompt}"
        # The driver's own refusals, volumes out of the way.
        echo "send utest only nvmew_refuse; echo rf-\"\"4 \$?"
        echo "wait 240 rf-4 0"
        echo "wait {prompt}"
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$dir/nvw-refuse.in"
    boot "$dir" nvw-refuse "nvmetest=on" "$win $blank $jam" || ok=0
    log="$dir/nvw-refuse.log"
    s_want "$log" "utest: 1 passed" "nvmew_refuse: drive [0-9]+ \(someone else's, 4 partitions\): every write refused" || ok=0
    [ "$(sha256 "$win" "$blank" "$jam" | cut -d' ' -f1)" = "$sums" ] || { echo "nvme-write-$c: FAIL: an image changed"; ok=0; }
    # (a wrong serial must not have prepared anything: the blank image is among those above)
    s_trace "$dir" 1 || ok=0
    finish $ok
}

# ---- prepare ----------------------------------------------------------------------------

# The prepared drive's checks, on the host: its table, the partitions.
prepared_ok() {
    written "$1" || return 1
    fatchk esp "$1@@1M" || return 1
    fatchk data "$1@@513M" --require-clean || return 1
}

# Drives: 0 Windows-like, 1 blank.
prepare() {
    ok=1
    win="$dir/windows.img" blank="$dir/blank.img"
    mk_win "$win"
    mk_blank "$blank"
    before=$(sha256 "$win" | cut -d' ' -f1)
    ser=$(tail4 1)
    {
        echo "# generated by tools/nvme-write-test.sh: prepare, first boot"
        settle
        names 0 1 ""
        printf '%s\n' "seen re:nvme[0-9]+ +QEMU NVMe Ctrl +serial jamos-nvme1 [^\r\n]*$NVW_BLANK_DISK"
        echo "send disk prepare nvme{bd}"
        echo "wait $NVW_ASK"
        echo "sleep 1"   # typed at once, the answer was once lost
        echo "send $ser"
        echo "wait 360 $NVW_PREPARED"
        echo "wait {prompt}"
        names "" "" 1
        echo "send mount"
        echo "wait /nvme{je}"
        echo "wait /nvme{jm}"
        echo "wait {prompt}"
        echo "send mount -w /nvme{jm}"
        echo "wait $NVW_RW"
        echo "wait {prompt}"
        echo "send disktest /nvme{jm} write 20 96"
        echo "wait 120 synced 20"
        echo "wait {prompt}"
        echo "send disktest /nvme{jm} check; echo pr-\"\"1 \$?"
        echo "wait files=20 whole=20 damaged=0 missing=0 prefix=20"
        echo "wait pr-1 0"
        echo "wait {prompt}"
        # the ESP is written the same way
        echo "send mount -w /nvme{je}"
        echo "wait $NVW_RW"
        echo "wait {prompt}"
        echo "send disktest /nvme{je} write 3 32; echo pr-\"\"2 \$?"
        echo "wait 60 synced 3"
        echo "wait pr-2 0"
        echo "wait {prompt}"
        echo "send sync"
        echo "wait {prompt}"
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$dir/nvw-prepare-1.in"
    boot "$dir" nvw-prepare-1 "nvmetest=on" "$win $blank" || ok=0
    {
        echo "# generated by tools/nvme-write-test.sh: prepare, second boot"
        settle
        names 0 "" 1
        echo "send disk prepare nvme{jd}"
        echo "wait $NVW_ALREADY"
        echo "wait {prompt}"
        echo "send mount -w /nvme{jm}"
        echo "wait $NVW_RW"
        echo "wait {prompt}"
        echo "send disktest /nvme{jm} check; echo pr-\"\"3 \$?"
        echo "wait files=20 whole=20 damaged=0 missing=0 prefix=20"
        echo "wait pr-3 0"
        echo "wait {prompt}"
        echo "send disktest /nvme{jm} write 5 96"
        echo "wait 60 synced 25"
        echo "wait {prompt}"
        echo "send disktest /nvme{jm} check; echo pr-\"\"4 \$?"
        echo "wait files=25 whole=25 damaged=0 missing=0 prefix=25"
        echo "wait pr-4 0"
        echo "wait {prompt}"
        echo "send mount -w /nvme{je}"
        echo "wait $NVW_RW"
        echo "wait {prompt}"
        echo "send disktest /nvme{je} check; echo pr-\"\"5 \$?"
        echo "wait files=3 whole=3 damaged=0 missing=0 prefix=3"
        echo "wait pr-5 0"
        echo "wait {prompt}"
        echo "send sync"
        echo "wait {prompt}"
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$dir/nvw-prepare-2.in"
    boot "$dir" nvw-prepare-2 "nvmetest=on" "$win $blank" || ok=0
    [ "$(sha256 "$win" | cut -d' ' -f1)" = "$before" ] || { echo "nvme-write-$c: FAIL: the Windows-like image changed"; ok=0; }
    prepared_ok "$blank" || ok=0
    s_trace "$dir" 2 --write-ranges "$ranges" || ok=0
    finish $ok
}

# ---- cutstorm ---------------------------------------------------------------------------

# Drives: 0 Windows-like, 1 Jam OS (formatted on the host). One chain of
# boots on the same two images: each mounts the data partition writable,
# checks what the last cut left, and goes on with the storm until it is
# killed.
cutstorm() {
    ok=1
    win="$dir/windows.img" jam="$dir/jamos.img"
    mk_win "$win"
    mk_jam "$jam"
    before=$(sha256 "$win" | cut -d' ' -f1)
    k=0
    logs=
    for delta in $NVW_POINTS; do
        k=$((k + 1))
        bn=nvw-cut-$k
        {
            echo "# generated by tools/nvme-write-test.sh: cutstorm $k (the cut $delta Writes into the storm)"
            settle
            names "" "" 1
            echo "send mount -w /nvme{jm}"
            echo "wait 60 $NVW_RW"
            echo "wait {prompt}"
            echo "send disktest /nvme{jm} check"
            echo "wait 60 check /nvme{jm}: files="
            echo "wait {prompt}"
            echo "send disktest /nvme{jm} write 400 64; echo cs-\"\"$k \$?"
            echo "wait 300 cs-$k"
            echo "send reboot -f"
            echo "wait reboot: resetting"
        } > "$dir/$bn.in"
        boot_cut "$dir" $bn "nvmetest=on" "$win $jam" "disktest: writing /nvme" "$delta"
        [ -n "$cut" ] || echo "nvme-write-$c: point $k: the storm ended before $delta Writes (not cut)"
        logs="$logs $dir/$bn.log"
    done
    {
        echo "# generated by tools/nvme-write-test.sh: cutstorm, the last boot (no cut)"
        settle
        names "" "" 1
        echo "send mount -w /nvme{jm}"
        echo "wait 60 $NVW_RW"
        echo "wait {prompt}"
        echo "send disktest /nvme{jm} check; echo cs-\"\"a \$?"
        echo "wait 60 check /nvme{jm}: files="
        echo "wait cs-a"
        echo "wait {prompt}"
        echo "send disktest /nvme{jm} write 10 64; echo cs-\"\"b \$?"
        echo "wait 120 cs-b 0"
        echo "wait {prompt}"
        echo "send disktest /nvme{jm} check; echo cs-\"\"c \$?"
        echo "wait 60 check /nvme{jm}: files="
        echo "wait cs-c 0"
        echo "wait {prompt}"
        echo "send sync"
        echo "wait {prompt}"
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$dir/nvw-cut-last.in"
    boot "$dir" nvw-cut-last "nvmetest=on" "$win $jam" || ok=0
    logs="$logs $dir/nvw-cut-last.log"
    # shellcheck disable=SC2086
    verify $logs || ok=0
    s_want "$dir/nvw-cut-last.log" "check /nvme[0-9]+: files=[0-9]+ whole=[0-9]+ damaged=0 missing=0 prefix=[0-9]+" || ok=0
    [ "$(sha256 "$win" | cut -d' ' -f1)" = "$before" ] || { echo "nvme-write-$c: FAIL: the Windows-like image changed"; ok=0; }
    written "$jam" || ok=0
    fatchk esp "$jam@@1M" || ok=0
    fatchk data "$jam@@513M" || ok=0
    s_trace "$dir" 2 --write-ranges "$ranges" || ok=0
    finish $ok
}

# ---- cutprep ----------------------------------------------------------------------------

# classify <image>: what a cut `disk prepare` left, from the image alone:
# "complete" (Jam OS's table, both partitions FAT32), "formats missing"
# (the table is Jam OS's, a partition isn't FAT32), "table incomplete".
classify() {
    if python3 tools/nvme-written.py "$1" > /dev/null 2>&1; then
        e=0 d=0
        python3 tools/fatcheck.py "$1@@1M" > /dev/null 2>&1 || e=$?
        python3 tools/fatcheck.py "$1@@513M" > /dev/null 2>&1 || d=$?
        if [ $e -ne 3 ] && [ $d -ne 3 ]; then echo complete; else echo "formats missing"; fi
    else
        echo "table incomplete"
    fi
}

# Drives: 0 Windows-like, 1 blank. Per point, a fresh blank drive: cut
# during `disk prepare`, then a boot that finishes it.
cutprep() {
    ok=1
    win="$dir/windows.img" blank="$dir/blank.img"
    mk_win "$win"
    before=$(sha256 "$win" | cut -d' ' -f1)
    ser=$(tail4 1)
    k=0
    for delta in $NVW_PREP_POINTS; do
        k=$((k + 1))
        mk_blank "$blank"
        bn=nvw-prep-$k
        {
            echo "# generated by tools/nvme-write-test.sh: cutprep $k (the cut at Write $delta of disk prepare)"
            settle
            names "" 1 ""
            echo "send disk prepare nvme{bd}"
            echo "wait $NVW_ASK"
            echo "sleep 1"   # typed at once, the answer was once lost
            echo "send $ser"
            echo "wait 360 $NVW_PREPARED"
            echo "send reboot -f"
            echo "wait reboot: resetting"
        } > "$dir/$bn.in"
        boot_cut "$dir" $bn "nvmetest=on" "$win $blank" "" "$delta"
        state=$(classify "$blank")
        [ -n "$cut" ] || state="not cut ($state)"
        echo "nvme-write-$c: point $k (Write $delta): $state"
        {
            echo "# generated by tools/nvme-write-test.sh: cutprep $k, the boot after: $state"
            settle
            case $state in
            complete | "not cut (complete)") names "" "" 1 ;;
            *)
                # not finished: `disk prepare` must take it up again
                names "" 1 ""
                echo "send disk prepare nvme{bd}"
                echo "wait $NVW_ASK"
                echo "sleep 1"   # typed at once, the answer was once lost
                echo "send $ser"
                echo "wait 360 $NVW_PREPARED"
                echo "wait {prompt}"
                names "" "" 1 ;;
            esac
            echo "send mount"
            echo "wait /nvme{jm}"
            echo "wait {prompt}"
            echo "send mount -w /nvme{jm}"
            echo "wait $NVW_RW"
            echo "wait {prompt}"
            echo "send disktest /nvme{jm} write 5 32; echo cp-\"\"$k \$?"
            echo "wait 120 synced 5"
            echo "wait cp-$k 0"
            echo "wait {prompt}"
            echo "send disktest /nvme{jm} check; echo cq-\"\"$k \$?"
            echo "wait files=5 whole=5 damaged=0 missing=0 prefix=5"
            echo "wait cq-$k 0"
            echo "wait {prompt}"
            echo "send sync"
            echo "wait {prompt}"
            echo "send reboot -f"
            echo "wait reboot: resetting"
        } > "$dir/$bn-after.in"
        boot "$dir" $bn-after "nvmetest=on" "$win $blank" || ok=0
        prepared_ok "$blank" || ok=0
        # this point's traces are checked now: the next point reuses the names
        s_trace "$dir" 2 --write-ranges "$ranges" || ok=0
        mkdir -p "$dir/point-$k"
        mv "$dir"/nvw-prep-$k.* "$dir"/nvw-prep-$k-after.* "$dir/point-$k/" 2> /dev/null || true
    done
    [ "$(sha256 "$win" | cut -d' ' -f1)" = "$before" ] || { echo "nvme-write-$c: FAIL: the Windows-like image changed"; ok=0; }
    finish $ok
}

# ---- cutdrv -----------------------------------------------------------------------------

# The same cut at the driver: create_table (utest's nvmew_prepare, no
# volumes in the way), killed at its Nth Write, then nvmew_prepare again.
cutdrv() {
    ok=1
    win="$dir/windows.img" blank="$dir/blank.img"
    mk_win "$win"
    before=$(sha256 "$win" | cut -d' ' -f1)
    k=0
    for delta in $NVW_DRV_POINTS; do
        k=$((k + 1))
        mk_blank "$blank"
        bn=nvw-drv-$k
        {
            echo "# generated by tools/nvme-write-test.sh: cutdrv $k (create_table cut at Write $delta)"
            settle
            echo "send utest only nvmew_prepare; echo cd-\"\"$k \$?"
            echo "wait 240 cd-$k"
            echo "send reboot -f"
            echo "wait reboot: resetting"
        } > "$dir/$bn.in"
        boot_cut "$dir" $bn "nvmetest=on" "$win $blank" "" "$delta"
        state=$(classify "$blank")
        [ -n "$cut" ] || state="not cut ($state)"
        echo "nvme-write-$c: point $k (Write $delta): $state"
        {
            echo "# generated by tools/nvme-write-test.sh: cutdrv $k, the boot after: $state"
            settle
            echo "send utest only nvmew_prepare; echo ce-\"\"$k \$?"
            echo "wait 240 ce-$k"
            echo "wait {prompt}"
            echo "send reboot -f"
            echo "wait reboot: resetting"
        } > "$dir/$bn-after.in"
        boot "$dir" $bn-after "nvmetest=on" "$win $blank" || ok=0
        s_want "$dir/$bn-after.log" "utest: 1 passed" || ok=0
        case $state in
        "table incomplete") s_want "$dir/$bn-after.log" "create_table: Jam OS's table written and read back" || ok=0 ;;
        *) s_want "$dir/$bn-after.log" "is Jam OS's already: create_table refused" || ok=0 ;;
        esac
        written "$blank" || ok=0
        s_trace "$dir" 2 --write-ranges "$ranges" || ok=0
        mkdir -p "$dir/point-$k"
        mv "$dir"/nvw-drv-$k.* "$dir"/nvw-drv-$k-after.* "$dir/point-$k/" 2> /dev/null || true
    done
    [ "$(sha256 "$win" | cut -d' ' -f1)" = "$before" ] || { echo "nvme-write-$c: FAIL: the Windows-like image changed"; ok=0; }
    finish $ok
}

# ---- restart ----------------------------------------------------------------------------

# Drives: 0 Jam OS (the first drive, so `kill nvme` is its driver), 1
# Windows-like.
restart() {
    ok=1
    win="$dir/windows.img" jam="$dir/jamos.img"
    mk_win "$win"
    mk_jam "$jam"
    before=$(sha256 "$win" | cut -d' ' -f1)
    {
        echo "# generated by tools/nvme-write-test.sh: restart"
        settle
        names 1 "" 0
        echo "send mount -w /nvme{jm}"
        echo "wait 60 $NVW_RW"
        echo "wait {prompt}"
        r=0
        # Rounds: the filesystem service killed under the storm (its spare takes
        # over the open file, so the storm goes on through the successor's block
        # channel, which must wait for the dead one's writes: the drain rule),
        # then the driver killed under it (both NVMe drivers: `kill nvme` takes
        # whichever of them it finds first).
        for victim in "kill fat-nvme{jm}" "kill nvme; kill nvme" "kill fat-nvme{jm}" "kill nvme; kill nvme"; do
            r=$((r + 1))
            echo "send disktest /nvme{jm} write 100000 64 &"
            printf '%s\n' "wait re:\[(?P<jn>[0-9]+)\] [0-9]+ disktest: in the background"
            echo "wait 60 disktest: writing /nvme{jm} from file"
            echo "send sleep 2; $victim; echo rs-\"\"$r \$?"
            echo "wait 60 rs-$r"
            echo "wait {prompt}"
            # two seconds more of the storm (through the successor, if the service
            # was the victim), then it is stopped, so the check sees a quiet disk
            echo "send sleep 2; kill %{jn}; echo rk-\"\"$r"
            echo "wait 60 rk-$r"
            echo "wait {prompt}"
            # the successor is up a moment later, and its mounts may be numbered
            # again and read-only again
            echo "send sleep 5; disks"
            printf '%s\n' "wait re:serial jamos-nvme0 [^\r\n]*\r?\n +1 +EFI system [^\r\n]*?/nvme(?P<je>[0-9]+)[^\r\n]*\r?\n +2 +Jam OS data [^\r\n]*?/nvme(?P<jm>[0-9]+)"
            echo "wait {prompt}"
            echo "send mount -w /nvme{jm}; echo rm-\"\"$r \$?"
            echo "wait 60 rm-$r 0"
            echo "wait {prompt}"
            echo "send disktest /nvme{jm} check; echo rc-\"\"$r \$?"
            echo "wait 60 check /nvme{jm}: files="
            echo "wait rc-$r"
            echo "wait {prompt}"
        done
        echo "send disktest /nvme{jm} write 10 64; echo rs-\"\"z \$?"
        echo "wait 120 rs-z 0"
        echo "wait {prompt}"
        echo "send disktest /nvme{jm} check; echo rc-\"\"z \$?"
        echo "wait 60 check /nvme{jm}: files="
        echo "wait rc-z 0"
        echo "wait {prompt}"
        echo "send sync"
        echo "wait {prompt}"
        echo "send reboot -f"
        echo "wait reboot: resetting"
    } > "$dir/nvw-restart.in"
    boot "$dir" nvw-restart "nvmetest=slow" "$jam $win" || ok=0
    log="$dir/nvw-restart.log"
    verify "$log" || ok=0
    # the right driver was killed (the Jam OS drive's), each time
    jd=$(grep -aoE "nvme[0-9]+  QEMU NVMe Ctrl  serial jamos-nvme0 " "$log" | head -1 | sed 's/^nvme\([0-9]*\) .*/\1/')
    [ "$(grep -ac "nvme$jd: its driver ended" "$log")" -ge 2 ] ||
        { echo "nvme-write-$c: FAIL: nvme$jd's driver wasn't killed in both driver rounds"; ok=0; }
    # the drain rule engaged: a partition held for a client that died with
    # requests out, and let go
    s_want "$log" "partition [12]: its client left with [1-9][0-9]* request" "partition [12]: the gone client's requests are done" || ok=0
    [ "$(sha256 "$win" | cut -d' ' -f1)" = "$before" ] || { echo "nvme-write-$c: FAIL: the Windows-like image changed"; ok=0; }
    written "$jam" || ok=0
    fatchk esp "$jam@@1M" || ok=0
    fatchk data "$jam@@513M" || ok=0
    s_trace "$dir" 2 --write-ranges "$ranges" || ok=0
    finish $ok
}

for c in $cases; do
    case $c in
    refuse | prepare | cutstorm | cutprep | cutdrv | restart) ;;
    *) echo "nvme-write-test: no case '$c'"; exit 2 ;;
    esac
    dir="$out/nvme-write-$c"
    rm -rf "$dir"
    mkdir -p "$dir"
    # the blocks a Jam OS drive's writes may reach: the layout's, from a fresh image
    # (a case that can read the real image's own overrides this)
    mk_jam "$dir/ref.img"
    ranges=$(python3 tools/nvme-written.py "$dir/ref.img" --ranges | tail -1)
    rm -f "$dir/ref.img"
    $c
done
[ $fails = 0 ]
