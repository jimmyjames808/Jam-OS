#!/bin/sh
# M12.7 stage 3's QEMU test of the installer (`install <disk>` in the shell,
# user/services/init/install.c; docs/M12.7-PLAN.md, I2 and I3 notes): Jam OS
# written from the stick to a blank NVMe drive (QEMU_NVME, a 1 GiB image from
# tools/mkgpt.py; its QEMU serial is jamos-nvme0, so the typed end is vme0),
# and the drive then booted on its own (QEMU_BOOT_NVME=1: the image is the
# NVMe drive, nothing is on the USB port; QEMU_MENU_AT=keep: Limine reads the
# menu `install` wrote, not a test's). The stick image has files in
# /data/etc (a file, one in a subdirectory, an empty one) for the copy.
# Cases (default: install; "all" runs every one; one <outdir> each, run in
# parallel as pc-batch shards):
#   install  the stick and a blank drive: `install` with the wrong end of
#            the serial (nothing written), then the right one: "installed
#            on nvme<N>"; on the host (mtools) the ESP holds Limine, the
#            build twice (as the current and the previous build, the stick's
#            files byte for byte) and the stick's boot menu, /data/etc is
#            the stick's (debugfs: the data partition is ext since M13.6's
#            U7b), the ESP's FAT volume is clean (tools/fatcheck.py), the
#            ext volume too once its journal is replayed (e2fsck), and
#            the table is Jam OS's (tools/nvme-written.py). Then a cold boot
#            of that drive alone: the kernel names its GPT disk GUID as the
#            boot disk (from the loader) and its command line is the stick
#            run's, /esp and /data are the drive's (/data ext, bin/extfs),
#            /data/etc/ has the files, the ESP's build is the stick's, the
#            data partition is /jam too (M13.6's DB1), /tmp a view of
#            /jam/tmp and /pkg of /jam/pkg
#   again    `install` over a Jam OS drive: after the first install a file
#            is put on its /data and /etc/probe.txt changed, and on the host
#            the ESP gets an old build, a spare menu and an update's
#            leftovers (.new, .old, .prev); a second stick boot installs
#            again ("a Jam OS disk already"): the ESP is the stick's build
#            again and has none of the leftovers, /data is as it was left
#            (the file, the changed probe.txt: /etc is not copied again),
#            the disk's GUID is the same; the cold boot of the drive reads
#            them
#   cut      the power cut at each change of an install (INSTALL_CUTS, in
#            this order: prepared etcnew etc cleared limine kernel prevkernel
#            bootfs prevbootfs menu, the points of install.c's cut_after):
#            on one drive, a stick boot with the boot word
#            nvmetest=on,instcut<point> stops the install right after that
#            change, the test ends QEMU on init's "cut after" line, and on
#            the host the disk is exactly what that point leaves (the files
#            written so far whole, the later ones not there; no menu before
#            the last point; a whole /etc.new or /etc as the point says; the
#            ESP's FAT volume without damage, the data partition's ext
#            volume clean once its journal is replayed). The points are tried in the order
#            given (each over what the one before left: keep them in the
#            order above). Then a boot of the drive alone, if the last cut
#            left no menu: Limine finds nothing to boot, no kernel starts.
#            Then a stick boot installs ("a Jam OS disk already"), and the
#            drive is a whole install, cold-booted
#   erase    a Windows-like drive (tools/nvme-img.py: four partitions, the PC's
#            layout) is not empty: `install usb0` is refused (the boot disk);
#            `install` shows what is on the drive and asks for its erase
#            after the serial's end: "no" stops (nothing written, init never
#            started an install), `erase` goes on; on the host every block
#            that changed lies in the new table or partitions
#            (tools/nvme-written.py --before), the old ESP's files are not
#            listed, the drive is a whole install, and it cold-boots
#   both     the stick put back beside the installed drive, picked (its
#            boot index first): it boots itself (the kernel names the
#            stick's MBR disk id, /data is the stick's), the drive's ESP is
#            /nvme<N> read-only, its ext data partition /jam, read-write
#            (M13.6's rule for a Jam OS drive that isn't the boot disk), a
#            file written there; every Write in QEMU's trace lies in the
#            data partition, the ESP and the table byte for byte the same;
#            then the drive picked with the stick beside it (its /usb1):
#            /data is the drive's, the file in it
#   damaged  an installed drive whose primary GPT is damaged after (a byte
#            of its primary entry array changed): the driver calls it
#            someone else's, volumes' mark Jam OS's, both volumes mount from
#            the backup table, the data partition read-only (/jam: R3 stage
#            3's question 2); `install` on it shows it as not empty and asks
#            for its erase (R3 stage 3's design question 3), answered `no`
#            twice: nothing stopped, its data partition stays mounted, the
#            image is unchanged
#   nomenu   a reinstall cut after the new kernel (the old boot image made
#            distinct on the ESP first), then the drive picked with a copy
#            of the stick as a second NVMe drive: no kernel starts from the
#            drive's ESP (nothing starts, or the stick's own build)
# INSTALL_CUTS picks the cut points; QEMU_TIMEOUT, QEMU_SMP pass through.
# Usage: tools/install-test.sh <outdir> [case...]; exit 0 on PASS (after
# `make -s image`; the PC only).
set -u
. tools/datafs.sh
out=$1
shift
cases=${*:-install}
[ "$cases" = all ] && cases="install again cut both erase damaged nomenu"
cuts=${INSTALL_CUTS:-prepared etcnew etc cleared limine kernel prevkernel bootfs prevbootfs}
mkdir -p "$out"
tmo=${QEMU_TIMEOUT:-300}
PATH=/usr/sbin:/sbin:$PATH   # e2fsck and debugfs (e2fsprogs: the test PC's WSL has them)

