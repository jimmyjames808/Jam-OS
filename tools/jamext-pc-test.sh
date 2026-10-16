#!/bin/bash
# jamext's round trips (tools/jamext-test.sh, which `make check` runs on the
# Mac) on a machine with e2fsprogs, where they also have Linux's e2fsck -fn
# call every image clean and Linux's debugfs read the files, links and modes
# jamext wrote (the test PC's WSL):
#
#     JAMOS_REMOTE=jamos-pc JAMOS_REMOTE_ONLY=1 \
#         tools/remote-test.sh tools/jamext-pc-test.sh build/t/jamext
#
# Into <outdir>: the test's scratch files. Exit 0: PASS; 2 without e2fsck.
set -u
out=${1:?usage: tools/jamext-pc-test.sh <outdir>}
export PATH=/usr/sbin:/sbin:$PATH LC_ALL=C
for t in e2fsck debugfs; do
    command -v $t > /dev/null || { echo "jamext-pc-test: no $t here (e2fsprogs)"; exit 2; }
done
make -s build/host/jamext build/host/extcheck build/extfix/jam4k.img build/extfix/linux1k.img \
    build/extfix/old2k.img || exit 1
if sh tools/jamext-test.sh build/host/jamext build/host/extcheck "$out" build/extfix; then
    echo "jamext-pc-test: PASS"
else
    echo "jamext-pc-test: FAIL"
    exit 1
fi
