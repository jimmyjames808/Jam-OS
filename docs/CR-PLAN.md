# CR plan: Chromium

Status (2026-10-11): a plan (CR0), nothing built. 2026-10-13: CT1, the clang
toolchain and CR-a's C++ half, built ([CT1 as built](#ct1-as-built-2026-10-13)).
2026-10-14: CT2, CR-a's Rust half and the pinned gperf, built ([CT2 as
built](#ct2-as-built-2026-10-14)). 2026-10-15: CB1, the Chromium build,
most of the way ([CB1 as built](#cb1-as-built-2026-10-14): the port, its
patches, the kept tree; the compile not yet finished). Written against main
d28b8b52 with [G2's rescope](G2-PLAN.md#the-chromium-rescope-2026-10-11)
on top (M13's sockets, sessions, ptys, tinyssh and pipelines merged;
M12.5's PK1 to PK4, PU and PF1 merged; M13.6's U1 merged; M13.7's PO1,
PO2, PO3 and PO8 merged; M12.7's stage 2 built on its own branch; G2's
GC1 merged and GP1 running). Its input is [CR-RESEARCH](CR-RESEARCH.md),
an hour's reading of Chromium's source; every claim here that depends on
Jam OS's code was checked against the code, and the places where the code
says something the research didn't are listed in
[Where CR starts](#where-cr-starts). Most of the owner's choices are
made ([Questions](#questions-for-the-owner)); four small questions are
open, each with a recommendation the plan assumes until he answers.

Goal ([roadmap](ROADMAP.md#now), the owner, 2026-10-11): **Chromium runs
on Jam OS**, built from its own source and run directly (no wrapper),
reached layer by layer. **Every layer is general**: a piece of Jam OS that
any later port uses the same way (no Chromium-only shortcut, nothing that
can't generalise), and only the layers Chromium needs are built now.
The route is Chromium's **Linux** code (`target_os="linux"`) against Jam
OS's musl sysroot, the way Alpine ships it, with its Ozone/Wayland
backend over Jam OS's compositor, rendering in software.

**Done when** (CR-d, the first browser; [the milestones](#milestones)
make each step concrete):
1. **`chrome` browses real sites on the PC**: started from the shell (and
   the search box if the owner wants it), it opens a window on the
   desktop, loads ordinary sites over HTTPS (news, Wikipedia, a search
   engine, GitHub), with tabs, the omnibox and its suggestions, menus,
   the keyboard and mouse, the clipboard both ways with Jam OS terminals,
   downloads to `/jam`, and history and settings kept across a restart.
2. **It is a plain open build** of Alpine's stable Chromium (155 today):
   no H.264 or AAC, no Widevine, no Google API keys; JavaScript runs
   `--jitless` until CR-e; no sandbox of Chromium's own until CR-f (the
   owner's rule: ordinary sites only meanwhile, not banking or logins
   that matter).
3. **Every layer it stands on is general and tested on its own**: each
   has its own tests (ktest, utest, posixtest, a port's test) that don't
   need Chromium, so a regression shows in the layer's suite first.
4. **It hurts only itself**: Chromium killed, hung, or out of memory
   leaves the desktop, the other windows and the network as they were; a
   renderer killed shows Chromium's "Aw, Snap!" page and the browser
   lives.
5. It is a package (M13.7), signed, installed with `pkg`, its licences
   beside it and its complete source (the tarball, Alpine's patches, ours,
   the recipe) served beside it.
6. As every milestone: an independent review-and-fix of each layer, then
   All tests and `soak 10` on the PC with `chrome` open, and the PC
   check by hand ([The PC check](#the-pc-check)).

After CR-d, in order: the JIT by capability (CR-e), Jam OS's own sandbox
for Chromium's processes (CR-f), sound (if not already in CR-d), then the
GPU after G3 and G4.

## What CR is not

- **Not the Fuchsia route.** No Zircon call library, no fdio, no FIDL
  services ([CR-RESEARCH section 1](CR-RESEARCH.md#1-the-linux-route-or-the-fuchsia-route)).
  One Fuchsia file is borrowed, as Alpine does: the stub DNS
  configuration service.
- **No Chromium-only layers.** Each new piece of Jam OS is named for what
  it does (sparse reservations, descriptor passing, a launcher call, JIT
  memory), not for Chromium, and is usable by any port. What is truly
  Chromium's (its toolchain file, its launcher branch, an audio back end
  in its tree) is a patch in Chromium's recipe, kept small and written to
  be offered upstream.
- **No dynamic linking** (M14.5). Software compositing loads nothing
  with `dlopen`; ANGLE, SwiftShader, VA-API, PulseAudio, GTK and Qt are
  off ([CR-RESEARCH section 5](CR-RESEARCH.md#5-dynamic-loading-in-a-static-non-component-build)).
- **No GPU, no WebGL** until G3 and G4. No SwiftShader (it is no longer a
  WebGL fallback anyway).
- **No CA store, no NSS, no D-Bus.** Chromium on Linux verifies TLS with
  its compiled-in root store; D-Bus and NSS are off by GN arguments.
- **No real `fork`.** No zygote (`--no-zygote`); children start through
  `posix_spawn` (section 5). M13.5's emulated `fork` and `vfork` stay off
  the path.
- **No X11, no Xwayland.**
- **No building on Jam OS.** Chromium is cross-built on the PC's WSL
  (the owner's choice); M15 is not on the path.
- **No codecs beyond the open build's** (VP8, VP9, AV1, Opus, Vorbis,
  FLAC, WebM, Ogg); no Widevine; no Google API keys (no sync, no Safe
  Browsing).

## Where CR starts

| What | Today | Where |
|---|---|---|
| The compiler | `tools/jamos-cc`: x86_64-elf-gcc 15.2, C only, static, no `__linux__` defined; no C++ runtime for the target, no clang, no Rust | `tools/jamos-cc` |
| Programs | static ELF, `ET_EXEC` only, linked at 0x400000 by `user/posix/posix.ld`, at most 8 loadable segments; W^X everywhere | `kernel/include/jam/elf.h`, `user/lib/spawn.c` |
| Program size | **64 MiB at most**: the shell's `PROGRAM_MAX` (a `/data` program read whole), libos's `FILE_MAX`, and `VMO_VERIFIED_MAX` for M12.5's paged programs (**1 GiB since CP1**: see "CP1 notes") | `user/services/shell/sh_allow.c`, `user/lib/spawn.c`, `kernel/include/jam/abi.h` |
| Programs from `/data` | the shell reads the whole file, seals it and runs it (approved by `allow`); M12.5's PX (paged, each page checked against `allow`'s table) to come | [M12.5-PLAN](M12.5-PLAN.md#programs-from-data) |
| `posix_spawn` | runs boot-image programs only; a program on a file system is EACCES until M13's F1b | `user/posix/sys/spawn.c` |
| The startup message | argv and the environment in one message of at most 2,048 bytes and 128 entries | `user/lib/spawn.c` (`MSG_MAX`) |
| Memory | `mmap` makes one VMO per mapping; a paged VMO is at most **64 GiB** (a fixed three-level table); execute on anonymous memory refused (EACCES); `MAP_FIXED` over libc's own mappings by `VMAR_REPLACE`; `madvise(MADV_DONTNEED)` decommits | `user/posix/sys/mem.c`, `kernel/object/vmo.c` |
| The address space | one flat VMAR; mappings in a **sorted list** (O(n) find), at most **16,384**, **never merged**; a VMO's user mappings (its reverse map) a list too | `kernel/mm/aspace.c`, `kernel/object/vmo.c` |
| Sockets | `AF_INET` over netstack; **`AF_UNIX` is `socketpair` only** (`SOCK_STREAM` and, since CU1, `SOCK_SEQPACKET`), a channel underneath; since CU1 a socketpair carries descriptors (`SCM_RIGHTS`) and `shutdown(SHUT_RDWR)` closes its end | `user/posix/sys/pipe.c`, `user/posix/sys/sockpair.c`, `user/posix/sys/sockio.c`, `user/posix/sys/sockconn.c` |
| netstack's limits | 32 openers, **16 sockets an opener and 48 in all**, 64 TCP connections an opener and 256 in all | `user/include/net.h` |
| memfd | a kept VMO, committed as it grows; seals kept by libc **per process** | `user/posix/sys/memfd.c` |
| Raw `syscall()` | answered: musl's `syscall()` goes through the layer's whole table (`__jam_calls_all`) | [M13-REVIEW-POSIX](M13-REVIEW-POSIX.md#program-size) |
| `/proc` | none | |
| Floating point | the kernel saves x87, SSE and AVX state (XCR0); no AVX-512 on the PC anyway; PKU's register is not enabled | `kernel/arch/x86_64/fpu.c` |
| Wayland | the compositor speaks Wayland over channels; G2's shim, subsurfaces and popups to come | [G2-PLAN](G2-PLAN.md) |
| Storage | FAT32 `/data` on the stick (768 MiB on the owner's); M13.6's ext filesystem (`/jam`) and M12.7's writes to the Jam OS drive to come | [M13.6-PLAN](M13.6-PLAN.md), [M12.7-PLAN](M12.7-PLAN.md) |
| Ports | M13.7's builder, packages and signing on the Mac; `pkg` on Jam OS to come; ports build on the Mac and on the PC's WSL | [M13.7-PLAN](M13.7-PLAN.md) |
| The PC's WSL | Ubuntu, 28 threads, the cross compiler and QEMU; every test runs there through `tools/remote-test.sh` (a budget of 26 vCPUs) | [TESTING](TESTING.md) |

**What the code says that the research didn't** (each shapes this plan):

1. **Programs are capped at 64 MiB** in three places (above), and the
   loader's limit for a program read from a file is the same. Chromium's
   static binary is 150 to 250 MB. A track raises all three (CP1, done:
   1 GiB), and PX's page table for such a program is 2 MB (32 bytes a
   page).
2. **netstack's real cap is 48 sockets in all**, not only 16 an opener:
   one browser tab with a few dozen connections would starve every other
   program. CN1 raises both.
3. **A Chromium child's command line doesn't fit the startup message.**
   A renderer's switches and `--enable-features` lists run to several
   KB; the message holds 2,048 bytes and 128 entries. CS1 carries big
   argv and environments in a VMO.
4. **Every `mmap` is a VMO of its own, and mappings never merge.** V8
   and PartitionAlloc reserve once and then commit and decommit pieces
   by `mprotect` and `MAP_FIXED` thousands of times; today each such call
   splits a mapping, and each `MAP_FIXED` makes a new VMO. With a list of
   at most 16,384 mappings and a fault that walks it, that is both a
   limit and a slowdown. The design (section 2) keeps one VMO per
   reservation and makes the kernel merge.
5. **`posix_spawn` can't run a program from a file system yet** (M13's
   F1b), and nothing lets a program start itself again cheaply. Chromium
   starts every child by exec'ing `/proc/self/exe`. Section 6's
   self-image answers both for Chromium's case.
6. **The loader takes `ET_EXEC` only.** Chromium links position-independent
   executables by default; its Jam OS toolchain file links non-PIE
   (section 1). Static-PIE (and with it address randomisation for every
   program) is a later general track.
7. **A socketpair end can't be shut down** (no half-close on a channel),
   and Mojo calls `shutdown(SHUT_RDWR)`: CU1 makes the full shutdown
   close the channel for every descriptor of that socket.
8. **`jamos-cc` doesn't define `__linux__`; a clang targeting
   `x86_64-unknown-linux-musl` does**, and Chromium's code and Linux's
   headers need it. Section 1 says which driver does what.

## How others do it

| System | Chromium there | Borrow | Leave |
|---|---|---|---|
| **Alpine** | the Linux build on musl: `is_musl=true` (copium's shared patch), `use_sysroot=false`, system clang 23 through the "unbundle" toolchain, `use_custom_libcxx=true`, about 10 small musl patches and 4 for the seccomp sandbox; `chrome`, `headless_shell`, chromedriver; stable 155.0.8059.39 today, from the source tarballs without `.git` | the patches, the GN args, the source tarball, the order (`content_shell` and `headless_shell` are small targets) | unbundling system libraries (we build Chromium's own copies), PulseAudio, Qt, VA-API, PipeWire |
| **Fuchsia** | Chromium's Fuchsia port: launching through `fdio_spawn_etc`, Mojo on Zircon channels, Flatland, a per-type sandbox policy that says which services and directories a child's namespace gets | the sandbox's shape (section 13): per process type, what the child is given at launch | the platform underneath (FIDL services) |
| **FreeBSD, OpenBSD** | the ports carry hundreds of patches (`is_bsd`), with their own sandbox (Capsicum, `pledge` and `unveil`) | that a non-Linux OS can carry Chromium release after release; `pledge`/`unveil` as a per-type policy applied at launch | the patch count: they target `target_os="freebsd"` and patch everywhere; we stay `linux` and patch little |
| **Genode** | Falkon on QtWebEngine (Chromium's engine inside Qt) runs on a capability microkernel, with its own POSIX layer and its own GUI session | the proof that Chromium's engine runs on a capability OS with a POSIX layer underneath | QtWebEngine and Qt: a second big port |
| **Haiku** | no Chromium; WebPositive on WebKit; Falkon through Qt | | |
| **SerenityOS** | no Chromium; Ladybird, its own engine | | writing a browser (the owner's rule: Jam OS ports) |

**What we take**: Alpine's build, nearly whole; Fuchsia's per-type
sandbox as the shape for later; the BSDs' lesson that patches must stay
few to be carried every release. **What is our own**: every layer under
Chromium, and the JIT and sandbox rules.

## The design

### 1. The toolchain

**What Chromium needs**: clang only, lld, libc++ (its own copy,
`use_custom_libcxx`), Rust (`enable_rust`), a sysroot with the C library
and Linux's UAPI headers (`linux/futex.h`, `linux/memfd.h` and the like).
Host tools built during the build (protoc, V8's `mksnapshot`, the
generators) run on the build host and are built by Chromium's host
toolchain.

**The pieces, and where each comes from** (Q2 asks the owner about the
first two):

| Piece | From | Runs on | Licence |
|---|---|---|---|
| clang and lld | **Chromium's own pinned build** for this version, fetched by its tools/clang/scripts/update.py into the source tree (the compiler Chromium is tested with, plugins included) | the build host (WSL) | Apache-2.0 with LLVM's exception |
| rustc, cargo, bindgen, rust-src | **Chromium's own pinned Rust**, fetched by tools/rust/update_rust.py; Chromium builds the Rust standard library itself from rust-src through GN, for whatever target it is told | the build host | Apache-2.0 or MIT |
| compiler-rt's builtins, libunwind | **built by us from the same LLVM release's source**, for `x86_64-unknown-linux-musl` against Jam OS's sysroot, static: the runtime every clang-built program links | Jam OS (linked into programs) | Apache-2.0 with LLVM's exception (the exception lets any program link them) |
| libc++ and libc++abi | for Chromium: **its own copy** from its tree, built by its build. For every other C++ port: the same LLVM release's, built by us beside the builtins (G2's Q6 answer: LLVM's libc++) | Jam OS | the same |
| musl, the call layer, libjam | Jam OS's sysroot (`build/sysroot`), built by `make` with gcc as today; clang-built code links against it (the C ABI is the same) | Jam OS | MIT, BSD-2 |
| Linux UAPI headers | a pinned Linux release's headers_install output (Alpine's `linux-headers` is the same thing), a data port, installed into a sysroot overlay | headers only | GPL-2.0 with the Linux syscall note (which lets any program include them) |

**The drivers.** A new pair, tools/jamos-clang and tools/jamos-clang++
(new), does for clang what `tools/jamos-cc` does for gcc: compile with
`--target=x86_64-unknown-linux-musl`, the port's sysroot (Jam OS's plus
the UAPI headers), `-D__jamos__=1` and, unlike `jamos-cc`, **`__linux__`
left defined** (the triple defines it; Chromium, Rust's `libc` crate and
Linux's headers need it); link static and non-PIE with lld through
`user/posix/posix.ld`, the builtins, libunwind (and libc++ for a C++
port that isn't Chromium), and the program's list as its ELF note, as
`jamos-cc` does. `jamos-cc` stays as it is for C ports (its choice of no
`__linux__` was right for configure scripts on the Mac). The track
checks that lld accepts `posix.ld` (its `PHDRS` and `KEEP` lines;
`.eh_frame_hdr` kept for unwinding) and adds a line if it doesn't.

**Where it is built.** Everything above builds on the PC's WSL (Linux
x86_64 host: Chromium's prebuilt clang and Rust are Linux binaries). The
target runtimes are M13.7 recipes (ports/llvm-runtimes and
ports/linux-uapi, new), so their builds are stamped, reproducible and
packaged like any port, and a C++ port on the Mac can use the same
recipes with Homebrew's LLVM of the same major version. The host
toolchain is fetched once per Chromium version into
`~/.cache/jamos/toolchains/` on the build host by a script
(tools/cr/toolchain.sh, new), pinned by Chromium's own revision files.

**How Chromium's GN arguments select it** (the plan's guess; CB1 tried it
and decided otherwise in places: GN gives the default toolchain the
build's own arguments, so `is_musl` and the sysroot are args.gn's and a
host toolchain of ours puts Linux's back, see [CB1 as
built](#cb1-as-built-2026-10-14)):
- `is_clang=true`, the default `clang_base_path` (Chromium's own clang in
  its tree), `use_lld=true`, `use_custom_libcxx=true`, `is_musl=true`
  (copium's patch), `enable_rust=true` with the default Rust in its tree;
- `custom_toolchain="//build/toolchain/jamos:x64"`: a small GN toolchain
  added by our patch (unbundle's shape), whose compiler is
  tools/jamos-clang++ and whose `ldflags` are the static non-PIE link with
  `posix.ld`; the target's Rust triple `x86_64-unknown-linux-musl` added
  to `build/rust/known-target-triples.txt` as Alpine adds its own;
- `host_toolchain` left as Chromium's default Linux clang, with
  `use_sysroot=true` so the host tools build against Chromium's own Debian
  sysroot, and **`target_sysroot`** pointed at Jam OS's sysroot for the
  default toolchain (Chromium's `build/config/sysroot.gni` takes it;
  Alpine instead builds host and target alike on musl, which a cross
  build can't);
- PIE off for the target: the toolchain's config drops `-pie` and builds
  `-fno-pie` (Chromium has no argument for it; one line of our patch).

**A recipe that uses the toolchain** names it (`toolchain clang`, a new
recipe line in tools/ports.py), and the builder sets `CC`, `CXX`, `AR`
and the cross files (`ports/jamos.meson`, `ports/jamos.cmake`) to the
clang drivers. Only Chromium uses it at first; a later C++ port needs
nothing more.

**The C++ and Rust hellos (CR-a)** prove the chain before Chromium:
exceptions thrown and caught across files, static constructors, eight
threads, `thread_local`, `std::filesystem` on `/data`, iostreams; a Rust
program with threads, a channel, file I/O and a panic (abort). Both run
from `/data` on Jam OS.

**Inline system calls.** Code that issues the `syscall` instruction
itself (linux_syscall_support.h in Chromium's sandbox and breakpad) would
run a Jam OS call with Linux's numbers. A check, tools/cr/syscall-scan.py
(new), disassembles a built program and fails on any `syscall` outside
Jam OS's own call stubs; every Chromium build runs it.

#### CT1 as built (2026-10-13)

The C++ half of CR-a passes: `tools/cr-test.sh build/test/cr hello` on
the PC builds `ports/cr-hello` with the chain below, the syscall scan
passes on it, the build host's own `c++` build of the same source prints
its `expected.txt`, and on Jam OS (QEMU, from `/data/pkg`) it prints the
same bytes: exceptions across files and through destructors, `bad_alloc`,
`exception_ptr` across threads, static constructors, eight threads with
`thread_local` destructors, a guarded static, `std::atomic::wait` (a
futex through musl's `syscall()`), `call_once`, iostreams and
`std::filesystem` on `/data`. What it is, and where it differs from the
design above:

- **The host toolchain**: `tools/cr/toolchain.pins` pins Chromium 155's
  clang package (`clang-llvmorg-24-init-7747-g62397f8b-27`, 57 MB, clang
  24.0.0git with lld, llvm-ar, llvm-nm, llvm-objcopy; no llvm-objdump:
  the scan uses the cross binutils' objdump) and CMake 3.31.10 (Kitware's
  wheel). `tools/cr/toolchain.sh` fetches both by size and SHA-256
  (`~/.cache/jamos/distfiles`, the mirror, then the URL), unpacks clang
  into `~/.cache/jamos/toolchains/clang-<version>/` (326 MB) and puts
  CMake into the ports builder's venv. Rust is CT2's line in the same file.
- **The runtimes come from LLVM 23.1.3's source release**, not the
  clang's own commit: Chromium's clang is a trunk build with no source
  release, and the release tarball has a published hash. The ABIs the
  runtimes carry (Itanium C++, DWARF unwinding, the builtins' names) are
  the same across them. `ports/llvm-runtimes` builds compiler-rt's
  builtins and crtbegin/crtend, libunwind, libc++abi and libc++ (musl,
  static, no PIC) in two CMake passes that never link (about 40 s at 10
  jobs; the stage is 20 MB: libc++.a 2.4 MB). libc++abi is told musl has
  no `__cxa_thread_atexit_impl` (a link-less check would say yes).
- **The UAPI headers** are `ports/linux-uapi`: Linux 6.18.56's
  `headers_install`, all of `include/` less `drm/` and the netfilter
  headers whose names differ from their twins only in case (FAT32), 8.7
  MB. llvm-runtimes needs it (libc++'s futex wait), so every clang port
  has it.
- **`posix.ld` changed for every program**, not only clang's: `.eh_frame`
  and `.eh_frame_hdr` (under a new `PT_GNU_EH_FRAME`) and
  `.gcc_except_table` are kept in the read-only segment, where it
  discarded `.eh_frame` before; `jamos-cc` links with `--eh-frame-hdr`.
  The tree's own programs are built without unwind tables, so they barely
  change (the boot image grew 0.5%); a `jamos-cc` port keeps its tables
  (posixtest +1.8%). lld takes the script as it is otherwise.
- **libc gained two of musl's `src/ldso` files** (`user/posix/posix.mk`):
  the static `dl_iterate_phdr` (libunwind finds the tables by it) and
  `dladdr`'s stub (always 0; libunwind's function names call it).
- **The drivers**: `tools/jamos-clang` (C) and `tools/jamos-clang++` (a
  link to it) compile with `--target=x86_64-unknown-linux-musl`
  (`__linux__` defined), `-nostdinc` and the sysroot's headers before
  clang's own (libc++'s `include/c++/v1` first for C++), `-D__jamos__=1`,
  `-fno-pie`; link with lld (`-fuse-ld=lld -static -nostdlib`, posix.ld,
  `--gc-sections`, `--eh-frame-hdr`, `--build-id=none`): crt1, crti,
  crtbegin, the objects, the program's list note, `-lc++ -lc++abi` (C++),
  `-lunwind -lc`, the builtins, crtend, crtn. `-shared`, `-pie` and
  `-static-pie` are refused; `-fPIC` objects are taken (they link into
  the non-PIE program). `JAMOS_SYSROOT` is the sysroot (it must hold the
  runtimes to link), `JAMOS_CLANG` another clang's `bin/` (Homebrew's
  LLVM and lld on the Mac: untested).
- **A recipe selects it** with `toolchain clang` and `needs llvm-runtimes`
  (the lint asks for both): the builder gives the port `jamos-clang` and
  `jamos-clang++` under the usual names (`x86_64-linux-musl-gcc`, `-g++`,
  also `-clang`, `-clang++`), so `ports/jamos.cmake` and
  `ports/jamos.meson` need no change, and the clang's identity is in the
  port's stamp. A port of Jam OS's own code (cr-hello, CT2's Rust hello)
  says `local files`: its source is its `files/` directory ([M13.7's
  keys](M13.7-PLAN.md#2-the-recipe)).
- **The PC check** (`tools/cr/hostcheck.sh`, 2026-10-13): 776 GB free under
  `~/cr`, 23 GB of memory, 8 GB of swap (the plan asks 16: a WARN below
  8), 28 threads, WSL kernel 5.15. It FAILed on `gperf` (Chromium's build;
  CT2 builds one, below) and WARNs on Node.js (CB1 says whether the tarball
  brings its own).

#### CT2 as built (2026-10-14)

The Rust half of CR-a passes: `tools/cr-test.sh build/test/cr rust` on the
PC builds `ports/cr-rust-hello`, the syscall scan passes on it (105 `syscall`
instructions, all in Jam OS's call stubs), the same source built for the
build host by the same rustc prints its `expected.txt` (and, with the
argument `panic`, aborts with status 134 and `expected-panic.txt`), and on
Jam OS from `/data/pkg` it prints the same bytes and aborts the same way
(the shell says "killed by SIGABRT", status 134). The program uses eight
threads with `thread_local` destructors, a mutex, a condition variable,
scoped threads, `OnceLock`, park and unpark, four channel kinds (mpsc, a
rendezvous `sync_channel`, `recv_timeout`, a worker pool over boxed jobs),
files (create, append, seek, `fs::copy`, rename, `read_dir`, error kinds
and their texts, four threads writing at once, `remove_dir_all`),
`HashMap` (std seeds it from the system), `u128` arithmetic, float and
Unicode formatting; the build is about 23 s, the program 1 MB. No change
to the kernel, the libc or `posix.ld` was needed: Rust's std ran on the
call layer as it stood.

- **Which std: built from rust-src, with Chromium's Rust.** Chromium's
  package (`rust-toolchain-<revision>-<clang revision>.tar.xz`, 276 MB, 1.4
  GB unpacked: rustc 1.100.0-nightly of 2026-09-09, cargo, bindgen, and
  `lib/rustlib/src/rust/library` with the crates it needs vendored beside
  it) has the standard library for the host only. rustup's prebuilt
  `x86_64-unknown-linux-musl` std was not tried: rustc takes only a
  standard library built by the very same compiler (a different build's
  rlibs are refused as "compiled by an incompatible version of rustc"),
  Chromium's rustc is a dev commit with its own version suffix, and no
  rustup std is built by it. The route that needs nothing more fetched is
  also Chromium's own (its GN builds std from the same rust-src for
  whatever target it is told): `cargo build -Zbuild-std=std,panic_abort
  --target x86_64-unknown-linux-musl --offline`, the vendored crates named
  by `--config` (`source.vendored-sources.directory`), `RUSTC_BOOTSTRAP=1`
  for the unstable flag. So CB1 has already seen Chromium's Rust build
  std for this target against Jam OS's musl and runtimes.
- **The link**: the port's compiler wrapper (tools/jamos-clang) is cargo's
  linker for the target. rustc's flags: `-C link-self-contained=no` (rustc's
  own musl, crt files and libunwind stay out: `-lc` and `-lunwind` resolve
  to Jam OS's musl and ports/llvm-runtimes' libunwind in the sysroot),
  `-C relocation-model=static` (musl's default is static-pie, which
  tools/jamos-clang refuses: Jam OS runs `ET_EXEC` only; std is built with
  it too), and `-C link-arg=--jamos-wants=<file>` for the program's list
  (the wrapper picks a list by the output's name, and cargo's output is
  `deps/cr_rust_hello-<hash>`). Paths are remapped
  (`--remap-path-prefix`) so the binary does not carry the build's.
  `panic=abort`, as Chromium builds its Rust, and so the abort is the
  panic test.
- **The recipe line `toolchain clang rust`** (tools/portslib/recipe.py) adds
  Chromium's pinned Rust to `toolchain clang`: the build gets `JAMOS_RUST`
  (the unpacked directory: JAMOS_RUST in the host's environment names
  another), its `build.sh` runs cargo, and the Rust's identity (its
  `rustc --version` line) is in the port's stamp. `tools/cr/toolchain.sh
  rust` unpacks the package (pinned by size and SHA-256 in
  `tools/cr/toolchain.pins`, like clang) into
  `~/.cache/jamos/toolchains/rust-<version>/`; it checks that rustc runs
  and that rust-src has its vendored crates.
- **gperf is built pinned into `~/.cache/jamos/tools`** (the owner,
  2026-10-14: Chromium's build needs gperf, and none is to be taken from
  the system): `tools/cr/toolchain.sh gperf` fetches GNU gperf 3.1 (the
  version Debian and Ubuntu ship; 1215925 bytes, SHA-256 in
  `toolchain.pins`), builds its `lib/` and `src/` with the host's C++
  compiler and make (`configure --prefix=$JAMOS_TOOLS`; the documentation
  is not built: its generated files look out of date and ask for makeinfo)
  and puts `gperf` in `~/.cache/jamos/tools/bin`, which `tools/ports.py`
  and `tools/cr/hostcheck.sh` put first on PATH, so the PC check passes
  (`tool: gperf GNU gperf 3.1`) and CB1's build finds it there. Done once
  per version; with no names the script fetches clang, cmake, Rust and
  gperf.

#### CB1 as built (2026-10-14)

Track CB1, the first Chromium build: ports/chromium builds Chromium
155.0.8059.39 for Jam OS on the PC's WSL with Chromium's own GN, clang
and Rust, through the ports builder. **Not finished yet** (2026-10-15
morning): GN generates the build (36,768 targets), every system library
and shared-library question below is answered, and ninja had built about
34,000 of the 44,700 steps base_unittests and headless_shell need
(base, V8, Skia, the Rust standard library for
`x86_64-unknown-linux-musl`, most of Blink) when the PC stopped
answering over the VPN (from about 08:25); the errors still open at that
point are fixed in the tree but not yet compiled (minigbm, fontconfig's
`strerror_r`, the trust store, Crashpad's `<sys/cdefs.h>`, expat,
Wayland's GBM). No program has been linked for Jam OS yet, so the link
(the driver, posix.ld, Rust's std in a C++ link) and the syscall scan of
the two programs are still to come; the next build round starts where
ninja stopped (`tools/cr/build.sh start`, with a `wait` kept open).

- **The source** is the chromium-linux-tarballs project's tarball Alpine
  uses (2021203420 bytes, its SHA-256 in the recipe; Alpine's SHA-512
  matches it), mirrored in the Mac's `~/jamos-distfiles`. It is the whole
  tree without `.git` (10 GB, 675,000 files, test data included: base's,
  net's) but without what Chromium's DEPS fetches from Google's storage
  and CIPD. **Node.js: the tarball brings none** (the plan's *unsure*),
  so Chromium's own pinned one (DEPS' `src/third_party/node/linux`
  object, v24.12.0) is a line of tools/cr/toolchain.pins, as its clang
  and Rust are, and the build links it into the tree. The same goes for
  the other things DEPS fetches that the build needs: the Debian bullseye
  sysroot Chromium's host tools are built against (its
  `sysroots.json`), the test fonts base_unittests' data names (DEPS'
  `src/third_party/test_fonts/test_fonts`; Google's separate
  `-testdata` tarball, 1.3 GB, would add nothing the linux tarball lacks
  but these fonts), and Go 1.26.6 (Dawn's DEPS' `dawn_go_version`; Go's
  own release, by its published SHA-256), which Dawn's build runs
  offline (`GOPROXY=off`) to generate Tint's sources. Each is fetched and
  unpacked by `tools/cr/toolchain.sh` into `~/.cache/jamos/toolchains`.
  The tarball's own prebuilt tools that the build runs are kept: GN
  (`buildtools/linux64/gn`, Chromium's pinned build) and the Python GN
  runs scripts with (Chromium's cpython3 in its third_party directory);
  its prebuilt gperf (3.2) is
  replaced by the pinned 3.1 the owner asked for (CT2). Alpine deletes
  every prebuilt ELF file and uses its system's tools instead; Jam OS
  takes Chromium's pinned ones, as for clang.
- **The patches** (ports/chromium/patches, each with its reason): Alpine's
  six musl ones and copium's eight the plan names, copied with their
  headers under a DEP-3 header of ours (the authors' e-mail addresses left
  out of their `From` lines). Seven had to be **rebased to apply exactly**
  (Alpine's `patch` takes fuzz, the ports builder takes none: the same
  change, made again by `diff -u` against this tarball), and three had a
  `+++` name that GNU patch ignores and ours doesn't (copium's two WebRTC
  ones lack a `/`, Alpine's llvm-subzero one names a `.orig`). The
  seccomp sandbox's four are not taken: they are run-time fixes, and the
  code compiles without them. Jam OS's own: 0901 the toolchains (below),
  0902 the Linux clipboard helpers without D-Bus (Chromium 155's Wayland
  needs `clipboard_util_linux`, which exists only with `use_dbus`), 0903 no
  Mesa DRI directory (content/gpu and media/gpu/sandbox ask pkg-config on
  every Linux build), 0904 `<sys/cdefs.h>` only where there is one
  (PartitionAlloc's shim, libsync; musl has none, Alpine adds a BSD copy),
  0905 base_unittests without shared libraries (its dlopen() helpers and
  the Rust toolchain tests left out on Jam OS, the test trace processor
  static), 0906 no dummy ANGLE libraries as data of programs that use GL.
  The launcher patch (2) waits for CB5: nothing needs it to compile.
- **The toolchains (patch 0901), and where the plan was wrong.** GN gives
  the default toolchain the build's own arguments and ignores its
  `toolchain_args`, so a target toolchain can't set `is_musl` or the
  sysroot for itself. So args.gn describes the target (`is_jamos`,
  `is_musl`, `target_sysroot` = the port's sysroot,
  `custom_toolchain = "//build/toolchain/jamos:x64"`), and a second
  toolchain of ours, `//build/toolchain/jamos:host` (Chromium's Linux clang
  toolchain otherwise), is `host_toolchain` and `v8_snapshot_toolchain`
  and puts `is_jamos`, `is_musl` and the Debian sysroot back. The target
  toolchain compiles with Chromium's clang (`clang_toolchain`) and
  `--target=x86_64-unknown-linux-musl -D__jamos__=1` added after
  Chromium's own flags (which say `x86_64-unknown-linux-gnu`; the last
  `--target` wins), against the port's sysroot (headers in `include/`,
  as Fuchsia's: the module map's root is changed to match), and links
  through `jamos_link_driver`: the port's wrapper of tools/jamos-clang
  (static, non-PIE, posix.ld, the program's list chosen by the output's
  name). `-pie` is not given on Jam OS, `use_mold` is off (lld), the Rust
  target is `x86_64-unknown-linux-musl` (added to the known triples), and
  libffi (Wayland's) is the libffi port's static `libffi.a`. Chromium's
  own code is compiled `-fPIC` as on Linux; the program is still linked
  position-dependent.
- **What the build needs of the sysroot**, found by `tools/cr/build.sh
  pkgs` (GN with a pkg-config that only writes down what it is asked):
  only xkbcommon (Jam OS's port, `needs xkbcommon`) once the arguments
  turn off the rest. Chromium's pkg-config script expects `usr/lib` under
  a sysroot; the port's own pkg-config wrapper (GN's `pkg_config`) knows
  the sysroot's layout.
- **The arguments** (ports/chromium/files/args.gn, the plan's set
  changed): `enable_pdf` and `enable_printing` stay on (GN reads the
  whole tree and //chrome/test asserts print preview; the targets build
  only what they need); off besides the plan's: `use_qt5`,
  `use_pangocairo`, `rtc_use_pipewire`, `use_nss_client_certs` and
  `use_nss_server_certs` (no NSS: Chrome's own root store),
  `use_system_minigbm`, `use_v8_context_snapshot` (it would build Blink
  twice, for the host too, as Mac and Windows cross builds avoid),
  `use_clang_modules` (as Alpine), Vulkan everywhere (`enable_vulkan`,
  ANGLE's, Dawn's, SwiftShader's), `skia_use_dawn`, `webnn_use_litert`,
  `use_libpci`, VR and OpenXR. ANGLE is linked into the program
  (`use_static_angle`, the default). Dawn is still built: the on-device
  model service links it on Linux, and leaving it out would take a patch
  to that service. No program carries a shared library as data now
  (`gn desc ... runtime_deps`: base_unittests needs `test_child_process`,
  headless_shell `chrome_crashpad_handler`, both static programs).
- **The kept tree and `package --from`** (tools/portslib/kept.py,
  pkgcmd.py): the recipe line `tree kept` keeps the unpacked source (its
  output inside it, `out/jamos`), the port's sysroot and its tools in
  `~/cr/<version>/` (`ports.py build --kept DIR`; `--clean` removes it),
  rewritten only where their bytes change, so ninja sees as changed only
  what is. The source is unpacked once with no patches; a patch change
  takes the files any old or new patch touches as upstream has them
  (`pristine/`, or the tree itself for a file no patch touched before),
  applies the new patches to those copies and writes only the files that
  differ (a patch that doesn't apply leaves the tree as it was). An
  iteration on a patch or an argument costs seconds, not the 3.5 minutes
  of an unpacking. `ports.py package --from DIR <port>` packages a build
  made elsewhere (its `stage/`, checked against `port.sha256`, the hash
  of the port's directory that built it), so the Mac signs what the PC
  built.
- **The PC's budget**: the build holds a claim in tools/remote-slots.py's
  budget (`--detach`: it waits for its turn, starts the build in the
  background and returns; the claim lasts as long as the build) of
  `CR_JOBS` CPUs (ninja's `-j`) and `CR_MEM_MB` of memory, at nice 19 and
  ionice idle, links one at a time (`concurrent_links = 1`). The build
  holds `~/cr/.lock`. While a big claim runs, a `tools/remote-test.sh`
  call's own build step (8 CPUs by default) waits for room: status calls
  give `JAMOS_REMOTE_BUILD_CPUS=1` and a light claim. A claim whose memory
  does not fit beside what runs waits (a run of 18 GB beside a 2 GB test
  in the 20 GB budget waited all night). **WSL ends all of a user's
  processes when their last session closes** (systemd's user sessions):
  a build left alone on an otherwise idle PC was killed, with no trace,
  the moment a status call ended and nothing else was connected. So a
  `tools/cr/build.sh wait 36000` stays connected (a background
  `tools/remote-test.sh` call, its light claim 1 CPU) for as long as the
  build runs; with other agents' tests on the PC it never showed.
- **The figures so far** (BENCH.md has them): the tarball unpacks in 3.5
  minutes into 12 GB; Chromium's tools in `~/.cache/jamos/toolchains`
  2.2 GB; the output directory 6.3 GB after the first 16,000 steps. On
  the i7-14700's WSL, base and its dependencies (3,100 steps) took 2
  minutes at `-j10`; the first 17 minutes at `-j18` did 15,900 steps; Blink's
  core runs at about 75 steps a minute at `-j16`. Memory is the limit,
  not CPUs: the build's processes' proportional sizes (Pss) peaked at 8 GB
  at `-j14` in V8 and Blink's bindings but 15 to 22 GB at `-j14` to `-j24`
  in Blink's core (single compiles of 2.5 to 3.6 GB), with 1.5 to 2.7 GB
  of swap used; `-j14` to `-j16` and a 16 GB claim kept it under WSL's 23
  GB. A clean build's time is not measured yet: at those rates it is
  roughly 2.5 to 3.5 hours at `-j16`.

### 2. The address space: huge sparse reservations (the top risk)

**What Chromium does** ([CR-RESEARCH section 6](CR-RESEARCH.md#6-v8s-jit)):
V8's sandbox reserves **1 TiB** plus guard regions up front (falling back
to a smaller, partly reserved sandbox of at least 8 GiB), pointer
compression a 4 GiB cage, and PartitionAlloc **pools of many GiB**
(*unsure* of their exact sizes: several of 16 GiB or more). All are
`mmap(PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE)`. Then,
inside them, thousands of times over a process's life: `mprotect` to
read-write (commit), `madvise(MADV_DONTNEED)` or `mmap(MAP_FIXED,
PROT_NONE)` (decommit), `MAP_FIXED` maps at chosen addresses, `mprotect`
back. Each renderer does this, and so does the browser.

**What Jam OS must make true**, each a general property any program can
rely on:
1. **A reservation costs nothing until touched**: no pages, no page
   tables, one mapping record and one VMO record, whatever its size, up
   to the user half of the address space (128 TiB).
2. **Touched pages are committed and charged as today** (to the
   process's job, on the fault); a job at its limit fails the touch, and
   the thread dies with a line saying so (Linux's overcommit with the
   OOM killer behaves the same for such programs). No page is ever
   committed by `mprotect` alone.
3. **Commit, decommit and `MAP_FIXED` inside a reservation cost O(log n)
   and leave the mapping count flat**: neighbours with the same
   permissions merge again.
4. **Decommit gives the pages back at once** (and the job's charge),
   so a long-lived renderer's memory follows its use.

**The design, in the kernel** (the real code: `kernel/object/vmo.c`,
`kernel/mm/aspace.c`):

- **VMOs past 64 GiB (CK1).** A paged VMO's table today is
  `root[64]` inline, then a mid table (1 GiB) and a leaf (2 MiB), so 64
  GiB is the most. Add one level above for VMOs bigger than 64 GiB: a
  top table of 512 entries of 512 GiB each, made on the first commit
  past the inline root's reach and charged like every table page. Small
  VMOs keep today's layout (no cost to the thousands of small ones).
  `VMO_MAX_SIZE` becomes 128 TiB for plain paged VMOs; kept, pager,
  contiguous and physical VMOs keep their limits (64 GiB, or less).
  `vmo_decommit`, shrink and destroy already skip a missing mid or leaf;
  they learn to skip a missing top entry, so decommitting a whole
  untouched TiB is a handful of checks, not 512 K lock holds. Commit's charging
  is unchanged (page first, then tables, undone together).
- **Mappings at scale (CK2).** The mapping list becomes a balanced tree
  keyed by base, each node keeping the largest free gap below it, so a
  fault's `find` and a placement's first fit are O(log n). After every
  map, protect and replace, a mapping **merges** with its neighbour when
  both map the same VMO at contiguous offsets with the same permissions,
  maximum, cache mode and neither is kept: a V8 page committed and later
  decommitted leaves the reservation one mapping again.
  `VMAR_MAX_MAPPINGS` rises from 16,384 to 65,536 (Linux's
  `max_map_count` is 65,530), still charged per page of records to the
  job. The region lock and the fault path's rules don't change.
- **The reverse map at scale (CK3).** A VMO's user mappings are a list
  walked under its lock with interrupts off on every decommit batch. A
  reservation split into thousands of pieces would make each batch
  walk them all. The list becomes an index by page range (an interval
  tree), so a decommit touches only the pieces it overlaps.
- **Page tables.** The address space's own tables are made on the first
  fault under each 2 MiB and charged, as now; unmap frees empty ones,
  decommit doesn't (Linux doesn't either). A program that touches one
  page in every 2 MiB of a TiB would pay 2 GiB of tables, charged to its
  job: correct, and a reason the tests measure V8's real pattern.
  `walk_leaves` already skips a missing table at each level.
- **Quotas.** Nothing new: pages, tables and mapping records are all
  charged to the job when made. A reservation's size is not charged
  (address space is per process); a hostile program can only spend its
  own job's pages.

**The design, in libc (CL1)**, `user/posix/sys/mem.c`:
- An anonymous `mmap` of any size up to the user half (any protection,
  with or without `MAP_NORESERVE`) is one VMO and one mapping, as today,
  now possibly huge (CK1). Its handle's maximum permissions are read and
  write (execute only for JIT memory: section 12).
- **`MAP_FIXED` anonymous inside one of libc's own private anonymous
  mappings** (V8's and PartitionAlloc's decommit and re-map) no longer
  makes a new VMO: libc decommits the range in the existing VMO
  (`vmo_decommit`: it reads as zeros after, Linux's semantics for a fresh
  anonymous map) and sets the new protection (`vmar_protect`), under its
  lock. CK2's merge keeps the count flat. A `MAP_FIXED` over anything
  else (a memfd's VMO, a file, a shared mapping) keeps today's replace.
- `mprotect` inside: the kernel splits and merges (CK2); `madvise`'s
  decommit as today; `mremap` stays ENOSYS (V8 copies instead);
  `mseal` ENOSYS.
- libc's table (`user/posix/sys/maptable.c`) holds one entry per
  reservation however many times the kernel splits it (it already
  tracks VMO ranges, not kernel mappings).

**Tests** that copy the real patterns, in posixtest's `mem` section:
reserve 1 TiB plus 32 GiB of guards (V8's sandbox), commit 256 KiB
pieces by `mprotect` 10,000 times at scattered offsets, write a byte in
each, decommit half by `MAP_FIXED PROT_NONE` and half by `MADV_DONTNEED`,
and check: the job's pages back where they started, the mapping count
(`process_get_info`) small, each step's time flat as the count grows;
PartitionAlloc's pattern (16 GiB pools, 2 MiB super pages, slot spans
committed and decommitted); and a job limit hit by a touch (the thread
dies with the reason, nothing else does).

### 3. libc additions: `/proc/self`, memfd, raw calls

- **`/proc` in libc** (CL2), like its `/dev` and `/etc`: a path libc
  answers itself, nothing in the namespace.
  - `readlink("/proc/self/exe")`: the absolute path the program was
    started from. Every starter already knows it; a new field in the
    POSIX hand-over (`user/include/posixfds.h`) and in libjam's spawn
    carries it (the shell and init write it for the programs they start).
    Chromium treats a failure here as fatal.
  - `/proc/self/fd`: `getdents` lists the descriptor table (Chromium
    closes everything else in a child this way, and falls back to a loop
    without it); `readlink` of an entry gives the path of a file
    descriptor, or Linux's `pipe:[N]`, `socket:[N]`, `anon_inode:[...]`
    forms.
  - **Reopening `/proc/self/fd/N`** makes a new description of the same
    object: for a memfd, the same VMO with the access asked, so
    `O_RDONLY` gives a handle **without `RIGHT_WRITE`**. That is
    Chromium's read-only twin of a shared memory region, and it is real:
    the receiver can't map it writable, whatever the sender does.
  - Everything else under `/proc` (`/proc/self/stat`, `/proc/cpuinfo`,
    `/proc/self/maps`, `/proc/sys/...`): **ENOENT, never a half-filled
    file.** Chromium's callers of these fail soft (metrics, CPU names,
    V8's remap fallback); the ENOSYS and ENOENT log (below) shows any
    that doesn't.
- **memfd seals across processes** (CU1 with CL2): seals already travel
  with a memfd in the hand-over (size and seals); `SCM_RIGHTS` carries
  them the same way. And once `F_SEAL_WRITE` or `F_SEAL_FUTURE_WRITE` is
  set, every handle libc gives out for that memfd afterwards (a reopen, a
  hand-over, `SCM_RIGHTS`) lacks `RIGHT_WRITE`, so the seal holds in the
  receivers by the kernel's rights, not by trust. `F_SEAL_WRITE` still
  refuses while this process has a writable mapping (Linux's EBUSY).
- **Raw calls and the log (CL3).** A mode that logs every system call
  number libc doesn't answer, every unknown `ioctl` and every `/proc`
  ENOENT, once per process, to the kernel log (on with an environment
  variable, `JAMOS_ENOSYS_LOG=1`, so a Chromium run lists its gaps).
  Then the calls already known to be used: `prctl(PR_SET_NAME)` and
  `PR_GET_NAME` (a thread's name kept by libc), `sched_setaffinity` and
  `getaffinity` (accepted; every CPU), `setpriority`/`getpriority` on a
  thread (mapped onto `thread_set_priority`'s range), `membarrier`
  (ENOSYS: musl falls back), futex operations PartitionAlloc and Mojo use
  by raw `syscall()` (`FUTEX_WAIT_PRIVATE`, `FUTEX_WAKE_PRIVATE`,
  `FUTEX_WAIT_BITSET` with `FUTEX_CLOCK_REALTIME`: checked against M13's
  handler, added if missing).

### 4. `AF_UNIX` socketpairs that carry descriptors (Mojo)

**What Mojo does**: `socketpair(AF_UNIX, SOCK_STREAM)`; `sendmsg` with
`MSG_NOSIGNAL | MSG_DONTWAIT` and an `SCM_RIGHTS` block (at most
`kMaxSendmsgHandles` a call, *unsure* of its value: 128 in older trees;
more handles go in several calls, each with at least one byte);
`recvmsg` with `MSG_DONTWAIT`, checking `MSG_CTRUNC`; `shutdown(SHUT_RDWR)`
when a broker closes. The child gets its end as an inherited descriptor
named on its command line. No named sockets, no `SOCK_SEQPACKET`
(*unsure*: only the Linux sandbox's IPC used it).

**The design (CU1)**, in `user/posix/sys/`, on G2's GS1 (whose two
translation tables are written in a file of their own for this):
- A socketpair end is a channel end (today). A message **without**
  handles is plain bytes (today's format: nothing changes for programs
  that send no descriptors). A message **with** handles starts with a
  small header: a magic, then one record per descriptor (its kind, its
  status flags, its seals for a memfd, how many handles it takes), then
  the bytes. The kinds and their handles are the POSIX hand-over's
  (`user/include/posixfds.h`): a pipe or socketpair end (a duplicate of
  its channel end), a memfd (its VMO, with the description's rights), an
  eventfd, a file (a fresh open, as the hand-over does, its offset and
  flags), `/dev` nodes. A network socket is refused (EINVAL, logged
  once): its ring can't be shared (M13's R1 answer B), and no Chromium
  path sends one.
- **More than 64 descriptors** (a channel message's most) go as several
  messages, the first carrying the first byte and each later one marked
  "more of the same send"; the receiver delivers them all with the first
  byte. Linux's own limit (253) is kept: more is EINVAL.
- `recvmsg`: descriptors made from a message's handles are delivered
  with its first byte, as Linux does; too little room closes the rest and
  sets `MSG_CTRUNC`; `MSG_CMSG_CLOEXEC` honoured; `MSG_DONTWAIT`,
  `MSG_PEEK` (bytes only), `MSG_WAITALL` as for bytes today.
- **`shutdown(SHUT_RDWR)`** closes the description's channel end (every
  descriptor sharing the description sees it closed, as Linux shuts a
  socket for every descriptor of it); the peer reads end of file, its
  writes get EPIPE. `SHUT_RD` and `SHUT_WR` alone stay EOPNOTSUPP
  (half-close needs a channel feature nobody has asked for).
- `MSG_NOSIGNAL`: no SIGPIPE (already the default without signals asked).
- **The Wayland socket** (GS1) keeps its own narrow list on the shared
  tables; one translation, two users.
- Size: about 800 lines and their tests.

**CU1 as built** (`user/posix/sys/sockpair.c`; the code's header has the
whole model). Where it differs from the design above:
- More than 64 handles: the parts come **first** and the bytes ride in
  the **last** one (each part a header with the send's cookie and its
  place, then hand-over records and their handles), so the receiver holds
  parts until the last and never waits between them. One send at a time
  per description (a lock across the send); parts of a send that never
  finished are dropped when the next send or plain bytes come.
- Not an inner channel per send: the kernel's cycle rule refuses a channel
  end whose queue holds a channel end, and that same rule makes sending an
  end into its own channel (or its peer's) ETOOMANYREFS (Linux allows it;
  Mojo never does it). A channel end with channel ends queued at it is
  refused the same way: a race CB5 should know of (an ipcz introduction
  whose new end got a message with a pipe or socketpair end in it before
  it was passed on).
- A file arrives **without its path** (a path is a name in the receiver's
  namespace: a later give of the file would reopen it with the receiver's
  rights), so a received file can't be given on to a child or sent again
  (ENOTSUP / EINVAL).
- **Eventfds are refused** (EINVAL, logged once): eventfd.c's counter is
  each process's own (the hand-over gives a child a copy), so one that
  crossed would not be signalled across processes. Sharing it needs the
  counter in a VMO and the hand-over's eventfd kind widened: a follow-up
  if anything needs it (Mojo's eventfd is its optional Linux fast path).
- `SOCK_SEQPACKET` pairs are built (one send one message, at most 64 KiB,
  `MSG_TRUNC`), with a hand-over kind of their own (`POSIXFDS_SEQPACKET`)
  so the type survives a child and `SCM_RIGHTS`.
- `MSG_PEEK` peeks bytes, never descriptors (Linux installs duplicates).
- `shutdown(SHUT_RDWR)` swaps the end for one whose peer is gone: a thread
  already asleep in a read or write of that description is not woken by
  it (the kernel wakes nothing when a waited-on handle closes).
- memfd: the write-seal rule is built (every VMO handle given out after
  `F_SEAL_WRITE` or `F_SEAL_FUTURE_WRITE` lacks `RIGHT_WRITE`);
  `F_SEAL_WRITE`'s EBUSY while a writable mapping exists is not (libc
  keeps no mapping's protection; Chromium seals no writes).
- Tests: posixtest `scm` (in the init run; `tools/shell-tests/posix-scm.txt`
  alone).
- For CR2 (a hostile peer): a received file is made by fobj.c's adopter,
  which asks the file's channel for its size and id within
  `FS_CALL_TIMEOUT` (60 s): a channel that never answers holds the
  receiving `recvmsg` that long for each such descriptor (Linux's never
  waits). A received memfd's VMO is used as it comes: one whose pager
  never answers would hold a fault in the receiver (the compositor's
  `VMAR_KEPT_ONLY` rule is the defence there; Chromium maps plainly).

**Named sockets (CU2, for `chrome` only)**: `chrome`'s process singleton
binds an `AF_UNIX` socket at a path in its profile directory (and a
symlink to it), and a second `chrome` connects to hand over its command
line. General design: a small broker service, `/svc/unix` (in init, or
its own process), that maps a **socket file** (the file `bind` creates:
an ext socket inode on `/jam`, M13.6) to the listener's channel; `connect`
asks the broker with the file it opened (so namespaces and views decide
who may reach it, exactly as for opening a file); `accept` gets a fresh
channel pair from the broker. Abstract names (Linux's `\0` paths): a
per-session table in the broker. One user, no `SO_PEERCRED` beyond "the
one user". Until CU2, `chrome` runs with its singleton failing (it then
behaves as the only instance: *unsure*; CB6 checks). As built (a socket
file is a plain file holding a key): [CU2 notes](#cu2-notes).

### 5. Launching other programs

**What Chromium does** ([CR-RESEARCH section 2](CR-RESEARCH.md#2-process-launching)):
`fork()`, then in the child: clear close-on-exec on the descriptors to
keep, `/dev/null` as stdin, `setpgid`, rlimits, every signal handler
reset and the mask cleared, a new environment, `dup2`s to put descriptors
at the numbers named on the child's command line, close everything else
(by `/proc/self/fd`), `PR_SET_NO_NEW_PRIVS` and `PR_SET_PDEATHSIG`,
`chdir`, then `execvp` of `/proc/self/exe`. `GetAppOutput` has a second,
smaller copy of the same.

**Two ways** for Jam OS, which has `posix_spawn` and no `fork`:

| | (A) A `posix_spawn` branch in Chromium's launcher | (B) An emulated `vfork` in libc that records every call between it and `exec` |
|---|---|---|
| What | `base/process/launch_posix.cc` gains a branch (a GN argument, `use_posix_spawn_launcher`) building `posix_spawn_file_actions` from its options (adddup2 for the remap, addopen of `/dev/null`, `addclosefrom_np`, addchdir) and attributes (process group, signal mask, default handlers), then one `posix_spawn`; `GetAppOutput` the same. macOS's launcher already has this shape | Chromium's two `fork` calls patched to `vfork`; libc records `dup2`, `close`, `fcntl`, `sigaction`, `sigprocmask`, `setpgid`, `setrlimit`, `prctl`, `chdir`, `open`, `getdents` of `/proc/self/fd` and `environ` until `execve`, then spawns with the record |
| Chromium patch | about 300 lines in two functions | two lines, but every call the child makes must be recorded faithfully |
| Jam OS work | `posix_spawn_file_actions_addclosefrom_np` (glibc's call, general), big argv and environments (finding 3), the self-image (section 6) | all of (A)'s, plus the recorder (M13.5's planned emulation), which must also show a recorded descriptor table to `/proc/self/fd` |
| Risk | low: one well-defined call; failures are errno values | high: a `fork` site in compiled code where the compiler doesn't know the function returns twice; any unrecorded call corrupts the parent ([CR-RESEARCH](CR-RESEARCH.md#2-process-launching)) |
| Generality | the call is POSIX; other ports use `posix_spawn` too (make, GLib) | general, and M13.5's goal, but off the path |

**Recommendation: (A)** (decided by this plan under the standing rule:
a technical choice). The Chromium patch is small, has macOS's shape, and
could go upstream; everything Jam OS adds for it is general. (B) stays
M13.5's, after Chromium.

**What Jam OS adds (CS1)**:
- `posix_spawn_file_actions_addclosefrom_np` (glibc 2.34's name and
  meaning: close every descriptor from N up in the child), on top of
  M13's spawn, which already passes only what isn't close-on-exec.
- **argv and the environment in a VMO** when they don't fit the startup
  message (finding 3): libjam's spawn writes them into a small read-only
  VMO and the message carries its handle; the child's start code reads
  them from there. Any program gains Linux-like command line lengths (a
  limit of 2 MiB, Linux's usual `ARG_MAX`).
- `posix_spawn` of the literal path `/proc/self/exe`: the self-image
  (section 6; done by CL2).
- `PR_SET_PDEATHSIG` (a child dies with its parent): accepted; on Jam OS
  the children are in the parent's job, and a browser that dies is
  usually killed with its job (the shell kills a program's job). A
  general "kill my children when I exit" option is a later small track
  if a port needs it exactly.

**Crashpad** forks, `setsid`s and `posix_spawn`s its handler: crash
reporting stays off (no build of the handler, and `--disable-crash-reporter`
where the build still has it).

### 6. Programs this big: paged, shared, and started again

A Chromium process is one static binary of 150 to 250 MB, and multi-process
Chromium runs it 5 to 20 times at once (browser, GPU, network, utility,
a renderer per site). Three pieces, all general:

- **Paged programs (M12.5's PX, a gate).** Programs from `/data` (and
  `/jam`, M13.6's U8 and U10) are paged in as they run, each page
  checked against `allow`'s hash table; the parts of the binary a
  process never touches are never read.
- **Limits raised (CP1)**: `VMO_VERIFIED_MAX` from 64 MiB to 1 GiB (its
  table at most 8 MiB, charged to the shell's job while it lives),
  the shell's `PROGRAM_MAX` and libos's `FILE_MAX` the same (they matter
  for the fallback that reads a program whole). `allow` and init's package
  approval hash 250 MB in about a second on the PC; the shell prints a
  line while it does.
- **The self-image (CL2 and CS1).** A starter (the shell, init, libjam's
  spawn) hands each program **its own executable VMO** (a new startup
  role, `SR_SELF_IMAGE`: a duplicate with read, map, execute, duplicate
  and transfer rights, never write). libc's `posix_spawn` of
  `/proc/self/exe` starts the child **from that same VMO** (its own path
  spelled out runs the file there now, approved now: "CL2 notes"):
  no file read, no approval step (the file the owner approved, the
  same list), and every process maps the same pages, so the text is
  shared and paged once for all of them. The child gets the role too.
  Spawning any other program from a file is M13's F1b (a gate).

### 7. Files and storage

- **Where Chromium lives**: the package and the profile on **`/jam`**,
  M13.6's ext filesystem on the Jam OS NVMe drive (M12.7's stage 2: the
  owner's TEAM drive). The stick's FAT `/data` (768 MiB) can hold the
  first test binaries (CR-b, CR-c), not a browser and its profile.
- **What the profile needs** ([CR-RESEARCH section 9](CR-RESEARCH.md#9-other-runtime-dependencies)):
  atomic rename-over and `fsync` (ImportantFileWriter), `fcntl` locks and
  SQLite's databases, symlinks (the singleton), many small files: M13.6's
  U2a to U6 give all of them.
- **Mapped files**: Chromium maps its `.pak` resources read-only and
  shared, and SQLite maps its databases while writing them with `write`
  (`mmap` on by default). So: M13's **F1b** (read-only shared mappings of
  a file's pager VMO) and **coherence** after a write (M12.5's PF2 on FAT,
  M13.6's U10 on ext) are gates. A mapping that only pretends to be
  `MAP_SHARED` (a copy) would corrupt databases: libc keeps answering
  ENODEV until the real one exists, and SQLite falls back to `read`.
  ICU's data and V8's snapshot are compiled in (`icu_use_data_file=false`,
  `v8_use_external_startup_data=false`), so fewer files are mapped.
- **`inotify`** (file watchers): ENOSYS, and Chromium's watchers that
  need it fail soft (*unsure*: CB2's run with the log on lists any that
  doesn't).
- **`/tmp`**: M13.6's U7a empties it at boot on the Jam OS drive;
  `XDG_RUNTIME_DIR` stays unset (Chromium warns).

### 8. The network

- **netstack's limits (CN1)**: sockets an opener 16 to **256**, sockets
  in all 48 to **512**, TCP connections an opener 64 to **512** and in
  all 256 to **1,024**, each with netstack's memory measured at the new
  caps (its heap, rings and lwIP's pools; BENCH.md gets the figure). The
  totals still bound netstack's memory; one program may take half.
  General: every program gets the higher caps.
- **Names**: `getaddrinfo` on worker threads to `/svc/dns` (A records:
  IPv6 is off anyway), through Alpine's `disable-dns_config_service.patch`
  (Fuchsia's stub configuration service), so Chromium's own resolver and
  DNS-over-HTTPS stay off.
- **TLS**: BoringSSL with Chromium's compiled-in root store: nothing from
  Jam OS. **Netlink**: `socket(AF_NETLINK)` is EAFNOSUPPORT; Chromium's
  network change notifier then assumes one connection that never changes
  (*unsure*: CB3 checks it doesn't loop).

### 9. Wayland

All in [G2-PLAN](G2-PLAN.md): the libc shim (GS1), the compositor's
protocols as [Chromium uses them](G2-PLAN.md#what-chromiums-wayland-backend-needs),
the owner's decoration rule (GC1b), subsurfaces for `chrome`'s bubbles
(GC2), popups for menus (GC3), wlprobe (GT). Chromium links its own
libwayland and xkbcommon. Its buffers are memfds from the GPU process,
passed to the browser by Mojo (section 4) and to the compositor by the
shim. Not offered on purpose: `wp_viewporter` (it turns on overlay
delegation) and linux-dmabuf, until the GPU.

### 10. Fonts

Chromium builds its own fontconfig, FreeType and HarfBuzz; it needs font
files and a configuration: G2's GP2, the `fonts-jamos` package
(Liberation, Noto Sans, Serif and Sans Mono, Inter, JetBrains Mono, Noto
Color Emoji) with `FONTCONFIG_FILE` set by the shell for every program
([G2 section 4](G2-PLAN.md#4-fonts)). The cache goes under
`XDG_CACHE_HOME` on `/jam`.

### 11. Sound

Chromium on Linux picks CRAS, PulseAudio or ALSA, and with none built a
fake output that drops samples ([CR-RESEARCH section 9](CR-RESEARCH.md#9-other-runtime-dependencies)).
**First builds have no sound** (nothing to do: none of the three is
built). Then, general first (CA1):
- **A small output API in libjam** over the mixer's `audio` protocol
  (`abi/idl/audio.idl`): open a stream (rate, channels, format), write
  frames (blocking or not), its latency, drain, close; a descriptor to
  poll for room. Any port's sound driver sits on it (G2's SDL driver
  would have; a later ALSA or PulseAudio shim could).
- **Chromium's back end** on it, in its tree (an `AudioManager` and an
  output stream, about 500 lines, Fuchsia's pair as the model), a patch
  in the recipe under a GN argument (`use_jamos_audio`). Output only;
  input (the microphone) later.

### 12. The JIT, by capability

**The owner's rule (2026-10-11)**: Jam OS keeps "never writable and
executable" for every program except one that asks for `right jit` in
its list and is approved. Such a program may make **JIT memory**: a VMO
of its own with write and execute rights, private to it, never
file-backed and never counted as approved code, marked in `ps`. Only its
creator writes it unless it hands on a handle (write-only, execute-only,
or both). Writable-and-executable pages first (V8's default on x64);
stricter modes later through the same capability. `--jitless` until it
lands (no WebAssembly then).

**The design**:
- **The power** (CJ1): a new root-resource right, `RIGHT_ROOT_JIT` (the
  next free bit, 27), sliced into the resource a program gets when its
  list says `right jit` (`WANT_RIGHT_JIT` beside `klog`, `sysinfo`,
  `clock` and `debug` in `user/include/wants.h`), as every `right` word
  is today.
- **JIT VMOs** (CJ1): a new call, `vmo_make_jit(vmo, root, *out)`, in
  `vmo_make_exec`'s shape: a fresh anonymous paged VMO (one handle, no
  mapping, no pin, never a pager VMO, a child or a kept one) becomes a
  JIT VMO, and `*out` is its handle with read, write, execute, map and
  duplicate (not transfer: the creator adds it to a duplicate if it hands
  one on). Needs `RIGHT_ROOT_JIT` on root. A JIT VMO can never be
  sealed, verified, made executable the old way, or given a pager.
- **W+X only there** (CJ1): `perms_ok` in `kernel/mm/aspace.c` allows
  write together with execute only for a mapping of a JIT VMO, and only
  as far as the handle's rights allow (`ASPACE_CAN_*` from the rights:
  a handle without write maps execute-only). `vmar_protect` follows the
  same rule. Everything else stays W^X.
- **Marked** (CJ1, CJ2): `process_get_info` and `proc_list` give
  `PROCESS_FLAG_JIT` (the address space has mapped a JIT VMO); `ps` puts
  `[jit]` after such a process's name, with a footnote saying what it
  means. A power held but never used isn't marked: only the root handle's
  rights say it, and with no JIT VMO mapped the program can't run code of
  its own.
- **Approval** (CJ1, CJ2): `tools/checkwants.py` allows `right jit` for
  packages and tests (ports/, user/ports/, user/tests/), never for the
  boot image's system programs (its self-test checks both, and that
  user/posix/'s programs and posixtest are refused); `allow` says it on a
  line of its own before its question ("it asks to write and run its own
  code (JIT memory): the system can't check that code, so allow it only if
  you trust the program"), then shows the list's words ("may write and run
  its own code: JIT") and asks, as for every grant. Every starter gives the
  power from the list alone through `dp_root_rights` (the shell, `/svc/exec`,
  init's apps), and init's test run gives it only to an init.cfg program
  whose list asks (utest; posixtest and usbtest keep W^X whole). `pkg`
  doesn't exist yet on Jam OS (M13.7's PO6): the package verifier already
  allows `right jit` for a package (checkwants' rule for ports/), and
  init's pkgctl (PO5) and `pkg` must show the same words and ask, as
  `allow` does.
- **libc** (CJ2, done): `mmap` with **`MAP_JIT`** (macOS's flag name; the
  value is Jam OS's, `0x2000000`: macOS's `0x800` is Linux's
  `MAP_DENYWRITE`, which programs pass and expect ignored, and bit 25 is
  free on x86-64 Linux, the top one below the huge-page field
  `MAP_HUGE_SHIFT`, farthest from where Linux's flags grow) with
  `MAP_PRIVATE | MAP_ANONYMOUS` (else `EINVAL`) in a process holding the
  power maps a JIT VMO made for it (`vmo_make_jit`); then `PROT_EXEC` is
  allowed on it in `mmap` and `mprotect`, alone or with `PROT_WRITE`.
  Without the power `MAP_JIT` is `EPERM`, and execute anywhere else stays
  `EACCES`. The switching the plan asks for is `mprotect`: a program may
  keep the pages writable and executable (V8's default on x64) or move
  them between `PROT_READ | PROT_WRITE` and `PROT_READ | PROT_EXEC`
  (macOS's per-thread `pthread_jit_write_protect_np` is for arm64 and has
  no x86 use until the stricter modes below). `MAP_FIXED | MAP_JIT` over
  JIT memory decommits in place (a reservation cut and committed again
  keeps one VMO); a `MAP_FIXED` without it replaces JIT memory with plain
  memory. `MAP_JIT` is in `<sys/mman.h>` through an override of musl's
  `arch/x86_64/bits/mman.h`, and an override of its `src/mman/mmap.c`
  keeps the `EPERM` (upstream turns an anonymous mapping's `EPERM` into
  `ENOMEM`, a fix-up for Linux) (user/posix/musl/OVERRIDES).
- **V8** (still to do, with CB5): V8 already tells its page allocator
  when a reservation will hold code (`kNoAccessWillJitLater`, made for
  macOS's `MAP_JIT`); a few-line patch in its POSIX platform passes
  `MAP_JIT` on Jam OS too (and its decommit of code pages, a `MAP_FIXED`
  remap, must keep `MAP_JIT`, or the pages become plain memory).
- **Only renderers** (still to do, with CB5): the owner's default is
  Chromium's renderers, nothing else. The browser holds the power (its
  list asks for it) and hands it to renderer children only (the launcher
  branch's grants for `--type=renderer`); every other Chromium process
  runs V8 with `--jitless`.
- **Stricter modes later** through the same power: a per-thread switch
  with Intel's protection keys (the PC has PKU; the kernel would enable
  PKRU in XCR0 and give `pkey_alloc`/`pkey_mprotect`), or dual mapping
  (a write-only view and an execute-only view of the same VMO, which the
  rights already allow) with a V8 patch.
- **A security review of its own** (CJR) before CR-e is used on the PC.

### 13. Jam OS's sandbox for Chromium (later: CR-f)

The first builds run `--no-sandbox` (the owner's choice). The design to
come, from Fuchsia's per-type policy and the BSDs' `pledge`: Chromium
still believes it runs without a sandbox, and **Jam OS's launcher branch
applies a per-type table at spawn**: a renderer gets **its own job** (a
child of the browser's, with limits on pages, handles and threads) and a
namespace of nothing but what it needs (no `/jam`, no `svc net`, no `svc
wayland`; its Mojo socketpair; fonts through the browser's font service,
which Chromium already has for sandboxed Linux renderers); the GPU
process gets `svc wayland` and nothing on disk; the network service gets
`svc net` and `svc dns`. General pieces: a spawn attribute for "a new
child job with these limits" (`posix_spawnattr_setjob_np`, new) beside
M13's grants attribute. A renderer that is compromised then holds
handles that reach nothing but its own channel. Its own plan when CR-d
works.

### 14. The Chromium recipe

**Source**: Chromium **155.0.8059.39** (Alpine's stable as of
2026-10-11), the Linux source tarball without `.git` that Alpine uses
(from the chromium-linux-tarballs project's release for that version;
its SHA-256 in the recipe, mirrored on the Mac like every distfile).
tools/clang/scripts/update.py and tools/rust/update_rust.py fetch the
pinned host toolchains (section 1). The version moves when Alpine's
stable does (the owner's rule); each move is a recipe change, its
patches rebased.

**Patches, from Alpine** (copied with their headers; each one's reason in
the recipe's lint): the musl ones: `no-execinfo.patch`,
`no-mallinfo.patch`, `no-res-ninit-nclose.patch`,
`temp-failure-retry.patch`, `pvalloc.patch`,
`disable-dns_config_service.patch`; and copium's
`cr140-musl-prctl.patch`, `cr143-libsync-__BEGIN_DECLS.patch`,
`cr145-iwyu-dev_t.patch`, `cr145-musl-unfortify-SkDescriptor.patch`,
`cr146-unfortify-blink-display_item_list.patch`,
`cr147-is-musl-libcxx.patch` (the `is_musl` argument),
`cr149-musl-alloc-shim-dispatch.patch` (PartitionAlloc's malloc shim on
musl), `cr149-rust-toolchain-var.patch`. The seccomp sandbox's four
(`musl-sandbox.patch`, `musl-tid-caching.patch`,
`no-sandbox-settls.patch`, `musl-conditional-tkill.patch`) only if the
code they touch must still compile. Not taken: unbundling, Qt, PipeWire,
FFmpeg's codec lists for proprietary codecs, the test and tooling fixes
for Alpine's own builders.

**Patches, Jam OS's own** (each small, each general in what it asks of
Jam OS):
1. The GN toolchain and config for Jam OS (section 1): a new
   `build/toolchain/jamos` directory and its config (static, non-PIE,
   `posix.ld`, the program's list as a note), the Rust triple line.
2. The `posix_spawn` launcher branch (section 5), under
   `use_posix_spawn_launcher`.
3. Later: the audio back end (section 11), `use_jamos_audio`.
4. Later: `MAP_JIT` in V8's POSIX platform (section 12).
5. Later: the per-type spawn table (section 13).
6. Whatever CB2 and CB3 find that no general layer can answer (each one
   reported first: the rule is to fix the layer, not to patch around it).

**args.gn for `content_shell` and `headless_shell` first** (the plan's
guess: [CR-RESEARCH section 8](CR-RESEARCH.md#8-the-build)'s lean set plus
the toolchain's; the set CB1 built with is ports/chromium/files/args.gn,
and [CB1 as built](#cb1-as-built-2026-10-14) says what changed and why):

```
target_os="linux" target_cpu="x64" is_debug=false is_component_build=false
is_official_build=false symbol_level=0 dcheck_always_on=false
is_clang=true use_lld=true use_custom_libcxx=true is_musl=true enable_rust=true
custom_toolchain="//build/toolchain/jamos:x64" use_sysroot=true
target_sysroot="<the port's sysroot>"
use_posix_spawn_launcher=true
treat_warnings_as_errors=false is_cfi=false use_thin_lto=false
chrome_pgo_phase=0 disable_fieldtrial_testing_config=true
icu_use_data_file=false v8_use_external_startup_data=false
ozone_platform="wayland" ozone_platform_wayland=true ozone_platform_x11=false
ozone_platform_headless=true use_wayland_gbm=false
use_gtk=false use_qt6=false use_glib=false use_dbus=false use_udev=false
use_gio=false use_vaapi=false use_pulseaudio=false use_alsa=false
use_cups=false enable_pdf=false enable_printing=false
enable_swiftshader=false use_dawn=false
use_nss_client_certs=false use_nss_server_certs=false use_kerberos=false
proprietary_codecs=false ffmpeg_branding="Chromium" enable_widevine=false
```

Then **`chrome`**: the same plus `enable_pdf` and `enable_printing` back
on if they build (PDF viewing is ordinary browsing; printing has no
printer anyway), `enable_extensions` as default, and the package's
initial preferences file (Q4). Targets: `base_unittests`,
`headless_shell`, `content_shell`, then `chrome`.

**The recipe** (ports/chromium, new): `style custom`, `toolchain clang`,
the build host Linux (it runs only where ports.py runs on WSL), and a
new builder option for huge ports, **a kept tree**: the unpacked,
patched source and the output directory stay between builds (keyed by
the recipe's stamp of source and patches), so a change to args or one
patch rebuilds incrementally; a clean build is one flag. Programs:
`chrome` (or `content_shell` first) with its list (`svc wayland`, `svc
net`, `svc dns`, `/jam` read-write for the profile, `svc audio` from
CA1, `right jit` from CJ2); files: the `.pak` resources, the locales,
the licences (Chromium's `about:credits` list and every third-party
licence file its build names), initial preferences.

### 15. The build on the PC

**The PC check (the first step, CT1)**: before anything is fetched, a
script run on WSL through `tools/remote-test.sh` (tools/cr/hostcheck.sh,
new) prints and checks: free disk under the build directory (**150 GB**
wanted), WSL's memory cap and swap (`free`), CPU threads, the kernel
version, and the host tools Chromium's build needs (Python 3, `gperf`,
`ninja`, `pkg-config`, `bison`, `flex`, `xz`; Node.js: decided by CB1,
the tarball brings none, so Chromium's own pinned Node.js from its DEPS,
pinned in tools/cr/toolchain.pins like its clang). A shortfall stops the track with the numbers (and Q1's
answer says what the owner may change on Windows).

**Budget** (the plan's estimate; CB1's measured figures are in [CB1 as
built](#cb1-as-built-2026-10-14) and BENCH.md):
| | Estimate |
|---|---|
| Disk | source about 10 to 15 GB unpacked, Chromium's clang and Rust about 3 GB, each output directory at `symbol_level=0` about 15 to 25 GB; two configurations (test targets, `chrome`) and room for a rebase: **about 80 GB used, 150 GB free asked** |
| RAM | compiling Blink and V8 files takes up to 1 to 2 GB a job; lld's link of `chrome` at `symbol_level=0` about 8 to 12 GB. WSL's default cap is half the PC's 32 GB: **raise it to 24 GB with 16 GB of swap** (Q1), or cap the jobs at 12 |
| Time | `content_shell` from scratch about 3 to 4 hours on the i7-14700's 28 threads ([CR-RESEARCH](CR-RESEARCH.md#8-the-build)); `chrome` about 1 to 1.5 hours more (most objects are shared); an incremental rebuild after one file minutes; the final link of `chrome` 2 to 5 minutes |

**Where it runs**: a persistent tree on WSL, `~/cr/` (one per Chromium
version), outside the per-checkout copies `tools/remote-test.sh` makes,
behind its own lock (one Chromium build at a time on the machine). The
build runs at low priority (`nice`) and takes several of remote-test's
slots, so QEMU timing tests aren't starved (the main session's
CPU-weighted slots, planned for 2026-10-12, are a gate for CB1). An agent
starts a build and waits in 4.5-minute chunks (AGENTS.md's rule); a
clean build is the main session's to start, at a quiet time.

**How runs reach QEMU**: tools/cr-test.sh (new), run on the PC through
`tools/remote-test.sh`, makes a test disk with the binaries straight
from `~/cr/out/<config>/` (nothing copied back to the Mac), approves them
in the test image, boots QEMU with KVM, 4 to 8 CPUs and **8 GB of
memory** (`QEMU_MEM`), runs the case and checks its output: a line, a
PNG compared with an expected one (as `tools/comp-test.sh` does), or a
screenshot of the desktop. Cases: `hello`, `rust`, `base`, `headless`,
`shell-wl`, `multi`, `chrome`, each its own shard.

**How it reaches the stick and the PC**: as a package. The PC builds;
the Mac packages and signs (the ports key stays on the Mac): tools/ports.py
gains `package --from <dir>` (new), which packages a port's artifacts
built elsewhere from the same recipe and stamp, fetched back from the PC
by a remote-test output directory. The Mac serves it; `pkg install
chromium` on the PC puts it under `/jam`'s package prefix (M13.7's prefix
moved to `/jam` with M13.6: PO13 below). Before `pkg` and `/jam` exist,
the first hardware runs (CR-c on the PC) use `fetch` from the Mac's
`serve` and `allow` on `/data`.

### 16. What the other plans become

**Gates from the milestones under way** (each is in its own plan; CR
waits for the ones named in its tracks):

| Plan | On CR's path | Off it (after Chromium) |
|---|---|---|
| M13 | **F1b** (read-only shared file mappings; `posix_spawn` of programs on file systems); the futex error pass-through (done, 39aec118); R2 and J as planned (not gates) | M13.5 (mksh, `fork`, `vfork`) |
| M12.5 | **PF2** (coherence: SQLite's maps), **PX** (paged programs, which CP1 extends), **PR** (its review, before CR-d on the PC) | |
| M12.7 | stage 2: W2 and W3 (on their branch), its **R3** review, **P2** with the owner (the TEAM drive prepared) | stage 3 (the installer) |
| M13.6 | **U2a, U2b, U3, U4, U5, U6, U7a, U7b, U8, U10**, its review and join: `/jam` with rename-over, locks, symlinks, a pager | (all of it is on the path) |
| M13.7 | **PO4, PO5, PO6, PO7** (`pkg` on Jam OS), **PR** and **PJ**; plus two new: **PO13** (the package prefix on `/jam`, and big packages: a 250 MB program, staged `.new` files on `/jam`) and the builder's kept tree and `package --from` (in CB1's files) | PO9's remaining rungs (bzip2, libarchive), PO10 (make), PO11 (ncurses, vim), PO12 (the audit: wanted for Chromium's security updates, but after CR-d) |
| G2 | GS1, GC1b, GC2, GC3, GP1, GP2, GT, GR, GJ; GD if Q3 says the search box | GS2, GC4, the apps, the GTK3 spike |

**M13.7 trimmed**: its done-when's `pkg install vim` becomes `pkg install
chromium` (with wlprobe and the fonts as the small first packages); the
ladder past zlib and xz waits.

## Milestones

| Milestone | Done when | Tests | Needs |
|---|---|---|---|
| **CR-a. The toolchain runs** | a C++ program (exceptions, static constructors, 8 threads, `thread_local`, iostreams, `std::filesystem`) and a Rust program (threads, channels, files, a panic) built by the toolchain run from `/data` in QEMU and print what the host build prints; the syscall scan passes on both | cr-test.sh `hello`, `rust` | CT1, CT2 |
| **CR-b. `base_unittests`** | built for Jam OS and run with `--single-process-tests` in QEMU: a named list of suites passes (at least `base` threading, files, strings, time, memory, PartitionAlloc), and every failure is either fixed in a layer or a named finding with its layer | cr-test.sh `base` (the list in the recipe's test file) | CB1, CL1, CL3, CP1 |
| **CR-c. `content_shell` draws pages** | (c1) `headless_shell --single-process --no-sandbox --jitless` writes a screenshot of a local page from `/data` and of an HTTPS page, both matching; (c2) `content_shell --single-process` on Wayland shows a page in a Jam OS window, keys and mouse and wheel work, a `<select>` list opens; (c3) multi-process (`--no-zygote --no-sandbox`): browser, GPU, network and renderer processes, a renderer killed gives "Aw, Snap!" and a reload works | cr-test.sh `headless`, `shell-wl`, `multi`; on the PC by hand from `/data` | c1: CB3's needs; c2: G2's GS1, GC1b, GT (GC3 for the list); c3: CU1, CS1, CL2, PX, CN1 |
| **CR-d. `chrome` browses** | the [Done when](#cr-plan-chromium) above: on the PC from `/jam`, installed by `pkg` | cr-test.sh `chrome` in QEMU (a local HTTPS server on the test host, a page per feature); the PC check | CB6's needs: GC2, GC3, GP2, CU2, M13.6, M12.7 stage 2, M13.7's `pkg` and PO13, CA1 for sound |
| **CR-e. The JIT** | renderers run V8 with its compilers and WebAssembly under `right jit`; JetStream and Speedometer scores against `--jitless` in BENCH.md; every other process still `--jitless`; CJR's findings fixed | ktests (CJ1), posixtest (`MAP_JIT`), cr-test.sh `chrome` with a Wasm page | CJ1, CJ2, CJR |
| **CR-f. Jam OS's sandbox** | each renderer in its own job with a narrowed namespace; a test renderer that tries to open `/jam`, connect out or reach the compositor fails each; a renderer that eats memory hits its job's limit, not the system's | its own plan | CR-d |
| **Later** | the GPU (G3, G4: ANGLE and Vulkan, then WebGL), video beyond the open codecs (never H.264 in the open build), CJK fonts, input methods, drag and drop | | |

## The interfaces that change (for M14's review)

| Where | What |
|---|---|
| system calls | `vmo_make_jit` (new number); `VMO_MAX_SIZE` 128 TiB for plain paged VMOs; `VMAR_MAX_MAPPINGS` 65,536; `VMO_VERIFIED_MAX` 1 GiB; mappings merge (visible in `process_get_info`'s count only) |
| rights | `RIGHT_ROOT_JIT` (bit 27); write together with execute allowed on a JIT VMO's mapping only |
| `process_get_info` | a JIT flag |
| `<wants.h>`, `tools/checkwants.py` | `right jit` |
| startup roles | `SR_SELF_IMAGE` (a program's own executable VMO); argv and environment in a VMO when big (the startup message's format) |
| the POSIX hand-over (`user/include/posixfds.h`) | the program's path; the self-image; seals' rule (a write-sealed memfd travels without write) |
| libc | `/proc/self/exe`, `/proc/self/fd` (list, readlink, reopen); `SCM_RIGHTS` on socketpairs (every hand-over kind), `MSG_CTRUNC`, `MSG_CMSG_CLOEXEC`, `shutdown(SHUT_RDWR)` on a socketpair end; named `AF_UNIX` sockets through `/svc/unix` (CU2); `posix_spawn_file_actions_addclosefrom_np`; `MAP_JIT`; `MAP_FIXED` inside a private anonymous mapping decommits in place; `prctl(PR_SET_NAME)`, affinity, thread priorities; the ENOSYS log |
| netstack | the new caps (`user/include/net.h`) |
| libjam | the audio output API (CA1) |
| M13.7 | recipe lines `toolchain clang` and the kept tree; `ports.py package --from`; the prefix on `/jam` |
| new service | `/svc/unix`, the named-socket broker (CU2); `/svc/exec`, init's sealer (CE1: abi/idl/exec.idl, `svc exec` in a list) |
| libos | `<dataprog.h>` (the approvals, page tables and list reader the shell and init share); `spawn_args.before_start` (CE1) |
| tools | tools/jamos-clang, tools/jamos-clang++ (new) |

## Questions for the owner

**Decided** (the owner, 2026-10-11, in the [roadmap](ROADMAP.md#now)):
the Linux route with musl and Ozone/Wayland; each layer general, only
what Chromium needs first; Chromium run directly, no wrapper; early builds
without Chromium's sandbox for ordinary sites (not banking or logins that
matter) until Jam OS's sandbox lands and is reviewed; built on the PC's
WSL (checking disk and memory first); Alpine's stable version, moving
when Alpine does; a plain open build (no H.264 or AAC, no Widevine, no
Google API keys); JIT memory by capability (`right jit`, approved,
private W+X, handles delegable, stricter modes later, `--jitless` first);
Jam OS stays BSD-2, GPL and LGPL only in separate programs with source
shipped (Chromium is such a program; its LGPL parts are covered as G2's
Q8 says); ports by need; off the path: M14.5, a CA store, D-Bus, NSS,
SwiftShader and WebGL, M13.5's mksh, M14, M15, M10 and M10.5, M12.7's
stage 3, M12.1's follow-ups; M12.7's stage 2 and M13.6 on the path (so
Chromium lives on the Jam OS drive's `/jam`); G2's decoration rule (Q4:
clients that bind the manager get the compositor's title bar).

**Decided by this plan's recommendation** (technical, under the standing
rule; say if you'd rather otherwise): the launcher is a `posix_spawn`
branch in Chromium's launcher, not an emulated `vfork` (section 5); the
first builds have no sound, then a mixer back end (section 11); the JIT
power goes to renderers only, every other Chromium process `--jitless`
(section 12); programs link non-PIE until a static-PIE track (finding 6);
crash reporting off.

**Open**, each with the plan's recommendation (assumed until you answer):

### Q1. The PC as Chromium's build machine

Building Chromium needs about 150 GB free on WSL's disk, more memory than
WSL's default cap (half of the PC's 32 GB), and hours of all 28 threads.

- **(A)** Raise WSL's cap to 24 GB with 16 GB of swap (a `.wslconfig`
  line on the Windows side, yours to change), give the build 150 GB, and
  let clean builds run at low priority whenever the PC is in Windows,
  incremental ones any time.
- **(B)** As (A), but clean builds only when you say (they take the PC's
  CPUs for 3 to 4 hours).
- **(C)** Keep WSL's defaults and build with fewer jobs (about twice as
  long).

*Recommendation: (A).* Low priority keeps Windows usable; a clean build
is needed only when the version moves or the toolchain changes.

**Answered (the owner, 2026-10-12): (A).** The owner raises WSL's cap in `.wslconfig` on the Windows side; CT1 checks the disk and memory before the first build.

### Q2. Chromium's prebuilt toolchains

Chromium's build fetches its own clang and Rust, binaries built by
Google, the exact ones each Chromium version is tested with.

- **(A)** Use them on the build host; build the target's runtimes
  (compiler-rt, libunwind, libc++) from LLVM's source ourselves, so
  nothing Google built runs on Jam OS except what Chromium's own build
  compiles.
- **(B)** Build clang and Rust from source on WSL too (about 2 to 3 more
  hours at each version move, and our own pinning to keep in step with
  Chromium's).
- **(C)** Ubuntu's or apt.llvm.org's clang and rustup's Rust, as Alpine
  uses its distribution's (a version that may lag what Chromium needs).

*Recommendation: (A).* It is how Chromium is meant to be built; (B)
costs hours at every bump for the same compiler; (C) risks a version
mismatch every release.

**Decided (2026-10-12): (A)**, as recommended (a build-host choice, under the standing rule).

### Q3. How you start Chromium

- **(A)** From the shell (`chrome`, or `chrome <url>`).
- **(B)** From the search box too (G2's GD: a package's `desktop` line,
  approved with the package).
- **(C)** From the search box only.

*Recommendation: (B).* The shell first (it needs nothing new), the
search box once the package exists; GD is a small track.

**Answered (the owner, 2026-10-12): (B).** The shell first, then the search box: G2's GD is on the path.

### Q4. `chrome`'s window frame

Under your decoration rule, a client that binds the decoration manager
gets the compositor's title bar. `chrome` normally draws its tab strip as
its title bar; told `server_side`, it should switch to its "use system
title bar" look (our title bar above the tab strip). If it doesn't, it
shows two title bars.

- **(A)** Ship the package's initial preferences with "use system title
  bar and borders" on, so `chrome` asks for what the rule gives it: our
  title bar, then its tab strip.
- **(B)** Let `chrome` draw its own frame (an exception to the rule for
  clients that ask `client_side`).
- **(C)** Decide when you see it.

*Recommendation: (A).* It keeps your rule, matches every other window,
and the setting stays yours to change in `chrome`'s own settings.

**Answered (the owner, 2026-10-12): (A)**, and later a Mac-style frame
(see [Later](#later)). Never (B): the rule stays, every window keeps the
jam circles.

## Tracks

Each track is about one to three agent-hours, starts from main, owns the
files listed (an edit outside them stays minimal and is named in its
report; new files are written without a path check), commits refactors
and behaviour changes separately, adds a test for each behaviour (one
that fails without it), and hands back at each usable stage. QEMU tests
run on the PC only, through `tools/remote-test.sh` (AGENTS.md's rule):
the quick tier (the build, `make check`, the init run) plus the track's
own scripts, in parallel shards; kernel tracks also the full ktest at 4
and 8 CPUs and the soak. Long waits (a Chromium build) in 4.5-minute
chunks. **Agents never push, never touch main, `~/jamos` or a USB disk,
never ssh to the PC except through `tools/remote-test.sh`, and never start
agents of their own.**

Models follow the owner's split: **Opus** for the kernel, what crosses
between processes (descriptor passing, launching, the self-image), the
toolchain decision, the JIT, and every review; **Sonnet 5.5** for the
ports, recipes, tests, netstack's caps, the audio API and the docs.

| Track | What | Files it owns | Waits for | Tests | Model |
|---|---|---|---|---|---|
| **CT1. The toolchain** | section 1: the PC check (tools/cr/hostcheck.sh, new: disk, memory, threads, host tools; stops on a shortfall); the host toolchain fetch (tools/cr/toolchain.sh, new: Chromium 155's pinned clang and Rust into `~/.cache/jamos/toolchains`); ports/linux-uapi and ports/llvm-runtimes (new: builtins, libunwind, libc++, libc++abi for the target, static); tools/jamos-clang and tools/jamos-clang++ (new); `posix.ld` under lld; the syscall scan (tools/cr/syscall-scan.py, new); tools/cr-test.sh (new) with its `hello` case; the C++ hello (ports/cr-hello, new) | the new files named; `user/posix/posix.ld` (a line if lld needs it); the recipe line `toolchain` in tools/ports.py (minimal) | nothing (the owner's Q1 and Q2 shape it: recommendations assumed) | cr-test.sh `hello` in QEMU; the scan on the hello; `make check` | **Opus** |
| **CT2. Rust** (built 2026-10-14) | a Rust hello for `x86_64-unknown-linux-musl` linked by tools/jamos-clang against Jam OS's musl (`-C link-self-contained=no`; std built from rust-src with Chromium's Rust, or rustup's prebuilt std if that links cleanly: the track says which, and why); threads, channels, files, a panic | ports/cr-rust-hello (new), cr-test.sh's `rust` case | CT1's drivers | cr-test.sh `rust`; the scan | Sonnet 5.5 |
| **CK1. Big sparse VMOs** | section 2: the fourth table level for VMOs past 64 GiB, `VMO_MAX_SIZE` 128 TiB for plain paged VMOs, decommit, shrink and destroy skipping missing levels, the charges | `kernel/object/vmo.c` (the table code), `kernel/include/jam/abi.h` (the constant), `abi/syscalls.def`'s comment, new ktests | nothing (no other kernel track in `vmo.c` now) | ktests: a 1 TiB VMO costs one record; 1,000 scattered touches charge pages and tables exactly; a full decommit gives the pages back; destroy leaves the job at zero; a job at its limit refused at a touch; a whole-TiB decommit in under a millisecond; full ktest 4 and 8; soak 2 | **Opus** |
| **CK2. Mappings at scale** | section 2: the mapping tree with gap sizes, merging after map, protect and replace, `VMAR_MAX_MAPPINGS` 65,536 | `kernel/mm/aspace.c` (the list and its users), `kernel/include/jam/abi.h` (the constant, a different line from CK1's), new ktests | nothing (beside CK1: different files) | ktests: 50,000 alternating `vmar_protect`s over one mapping end as a few mappings; a fault's time flat with 60,000 mappings; replace inside a big mapping; the job's record charges exact after merges; full ktest 4 and 8; soak 2 | **Opus** |
| **CK3. The reverse map at scale** | section 2: a VMO's user mappings indexed by page range | `kernel/object/vmo.c` (the reverse map), `kernel/include/jam/aspace_vmo.h` | CK1 and CK2 merged | ktests: a VMO split into 10,000 mappings, a one-page decommit touches one; interrupts-off time measured before and after; full ktest 4 and 8; soak 2 | **Opus** |
| **CL1. Reservations in libc** | section 2's libc part: any size, one VMO; `MAP_FIXED` anonymous inside libc's own private mapping decommits in place; the tests of V8's and PartitionAlloc's patterns | `user/posix/sys/mem.c`, `user/posix/sys/maptable.c` if needed, posixtest's `mem` section | stage 1 (up to 64 GiB) at once; stage 2 after CK1 and CK2 | posixtest `mem`: section 2's cases; libc-test unchanged | **Opus** |
| **CL2. `/proc/self` and the self-image** | section 3's `/proc` and section 6's self-image: the program's path in the hand-over and libjam's spawn, `SR_SELF_IMAGE` from every starter, `/proc/self/exe` (readlink, and spawn from the image), `/proc/self/fd` (list, readlink, reopen with the read-only twin), ENOENT for the rest; built, see "CL2 notes" | a new user/posix/sys/procfs.c, the path lines in `user/posix/sys/path.c`, `user/posix/sys/spawn.c` (the self branch), `user/include/posixfds.h`, `user/include/roles.h`, `user/lib/spawn.c` (the path and the role), the shell's and init's start of programs (the role, a line each) | nothing for `/proc`; the self-image of a `/data` program after M12.5's PX | posixtest: readlink of `/proc/self/exe` from a boot-image program and a `/data` one; a spawn of `/proc/self/exe` (shared pages: the child adds no text pages to the job); a memfd reopened read-only can't be mapped writable; `/proc/cpuinfo` ENOENT | **Opus** |
| **CL3. Raw calls and the ENOSYS log** | section 3's log and calls: `prctl` names, affinity, thread priorities, futex operations checked, `membarrier` | `user/posix/sys/calls.def` and the handlers' files it touches (`user/posix/sys/misc.c`, `user/posix/sys/thread.c`), a new log file in user/posix/sys/ | nothing | posixtest: each call's cases; the log names an unknown call once | Sonnet 5.5 |
| **CU1. Descriptor passing on socketpairs** | section 4: the framing, every hand-over kind on GS1's tables, more than 64 descriptors, `MSG_CTRUNC`, `MSG_CMSG_CLOEXEC`, `shutdown(SHUT_RDWR)`, seals travelling and the write-seal rule | the translation file GS1 made (widened), `user/posix/sys/pipe.c` (the socketpair's send and receive), `user/posix/sys/sockio.c`, `user/posix/sys/sockconn.c` (shutdown), `user/posix/sys/memfd.c` (the seal rule), posixtest's socketpair section | G2's GS1 merged | posixtest: 1 byte with 64 descriptors, 128, 254 (EINVAL); every kind passed and used on the other side (a pipe written, a memfd mapped, a file read at its offset, an eventfd signalled); the read-only twin passed on stays read-only; too little room; a peer gone mid-send; Mojo's pattern (non-blocking, partial writes, descriptors with the first byte) between two processes; a fuzz loop of mutated headers (no crash, no handle leaked) | **Opus** |
| **CU2. Named `AF_UNIX` sockets** | section 4's broker, `/svc/unix`: `bind`, `listen`, `accept`, `connect` on socket files and abstract names | the broker (new, user/services/), its IDL (new), `user/posix/sys/socket.c`'s `AF_UNIX` paths, init's start of it | CU1; M13.6's U4 (socket files on `/jam`) | posixtest: a listener and two connectors, a socket file removed, a connector without the file's view refused, abstract names; utest for the broker's limits | **Opus** |
| **CE1. The exec sealer** | `/svc/exec` in init: a program `posix_spawn`s an approved `/data` program (its own open of the file; init checks the allow line and page table, seals the file's pages with `vmo_make_exec_verified`) and the child gets its own approval only, never its parent's (the owner's decision, 2026-10-12); built, see "CE1 notes" | `abi/idl/exec.idl` (new), `user/services/init/exec.c` (new), `user/include/dataprog.h` + `user/lib/dataprog.c` (PX's helpers moved from the shell), `user/posix/sys/spawn.c` (`file_program`), `user/posix/sys/proc.c` (a child's job for SIGKILL), `user/lib/spawn.c` (`before_start`), utest `execsvc.c` with `user/tests/exectest` and `user/tests/execwide` | M12.5's PX, M13's F1b, R2 | utest `exec_*`; `tools/posix-mmap-test.sh` case `spawn` (the allowed run starts and exits 7); `allow.txt` and pager-test `exec` unchanged | **Opus** (a trust boundary) |
| **CS1. Launching** | section 5: `addclosefrom_np`, argv and environments in a VMO, `/proc/self/exe` spawns through CL2's image, a child's argv of 64 KiB | `user/posix/sys/spawn.c`, `user/posix/sys/spawnfd.c`, `user/lib/spawn.c` (the VMO), the POSIX start code (`user/posix/crt/`) | CL2's self-image part (or beside it, merged after) | posixtest: a child with 500 arguments and 64 KiB of environment sees them all; `addclosefrom_np` leaves exactly the remapped descriptors (checked through `/proc/self/fd`) | **Opus** |
| **CP1. Programs this big** | section 6's limits: `VMO_VERIFIED_MAX` 1 GiB, `PROGRAM_MAX`, `FILE_MAX`; `allow`'s progress line; a 300 MB generated test program; built, see "CP1 notes" | `kernel/include/jam/abi.h` (the constant), `kernel/object/vmo_verify.c` if a size is assumed there, `user/services/shell/sh_allow.c` (the constant and the line), `user/lib/spawn.c` (the constant), a generated test program (new) | M12.5's PX merged (it rewrites `sh_allow.c`) | ktest: a 1 GiB verified table; the shell's allow script with the 300 MB program (allowed, run, a changed page refused); cr-test.sh `hello` with the big program | **Opus** (small) |
| **CN1. netstack's caps** | section 8: the new caps, netstack's memory measured at them | `user/include/net.h`, `user/services/netstack/stack.h` and lwIP's options file if pools must grow, BENCH.md's line | nothing | utest: 256 sockets from one opener, the 257th refused; 512 in all; 1,024 TCP connections to the test host; `tools/net-test.sh` unchanged | Sonnet 5.5 |
| **CB1. The Chromium build** | section 14 and 15: ports/chromium (the recipe, Alpine's patches, our toolchain patch, args for `base_unittests` and `headless_shell`), the builder's kept tree and `package --from` in tools/ports.py, the persistent tree on WSL behind its lock; builds `base_unittests` and `headless_shell`; reports disk, peak memory and time | ports/chromium (new), tools/ports.py (the two options), tools/cr/ (the build wrapper, new) | CT1, CT2; the main session's CPU-weighted slots in `tools/remote-test.sh` | the build finishes; the syscall scan passes (or lists each site); `make check` (the recipe's lint) | **Opus** (the build glue decides much) |
| **CB2. `base_unittests` (CR-b)** | cr-test.sh's `base` case; the run with the ENOSYS log on; every failure triaged to a layer (a finding for its track) or fixed in the recipe's test list with a reason | cr-test.sh's case, ports/chromium's test list | CB1, CL1 (stage 1 at least), CL3, CP1 | cr-test.sh `base` | Sonnet 5.5 |
| **CB3. `headless_shell` (CR-c1)** | the single-process headless screenshot of a local page and an HTTPS page; the findings | cr-test.sh's `headless` case, the recipe's args | CB2; CL1 stage 2 (V8's cage); M13's F1b (the `.pak` maps) | cr-test.sh `headless` | Sonnet 5.5 |
| **CB4. `content_shell` on Wayland (CR-c2)** | `content_shell --single-process` on the desktop; keys, mouse, wheel, a `<select>` list; screenshots | cr-test.sh's `shell-wl` case | CB3; G2's GS1, GC1b, GT (GC3 for the list), GP2 | cr-test.sh `shell-wl` | Sonnet 5.5 |
| **CB5. Multi-process (CR-c3)** | the launcher patch (section 5), `--no-zygote --no-sandbox`, every process type started and talking over Mojo; a renderer killed | ports/chromium's launcher patch, cr-test.sh's `multi` case | CB4; CU1, CS1, CL2, CN1; M12.5's PX | cr-test.sh `multi` | **Opus** (the patch crosses processes) |
| **CB6. `chrome` (CR-d)** | the `chrome` target, its args, its package (programs, paks, licences, initial preferences: Q4), the profile on `/jam`, the singleton on CU2; `pkg install chromium` | ports/chromium (the target's lines), cr-test.sh's `chrome` case | CB5; GC2, GC3, CU2, CA1 (sound), M13.6 (its tracks in section 16), M13.7's `pkg` and PO13 | cr-test.sh `chrome`; the PC check | Sonnet 5.5 |
| **PO13. The package area on `/jam`** | M13.7's prefix on `/jam` (after M13.6), the guard on it, big packages (a 250 MB program: staged files on `/jam`, init's hashing in steps) | M13.7's files named in its plan's PO5 and PO6 notes (the prefix constant, the guard's line for extfs) | M13.7's PO5 and PO6; M13.6's U4 and U8 | tools/pkg-test.sh (PO8, to come) with a big package on an ext test disk | Sonnet 5.5 |
| **CA1. Sound** | section 11: libjam's audio output API and Chromium's back end patch | a new user/lib/ audio file and `<os.h>`'s lines (or a header of its own), ports/chromium's audio patch | CB4 (a browser to test it with); the API part at once | utest: a stream's frames reach the mixer (`tools/mixer-test.sh`'s way); cr-test.sh `chrome` playing a WebM's sound into the mixer's capture | Sonnet 5.5 |
| **CJ1. JIT memory in the kernel** | section 12's kernel part: `RIGHT_ROOT_JIT`, `vmo_make_jit`, W+X only on a JIT VMO's mapping within its handle's rights, the flag in `process_get_info` | `kernel/object/vmo.c` (the kind), `kernel/abi/vmo_sys.c` (the call), `kernel/mm/aspace.c` (`perms_ok` and protect), `abi/syscalls.def`, `kernel/include/jam/abi.h`, new ktests | CK2 merged (`aspace.c`); CK1 merged (`vmo.c`) | ktests: W+X refused on every other VMO; a JIT VMO without the power refused; an execute-only duplicate can't write; a write-only one can't run; a JIT VMO can't be sealed, verified or paged; full ktest 4 and 8; soak 2 | **Opus** |
| **CJ2. JIT for programs** | section 12's rest: `right jit` (wants, checkwants, allow's and pkg's words, init's slice), `ps`'s mark, libc's `MAP_JIT` (this half done 2026-10-14, see "CJ2 notes"); **still to do, after CB5**: V8's patch, the launcher giving the power to renderers only, `--jitless` for the rest | `user/include/wants.h`, the wants parser in libos, `tools/checkwants.py`, `user/services/shell/sh_allow.c`'s prompt, init's grant, `user/posix/sys/mem.c` (`MAP_JIT`), ports/chromium's V8 and launcher lines | CJ1; CB5 | posixtest: `MAP_JIT` with and without the power; a JIT program writes code and runs it; cr-test.sh `chrome` with a Wasm page (CR-e) | **Opus** |
| **CR1. Kernel review** | CK1, CK2, CK3, CP1's kernel line, CJ1 if merged: findings first (docs/CR-REVIEW-KERNEL.md, new), then High and Medium fixed with a test each; hostile tests (a TiB of reservations from a job with 16 MiB, 60,000 mappings in a storm of protects from 8 threads while another unmaps) | the files its fixes need | CK1 to CK3 merged | its own; full ktest 4 and 8; soak 4 | **Opus** |
| **CR2. POSIX and IPC review** | CL1, CL2, CL3, CU1, CU2, CS1: the descriptor passing (what crosses, what leaks, what a hostile peer can make the receiver do), the self-image (can it run anything the owner didn't approve?), `/proc`'s reopen | the files its fixes need | CL1, CL2, CU1, CS1 merged | its own; posixtest; a fuzz loop between two processes | **Opus** |
| **CJR. JIT review** | CJ1 and CJ2 against the owner's rule: can any program without the power get W+X, can a JIT VMO become approved code, can a handle hand-on widen rights; findings first, fixes with tests | the files its fixes need | CJ1, CJ2 merged | its own | **Opus** |
| **CRJ. The join** | ARCHITECTURE (Memory: reservations, JIT VMOs; Userland: `/proc`, descriptor passing, the self-image, the toolchain), the porter's guide (C++ and Rust ports, the clang drivers, big ports), USING (Chromium), TESTING (cr-test.sh, the build), BENCH (the build's figures, page loads, the JIT's), ROADMAP and HISTORY | the docs | CR1, CR2 merged; CB6 | `make check` | Sonnet 5.5 |

**Later** (not tracked yet): the Jam OS sandbox (CR-f, its own plan),
static-PIE for every program, `PR_SET_PDEATHSIG` exactly, the GPU.

## Order

**Wave 1, at once** (no gates; none touches another's files):
- **CT1** (the toolchain; first step the PC check),
- **CK1** and **CK2** (the kernel: `vmo.c` and `aspace.c`),
- **CL1** stage 1 (libc reservations up to 64 GiB),
- **CL2**'s `/proc` part, **CL3**, **CN1**,
- beside them from G2: **GS1**, **GC1b**, **GP1** (resumed); and the
  gates already queued: M12.5's **PF2** and **PX**, M13's **F1b**, M13.6's
  **U2a**, **U2b**, **U3**, M13.7's **PO4**, M12.7's stage 2 **R3**.

**Wave 2**: **CT2** after CT1's drivers; **CK3** after CK1 and CK2; CL1's
stage 2 after CK1 and CK2; **CU1** after GS1; **CS1** after CL2; **CP1**
after PX; **CB1** after CT1, CT2 and the CPU-weighted slots; G2's **GC2**
after GC1b, **GT** after GS1 and GP1.

**Wave 3**: **CB2** (CR-b) after CB1; **CB3** after CB2, CL1 stage 2 and
F1b; **CR1** (the kernel review) once CK1 to CK3 are merged; **CB4**
after CB3 and G2's pieces; **CB5** after CB4, CU1, CS1 and PX; **CR2**
once CL1, CL2, CU1 and CS1 are merged; **CA1**'s API part any time.

**Wave 4**: **CU2** after CU1 and M13.6's U4; **PO13** after PO5, PO6
and M13.6; **CB6** (CR-d) after all its gates; **CRJ**; the main
session's regression on the PC; the PC check.

**Wave 5**: **CJ1** after CK1 and CK2 (it may start in wave 3 if a
kernel slot is free), **CJ2** after CJ1 (its program half, done) and CB5 (its Chromium half), **CJR**, then CR-e on
the PC.

**Merge order rules**: the kernel tracks one at a time into `vmo.c` and
`aspace.c` (CK1 and CK2 first, then CK3, then CJ1); after each kernel
merge the main session runs `make syscalls` and commits what it writes;
`user/lib/spawn.c` is touched by CL2, CS1 and CP1 (merged in that order,
each rebasing); `tools/ports.py` is touched by G2's GP1 (host builds),
CT1 (the `toolchain` line) and CB1 (the kept tree, `package --from`),
merged in that order; ports/chromium is CB1's, then each CB track's in
turn (one Chromium track at a time: one build tree).

**Beside the other milestones**: their own plans' orders hold; this plan
only says which of their tracks are gates (section 16) and adds PO13.

## Tests and the sign-off

| What | Where |
|---|---|
| The toolchain | cr-test.sh `hello` and `rust`; the syscall scan on every Chromium build |
| The kernel's memory | ktests (CK1 to CK3, CJ1); CR1's hostile tests; full ktest at 4 and 8; the soak |
| libc | posixtest's `mem`, socketpair and `proc` sections; libc-test unchanged |
| IPC between processes | CU1's two-process cases and fuzz loop |
| netstack | utest's caps cases; `tools/net-test.sh` |
| Chromium | cr-test.sh `base`, `headless`, `shell-wl`, `multi`, `chrome` (QEMU with KVM on the PC, 8 GB, a local HTTPS server on the test host for pages) |
| What didn't change | every area script, the desktop scripts, G2's wl-port-test.sh |
| The bench | BENCH.md gains: the build's disk, memory and time; `content_shell`'s start to first paint; a page load (a local copy of a news page) on Jam OS and on Linux on the same PC; Speedometer and JetStream `--jitless` (CR-d) and with the JIT (CR-e); memory of a browser with five tabs |
| The regression | once after CRJ, on the PC: ktest at 4, the init run twice, every area script, the new scripts, the soak at 4 |

## The PC check

After `pkg install chromium` (CR-d), with the Mac's ports server running:

1. `chrome` from the shell: a window with our title bar and the tab
   strip under it (Q4's (A)); `chrome` from the search box if GD merged.
2. Type a URL and search terms in the omnibox (its suggestions list is a
   subsurface); open a news site, Wikipedia, GitHub over HTTPS; scroll
   with the wheel; open five tabs; a right-click menu; a `<select>` list.
3. Copy text from a page into a Jam OS terminal and back into a form.
4. Download a file; it is on `/jam` (`ls -l`).
5. Close `chrome` and start it again: history, the open tabs (if asked
   to restore) and a changed setting are there.
6. Kill a renderer from the shell (`ps` names it): its tab shows "Aw,
   Snap!", reload works; kill `chrome`: the desktop and the other windows
   unchanged.
7. Play a YouTube video (VP9 and Opus in the open build) with sound
   (CA1); its frames a second in the overlay noted for BENCH.md.
8. **All tests and `soak 10`** with `chrome` open on a page and the
   second stick mounted read-write, pulled and replugged (the sign-off
   tier).

## Size

- **Agents** (CR's own): CT1 3, CT2 1.5, CK1 3, CK2 3, CK3 1.5, CL1 2,
  CL2 2, CL3 1.5, CU1 3, CU2 2, CS1 2, CP1 1, CN1 1.5, CB1 3, CB2 2, CB3
  2, CB4 2, CB5 2.5, CB6 3, PO13 1.5, CA1 2, CJ1 2.5, CJ2 2, CR1 3, CR2 3,
  CJR 2, CRJ 1.5: **27 tracks, about 59 agent-hours**, plus the
  machine's build time (a clean build 3 to 4 hours, perhaps six clean
  builds over the whole plan, and many incremental ones). Seventeen Opus,
  ten Sonnet 5.5.
- **The gates' own work** (in their plans): G2 about 18 agent-hours,
  M13.6 about 19, M13.7's trimmed rest about 8, M12.5's PF2, PX and
  review about 6, M13's F1b about 2, M12.7's R3 and P2 about 4: about 57
  more. **About 115 agent-hours on Chromium's path in all.**
- **Lines**: about 2,500 in the kernel (CK1 to CK3 1,500, CJ1 400,
  tests); about 4,000 in libc and libjam (reservations 400, `/proc` 500,
  descriptor passing 800, named sockets 700 with the broker, launching
  400, the audio API 400, tests); netstack's caps small; patches to
  Chromium about 1,000 lines of ours (toolchain 200, launcher 300, audio
  500) plus Alpine's; recipes and scripts about 1,500.
- **Real time** (*unsure*, honestly): CR-a in the first day of agent
  work; CR-b in 2 to 3 days (the first clean build and its fixes);
  CR-c in 1 to 2 weeks (the kernel's memory, descriptor passing and G2's
  shim are its long poles); CR-d in **3 to 5 weeks** from now, gated by
  M13.6 and M12.7's drive as much as by CR's own tracks; CR-e about a
  week after; CR-f its own plan. Each estimate doubles if the hidden
  Linux assumptions (risk 5) are many.

## Risks

1. **The address space** (the top risk). Huge sparse reservations touch
   the kernel's core VM: the VMO table, the mapping tree, merging, the
   reverse map, and the fault path's locking around all of them. A bug is
   a crash or a hole in every process. Mitigation: three small kernel
   tracks one at a time, each with ktests and soaks; CR1's hostile tests;
   the real V8 and PartitionAlloc patterns in posixtest before Chromium
   runs. If V8's 1 TiB proves costly, V8 falls back to a smaller,
   partly reserved sandbox (8 GiB at least) on its own.
2. **One big binary in every process.** Without PX's shared paged text
   each renderer costs 200 MB of RAM and a full read; with it, the
   self-image shares the pages. PX's verified pages make every first
   touch of a page a SHA-256 (about 2 µs a page on the PC): a cold start
   touches tens of thousands of pages, so the first window may take
   noticeably longer than on Linux (BENCH.md measures it).
3. **The build itself**: a non-Linux sysroot under `target_os="linux"`,
   a custom toolchain, static non-PIE linking, Rust's std for it; 3 to 4
   hours a clean build; Alpine's patches rebased each release (Alpine
   does it for us first, which is why we track its version).
4. **W^X against V8's JIT**: `--jitless` is slow on compute-heavy pages
   and has no WebAssembly, so some sites break until CR-e.
5. **Hidden Linux assumptions** found only at run time: raw calls,
   `/proc` files, netlink, signals into a specific thread, `ioctl`s. The
   ENOSYS log and the syscall scan turn them into a list; each goes to a
   layer, never into a Chromium patch unless no layer can answer.
6. **Signals and faults**: V8's WebAssembly trap handler and its CPU
   profiler need a fault delivered to the faulting thread and a signal to
   a chosen thread; M13's signal thread does neither. Jitless (no Wasm
   traps) and no profiler avoid it until CR-e, where the trap handler is
   off (`--no-wasm-trap-handler`: bounds checks in code instead;
   *unsure* that Chromium's renderer honours it rather than enabling the
   handler itself).
7. **Memory on the PC**: a browser with tabs takes GBs; jobs and the
   kept memfds (G2's Q12) mean no overcommit beyond reservations. The
   PC's 32 GB is plenty; QEMU tests run with 8 GB.
8. **Storage and coherence**: SQLite with `mmap` on a filesystem whose
   mapped pages and writes disagree corrupts databases. libc answers
   ENODEV until PF2 and U10's coherence exist; never a copy.
9. **The decoration look** (G2's risk): `chrome` may show two title bars
   if it ignores `server_side`; Q4's (A) avoids depending on it.
10. **Security before the sandbox**: unsandboxed renderers can read what
    the browser's namespace holds (`/jam`, the network). The owner's rule
    (ordinary sites only) holds until CR-f, and the package's list keeps
    the browser's namespace to what it needs.
11. **The PC is shared**: builds and QEMU tests compete for its 28
    threads; the CPU-weighted slots and low priority keep tests' timing
    honest (a timing test that fails only during a build is rerun after
    it).

## CJ2 notes

Track CJ2's program half (2026-10-14); section 12 has the design as built.

- **What it touches**: libc's `mmap` and `mprotect` (user/posix/sys/mem.c,
  a table kind `MT_JIT`), musl's `bits/mman.h` (an override), `ps`,
  `allow`'s question, init's test run (the power by the list only), and
  checkwants' self-test. No kernel change: CJ1's `vmo_make_jit` and
  `PROCESS_FLAG_JIT` are all it needs.
- **Tests**: bin/jittest (user/tests/jittest, a POSIX program whose list
  says `right jit`, built by user/posix/posix.mk; the Makefile's
  `POSIX_TESTS` keeps it out of the native programs) writes code and runs
  it with the power, and checks the refusals without it (`jittest
  without`); posixtest's `jit` section runs without the power and starts
  `jittest without` as its child; tools/shell-tests/jit.txt runs both from
  the shell, `ps`'s mark, and a copy on `/data` through `allow`.
- **Still to do** (with CB5, when Chromium runs multi-process): V8's
  `MAP_JIT` patch, the launcher giving the power to renderers only,
  `--jitless` for every other process type, and cr-test.sh `chrome` with a
  Wasm page (CR-e). `pkg`'s words when PO5 and PO6 land (section 12).

## CE1 notes

Track CE1, `/svc/exec`, the exec sealer (2026-10-13). The owner decided
(2026-10-12) that a child started through it gets **its own approval
only**: the namespace and root powers its own allow line's list asks for,
whatever its parent holds, so a low-access parent may start a
higher-access approved child, and a high-access parent gives it nothing
of its own.

- **Why init starts the child, not the parent.** F1b's proposal had init
  answer a sealed VMO and the parent spawn it. Under the owner's rule that
  can't work: a parent can only grant what it holds (spawn's grants are
  the starter's namespace), and anything init handed it for the child (the
  namespace's channels, the root with the child's powers) the parent could
  keep and use itself, a wider hole than starting the child. So init makes
  the process: the parent never holds the child's authority, nor a
  process handle with `RIGHT_WRITE` (a thread made in a non-PIE program
  that holds more than its parent is the parent running code with the
  child's authority).
- **The protocol** (abi/idl/exec.idl): `exec.open(path, file, handover)`
  then, on the answered `exec_prog` channel, one `start(argc, strings,
  job, signals)`. Two steps so that a refusal (EACCES, ENOEXEC, ETXTBSY,
  ENOENT) comes before posix_spawn's hand-over moves anything out of the
  parent's table. `handover` is an empty channel end given with `open`
  (a channel end whose queue holds channel ends can't be sent): the parent
  writes the POSIX hand-over on its end, init moves it message for message
  onto the child's startup channel (spawn's new `before_start`). The child
  goes in a new job inside the parent's job (a kill of the parent's job
  takes it); the parent gets the process (wait, inspect, duplicate,
  transfer) and that job (`RIGHT_MANAGE`: libc's SIGKILL is `job_kill`).
- **What open checks**: a cleaned absolute path on `/data`; the caller's
  `file` is a channel whose `file.stat` id and size are those of the file
  init's own `/data` has at the path (its views decided what it could
  open); the path's approval (its last allow line) and its page table
  for this size; `file.map(FILE_MAP_EXEC)` of the caller's file (a VMO only
  init holds) sealed with that table; the list read from the sealed VMO
  with `vmo_read`, and the shell's refusals (`dp_wants_refused`). A forged
  file channel (a caller's own server) gains nothing: only a pager's VMO
  can be sealed (no program from `/data` can be a pager), and every page is
  checked against the approval of the path it names, so the worst it can
  do is start that approved program.
- **Not done**: no whole-read fallback (an approval from before page
  tables is refused until the shell has run it once and written its
  table; a `/data` without a pager is refused); one start per seal
  (Chromium's own children are `/proc/self/exe`, CL2/CS1's). The child's
  `SR_SELF_IMAGE` (its sealed VMO) is libjam spawn's, since CL2 ("CL2
  notes").
- **Limits**: 32 openers, 16 sealed programs waiting (4 per opener),
  dropped after 10 s unstarted; 12 KiB of argv and environment (spawn's
  2 KiB startup message refuses less, until CS1); 128 hand-over messages.
  A worker thread with its own port serves it, every call on a caller's
  channel with a 5 s deadline: a hostile caller can delay other callers by
  that much a request, never init's loop.
- **Charges**: the child's own allocations go to its job (inside the
  parent's); what spawn makes for it (the writable segments' copies, the
  stack's pages as they are touched) and the sealed VMO's table are
  charged to init's job, as a shell-started program's are to the shell's.
- **The init run** now gives volumes the pager power as shell mode does
  (main.c), so its `/data` supplies pages and can be sealed from; posixtest's
  mmap section there maps the file's own pages now. utest's `exec_*` tests
  (execsvc.c) copy exectest, execwide, pxtest and wantsession to `/data`,
  approve them as `allow` does and put `/data` back after; utest, started
  before `/data` is mounted, takes `/data` from volumes' watch for that.
- **Tests run** (2026-10-13, the PC): the init run (643 utests, posixtest
  PASS, the root job clean), `posix-mmap-test.sh` case `spawn` (the
  allowed run started, exited 7), `allow.txt`, `pager-test.sh` case
  `exec`, one ktest pass at 4.

## CP1 notes

Track CP1, programs this big (2026-10-14): section 6's limits raised to
1 GiB, the size of the biggest Chromium build with room to spare.

- **The limits**: `VMO_VERIFIED_MAX` (`kernel/include/jam/abi.h`) is 1 GiB;
  the shell's `PROGRAM_MAX` is libos's `DP_PROGRAM_MAX`
  (`user/include/dataprog.h`), now `VMO_VERIFIED_MAX` itself, so the
  approvals, the tables (`dp_table_read`) and the kernel agree; spawn's
  `FILE_MAX` (a program read from a file) is 1 GiB too. `vmo_verify.c`
  assumed no size beyond its comments.
- **What verification costs at 1 GiB.** The kernel's table is 32 bytes a
  page: 262,144 hashes in 2048 buddy pages (8 MiB), listed by a 16 KiB
  array (one 32 KiB heap block), charged to the job of the caller of
  `vmo_make_exec_verified` (the shell's, or init's for `/svc/exec`) and
  freed with the VMO; the shell keeps up to 8 such VMOs while their
  programs run, so 64 MiB of tables at worst. Starting one also reads the
  table file into a VMO of the same size, closed once the kernel has its
  copy. The pages themselves are the pager's, fetched as they are touched
  (a 300 MiB program's `lazy` run fetched 18 pages). Each fetch hashes
  its 4 KiB in the kernel, as before.
- **What `allow` costs.** The whole read still holds the whole file: it
  is read into a VMO only the shell holds, so the bytes approved are the
  bytes hashed, and that VMO lives until the owner answers, charged to the
  shell's job (1 GiB of memory for a 1 GiB program, on the PC fine; on a
  small machine the limit is memory, not the code). The hash is taken as
  the file is read (one pass, where it was a read then a hash), and the
  page table is a second pass over the same memory once the answer is yes,
  built in a VMO of its own (not the heap, whose pages would stay the
  shell's for good). A program not allowed yet is also read whole once to
  say why, as before. On the PC's QEMU the 300 MiB program was read and
  hashed in about 3 s.
- **The progress line**: a file of 32 MiB or more read whole (`allow`, or
  a run that falls back to the whole read) shows "checking <path> (N MiB):
  10% 20% ... 100%" while it is read; Ctrl+C stops it ("interrupted").
- **The test program** is generated, never kept: `user/tests/bigprog`
  holds its code and `table.S`, a table of `BIG_PAGES` pages (page i holds
  the word i 512 times, so a page from the wrong place shows), 16 in the
  boot image and 76,800 (300 MiB) in the Makefile's
  `$(BUILD)/bigprog/bigprog`, which only `tools/bigprog-test.sh` asks for
  (the assembler makes it in under a second).
- **Not done here**: packages still cap a file at 64 MiB
  (`tools/portslib/pkgformat.py`'s `FILE_MAX` and `FILES_TOTAL_MAX` of
  512 MiB, mirrored by `tools/pagetable.c`): a package of Chromium's
  size is PO13's ("big packages: a 250 MB program"). So cr-test.sh
  `hello` runs as it was (a small C++ program from a package, now through
  the raised limits); the 300 MiB program is `bigprog-test.sh`'s.
- **Tests run** (2026-10-14, the PC): ktest at 4 and 8 CPUs (543 each,
  with `pager_verified_max`: a 1 GiB VMO's table, entries at each table
  page's edges, a page at the far end taken and a changed one refused,
  2048 pages charged and freed; 7 ms); `bigprog-test.sh` (the lazy run
  fetched 18 of 76,807 pages, `check` read 1,201 table pages in 2.8 s,
  the changed page killed `check`, the shell lived); `allow.txt`; every
  `pager-test.sh` case and every `posix-mmap-test.sh` case; cr-test.sh
  `hello`; the init run (clean); make check.

## CL2 notes

Track CL2, `/proc/self` and the self-image (2026-10-14; the `/proc` half,
user/posix/sys/procfs.c, came first). Section 3's `/proc` and section 6's
self-image.

- **The program's path.** The POSIX hand-over's first message carries it
  (`exe_len` after the directory, `<posixfds.h>`; the old `reserved`
  word's first half, so the format's version stays 1). libjam's spawn
  names it for every program it gives a hand-over to: `spawn_args.path`
  when absolute, a boot-image program's as `/boot/<name>`; posix_spawn
  names it for a program init's `/svc/exec` starts (init passes the
  hand-over on as written). libc's `/proc/self/exe` takes it first; the
  old look (argv[0]'s file, then the boot image, compared byte for byte
  with the program in memory) stays for a program started without a
  hand-over (the shell's background runs, a native starter's). The path is
  the starter's word, as argv is: the starter chose the program, and
  nothing is run by it (below).
- **`SR_SELF_IMAGE`** (`<roles.h>`, 20): libjam's spawn gives it to every
  program it starts from a caller's VMO at offset 0 that can run
  (`RIGHT_EXEC`): a duplicate of that VMO with read, map, execute,
  duplicate and transfer, never write. That covers every starter of a
  program not in the boot image with no line of their own: the shell's
  runs from `/data` (the verified VMO it keeps for the instances), init's
  `/svc/exec` (the sealed VMO), libc's self-image children. A boot-image
  program has none: its file is `SR_BOOTFS`'s, by name. It is left out
  only when the extras fill the startup message (16 handles).
- **posix_spawn and execve of `/proc/self/exe`** (or `/proc/<our pid>/exe`;
  spawn.c's `self_program`): with an image, the child starts from a
  duplicate of it: no file read, no approval step, the read-only segments
  mapped from the same VMO, so its pages are fetched and checked once for
  every instance; the child gets the image and our path. Without one, the
  path as any other: a boot-image program's file, or a file on `/data`
  through `/svc/exec`. Our own path spelled out (`/data/x` from `/data/x`)
  is that file, not the image: what is there now under its approval now,
  as Linux runs the file at a path and `/proc/self/exe` the running one.
- **Can the self-image run something the owner didn't approve? No.**
  - *Only approved bytes*: the image is the VMO the process already runs
    from, a boot-image program's or one the kernel checks every page of
    against the approval it was sealed with; a page that doesn't match
    kills whoever touched it, and fat refuses to write a file while a VMO
    made for running it lives. Spawn puts into a child only the VMO that
    child runs, so no starter (shell, init, libc) hands a program another
    program's image, whatever path the hand-over names: a hostile parent
    that names another path only misleads its child's `readlink`, and its
    child's `/proc/self/exe` still runs its own pages.
  - *Never more authority*: a self-image child is ours, in our job, with
    our namespace or less (spawn's grants) and no root powers (posix_spawn
    passes none). Started by `/svc/exec`, we hold our own approval only,
    so our children hold that or less: the owner's rule for `/svc/exec`
    ("its own approval only") holds for them. Nothing in the image grants
    anything: it is pages.
  - *A revoked approval*: `allow -r` stops new starts by the shell and
    `/svc/exec`; a program running already keeps running (as before CL2),
    and its `/proc/self/exe` children with it, all in its job, so the
    shell's kill of it (Ctrl+C, `kill`) ends them all. A self-image start
    is the same program doing more of what it already does with its own
    authority (as more threads would); its own path spelled out is
    checked again and refused.
  - *What is left*: the image carries `RIGHT_TRANSFER` (the child gets it
    too, so a self-started program can start itself again). A program can
    therefore hand its own image to another process on purpose, and a
    receiver that chooses to run it runs that approved program's pages
    with the receiver's own authority. That needs both programs to do it
    deliberately, and is no wider than what a process can already do by
    handing another its address space (`SR_SELF_VMAR`): no unapproved byte
    runs, and nobody gains authority they didn't hold. Taking
    `RIGHT_TRANSFER` away would stop it at the cost of the second level
    (a self-started child's own `/proc/self/exe` would go through
    `/svc/exec`): a question for CR2 and the owner.
- **The tests**: posixtest's procfs section spawns `/proc/self/exe` from
  the boot image twice (the child's exe our path, no image; a child that
  reads every page of its code grows the job no more than one that
  doesn't, and less than the code's pages: posixtest's 162 against about
  95 of the child's own); its `self-image` section does the same from
  `/data/pt-prog` started by the shell and by `/svc/exec`
  (posix-mmap-test.sh `spawn`): the path, the image held and never
  writable, the child's image our very VMO (the same object id). utest's
  malformed hand-overs add a program's path that isn't there and one past
  the first message.

## CU2 notes

Track CU2, named `AF_UNIX` sockets (2026-10-15): `bind`, `listen`,
`accept` and `connect` on socket files and abstract names, through the
broker `/svc/unix` (bin/unix, user/services/unix; abi/idl/unix.idl has
the model). Where it differs from section 4's design:

- **A socket file is an ordinary file**, not a socket inode: the `fs`
  protocol has no `mknod` and FAT none at all, and Chromium's singleton
  binds its socket in a temporary directory (`/tmp`, which may be FAT),
  not only on `/jam`. `bind` asks the broker for a listener and a random
  128-bit key, makes the file with `O_EXCL` (anything there:
  `EADDRINUSE`; mode 0777 less the umask) and writes a magic and the key
  (24 bytes); `connect` opens the file for reading as any open does and
  gives the broker the key. So the namespace and its views decide who may
  reach a socket, as the design wanted, by whether the file can be read
  (Linux asks for write permission; a read-only view lets a connector in
  here). The broker never reads a file and holds no namespace. What
  follows: `stat` says `S_IFREG`, not `S_IFSOCK`; a program that read the
  key once can still connect after the file is unlinked (until the
  listener closes); a file whose listener is gone is a stale socket
  (`ECONNREFUSED`, unlink and bind again: Chromium's singleton does just
  that); a file that isn't a socket file is `ECONNREFUSED`, as Linux's.
- **Abstract names**: one table for the broker (Jam OS has one user), not
  per session; any program with `svc unix` reaches any abstract name, as
  on Linux within a network namespace. Autobind (`bind` of an empty name,
  `listen` unbound) is not built (`EINVAL`).
- **A connection is a channel pair** the broker makes (both ends
  `CHANNEL_BYTE_BOUND`, as `socketpair`'s): the connector's socket holds a
  socketpair end inside it (unixsock.c's framing passes every call to
  sockpair.c: `SCM_RIGHTS`, `MSG_PEEK`, `MSG_WAITALL`, `shutdown`, poll),
  the listener gets the other end as a `conn` event and `accept` makes it
  a plain socketpair end's descriptor. A connect never waits for accept.
  A named socket's connection is handed to a child, or sent with
  `SCM_RIGHTS`, as the socketpair end it holds; a listener is never.
- **The backlog** is the `conn` events the listener hasn't read (the
  kernel's `peer_queued`), clamped to 1..64: past it a connect is
  `EAGAIN` (Linux makes a blocking connect wait for room). A second
  `listen` keeps the first backlog (as the AF_INET path does).
- **Limits** (unix.idl): 64 openers, 128 listeners (32 per opener
  channel: libc keeps one per process), names of at most 107 bytes. What
  waits at a listener is charged to the broker's job until accept reads it:
  at most 64 small messages a listener.
- **Names**: `getsockname` of a bound socket says its path or abstract
  name; of a connection, and `getpeername`, `AF_UNIX` with no path, and
  `accept`'s address the same.
- **Who starts it**: init, in shell mode as a supervised service (`UNIX`
  in the services' order, after the file server; init keeps both ends of
  the shared channel), and in the init run before the programs, stopped
  after them (init/unixsvc.c). A restarted broker loses its listeners:
  `accept` is `ECONNABORTED`, the program must bind again.
- **For CR2**: a program with `svc unix` can fill the 128 listeners (32
  per opener channel, but it can open several channels with `svc_open`);
  any abstract name is anyone's to take first. A listener that never
  accepts costs the broker at most 64 queued messages.
- **Tests**: posixtest's `unix` section (in the init run;
  `tools/shell-tests/posix-unix.txt` alone, in shell mode); utest's
  `unix_names`, `unix_backlog`, `unix_limits` (the broker's protocol and
  limits). The `wayland` section's refusals changed with it: a bind to
  `/svc/wayland` is `EADDRINUSE`, `listen` unbound `EINVAL`, a connect to
  an empty path (an abstract name of NULs) `ECONNREFUSED`.

## Later

- **CR-f, the sandbox** (section 13): its own plan once CR-d works.
- **A Mac-style frame** (the owner, 2026-10-12, after Q4's (A)): tabs in
  the title bar row with the real jam circles, as on macOS. A Jam OS
  Wayland protocol any client may use: the client keeps a corner clear and
  names the parts of its top strip that move the window; the compositor
  draws the jam circles over that corner and handles their clicks (close,
  minimise, full screen as for every window). Chrome's side is a patch to
  its frame view (no buttons of its own, the corner kept clear, the
  protocol spoken), as its macOS frame does with AppKit. After CR-d.
- **Stricter JIT modes**: PKU per thread or dual mapping (section 12).
- **The GPU**: G3 (mode setting through the iGPU, likely Linux's DRM
  drivers as programs) and G4 (3D), then ANGLE over Vulkan or GL, WebGL,
  `wp_viewporter` and linux-dmabuf in the compositor; M14.5 if Mesa's
  drivers must be loaded.
- **Static-PIE** for every program (address randomisation): the loader
  takes `ET_DYN` with musl's self-relocating start.
- **An emulated `fork`** stays M13.5's; Chromium doesn't need it.
- **Security updates**: M13.7's PO12 audit with Chromium's CVEs, and a
  rebuild at each Alpine stable bump.
- **More**: CJK fonts, input methods, drag and drop, the microphone and
  camera, printing to a file, Chromium's DevTools profiler (with signals
  to a thread).