sha() { sha256sum "$1" | cut -d' ' -f1; }

va=$(python3 tools/update-server.py --manifest build/jamos.elf build/bootfs.img |
     sed -n 's/^version //p')
[ -n "$va" ] || { echo "install-test: no version for the build"; exit 1; }

ok=1
fail() {
    echo "install-$c: FAIL: $1"
    ok=0
}

# The stick: the image with the files in /data/etc the install copies, and one
# on /data's root that only the stick has. ref-* are the stick's own files, to
# compare the drive's with.
mk_stick() {
    stick="$dir/stick.img"
    cp build/jamos.img "$stick"
    printf 'install-test probe %s\n' "$$" > "$dir/probe.txt"
    printf 'deep file\n' > "$dir/deep.txt"
    : > "$dir/empty.txt"
    printf 'only on the stick\n' > "$dir/stick-only.txt"
    printf 'changed\n' > "$dir/changed.txt"
    data_put "$stick" "$dir/probe.txt" etc/probe.txt &&
        data_put "$stick" "$dir/deep.txt" etc/sub/deep.txt &&
        data_put "$stick" "$dir/empty.txt" etc/empty.txt &&
        data_put "$stick" "$dir/stick-only.txt" stick-only.txt &&
        data_put "$stick" "$dir/changed.txt" changed.txt &&
        mcopy -o -i "$stick@@1M" ::/EFI/BOOT/BOOTX64.EFI "$dir/ref-limine.efi" &&
        mcopy -o -i "$stick@@1M" ::/boot/jamos.elf "$dir/ref-jamos.elf" &&
        mcopy -o -i "$stick@@1M" ::/boot/bootfs.img "$dir/ref-bootfs.img" ||
        { fail "can't make the stick"; return 1; }
    want_probe="$dir/probe.txt"
}

mk_blank() {
    python3 tools/mkgpt.py "$dir/nvme.img" --layout blank --size 1G > /dev/null ||
        { fail "can't make the blank drive"; return 1; }
}

# The GPT disk GUID of an image, as the kernel prints it.
guid_of() {
    python3 -c 'import sys, uuid; f = open(sys.argv[1], "rb"); f.seek(568);
print(uuid.UUID(bytes_le=f.read(16)))' "$1"
}

# ---- the guest scripts -------------------------------------------------------------------

# inst_in <file> <what the shell says first> <cut point or none>: the stick
# boot that installs on the drive (QEMU's slot 0, serial jamos-nvme0). With
# $wrong set, the wrong end of the serial is typed first. A cut ends QEMU on
# init's line for it. No reboot is typed after: the caller adds the rest.
inst_in() {
    {
        echo "# generated by tools/install-test.sh: $c"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "seen 60 init: /data mounted"
        echo "send sleep 4"
        echo "wait {prompt}"
        echo "send disks"
        printf '%s\n' "seen re:nvme(?P<bd>[0-9]+) +QEMU NVMe Ctrl +serial jamos-nvme0 "
        echo "wait {prompt}"
        if [ "${wrong:-0}" = 1 ]; then
            echo "send install nvme{bd}"
            echo "wait $2"
            echo "wait Type the last 4 characters"
            echo "send zzzz"
            echo "wait is not the end of"
            echo "wait {prompt}"
        fi
        echo "send install nvme{bd}"
        echo "wait $2"
        echo "wait Type the last 4 characters"
        echo "send vme0"
        if [ "$3" != none ]; then
            echo "wait 300 init: install: cut after $3,"
            echo "monitor quit"
        else
            echo "wait 300 installed on nvme{bd}"
            echo "wait {prompt}"
        fi
    } > "$1"
}

