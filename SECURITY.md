# Security policy

Jam OS is a hobby operating system under active development, run on its
author's own PC. It is not meant for production use or for protecting
anything valuable, and it has no Spectre-class mitigations
([ARCHITECTURE.md](ARCHITECTURE.md) says what it does and doesn't defend).

Its security model is still worth getting right: capabilities, per-program
views of services and storage, and drivers isolated in their own processes.
A way for a program to get something it wasn't granted is a real bug.

## Supported versions

Only the latest commit on `main`. There are no releases yet.

## Reporting a problem

Please report it privately through GitHub's **Report a vulnerability**
button on this repository's Security tab (a private security advisory),
not in a public issue. Include:

- what a program or device can do that it shouldn't,
- the commit you tested,
- how to reproduce it (QEMU is fine; say if it needs the real hardware).

You can expect a first answer within a week. Fixes land on `main` with a
test that fails without them, and the advisory is published once the fix
is in.
