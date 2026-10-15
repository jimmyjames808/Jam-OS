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
#            the stick's, both FAT volumes are clean (tools/fatcheck.py) and
#            the table is Jam OS's (tools/nvme-written.py). Then a cold boot
#            of that drive alone: the kernel names its GPT disk GUID as the
#            boot disk (from the loader) and its command line is the stick
#            run's, /esp and /data are the drive's, /data/etc/ has the
#            files, the ESP's build is the stick's
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
#            FAT volumes without damage). The points are tried in the order
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
#            stick's MBR disk id, /data is the stick's), the drive is
#            /nvme<N> read-only, a write there is refused, and the drive's
#            image is byte for byte the same after the run with no Write in
#            QEMU's trace; then the drive picked with the stick beside it
#            (its /usb1): /data is the drive's
# INSTALL_CUTS picks the cut points; QEMU_TIMEOUT, QEMU_SMP pass through.
# Usage: tools/install-test.sh <outdir> [case...]; exit 0 on PASS (after
# `make -s image`; the PC only).
set -u
out=$1
shift
cases=${*:-install}
[ "$cases" = all ] && cases="install again cut both erase"
cuts=${INSTALL_CUTS:-prepared etcnew etc cleared limine kernel prevkernel bootfs prevbootfs}
mkdir -p "$out"
tmo=${QEMU_TIMEOUT:-300}

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
    sat=$(python3 tools/mkimage.py --data-at "$stick") || return 1
    printf 'install-test probe %s\n' "$$" > "$dir/probe.txt"
    printf 'deep file\n' > "$dir/deep.txt"
    : > "$dir/empty.txt"
    printf 'only on the stick\n' > "$dir/stick-only.txt"
    printf 'changed\n' > "$dir/changed.txt"
    mmd -i "$stick@@$sat" ::/etc 2> /dev/null
    mmd -i "$stick@@$sat" ::/etc/sub 2> /dev/null
    mcopy -o -i "$stick@@$sat" "$dir/probe.txt" ::/etc/probe.txt &&
        mcopy -o -i "$stick@@$sat" "$dir/deep.txt" ::/etc/sub/deep.txt &&
        mcopy -o -i "$stick@@$sat" "$dir/empty.txt" ::/etc/empty.txt &&
        mcopy -o -i "$stick@@$sat" "$dir/stick-only.txt" ::/stick-only.txt &&
        mcopy -o -i "$stick@@$sat" "$dir/changed.txt" ::/changed.txt &&
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

