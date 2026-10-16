# The stick's /data in an image file, for the test scripts that put files there before a boot
# or read them back after it (docs/M13.6-PLAN.md, "Ext by default"). Sourced:
#
#     . tools/datafs.sh
#
# Run from the checkout's top, as the scripts are. The image is a copy of build/jamos.img (or
# a stick QEMU_SAVE kept): its data partition is ext, made with jamext (the default since
# U11b), or FAT when it was built with JAMOS_DATA_FS=fat (tools/mkimage.py --data-fs says
# which), and each function does what its name says on either one: jamext for ext, mtools for
# FAT. The ESP (partition 1, always FAT) is the scripts' own mtools business, as before.
#
#   data_fs  <image>                       ext or fat
#   data_put <image> <file> <path> [mode]  /data/<path> gets the host file's bytes (the
#                                          directories above it are made; mode, octal, for
#                                          ext only: a program needs 755)
#   data_get <image> <path> <file>         the file /data/<path> copied out; false if it
#                                          isn't there
#   data_cat <image> <path>                the file /data/<path> on stdout; false if not there
#   data_has <image> <path>                is there a file or directory /data/<path>?
#   data_ls  <image> <dir>                 the names in the directory /data/<dir>, one to a
#                                          line (a directory's without a slash); false if
#                                          there is no such directory
#   data_check <image>                     is the volume whole (ext: jamext's check; FAT: its label)
#   data_fat_image <image>                 a twin of build/jamos.img with a FAT data partition,
#                                          for the scripts that test FAT itself
#   data_grow <image> <mib>               the data partition grown to <mib> MiB and an empty
#                                          volume made on it (the image's kind)
#   data_mtime <image> <path>             when it was last written: "2026-01-14 14:02"
#                                          (ext keeps UTC, FAT the guest's local time)
#   data_mkdir <image> <dir>              the directory /data/<dir> made, with its parents
#                                          (an empty directory, which data_put never makes)
#   data_tree  <image> <dir>               everything under /data/<dir>, as the tool lists it
#                                          (for grep: the names are as stored)
#
# Paths are relative to /data, without a leading slash ("etc/settings"). A read-back looks at
# a stick the guest has written: jamext replays its journal first (--replay, on the image
# file, which is a copy the test throws away), as a mount would. A function that can't do
# what it was asked returns false (a script under `set -e` stops).

data_fs() {
    _dfs=$(python3 tools/mkimage.py --data-fs "$1") || return 1
    if [ "$_dfs" = gpt ]; then   # an installed drive's table doesn't say; an ext volume has a superblock
        if data_jamext info "$1" > /dev/null 2>&1; then _dfs=ext; else _dfs=fat; fi
    fi
    echo "$_dfs"
}

# data_jamext <jamext arguments...>: build/host/jamext on the data partition (partition 2).
data_jamext() {
    [ -x build/host/jamext ] || make -s build/host/jamext 1>&2 || return 1
    build/host/jamext -q --part 2 "$@"
}

# data_mtools <tool> <image> <arguments...>: an mtools command on the data partition.
data_mtools() {
    _dm_tool=$1 _dm_img=$2
    shift 2
    _dm_at=$(python3 tools/mkimage.py --data-at "$_dm_img") || return 1
    "$_dm_tool" -i "$_dm_img@@$_dm_at" "$@"
}

# data_fat_dirs <image> <dir>: mmd for each level of /data/<dir> (mtools has no -p).
data_fat_dirs() {
    printf '%s\n' "$2" | tr / '\n' | {
        _df_dir=
        while IFS= read -r _df_part; do
            [ -n "$_df_part" ] && [ "$_df_part" != . ] || continue
            _df_dir="$_df_dir/$_df_part"
            data_mtools mmd "$1" "::$_df_dir" 2> /dev/null || true
        done
    }
}

data_put() {
    if [ "$(data_fs "$1")" = ext ]; then
        data_jamext put ${4:+-m "$4"} "$1" "$2" "/$3"
    else
        data_fat_dirs "$1" "$(dirname "$3")"
        data_mtools mcopy "$1" -o "$2" "::/$3"
    fi
}

data_get() {
    if [ "$(data_fs "$1")" = ext ]; then
        data_jamext --replay get "$1" "/$2" "$3" 2> /dev/null
    else
        data_mtools mcopy "$1" -o "::/$2" "$3" 2> /dev/null
    fi
}

