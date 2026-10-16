#!/bin/sh
# tools/jamext-test.sh <jamext> <extcheck> <work dir> [<fixtures dir>]
#
# jamext (tools/jamext) round trips, on the Mac or any Unix, no QEMU:
#   - mkimage from a directory with every kind of name (files over a
#     megabyte, empty, a sparse 5 MiB hole, symbolic links of both kinds,
#     hard links, modes, a 255-byte name, hundreds of names in one
#     directory): every file comes back byte for byte (get), the listing
#     shows the links and modes, the same --seed and --time make the same
#     bytes, and the volume passes `jamext check`, extcheck --image (the
#     sanitised library) and, where Linux's e2fsck is installed, e2fsck -fn;
#   - put, get, ls, ln, mkdir, rm, chmod on an image, the volume checked
#     after each kind of change;
#   - a volume at an offset in a bigger file, and --part N of an MBR;
#   - what must fail: a missing name, a name in the way, no space, a file
#     that is not a volume, a check that finds a spoilt volume;
#   - with a fixtures dir (build/extfix: the volumes Linux's mke2fs made):
#     `jamext check` calls each of them clean;
#   - where Linux's debugfs is installed, it reads the files and links
#     jamext wrote the same way (a file dumped, a link count, a mode).
set -eu

abs() { (cd "$(dirname "$1")" && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }
JX=$(abs "$1")
EC=$(abs "$2")
W=$3
FIX=${4:-}
[ -z "$FIX" ] || FIX=$(cd "$FIX" && pwd)

rm -rf "$W"
mkdir -p "$W/src/sub/deeper" "$W/src/many" "$W/out"
cd "$W"

fail() {
    echo "jamext-test: FAIL: $*" >&2
    exit 1
}

# ---- the tree ---------------------------------------------------------------------------

printf 'hello\n' > src/hello.txt
: > src/empty
dd if=/dev/urandom of=src/rand.bin bs=1024 count=1500 2>/dev/null
dd if=/dev/urandom of=src/sub/four.bin bs=4096 count=1 2>/dev/null
dd if=/dev/urandom of=src/sub/four1.bin bs=4097 count=1 2>/dev/null
dd if=/dev/urandom of=src/sub/deeper/small.bin bs=100 count=1 2>/dev/null
# 4 bytes, a 5 MiB hole, 4 bytes
printf 'head' > src/sparse.bin
dd if=/dev/zero of=src/sparse.bin bs=1 count=0 seek=5242880 2>/dev/null
printf 'tail' | dd of=src/sparse.bin bs=1 seek=5242880 conv=notrunc 2>/dev/null
printf '#!/bin/sh\necho hi\n' > src/exec.sh
chmod 755 src/exec.sh
ln -s hello.txt src/link-rel
ln -s /an/absolute/target src/link-abs
ln -s sub src/dirlink
ln -s 0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789 src/link-long
ln src/hello.txt src/hard1
ln src/rand.bin src/sub/hard2
LONG=$(printf 'n%.0s' $(seq 1 255))
printf 'long name\n' > "src/$LONG"
printf 'utf8\n' > "src/caf$(printf '\303\251').txt"
i=1
while [ $i -le 400 ]; do
    echo "$i" > "src/many/f$i"
    i=$((i + 1))
done

# ---- an image from the directory ---------------------------------------------------------

"$JX" --seed 7 --time 1700000000 -q mkimage img1 auto src || fail "mkimage auto"
"$JX" check img1 || fail "check after mkimage"
"$EC" --image img1 || fail "extcheck --image after mkimage"
if command -v e2fsck >/dev/null 2>&1; then
    e2fsck -fn img1 >/dev/null || fail "e2fsck -fn after mkimage"
fi

if command -v debugfs >/dev/null 2>&1; then
    debugfs -R "stat /hello.txt" img1 2>/dev/null | grep -q 'Links: 2' || fail "debugfs: hello.txt's links"
    debugfs -R "stat /exec.sh" img1 2>/dev/null | grep -q 'Mode:  0755' || fail "debugfs: exec.sh's mode"
    debugfs -R "stat /link-long" img1 2>/dev/null | grep -q 'Type: symlink' || fail "debugfs: link-long"
    debugfs -R "dump /rand.bin out/dbg-rand.bin" img1 2>/dev/null
    cmp -s src/rand.bin out/dbg-rand.bin || fail "debugfs reads rand.bin differently"
    debugfs -R "dump /sparse.bin out/dbg-sparse.bin" img1 2>/dev/null
    cmp -s src/sparse.bin out/dbg-sparse.bin || fail "debugfs reads sparse.bin differently"
    echo "jamext-test: debugfs reads the same files, links and modes"
fi

# every file back, byte for byte
(cd src && find . -type f | sort) > files.txt
n=0
while IFS= read -r f; do
    f=${f#./}
    mkdir -p "out/$(dirname "$f")"
    "$JX" get img1 "/$f" "out/$f" || fail "get $f"
    cmp -s "src/$f" "out/$f" || fail "$f differs after get"
    n=$((n + 1))
done < files.txt
echo "jamext-test: $n files back byte for byte"

# the listing: links, modes, owners
"$JX" ls -l img1 / > ls-root.txt
grep -Eq '^lrwxrwxrwx +1 +1000 +1000 +[0-9]+ [-0-9: ]+ link-rel -> hello.txt$' ls-root.txt ||
    fail "link-rel in the listing"
grep -Eq ' link-abs -> /an/absolute/target$' ls-root.txt || fail "link-abs in the listing"
grep -Eq ' link-long -> 0123456789' ls-root.txt || fail "link-long in the listing"
grep -Eq '^-rw-r--r-- +2 +1000 +1000 +6 .* hello.txt$' ls-root.txt || fail "hello.txt has two links"
grep -Eq '^-rw-r--r-- +2 +1000 +1000 +6 .* hard1$' ls-root.txt || fail "hard1 has two links"
grep -Eq '^-rwxr-xr-x +1 +1000 +1000 .* exec.sh$' ls-root.txt || fail "exec.sh is 0755"
grep -Eq '^-rw-r--r-- +1 +1000 +1000 +5242884 .* sparse.bin$' ls-root.txt || fail "sparse.bin's size"
"$JX" ls img1 /many | wc -l | grep -q '^ *400$' || fail "400 names in /many"
# a link followed in the middle of a path
"$JX" get img1 /dirlink/four.bin out/via-dirlink.bin || fail "get through dirlink"
cmp -s src/sub/four.bin out/via-dirlink.bin || fail "through a directory link"
"$JX" get img1 /link-rel out/via-link.txt || fail "get a link"
cmp -s src/hello.txt out/via-link.txt || fail "get follows a link"

# the same bytes from the same inputs
"$JX" --seed 7 --time 1700000000 -q mkimage img1b auto src || fail "second mkimage"
cmp -s img1 img1b || fail "two images from the same directory, seed and time differ"

# ---- changes on an image ------------------------------------------------------------------

"$JX" -q mkfs --seed 3 --time 1700000000 img2 16M || fail "mkfs"
"$JX" check img2 >/dev/null || fail "check an empty volume"
"$JX" put img2 src/hello.txt /dir/a/hello.txt || fail "put makes the parents"
"$JX" put -m 600 img2 src/rand.bin /dir/rand.bin || fail "put -m"
"$JX" ls -l img2 /dir | grep -Eq '^-rw------- +1 .* rand.bin$' || fail "put -m 600"
"$JX" get img2 /dir/rand.bin out/r2.bin || fail "get rand"
cmp -s src/rand.bin out/r2.bin || fail "put/get rand"
"$JX" put img2 src/empty /dir/rand.bin || fail "put replaces a file"
"$JX" ls -l img2 /dir | grep -Eq '^-rw-r--r-- +1 +1000 +1000 +0 .* rand.bin$' || fail "replaced by an empty file"
"$JX" check img2 || fail "check after put"
"$JX" ln -s img2 hello.txt /dir/a/sym || fail "ln -s"
"$JX" ln img2 /dir/a/hello.txt /dir/hard || fail "ln"
"$JX" ls -l img2 /dir | grep -Eq '^-rw-r--r-- +2 .* hard$' || fail "hard link count"
"$JX" get img2 /dir/a/sym out/sym.txt || fail "get a symbolic link"
cmp -s src/hello.txt out/sym.txt || fail "symbolic link read"
"$JX" mkdir -p img2 /x/y/z || fail "mkdir -p"
"$JX" mkdir img2 /x/y/z 2>/dev/null && fail "mkdir on an existing name" || :
"$JX" chmod img2 700 /x/y || fail "chmod"
"$JX" ls -l img2 /x | grep -Eq '^drwx------ +3 .* y$' || fail "chmod on a directory"
"$JX" check img2 || fail "check after ln, mkdir, chmod"
"$EC" --image img2 || fail "extcheck --image after the changes"
"$JX" rm img2 /dir/hard || fail "rm"
"$JX" rm img2 /dir 2>/dev/null && fail "rm of a directory without -r" || :
"$JX" rm -r img2 /dir || fail "rm -r"
"$JX" get img2 /dir/a/hello.txt out/gone 2>/dev/null && fail "get of a removed name" || :
"$JX" check img2 || fail "check after rm -r"
"$EC" --image img2 || fail "extcheck --image after rm -r"
if command -v e2fsck >/dev/null 2>&1; then
    e2fsck -fn img2 >/dev/null || fail "e2fsck -fn after the changes"
fi

# a file bigger than the volume's free space: refused, the volume still whole
"$JX" -q mkfs --seed 4 --time 1700000000 img3 16M || fail "mkfs 16M"
"$JX" put img3 src/sparse.bin /s1 || fail "put sparse (holes take no space)"
"$JX" put img3 out/rand.bin /r1 || fail "put 1.5 MiB"
dd if=/dev/urandom of=big.bin bs=1048576 count=12 2>/dev/null
"$JX" put img3 big.bin /big 2>/dev/null && fail "put over the free space" || :
"$JX" check img3 || fail "check after a put that failed for space"
"$JX" ls img3 / | grep -q '^big' && fail "a half file was left behind" || :
"$JX" check img3 | grep -Eq ' [0-9]{4} of 4096 blocks free' || fail "the space was not given back"

# ---- a volume inside a bigger file ---------------------------------------------------------

dd if=/dev/zero of=disk bs=1048576 count=0 seek=80 2>/dev/null
"$JX" -q mkfs --seed 5 --time 1700000000 --offset 1048576 --size 32M disk || fail "mkfs at an offset"
"$JX" put --offset 1048576 --size 32M disk src/hello.txt /h || fail "put at an offset"
"$JX" get --offset 1048576 --size 32M disk /h out/h.txt || fail "get at an offset"
cmp -s src/hello.txt out/h.txt || fail "round trip at an offset"
"$JX" check --offset 1048576 --size 32M disk || fail "check at an offset"
# the first megabyte was not touched
HEAD=$(dd if=disk bs=1048576 count=1 2>/dev/null | od -An -v -tx1 | tr -d ' \n' | tr -d '0')
[ -z "$HEAD" ] || fail "mkfs at an offset wrote before the volume"
# an MBR with one Linux partition at sector 2048, 32 MiB
printf '\200\000\000\000\203\000\000\000\000\010\000\000\000\000\001\000' |
    dd of=disk bs=1 seek=446 conv=notrunc 2>/dev/null
printf '\125\252' | dd of=disk bs=1 seek=510 conv=notrunc 2>/dev/null
"$JX" get --part 1 disk /h out/h2.txt || fail "get --part 1"
cmp -s src/hello.txt out/h2.txt || fail "round trip --part 1"
"$JX" put --part 1 disk src/exec.sh /bin/x || fail "put --part 1"
"$JX" check --part 1 disk || fail "check --part 1"
"$JX" get --part 2 disk /h out/none 2>/dev/null && fail "--part 2 of a one-partition table" || :

# ---- what must fail -------------------------------------------------------------------------

"$JX" get img2 /no/such/file out/none 2>/dev/null && fail "get of a missing file" || :
"$JX" check src/hello.txt >/dev/null 2>&1 && fail "check of a file that is not a volume" || :
"$JX" put img2 src/hello.txt /x/y/z/../.. 2>/dev/null && fail "put onto a directory" || :
# a spoilt volume: flip a byte in the first group's inode bitmap region and look for a problem
cp img1 spoilt
# the root directory's inode: find it by listing (its link count is at a fixed place);
# simpler: zero 4 KiB of block 1..3 (group descriptors and bitmaps) so something is wrong
dd if=/dev/zero of=spoilt bs=4096 seek=1 count=2 conv=notrunc 2>/dev/null
"$JX" check spoilt >/dev/null 2>&1 && fail "check of a volume with its descriptors zeroed" || :

if [ -n "$FIX" ]; then
    for f in "$FIX"/*.img; do
        "$JX" -q check "$f" || fail "check of $f (made by Linux)"
    done
fi

echo "jamext-test: ok"
