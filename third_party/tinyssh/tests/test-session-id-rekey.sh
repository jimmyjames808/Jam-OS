#!/bin/sh

LANG=C
export LANG
LC_ALL=C
export LC_ALL

if test "$(id -u)" = 0; then
    echo 'test-session-id-rekey: must run as a non-root user' >&2
    exit 111
fi

ssh=${SSH-ssh}
if ! command -v "$ssh" >/dev/null 2>&1; then
    echo 'test-session-id-rekey: OpenSSH client not found' >&2
    exit 111
fi

for kex in sntrup761x25519-sha512@openssh.com curve25519-sha256; do
    if ! "$ssh" -Q kex 2>/dev/null | grep -Fx "$kex" >/dev/null; then
        echo "test-session-id-rekey: OpenSSH client lacks $kex" >&2
        exit 111
    fi
done

user=$(id -un) || exit 111
testdir=$(pwd) || exit 111
tmpdir=$(mktemp -d "${TMPDIR-/tmp}/tinyssh-session-id.XXXXXXXXXX") ||
    exit 111
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

keydir=$tmpdir/keydir
./tinysshd-makekey "$keydir" >/dev/null 2>&1 || exit 111

run_rekey() {
    initial_hash=$1
    proxycommand="env TINYSSH_TEST_INITIAL_HASH=$initial_hash '$testdir/_tinysshd-test-session-id' -q -e 'dd if=/dev/zero bs=65536 count=4 2>/dev/null' '$keydir'"

    "$ssh" -F /dev/null -T \
        -o "ProxyCommand=$proxycommand" \
        -o KexAlgorithms=sntrup761x25519-sha512@openssh.com,curve25519-sha256 \
        -o RekeyLimit=16K \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o PreferredAuthentications=none \
        -o PubkeyAuthentication=no \
        -o PasswordAuthentication=no \
        -o KbdInteractiveAuthentication=no \
        -o LogLevel=ERROR \
        "$user@tinyssh-session-id.invalid" </dev/null >/dev/null
}

status=0
if ! run_rekey 512; then
    echo 'test-session-id-rekey: SHA-512 -> SHA-256 failed' >&2
    status=1
fi
if ! run_rekey 256; then
    echo 'test-session-id-rekey: SHA-256 -> SHA-512 failed' >&2
    status=1
fi
test "$status" -eq 0 || exit 111

echo 'test-session-id-rekey: ok'
