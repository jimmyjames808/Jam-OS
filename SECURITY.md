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
IOMMU on (the boot word `iommu=on`, the "Jam OS (IOMMU)" boot entry) a
driver is also contained: its device reaches only the memory the driver
pinned and raises only its own interrupts, so a device's DMA or interrupt
outside that is a real bug too. **The IOMMU is off by default** until it
has been signed off on the real PC (M11 in the roadmap); on a default
boot a driver can program its device to read or write any memory, so
drivers, and usb-bus's parsing of what USB devices send it, are trusted.

## Supported versions

Only the latest commit on `main`. There are no releases yet.

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
