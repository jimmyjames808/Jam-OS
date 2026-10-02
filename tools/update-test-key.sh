#!/bin/sh
# For the update tests (tools/update-test.sh, update-net-test.sh,
# update-write-test.sh): throwaway update keys and a stick whose build has
# the first one's public half, so the tests never depend on (or touch) the
# owner's key in ~/.config/jamos.
#   tools/update-test-key.sh <outdir> <in.img> <out.img> [nokey]
# Makes <outdir>/testkey/key1 and key2 (build/host/jamos-sign keygen; once:
# kept for later runs), and <out.img>: a copy of <in.img> whose ESP holds
# <in.img>'s boot image with key1's update.pub in it (with `nokey`: with
# none). That boot image is also left as <outdir>/testkey/bootfs-key.img
# (bootfs-nokey.img), the base of a test's build B. Exit 0 if all went well.
set -eu
out=$1 in=$2 img=$3 mode=${4:-key}
keys="$out/testkey"
mkdir -p "$keys"
for k in key1 key2; do
    [ -s "$keys/$k/update.key" ] || build/host/jamos-sign keygen "$keys/$k" > /dev/null
done
rm -f "$keys/bootfs-in.img"
mcopy -i "$in@@1M" ::/boot/bootfs.img "$keys/bootfs-in.img"
pub="$keys/key1/update.pub"
[ "$mode" = nokey ] && pub=
python3 tools/bootfs-edit.py "$keys/bootfs-in.img" "$keys/bootfs-$mode.img" "update.pub=$pub"
cp "$in" "$img"
mcopy -o -i "$img@@1M" "$keys/bootfs-$mode.img" ::/boot/bootfs.img
rm -f "$keys/bootfs-in.img"
