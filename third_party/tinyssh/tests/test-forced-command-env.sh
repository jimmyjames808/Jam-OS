#!/bin/sh

LANG=C
export LANG
LC_ALL=C
export LC_ALL

if test "$(id -u)" = 0; then
  echo 'test-forced-command-env: must run as a non-root user' >&2
  exit 111
fi

user=$(id -un) || exit 111
accountshell=${SHELL-}
if command -v getent >/dev/null 2>&1; then
  passwdline=$(getent passwd "$user") || exit 111
  accountshell=${passwdline##*:}
fi

case $accountshell in
  */bash) ;;
  *)
    echo 'test-forced-command-env: account login shell must be bash' >&2
    exit 111
    ;;
esac

ssh=${SSH-ssh}
if ! command -v "$ssh" >/dev/null 2>&1; then
  echo 'test-forced-command-env: OpenSSH client not found' >&2
  exit 111
fi

testdir=$(pwd) || exit 111
tmpdir=$(mktemp -d "${TMPDIR-/tmp}/tinyssh-env.XXXXXXXXXX") || exit 111
trap 'rm -rf "$tmpdir"' EXIT HUP INT TERM

keydir=$tmpdir/keydir
./tinysshd-makekey "$keydir" >/dev/null 2>&1 || exit 111

proxycommand="'$testdir/tinysshnoneauthd' -q -e 'echo FORCED_COMMAND_ONLY' '$keydir'"

control=$(
  "$ssh" -F /dev/null -T \
    -o "ProxyCommand=$proxycommand" \
    -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null \
    -o PreferredAuthentications=none \
    -o PubkeyAuthentication=no \
    -o PasswordAuthentication=no \
    -o KbdInteractiveAuthentication=no \
    -o LogLevel=ERROR \
    "$user@tinyssh-env.invalid" </dev/null
) || exit 111

if test "$control" != FORCED_COMMAND_ONLY; then
  echo "test-forced-command-env: unexpected control output: $control" >&2
  exit 111
fi

attack=$(
  printf 'echo CLIENT_INPUT_EXECUTED\nexit 0\n' | \
    "$ssh" -F /dev/null -T \
      -o "ProxyCommand=$proxycommand" \
      -o StrictHostKeyChecking=no \
      -o UserKnownHostsFile=/dev/null \
      -o PreferredAuthentications=none \
      -o PubkeyAuthentication=no \
      -o PasswordAuthentication=no \
      -o KbdInteractiveAuthentication=no \
      -o SetEnv=BASH_ENV=/dev/stdin \
      -o LogLevel=ERROR \
      "$user@tinyssh-env.invalid"
) || exit 111

if test "$attack" != FORCED_COMMAND_ONLY; then
  echo "test-forced-command-env: forced command bypassed: $attack" >&2
  exit 111
fi

echo 'test-forced-command-env: ok'
