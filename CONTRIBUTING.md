# Contributing to Jam OS

Jam OS is a personal project: a from-scratch x86_64 capability operating
system, built and tested on one real PC ([docs/HARDWARE.md](docs/HARDWARE.md)).
Issues are welcome: bug reports, questions, ideas. Pull requests are
welcome too, but talk first in an issue for anything bigger than a small
fix, because the design moves quickly and follows a plan
([docs/ROADMAP.md](docs/ROADMAP.md)).

## Before you start

- Read [ARCHITECTURE.md](ARCHITECTURE.md) for the design and its rules, and
  [CODING-GUIDE.md](CODING-GUIDE.md) for how the code is written. Its
  sections "Contributing" and "Before you commit" are the checklist every
  change follows.
- [README.md](README.md) says how to build, and
  [docs/TESTING.md](docs/TESTING.md) how to run the tests in QEMU.

## What a change needs

- `make`, `make KTESTS=0` and `make check` pass with no warnings.
- The tests for the area you touched pass in QEMU (`make -s image` first),
  at 4 and 8 CPUs for kernel code.
- A bug fix comes with a test that failed before the fix.
- Refactors and behaviour changes are separate commits; commit messages
  read `area: what changed`.
- Docs change in the same commit as the code that changes the fact they
  state.
- Outside code goes only in `third_party/`, with its licence, and only
  under a licence compatible with BSD-2-Clause (no GPL or LGPL).

## Licence

Jam OS is under the BSD 2-Clause licence ([LICENSE](LICENSE)). By sending a
change you agree it is distributed under the same licence.

## Security

Please don't report security problems in public issues: see
[SECURITY.md](SECURITY.md).

## Conduct

Everyone taking part follows the [code of conduct](CODE_OF_CONDUCT.md).