# disks_in: `disks` after the install, with the drive's two partitions' mounts
# captured as {je} (ESP) and {jm} (data).
disks_in() {
    echo "send disks"
    printf '%s\n' "seen re:serial jamos-nvme0 [^\r\n]*\r?\n +1 +EFI system [^\r\n]*?/nvme(?P<je>[0-9]+)[^\r\n]*\r?\n +2 +Jam OS data [^\r\n]*?/nvme(?P<jm>[0-9]+)"
    echo "wait Jam OS disk"
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
# in_data <image> <path> <file>: the same on the data partition.
in_data() {
    rm -f "$dir/got"
    mcopy -i "$1@@$(python3 tools/mkimage.py --data-at "$1")" "::$2" "$dir/got" 2> /dev/null && cmp -s "$dir/got" "$3"
}
no_esp() { ! mdir -i "$1@@1M" "::$2" > /dev/null 2>&1; }
no_data() { ! mdir -i "$1@@$(python3 tools/mkimage.py --data-at "$1")" "::$2" > /dev/null 2>&1; }

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
    dat=$(python3 tools/mkimage.py --data-at "$img") || { fail "$n: no data partition"; return; }
    python3 tools/nvme-written.py "$img" --pattern none ${before_img:+--before "$before_img"} > "$dir/written-$n.txt" 2>&1 ||
        fail "$n: $(tail -1 "$dir/written-$n.txt")"
    fatchk "$n-esp" "$img@@1M" $clean
    fatchk "$n-data" "$img@@$dat" $clean
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
        /boot/jamos.elf.new /boot/jamos.elf.old /boot/bootfs.img.new /boot/bootfs.img.old /boot/prev-jamos.elf.new; do
        no_esp "$img" "$f" || fail "$n: $f is left on the ESP"
    done
    # /data/etc: copied as /etc.new first, then renamed
    base=
    if [ "$r" -ge 3 ]; then
        base=/etc
        no_data "$img" /etc.new || fail "$n: /etc.new is left"
    elif [ "$r" -eq 2 ]; then
        base=/etc.new
        no_data "$img" /etc || fail "$n: /etc is there before its rename"
    else
        no_data "$img" /etc || fail "$n: /etc is there before its point"
        no_data "$img" /etc.new || fail "$n: /etc.new is there before its point"
    fi
    if [ -n "$base" ]; then
        in_data "$img" "$base/probe.txt" "$want_probe" || fail "$n: $base/probe.txt isn't the stick's"
        in_data "$img" "$base/sub/deep.txt" "$dir/deep.txt" || fail "$n: $base/sub/deep.txt isn't the stick's"
        in_data "$img" "$base/empty.txt" "$dir/empty.txt" || fail "$n: $base/empty.txt isn't the stick's (empty)"
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
    grep -aq "partition 2 -> bin/fat at /data (OK)" "$log" || fail "$1: no /data from the drive"
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
        echo "send mount -w /nvme{jm}"
        echo "wait 60 is now read-write"
        echo "wait {prompt}"
        echo "send cp /boot/build.txt /nvme{jm}/keep.txt; cp /data/changed.txt /nvme{jm}/etc/probe.txt; sync"
        echo "wait {prompt}"
        echo "send mount -r /nvme{jm}"
        echo "wait 60 is now read-only"
        echo "wait {prompt}"
        end_in
    } >> "$dir/a1.in"
    stick_boot a1 "" || return
    want_probe="$dir/changed.txt"
    in_data "$dir/nvme.img" /keep.txt build/build.txt || fail "a1: the file put on the drive's /data isn't there"
    check_disk a1 "$dir/nvme.img" full "$dir/a1.conf" --require-clean
    guid1=$(guid_of "$dir/nvme.img")
    # what an older install or an update leaves: another build, a spare menu, leftovers
    printf 'an old build\n' > "$dir/old.txt"
    for f in /boot/jamos.elf /boot/prev-bootfs.img /boot/limine.conf /boot/limine/limine.conf.prev \
        /boot/jamos.elf.new /boot/bootfs.img.old /boot/prev-jamos.elf.new; do
        mcopy -o -i "$dir/nvme.img@@1M" "$dir/old.txt" "::$f" || { fail "can't age the ESP ($f)"; return; }
    done
    inst_in "$dir/a2.in" "a Jam OS disk already" none
    end_in >> "$dir/a2.in"
    stick_boot a2 "" || return
    [ "$(guid_of "$dir/nvme.img")" = "$guid1" ] || fail "the second install changed the disk's GUID"
    grep -aq "init: install: the disk is a Jam OS disk already" "$dir/a2.log" || fail "init didn't find a Jam OS disk"
    grep -aq "init: install: the disk has a /etc already" "$dir/a2.log" || fail "init didn't keep the drive's /etc"
    # /data as it was left: the file on it, and the changed probe.txt
    in_data "$dir/nvme.img" /keep.txt build/build.txt || fail "the file on the drive's /data is gone or changed"
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
    before=$(sha "$dir/nvme.img")
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
        echo "send ls /nvme{jm}"
        echo "wait etc"
        echo "wait {prompt}"
        echo "send cp /data/stick-only.txt /nvme{jm}/x.txt; echo wr-\"\"\$?"
        echo "wait re:wr-[1-9]"
        echo "wait {prompt}"
        end_in
    } > "$dir/b2.in"
    stick_boot b2 "" "-trace pci_nvme_io_cmd -trace pci_nvme_write -D $dir/b2.trace" || return
    grep -aq "boot disk:   MBR disk id $sid" "$dir/b2.log" || fail "b2: the kernel didn't name the stick's disk id $sid"
    grep -aqE "partition 1 is /nvme[0-9]+, read-only" "$dir/b2.log" || fail "b2: the drive's ESP isn't read-only"
    grep -aqE "partition 2 is /nvme[0-9]+, read-only" "$dir/b2.log" || fail "b2: the drive's data partition isn't read-only"
    [ "$(sha "$dir/nvme.img")" = "$before" ] || fail "b2: the drive's image changed while the stick booted"
    grep -aq '^pci_nvme_io_cmd ' "$dir/b2.trace" 2> /dev/null || fail "b2: the trace is dead (no command seen)"
    [ "$(grep -ac '^pci_nvme_write ' "$dir/b2.trace" 2> /dev/null)" = 0 ] || fail "b2: a Write reached the drive"
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

fails=0
for c in $cases; do
    dir="$out/$c"
    rm -rf "$dir"
    mkdir -p "$dir"
    ok=1
    case $c in
    install | again | cut | both | erase) ;;
    *) echo "install-test: no case $c"; fails=$((fails + 1)); continue ;;
    esac
    mk_stick && mk_blank && "c_$c"
    finish
done
[ $fails -eq 0 ]