# disks_in [ro]: `disks` after the install, the drive's ESP's mount captured
# as {je}, its data partition /jam (ext); `ro`: /jam read-only, its driver
# reading the drive as someone else's (damaged).
disks_in() {
    echo "send disks"
    printf '%s\n' "seen re:serial jamos-nvme0 [^\r\n]*\r?\n +1 +EFI system [^\r\n]*?/nvme(?P<je>[0-9]+)[^\r\n]*\r?\n +2 +Jam OS data [^\r\n]*?/jam \(ext\)"
    [ "${1:-}" = ro ] || echo "wait Jam OS disk"
    echo "wait {prompt}"
}

end_in() {
    echo "send reboot -f"
    echo "wait reboot: resetting"
}

# nvme_in <file> [more]: the drive booted alone: the build, /data/etc and the
# ESP's build; <more> is more guest script lines.
nvme_in() {
    {
        echo "# generated by tools/install-test.sh: $c"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "seen 60 init: /data mounted"
        echo "send version"
        echo "wait Jam OS $va, git"
        echo "wait {prompt}"
        echo "send cat /data/etc/probe.txt"
        echo "wait $(cat "$want_probe")"
        echo "wait {prompt}"
        echo "send cat /data/etc/sub/deep.txt"
        echo "wait deep file"
        echo "wait {prompt}"
        echo "send sha256sum /esp/boot/jamos.elf"
        echo "wait $(sha "$dir/ref-jamos.elf")"
        echo "wait {prompt}"
        echo "send sha256sum /esp/boot/prev-bootfs.img"
        echo "wait $(sha "$dir/ref-bootfs.img")"
        echo "wait {prompt}"
        # /data is /jam too on this boot (M13.6's DB1): /tmp a view of /jam/tmp
        echo "seen 60 init: /jam mounted"
        echo "seen 60 init: /tmp: a view of /jam/tmp"
        echo "seen 60 init: /pkg: a view of /jam/pkg, read-only"
        echo "send T=x"
        echo "wait {prompt}"
        echo "send write /tmp/t.txt in-tmp-\$T && cat /data/tmp/t.txt"
        echo "wait in-tmp-x"
        echo "wait {prompt}"
        echo "send cat /jam/etc/sub/deep.txt"
        echo "wait deep file"
        echo "wait {prompt}"
        [ -z "${2:-}" ] || printf '%s\n' "$2"
        end_in
    } > "$1"
}

# ---- boots ---------------------------------------------------------------------------------

# stick_boot <name> <words> [extra QEMU args]: a fresh copy of the stick with
# the drive (nvme.img) in QEMU's slot 0; the script is $dir/<name>.in.
stick_boot() {
    QEMU_TIMEOUT=$tmo QEMU_NVME="$dir/nvme.img" QEMU_IMAGE="$dir/stick.img" QEMU_WORDS="$2" \
        QEMU_EXTRA="${3:-}" QEMU_INPUT="$dir/$1.in" tools/qemu-test.sh "$dir" "$1" shell > "$dir/$1.out" 2>&1 ||
        { fail "$1: the run failed (see $dir/$1.out)"; grep -a "serial-feed: .*no '" "$dir/$1.out" | head -2; return 1; }
}

# nvme_boot <name> [extra QEMU args]: the drive alone, its own menu.
nvme_boot() {
    QEMU_TIMEOUT=$tmo QEMU_BOOT_NVME=1 QEMU_MENU_AT=keep QEMU_IMAGE="$dir/nvme.img" QEMU_USB="${usb:-}" \
        QEMU_EXTRA="${2:-}" QEMU_INPUT="$dir/$1.in" tools/qemu-test.sh "$dir" "$1" shell > "$dir/$1.out" 2>&1 ||
        { fail "$1: the run failed (see $dir/$1.out)"; grep -a "serial-feed: .*no '" "$dir/$1.out" | head -2; return 1; }
}

# loader_line <log>: the command line the kernel says the loader gave it.
loader_line() {
    tr -d '\r' < "$1" | sed -n 's/^.*loader: *Limine, cmdline "\(.*\)"$/\1/p' | head -1
}

# ---- the host's checks of a drive image ----------------------------------------------------

rank() {
    case $1 in
    prepared) echo 1 ;; etcnew) echo 2 ;; etc) echo 3 ;; cleared) echo 4 ;; limine) echo 5 ;;
    kernel) echo 6 ;; prevkernel) echo 7 ;; bootfs) echo 8 ;; prevbootfs) echo 9 ;;
    menu | full) echo 10 ;;
    *) echo "install-test: no cut point $1" >&2; echo 0 ;;
    esac
}

# in_esp <image> <path> <file>: the drive's ESP has <path>, the bytes of <file>.
in_esp() {
    rm -f "$dir/got"
    mcopy -i "$1@@1M" "::$2" "$dir/got" 2> /dev/null && cmp -s "$dir/got" "$3"
}
no_esp() { ! mdir -i "$1@@1M" "::$2" > /dev/null 2>&1; }

