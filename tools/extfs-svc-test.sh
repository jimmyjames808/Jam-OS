#!/bin/sh
# The extfs service in QEMU (tools/shell-tests/extfs-svc.txt): utest's
# extfs cases from the shell, then every RAM disk they saved on /data
# (sparse, user/tests/utest/extfs.c's extfs_save) made an image again and
# checked by Linux's e2fsck -fn: what extfs leaves must be a volume Linux
# calls clean. Needs e2fsprogs and mtools (the test PC's WSL):
#
#     JAMOS_REMOTE=jamos-pc JAMOS_REMOTE_ONLY=1 \
#         tools/remote-test.sh tools/extfs-svc-test.sh build/test/extfs-svc
#
# Into <outdir>: the run's log, <name>.img for each disk, <name>.fsck.
# Exit 0 when the run passed and e2fsck called every disk clean.
set -u
out=${1:?usage: tools/extfs-svc-test.sh <outdir>}
export PATH=/usr/sbin:/sbin:$PATH LC_ALL=C
command -v e2fsck > /dev/null || { echo "extfs-svc-test: no e2fsck here (e2fsprogs)"; exit 2; }
mkdir -p "$out"
DATA_AT=$(python3 tools/mkimage.py --data-at build/jamos.img) || exit 1
ok=0
QEMU_TIMEOUT=${QEMU_TIMEOUT:-1500} QEMU_SAVE="$out/extfs-svc.after.img" \
    QEMU_INPUT=tools/shell-tests/extfs-svc.txt tools/qemu-test.sh "$out" extfs-svc shell || ok=1
grep -E "utest: (extfs|[0-9]+ passed)|FAILED" "$out/extfs-svc.log" | head -40
n=0
for f in $(mdir -b -i "$out/extfs-svc.after.img@@$DATA_AT" ::/extfs-e2fsck 2>/dev/null); do
    name=$(basename "$f" .sparse)
    mcopy -o -i "$out/extfs-svc.after.img@@$DATA_AT" "::/extfs-e2fsck/$name.sparse" \
        "$out/$name.sparse" || { ok=1; continue; }
    python3 - "$out/$name.sparse" "$out/$name.img" <<'EOF' || { ok=1; continue; }
import struct, sys
src = open(sys.argv[1], "rb").read()
if src[:8] != b"jamspars":
    sys.exit("not a sparse disk")
size = struct.unpack_from("<Q", src, 8)[0]
with open(sys.argv[2], "wb") as dst:
    dst.truncate(size)
    at = 16
    while at < len(src):
        off = struct.unpack_from("<Q", src, at)[0]
        dst.seek(off)
        dst.write(src[at + 8:at + 8 + 4096])
        at += 8 + 4096
EOF
    n=$((n + 1))
    if e2fsck -fn "$out/$name.img" > "$out/$name.fsck" 2>&1; then
        echo "extfs-svc-test: $name: e2fsck: clean"
    else
        echo "extfs-svc-test: $name: e2fsck exit $? (see $name.fsck)"
        ok=1
    fi
done
[ "$n" -gt 0 ] || { echo "extfs-svc-test: no disk was saved"; ok=1; }
[ $ok = 0 ] && echo "extfs-svc-test: PASS ($n disks clean)" || echo "extfs-svc-test: FAIL"
exit $ok