data_cat() {
    if [ "$(data_fs "$1")" = ext ]; then
        data_jamext --replay get "$1" "/$2" - 2> /dev/null
    else
        data_mtools mtype "$1" "::/$2" 2> /dev/null
    fi
}

data_has() {
    if [ "$(data_fs "$1")" = ext ]; then
        data_jamext --replay ls "$1" "/$2" > /dev/null 2>&1
    else
        data_mtools mdir "$1" "::/$2" > /dev/null 2>&1
    fi
}

data_ls() {
    if [ "$(data_fs "$1")" = ext ]; then
        _dl=$(data_jamext --replay ls "$1" "/$2" 2> /dev/null) || return 1
    else
        _dl=$(data_mtools mdir "$1" -b "::/$2" 2> /dev/null) || return 1
    fi
    [ -z "$_dl" ] || printf '%s\n' "$_dl" | sed -e 's|/$||' -e 's|.*/||'
}

# data_check <image>: the data volume is whole after a run: jamext's check (its journal replayed
# first, as the next mount would) for ext, the volume label's check for FAT.
data_check() {
    if [ "$(data_fs "$1")" = ext ]; then
        data_jamext --replay check "$1" > /dev/null 2>&1
    else
        _dc_at=$(python3 tools/mkimage.py --data-at "$1") || return 1
        python3 tools/fat-label.py "$1" "${_dc_at%M}" > /dev/null 2>&1
    fi
}

# data_fat_image <image>: build/jamos.img's twin with a FAT32 data partition (`make image
# DATA_FS=fat IMAGE=<image>`, the build's other products already made), for the scripts that are
# about FAT: the fat service, its restarts and storms, FAT as a data volume. They boot it with
# QEMU_IMAGE. Quiet; the make's output is <image>.make.log.
data_fat_image() {
    make -s image DATA_FS=fat IMAGE="$1" > "$1.make.log" 2>&1 ||
        { echo "datafs: can't make the FAT image $1 (see $1.make.log)" >&2; return 1; }
    rm -f "$1.make.log"
}

# data_grow <image> <mib>: the data partition made <mib> MiB (the image file grown to hold it;
# tools/mbr-grow.py, as tools/write-usb.sh grows a stick's) and an empty volume of the image's
# own kind made on it: jamext's mkfs for ext, mformat for FAT. The files in it are gone.
data_grow() {
    _dg_fs=$(data_fs "$1") || return 1
    _dg_at=$(python3 tools/mkimage.py --data-at "$1") || return 1
    _dg_at=${_dg_at%M}
    python3 -c "import sys; open(sys.argv[1], 'r+b').truncate(int(sys.argv[2]) << 20)" "$1" \
        $((_dg_at + $2)) &&
        python3 tools/mbr-grow.py "$1" $(((_dg_at + $2) * 2048)) > /dev/null || return 1
    if [ "$_dg_fs" = ext ]; then
        data_jamext mkfs --label JAMOS-DATA "$1" "$2M"
    else
        data_mtools mformat "$1" -T $(($2 * 2048)) -F -v JAMOS-DATA :: &&
            python3 tools/fat-label.py "$1" "$_dg_at" --fix > /dev/null
    fi
}

data_mtime() {   # data_mtime <image> <path>: "YYYY-MM-DD H:MM", UTC on ext, FAT's local stamp on FAT
    if [ "$(data_fs "$1")" = ext ]; then
        data_jamext --replay ls -l "$1" "/$2" 2> /dev/null | awk 'NR == 1 { print $6, $7 }'
    else
        data_mtools mdir "$1" "::/$2" 2> /dev/null |
            sed -n 's/.*\(20[0-9][0-9]-[0-9][0-9]-[0-9][0-9]\) *\([0-9]*:[0-9][0-9]\).*/\1 \2/p' | head -1
    fi
}

data_mkdir() {
    if [ "$(data_fs "$1")" = ext ]; then
        data_jamext mkdir -p "$1" "/$2"
    else
        data_fat_dirs "$1" "$2"
        data_mtools mdir "$1" "::/$2" > /dev/null 2>&1
    fi
}

data_tree() {
    if [ "$(data_fs "$1")" = ext ]; then
        data_jamext --replay ls -R "$1" "/$2" 2> /dev/null
    else
        data_mtools mdir "$1" -/ "::/$2" 2> /dev/null
    fi
}