# data_out <image>: its data partition, an ext volume (M13.6's U7b: `install`
# makes it with ext), copied to $dir/data.ext and its journal replayed
# (e2fsck -E journal_only: what was committed is there, nothing else
# fixed); then e2fsck -fn must find it clean. in_data and no_data read that
# copy (debugfs).
data_out() {
    dat=$(python3 tools/mkimage.py --data-at "$1") || { fail "no data partition in $1"; return 1; }
    dd if="$1" of="$dir/data.ext" bs=1M skip="${dat%M}" 2> /dev/null
    e2fsck -E journal_only -y "$dir/data.ext" > "$dir/e2fsck-replay.txt" 2>&1 || [ $? -le 1 ] ||
        { fail "e2fsck's journal replay of the data partition (see $dir/e2fsck-replay.txt)"; return 1; }
    e2fsck -fn "$dir/data.ext" > "$dir/e2fsck.txt" 2>&1 ||
        { fail "the data partition's ext volume isn't clean (see $dir/e2fsck.txt)"; return 1; }
}
# in_data <path> <file>: the data partition (data_out's copy) has <path>,
# the bytes of <file>; no_data <path>: it hasn't.
in_data() {
    debugfs -R "stat $1" "$dir/data.ext" 2> /dev/null | grep -q "^Inode:" || return 1
    debugfs -R "cat $1" "$dir/data.ext" > "$dir/got" 2> /dev/null && cmp -s "$dir/got" "$2"
}
no_data() { ! debugfs -R "stat $1" "$dir/data.ext" 2> /dev/null | grep -q "^Inode:"; }

# fatchk <label> <image@@offset> [--require-clean]: damage fails; lost
# clusters (fatcheck's 2: a cut leaves them, there is no fsck) are said.
fatchk() {
    st=0
    python3 tools/fatcheck.py "$2" ${3:-} > "$dir/fatcheck-$1.txt" 2>&1 || st=$?
    case $st in
    0) ;;
    2) echo "install-$c: $1: $(grep -a 'lost' "$dir/fatcheck-$1.txt" | head -1) (no damage)" ;;
    *) fail "fatcheck of $1: exit $st (see $dir/fatcheck-$1.txt)" ;;
    esac
}

# check_disk <name> <image> <point> <menu file> [--require-clean]: the image as
# a cut after <point> leaves it ("full": a whole install, with the menu).
check_disk() {
    n=$1 img=$2 r=$(rank "$3") conf=$4 clean=${5:-}
    python3 tools/nvme-written.py "$img" --pattern none ${before_img:+--before "$before_img"} > "$dir/written-$n.txt" 2>&1 ||
        fail "$n: $(tail -1 "$dir/written-$n.txt")"
    fatchk "$n-esp" "$img@@1M" $clean
    data_out "$img" || return
    # the ESP: each file whole once its point is past, not there before
    for spec in "5 /EFI/BOOT/BOOTX64.EFI $dir/ref-limine.efi" "6 /boot/jamos.elf $dir/ref-jamos.elf" \
        "7 /boot/prev-jamos.elf $dir/ref-jamos.elf" "8 /boot/bootfs.img $dir/ref-bootfs.img" \
        "9 /boot/prev-bootfs.img $dir/ref-bootfs.img" "10 /boot/limine/limine.conf $conf"; do
        set -- $spec
        if [ "$r" -ge "$1" ]; then
            in_esp "$img" "$2" "$3" || fail "$n: $2 isn't whole on the ESP"
        else
            no_esp "$img" "$2" || fail "$n: $2 is on the ESP before its point"
        fi
    done
    for f in /boot/limine.conf /boot/limine/limine.conf.new /boot/limine/limine.conf.prev \
        /boot/jamos.elf.new /boot/jamos.elf.old /boot/bootfs.img.new /boot/bootfs.img.old /boot/prev-jamos.elf.new \
        /EFI/BOOT/limine.conf /limine.conf; do
        no_esp "$img" "$f" || fail "$n: $f is left on the ESP"
    done
    # /data/etc: copied as /etc.new first, then renamed
    base=
    if [ "$r" -ge 3 ]; then
        base=/etc
        no_data /etc.new || fail "$n: /etc.new is left"
    elif [ "$r" -eq 2 ]; then
        base=/etc.new
        no_data /etc || fail "$n: /etc is there before its rename"
    else
        no_data /etc || fail "$n: /etc is there before its point"
        no_data /etc.new || fail "$n: /etc.new is there before its point"
    fi
    if [ -n "$base" ]; then
        in_data "$base/probe.txt" "$want_probe" || fail "$n: $base/probe.txt isn't the stick's"
        in_data "$base/sub/deep.txt" "$dir/deep.txt" || fail "$n: $base/sub/deep.txt isn't the stick's"
        in_data "$base/empty.txt" "$dir/empty.txt" || fail "$n: $base/empty.txt isn't the stick's (empty)"
    fi
}

