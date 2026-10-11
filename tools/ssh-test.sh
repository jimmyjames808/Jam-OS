#!/bin/sh
# The SSH server end to end in QEMU (docs/M13-PLAN.md section 15, "X2
# notes"): bin/sshd (the tinyssh port) in the guest, OpenSSH's client on
# the test host (tools/sshcheck.py) through the network peer's TCP relay
# (tools/netpeer.py --tcp-forward: a host port to the guest's 22), on VLAN
# 21. Throwaway keys only: ssh-keygen makes a key to let in and a wrong one
# here, every run; the first goes into the stick's
# /data/etc/authorized_keys (with an ssh-rsa line, which must not count).
#   ssh        the first boot (tools/shell-tests/ssh.txt): init makes the host
#              key and starts sshd; sshcheck.py logs in for a command and
#              its exit status, an interactive session (TERM, the size, a
#              window change, Ctrl+C, `exit 7`), checks the host key the
#              client saw against the log, has a wrong key, a password and
#              another user refused, 8 sessions at once and a 9th refused, 4
#              sessions' output at once, a session killed mid-output, then
#              nothing left of them, and the server still there
#   ssh-again  a second boot of the same stick (ssh-again.txt): the same host
#              key (the first boot's known_hosts, strictly) and no new one
# PASS needs both boots' scripts, sshcheck.py's PASS twice and the runs'
# VLAN checks (tools/qemu-test.sh). Needs ssh and ssh-keygen on the host.
# Usage: tools/ssh-test.sh <outdir>
set -eu
out=$1
mkdir -p "$out"
img="$out/ssh.base.img"
saved="$out/ssh.saved.img"
cp "${QEMU_IMAGE:-build/jamos.img}" "$img"
printf 'net.address = 10.2.21.5/24 10.2.21.1 10.2.21.1\n' > "$out/ssh.settings"
rm -f "$out/key" "$out/key.pub" "$out/wrong" "$out/wrong.pub" "$out/known_hosts" "$saved"
ssh-keygen -q -t ed25519 -N '' -C jamos-ssh-test -f "$out/key"
ssh-keygen -q -t ed25519 -N '' -C jamos-ssh-test-wrong -f "$out/wrong"
{
    echo "# tools/ssh-test.sh's throwaway key"
    echo "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQC1 not-a-key"
    cat "$out/key.pub"
} > "$out/authorized_keys"
python3 -c "import sys; sys.stdout.write(''.join('%07d the same line of text, again\n' % i for i in range(120000)))" \
    > "$out/big.txt"   # about 4 MiB, for the session killed mid-output
mmd -i "$img@@64M" ::/etc 2>/dev/null || true
mcopy -o -i "$img@@64M" "$out/ssh.settings" ::/etc/settings
mcopy -o -i "$img@@64M" "$out/authorized_keys" ::/etc/authorized_keys
mcopy -o -i "$img@@64M" "$out/big.txt" ::/big.txt
ok=1
check() {   # <name> <script> <image> [sshcheck flags]
    name=$1 script=$2 image=$3
    shift 3
    port=$(python3 tools/netpeer.py --free-ports 1)
    rm -f "$out/$name.log"
    python3 tools/sshcheck.py --log "$out/$name.log" --port "$port" --key "$out/key" \
        --wrong "$out/wrong" --known "$out/known_hosts" --out "$out" "$@" \
        > "$out/$name.check.log" 2>&1 &
    cpid=$!
    QEMU_IMAGE="$image" QEMU_NET=1 QEMU_TIMEOUT=${QEMU_TIMEOUT:-900} \
        QEMU_NET_PEER="--tcp-forward $port:10.2.21.5:22" \
        QEMU_INPUT="$script" tools/qemu-test.sh "$out" "$name" shell || ok=0
    wait $cpid || ok=0
    sed "s/^/ssh-test: $name: /" "$out/$name.check.log"
}
export QEMU_SAVE="$saved"   # the first boot's stick, with its host key, for the second
check ssh tools/shell-tests/ssh.txt "$img"
unset QEMU_SAVE
rm -f "$img"
if [ -f "$saved" ]; then
    check ssh-again tools/shell-tests/ssh-again.txt "$saved" --again "$out/ssh.log"
else
    echo "ssh-test: no saved stick from the first boot: no second boot"
    ok=0
fi
rm -f "$saved"
grep -a "init: SSH host key\|sshd: " "$out/ssh.log" | tr -d '\r' |
    sed 's/^.*\(init: SSH\|sshd: \)/ssh-test: guest: \1/' | head -40 || true
if [ $ok = 1 ]; then
    echo "ssh-test: PASS"
else
    echo "ssh-test: FAIL"
    exit 1
fi
