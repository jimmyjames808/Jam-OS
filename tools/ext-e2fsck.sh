#!/bin/bash
# Linux's e2fsck -fn on every volume extcheck's writers leave
# (tools/extwrite.c and extfile.c, `extcheck --save`), and debugfs's view
# of the files they made: the independent check docs/M13.6-PLAN.md's U2a
# asks for, on a machine with e2fsprogs (the test PC's WSL):
#
#     JAMOS_REMOTE=jamos-pc JAMOS_REMOTE_ONLY=1 \
#         tools/remote-test.sh tools/ext-e2fsck.sh build/ext-e2fsck
#
# Into <out>: extcheck.log; <image>.fsck, e2fsck's output for each image;
# <image>.debugfs, `ls -l /emptydir` and a `stat` of each test file there.
# The images themselves stay behind (16 and 32 MiB each). Exit 0 when
# e2fsck calls every image clean; 1 when one isn't (its name printed); 2
# without e2fsck.
set -u
out=${1:?usage: tools/ext-e2fsck.sh <outdir>}
export PATH=/usr/sbin:/sbin:$PATH LC_ALL=C
command -v e2fsck > /dev/null || { echo "ext-e2fsck: no e2fsck here (e2fsprogs)"; exit 2; }
mkdir -p "$out"
imgs=$(mktemp -d)
trap 'rm -rf "$imgs"' EXIT
fixtures=()
for gz in tools/extfix/*.img.gz; do
    fixtures+=("build/extfix/$(basename "$gz" .gz)")
done
make -s build/host/extcheck "${fixtures[@]}" || exit 1
if ! build/host/extcheck tools/extfix build/extfix --fuzz 0 --save "$imgs" > "$out/extcheck.log" 2>&1; then
    cat "$out/extcheck.log"
    exit 1
fi
bad=0
for img in "$imgs"/*.img; do
    n=$(basename "$img" .img)
    if e2fsck -fn "$img" > "$out/$n.fsck" 2>&1; then
        echo "$n: clean"
    else
        echo "$n: e2fsck exit $? (see $n.fsck)"
        bad=1
    fi
    {
        debugfs -R "ls -l /emptydir" "$img"
        for f in depth0 depth1 depth2 depth3 depth2r sparse6g maxsize random0 random1 random2; do
            debugfs -R "stat /emptydir/$f" "$img" 2> /dev/null | grep -v "^$"
        done
    } > "$out/$n.debugfs" 2>&1
done
exit $bad