# nvme_log_checks <name> <stick log>: the cold boot of the drive says what it should.
nvme_log_checks() {
    log="$dir/$1.log"
    want=$(guid_of "$dir/nvme.img")
    grep -aq "boot disk:   GPT disk GUID $want (from the loader)" "$log" ||
        fail "$1: the kernel didn't name the drive's GUID $want as the boot disk"
    grep -aq "init: hello from ring 3 (.* bootdisk=gpt:$want" "$log" || fail "$1: init was not given bootdisk=gpt:$want"
    grep -aq "volumes: nvme[0-9]* is the boot disk: its ESP is mounted, its data partition too" "$log" ||
        fail "$1: volumes didn't say the drive is the boot disk"
    grep -aq "partition 1 -> bin/fat at /esp, read-only" "$log" || fail "$1: no /esp from the drive"
    grep -aq "partition 2 -> bin/extfs at /data (OK)" "$log" || fail "$1: no ext /data from the drive"
    grep -aq "its data partition is /jam too" "$log" || fail "$1: the drive's data partition isn't /jam too"
    grep -aq "init: /tmp: a view of /jam/tmp" "$log" || fail "$1: /tmp isn't a view of the drive's /jam/tmp"
    got=$(loader_line "$log")
    [ -n "$got" ] && [ "$got" = "$(loader_line "$2")" ] ||
        fail "$1: the command line isn't the stick run's (the installed menu's): '$got'"
}

# installed_ok <name> <stick run name>: after a whole install the host sees
# a whole drive, which then boots alone.
installed_ok() {
    check_disk "$1" "$dir/nvme.img" full "$dir/$2.conf" --require-clean
    nvme_in "$dir/$1-cold.in" "${3:-}"
    nvme_boot "$1-cold" && nvme_log_checks "$1-cold" "$dir/$2.log"
}

