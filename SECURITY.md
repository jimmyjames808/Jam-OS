# Security policy

Jam OS is a hobby operating system under active development, run on its
author's own PC. It is not meant for production use or for protecting
anything valuable, and it has no Spectre-class mitigations
([ARCHITECTURE.md](ARCHITECTURE.md#what-jam-os-defends-against) says what
it does and doesn't defend against).

Its security model is still worth getting right: capabilities and
per-program views of services and storage. A way for a program to get
something it wasn't granted is a real bug. Drivers run in processes of
their own, so a crashing driver can't take the kernel down. With the
IOMMU on (the default since the real PC signed it off on 2026-10-07: M11
in the roadmap) a driver is also contained: its device reaches only the
memory the driver pinned and raises only its own interrupts, so a
device's DMA or interrupt outside that is a real bug too. On a boot with
`iommu=off` (the "Developer > Jam OS (no IOMMU)" boot entry) a driver can
program its device to read or write any memory, so drivers, and
usb-bus's parsing of what USB devices send it, are trusted there.

## Known limits

What is deliberately not defended yet, so a report about it is a known
limit and not a new bug:

- **JIT memory is the one exception to "never writable and executable".**
  Every mapping obeys that rule except a JIT VMO, which only a program
  whose list says `right jit` (approved by the owner with `allow`, or by
  its package) can make: private to it, never file-backed, never counted
  as approved code. Today that is Chromium's renderers and, only when
  WebGL runs in software, its GPU process. A program without the power
  getting a writable and executable page, or a JIT VMO becoming approved
  code, is a real bug. Two things are open, to be closed with the sandbox
  (below): the browser process holds the power too, and can pass it on, and
  a process that gave the power up (`PR_SET_MDWE`) does not pass that on
  across `/svc/exec`.
- **Chromium has no sandbox of Jam OS's own yet** (CR-f is not built; its
  plan is being written). It runs `--no-sandbox`, so a bug in a renderer
  gives an attacker what the browser's list allows: the network, `/data`
  and the desktop. Use it for ordinary sites only. The review of the layers
  the sandbox will stand on lists what it must close
  ([docs/CR-REVIEW-LAYERS.md](docs/CR-REVIEW-LAYERS.md)).
- **WebGL in software (SwiftShader) is opt-in**: the browser starts it only
  with `--enable-unsafe-swiftshader`, which a page can't turn on, and it
  gives the JIT power to the GPU process only then.
- **Anyone holding the stick or the drive** can change `/data/etc`
  (the settings, the SSH keys) and the logs: nothing on it is checked at
  boot, and only `update`'s builds are signed.
- **SSH** is key login only (ed25519, no passwords) and is off with no key
  in `/data/etc/authorized_keys`; the shell it opens has everything the
  shell has.
- **No Spectre-class mitigations** and no protection of the machine
  against a hostile device on the boot with `iommu=off`
  ([ARCHITECTURE.md](ARCHITECTURE.md#what-jam-os-defends-against)).

## Supported versions

Only the latest commit on `main`. The `v0.1.0` tag marks an early state
and is not supported; nothing else has been released.

## Reporting a problem

Please report it privately through GitHub's **Report a vulnerability**
button on this repository's Security tab (a private security advisory),
not in a public issue. Include:

- what a program or device can do that it shouldn't,
- the commit you tested, and whether the boot had `iommu=on`,
- how to reproduce it (QEMU is fine; say if it needs the real hardware).

You can expect a first answer within a week. Fixes land on `main` with a
test that fails without them, and the advisory is published once the fix
is in.