finish() {
    if [ "$ok" = 1 ]; then
        rm -f "$dir"/*.img "$dir"/*.vars
        echo "install-$c: PASS"
        return 0
    fi
    mv "$dir/nvme.img" "$dir/nvme.keep" 2> /dev/null   # the drive as the run left it; the copies go
    rm -f "$dir"/*.img "$dir"/*.vars
    echo "install-$c: FAIL (see $dir)"
    fails=$((fails + 1))
    return 1
}

# ---- the cases --------------------------------------------------------------------------

c_install() {
    wrong=1
    inst_in "$dir/inst.in" "nothing is on it" none
    { disks_in; end_in; } >> "$dir/inst.in"
    stick_boot inst "" || return
    [ "$(grep -ac "init: install: step 1/6" "$dir/inst.log")" -eq 1 ] || fail "install ran more or less than once"
    [ "$(grep -ac "that is not the end of" "$dir/inst.log")" -eq 1 ] || fail "the wrong serial end wasn't stopped"
    grep -aq "init: install: .* installed in " "$dir/inst.log" || fail "init didn't log the install as done"
    grep -a "init: install: " "$dir/inst.log" | tr -d '\r' | sed "s/^/install-$c: /" | head -40
    installed_ok inst inst
}

c_again() {
    wrong=0
    inst_in "$dir/a1.in" "nothing is on it" none
    {
        disks_in
        echo "send cp /boot/build.txt /jam/keep.txt; cp /data/changed.txt /jam/etc/probe.txt; sync"
        echo "wait {prompt}"
        end_in
    } >> "$dir/a1.in"
    stick_boot a1 "" || return
    want_probe="$dir/changed.txt"
    check_disk a1 "$dir/nvme.img" full "$dir/a1.conf" --require-clean
    in_data /keep.txt build/build.txt || fail "a1: the file put on the drive's /data isn't there"
    guid1=$(guid_of "$dir/nvme.img")
    # what an older install or an update leaves: another build, a spare menu, leftovers;
    # and menus put by hand where Limine 11 looks first (/EFI/BOOT/limine.conf)
    # or last (/limine.conf): left, the first would be read instead of the
    # installed menu (R3 stage 3's S11)
    printf 'an old build\n' > "$dir/old.txt"
    for f in /boot/jamos.elf /boot/prev-bootfs.img /boot/limine.conf /boot/limine/limine.conf.prev \
        /boot/jamos.elf.new /boot/bootfs.img.old /boot/prev-jamos.elf.new /EFI/BOOT/limine.conf \
        /limine.conf; do
        mcopy -o -i "$dir/nvme.img@@1M" "$dir/old.txt" "::$f" || { fail "can't age the ESP ($f)"; return; }
    done
    inst_in "$dir/a2.in" "a Jam OS disk already" none
    end_in >> "$dir/a2.in"
    stick_boot a2 "" || return
    [ "$(guid_of "$dir/nvme.img")" = "$guid1" ] || fail "the second install changed the disk's GUID"
    grep -aq "init: install: the disk is a Jam OS disk already" "$dir/a2.log" || fail "init didn't find a Jam OS disk"
    grep -aq "init: install: the disk has a /etc already" "$dir/a2.log" || fail "init didn't keep the drive's /etc"
    # /data as it was left: the file on it, and the changed probe.txt
    data_out "$dir/nvme.img" && { in_data /keep.txt build/build.txt || fail "the file on the drive's /data is gone or changed"; }
    installed_ok a2 a2 "send cat /data/keep.txt
wait $(head -1 build/build.txt)
wait {prompt}"
}

c_cut() {
    first=1 last=none wrong=0
    for p in $cuts; do
        what="a Jam OS disk already"
        if [ $first = 1 ]; then
            what="nothing is on it"
            first=0
        fi
        inst_in "$dir/cut-$p.in" "$what" "$p"
        stick_boot "cut-$p" "nvmetest=on,instcut$p" || return
        grep -aq "init: install: cut after $p, " "$dir/cut-$p.log" || fail "cut-$p: init never reached the point"
        grep -aq "installed on nvme" "$dir/cut-$p.log" && fail "cut-$p: the install went on past the cut"
        check_disk "cut-$p" "$dir/nvme.img" "$p" "$dir/cut-$p.conf"
        echo "install-$c: cut after $p: the drive is as that point leaves it"
        last=$p
    done
    if [ "$last" != none ] && [ "$(rank "$last")" -lt 10 ]; then
        # a drive with no menu: nothing boots from it, no kernel starts
        printf '# generated by tools/install-test.sh: %s\nwait 40 Jam OS shell\n' "$c" > "$dir/noboot.in"
        QEMU_TIMEOUT=60 QEMU_BOOT_NVME=1 QEMU_MENU_AT=keep QEMU_IMAGE="$dir/nvme.img" \
            QEMU_INPUT="$dir/noboot.in" tools/qemu-test.sh "$dir" noboot shell > "$dir/noboot.out" 2>&1
        if grep -aq "loader:\|Jam OS shell\|boot disk:" "$dir/noboot.log"; then
            fail "noboot: a kernel started from a drive with no boot menu"
        elif [ "$(rank "$last")" -lt 5 ]; then   # no Limine yet: the firmware finds nothing to start
            grep -aq 'failed to load Boot0001 .*Not Found' "$dir/noboot.log" ||
                fail "noboot: the firmware didn't say the drive has nothing to boot"
        else   # Limine starts and finds no menu
            grep -aq 'starting Boot0001' "$dir/noboot.log" || fail "noboot: the firmware didn't start the drive's Limine"
        fi
        echo "install-$c: the drive cut after $last starts no kernel"
    fi
    inst_in "$dir/fin.in" "$([ $first = 1 ] && echo 'nothing is on it' || echo 'a Jam OS disk already')" none
    end_in >> "$dir/fin.in"
    stick_boot fin "" || return
    installed_ok fin fin
}

c_erase() {
    python3 tools/nvme-img.py "$dir/nvme.img" windows > /dev/null &&
        cp "$dir/nvme.img" "$dir/windows.keep" || { fail "can't make the Windows-like drive"; return; }
    {
        echo "# generated by tools/install-test.sh: $c"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "seen 60 init: /data mounted"
        echo "send sleep 4"
        echo "wait {prompt}"
        echo "send disks"
        printf '%s\n' "seen re:nvme(?P<bd>[0-9]+) +QEMU NVMe Ctrl +serial jamos-nvme0 "
        echo "wait {prompt}"
        # the disk Jam OS booted from is refused
        echo "send install usb0"
        echo "wait is the disk Jam OS booted from"
        echo "wait {prompt}"
        # the drive is shown as not empty; its erase is asked for and refused
        echo "send install nvme{bd}"
        echo "wait It is NOT empty"
        echo "wait a GPT partition table with 4 partitions"
        echo "wait EVERYTHING ON nvme{bd} WILL BE LOST"
        echo "wait Type the last 4 characters"
        echo "send vme0"
        echo "wait type erase"
        echo "send no"
        echo "wait not erased: nothing was written"
        echo "wait {prompt}"
        echo "send install nvme{bd}"
        echo "wait Type the last 4 characters"
        echo "send vme0"
        echo "wait type erase"
        echo "send erase"
        echo "wait 300 installed on nvme{bd}"
        echo "wait {prompt}"
        disks_in
        end_in
    } > "$dir/er.in"
    stick_boot er "" || return
    [ "$(grep -ac "init: install: step 1/6" "$dir/er.log")" -eq 1 ] || fail "install ran more or less than once"
    # every block that changed lies in the new table or partitions; the old files are gone
    before_img="$dir/windows.keep"
    no_esp "$dir/nvme.img" /EFI/Microsoft || fail "er: the old ESP's files are still listed"
    installed_ok er er
    before_img=
}

c_both() {
    wrong=0
    inst_in "$dir/b1.in" "nothing is on it" none
    end_in >> "$dir/b1.in"
    stick_boot b1 "" || return
    check_disk b1 "$dir/nvme.img" full "$dir/b1.conf" --require-clean
    data=$(sed -n 's/.*data \([0-9]*\)+\([0-9]*\);.*/\1-\2/p' "$dir/written-b1.txt")
    esp_before=$(dd if="$dir/nvme.img" bs=1M count=513 2> /dev/null | sha256sum | cut -d' ' -f1)
    # the stick picked, the installed drive beside it
    sid=$(od -An -tx4 -j440 -N4 build/jamos.img | tr -d ' ')
    {
        echo "# generated by tools/install-test.sh: $c"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "seen 60 init: /data mounted"
        echo "send sleep 4"
        echo "wait {prompt}"
        echo "send ls /data"
        echo "wait stick-only.txt"
        echo "wait {prompt}"
        disks_in
        echo "send ls /jam"
        echo "wait etc"
        echo "wait {prompt}"
        echo "send cp /data/stick-only.txt /jam/x.txt; sync; echo wr-\"\"\$?"
        echo "wait wr-0"
        echo "wait {prompt}"
        echo "send write /nvme{je}/x.txt x; echo we-\"\"\$?"
        echo "wait re:we-[1-9]"
        echo "wait {prompt}"
        end_in
    } > "$dir/b2.in"
    stick_boot b2 "" "-trace pci_nvme_io_cmd -trace pci_nvme_write -D $dir/b2.trace" || return
    grep -aq "boot disk:   MBR disk id $sid" "$dir/b2.log" || fail "b2: the kernel didn't name the stick's disk id $sid"
    grep -aqE "partition 1 is /nvme[0-9]+, read-only" "$dir/b2.log" || fail "b2: the drive's ESP isn't read-only"
    grep -aqE "partition 2 is /jam, read-write" "$dir/b2.log" || fail "b2: the drive's data partition isn't /jam, read-write"
    [ "$(dd if="$dir/nvme.img" bs=1M count=513 2> /dev/null | sha256sum | cut -d' ' -f1)" = "$esp_before" ] ||
        fail "b2: the drive's table or ESP changed while the stick booted"
    # every Write in the data partition (b exclusive: start-blocks as nvme-written.py says)
    a=${data%-*} n=${data#*-}
    python3 tools/nvme-trace-check.py --stage 2 --require-io 02 --write-ranges "$a-$((a + n))" "$dir/b2.trace" \
        > "$dir/b2.trace.check" 2>&1 || fail "b2: a Write outside the data partition (see $dir/b2.trace.check)"
    data_out "$dir/nvme.img" && { in_data /x.txt "$dir/stick-only.txt" || fail "b2: the file written to /jam isn't there"; }
    # the drive picked, the stick beside it
    want=$(guid_of "$dir/nvme.img")
    cp "$dir/stick.img" "$dir/stick2.img"
    {
        echo "# generated by tools/install-test.sh: $c"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "seen 60 init: /data mounted"
        echo "send sleep 4"
        echo "wait {prompt}"
        echo "send ls /data"
        echo "wait etc"
        echo "wait x.txt"
        echo "wait {prompt}"
        echo "send ls /usb1"
        echo "wait stick-only.txt"
        echo "wait {prompt}"
        end_in
    } > "$dir/b3.in"
    usb="-drive if=none,id=other,format=raw,file=$dir/stick2.img -device usb-storage,bus=xhci.0,port=2,drive=other"
    nvme_boot b3 || return
    grep -aq "boot disk:   GPT disk GUID $want (from the loader)" "$dir/b3.log" || fail "b3: the kernel didn't name the drive"
    if awk '/ls \/data/{d=1} /ls \/usb1/{d=0} d && /stick-only.txt/{f=1} END{exit !f}' "$dir/b3.log"; then
        fail "b3: /data is the stick's"
    fi
}

# damaged: an installed drive whose primary GPT is damaged later (a byte of
# its primary entry array changed: the array's CRC is wrong, so the backup
# table is read): the driver calls it someone else's and writes nothing,
# while volumes' own mark sees Jam OS's two partitions and both volumes
# mount, the data partition read-only (/jam: R3 stage 3's question 2).
# `install` on it shows it as not empty and asks for its erase (R3 stage 3's
# design question 3, decided yes: before it, S3's refusal in the hold), and
# `no` stops it twice: no install started, nothing held, its data partition
# still mounted; the image is unchanged.
c_damaged() {
    wrong=0
    inst_in "$dir/d1.in" "nothing is on it" none
    end_in >> "$dir/d1.in"
    stick_boot d1 "" || return
    check_disk d1 "$dir/nvme.img" full "$dir/d1.conf" --require-clean
    python3 -c 'import sys; f = open(sys.argv[1], "r+b"); f.seek(1040); b = f.read(1)[0]
f.seek(1040); f.write(bytes([b ^ 0xff]))' "$dir/nvme.img"   # LBA 2 + 16: entry 1's unique GUID
    before=$(sha "$dir/nvme.img")
    {
        echo "# generated by tools/install-test.sh: $c"
        echo "wait 120 Jam OS shell"
        echo "wait {prompt}"
        echo "seen 60 init: /data mounted"
        echo "send sleep 4"
        echo "wait {prompt}"
        echo "send disks"
        printf '%s\n' "seen re:nvme(?P<bd>[0-9]+) +QEMU NVMe Ctrl +serial jamos-nvme0 "
        echo "wait {prompt}"
        disks_in ro
        for i in 1 2; do
            echo "send install nvme{bd}"
            echo "wait It is NOT empty"
            echo "wait mounted read-only at /jam"
            echo "wait Type the last 4 characters"
            echo "send vme0"
            echo "wait type erase"
            echo "send no"
            echo "wait not erased: nothing was written"
            echo "wait {prompt}"
        done
        echo "send ls /jam"
        echo "wait etc"
        echo "wait {prompt}"
        end_in
    } > "$dir/d2.in"
    stick_boot d2 "" || return
    [ "$(sha "$dir/nvme.img")" = "$before" ] || fail "d2: the damaged drive's image changed"
    grep -aq "the drive is read-only" "$dir/d2.log" || fail "d2: the driver didn't call the drive someone else's"
    grep -aqE "partition 2 is /jam, read-only" "$dir/d2.log" || fail "d2: the data partition isn't /jam, read-only"
    grep -aq "is the installer's, read-write" "$dir/d2.log" && fail "d2: a partition was held writable"
    grep -aq "init: install: step 1/6" "$dir/d2.log" && fail "d2: an install was started"
    grep -aq "a Jam OS disk already" "$dir/d2.log" && fail "d2: the damaged drive was installed on as a Jam OS disk"
}

# nomenu: a reinstall cut right after the new kernel is written, the old
# build's boot image (made distinct here) still on the ESP, and then the
# drive picked with a copy of the stick beside it (R3 stage 3's S4). Limine
# 11 has a fallback to "the first volume containing a Limine configuration"
# (its strings): if it took the stick's menu while `boot()` still named the
# drive, the drive's new kernel would start with the old boot image. No
# kernel may start from the drive: either nothing starts (QEMU's OVMF and
# Limine, with the copy on xhci port 2 or as a second NVMe drive as here:
# no kernel started in either) or the stick's own build does, whole.
c_nomenu() {
    wrong=0
    inst_in "$dir/n1.in" "nothing is on it" none
    end_in >> "$dir/n1.in"
    stick_boot n1 "" || return
    printf 'an old boot image\n' > "$dir/old.txt"
    mcopy -o -i "$dir/nvme.img@@1M" "$dir/old.txt" ::/boot/bootfs.img || { fail "can't age the ESP"; return; }
    inst_in "$dir/n2.in" "a Jam OS disk already" kernel
    stick_boot n2 "nvmetest=on,instcutkernel" || return
    grep -aq "init: install: cut after kernel, " "$dir/n2.log" || { fail "n2: init never reached the cut"; return; }
    sid=$(od -An -tx4 -j440 -N4 build/jamos.img | tr -d ' ')
    cp "$dir/stick.img" "$dir/stick2.img"
    printf '# generated by tools/install-test.sh: %s\nwait 60 Jam OS shell\n' "$c" > "$dir/n3.in"
    QEMU_TIMEOUT=90 QEMU_BOOT_NVME=1 QEMU_MENU_AT=keep QEMU_IMAGE="$dir/nvme.img" \
        QEMU_NVME="$dir/stick2.img" QEMU_INPUT="$dir/n3.in" tools/qemu-test.sh "$dir" n3 shell \
        > "$dir/n3.out" 2>&1
    if ! grep -aq "loader:" "$dir/n3.log"; then
        echo "install-$c: n3: no kernel started"
    elif grep -aq "boot disk:   MBR disk id $sid (from the loader)" "$dir/n3.log"; then
        echo "install-$c: n3: Limine read the stick's menu and started the stick's build"
    else
        fail "n3: a kernel started from the cut drive's ESP (its new kernel, the old boot image)"
        grep -a "boot disk:\|panic" "$dir/n3.log" | head -3
    fi
}

fails=0
for c in $cases; do
    dir="$out/$c"
    rm -rf "$dir"
    mkdir -p "$dir"
    ok=1
    case $c in
    install | again | cut | both | erase | damaged | nomenu) ;;
    *) echo "install-test: no case $c"; fails=$((fails + 1)); continue ;;
    esac
    mk_stick && mk_blank && "c_$c"
    finish
done
[ $fails -eq 0 ]
