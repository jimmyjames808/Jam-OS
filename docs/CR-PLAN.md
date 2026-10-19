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
- **No zygote** (`--no-zygote`). Children start through Chromium's own
  launcher, `fork` then `execvp`, on libc's `fork` and an `exec` in place
  ([FORK-PLAN](FORK-PLAN.md); the owner, 2026-10-16: no launcher patch).
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
  `-fPIE` (`-fno-pie` until MH); link with lld (`-fuse-ld=lld -static
  -nostdlib`, `--defsym=__jamos_image_base=` 0x400000 or
  `--jamos-image-base`'s, posix.ld, `--gc-sections`, `--eh-frame-hdr`,
  `--build-id`, `=none` until MH): crt1, crti,
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
and Rust, through the ports builder. **Both programs link** (2026-10-15,
track CB1b): `base_unittests` (76.7 MB: 56.8 MB of code, 1.7 MB of data)
and `headless_shell` (335.7 MB: 213.4 MB of code, 6.5 MB of data), with
the two programs they run, `test_child_process` (2.6 MB) and
`chrome_crashpad_handler` (6.3 MB), static and position-dependent through
Jam OS's link driver; the ports builder stages them. The syscall scan
passes `base_unittests` (105 `syscall` instructions, all in Jam OS's
stubs) and lists 13 sites in `headless_shell`: 12 in Crashpad's client
(`CrashpadClient::DumpWithoutCrash`, its signal handler's
`HandleOrReraiseSignal` and `RequestCrashDumpHandler::HandleCrashImpl`,
`ExceptionHandlerClient::SignalCrashDump`, `ScopedMmap::ResetMmap`,
`LoggingMunmap`: linux_syscall_support's inline calls) and `SyscallAsm`
(the seccomp sandbox's raw call), for CB3. Still open:
`ports.py package --from` refused the build, as the package format held
no file over 64 MiB (PO13 raised it to 1 GiB, 2 GiB a package: not yet
tried on this build's stage); `test_child_process` is in the recipe since
CB2, `chrome_crashpad_handler` is not yet (CB3's runs did not need it:
the log of `headless_shell --single-process` has no crash-reporting line;
whether a multi-process run starts one is CB5's to see).

The last round's link errors and their fixes (CB1b): base's `close()`
wrapper found libc's with `dlsym(RTLD_NEXT)` (patch 0910: the linker's
`--wrap=close` instead, so the FD-ownership check stays); Jam OS's libc
had no `dlopen()` family at all (it gained musl's static stubs, as a
static Linux program has them: `dlopen()` fails with "Dynamic loading not
supported"); Crashpad's upload transport wanted libcurl (args.gn:
`crashpad_http_transport_impl = "socket"`, BoringSSL for https);
`SafeSPrintfTest.EmitNULL` can't compile where `NULL` is `nullptr`
(Alpine's patch, 0007, leaves it out); `StackTrace`'s stream output had
no definition without `backtrace()` (0911, since replaced: libc has
`<execinfo.h>`, see "BT notes"); Chromium's fontconfig named a
libuuid it doesn't call (0912); FFmpeg's build links the system's libz
(the recipe `needs zlib`). `tools/cr/build.sh`'s detached build now gets
`CR_JOBS` and `CR_MEM_MB` (the keeper's environment had neither, so
ninja ran `-j 10`).

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
  The launcher patch (2) is dropped: Chromium's launcher runs on libc's
  `fork` ([FORK-PLAN](FORK-PLAN.md)).
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

#### CB2 as built (2026-10-16)

Track CB2, `base_unittests` on Jam OS (CR-b): `tools/cr-test.sh base` runs
every unit test of base, one program for each suite, in QEMU on the test PC
with the ENOSYS log on. **base_unittests runs**: 77 MB from `/data`, started
in about a second (it runs from its pages). Of the 8,956 tests in 778 suites,
8,277 pass, 39 skip themselves and 640 are on the skip list
(`tools/cr/base_unittests.skip`: 475 named tests and the 165 of
`FilePathWatcher*`, each with its layer and reason). `base` leaves those out,
and with the list the six shards of `base` all PASS (2026-10-16, main at
6af21c8a plus this track): a PASS means every test that is not on the list
passed. The findings below are the list's groups, most tests first.

- **How the programs get onto the test disk.** The package format holds no
  file over 64 MiB (PO13), so nothing is packaged: `tools/cr/base-stick.py`
  copies the image, grows its `/data` to 6 GiB (640 MiB until CBF6's
  `DriveInfoTest` wanted more; see CBF6 notes, "The stick is 6 GiB") and writes `/data/cr/out/jamos/base_unittests` and
  `test_child_process` (from `~/cr/ports/chromium/stage`, built by
  `tools/cr/build.sh`; the recipe lists `test_child_process` now), their
  approval lines in `/data/etc/allow` and their page tables in
  `/data/etc/allow.d/<sha-256>` as the shell's `allow` would have made them
  (`tools/portslib/pagetable.py` stops at 64 MiB, so the stick's writer has
  its own). Base's test files go beside them (`/data/cr/base/test/data`,
  three metrics XML files, two generated ones): the source root is two
  directories above the program's own (`DIR_SRC_TEST_DATA_ROOT`), hence
  `out/jamos/`. **A static program is linked against its build's libc**: after
  the start protocol changed (CS1, CU2, U6) the stage's old `base_unittests`
  died with a general protection fault in `ids_init_locked` at its first
  instruction. After a libc change, remove it from the kept tree and
  `tools/cr/build.sh start base_unittests test_child_process` (2 minutes).
- **How the tests run.** `tools/cr/base-run.py`. A boot lists the tests (the
  program writes `--gtest_list_tests` to a file and the host reads it from
  the saved stick: ten thousand lines through the serial line come out
  garbled). A boot then runs each suite as a program of its own
  (`--single-process-tests --gtest_filter=<suite>.*`), typed into the shell
  by `tools/serial-feed.py` with `JAMOS_ENOSYS_LOG=1`; and rounds run only
  the tests that have no result, six to a program, then two, then one, so a
  crash costs its own test and one boot, not its neighbours. A hang ends
  the script (a wait of 240 s), and the driver ends QEMU itself when
  serial-feed's log says so. A suite's shard is its name's checksum.
  Results are in `<outdir>/base/run/`: `results.txt`, `failures/<test>.txt`
  (the test's output and the kernel log lines that came with it),
  `enosys.txt`, `summary.txt`. A shard takes 4 to 11 minutes with the list,
  and 5 to over 60 without it (`CR_BASE_ALL=1`: a crash is a boot, and
  every `FilePathWatcher` test waits out its timeout). `CR_BASE_ARGS` adds
  arguments for the program (`--v=1`, `--gtest_death_test_style=fast`).
- **No `/tmp`** (when CB2 ran; init's `/tmp` came after, see [CB2b](#cb2b-on-ext-2026-10-16)).
  `ls /tmp` said "no such file or directory": every Chromium
  program expects one. The run points `TMPDIR` (base) and `TEST_TMPDIR`
  (gtest's captured streams and death tests, which use `/tmp` whatever
  `TMPDIR` says) at `/data/cr/tmp`. Without `TEST_TMPDIR` a death test
  dies earlier, in `CaptureStderr`'s `mkstemp`.

**Findings.** The causes marked "seen" were seen in the log; "?" is a guess
from the failing call.

1. **POSIX layer: a process `clone` and `fork`. 281 death tests**
   (`*DeathTest.*` and every test that expects a `CHECK` to abort:
   `SpanTest`, `CheckDeathTest`, `ByteSizeTest`, `FeatureListTest`,
   PartitionAlloc's death tests, and the rest). gtest makes a death test's
   child with `fork()` (`--gtest_death_test_style=fast`) or
   `clone(SIGCHLD)` (`threadsafe`: `gtest-death-test.cc:1329`; the style the
   default runs use, since they log no fork), after `CaptureStderr()` has pointed stderr at
   a file; so the abort that follows prints nothing, and the program is
   killed by SIGABRT about 0.8 s after it starts. Seen: with the fast style
   `ClampTest.Death` logs `syscall 57 (fork)` (`ENOSYS`); with the default,
   every death test aborts the same way and no fork is logged, because
   `__jam_clone` returns `-ENOSYS` for a process clone (`thread.c:166`) without
   a log line. [FORK-PLAN](FORK-PLAN.md) answers it (`clone(fn, stack,
   SIGCHLD)` for gtest's threadsafe death tests, `fork` for the fast style:
   FL1b); until FL1b merges these stay skipped. Small and in the layer now:
   `__jam_clone` should say what it refused.
2. **POSIX layer: a user pointer that faults kills the process. 29 tests**
   (`BackupRefPtrTest` 23, `ProtectedMemoryTest` 4, `CPUDeathTest`,
   `ProtectedMemoryDeathTest`). Seen: the page fault ("page fault at rip
   ..., address <a page of the program's `protected_memory` section>") is the
   store in `lx_prlimit64` (`rlimit.c`, `movups` through the fourth
   argument), symbolised and disassembled. base's `CheckMemoryReadOnly`
   (`protected_memory_posix.cc:45`) calls `getrlimit(RLIMIT_NPROC, <a
   page it made read-only>)` and CHECKs for `-1` with `EFAULT`: Linux's way
   of asking "is this page read-only". Here the handler writes through the
   pointer and the process dies. Every handler that stores into a caller's
   buffer has the hole, and Chromium and Crashpad probe memory with `write`
   to a pipe and expect `EFAULT`: one checked copy-out (a fault-tolerant
   store, or a call that writes for the caller) answers them all. Built:
   "CBF1 notes" below.
3. **POSIX layer: calls the log names.** `inotify_init` and `inotify_init1`
   (253, 294): all 165 `FilePathWatcher*` tests (37 failing at the first
   call, the rest wait out their timeouts: a minute each, so the list
   leaves them all out); one hangs. `mincore` (27): 21 tests, PartitionAlloc's
   purge tests, `ProcessMemoryDumpTest.CountResidentBytes*`,
   `SamplingHeapProfilerTest.GetSamplesWithResidency`. `pkey_alloc` (330):
   `PartitionAllocDeathTest`'s set-up (22 of the death tests). `sendfile` (40):
   `FileUtilTest.CopyFileContentsWithSendfile*` (3). Socket options:
   `UnixDomainSocket::EnableReceiveProcessId` fails (`SO_PASSCRED`: 3 tests
   `RecvPid*`), a non-blocking `Send` on a socket pair returns a wrong
   count (`SyncSocketTest.NonBlockingWriteTest`), and two tests wait for a
   blocked receiver that `shutdown()` should wake (`SyncSocketTest.DisconnectTest`,
   `CancelableSyncSocketTest.ShutdownCancelsReceive`: hang). The causes and
   their fixes: [CBF2a notes](#cbf2a-notes-2026-10-17).
   `uname`: the release reads `0.1.0`, so `SysInfo::KernelVersionNumber` is
   below every Linux feature gate (`SysInfoTest.KernelVersionNumber`), and
   there is no DMI hardware information (`GetHardwareInfo`). Shared memory:
   a purged discardable page does not read back as zeros
   (`DiscardableSharedMemoryTest.ZeroFilledPagesAfterPurge`).
4. **File system under `/data` (FAT now): 54 tests; on ext 10 of them still
   fail, see [CB2b](#cb2b-on-ext-2026-10-16)** (`FileUtilTest` 30,
   `ImportantFileWriterCleanerTest` 10, `FileTest` 6, `FileProxyTest` 3,
   `ImportantFileWriterTest` 2, `FileEnumerator.SymLinkLoops`,
   `FileUtilMultiThreadedTest`, `DriveInfoTest`). `symlink()` fails (the
   `SingletonLock` of a Chromium profile is a symlink), `mkfifo` fails,
   `chmod` has no effect (modes read back 0644 and 0755, and the
   read-only bit is not kept), `utimensat` does not set times (so the
   cleaner's old-file tests find nothing old), a file unlinked while open is
   still found by name, `mkdirat` of a long name says "Filename too long" (which
   limit: ?), and a descriptor's `O_RDONLY` or write-only mode is not
   enforced (`FileTest.Length`, `ReadWriteSpans`: ?). M13.6's ext volume
   (U6 gives `extfs` symlinks, modes, owners and times) should clear most:
   rerun `base` on an ext test disk once U7a merges. **A writable shared
   mapping of a `/data` file** (8 tests: `MemoryMappedFileTest`,
   `FilePersistentMemoryAllocatorTest`, `PersistentHistogramAllocatorTest`,
   `PersistentMemoryAllocatorTest.TruncateTest`): `MemoryMappedFile::Initialize`
   with `READ_WRITE` fails (U10 and WRITEMAP-PLAN's writable pager VMOs);
   two of them then go on to use a null object and fault.
5. **Starting programs, FORK-PLAN (or CB5's launcher patch): 46 tests**
   (`ProcessUtilTest` 16, `ProcessTest` 12, `FileLockingTest` 7,
   `PlatformThreadTidCacheTest` 2, `ProcessGTestOutputTest` 2, and `LaunchTest`,
   `ForkWithFlagsTest`, `UnitTestLauncherDelegateTester`, `RustCommandLineTest`,
   `ProcessMetricsTest.GetChildOpenFdCount`,
   `SystemMetricsTest.MeasureChildCpuUsage`, `LoggingTest`). Seen with
   `--v=1`: the ENOSYS log shows `syscall 57 (fork)` first with
   `ProcessUtilTest.SpawnChild`, and `process.IsValid()` is false right after
   `base::LaunchProcess` / `SpawnMultiProcessTestChild` in each, so
   `test_child_process` is never started. A `fork` (FORK-PLAN's FL1b) or
   section 5's launcher patch (`posix_spawn` for `LaunchProcess`) answers
   all of them.
6. **/proc, CL2 and PRC: 14 tests.** The log names `/proc/self/cmdline`
   (`SetProcTitleLinuxTest` 3), `/proc/self/stat`, `/proc/<pid>/stat`,
   `/proc/<tid>/stat` (`PlatformThreadMetricsTest`, `ProcessMetricsTestLinux`),
   `/proc/<pid>/task`, `/proc/self/ns/user`, `/proc/self/mem`, `/proc/meminfo`,
   `/proc/cpuinfo`. `/proc/self/maps` is there but lacks what the tests look
   for (the stack's line, the program's own mappings: `ProcMapsTest`,
   `ModuleCacheTest` 5, `PlatformSharedMemoryRegionTest.MappingProtectionSetCorrectly`).
   What each of the 14 really needed, and what CBF4 built: [CBF4 notes](#cbf4-notes-2026-10-17).
7. **Executable memory, CJ1: `PartitionAllocPageAllocatorTest` (5).**
   `mprotect(PROT_EXEC)` of anonymous memory is refused without the JIT
   right (W^X, by design: section 12); the tests then use a null buffer.
8. **`dlopen`: 3 tests** (`NativeLibraryTest.LoadLibrary`,
   `ImmediateCrashTest.ExpectedOpcodeSequence`,
   `ElfReaderTestWithCurrentImage.ReadElfBuildId`). Seen: the static
   `dlopen` fails, and the ENOSYS log now names the library (`dlopen
   /data/cr/out/jamos/libtest_shared_library.so`, `dlopen libmalloc_wrapper.so`).
   They need shared libraries as data and stay skipped. Any Chromium
   program that tries to load one (a GPU driver, a codec) shows here.
9. **The link, 1 test** (`ElfReaderTestWithCurrentElfImage.ReadElfBuildId`):
   the program has no GNU build-id note (Chromium's crash reporting keys
   modules by it): `--build-id` in the link driver (CT).
10. **Not understood, 1**: `PartitionAddressSpaceTest.ZeroSegment` reads an
    address-space size of 65,536 where it wants at least 4 GiB (understood
    by CBF2b: [notes below](#cbf2b-notes-2026-10-16); items 3 and 9 have
    their answers there too).

**Infrastructure notes** (for the main session). `pc-batch.sh cancel` did not
stop one run's QEMU and driver (`remote-slots.py --status` showed it holding
4 CPUs an hour later; the classifier refused a `pkill` through
`remote-test.sh`). A batch still running when the laptop sleeps is lost,
with its output. The serial line garbles a burst of ten thousand lines and can
cut a kernel log line into a test's result line (the driver reads the list
from the stick and tolerates the cut). `QEMU_SAVE` is needed to read
anything the guest wrote. The init run on this tree fails only
posixtest's `execinfo` section (`execinfo.c:194/195/227/235`), the known
regression since BT.

#### CB2b on ext (2026-10-16)

Track CB2b: the same `base_unittests`, the same tests, on a `/data` that is an
ext volume (`JAMOS_DATA_FS=ext`, U11a's image), to find which of CB2's
file-system failures were only FAT's. Main at c961c5f8 plus this track; the
programs relinked first. **The skip list's file-system group shrank from 54
tests to 10** (and 44 tests now pass), and `base` on ext leaves 596 tests out:

| `/data` | pass | self-skip | on the skip list |
|---------|-----:|----------:|-----------------:|
| FAT (CB2; the same again today) | 8,277 | 39 | 640 |
| ext, `TMPDIR` and `TEST_TMPDIR` at `/data/cr/tmp` | 8,321 | 39 | 596 |
| ext, neither set (init's `/tmp`) | 8,321 | 39 | 596 |

Each row is the six shards of 8,956 tests, all PASS (a test of shard 4 self-skips
in some runs: 17 or 18 of the 39). Two shards also ran booted from an NVMe drive
(3/6 and 1/6 in the first batches): the same counts as the stick's. On the first
ext run, with CB2's full list, the pass count was 8,278 and 640 stayed listed.

- **Relink.** `base_unittests` and `test_child_process` are static: they were
  removed from the kept tree on the PC
  (`~/cr/155.0.8059.39/src/chromium-155.0.8059.39/out/jamos/`, the stage in
  `~/cr/ports/chromium/stage`) and `tools/cr/build.sh start base_unittests
  test_child_process` linked them again from this checkout's libc: 140 s
  (2 min 20 s) from the command to "built", with the PC's CPUs busy with other
  runs. The stage's files changed (76,865,672 to 76,890,904 bytes).
- **The ext stick.** `tools/cr/base-stick.py` now follows the image: for an
  ext image (`mkimage.py --data-fs`) it grows the partition, makes the volume
  with `jamext mkfs` (as `tools/stick-data.sh` does) and writes the whole tree
  (`/cr` and `/etc`: the programs, their approval lines and page tables, base's
  test files, `/cr/tmp`) with one `jamext import`, and reads the programs back
  by hash with `jamext get`; modes are jamext's (0755 for a program). FAT is
  as before. `base-run.py` reads the listing back with `jamext` on an ext
  stick, adds `tools/cr/base_unittests.fat.skip` (the 44 tests only FAT fails)
  on a FAT one, names a run's QEMU files after its output directory (two runs
  of one batch with the same shard number shared a name), and
  `CR_BASE_ONLY` takes several globs. `tools/cr-test.sh base` makes
  `build/host/jamext` for an ext image. Set `JAMOS_DATA_FS=ext` for **every**
  run of a batch: a run with another value rebuilds the image under the others.
- **What ext fixed (44 tests).** Mostly what CB2 expected of it: symbolic
  links (`CreateAndReadSymlinks`, `CopyDirectory*OverSymlink`,
  `NormalizeFilePathSymlinks`, `FileEnumerator.SymLinkLoops`, `DeleteSymlink*`),
  modes (`ChangeFilePermissionsAndRead`, `CopyFileExecutablePermission`,
  `ExecutableExistsInPath`, `CopyDirectoryPermissions*`), times (`TouchFile`,
  `FileProxyTest.SetTimes`, the ten `ImportantFileWriterCleanerTest` tests that look for
  old files, `GetLatestTemporaryFileWithNamePrefix`), a file unlinked while
  open (`DeleteOpenFile`, `DuplicateDeleteOnClose`, `CreatingFileWithSameNameAfterDelete`,
  `DeleteDirRecursiveWithOpenFile`), and tests CB2 could only guess at
  (`FileTest.Length` and `ReadWriteSpans`, set-length, creation time, the ACL copies).
  They are in `tools/cr/base_unittests.fat.skip`, which `base-run.py` adds on a FAT `/data`.
- **What still fails on ext**, with `CR_BASE_ALL=1` on the file-system suites
  (`FileUtil*`, `ImportantFileWriter*`, `FileTest`, `FileProxyTest`,
  `FileEnumerator*`, `DriveInfoTest`) and the four mapping suites, 246 tests:
  223 pass, 23 fail. Three of the 23 are `sendfile` (`FileUtilTest.CopyFileContentsWithSendfile*`:
  syscall 40, CBF2b, and `/proc/self/cmdline`, CBF4) and two are death tests
  (fork, FL1b). The other 18 are below; each is in `base_unittests.skip` under
  its layer.

  | Layer | Tests | Cause (seen = in the log or the code) | Fix track |
  |-------|-------|----------------------------------------|-----------|
  | extfs | `FileUtilMultiThreadedTest.MultiThreadedTempFiles` | 64 threads each make, write, close and read back a temporary file. Seen: `CreateAndOpenTemporaryStream` returns null (file_util_unittest.cc:5363, then a null dereference: SEGV) or, another run, `ReadFileToString` of a just-written file fails (:5381), 2 of 2,048 times, in `/data/cr/tmp` and in `/tmp` alike. Likely: `XF_MAX_FILES` is 32 (extfs.h): `files_open` answers `ERR_NO_RESOURCES` (files.c:395) when no slot is free, and the layer turns that into `EAGAIN` (errno.c:29); the test holds up to 64 open at once. Not proved (no errno is logged). Chromium opens far more than 32 files at once (profile databases, caches, packs): raise it, and look at `XF_MAX_MAPS` (64 mapped files) with it; each open file has a 64 KiB buffer (`XF_FILE_BUF`) | extfs (U9 / the ext review) |
  | libc: libos paths | `FileUtilTest.DeleteDeep` | `mkdirat(dirfd, <255-byte name>)` says "Filename too long": libos limits a path to `FS_PATH_MAX`, 256 bytes (os.h:379, `ns_resolve` ns.c:351 and every `rel[FS_PATH_MAX]`), though the fs protocol carries 4,095 (`FS_PATH_LEN`) and the layer's own `PATH_BYTES` is 4096. os.h says libos's limit "goes up to it when nothing shares it any more". Any path past 255 bytes fails, not only this test's. **CBF6 found otherwise:** the layer never goes through `ns_resolve` for a mount's paths, and a 255-byte name under a 50-byte directory works on ext; the test builds 26 levels of 255-byte names through directory descriptors (6.6 KB of path), and a directory description here is a path as text, so it stops at 15 levels, the layer's `PATH_BYTES` (see CBF6 notes) | POSIX layer + fs protocol: directory handles (a design) |
  | libc: file calls | `FileUtilTest.ChangeFilePermissionsAndWrite` | an `O_WRONLY` open asks the service for read access too (`fs_access`, file.c:56: "the one open serves every description"), so a file with mode 0200 gives EACCES; Linux opens it for its owner | POSIX layer (CBF6: fixed) |
  | libc: file calls | `CopyDirectoryExclFileOverFifo`, `CopyDirectoryWithNonRegularFiles`, `ReadFileToStringWithNamedPipe`, `TestNonBlockingFileReadLinux` | `mkfifo` is `mknod`, and `lx_mknodat` answers `EPERM` (meta.c:537, "mknod is EPERM"): no named pipes; fs.idl has no `mknod` (extfs can hold the inode). Only tests need it | POSIX layer + fs.idl (a named-pipe design) |
  | libc: file calls | `MakeAbsoluteFilePathNoResolveSymbolicLinks` | the working directory is a path as text (dir.c `lx_getcwd`): after the test removes it, `getcwd` still answers; Linux gives `ENOENT` | POSIX layer (small; CBF6: fixed) |
  | libc: sysfs | `DriveInfoTest.GetFileDriveInfo` | `has_seek_penalty`, `is_removable` and the size come from `/sys/dev/block/<major:minor>`; none of it exists, so the calls give `nullopt` | POSIX layer (with uname/sysfs, CBF2b; CBF6: the files are there) |
  | musl | `FileUtilTest.FileToFILE` | `fdopen(-1, "w")` returns a `FILE` (musl does not look at the descriptor), the test expects null. Alpine's build fails it the same | none; leave it out |
  | mmap (WE1) | `MemoryMappedFileTest.WriteableFile`, `ExtendableFile`; `FilePersistentMemoryAllocatorTest.AcceptableTest`, `ExtendTest`; `PersistentHistogramAllocatorTest.CreateSpareFile`, `CreateWithFile`, `MovePersistentFile`; `PersistentMemoryAllocatorTest.TruncateTest` (8) | `MemoryMappedFile::Initialize` with `READ_WRITE` fails on ext too (expected: WK2 and WE1, extfs's write-back, are not built); `CreateWithFile` and `MovePersistentFile` then dereference the failed object (SEGV at 0x8) | WE1 |

- **`/tmp`.** init's `/tmp` is there in every boot: on the ext stick and on
  the NVMe boot it is a view of `/data/tmp` ("init: /tmp: a view of
  /data/tmp (emptied: 0 removed)", after a memory volume first). All six
  shards with `CR_BASE_ENV=` empty (no `TMPDIR`, no `TEST_TMPDIR`: base uses
  `/tmp`, shown by the temporary names in the failure output) pass with the
  same results as with both set at `/data/cr/tmp`. gtest's
  `CaptureStderr` `mkstemp` works: `ClampTest.Death` without either variable
  aborts exactly as it does with them (silently, killed by SIGABRT, no
  "Unable to create a temporary file" line), at the child's `clone`/`fork`. The
  harness keeps `TMPDIR=/data/cr/tmp` by default, so a run is the same on a
  boot with no `/tmp`.
- **The NVMe boot.** `QEMU_BOOT_NVME=1` boots the same stick as an installed
  drive (`tools/qemu-test.sh` makes it GPT: `nvme0`, "Jam OS data" mounted as
  `/data`, ext). Shards 3/6 and 1/6 pass with the numbers of the stick runs
  and cost no change to the scripts (the listing is read back with `jamext
  --part 2` from the GPT image). `/tmp` is the same view of `/data/tmp`. A
  drive boot with `/jam` is DB1's.
- **A transient disk error kills the program.** On a loaded PC (15 runs at
  once) one shard lost a program to `usb-storage: command 28: ERR_TIMED_OUT`
  (QEMU's USB disk, ten seconds late), `extfs: read of 128 blocks failed`,
  `mapped file ... can't read at 3624960: ERR_IO` and then SIGBUS in the
  program running from its pages (`ToArrayTest.IdentityFixedExtent` was the
  test that was running); one timing test of a socket pair
  (`CancelableSyncSocketTest.ShutdownCancelsReceiveWithTimeout`) failed in
  another. Both passed run again alone; a read error of a program's page is a
  SIGBUS by design.
- **Evidence.** Run with `tools/pc-batch.sh` (batches 16211943, 16213418,
  16214455, 16215035): `cr-test.sh base` shards `e0`..`e5` (ext, `TMPDIR`
  set), `n0`..`n5` (ext, none), `f0`..`f5` (FAT), `fsx` and `fsn` (the
  file-system and mapping suites with `CR_BASE_ALL=1`, with and without
  `TMPDIR`), `nv1` and `nv3` (NVMe boot). The numbers above are each
  `base/run/summary.txt`'s; the output of each failure is in `failures/`.

#### CBF1 notes (2026-10-16)

Track CBF1 answers finding 2: a call given a pointer the program can't
read or write returns -1 with `EFAULT` and the process goes on, as on
Linux. It clears finding 2's skip-list group once the rerun confirms it:
`BackupRefPtrTest` (23) and `ProtectedMemoryTest` (4) need nothing more;
`CPUDeathTest` and `ProtectedMemoryDeathTest` are death tests, so they also
wait for FL1b's fork (finding 1). The skip list itself is left to the rerun.

- **Checked copies, kernel style.** user/posix/sys/ucopy.S holds the only
  code that touches a program's memory for a handler: `uc_get` and `uc_put`
  (one `rep movsb`), `uc_zero`, `uc_strnlen` (a byte at a time), and the
  probes `uc_readable` and `uc_writable` (a load, or a locked add of 0, on
  each page). Each of their loads and stores of the caller's memory has a
  line in an exception table in the same file (the instruction and its
  fixup, as offsets: no absolute address); no routine moves RSP, so the one
  fixup returns -EFAULT to the routine's caller. Only those instructions:
  a fault on the layer's own side of a copy, or anywhere else in the
  layer, is still SIGSEGV. ucopy.h says the rule for handlers (a struct
  built in a local and copied out once, read into a local once), and
  `uc_put_int` is the int answer of many ioctls and calls.
- **The fault path.** sig_fault (sigfault.c) first asks `uc_fault`
  (ucopy.c): a page fault (vector 14, any status: Linux's `EFAULT` for a
  truncated file's page too; since triage TB, `EIO` for a page its pager
  failed, as the kernel's own copies say and `write()` returns, so
  posix-mmap-test's pull case gets `EIO` again) or a general protection fault (13: an address
  that isn't one) at a table line moves RIP to the fixup and `EXC_RESUME`s
  in the kernel's window, before any signal logic: it works with no handler
  installed, on any thread (the signal thread too), and touches only the
  frame and the table. SG5's raised frames and everything after are
  unchanged below it.
- **The entry from the start.** The exception entry used to be set only
  while a fault signal had a handler. Now `sig_start_state` (the start,
  under sig_lock) sets it for every POSIX program with a new reason,
  `SIG_ENTRY_COPY`, which is never cleared, so `sigaction` never removes
  it. A fault with no handler goes entry, `sig_fault_begin` false,
  `EXC_UNHANDLED`: the kernel's kill with the same line (posix-fault's
  first child checks it). **For FL1b:** the child of a fork must set the
  entry again unconditionally (the entry is the process's).
- **The sweep** (user/posix/sys; every handler that reads or writes a
  pointer it was given):
  `rlimit.c` getrlimit, setrlimit, prlimit64 (getrlimit and setrlimit with
  NULL are EFAULT, as Linux); `time.c` clock_gettime, clock_getres,
  nanosleep and clock_nanosleep (request and remainder), times, getrusage;
  `misc.c` uname, sysinfo, sched_getaffinity, sched_setaffinity (the mask
  read before the thread is looked up, Linux's order), getrandom (made in a
  512-byte chunk, then copied: a fault part way returns the bytes so far);
  `sched.c` getcpu, sched_getparam, sched_setparam, sched_setscheduler;
  `user.c` getresuid, getresgid, getgroups; `prctl.c` PR_SET_NAME,
  PR_GET_NAME and every int answer; `thread.c` arch_prctl(ARCH_GET_FS),
  futex (the timeout read once, first; a word the kernel can't read is
  EFAULT through `uc_or_fault`); `sigcalls.c` rt_sigaction,
  rt_sigprocmask, rt_sigpending, rt_sigsuspend, rt_sigtimedwait (set,
  timeout, info), rt_sigqueueinfo, rt_tgsigqueueinfo, sigaltstack;
  `sigtimer.c` getitimer, setitimer, timer_create (sigevent, and the id:
  a bad id pointer makes no timer), timer_settime, timer_gettime;
  `proc.c` wait4 and waitid (status, rusage, siginfo: written last, the
  child reaped, as Linux); `spawn.c` execve (path, argv and envp and each
  string checked before the start reads them); `path.c` path_get (the
  path checked with `uc_strnlen`, then read in place: every path call),
  `meta.c` stat, lstat, newfstatat, statfs, fstatfs (a local struct copied
  out), the `AT_EMPTY_PATH` tests, utimensat's times, symlink's target,
  readlink's answer (and procfs's `copy_link`); `dir.c` getcwd and
  getdents64 (`dir_put` builds each record locally; procfs's listing too);
  `fd.c` fstat, ioctl(FIONBIO), the iovec arrays of readv, writev,
  preadv, pwritev, preadv2, pwritev2 (`fd_iov_in`: copied, 8 on the stack
  and more from malloc, as Linux's `UIO_FASTIOV`), and `fd_iov_scatter`
  and `fd_iov_gather` now checked (they return -EFAULT; every caller
  stops there: pipe, socketpair, tty, file, `/etc`, `/proc`, the devices,
  sockets, Wayland); `dup.c` fcntl's record locks and F_GETOWN_EX;
  `epoll.c` epoll_ctl (not read for DEL, Linux's) and epoll_wait's events
  (a store that faults keeps that event and the rest for the next wait,
  and says EFAULT if none went: Linux's); `ready.c` the wait masks of
  ppoll, pselect6 and epoll_pwait; `poll.c` poll and ppoll (the pollfds
  copied into the thread's scratch array, each revents copied back);
  `select.c` select and pselect6 (the sets' longs copied in and out, the
  timeouts and pselect6's mask pair); the ioctls of `file.c`, `pipe.c`,
  `socket.c`, `sockpair.c` (FIONREAD, SIOCATMARK), `pty.c` (window sizes,
  TIOCGPTN) and `tty.c` (termios, window sizes, FIONREAD, the process
  group calls); `pipe.c` pipe and pipe2, `sockpair.c` socketpair (the pair
  written last; a bad pointer closes both, Linux's); `eventfd.c` (the
  buffer checked before the count is taken, so it stays); `memfd.c`
  memfd_create's name (copied) and the reads the kernel makes into the
  caller's buffer (`uc_or_fault`); `dev.c` /dev/urandom (made in a chunk,
  then copied) and /dev/zero; `socket.c` `sock_addr_in` and
  `sock_addr_out`, `sockconn.c` connect's family, accept4's length (read
  before the wait; a bad address after it closes the new descriptor,
  Linux's), a socketpair's name; `unixsock.c` and `svcsock.c` an
  `AF_UNIX` name in and out; `sockopt.c` every option in and out;
  `sockio.c` recvfrom's length, sendto, and sendmsg, recvmsg, sendmmsg and
  recvmmsg through a `struct umsg` (the header, its iovecs and the control
  data copied in; a receive's control data and its lengths and flags
  copied out after; a send's control data over 64 KiB is ENOBUFS).
- **Two ways for a buffer the kernel or another library touches.** A
  write's and a send's bytes are probed readable before any goes
  (`fd_iov_check` in `rw` and the send calls): kinds hand a single buffer
  to the kernel as it is (a pipe's message, a tty's, a memfd's), and this
  is the probe Chromium and the sanitizers make (a write to a pipe of
  memory that may not be readable). A read is not probed up front (it would
  commit pages a short read never touches): a kernel call that writes the
  caller's buffer and fails with `ERR_INVALID_ARGS` is asked again by
  `uc_or_fault` (the buffer probed only then), and a TCP read probes just
  the bytes the ring is about to give (the ring's code writes the buffer
  itself).
- **Where it differs from Linux** (all decided here, small): a write to
  `/dev/null` of an unreadable buffer is EFAULT (Linux's null device never
  reads it); a pipe read into a buffer the kernel can't write loses that
  message (the kernel's `channel_read` has taken it off the queue; Linux
  keeps the bytes) where a socketpair's, an eventfd's and epoll's keep
  theirs; a write's buffer is checked whole before any byte goes (Linux
  may write the readable part first); a path, an `AF_UNIX` send and
  execve's strings are checked and then read in place, and a gather after
  a send's check sends zeros, so a program that unmaps its own buffer
  during the call can still fault there (SIGSEGV) or send zeros; a
  received descriptor whose control-data copy-out then faults stays open.
  musl's own wrappers still read some pointers first (`sendmsg` copies the
  control data, `sigaction` the struct), as glibc's do on Linux: those
  fault in the program, not the layer. `__jam_clone`'s stores (musl's own
  pointers) and `set_tid_address` (the kernel stores at exit) are not
  checked.
- **Tests.** posixtest's `efault` section (efault.c, in the init run, and
  `tools/shell-tests/posix-efault.txt` from the shell): base's
  `getrlimit(RLIMIT_NPROC, <read-only page>)` exactly; 428 calls with bad
  pointers, every converted call with each of its kind (a read-only page
  where it writes, a `PROT_NONE` page where it reads, address 0x10, NULL
  where NULL isn't "not given", a non-canonical address: the #GP path),
  made raw with `syscall()`; that a socketpair's bytes, an eventfd's count
  and a ready epoll event survive the failed call; the same with SIGSEGV
  and SIGBUS handled (EFAULT, the handlers never run, a second thread's
  fixup too) and a real store to the read-only page reaching the handler;
  and a child that never installs a handler getting EFAULT, then dying by
  a real fault with SIGSEGV and the kernel's line.

#### CBF2b notes (2026-10-16)

Track CBF2b (branch `cbf2b-calls`): the small calls CB2's findings 3, 9 and
10 named, and PO9's `O_PATH`. Posixtest has a case for each (`posix-mem.txt`
and the files and start sections: [TESTING](TESTING.md)). The base tests
below stay on `tools/cr/base_unittests.skip` (their lines say what changed)
until the CB3-gate rerun runs them (`CR_BASE_ALL=1`) and takes the passing
ones off; the programs need relinking against the new libc first.

- **`open(dir, O_PATH | O_DIRECTORY)`** (PO9). musl's `O_ACCMODE` is
  `03 | O_PATH`, so every "is it read-only?" test of open flags read an
  `O_PATH` open as write access. The tests that mean the mode now mask with
  `FD_ACCMODE` (`fd.h`; `open_dir`, the mode-3 check and `fs_access` in
  file.c, dir.c's `dir_new`, dev.c, etc.c, pty.c). An `O_PATH` directory
  description keeps `O_PATH` (`F_GETFL` says so; `getdents64` on it is
  EBADF, `fstat` and the `*at` calls work), and `open("/etc/passwd",
  O_PATH)` opens instead of EROFS. ports/libarchive's patch is removed and
  `ports-test.sh libarchive` passes without it. Clears: libarchive's
  extraction (no base test).
- **`mincore`** (27). Reads `process_get_maps` (the kernel counts the pages
  of each mapping that are present in the page tables, not which ones) of
  the range's mappings: a mapping with none of its pages present reads as
  none resident, with all of them as all, and in between the first
  `resident` pages of the mapping read as resident (so a sum over a whole
  mapping is right, a page's own answer is a guess). `PROT_NONE` memory has
  no entries (the kernel clears them), so it reads as not resident;
  `EINVAL` for an unaligned start, `ENOMEM` for a range with an unmapped
  page (the pages before it are filled in, as Linux does), 0 for a length
  of 0. Should clear `ProcessMemoryDumpTest.CountResidentBytes` and
  `...InSharedMemory` (fresh or fully written mappings; base_unittests was
  not run for any of this). **Will not clear
  the 18 `PartitionAllocTest.Purge*` tests whose mappings are part-resident**
  (they ask about one page of a committed run, some of it discarded with
  `madvise`): they need which pages, a kernel query (below). The heap
  profiler's residency test: unknown until it runs. The bytes of `vec` are
  stored without a checked copy: CBF1's EFAULT work should cover them.
  (MC replaced the guess with the kernel's answer per page: [MC
  notes](#mc-notes-2026-10-18).)
- **`pkey_alloc`, `pkey_free`, `pkey_mprotect`** (330, 331, 329). Jam OS has
  no protection keys, so the answers are Linux's on a machine without them:
  `pkey_alloc` EINVAL for flags or an unknown access right, else ENOSPC;
  `pkey_free` EINVAL; `pkey_mprotect` is `mprotect` for key 0 (the default)
  and -1 (none asked for), EINVAL for any other key (the key is an `int`:
  musl passes it in a 64-bit register, so it is cut to 32 bits first).
  What PartitionAlloc expects: `PartitionAllocTest::InitializeMainTestAllocators`
  calls `PkeyAlloc` whatever CPUID says (`has_pku` is CPUID leaf 7 ECX bit 4,
  OSPKE, 0 here: the kernel never sets CR4.PKE) and takes -1 as "no key"
  (`pkey_` stays `kInvalidPkey`, the pkey allocator is made without thread
  isolation, `PkeyFree` is not called). With ENOSYS the same call ended the
  set-up. Clears the set-up of `PartitionAllocDeathTest`'s 22 tests; they
  still need FL1b's `fork`/`clone` to be death tests at all.
- **`sendfile`** (40). In `fd.c` over the descriptions' own `read` and
  `write` (64 KiB at a time through a buffer): from `*offset` (which moves
  on; the input's offset stays) or from the input's offset (which moves
  on), to a file at its offset, a socket or a pipe; the count cut at the end
  of the file and at 0x7ffff000; only the bytes written count (after a short
  write the offsets stand where the bytes sent end). An input with no offset
  (pipe, socket, terminal) is EINVAL, as Linux, which is what base's
  `CopyFileContentsWithSendfile` falls back on; EBADF for a descriptor that
  can't read or write; a write's EPIPE raises SIGPIPE (calls.c). Should clear
  `FileUtilTest.CopyFileContentsWithSendfile` and `...Socket`;
  `...SeqFile` also reads `/proc/meminfo` and `/proc/self/cmdline`, CBF4's.
- **`uname`** (63, and the DMI files). **Decision: the release is
  `5.10.0-jamos`**, the version `#1 JamOS <Jam OS's version>`. Programs of
  the Linux ABI parse the release as `major.minor.patch` and gate features
  on it: Chromium's `SysInfo::KernelVersionNumber` (`rand_util` wants 3.17
  for `getrandom`), libuv's io_uring check, glibc's minimum 3.2. `0.1.0`
  put Jam OS below every gate. 5.10 is what the Debian 11 sysroot
  Chromium's host tools build against runs on, and below the few gates that
  want calls Jam OS lacks (the priority-inheritance futex check in
  `lock_impl_posix.cc` is Android only; programs probe newer calls and take
  ENOSYS); the suffix says it is not a Linux. `sysname` stays `JamOS` (shell
  scripts match `uname -s`); Jam OS's own version is in `version`. The
  `/proc` files that say the same (`sys/kernel/osrelease`, `version`) are
  made to agree by CBF4 (`LX_RELEASE`, misc.c). `SysInfo::GetHardwareInfo`
  reads `/sys/devices/virtual/dmi/id/sys_vendor` and `product_name` and wants
  both non-empty: etc.c serves them like `/etc`'s files (read only, `JamOS`
  and `JamOS machine`: Jam OS can't tell the machine's real maker and model)
  unless the namespace has a `/sys` mount. Clears `SysInfoTest.KernelVersionNumber`
  and `SysInfoTest.GetHardwareInfo`.
- **A purged discardable page reads zero.** base's
  `DiscardableSharedMemory::Purge` uses `madvise(MADV_REMOVE)` on Linux and
  Android (and `MADV_FREE` elsewhere); the layer had refused it (EINVAL).
  Now over MAP_SHARED memory it empties the range for every holder: an
  anonymous shared mapping's VMO gives its pages back, a memfd's (a
  `VMO_KEEP_PAGES` VMO: no page ever leaves) is overwritten with zeros, so
  the memory is not freed there but reads as Linux's does; private memory is
  EINVAL, a range nothing maps ENOMEM, a mapping whose handle can't write
  EACCES. Clears `DiscardableSharedMemoryTest.ZeroFilledPagesAfterPurge`.
- **`PartitionAddressSpaceTest.ZeroSegment`: not a layer answer** (done since by [MH](#mh-2026-10-18): the hint, top-down placement, the programs at 4 GiB). Nothing
  is read from the OS: `GetZeroSegmentSizeFromOS` is a constant (0x10000, a
  TODO for a larger `mmap_min_addr`), and `InitZeroSegment` reserves
  `[0x10000, 4 GiB)` as one PROT_NONE mmap at that address, keeping the
  64 KiB (65,536) if it doesn't get it. Two things stop it. The layer
  ignores an `mmap` address hint (`map_new` passes it to `vmar_map` without
  `VMAR_FIXED`, so the kernel chooses): Linux uses a free hint, and
  PartitionAlloc's pools and V8's code range ask for theirs. And the
  program's image is linked at 0x400000 (`posix.ld`), inside the range, so
  the reservation could not be made even with the hint honoured. Left as
  is: honouring a free hint is a layer change with a wider effect (every
  pool and V8 range would land where asked: do it with a test of V8 and
  PartitionAlloc on it), and the test needs the Chromium programs linked
  above 4 GiB (a base address for `posix.ld` that the link driver can set;
  that the kernel's loader takes an ET_EXEC there is untested) or the test
  skipped.
- **`--build-id`: not done** (done since by [MH](#mh-2026-10-18), as below). The driver (`tools/jamos-clang`) passes
  `-Wl,--build-id=none` before Chromium's own flags, and Chromium's
  `build/config/compiler/BUILD.gn` (line 1085) adds `-Wl,--build-id=fast`
  after, so the flag is not what is missing: `user/posix/posix.ld` ends with
  `/DISCARD/ : { *(.note .note.*) ... }`, which takes lld's
  `.note.gnu.build-id` too. Checked on the PC with the Chromium lld and a
  one-instruction program (a scratch run, not kept): the script as
  it is gives no note with `--build-id=fast`; with one added PHDR
  (`buildid PT_NOTE FLAGS(4);`, after `wants`, so the program's list stays
  the first note segment) and one line before `.jamos.wants`
  (`.note.gnu.build-id : { *(.note.gnu.build-id) } :rodata :buildid`) the
  note is there, 8 bytes (16 hex digits: what `ElfReaderTest` accepts), in
  its own PT_NOTE of the read-only segment; with `--build-id=none` the extra
  PT_NOTE is empty (size 0, as the `wants` one already is when a program has
  no list). It is a `posix.ld` change, so it changes the sysroot and with it
  every port's stamp, and Chromium needs relinking: **written down, not
  done**. Whoever does it should check the kernel's and the exec service's
  readers of PT_NOTE (they look for the `JamOS` note; a second segment, or
  an empty one, must not confuse them) and GNU ld on the same script
  (jamos-cc's links).
- **For a later track (decisions for the owner).** (1) Exact `mincore`
  needs a kernel query for which pages of a range are present (a
  `vmar` call filling one byte a page, over the walk `process_get_maps`
  already does); without it the 18 purge tests and any caller of one page
  stay a guess (built: [MC notes](#mc-notes-2026-10-18)). (2) Honour
  `mmap`'s free address hint. (3) A high image base
  for Chromium's programs (ZeroSegment, and a null-page hardening that is
  worth having anyway). (4) `/proc/sys/kernel/osrelease` and
  `/proc/version` to say what `uname` says (done: [CBF4 notes](#cbf4-notes-2026-10-17)).

#### CBF4 notes (2026-10-17)

Track CBF4 (branch `cbf4-proc`): the `/proc` files `base_unittests` still
found missing or wrong (CB2's finding 6), the files `uname` also says, and
posixtest's `procinfo` section made robust. Read CB2's log of what the tests
name against the tests' own source (the Chromium tree on the PC) rather than
the ENOENT log alone: **five of the 14 are not about a file** (the four
`ModuleCacheTest`s want `dladdr`, the fifth two modules).
`/proc/self/stat`, `meminfo`, `cpuinfo` and `maps` were built by PRC after
that log; what the tests then still lacked is below, per test. Not run here
(`base_unittests` is the CB3 gate's); posixtest's `procfs` and `procinfo`
sections have a case for each file, run on the PC (the `init` run and
`posix-procinfo.txt`: [TESTING](TESTING.md)).

**Which skip-list tests each change should clear** (take them off
`tools/cr/base_unittests.skip` after the gate's `CR_BASE_ALL=1` run; the
programs need relinking against this libc first):

| Change | Tests |
|--------|-------|
| `/proc/self/cmdline` and the strings' run | `SetProcTitleLinuxTest.Simple`, `.Empty`, `.Long`; with CBF2b's `sendfile`, `FileUtilTest.CopyFileContentsWithSendfileSeqFile` |
| `/proc/<tid>/stat` and `/proc/<pid>/task` | `PlatformThreadMetricsTest.GetCumulativeCPUUsage_CurrentThread`, `.GetCumulativeCPUUsage_OtherThread`; `ProcessMetricsTestLinux.GetCumulativeCPUUsagePerThread` |
| `minflt` in stat (the kernel's fault count) | `ProcessMetricsTestLinux.GetPageFaultCounts` |
| `[stack]` in `maps` | `ProcMapsTest.ReadProcMaps` |
| `meminfo`'s buffers, caches and splits | `SystemMetrics2Test.GetSystemMemoryInfo` |
| `dladdr` of the program's image (not `/proc`) | `ModuleCacheTest.GetDebugBasename`, `.LookupCodeAddresses`, `.ModulesList`, `.NativeModuleOverAuxiliaryModuleProvider` |

`ModuleCacheTest.CheckAgainstProcMaps` **stays**: it wants two modules ("this
module and ld-linux.so") and a static program has one. It moves out of the
`[proc]` group on the next edit of the list (reason: a static program has no
ld.so). `PlatformSharedMemoryRegionTest.MappingProtectionSetCorrectly`, which
finding 6 names, is not on the list: PRC's `maps` already gave it its line.

- **`/proc/self/cmdline`** (also `/proc/<pid>/cmdline` of ours and
  `/proc/self/task/<tid>/cmdline`): the argument strings, NUL-ended, as
  Linux reads them from the process. Linux reads the memory, so a program
  that overwrote it (`setproctitle`) changes the file, by Linux's rule
  (`get_mm_cmdline`): the run `arg_start..arg_end` while its last byte is
  NUL, else, once that byte was overwritten, from `arg_start` to the first
  NUL (included), a page at most. base's `setproctitle` probes this at
  its first call (`buggy_kernel`: it zeroes the run and sets argv's last byte
  to `.`; Linux gives back one NUL byte). **The strings are now one run**
  (`user/posix/crt/start.c`): argv's, then the environment's (and `TZ=` when
  the starter gave none), each NUL-ended with nothing between, as Linux's
  kernel puts them on the stack. base's `setproctitle_init` returns without
  doing anything unless every string begins where the last ended, and
  `base_unittests` main calls it first thing: whenever the starter gave no
  `TZ`, the old start put the added one outside the run and all three tests
  would have failed. The run is on the stack (up to 4 KiB; a VMO above), so
  the start pays one copy of the strings. A child's `cmdline`: **not built**
  (libc keeps no copy of the argv it gave a child): `ENOENT`, as before.
  stat's `arg_start`, `arg_end`, `env_start` and `env_end` are the run's.
- **Threads** (`/proc/<pid>/task`, `/proc/self/task/<tid>/{stat,statm,status,
  maps,cmdline,mem}`, and `/proc/<tid>/...` for any thread of ours, not
  listed, as Linux). The tids are libc's table of threads (`thread.c`: the
  first thread, then those `clone` made); the layer's signal thread is in
  `num_threads` but not listed (a Linux program has no such thread). A
  thread's stat has its own id first, its **own CPU time** (the kernel's
  `thread_info.cpu_ns`, as `utime`), state `R` for the thread reading and
  `S` for the others (the kernel doesn't say what a thread waits on), the
  process's start time, faults and sizes. Its comm is the calling thread's
  own name if it set one (`prctl(PR_SET_NAME)`, kept per thread), else the
  program's: another thread's name is its own business. Its signal sets are
  the calling thread's (the layer keeps no other thread's). status has
  `Pid` the thread's, `Tgid` ours. **Decision: a number that is a pid and a
  tid is the process.** pids and tids are separate id spaces
  (`process_info.pid`, `thread_info.tid`), Linux's is one; `/proc/<n>` is
  our pid, else a child's, else one of our threads. A thread whose id is a
  child's pid is shadowed there (and always reachable as
  `/proc/self/task/<tid>`). (Linux's main thread has the pid for its tid;
  here the two come from separate id spaces.)
- **`minflt`** (stat field 10) is the address space's count of page faults:
  **one kernel change**, `process_info.faults` (`kernel/include/jam/abi.h`,
  96 to 104 bytes), the existing `aspace_fault_count` that the tests used
  (every fault the handler is asked about, resolved or not). Kernel and
  libc build together; `abi_check.c` has the offset. `cminflt`, `majflt`
  and `cmajflt` stay 0 (the pager's waits are not told apart). A thread's
  file has the process's count.
- **`[stack]`** in `maps`: the mapping that holds an address on the main
  thread's stack, taken in a constructor (`procfs.c`: constructors run on
  the main thread). Only that one is named; thread stacks (musl's mmaps)
  stay blank, as Linux's. `VmStk` is its size and `VmData` leaves it out,
  `statm`'s data field is both (Linux's). The program's data and bss lines
  already carried the executable's path (PRC); a posixtest case now says so.
- **`meminfo`**: `Buffers`, `Cached`, `Active`, `Inactive`, `Active(anon)`,
  `Inactive(anon)`, `Active(file)`, `Inactive(file)` and `AnonPages` were 0;
  base's `GetSystemMemoryInfo` wants the first six non-zero and each under
  `MemTotal` (`SystemMetrics2Test`). Jam OS has no source (a page cache per
  pager, counted in no figure a program can read), so they are **fractions of
  the memory in use** (MemTotal - MemFree): Buffers 1/64, Cached 1/8 (half
  active), the rest anonymous (3/4 active). They add up with MemFree to
  MemTotal. A plausible constant, not a measurement; `MemAvailable` stays
  `MemFree`.
- **`/proc/self/mem`** (also `/proc/self/task/<tid>/mem`): read only, size 0,
  mode 0400. `pread` at an address reads the program's memory there through the
  checked copy (`ucopy.h`), a page at a time; the first page it can't read
  (unmapped, no access, not an address of ours) ends the read, and `EIO` if it
  is the first, as Linux's. **Never writable**: opening it for writing is
  `EACCES`, because on Linux a write through it passes the pages' permissions
  and Jam OS's W^X is not to be passed by the layer. A child's `mem`: not built.
  `SEEK_END` is `EINVAL`. (Chromium's stack trace code opens it to read
  memory safely.)
- **`/proc/self/ns/user` stays ENOENT.** PRC's reason holds: Linux has no such
  file without user namespaces, and the sandbox code's "are unprivileged user
  namespaces supported" check (`PathExists("/proc/self/ns/user")`, in
  `ProcessUtilTest.CloneFlags` too) then says no, which is true. `/proc/self/ns`
  is not made either. Ask again only with a namespace design.
- **`/proc/version`, `/proc/sys/kernel/{osrelease,ostype,version,hostname}`**
  and `/proc/sys`, `/proc/sys/kernel` as directories: what `uname` says (misc.c
  gave its fields a function, `lx_uname_fields`, so the two cannot disagree):
  `5.10.0-jamos` (`LX_RELEASE`), `JamOS`, `#1 JamOS <version>`, the host name.
  `/proc/version` is Linux's shape with Jam OS's words: `JamOS version
  5.10.0-jamos (jamos@<host>) #1 JamOS <version>`. Anything else under
  `/proc/sys` is ENOENT. Owned by uid 0, mode 0444 (directories 0555).
- **`dladdr`** (musl's static stub says no to every address): our override
  (`user/posix/musl/src/ldso/dladdr.c`, `OVERRIDES`; `posix.mk` no longer builds
  upstream's) answers for the program's own image, the one module a static
  program has: `dli_fbase` its ELF header (which `posix.ld` maps), `dli_fname`
  the path `/proc/self/exe` gives (else argv[0]), no symbol (`dli_sname` and
  `dli_saddr` null); 0 for any other address. Chromium's `ModuleCache` finds
  its module this way (four tests above), and `StackTrace` can name the
  program in a trace. Not `/proc`, but the same tests; named here so the files
  are in one place.
- **posixtest's `procinfo` (the old line 526)**: after `munmap` of the
  16-page test mapping the section looked for lines of the pieces' exact
  shapes. Reading `maps` allocates (the text, the layer's own buffers; musl
  maps any allocation over 7 KiB), and an earlier section in the same process
  can leave malloc needing a new mapping, which lands in the hole and can have
  a piece's shape. Now: the section reads `maps` into a static buffer of its
  own, and every page of the mapping holds a marker; a line of an old
  shape counts as the old mapping only if its page still holds the marker (a
  mapping nobody can read counts). A fresh mapping never has it.
- **Left as before** (each shows in the ENOSYS log if something asks):
  `smaps`, `smaps_rollup`, `limits`, `environ`, `auxv`, `io`, `uptime`,
  `loadavg`, `vmstat`, `diskstats`, `comm`, `thread-self`, `ns/`, `cwd`,
  `/proc/sys/vm` and the rest of `/proc/sys`, other processes' files.
- **Edits outside /proc**: the kernel's `process_info.faults`
  (abi.h, abi_check.c, process.c, a declaration moved to aspace.h); the start
  (`user/posix/crt/start.c`); `thread.c` (`thread_tids`, `thread_tid_exists`,
  `thread_cpu_ns`); `misc.c` (`lx_uname_fields`); `glue.h`, `OVERRIDES`,
  `posix.mk` and the new `dladdr.c` for the override; utest's `posix_ids`.

#### CBF2a notes (2026-10-17)

Track CBF2a (branch `cbf2a-sockets`): the socket findings of CB2's item 3
and CB2b, each a posixtest case that does what base does (the `scm`
section's `scmcred.c`, its Mojo child, and the `unix` section), no
Chromium needed. Which entries of `tools/cr/base_unittests.skip` each fix
should clear (the CB3-gate rerun checks; the list's comments say so):

| Fix | Skip-list tests it should clear | Left |
|-----|---------------------------------|------|
| `SO_PASSCRED` and `SCM_CREDENTIALS` | `UnixDomainSocketTest.RecvPid`, `UnixDomianSocketTest.RecvPidDisconnectedSocket` | `RecvPidWithMaxDescriptors` sends the socket over itself 16 times; the kernel's cycle rule refuses a channel end into its own channel (`ETOOMANYREFS`, CU1), where Linux keeps it in flight. Stays listed with that reason |
| `shutdown` wakes what sleeps on the end | `SyncSocketTest.DisconnectTest`, `CancelableSyncSocketTest.ShutdownCancelsReceive` (both hung). Not listed but mended: `ShutdownCancelsReceiveWithTimeout`, which CB2b saw fail once under load: its poll ran to its own 750 ms timeout, a race with the test's 750 ms wait for it; the poll now ends at the shutdown | nothing |
| `FIONREAD` leaves the channel's room | `SyncSocketTest.NonBlockingWriteTest` | nothing |

- **`SO_PASSCRED` needs a true pid, so the kernel stamps it.** A message
  on a channel says nothing of its writer, and the receiver can't ask the
  sender (another process, which can't see the receiver's option and
  could write any pid into its bytes). So every channel message now
  carries the pid of the process that wrote it, set as it is written (0
  for a kernel thread's), and `channel_read`'s `rd_flags` (the struct's old
  reserved word: no size change) asks for it with `CHANNEL_READ_PID`,
  written after the handles' count (`rd_hactual + 4`; utest
  `chan_writer_pid`). A socketpair end always asks; `recvmsg` with
  `SO_PASSCRED` on gives `SCM_CREDENTIALS` with the pid of the process that
  wrote the bytes it took, and the uid and gid `getuid` and `getgid` say
  (1000: one user), before any `SCM_RIGHTS` block, as Linux orders them.
  None at the end of the stream (base's `RecvPidDisconnectedSocket` wants
  pid -1 there); an empty seqpacket message and `MSG_PEEK` have them;
  too little room is `MSG_CTRUNC` with nothing written (Linux writes what
  fits of the block); `MSG_WAITALL` stops where the writer changes, as
  Linux's does. A sender's own `SCM_CREDENTIALS` block is taken and changes
  nothing, anyone else's pid `EPERM` (fdpass.c). Named sockets: `setsockopt`
  and `getsockopt` answered `ENOTSOCK` on them; they now answer as a
  socketpair end does, and `SO_PASSCRED` set before connect reaches the
  connection, a listener's each connection it accepts (Linux inherits it
  so). The posixtest Mojo child's "OK" says the child's pid.
- **The hangs: shutdown closed the channel the sleepers waited on.**
  `shutdown(SHUT_RDWR)` swaps the end's channel for a dead one and closes
  the old; a thread asleep in a read or a write of that end waits on the
  old channel (`object_wait_one`), which nothing signals once its handle is
  closed (the kernel's wait rule), so `DisconnectTest`'s worker and
  `ShutdownCancelsReceive`'s thread slept on. Now a thread lists itself on
  the end while it sleeps (sockpair.c `sp_sleep`), and shutdown interrupts
  each (`thread_interrupt`): it wakes, sees the end shut, and reads the end
  (0) or gets `EPIPE`. An interrupt that comes after the sleeper's wait
  ended on its own is latched for its next wait (the kernel's rule), so
  the sleeper takes it with a short wait of its own. poll, select and
  epoll_wait asleep with the end in their sets were in the same spot:
  `ready_wake_all` wakes every one of the process's (poll's sets list
  themselves before they sleep, epoll's count their waiters) and each looks
  again, where the shut end is ready. Shutdown is rare, so waking every
  waiter once costs nothing that matters.
- **`FIONREAD` freed a message's room.** It read the next message off the
  channel into the leftover to count it, so a writer that had filled the
  channel to `EAGAIN` could write one more after the reader's `FIONREAD`:
  `NonBlockingWriteTest` fills it, counts, and expects the next send to
  fail. The send's own count was right (the bytes taken, or `EAGAIN`). Now
  `FIONREAD` reads the next message's size with no room (`ERR_BUFFER_TOO_SMALL`
  leaves it queued); a message with descriptors is still read into the
  leftover, as a read would. It still counts one message, where Linux's
  counts every byte queued on a stream (a later kernel count of an end's
  queued bytes could give that).
- **Not done**: `SO_PEERCRED` (no need found; a socketpair's would be its
  maker's, which a hand-over would have to carry). A leftover handed to a
  child (`posix_spawn`) keeps its bytes but not its writer's pid (0 there).
- **For FL2 (fork's descriptors)**: a forked child's copy of a socketpair
  end must start with no sleepers (`pipe_end.sleepers` NULL: they were
  the parent's threads), and poll.c's list of sleeping sets and epoll's
  `waiters` counts likewise (only the forking thread goes on, and it is in
  neither).

#### CBF6 notes (2026-10-17)

Track CBF6 (branch `cbf6-file-calls`): five of CB-RM's small POSIX-layer
items, each a posixtest case that does what the base test does. Which
`base_unittests.skip` entries each should clear:

| Fix | Skip-list tests it should clear | Left |
|-----|---------------------------------|------|
| `O_WRONLY` on a file of mode 0200 | `FileUtilTest.ChangeFilePermissionsAndWrite` | nothing |
| `getcwd` of a removed directory | `FileUtilTest.MakeAbsoluteFilePathNoResolveSymbolicLinks` | `chdir("..")` out of a removed directory (Linux's works, ours is a path as text) |
| `/sys/dev/block/<major>:<minor>/{removable,queue/rotational,size}` | `DriveInfoTest.GetFileDriveInfo` | the test wants `size` of at least 10,000,000, and sysfs counts 512-byte sectors (base reads it as bytes): the file's volume must be 5.12 GB or more; the stick's `/data` is 6 GiB now (item below), off the skip list once the stick had it ("The stick is 6 GiB", below) |
| `/proc/<pid>/smaps_rollup` | `SmapsRollupTest.ReadAndParse` | nothing |
| (the 256-byte path) | `FileUtilTest.DeleteDeep` | **not a 256-byte limit; stopped on a design question, below** |

- **`O_WRONLY` asked for reading too.** The layer's one open of a file
  serves every description of it, so a write-only open asked the service
  for `FS_READ | FS_WRITE`, and extfs refused a file whose mode has no
  read bit. `open_file` now tries again with the write alone when the
  open is `O_WRONLY` and the answer is `EACCES` (the refusal comes before
  `O_TRUNC` truncates anything, and a mode of 0 still fails the second
  try); `kind_give`'s reopen for a child likewise. Objects shared by path
  (`fobj.c settle`) were shared by anything that could write; they are
  shared now only when they can do what the description wants, read and
  write, so a read opened after `chmod` is not given a write-only open.
  Test: the links section's `write_only` (ext, mode 0200: `O_WRONLY |
  O_CREAT | O_TRUNC` as base's `WriteFile`, a second description, a read
  refused as `EBADF`, `O_RDONLY` and `O_RDWR` `EACCES`, `O_APPEND`, a read
  after `chmod 0600`, a child given the description, mode 0 refused and
  the file not truncated).
- **`getcwd` of a removed directory.** The working directory is a path as
  text; `lx_getcwd` now asks the service whether it is still there
  (`path_cwd_removed`: a mount's directory that answers "not found") and
  says `ENOENT` if not, before `ERANGE` as Linux does. A service that is
  slow or refuses leaves the answer as it was, and so does a program whose
  namespace lacks the mount (the shell's `cd /data`, then `pxio`, which
  has no `/data`: sh-posix caught that). One call to the service per
  `getcwd` on a mount (none in `/`, `/svc`, `/proc`, `/dev`, `/etc`). Not
  as Linux: a directory made again at the same path is "there" (Linux's
  stays the removed one), and `chdir("..")` out of a removed directory
  fails (ENOENT; the path as text walks through the removed name). The
  function's 4 KiB buffer is on the heap now. Test: `files2.c
  current_dir`.
- **`/sys/dev/block`.** `st_dev` is the mount's place in the namespace
  (path.c `mount_dev`, 1 on), read by base with `<linux/kdev_t.h>`'s
  macros as major 0, minor n. etc.c serves, for the n of each mount of the
  namespace and when there is no `/sys` mount, `removable` and
  `queue/rotational` ("0": the layer is not told what kind of drive a
  volume is, and none it can be on rotates or is pulled out in a way it
  knows) and `size` (the volume's bytes from `fs.statfs`, in 512-byte
  sectors, asked at the open). They stat as sysfs attributes (mode 0444,
  size 4096) and are read only. The directories themselves are not
  listed. Test: the start section's `check_drive_info` for `/boot` and
  `/data`.
- **`/proc/<pid>/smaps_rollup`.** procinfo.c's walk of the mappings now
  also sums the pages by kind of mapping. Rss, Referenced and Pss are the
  pages present (nothing says a page is shared with another process, so a
  page counts whole); Pss_Anon, Pss_File and Pss_Shmem and Anonymous are
  the splits `status` already has; a page is dirty if its mapping can be
  written (there is no per-page bit) and shared if the mapping is
  `MAP_SHARED`: Private_Dirty, Private_Clean, Shared_Dirty and
  Shared_Clean add up to Rss, Pss_Dirty is the two dirty ones; the rest
  is 0. The first line is the pseudo-mapping `lo-hi ---p 00000000 00:00 0
  [rollup]` over the lowest start and the highest end; the text is well
  under one page, which base reads in one `read`. It is in the listing of
  `/proc/<pid>` (ours and a child's) and of a thread's directory. Test:
  procinfo.c's `check_smaps_rollup`.
- **`DeleteDeep` is not the 256-byte limit CB-RM took it for; stopped on a
  design question.** The layer takes a mount's paths whole to the
  service (`fs_*_within` with the path from `path_rel`), never through
  libos's `ns_resolve`, so libos's `FS_PATH_MAX` of 256 is not on this
  path (it is on `fs_readdir` of `/` and `/svc`, the spawn's `fs_stat` of
  a program's name and `fs_path_clean`, none of which the test uses), and a
  255-byte name under a 50-byte directory is made on ext. The test
  (base/files/file_util_unittest.cc:1766) makes 26 levels of 255-byte
  names, each through `mkdirat` and `openat` on the level above's
  descriptor, a link `up` and a file at the bottom, then
  `DeletePathRecursively` (`fstatat`, `openat(O_NOFOLLOW)`, `fdopendir`
  and `unlinkat` on descriptors, file_util_posix.cc:313-369): a tree 6.6 KB
  deep that Linux walks by descriptor and no path string names. Here a
  directory description is its absolute path as text (files.h `struct
  path`, `PATH_BYTES` 4096, the same as Linux's `PATH_MAX`), and the fs
  protocol takes 4,095 bytes (`str[<=4095]`), so `mkdirat` at the 16th
  level is `ENAMETOOLONG` (measured on ext: 15 levels). More bytes would
  also need extfs's buffers (`rest_buf`, `w->phys`, a subview's root of
  `XF_ROOT_MAX` 1,024), FAT's `FAT_PATH_MAX` and the layer's `struct path`
  (4,120 bytes, already on stacks), so raising a constant is the wrong
  fix. The right one is directory handles: `fs.idl` gets an open that
  returns a directory (a channel) whose calls (open, stat, mkdir, unlink,
  rename, readdir, getattr) take names relative to it, extfs walks by
  inode from it, and a directory description holds the handle beside its
  path (the path stays for `fchdir`, `getcwd` and the mount lookup, cut
  short where it would be too long). FAT could serve a handle only to
  the depth FatFs's own path buffer allows. That is a protocol and
  extfs change, the owner's call. Test (what does work): `files2.c
  deep_names`: a 255-byte name made, opened, listed and removed, a
  256-byte one `ENAMETOOLONG`, 15 levels through descriptors with a file
  and a link at the bottom, the 16th `ENAMETOOLONG`, all removed from the
  bottom through the parents' descriptors.
- **The stick is 6 GiB (tooling, 2026-10-18).** `DriveInfoTest` needs a
  volume of 5.12 GB or more (`size` is in sectors and base reads bytes), so
  `tools/cr/base-stick.py` makes `/data` 6,144 MiB (`CR_BASE_DATA_MIB`,
  was 640) and the stick is a sparse file: 6.25 GiB apparent, 117 MiB on the
  disk of the PC (measured: `du` 119,368 KiB; `e2fsck -fn` of its ext volume
  exits 0). What keeps it sparse: the image is copied up to where `/data`
  starts only, through `tools/cr/sparsecopy.py` (holes and 64 KiB blocks of
  zeros stay holes; selftest in `make check`); an ext volume is made on a
  file that ends where `/data` starts, so `jamext mkfs` extends it, knows
  the new blocks read zeros and writes none of its own (a FAT one is
  `mformat`ted at full size: its two tables, 12 MB); `base-run.py` copies the
  stick for every boot with `sparsecopy` too (Python's `shutil.copyfile`
  wrote the whole 6 GiB: 6,553,604 KiB measured, 4 to 9 s each time);
  `tools/qemu-test.sh`'s `cp` keeps the holes (GNU cp, measured 116 MiB); and
  `tools/mkimage.py --gpt` (`QEMU_BOOT_NVME=1`) now leaves 64 KiB blocks of
  zeros as holes in the drive it makes (it wrote out the whole partition).
  A base run's disk use on the PC stays at some 120 to 240 MB for each boot
  (the stick, its copies, the saved one), as with 640 MiB. Off the skip
  list: `FileUtilTest.ChangeFilePermissionsAndWrite`,
  `MakeAbsoluteFilePathNoResolveSymbolicLinks`, `SmapsRollupTest.ReadAndParse`
  and `DriveInfoTest.GetFileDriveInfo`. Not run: the PC's stage still has
  CCS's `base_unittests`; the next measurement relinks it from main first.

#### CB-RM (2026-10-17)

Track CB-RM (branch `cb-rm`): `base_unittests` measured again after the layer
fixes of the last two days (CBF1's EFAULT, CBF2a's sockets, CBF2b's calls,
CBF4's `/proc`, FL1b's `fork`, FL2's descriptors, R3's ext limits), main at
0fa8eea5. The two programs were relinked first (`tools/cr/build.sh start
base_unittests test_child_process` after removing them from the kept tree:
116 s; the stage's `base_unittests` 76,946,736 bytes, was 76,890,904). The six
shards on an ext `/data` with the new list **all PASS** (batch 17170310):

| `/data` | pass | self-skip | on the skip list |
|---------|-----:|----------:|-----------------:|
| ext, CB2b (2026-10-16) | 8,321 | 39 | 596 |
| ext, CB-RM, death tests in gtest's fast style | 8,660 | 36 | 260 |

Per shard (of 8,956 tests): 1,227 + 3 + 8 of 1,238; 1,465 + 0 + 11 of 1,476;
1,943 + 4 + 68 of 2,015; 1,404 + 9 + 142 of 1,555; 1,326 + 16 + 7 of 1,349;
1,295 + 4 + 24 of 1,323 (pass + self-skip + listed). **343 tests came off
the list and 7 went on** (below): 596 became 260 (95 named tests and the 165
of `FilePathWatcher*`). The pass count is not CB2b's like for like: 275 of
those 343 are death tests, which the run now makes in the fast style (below).
The numbers came from `CR_BASE_ALL=1 CR_BASE_LISTED=1` runs of the suites
that hold a listed test (batches 17160914 and 17162403, base's default style
and the fast one), the six plain shards before and after the list changed
(17160914, 17164338, 17170310), and a probe of `execve` in a forked child
(17172112).

Per group of the list, before and after (the list's groups, CB2 finding in
brackets):

| Group (CB2 finding) | Listed before | Listed now | What decided it |
|---------------------|--------------:|-----------:|-----------------|
| Death tests (1) | 281 | 5 (+1 moved) | 275 pass with the fast style. Left: 4 that set the threadsafe style themselves, and the stack-trace one (below) |
| EFAULT (2) | 29 | 0 | CBF1: 26 pass at once, the 3 death ones with the fast style |
| `inotify` (3) | 165 | 165 | `inotify_init` is still `ENOSYS` (`enosysnames.c`); not run, a minute each |
| Launching (5) | 46 | 52 | `execve` in a forked child fails (below); 6 tests that passed by accident in CB2b fail now |
| `mincore` (3) | 21 | 9 | CBF2b: 12 pass; the 8 `PartitionAllocTest.Purge*` and the heap profiler's residency test still guess |
| `/proc` (6) | 14 | 2 | CBF4: 13 pass; `ModuleCacheTest.CheckAgainstProcMaps` is by design; `SmapsRollupTest.ReadAndParse` is new |
| `sendfile`, `uname`, discardable zeros (3) | 3 + 2 + 1 | 0 | CBF2b (`pkey_alloc` was in the death tests: the 22 `PartitionAllocDeathTest` set-ups pass) |
| Socket options, `shutdown`, non-blocking send (3) | 3 + 2 + 1 | 1 + 0 + 0 | CBF2a; `RecvPidWithMaxDescriptors` stays (the kernel's cycle rule) |
| ext limits (CB2b) | 1 | 0 | R3: `MultiThreadedTempFiles` passes (64 threads, 2,048 files) |
| Executable memory (7) | 5 | 3 | `DecommitAndZero` and `InaccessiblePages` pass; the other three need the JIT right |
| `dlopen`, build-id, musl, `ZeroSegment` (8, 9, 10) | 3, 1, 1, 1 | 3, 1, 1, 2 | unchanged; `ZeroSegmentInaccessibleEnd` joins `ZeroSegment` (nothing is reserved at the low segment) |
| File calls and sysfs (CB2b) | 4 + 1 + 1 + 1 + 1 | the same | `mkfifo`, `O_WRONLY` on mode 0200, the 256-byte path, `getcwd` after `rmdir`, `/sys/dev/block`: no fix has merged |
| Writable shared mapping (4) | 8 | 8 | WE1 is not built |

**The death tests: fast style, and what the threadsafe style needs.** base
sets gtest's death test style to `threadsafe` (`base/test/test_suite.cc:712`),
and a command-line `--gtest_death_test_style=fast` overrides that. In
`threadsafe` gtest makes the child with `clone(fn, stack, SIGCHLD)` on a
**two-page stack** it maps itself (`gtest-death-test.cc:1309`), and the child
runs `execve` of the same program. With base's style (the old list, batch
17160914) 145 of the 281 fail ("died but not with expected error": no `Check
failed` text) and 136 pass, **125 of them only because their matcher accepts
any death**: in all of those the kernel log has `user: process
"base_unittests" killed: page fault at rip 265e63e, address 227bd8`, the
same rip every time. Symbolized (llvm-symbolizer on the unstripped program):
`__jl_path_get` at `path.c:188`, the `call` of `uc_strnlen` itself: a push,
so the stack pointer has gone below the pages the child was given. The layer
runs on the caller's stack and `path_get`'s frame alone is 4,120 bytes
(`char base[PATH_BYTES]`; its callers hold a `struct path` beside it), so
`execve` in a clone child dies on its first path. Not FL3's code but FL3's
reach: **exec in place will walk the same `path_get` from the same two-page
stack** (and so will any layer call that takes a path). Options for
whoever fixes it (not decided here): the child's `__jam_clone_process` runs
`fn` on a layer-owned stack of some 64 KiB, or the layer's path buffers move
off the stack. The run therefore makes death tests in the fast style
(`base-run.py --death-style`, default `fast`; `CR_BASE_DEATH=threadsafe` in
`cr-test.sh`), which needs only `fork`: 275 of the 281 pass for real (the
child dies of its `CHECK` and prints it). Left, all listed:
`SequenceCheckerTest.MoveOffSequenceBanned`, `ThreadCheckerTest.MoveOffThreadBanned`,
`WeakPtrDeathTest.ArrowOperatorChecksOnBadDereference` and
`...StarOperatorChecksOnBadDereference` set the threadsafe style in their own
body, so they hit the stack; and `StackTraceDeathTest.StackDumpSignalHandlerIsMallocFree`:
the forked child dies of SIGSEGV but the in-process stack trace never reaches
its captured stderr (a file the child reopens by path at its first use, FL2;
cause not found). The fast style is a deviation from Chromium's default, kept
until the threadsafe one can run: flip the default back when FL3 and the stack
land, and rerun.

**Launching (FL3): confirmed to fail, and how.** `fork` works (FL1b, FL2): the
tests get a child. Its `execvp` fails: base's child prints `LaunchProcess:
failed to execvp:` and the program's path, and exits 127 (`SharedMemorySwitchTest`,
`ProcessTest.WaitForExit`, `ProcessUtilTest.LaunchProcess`, `FileLockingTest`,
...; 27 fail, 3 crash, `ProcessMetricsTest.GetChildOpenFdCount` hangs). No
kernel line, no ENOSYS line. The errno came from a probe: gtest's threadsafe
death test with `--gtest_death_test_use_fork` makes its child with `fork`
(a normal stack) and prints its `execv` error: **`execv(/data/cr/out/jamos/base_unittests, ...)
failed: Permission denied`**, `EACCES`, 94 times in the 17 `CheckDeathTest`
tests of batch 17172112 (the fork itself was fine). init's exec service says
every refusal in the log ("no approval for it", ...) and its log has none, so
the call fails before init refuses: in `file_program` (`spawn.c`),
`svc_get(SVC_EXEC)` null in the child (`-EACCES`, "not ours to ask"), or the
child's `file_open` of the program denied. Not told apart here; FL3 will meet
it first. 15 of the 46 launching tests pass anyway, because they never read
the child's exit status or look before it has failed
(`ProcessUtilTest.SpawnChild`, `ProcessTest.Terminate`, ...): they stay
listed so that FL3's `CR_BASE_ALL=1` rerun checks them for real. Six tests
that CB2b counted as passing were passing the same way (`exit_code` stays 0
until a wait sets it) and fail now that the child is real:
`SharedMemorySwitchTest.PassViaSwitch` (4), `HistogramSharedMemoryTest.PassSharedMemoryRegion_Enabled/0`
and `FieldTrialListTest.PassFieldTrialSharedMemoryOnCommandLine`; they went on
the list. Others may still pass this way.

**Other findings.**
- **`uname` turned a test on.** `SmapsRollupTest.ReadAndParse` skipped itself
  while the release read 0.1.0 (it gates on the kernel version) and runs now
  that it reads 5.10.0-jamos; `/proc/self/smaps_rollup` is one of the files
  CBF4 left, so it aborts on an empty optional. It joined the list. The
  self-skips went from 39 to 36.
- **`mincore`**: as CBF2b said, exact only for a mapping with all or none of its
  pages present. The nine left ask about one page of a part-resident
  mapping (`CheckPageInCore` false for a page that is there; the sampling
  profiler's `resident_total` reads 0). The per-page kernel query is still
  the open owner question (built since: [MC notes](#mc-notes-2026-10-18)).
- **`ZeroSegmentInaccessibleBegin` passes**, `...End` does not ("failed to
  die": the segment is not reserved as the test expects).
- **The disk can fail a shard.** Under a loaded PC (12 to 14 of this track's
  QEMUs and FL3's) 8 of 61 boots logged `volumes: can't open /data's block
  channel for the spare (ERR_TIMED_OUT)` every 3 s from 3.2 s, and 3 of those
  (the listing boot of one shard, and the first boot of two others) went on
  to `extfs /data: a flush of the disk's cache failed: ERR_TIMED_OUT` and
  `journal: commit write failed ...: the volume is read-only from now on`.
  A program that makes a temporary file then crashes in every test (gtest's
  `CaptureStream`: `Failed to create tmp file`; 33 crashes in two shards, which
  made the first fast-style numbers look worse than they were). The warm
  spare is `volumes`' (it starts a spare `bin/fat` at 1.2 s); the disk is the
  stick behind a loaded QEMU, as in CB2b's "a transient disk error", but
  here the volume stays read-only for the rest of the boot, and the first
  timeout is at boot, not under a test. For the `volumes` track to look at.
  `base-run.py` now says so in the summary ("WARNING: /data went read-only in
  round(s) ...") instead of leaving it to the failures.
  **Fixed by DRO:** the logs show one SYNCHRONIZE CACHE, sent just after the
  mount, waiting 10 s for its status (QEMU's flush of the image on the loaded
  host); usb-storage then reset the transport and answered `ERR_TIMED_OUT`,
  and the spare's opens were only queued behind that command (usb-storage
  serves one request at a time). extfs now tries a block call that times out
  again (`user/services/extfs/disk.c`: a flush on any channel, a read or a
  write where answers come in order; 5 tries within 40 s, pauses from 0.5 s
  doubling, one line per episode), so the volume goes read-only only for a
  real failure or a disk that stays silent; volumes' spare backs off its opens
  (1 s doubling to 32 s) and says a run of failures once (utest
  `extfs_timeout`, `extfs_timeout_spent`). **DRO2** finished it: the rules
  moved to `<blockcall.h>` (libos), which fat uses too (utest `fat_timeout`,
  `fat_timeout_spent`); usb-storage gives a SYNCHRONIZE CACHE 20 s, and the
  tries 45 s (`drivers/usb-storage/scsi.c` has the sums); a call waits 90 s on
  drv/nvme, longer than it holds a command (its 30 s, an Abort, a reset, a
  read sent once more), and a read or a write that gets no answer at all
  ends the service (exit 1, a restart on a new channel) instead of reusing a
  buffer the driver may still write (`extfs_late`, `fat_late`); drv/nvme
  answers what was out only once bus mastering is off when the controller
  doesn't stop or the driver ends (`nvme-driver-test.sh nostop`).

**Harness changes** (`tools/cr`, `tools/cr-test.sh`): `CR_BASE_LISTED=1` (only
the suites with a test named on the skip list, not `FilePathWatcher*`'s
wildcard; with `CR_BASE_ALL=1` the way to measure a list), `CR_BASE_DEATH`
(`fast` default, `threadsafe`), the read-only warning, and the list itself
(header text per group; `ZeroSegmentInaccessibleEnd` moved under `ZeroSegment`).

**What stands between here and the CB3 gate** (a clean rerun except
`fork`/`LaunchProcess` and the by-design skips). The 260 listed tests, by
who clears them:
- **FL3** (exec in place), with the stack: 52 launching tests, the 4
  threadsafe death tests, and then base's own death test style back on (the
  whole of the 281 run for real again). Include the EACCES above and the clone
  child's two-page stack in its brief; `vfork` and `functional/vfork`
  follow it.
- **A fix track not yet launched** for what no merged work covers, none
  big: file calls (`mkfifo` 4, `O_WRONLY` on mode 0200, the 256-byte path
  limit, `getcwd` after `rmdir`: 7), `/sys/dev/block` (1), `smaps_rollup` (1),
  the stack-trace death test (1), exact `mincore` (9, the kernel query).
- **Owner decisions** that were already open: honour `mmap`'s address hint and
  link Chromium's programs above 4 GiB (`ZeroSegment` 2), `--build-id` in
  `posix.ld` (1), the kernel's cycle rule for a socket sent over itself
  (`RecvPidWithMaxDescriptors`, 1).
- **WE1** (writable shared mappings of files, 8) and **CBF3** (`inotify`, the
  165 `FilePathWatcher*` tests; unmeasured, `inotify_init` is `ENOSYS`):
  Chromium falls back where it can't watch (net's DNS config reads
  `getaddrinfo` instead), so whether the gate wants them is the owner's
  reading of its wording. The 260 add up: FL3 56, the fix track 19, owner
  decisions 4, WE1 8, `inotify` 165, by design 8.
- **By design, stay listed**: `dlopen` (3: a static program), `ModuleCacheTest.CheckAgainstProcMaps`
  (1: one module), `FileUtilTest.FileToFILE` (1: musl's `fdopen`), executable
  memory without the JIT right (3).

#### CCS (2026-10-17)

Track CCS (branch `ccs-clone-stack`): the clone child's stack, base's own
(threadsafe) death test style again, and the launching tests after FL3, on
main f1c4ba6a. `base_unittests` and `test_child_process` were relinked three
times (the stack; `svc exec` in the list; the forwarding fix), each in about
two minutes.

**The clone child's stack.** musl's `clone()` without `CLONE_VM` ran fn on
the stack its caller gave, and the layer runs a call on its caller's stack,
so a two-page stack (gtest's) died at its first path. On Linux the kernel
runs a call on a stack of its own; the layer now does the same for that one
thread (`fork.c`): the child maps 64 KiB over a guard page, and calls.c's
boundary moves each call fn makes onto it (`lx_call_on_stack`: one test of
a global per call, `false` in every other process), as do the child's exit
and a clone of its own. fn's frames stay on the stack it was given, where
Linux has them (posixtest checks a local's address). The thread is told by
a thread-local top: a thread the child makes later, and the signal thread
(told by its stack, it has no thread pointer), make their calls where they
are; so does a handler on its sigaltstack (a call it interrupted may have
frames on the stack for calls). The size: GCC's `-fcallgraph-info=su` over
the layer and libjam gives ~29 KiB for the deepest call made of direct calls
(`lx_mkdir` through `path_libc_stat`, `stat_at`, `path_get`: 4 KiB frames each),
16 to 25 KiB for most calls that take a path, 1.9 KiB for the boundary's
delivery; calls through a descriptor kind's table are not followed. 64 KiB
is about twice the deepest. Tests: posixtest's fork `small` case and exec
`clone` case (TESTING.md); without the stack (a guard page below the two
pages) the child faults in `stat_at` (`meta.c:97`), batch 17200451.

Other places the layer runs on a small stack it was given (listed, not
fixed):
1. A handler on its sigaltstack: musl's `SIGSTKSZ` is 8 KiB, and one call
   that takes a path needs 16 to 25 KiB of it.
2. A thread with a small stack: `pthread_attr_setstacksize` takes down to
   musl's `PTHREAD_STACK_MIN`, 2 KiB (musl's default, 128 KiB, is fine).
3. The fault and raise entries (`sigentry.S`, `sigfault.c`) build the
   handler's context on the interrupted stack, as Linux builds its frame,
   but the layer's C around it is bigger than Linux's frame: a signal
   raised in a clone child's own code runs that on its two pages.
4. musl's own ways into the layer that aren't calls (`__jam_posix_spawn`,
   `__jam_resolve`, `__jam_ttyname`, `__jam_dladdr`,
   `__jam_unmapself_prepare`) run on their caller's stack, a clone child's
   too (a `posix_spawn` there).
(TS made 1, 2 and 4 so: "TS" below.) The boundary's switch could serve the first two later (a stack for calls
per thread); moving the 4 KiB path buffers off the stack would shrink every
one (CBF6's files).

**Launching after FL3.** With the stack, the threadsafe children got as far
as `execv` and failed as the forked ones did (CB-RM): `EACCES`. The cause
was the program's list: `ports/chromium/wants/base_unittests` had no `svc
exec`, so `file_program` (`spawn.c`) answered "not ours to ask" before init
was asked, for every program on `/data` (`base_unittests` itself and
`test_child_process`). The list is in the ELF file, so the programs were
linked again. Then 38 of the 52 launching tests and the four that set the
threadsafe style passed for real (batch 17202701). One more layer bug
showed: a forked child's `execve` of a `/data` program is spawn, wait,
exit, and its signal thread forwards every signal to the program through
`lx_child_signal`, which `malloc`ed; that thread has no thread pointer, so
Chromium's PartitionAlloc faulted there (`FreeInUnknownRoot`, address 0x28,
thread "signals", in `SystemMetricsTest.MeasureChildCpuUsage`). One child
now takes a slot on the stack (`proc.c`); posix-mmap-test's `spawn` case
checks the forwarding (a forked child's `execve` of `/data/pt-prog`, our
`SIGTERM` to it, its exit 9).

**What is left of the two groups (20), by cause** (the skip list's groups):
- Q1 (A) of CR-REVIEW-POSIX (CR2F): a `/svc/exec` child gets `/` for its
  directory and `AT_SECURE` (the crt drops `TMPDIR`): `CurrentDirectory`, and
  the four signal-file tests, whose child looks for a relative name in `/`
  and runs to the test's timeout (5). Q1's option C would mend them; SX
  (below: D2) does, for a child that isn't raised.
- A thread's id is never the pid (3: base's tid cache, `ForkWithFlags`).
- `fcntl` record locks are each process's own (7 `FileLockingTest`; kept
  by the file service since RL: [RL notes](#rl-notes), off the list after
  a relink).
- `clone` with `CLONE_FS` is refused (1, `LaunchProcess`'s one case).
- A handler run at a call's boundary gets zeroed registers in its
  ucontext (1, `LoggingTest.CheckCausesDistinctBreakpoints`: `abort()`'s
  `REG_RIP` is 0; signal.c's `run_handler`).
- Another process's `/proc/<pid>/fd` and `/stat` (2; `MeasureChildCpuUsage`
  also watches the waiting forked child, not the program it runs).
- **The stack-trace death test (1): cause found.** The test puts in a
  `malloc` that crashes and raises `SIGSEGV`; base's handler is
  async-signal-safe, but its `BeingDebugged()` opens `/proc/self/status`, and
  the layer's `open` allocates from the program's `malloc` (fd.c's `calloc` of
  the description; procfs's buffers), so the handler dies at that open
  and prints nothing, in either style. Linux's calls never touch the
  program's heap. The fix is the layer's own heap (musl's internal
  `__libc_malloc` family under the layer's ~200 allocations in some 30
  files, libjam's objects in libc too, and no free of what a musl helper
  allocated): it would also make the layer's calls safe in a handler that
  interrupted the program's `malloc` (a crash inside PartitionAlloc with its
  lock held, then the handler's `open`, would wait for ever) and keep the
  signal thread off the program's heap for good. A track of its own; not
  done here.

**Numbers.** `--death-style` is `threadsafe` again by default (`CR_BASE_DEATH=fast`
for the old way). The list went from 260 to 223 (95 named and the 165
`FilePathWatcher*` became 58 and 165). The six shards on an ext `/data` with
the new list in base's own style **all PASS** (batch 17210913):

| `/data` | pass | self-skip | on the skip list |
|---------|-----:|----------:|-----------------:|
| ext, CB-RM, death tests in gtest's fast style | 8,660 | 36 | 260 |
| ext, CCS, death tests in base's threadsafe style | 8,697 | 36 | 223 |

Per shard (of 8,956): 1,232 + 3 + 3 of 1,238; 1,466 + 0 + 10 of 1,476;
1,960 + 3 + 52 of 2,015; 1,405 + 9 + 141 of 1,555; 1,328 + 17 + 4 of 1,349;
1,306 + 4 + 13 of 1,323 (pass + self-skip + listed). All 281 death tests CB2
listed now run as base makes them, each child a new `base_unittests` started
by init: a shard takes 5 to 8.5 minutes (CB-RM's fast style: 4 to 11 with
the PC busier). The listed numbers came from `CR_BASE_ALL=1 CR_BASE_LISTED=1`
runs in the threadsafe style before the list changed (batches 17200816,
17201643, 17202701, 17205730: the stack alone, then the list's `svc exec`, then
the forwarding fix).

**Harness changes**: `base-run.py --death-style` and `cr-test.sh`'s
`CR_BASE_DEATH` default to `threadsafe`; the skip list's groups as above.

#### SX (2026-10-18)

Track SX (branch `sx-exec-env`): the owner's decision D2 (made for him,
his to override). A child started through `/svc/exec` starts in its
caller's directory, and is `AT_SECURE` only when it is **raised**: when its
list asks for more than its caller's approval gives. Before, every child
was `AT_SECURE` and started in `/` (Q1's answer A,
[CR-REVIEW-POSIX](CR-REVIEW-POSIX.md)): init couldn't tell which program
called.

**Why the five failed.** All five are `ProcessUtilTest` children that
base starts as Chromium's launcher does: fork, `chdir` to
`options.current_directory` in the child, `execve` of `base_unittests`
(a `/data` program: spawn, wait, exit through `/svc/exec`). The four
signal-file tests (`GetTerminationStatusCrash`, `...SigKill`,
`...SigTerm`, `KillSlowChild`) hand the child a file name relative to the
directory (`GetSignalFilePath` is the bare name on Linux): the child
started in `/`, never found its file and ran to the test's timeout.
`CurrentDirectory`'s child compares `GetTempDir()` with its current
directory: it was in `/`, and `TMPDIR` was gone too (the crt drops it for
`AT_SECURE`), so `/tmp` against `/`; both had to be fixed.

**How init decides** (user/services/init/exec.c, "who is raised", where
the reasoning is written out):
- `exec_prog.start` takes the caller's own program, its `SR_SELF_IMAGE`
  (a new optional handle; libc's `start_sealed` sends it, as exectest's
  client does). init counts it only if it is a VMO whose handle has
  `RIGHT_EXEC` and that a pager supplies (`handle_info`'s
  `related_koid`): only `vmo_make_exec_verified` makes one, which only init
  and the shell may call, each for a program the owner approved, every
  page checked by the kernel. The list read from it (`dp_wants_vmo`) is
  that approval's.
- The child is raised unless every grant of its list is within that list
  (`dp_wants_within`, `<dataprog.h>`): the same service; a mount at the
  same point (or within `*`, or `/usb*`) and as much (read within write,
  write within write-with-packages). A child that asks for a power (`right
  ...`) is always raised: a root is never passed on, so no fork or
  `posix_spawn` child holds one, and init can't see whether its caller is
  the program's first process.
- Not known is raised: no image (a boot-image program, utest, a native
  caller that sends none), an image that isn't sealed (a plain or JIT VMO,
  a copy; the shell's whole-read first run after `allow`, an anonymous VMO,
  too), one that doesn't read. Old static programs that send no image keep
  the old behaviour until relinked.
- What the child is given is its own approval either way (the owner's
  CE1 rule): this decides only the mark and the directory.

**The directory.** The hand-over's `cwd` (text the caller wrote) becomes
the child's when the child isn't raised, the path's mount is one its list
gives (`dp_wants_mounts`), it isn't under the mount's top-level `etc` (no
list's view has it), and init's namespace has a directory there; else `/`
with a log line (`init: exec: <path>: its caller's directory <dir> isn't
in its namespace: it starts in /`). A raised child starts in `/` whatever
its caller says (`init: exec: <path> is raised (AT_SECURE): ...`), since a
path is text and proves nothing about what the caller can reach. When
directory handles land (docs/DIRHANDLE-PLAN.md, being written) the caller
gives an open directory instead; `child_dir` is where that goes, and a
raised child may then be given its caller's directory too.

**Security.** A raised program still gets none of its caller's unsecure
variables (`TMPDIR`, `LD_*`, `NLSPATH`, ...: the crt drops them) and
starts in `/`. A child that is not raised asks for nothing its caller's
approval doesn't give, so whatever its environment and directory steer it
to do, its caller could do itself; and a directory its caller could see
and it shouldn't is not in its own namespace, so it starts in `/`. The
trust is in the program, not the process: a caller holding program P's
image counts as having P's list, which is what the shell and `/svc/exec`
give P; a child P makes of itself gets P's namespace too (`posix_spawn`'s
`NS_ALL`, fork's copy) unless P asks for less with
`posix_spawnattr_setgrants_np` and gives that narrower child `svc exec`:
then the narrower child still counts as P (a question for the owner,
below). What musl does with `AT_SECURE`: `secure_getenv` and `issetugid`,
no `NLSPATH`, `MUSL_LOCPATH` or TZ file but `/etc/localtime`, 0 to 2 on
`/dev/null` if closed; base reads `TMPDIR` with plain `getenv`, so only the
crt's dropping mattered to it.

**Tests**: utest `exec_within` (the rule) and `exec_starter`: posixtest on
`/data` (`--child-launch from`) in `/data/ce-dir` with `TMPDIR` set forks
and execs itself, and the child (`inherit`) is there, with `TMPDIR` and
`AT_SECURE` 0 (it fails before SX: `AT_SECURE` 1); posixtest starts pxio
(`from-out`: its list `/tmp` alone, not raised) and pxio's `cwd` is `/`,
`/data` not being in its namespace; exectest (`/data` read-only) starts
posixtest, raised (`sealed-bare`: `AT_SECURE`, `HOME` without exectest's
`TMPDIR`, `/`); posixtest's bytes in a VMO never sealed, sent as the
image, count for nothing (raised). `exec_handover` is unchanged: utest has
no image.

**Left for the owner**: (1) should a narrower child of P's
(`posix_spawnattr_setgrants_np`) lose the image, or keep it without
`RIGHT_TRANSFER`, so that it can't vouch as P? Nothing in the tree makes
one with `svc exec` today. (Decided: no image; TS built it, "TS" below.)
(2) A raised child starts in `/`; Linux keeps a
set-user-ID program's directory (the caller could reach it). Until
directory handles, the text can't show that. The five tests come off the
skip list's `[launch]` group once a base run shows them passing.

#### BM1 (2026-10-18)

Track BM1 (branch `bm1-measure`): `base_unittests` measured on main as it
stood after CCS, CBF6, FL4, WK3, WE1, DRO2 and TL (d0fc314c), a checkpoint
before the CB3 gate. Nothing in the layer or the programs changed. The two
programs were relinked first (`base_unittests` and `test_child_process`
removed from the kept tree's `out/jamos`, then `tools/cr/build.sh start
base_unittests test_child_process`: 202 s; the stage's `base_unittests` is
76,956,088 bytes, CCS's was 76,947,456). The six shards on an ext `/data`,
in base's own (threadsafe) death style, **all PASS** (batch 18090349, 698 s
for all six, each shard 407 to 696 s on a PC the other tracks were using):

| `/data` | pass | self-skip | on the skip list |
|---------|-----:|----------:|-----------------:|
| ext, CCS (2026-10-17) | 8,697 | 36 | 223 |
| ext, BM1 | 8,700 | 37 | 219 |

Per shard (of 8,956): 1,232 + 3 + 3 of 1,238; 1,468 + 0 + 8 of 1,476;
1,959 + 4 + 52 of 2,015; 1,406 + 9 + 140 of 1,555; 1,329 + 17 + 3 of 1,349;
1,306 + 4 + 13 of 1,323 (pass + self-skip + listed). Compared test by test
with CCS's six shards (both runs' `results.txt`): **no regression**. The
only differences are the four that CBF6 and TL took off the list, which now
pass for real (`SmapsRollupTest.ReadAndParse`,
`FileUtilTest.ChangeFilePermissionsAndWrite`,
`FileUtilTest.MakeAbsoluteFilePathNoResolveSymbolicLinks` and
`DriveInfoTest.GetFileDriveInfo`, on the 6 GiB stick), and one result CCS's
parser got wrong: `SchedulerLoopQuarantineTest/0.ScopedOptOut` is a
deterministic skip (the typed test skips the cases that are not thread
bound, `/0` and `/1`), but in CCS's log a kernel line (`extfs /data: mapped
file ... gone`) fell in the middle of its `[  SKIPPED ]` line and the test
was counted OK. CCS's true count was 8,696 pass / 37 self-skip. Fixed in
`base-run.py`: a test whose result line is lost is taken from gtest's list
of skipped tests at the end. The ENOSYS log is CCS's (`dlopen`, ioctl
`0x80086601`, another process's `/proc/<pid>/stat` and `task/0/status`,
`/proc/self/ns/user`: all in the listed groups). No panic and no "killed"
line in any shard's kernel log.

**The 6 GiB sparse stick (TL), first boots.** Every boot started, mounted
`/data` (`init: /data mounted`) and ran its tests. A one-suite run
(`DriveInfoTest`, batch 18092250) sampled the images as it went: the stick
`base-stick.img` is 6,400 MiB apparent (the 6,144 MiB `/data` and the boot
parts) and **117 MiB on disk**; each boot's copies (the listing boot's and
the run's stick and image) 120 to 121 MiB on disk; a run holds at most
five such files, and none after it ends. The PC's used disk was 35 MiB more
after the run than before (the outputs). Six shards at once hold some
3.5 GB at the most, where a 6 GiB write per boot would have held many times
that.

**What came off the list: nothing more.** The listed suites run in full with
`CR_BASE_ALL=1 CR_BASE_LISTED=1` (batch 18091719, 311 s, six shards of 3 to 6
suites): the 54 named tests all still fail (50) or crash (4), each for its
group's cause (`FilePathWatcher*`, 165 tests, is not run: `inotify_init` is
`ENOSYS`). The 8 writable-mapping tests (`MemoryMappedFileTest.WriteableFile`
and the like) still fail at the mapping itself: WE1 is the file service's
side, libc's side (WL1) has not merged. The list stays 219: 165 `inotify`
and 54 named, by group: launching 5, thread id 3, record locks 7, `CLONE_FS`
1, handler registers 1, other processes' `/proc` 2, `mincore` 9,
`CheckAgainstProcMaps` 1, `dlopen` 3, build-id 1, PROT_EXEC 3, a socket
sent over itself 1, `mkfifo` 4, the 256-byte path 1, `fdopen(-1)` 1,
writable mappings 8, `ZeroSegment` 2, and the stack-trace death test (the
layer's own heap) 1.

#### MC notes (2026-10-18)

Track MC (branch `mc-mincore`): exact `mincore`, the owner's decision D7 (a
read-only kernel query of which pages of the caller's own mappings are
there).

- **The call: `vmar_resident` (66)** (`abi/syscalls.def` has the contract).
  One byte per page of `[addr, addr + len)` of the caller's own address
  space, 1 when the VMO mapped at the page holds it: committed (a page
  shared copy-on-write counts in every table holding it, so a fork's child
  and its parent both have it), any page of a contiguous or physical VMO,
  a pager VMO's page in its cache whatever its state (clean, dirty,
  writing); 0 for a page never touched, decommitted, evicted, or past the
  VMO's size. The mapping's protection doesn't matter: `PROT_NONE` memory
  keeps its pages, as Linux reports them. It returns how many pages from
  `addr` are mapped (fewer: a hole right after them; 0: none at `addr`), at
  most `VMAR_RESIDENT_MAX` (4096, a page of bytes: Linux's chunk) a call;
  `RIGHT_READ` on the vmar, which must be the caller's own
  (`ERR_ACCESS_DENIED` for a parent's handle of its child's: no process
  learns which pages another touched). The ABI stamp changes.
- **Pager VMOs and Linux's side-channel rule.** Since Linux 5.0 `mincore`
  tells the page cache's state only for a file the caller could open for
  writing, and says every page is resident for any other mapping of a file,
  because a repeatable look at another program's reads of a shared file
  (a library, a font) is a side channel. The kernel keeps the same rule: a
  pager VMO's cache is told only through a mapping whose maximum may write
  it (a writable pager VMO through a handle with `RIGHT_WRITE`); through
  any other its pages below the size read 1, and so do a pager VMO's
  child's window pages it doesn't hold itself (they would come from the
  pager VMO; a `MAP_PRIVATE` mapping of a file). A private file mapping of
  a file the program may write therefore reads 1 where Linux would tell the
  cache: the child can't know its creator's rights on the file.
- **The work.** `aspace_resident` looks once under the region lock and
  `vmo_resident` reads each VMO's table a leaf (or a run of missing
  tables) per lock hold, as decommit does, into a kernel buffer the call
  copies out with no lock held. A 256 GiB reservation with three pages
  touched is swept in 16 ms (16,384 calls).
- **libc.** `lx_mincore` (`user/posix/sys/mem.c`) asks a chunk at a time,
  the kernel writing `vec`: `EINVAL` for an unaligned start, `ENOMEM` past
  the user range or at an unmapped page (the bytes before it written),
  `EFAULT` for `vec` (at once past Linux's `TASK_SIZE_MAX`, else when the
  copy fails, after a hole's `ENOMEM`), `EAGAIN` if the kernel can't get
  its buffer. A memfd's pages all read 1: they are committed as it grows
  (a kept VMO, `memfd.c`), where Linux's shmem counts the written ones.
- **Tests.** ktests `vmar_resident_*` (anonymous, holes, copy-on-write,
  pager, the sparse sweep, the handle layer's refusals, a fuzz against the
  VMOs' tables) and posixtest mem's `check_mincore_pages` (one page in the
  middle, as `CheckPageInCore` asks; fails with the old guess).
- **Should clear** (`base_unittests` not run here): the nine listed for
  `mincore`, `AlternateTestParams/PartitionAllocTest.PurgeDiscardable*`
  (`NonPageSizedAlloc`, `NonPageSizedAllocOnSlotBoundary`, `SecondPage`,
  `WithFreeListStraightening`, each `/0` and `/1`) and
  `SamplingHeapProfilerTest.GetSamplesWithResidency`. They stay on the skip
  list until a base run shows them passing.

#### TS (2026-10-18)

Track TS (branch `ts-threads`): four decided libc changes (the owner's D1
and D10, SGR's design question 1 answered (a), SX's question 1), and
FR2's findings 9 and 10.

**The main thread's tid is the pid (D1).** The kernel hands out pids and
tids from two spaces, each 1 .. `IDS_MAX` - 1, so a kernel tid may be a
live pid, ours included. thread.c now gives Linux's ids: the first
thread's is the pid (in a forked child its forking thread's, and after an
exec in place the thread that exec'd), every other thread's its kernel
tid plus `IDS_MAX` (2^22): unique, never a pid, within musl's 30 bits
(below 2^23). Only thread.c sees kernel tids; every user of a tid
(`tgkill`/`tkill`, `/proc/self/task` and `/proc/<tid>`, the `sched_*` and
priority calls, `SIGEV_THREAD_ID`, the signal list, socketpair sleepers,
the tid words) goes through it or `gettid`. A forked child's
`CLONE_CHILD_SETTID` word is its pid (`ForkWithFlags`). Robust lists stay
`ENOSYS` (musl walks them itself); no ptrace. Another process's view of
our threads (D12's `/proc/<pid>/task`, later) must map the same way.

**`CLONE_FS` for a process child (D10)** is a copy, not a share: the
child starts with our directory, root and umask (fork's), and a later
change on either side is its own (fork.c's `CLONE_PROC_OK` says why).

**A stack for calls on small stacks (SGR (a)).** CCS's stack for
clone()'s child is every small stack's (callstack.c): a pthread whose
stack is under 64 KiB (musl's clone.c passes pthread_create's size; an
unknown size counts as small) gets 64 KiB over a guard page at creation
and makes every call there; a thread given a sigaltstack under 64 KiB
gets one there, and a handler's calls on its alternate stack move to it.
While such a call runs, the kernel's exception stack is cut to below the
handler's frames (sigfault.c's `sig_alt_call`): the kernel puts a raise's
or a fault's frame at the top of the range when RSP is outside it, which
would be over them. The thread counts as on its alternate stack until the
call is back (`sigaltstack`'s `SS_ONSTACK` and `EPERM`; a nested
`SA_ONSTACK` handler runs where it is). A handler run on the alternate
stack from a call's boundary, or from a fault or raise while on the stack
for calls, leaves frames there: a call it makes starts below them
(`lx_call_stack_hold`). musl's direct entries (`__jam_posix_spawn`,
`__jam_resolve`, `__jam_ttyname`, `__jam_dladdr`'s lookup,
`__jam_unmapself_prepare`, `__jam_clone`) run where calls run. The stack
goes at the thread's end (`lx_exit` unmaps it from on it, registers only
after the unmap; a detached thread's `__unmapself` half from off it).
**Cost** (posixtest fork's `stacks` line, 4 CPUs under KVM, `getppid`):
22 ns a call in a process where no thread has one (CCS's one global
load), 24 ns once one has (a thread with its own big stack), 24 ns with
a small sigaltstack (calls from the thread's own stack), 25 ns on a
16 KiB thread (every call moved). Left as CCS listed it: the fault and
raise entries' own C on a small interrupted stack (SGR finding 7).

**A narrowed child gets no image (SX's question 1).** libjam's
`spawn_args.no_image`; libc's spawn sets it for a `posix_spawn` child
with `posix_spawnattr_setgrants_np`'s list, which is then raised at
`/svc/exec` (its caller not known) and finds `/proc/self/exe` by its file.
utest `exec_starter`'s last step checks it (launch.c's `narrow`).

**FR2's findings 9 and 10.** The tid words of a clone go through
`uc_put_int`: a bad pointer is no store and the clone goes on, as Linux's
unchecked `put_user` (FR2's coordinator asked for `EFAULT`; Linux doesn't
fail the clone). A `clone()` child's musl tid is not stale: musl 1.2.6's
`clone()` runs the child through `clone_start` and `__post_Fork`, which
sets it from `set_tid_address` (the child's pid since D1); posixtest's
`clone` case checks an aborting child ends with `SIGABRT`.

**Base tests that should clear** (a relink; the skip list's groups):
`ForkWithFlagsTest.UpdatesPidCache`,
`PlatformThreadTidCacheTest.MainThreadFirst` and `.MainThreadSecond`
(thread id), `ProcessUtilTest.LaunchProcess` (`CLONE_FS`).

#### MH (2026-10-18)

Track MH (branch `mh-mmap-hint`): the owner's decisions D4 (mmap's address
hint; Chromium's programs above 4 GiB by a link flag) and D5 (`--build-id`).

- **The hint (D4a, `user/posix/sys/mem.c`).** Without `MAP_FIXED`, an
  address rounded down to its page is tried as a fixed map when no libc
  mapping overlaps it; the kernel refuses one over anything else or outside
  the user range, and the mapping then goes where the kernel places it.
  `MAP_FIXED_NOREPLACE` was already `EEXIST` over anything. posixtest's mem
  section checks both ways (memres.c).
- **Placement top down (kernel, `kernel/mm/aspace.c`).** A hint alone could
  not empty the low 4 GiB: first fit from `USER_BASE` put every process's
  first mappings at 0x1000 up (MC saw `mincore` find a mapping at 0x1000:
  that, the layer's and libjam's first maps), and the layer's table alone
  is 2.5 MiB. `vmar_map` without `VMAR_FIXED` now takes the top of the
  highest gap that holds the mapping, as Linux places mmap; no ABI change.
  The ktests that pinned first fit's addresses were mirrored.
- **A program anywhere.** `posix.ld` starts the image at
  `__jamos_image_base` when the link defines it (the drivers'
  `--jamos-image-base=ADDR`, defined before the script is read) and every
  object a program links is position-independent: libc.a (musl, the layer,
  libjam's part, crt1) and the drivers' default are `-fPIE`, LLVM's runtimes
  `CMAKE_POSITION_INDEPENDENT_CODE=ON`. A recipe line `image-base <address>`
  links all of a port's programs there. posixtest is linked at 1 TiB, so
  every posixtest run (start, fork, exec in place, backtrace, `/proc`,
  `dladdr`) is a program above 4 GiB, and its mem section checks that
  `[0x1000, 4,097 MiB)` is free and that the zero segment's hint is taken.
- **The zero segment is `[64 KiB, 4,097 MiB)`**, not 4 GiB:
  PartitionAlloc's `InitZeroSegment` reserves from the end of its
  well-known regions to `USER_SPACE_ZERO_SEGMENT_SIZE_MB` (4,097) MiB, one
  `AllocPages` at that address, keeping 64 KiB when it doesn't get it. With
  `base_unittests` linked at 4 GiB (one run, batch 18105816):
  `ZeroSegmentInaccessibleEnd` passed, `ZeroSegment` read 65,536 (the
  image at 4 GiB was inside the 4,097 MiB).
- **Not done: Chromium at 1 TiB.** Linked there, `headless_shell` fails:
  BoringSSL calls weak functions nothing defines (`OPENSSL_memory_alloc`,
  `_free`, guarded by a null check), and a static non-PIE link resolves
  them to 0, which a call (`R_X86_64_PLT32`, plus or minus 2 GiB) can't
  reach from 1 TiB. Linux links Chromium as a PIE, at 0, where 0 is near.
  So the recipe has no `image-base` yet and both `ZeroSegment` tests stay
  listed. The way: **static PIEs** (lld `-static -pie --no-dynamic-linker`,
  `posix.ld` keeping `.dynamic` and the relocations, libjam's `spawn.c`
  loading an `ET_DYN` at a high base, the layer's start applying its
  `R_X86_64_RELATIVE` relocations before anything else), which would also
  give the programs ASLR later.
- **The build-id (D5).** `posix.ld` keeps `.note.gnu.build-id` in its own
  `PT_NOTE` (after the program's list; the list's readers, `wants.c`,
  `pkglist.c`, `dataprog.c` and `tools/checkwants.py`, match the JamOS name
  and skip it), and both drivers pass `--build-id` (ld's SHA-1, lld's 64-bit
  hash; Chromium's later `=fast` wins). A content hash: posix-check links
  its test program twice and compares the bytes; the ABI stamp hashes only
  `abi/` and `posixfds.h`, so it is unchanged.
  `ElfReaderTestWithCurrentElfImage.ReadElfBuildId` passes and is off the
  list (218).
- **Relinking.** `ports/chromium/build.sh` removes a target's program when
  the link's inputs ninja can't see (the wrapper, `tools/jamos-clang`, the
  sysroot's libraries, crt files and `posix.ld`) changed since its last
  link (`<target>.links`), so a libc change relinks without a hand removal;
  `test_child_process` is a default target.

#### WT1 (2026-10-18)

Track WT1 (branch `wt1-wmap-tests`, [WRITEMAP-PLAN](WRITEMAP-PLAN.md#wt1-notes)):
the eight writable-mapping tests, run with `CR_BASE_ONLY="MemoryMappedFileTest
FilePersistentMemoryAllocatorTest PersistentHistogramAllocatorTest
PersistentMemoryAllocatorTest" CR_BASE_ALL=1` (35 tests of the four
suites) on an ext `/data` with the extfs and libc of main after WE2 and WL1:
**all 35 pass**, the eight among them (`FilePersistentMemoryAllocatorTest`'s
`AcceptableTest` and `ExtendTest`, `MemoryMappedFileTest`'s `ExtendableFile`
and `WriteableFile`, `PersistentHistogramAllocatorTest`'s `CreateSpareFile`,
`CreateWithFile` and `MovePersistentFile`, `PersistentMemoryAllocatorTest.TruncateTest`).
They are off `tools/cr/base_unittests.skip` (211 on ext); on a FAT `/data`
they are in `base_unittests.fat.skip` (no writable shared mapping there; not
measured on FAT). The programs were the stage's as they stood (76,956,088
bytes, linked at 10:06 that day, four minutes after WL1 merged at 10:02, so
by all signs with WL1's libc): the relink that [BM1](#bm1-2026-10-18) did by
removing the two programs from the kept tree was refused to the track, and
`tools/cr/build.sh start base_unittests test_child_process` alone relinks
nothing (ninja does not see the sysroot's libc), so the track could not
make that sure itself: whoever relinks next, `CR_BASE_ONLY` with these four
suites is a one-minute check.

#### TD notes (2026-10-18)

Track TD (branch `td-threads`): a way to see where a stuck program's
threads wait, for `headless_shell`'s start-up hang and every later one.

- **The listing.** debug_command `threads <pid|name>` (kernel/debug/threads.c,
  `<jam/threadlook.h>`), the shell's `ps -t <pid|name>` (ps's PID is the
  process's koid; a name finds the first process of that name in the
  caller's tree). Into the kernel log, so a serial log has it too. For each
  thread: its state and for how long (the TSC when it last left a CPU,
  `off_tsc`, one store in the scheduler's switch accounting); what it waits
  for, from a **wait note** the thread itself sets around the waits worth
  naming (`thread_wait_note`, three stores and three back: a futex's
  user address, object_wait_one's object and signals, a channel read's or
  call's end, a port, a pager VMO's page and its write-back, a sleep), with
  the wait's deadline (`wake_at_ns`, now stored for every wait); a kernel
  mutex wait (`mutex_since`); the exception entry's window; the system call
  it is in with its first three arguments, and its user RIP, RSP and RBP
  from the frame at the top of its kernel stack (a system call's while
  `in_syscall`, else a trap's from ring 3); its kernel frames while it is
  off a CPU (switch_context's saved frame, then the frame-pointer chain,
  every read inside its own stack); a user backtrace by frame pointers (48
  frames at most); and the words among the 64 above RSP that point at code
  (`ps -t <pid|name> <words>` looks at up to 4,096: the whole call chain of
  a thread whose frames keep no records).
- **Read only, nothing a program can reach.** Only debug_command runs it
  (`RIGHT_ROOT_DEBUG`); no new system call. Each thread's own fields and
  stack words are copied under its process's lock, which keeps the stack
  (an exiting thread marks itself gone under that lock first). The user
  memory is read a word at a time by `aspace_peek` under the page-table
  spinlock, with no lock held across two words and no fault or wait: an
  absent, untouched or evicted page, a device's page or one with a cache
  mode of its own ends the walk. A frame chain is followed only upward and
  within 16 MiB, so a hostile chain ends.
- **Frame pointers.** The kernel, libjam and the POSIX layer keep them;
  musl's own code is built with `-fomit-frame-pointer` (user/posix/posix.mk
  MUSL_CFLAGS), so a wait inside musl (its locks, `__wait`, `pthread_once`)
  leaves RBP, or a record's saved RBP, holding something else: the walk
  then goes on from the first record in the 256 words above (one whose
  saved RBP points higher up; such frames are marked "found by a scan", and
  may now and then be a stale pair), and the stack words that point at code
  name musl's own frames in between. Chromium is built with frame pointers
  (`enable_frame_pointers = true` for Linux in build/config/compiler/
  compiler.gni; out/jamos's args.gn doesn't change it), `symbol_level = 0`:
  function names from the symbol table, no lines.
- **On the host.** `tools/symbolize-threads.py <log> <program>` prints a
  captured listing again with each user frame's function and line from the
  unstripped program (llvm-symbolizer: Chromium's toolchain's under ~/cr on
  the PC), each futex address with the global it lies in, and each system
  call's name from abi/syscalls.def.
- **`headless_shell`'s start-up hang, found with it** (main 0de63088, the
  out/jamos link of 13:10): a **recursive `pthread_once` in Chromium's
  sandbox libc interceptor**, not PartitionAlloc. The main thread waits on
  the futex at `sandbox::g_libc_localtime_funcs_guard` (value 3: "init
  running, waiters present") in musl's `__wait`, with the signal thread in
  its port_wait. The chain, from the walk and a 2,048-word scan: `main` ->
  `headless::HeadlessShellMain` -> `content::ContentMain` ->
  `ContentMainRunnerImpl::Initialize` -> `base::i18n::InitializeICU` ->
  `icu::TimeZone::createDefault` -> `detectHostTimeZone` -> `uprv_timezone`
  -> `localtime` (the interceptor's override,
  sandbox/linux/services/libc_interceptor.cc) -> `pthread_once(guard,
  InitLibcLocaltimeFunctionsImpl)` -> `InitLibcLocaltimeFunctionsImpl`:
  `dlsym(RTLD_NEXT, "localtime")` finds nothing in a static program, so it
  logs "Your system is broken: dlsym doesn't work!" -> `LOG(ERROR)` ->
  `logging::LogMessage::Init` (the line's time stamp) -> `localtime_r` (the
  interceptor's again) -> `pthread_once` on the same guard, still running
  -> waits for itself for ever. Nothing was printed because the log line
  is never finished. A fix (main session's call): give the interceptor
  libc's functions, as patch 0910 gave base its `close`: the layer's
  `dlsym(RTLD_NEXT, ...)` answering the four names `localtime`,
  `localtime_r`, `localtime64`, `localtime64_r` with musl's
  (`__localtime_r` and a `localtime` on it) is the layer-side way; any
  other way through `InitLibcLocaltimeFunctionsImpl`'s fallback still logs
  inside the once.

#### HS notes (2026-10-18)

Track HS (branch `hs-headless`): `headless_shell --dump-dom` of a `data:`
page, one stop at a time, each fixed in the layer or a service.

- **Stop 1: socketpair half-close.** After the dlsym fix Chromium got
  through ICU, `ContentMainRunner` and logging, then
  content/browser/sandbox_host_linux.cc:41 `PCHECK(0 == shutdown(child_socket_,
  SHUT_RD))` failed with EOPNOTSUPP and aborted. `SandboxHostLinux::Init`
  makes a `SOCK_SEQPACKET` pair, shuts the child's end for reading and the
  browser's for writing. Built in sockpair.c: `SHUT_WR` ends the
  description's writes (EPIPE) and queues an **end part** (an `sp_hdr` of
  magic `SP_END` and nothing else, with an event as its one handle, so it
  is never taken for plain bytes): the peer reads what came before it, then
  the end for good. `SHUT_RD` reads what was queued, then the end instead
  of a wait. Sleepers in a read (`SHUT_RD`) or a write (`SHUT_WR`) are
  woken as `SHUT_RDWR`'s are; poll says readable (and writable after
  `SHUT_WR`); both halves are a `SHUT_RDWR`. **Gaps:** the peer of a
  `SHUT_RD` end can still write to it (Linux: EPIPE), and the end reads
  those bytes: telling the peer needs a signal channels don't have (a
  kernel change; nothing Chromium does needs it). An end part that finds the
  channel full is owed and goes with the end's next read or send (else the
  peer sees the end at the close). A child given the end starts with
  neither half shut. posixtest scm: `check_half` and scmcred's wakes.
- **Stop 2: V8's code range.** Next, `V8 process OOM (Failed to reserve
  virtual memory for CodeRange)` and the crash's breakpoint: V8 with its JIT
  reserves code memory, which needs `MAP_JIT` and its patch (section 12,
  CR-e). Not a layer stop: the plan's mode until CR-e is `--jitless`, so
  the run says `--js-flags=--jitless` (V8 then reserves no code range).
- **Stop 3: nothing printed, exit 0.** `--dump-dom` runs its commands in a
  page (`chrome://headless/headless_command.html` and its script) served from
  `headless_command_resources.pak`, which `EnsureHeadlessCommandResources`
  loads from beside the program; the port staged only
  `headless_lib_data.pak` and `headless_lib_strings.pak`, so the page was
  empty, `executeCommands` threw, and the handler printed nothing and exited
  0. The port's recipe and install now ship the pak too.
- **It prints the page.** `run /data/cr/out/jamos/headless_shell
  --no-sandbox --single-process --no-zygote --headless --disable-gpu
  --js-flags=--jitless --dump-dom data:text/html,Hello%20from%20Jam%20OS`
  prints `<html><head></head><body>Hello from Jam OS</body></html>` and
  exits 0 after 1.5 s (QEMU on the PC, 4 GiB, the program on an ext
  `/data`). Logged on the way, none fatal: `landlock_create_ruleset`
  ENOSYS, fontconfig's missing default config, `NETLINK` sockets
  unsupported (`address_tracker_linux`). CB3's screenshots are next.

#### HS2 notes (2026-10-18)

Track HS2 (branch `hs2-screenshot`): `headless_shell --screenshot` of a
local page, one stop at a time.

- **Stop 1: `Page load failed: net::ERR_FILE_NOT_FOUND`, not a layer
  gap.** The run's log shows `FileURLLoader::Start:
  file:///data/cr/out/jamos/p`: the typed `run` line was 261 characters
  and the shell keeps 240 (`SH_LINE_MAX`, [USING](USING.md)), so the
  URL was cut to a file that isn't there, and the `; echo` after it was
  lost too. The test's script now puts the program and the URL in shell
  variables (`H=...`, `U=...`, `run $H ... $U`). No change to the layer.
- **The screenshot.** `run $H --no-sandbox --single-process --no-zygote
  --headless --disable-gpu --js-flags=--jitless --window-size=800,600
  --screenshot=/data/cr/tmp/shot.png file:///data/cr/out/jamos/page.html`
  (Inter from `third_party/inter` and a one-line `fonts.conf` with its
  cache in `/data/cr/tmp/fccache`, `FONTCONFIG_FILE` set) printed `10959
  bytes written to file /data/cr/tmp/shot.png` and exited 0 after 1.9 s
  (QEMU on the PC, 4 GiB, an ext `/data`). The PNG is 800x600 RGB and
  drawn right: the page's background `#1e3a5f`, the red box `#e94560`
  320x140 at (40, 40) and the yellow `#f5d76e` 220x90 at (40, 200), exact
  to the pixel and colour, and the heading and line of text in Inter
  below them. fontconfig made its cache directory; nothing else was
  logged but HS's `landlock_create_ruleset` ENOSYS and `NETLINK`. The
  software compositor, shared memory for raster and the PNG write all
  worked without a change. CB3's HTTPS page and cr-test.sh's `headless`
  case are still to do.

#### CB3 notes (2026-10-18)

Track CB3 (`headless_shell`, CR-c1): one process drawing a screenshot of a
page on `/data` and of a page over HTTPS. It was reached by hand on
2026-10-18, one stop at a time (the owner wanted to see it before the rest
of the gate: tracks TD, HS and HS2, then the main session's own fixes and
runs on the hardware); CB3's part is this record and cr-test.sh's
`headless` case, which runs it again whenever the layer changes.

The run: `headless_shell --no-sandbox --single-process --no-zygote
--headless --disable-gpu --js-flags=--jitless`, then `--dump-dom` (the DOM
on the program's standard output) or `--window-size=800,600
--screenshot=<file>`; `TMPDIR` and `HOME` at `/data/cr/tmp`,
`FONTCONFIG_FILE` at a fonts.conf (one `<dir>` line and a `<cachedir>`)
beside Inter, the only font staged. The program's list is
`ports/chromium/wants/headless_shell` (`posix`, `svc net`, `svc dns`,
`mount /data rw`); beside it in `/data/cr/out/jamos` three paks:
`headless_lib_data.pak`, `headless_lib_strings.pak` and
`headless_command_resources.pak`. No GN argument changed: CB1's
`files/args.gn` build is the one that runs (the recipe's comment names the
flags). The sandbox, `fork` (no zygote) and the JIT stay off until CB5 and
CR-e.

**The stops, in the order they came** (all but the last two found in QEMU
on the PC; none needed a Chromium patch):

1. **A hang at start-up.** The first smoke runs (`--dump-dom` of a `data:`
   URL) sat at 0 CPU with two threads for 250 s and printed nothing (hs3
   ran the stage's older link: a relink leaves the new program in the
   kept tree's `out/jamos`, not in the stage, which is why the `headless`
   and `shell-wl` cases read it from there; hs4, the fresh link, hung the
   same way). TD's thread listing (74473307) found the wait: Chromium's sandbox libc interceptor
   looks up `localtime` with `dlsym(RTLD_NEXT)`, a static program has no
   other module, the miss is logged from inside a `pthread_once`, and the
   log's time stamp calls the interceptor's `localtime_r` again: the main
   thread waits for itself ([TD notes](#td-notes-2026-10-18)). Fixed in the
   layer, 4d756b99: an override of musl's `src/ldso/__dlsym.c` answers
   `RTLD_NEXT` for `localtime`, `localtime_r` and their 64 names with
   libc's own; posixtest's `start` checks it.
2. **`shutdown(SHUT_RD)` on a socketpair** (HS stop 1, 1a1b6b07): the
   sandbox host's `SOCK_SEQPACKET` pair needs half-close; the end part and
   the wake-ups are in [HS notes](#hs-notes-2026-10-18).
3. **V8's code range** (HS stop 2): `--js-flags=--jitless`, the mode until
   CR-e; **`headless_command_resources.pak`** (HS stop 3, 64e1924e):
   without it `--dump-dom` printed nothing and exited 0. HS's merge
   (dddfb00f) printed `<html><head></head><body>Hello from Jam OS</body></html>`.
4. **The shell's 240-character line** (HS2 stop 1, f4a213fc): a
   `--screenshot` of a local page failed with `net::ERR_FILE_NOT_FOUND`
   because the typed `run` line was 261 characters and the shell kept 240,
   so the URL was cut to a file that isn't there (and the `; echo` after
   it was lost). Not a layer gap. The scripts keep a program and a URL in
   shell variables; the `headless` case now refuses a script with a `send`
   line over 240. The shell should flag a cut line (beep or a message)
   instead of dropping the rest silently: a small queued fix.
5. **The screenshot** (HS2, merged 43923620): an 800x600 PNG of a local
   page, every box and colour exact, Inter text through fontconfig, in
   1.9 s; the software compositor, the shared memory for raster and the
   PNG write needed no change ([HS2 notes](#hs2-notes-2026-10-18)). Six
   harder pages followed the same day (layout, text, SVG and canvas, a
   page built by JS, advanced CSS: 3D transforms, `clip-path`, blend modes,
   backdrop blur; a landing-page mock): all exit 0 in 2 to 3 s;
   `WebGL` says "not available" under `--disable-gpu`.
6. **A page by name** (e504d6f4): the same interceptor's `getaddrinfo`
   asks `dlsym(RTLD_NEXT, "getaddrinfo")` for the real one, got nothing
   ("Cannot find getaddrinfo with dlsym") and every name failed with
   `ERR_NAME_NOT_RESOLVED`: a page loaded only with `--host-resolver-rules`,
   which skips the resolver. musl's `getaddrinfo` is now an override
   (upstream's code as `__getaddrinfo`, `getaddrinfo` a weak alias so a
   program's own definition still wins) and the same `__dlsym.c` hands it
   out. Checked in QEMU the same day: `http://www.jam:8000/page.html` by
   name through netpeer's DNS and relay, its picture from `mac.jam`.
7. **On the hardware** (the PC from its installed drive, a shell over
   ssh, the program fetched into `/data/cr` and approved by the owner):
   `--dump-dom http://example.com` and a `--screenshot` of
   `https://example.com` worked, TLS and Chromium's built-in root store
   with no certificate files on Jam OS (names then went through
   `--host-resolver-rules`: stop 6 was not yet in), and Google, Wikipedia,
   GitHub, Apple, the BBC, Hacker News and YouTube drew (1280x900,
   `--timeout=20000`); the screenshot is taken at the load event (about
   5 s on YouTube). Non-Latin text is tofu (Inter only is
   staged: a font set is a later track). Not yet repeated on the hardware
   with stop 6 in.

**Findings that stay open.**

- **WebAssembly is off under `--jitless`** (V8 has no Wasm without its
  compilers): CR-e (the JIT) brings it.
- **WebGL is off** with `--disable-gpu`: a software GL (SwiftShader) is a
  small track of its own.
- **`content_shell` ignores `--proxy-server`**, `headless_shell` honours it
  (found browsing the open internet from QEMU, 10-18: a CONNECT proxy on
  the test host through netpeer's relay carried YouTube's home page and a
  search, both rendered; for `content_shell` the way was QEMU's own NAT
  with an untagged image). Google's search shows a reCAPTCHA to the
  `HeadlessChrome` user agent: left as it is. `--virtual-time-budget`
  never ends on YouTube (the page is never idle): use `--timeout`.
- The `NETLINK` socket (`address_tracker_linux`) fails with EAFNOSUPPORT in
  every run: a log line, and the pages load. `landlock_create_ruleset`
  ENOSYS is logged at start-up and is as harmless.

**The test: cr-test.sh's `headless` case** (TESTING.md). The program, its
three paks, Inter and a fonts.conf go on a 2 GiB stick
(`tools/cr/base-stick.py`'s lists, as `shell-wl` does) and one boot runs
`tools/cr/headless.txt`: the page `tools/cr/headless.html` (a red box, a
picture, a heading, and a script that writes a line into the page: `js ok
42, picture 320x160`, so the DOM shows that V8's interpreter ran and the
picture arrived) three times, each with `--dump-dom` (into a file) and
`--screenshot`:

- **(a)** `file:///data/cr/out/jamos/page.html`, the picture beside it;
- **(b)** `http://www.jam:8000/page-http.html` by name through netpeer's
  DNS and `--tcp-relay` to `tools/cr/headless-server.py` on the test host,
  its picture from `http://mac.jam:8000/pic.svg` (a second name);
- **(c)** `https://www.jam:8443/page-https.html` from the same server over
  TLS with a certificate `openssl` made when the case started (an EC
  key, `www.jam` and `mac.jam` as its names, two days), trusted by this
  run only: `--ignore-certificate-errors-spki-list=<the base64 SHA-256 of
  its public key>`, not the blanket `--ignore-certificate-errors`; the
  picture from `https://mac.jam:8443/`. A last run of (c) without the flag
  must fail: `Page load failed: net::ERR_CERT_AUTHORITY_INVALID`, nothing
  dumped, and the server sees the handshake fail, which shows that the
  flag is what let the others through and that Chromium on Jam OS
  verifies the chain.

`tools/cr/headless-check.py` then reads what the guest wrote (out of the
saved stick, by `jamext`) with the serial log and the servers' logs: each
run's exit status, the DOM's title and script line, each PNG's size and
the colour at six points (the box, the picture, its circle, the ground)
and that the heading and the picture's own text have white pixels, and
what each server saw: `GET /page-http.html` for `www.jam:8000` and
`GET /pic.svg` for `mac.jam:8000` (twice each: the dump and the shot, from
a `HeadlessChrome`), the same on 8443 over TLS 1.3, and the one failed
handshake of the refused run. The three PNGs came out byte for byte the
same (15,069 bytes). A headless run takes some 2.2 s in the guest; the
case about 25 s once the PC gives it its slot (batch 18220809: three runs
of it and the full init run, all PASS).

#### CB4 notes (2026-10-18)

Track CB4 (`content_shell` on the desktop): one stop, then everything the
plan asked for worked without a change to the layer, the compositor or
Chromium.

- **Stop 1: every open under /data said ENOENT.** content_shell aborted
  250 ms after its start at ui/base/resource/resource_bundle.cc:579
  (`data_pack->LoadFromPath(path)`: content_shell.pak, beside the
  program), after base's stack-trace set-up logged "Failed to open file:
  /data/cr/out/jamos/content_shell: No such file or directory", though
  `ls` showed both files and headless_shell, started the same way in the
  same boot, read its paks. A trace in libos's `ns_channel` (taken out
  again) showed the cause: the program's namespace held one entry, `/tmp`.
  Its ELF note, which is what the shell grants from, said only `posix`:
  the port's link wrapper (tools/portslib/toolchain.py, `cc_wrapper`)
  takes a program's list from `wants/<name>` only for the programs the
  recipe names with a `program` line, and content_shell had none, so the
  wants file (and the allow line made from it) never reached the note.
  Fixed in the port: the recipe names `lib/chromium/content_shell` and its
  `content_shell.pak`, and ports/chromium/build.sh builds and installs
  both by default. Relinked, the note lists `svc wayland` and `mount /data
  rw`, and the program starts.
- **The window.** `run /data/cr/out/jamos/content_shell --no-sandbox
  --single-process --no-zygote --disable-gpu --js-flags=--jitless
  --enable-logging=stderr http://www.jam:8000/page.html` (QEMU on the PC,
  4 GiB, the desktop at 1280x800) opens a window tiled beside the
  terminal: content_shell's toolbar (Back, Forward, Refresh, Stop, the
  address) and the page under it, 623x695, loaded by name through
  netpeer's DNS, drawn in Inter, in about 5 s. Ozone's Wayland found what
  it needs (wl_shm, xdg_shell, the seat, the cursor shape); logged and not
  needed: no DRM render node (software output), no `wp_viewporter`,
  `zcr_alpha_compositing_v1`, `overlay_prioritizer`, `wp_content_type_v1`
  or `wp_color_management_surface_v1` in the compositor, `NETLINK`
  sockets, and devtools' HTTP server refused (`listen` EACCES: the list
  asks for no listening).
- **Input.** All of it reached the page as it should: a click lands at
  the page's own coordinates (screen (800, 200) is the page's (153, 108));
  text typed while the window has the focus is the page's (the compositor
  types serial text on the US layout, Shift included); the `<select>`
  opens its list as a popup under it (GC3), picked by Down, Down, Enter
  and by a click on a row; the wheel scrolls (QEMU's `mouse_move 0 0 -1`
  is a notch down: three gave 360 pixels, smoothly); Super+Q closes the
  window and content_shell exits with status 0. The page says each event
  as a console line, which content_shell logs on its stderr
  (`INFO:CONSOLE`): that is what cr-test.sh's script waits for.
- **The shell's Ctrl+C.** While content_shell's window has the focus, the
  keys (and a test's serial text) are the window's, so Ctrl+C typed there
  is the page's, not the shell's, and so is Ctrl+Alt+C (the console's way
  to the shell's keys works among its own windows only). Super+Q ends
  content_shell (its window closed, status 0); so does a click on the
  terminal and Ctrl+C there: the shell's SIGINT, which content_shell
  leaves at its default ("killed by SIGINT").
- **The test.** cr-test.sh's `shell-wl` case (TESTING.md): the program
  from the kept tree's `out/jamos` (a relink leaves it there, not in the
  stage), a 2 GiB stick made by tools/cr/base-stick.py, which now takes
  its lists as options (`--programs`, `--program-dir`, `--out-files`,
  `--source-files`, `--add`; base_unittests' are the defaults), the page
  tools/cr/shell-wl.html, the script tools/cr/shell-wl.txt and
  tools/cr/shell-wl-check.py on the three screenshots. About 80 s.

#### CB5 notes (2026-10-19)

Track CB5 (multi-process `content_shell`): Chromium's own launcher,
unpatched, with `--no-zygote --no-sandbox --disable-gpu
--js-flags=--jitless`, no `--single-process`. Five stops, each a gap in
the layer; no Chromium patch, no kernel change.

- **Stop 1: the GPU process died at start.** `sandbox/linux/services/
  thread_helpers.cc:57 Check failed: ENOENT == errno: Not a directory`
  (signal 6), from `GpuInit::InitializeAndStartSandbox`: the sandbox's
  set-up runs with `--no-sandbox` too, stops the watchdog thread and
  waits, with `fstatat(proc_fd, "self/task/<tid>/")` on an `open("/proc")`,
  for the thread's directory to go. path.c took only a mount's directory as
  the start of an `*at` call (ENOTDIR for /proc's). Fixed (da623b1d): a
  /proc directory's descriptor stands for its path (procfs.c's
  `proc_dir_base`), and /proc/self/task's link count is 2 and one a thread,
  as Linux's (`IsSingleThreaded` reads it). The browser counted the GPU
  process's death a minute late: the crash handler's in-process
  symbolising of a 340 MB program.
- **Stop 2: a utility process never connected** ("Terminating current
  process after 15 seconds with no connection"), and the log said `posix:
  sendmsg: a descriptor of kind directory can't cross this socket`: the
  storage service's leveldb fsyncs its directories through the browser,
  which opens them and sends the descriptor over Mojo; the refused send
  broke the channel. Fixed (2d67cc9a): a directory crosses a socketpair by
  its path and getdents position, opened again in the receiver's namespace,
  as the hand-over gives one to a child (until directory handles:
  DIRHANDLE-PLAN). The refusal's log line now names the descriptor's path.
- **Stop 3: the renderer said nothing.** The window and toolbar drew (the
  GPU process's software compositor, its buffers to the browser and on to
  the Wayland compositor) but the page area stayed empty and the browser
  logged "renderer unresponsive": `sendmsg: a descriptor of kind /proc file
  /proc/36/statm can't cross`. The browser opens each renderer's
  /proc/<pid>/statm and status and sends them to it (blink's
  `MemoryUsageMonitorLinux`). Fixed (19f00760, ABI stamp: hand-over kind 14
  `POSIXFDS_PROC`; 7edf206e): a /proc file crosses by its path, the pid in
  it, and the receiver opens its own /proc's file there (the renderer's own,
  read live). A child's hand-over still leaves /proc files out, so the
  format's version stays 2.
- **Stop 4: the renderer waited for its fonts forever.** The title came
  ("Jam OS page") but the main thread sat in a sync Mojo call,
  `FontDataServiceProxy::MatchFamilyName` (and with
  `--disable-features=FontDataServiceLinux`, the older font service's), the
  browser idle. A scratch trace of every socketpair send and receive
  (JAMOS_SP_TRACE, not committed) showed one send from the browser to each
  renderer failing with ENOENT, and the browser never writing to that
  renderer again (Mojo takes a failed sendmsg for a broken channel): the
  swap file the browser gives each renderer (`ProvideSwapFileForRenderer`:
  `base::File` with `FLAG_DELETE_ON_CLOSE`, unlinked right after its open)
  had no path to open again, and a file's give on extfs opens its path.
  Fixed (b6f09dea): a give whose path is gone moves our own open (channel
  and buffer) to the receiver when its access is no wider than the
  description's, as fat's one writer is lent to a child; the sender's open
  is lent for good (its own later use of the file is an error: Chromium
  closes it at the send).
- **The page draws** (build/t/m10, batch 19062854; staged to the main
  session): the browser, the GPU process (software output), the network
  service, the storage service and two renderers (a spare one), six
  processes, `ps` showing each forked child in a job of its own under the
  browser's (FJ1); the page by name through the network service, Inter text,
  its console line logged by the browser.
- **Kill and reload.** content_shell has no "Aw, Snap!" (that is chrome's
  sad tab, CB6): a killed renderer leaves its view blank. Nothing outside
  the browser can kill a grandchild here (the shell's `kill` takes its own
  jobs and init's services; there is no kill by pid), so the test kills it
  the way Chromium's own debug URL does: `chrome://kill` typed in the
  address bar makes the renderer `kill(getpid(), SIGTERM)`. The browser
  reaps it (waitpid: killed by SIGTERM), its view goes blank, and Refresh
  starts a renderer again (fork and exec in place) whose page says visit 2
  (the tab's session storage, kept by the storage service).
- **Quitting.** Super+Q closes the window; the browser stops its children
  (some exit at once, the rest within about ten seconds) and exits 0, no
  process of it left. The shell's notice of a background program's end
  comes at its next prompt.
- **The stage** (owner's item for this track): a relink did refresh it
  already (every run that builds installs out/jamos's programs into
  ~/cr/ports/chromium/stage, stripped), but the stage's file times are the
  port's fixed epoch, so a look at them said nothing changed. The run's
  summary (`tools/cr/build.sh status|wait`) now gives each stage program's
  build id and whether it is linked against that run's libc (a9576ac9).
- Logged and not needed: `landlock_create_ruleset` ENOSYS, the f2fs
  `F2FS_IOC_GET_FEATURES` ioctl (0x8004f50c) ENOTTY from sqlite or leveldb,
  /proc/<child>/task (the browser setting its children's priorities; a
  child's directory has no task), `/proc/sys/kernel/yama/ptrace_scope`.
- **The test**: cr-test.sh's `multi` case (TESTING.md): the shell-wl
  stick and server (cr-test.sh's `shell_run`, shared now), the page
  tools/cr/multi.html, the script tools/cr/multi.txt (the processes listed,
  chrome://kill, Refresh, Super+Q, `ps` after) and
  tools/cr/multi-check.py on three screenshots and the log. About 2
  minutes, 6 GiB of QEMU memory.
- **Runs** (programs relinked against this branch's libc, run
  20261019-064751: all four stage programs "linked against this run's
  libc"): batch 19070239, 16 runs: make check, the init run, ktest at 4,
  basic, cmds, commands, kdump save, posix scm, procinfo, fork, execinfo,
  signal, cr-test shell-wl and multi PASS; files and posix-files FAIL on a
  boot race, not this track's: posix-files typed `posixtest files-many`
  before init had mounted /data (PASS alone, 19070749), and files.txt
  line 234 waits for "init: /tmp: in memory", which an ext /data mounted
  before init's /tmp thread first looks never prints (init goes straight
  to the view of /data/tmp): it failed twice under load, then PASS twice
  alone (19071309).

#### IC (2026-10-18)

Track IC (branch `ic-inotify-base`, [INOTIFY-PLAN](INOTIFY-PLAN.md#ic-notes)):
the 165 `FilePathWatcher*` tests of `base/files/file_path_watcher_unittest.cc`
on Jam OS's inotify (IP, IE1, IE2, IL1, IL2 merged).

**The relink.** `tools/cr/build.sh start base_unittests test_child_process`
through `remote-test.sh` alone relinked both (the per-target `.links` hash
of 1238f0da: WT1's note that it relinks nothing is out of date): 114 s, the
programs in `out/jamos` from 11:31 to 14:05 that day (11:31 was before IE2
merged at 11:45), `base_unittests` 79,192,072 to 79,206,176 bytes,
`test_child_process` 3,773,440 to 3,783,320; the stage's pair (stripped) is
76,973,832 and 1,662,976 bytes.

**All 165 pass.** `CR_BASE_ALL=1 CR_BASE_ONLY="FilePathWatcher*"` on an ext
`/data` (batch 18141141, twice: `TMPDIR` and `TEST_TMPDIR` at `/data/cr/tmp`,
and with `CR_BASE_ENV=` empty, where `/tmp` is a view of `/data/tmp` once
init has `/data`, so the same ext volume): 164 tests pass in 45 s of test
time (76 to 460 ms a test; the slowest 1,570 ms, a `...WithChangeInfoTest`;
`DirectoryChain` 1,098 ms, `InotifyLimitInUpdateRecursive` 856 ms) and one,
`FilePathWatcherTest.RacyRecursiveWatch`, ran past the 240 s a suite had. No
mismatch of events, no missing event, no wrong order, no limit reached (the
three `InotifyLimitIn*` tests pass with the 1,024 watches a descriptor and 8
descriptors a process of IL2), no crash, no `ENOSYS` line and no watch line
in the kernel log. `FilePathWatcher*` is off `tools/cr/base_unittests.skip`.

**`RacyRecursiveWatch`: 208 to 337 s, and the cost is extfs's.** With the
suite given 2,400 s (`CR_BASE_TIMEOUT`, new) it passes in 259 s (batch
18142200, `/data/cr/tmp`), 208 s (batch 18143407, `/tmp`) and 337 s in the
six-shard run (the PC at full load). The test makes 20 directories, each
with a file written and then both removed, 100 times over on a second
thread, and the main thread starts a watcher for each round and ends it at
its first event: 2,000 cycles of four requests on the stick's ext `/data`,
each request a commit. Posixtest's `inotify` section now times the same
cycle on `/data`: 63 to 330 ms a cycle by the PC's load, **the same with
Chromium's watches (a watch added on each new directory, 7 records read per
cycle) as without** (63.7 and 63.3 ms on a quiet PC; 272 and 223 ms, 329 and
304 ms under load), and 0.27 ms on the in-memory `/tmp` of a shell boot; a
`poll` blocked in one thread sees another thread's `mkdir` in 10 to 35 us.
So the 2,000 cycles are 127 to 540 s of commits on the PC's disk and the
watching is not in it. `base-run.py` now lets `FilePathWatcherTest` run 1,200
s (`SLOW_SUITES`); every other suite keeps 240 s (`--suite-timeout`,
`CR_BASE_TIMEOUT` in `cr-test.sh`).

**The whole program** (batch 18144626, six shards on an ext `/data`, base's
threadsafe death style, all PASS, 391 to 835 s a shard with the PC loaded;
the shard with `FilePathWatcherTest` is the slowest):

| `/data` | pass | self-skip | on the skip list |
|---------|-----:|----------:|-----------------:|
| ext, BM1 (2026-10-18) | 8,700 | 37 | 219 |
| ext, IC | 8,874 | 37 | 45 |

Per shard (of 8,956): 1,232 + 3 + 3 of 1,238; 1,468 + 0 + 8 of 1,476;
2,001 + 4 + 10 of 2,015; 1,532 + 9 + 14 of 1,555; 1,330 + 17 + 2 of 1,349;
1,311 + 4 + 8 of 1,323 (pass + self-skip + listed). It adds up from BM1's
without a remainder: 8,700 + 1 (MH, the build-id test) + 8 (WT1, the
writable mappings) + 165 (this track) = 8,874; listed 219 - 1 - 8 - 165 =
45; 37 self-skips as before. **No regression**: no new failure and no test
missing in any shard. The 45 left are BM1's groups less those three:
launching 5, thread id 3, record locks 7, `CLONE_FS` 1, handler registers 1,
other processes' `/proc` 2, `mincore` 9, `CheckAgainstProcMaps` 1, `dlopen`
3, PROT_EXEC 3, a socket sent over itself 1, `mkfifo` 4, the 256-byte path
1, `fdopen(-1)` 1, `ZeroSegment` 2 and the stack-trace death test 1 (the
layer's own heap); `base_unittests.skip` gives each with its reason.

**On a FAT `/data`** the 165 are in `tools/cr/base_unittests.fat.skip`: libc
refuses a watch on a writable FAT mount (`EOPNOTSUPP`, the owner's Q1 (B)),
so Chromium's watcher never fires there and each test would wait out its
timeout. Not measured on FAT.

#### LH (2026-10-18)

Track LH (branch `lh-layer-heap`): the layer's own heap, the High of
[SG-REVIEW](SG-REVIEW.md) (finding 1) and the last part of
[FORK-REVIEW-LIBC](FORK-REVIEW-LIBC.md)'s finding 1. The layer
(`user/posix/sys`) and libjam's objects in libc.a allocated with the
program's `malloc`: a program whose `malloc` is its own (PartitionAlloc)
and calls `mmap` with its lock held, or a handler that interrupts it and
calls `open`, could wait for that lock for ever.

**The heap** (`user/posix/sys/heap.c`): `jam_malloc`, `jam_calloc`,
`jam_free` (`<os.h>`: libjam's names, kept unrenamed in libc.a by
`tools/posixnames.py`), `jam_realloc` and `jam_strdup` (`layer.h`). Not
musl's internal allocator: in a program that keeps musl's `malloc`,
`__libc_malloc` *is* the program's, so a handler interrupting it would
still wait. Blocks up to 16 KiB come from size classes (16-byte steps to
64, then four to each doubling) cut from 64 KiB runs of one arena, a 1 GiB
VMO of address space mapped at the first allocation, pages committed on
touch (libos's heap's way); freed blocks go on their class's list and a
class's runs stay its own. A bigger block is a span of whole runs, given
back by its free to a list of free spans (by address, merged with free
neighbours). So the heap is one mapping, made once: the first build gave
each big block a VMO of its own, and posixtest's scm and peer cases, which
count the job's handles across calls, saw one more (a socket's lazily
allocated buffer: batch 18145113, both tries; init too, through peer).
Only once the arena is full is a block a VMO of its own. One count moved
for a reason of the program's own: scm's `counts` case now runs a round
before its first count, since that round's `malloc` grew posixtest's own
allocator by one unit for good (seen with counts between each step, batch
18152308: not the layer's heap, which made no kernel object; most likely
the break's own handle), which the layer's allocations used to grow at
start-up. No VMO handle is
kept (a forked child's copy is a VMO it has no handle to), so nothing
decommits: pages a big block touched stay committed for the next. Neither
is in libc's table of mappings, so the program's `munmap` never reaches
them. A host harness (scratch: heap.c with stub headers, 400,000 random
`jam_malloc`, `jam_calloc`, `jam_realloc` and `jam_free` of 0 bytes to 8
MiB under AddressSanitizer, every block's bytes checked) passed. In a native program the `jam_` names are libos's heap (`heap.c`'s
aliases). Every allocation of the layer and of libjam was converted (312
calls in 53 files, and the layer's six `strdup`s), with dr_mp3's default
allocators; posixtest's one free of a layer block (`fileskid.c`) too.

**Its rules.** The lock is a leaf (only the kernel's VMO and address-space
calls under it). It is held only inside a quiet stretch
(`sig_quiet_begin`): a raise there waits for its end, so no handler runs on
a thread holding it; a fault there is the kernel's kill (`sig_fault_begin`
asks `heap_holder`, as for `sig_lock`). fork's prepare takes it last, for
every way in (musl's `fork`, `_Fork`, a raw `SYS_fork`, `vfork`, both
clones all reach `fork_core`), gives it back as soon as the clone is made
(`heap_back`: the parent's walk may allocate), and the child makes it
fresh before anything else. vfork and clone still take musl's `malloc`
lock as FR2 made them (for the child's own `malloc`: posixtest's `alloc`).

**Proved by make check**: `tools/checklayerheap.py` reads `libmusl.a`'s
members and refuses any layer or libjam object (before the renaming) that
names an allocator function or one of musl's that reaches one through
other members (it found the six `strdup`s and dr_mp3's defaults; it checks
itself by finding `strdup` and `fopen`; `__malloc_atfork`, which only
locks, is allowed). The libjam link test's stub has no `malloc`, `calloc`
or `free` any more.

**Tests** (TESTING.md: `posix-heap.txt`, posix-mem, posix-fork): bin/heaptest
(`user/tests/heaptest`), whose own `malloc` takes its lock and then calls
`mmap`, `munmap`, `open`, `read` and `close` (`own`), and runs a `SIGUSR1`
handler's `open` and `read` of `/proc/self/status` while busy inside it
(`handler`); posixtest's mem section runs both. posixtest fork's `heap`: 30
children each way beside two threads in `jam_malloc`. Without the heap (a
throwaway build whose `jam_*` called `malloc` and whose `heap_fork` did
nothing, batch 18143007) all three failed: `own` saw 2105 calls of `malloc`
from inside `malloc`, `handler` 5, and the fork case hung; with the heap
and no `heap_fork` (18143427) the fork case hung too. heaptest costs the
boot image 0.5 MB (every POSIX program here is about that size).

**headless_shell** (the main session's scratch smoke, never committed): relinked
against this libc (`tools/cr/build.sh start headless_shell`, 152 s;
`jam_malloc` and `heap_fork` are in the program), it **still hangs at
start-up** exactly as before: 2 threads, 0:00.00 CPU, 544 KiB (its job 136
pages) at 45 s and at 65 s, no output at `--v=1`. So the layer heap was not
that hang's cause (TD found it: the sandbox's `localtime` interceptor's
`dlsym(RTLD_NEXT)` came back NULL and its log re-entered a `pthread_once`;
fixed on main by 4d756b99).

**Left.** `StackTraceDeathTest` (base_unittests) not run here (IC had
base): the next relink of base_unittests runs it with this libc. musl's
`posix_spawn` override still allocates its `ops` with `malloc` (musl's
side, not a call; `posix_spawn` isn't async-signal-safe), and glibc's
`backtrace_symbols` with `malloc` (its caller frees). The arena is 1 GiB of
address space in every POSIX process that allocates (pages only as used).

#### PR notes (2026-10-18)

Track PR (branch `worktree-agent-a8eaf264df3efe70d`; the main session's D12):
other processes' `/proc/<pid>/...` for Chromium's browser, which reads its
children's `stat`, `statm` and `fd` (CB5).

**What was missing.** Less than CCS's list said. PRC and CBF4 already
answer a *child's* `stat`, `statm`, `status`, `maps` and `smaps_rollup`
(from the child's process handle: `process_get_info` and
`process_get_maps`), and the base run's "ENOSYS" lines after those two tests'
`stat` were the end of each test, after the child was reaped (ENOENT, as it
should be; the log notes every name `/proc` hasn't). Only `fd` was missing
(`ProcessMetricsTest.GetChildOpenFdCount`: `GetOpenFdCount()` counts the
entries of `/proc/<pid>/fd`, its first answer was -1).

**How the layer reaches another process: its children, through what
`fork` and `posix_spawn` keep.** Each child's process handle (kernel facts:
state, CPU, threads, memory) and the client end of its signal channel
(`signal.idl`: kill's way to it, and its notices to us). Nothing wider is
built, and the answer for any other pid stays ENOENT: a process can reach
only what it started (the capability model), and the kernel offers no call
that turns a pid into a handle. **Recommendation for wider reach** (not
built, nothing needs it; `ps` and `top` ports would): a kernel call that
lists the processes of a job the caller holds (`job_get_processes`: koids
and pids) and one that opens one of them for inspection with the job's
`RIGHT_INSPECT`; `/proc/<pid>` then answers for every process under the
caller's own job (a grandchild, a sibling it spawned through the same job),
the facts from the process handle as for a child. The fd listing needs a
signal channel, so it stays the starter's.

**The descriptor table lives in the child's own layer, so the child is
asked.** The smallest sound way was a question on the channel the parent
already holds; a kernel handle listing doesn't answer it (a descriptor is
not a handle: a file is a channel, a pipe is half of a pair, an epoll set a
port; the count would be wrong). Two events in `signal.idl` (commit
d87ed347): `list_fds(seq)` from the starter and `fds(seq, bits)` back, the
open descriptors as a bitmap (bit n of the string is descriptor n; up to
128 bytes, trailing zero bytes left out). The child's signal thread
answers (proc.c's `lx_answer_fds`, over `fd_bitmap`: one lock take, no
reference on any description). The starter's side is proc.c's
`lx_child_fds`: it links a `fdwait` on its stack, writes the question, reads
the child's notices itself (so the answer doesn't wait for its signal
thread to be scheduled; either may take it, under proc.c's lock) and sleeps
on `gen` for at most 3 s. Questions have numbers, so threads can ask at
once, and an old or forged answer matches nothing. `NOTICE_BYTES` went
from 64 to 160 (the answer is up to 142). procfs.c: `/proc/<pid>/fd` of a
child opens (EACCES when the child has no signal protocol to ask on: init's
session, which is a `session_ctl`; ENOENT once reaped) and each `getdents`
asks once for its whole call (the position is the descriptor's number, as
for our own). A zombie has none open. Names below it
(`/proc/<child>/fd/N`: where each descriptor goes) are ENOENT: what a
descriptor is isn't asked; nothing reads it for another process.

**The stub.** A forked child's `execve` of a program from `/data` is
still spawn, wait and exit (FORK-PLAN section 2, way (a); FX is the later
fix), so the pid its parent holds is the waiting stub's, and the stub's
descriptors (a copy) are not the program's. The stub passes the question on
to the program over the program's signal channel (it is the program's
starter), number and all, and an answer from the program that none of
its own questions waits for goes on to its starter with the same number
(proc.c's `lx_answer_fds` and `on_fds`), as its signal thread forwards the
signals. So `GetOpenFdCount()` counts the program's descriptors, which is
what the test counts. The relay keeps nothing, so any number of questions
at once go through.

**`MeasureChildCpuUsage` stays on the list** ([launch], with the reason):
its busy child is the same kind of stub. `/proc/<pid>/stat` is the pid's
own CPU time, the waiting stub's (about none), and only the program that
the stub waits for burns. Passing it needs the pid to *be* the program:
**FX** (FORK-PLAN: init builds the `/data` program in a process the caller
may only `process_replace` itself with). Faking the figure (the stub's
`stat` carrying the program's CPU, or the program's facts under the stub's
pid) would make `ps` lie; my recommendation is FX before CB5's tests of
`/data` programs as children, and none of Chromium's own children meet this
(they are `/proc/self/exe`, exec'd in place).

**Programs linked before this change** don't answer `list_fds` (a program's
signal thread drops what it doesn't know): `getdents` on their
`/proc/<pid>/fd` ends with EIO after 3 s. Relink (as FL3's hand-over
version 2 already asks).

**Not built** (ports by need): `/proc/<child>/fd/N` (links), a child's
`cmdline`, `exe`, `task`, `limits` and `mem`. `GetOpenFdSoftLimit()` reads
`/proc/<pid>/limits` (of ours too; not built): Chrome's task manager and
`memory_details_linux.cc` use it, `content_shell` doesn't.

**Tests.** posixtest's procinfo section, `check_child_fds` (a child posixtest
that reports its descriptors by `fcntl` and its own `/proc/self/stat` and
`statm` on a pipe): our `/proc/<pid>/fd` is the same set at three moments
(started, 40 more opened, closed), one entry a `getdents` call, four
threads asking 25 times each at once, none for a zombie, ESRCH for a
directory opened before the reap, ENOENT for a name below it, for the
reaped child and for our parent's; its stat and statm against its own view
(same pid, name, parent, group, session, threads and start time; `S` to us,
`R` to itself; sizes within 1 MiB). `check_child` now expects `fd` in the
child's directory. posixtest's `spawn-data` (tools/posix-mmap-test.sh,
case `spawn`): a forked child's `execve` of `/data/pt-prog` gets the same
three-moment cycle, which only passes if the answer is the program's.

**base_unittests** relinked against this libc (the stage's two programs
replaced by the relink's, out/jamos: a relink does not refresh the stage).
`*MetricsTest*` with `CR_BASE_ALL=1` (the five suites, 20 tests): 19 pass,
`SystemMetricsTest.MeasureChildCpuUsage` fails as listed;
`ProcessMetricsTest.GetChildOpenFdCount` passes (it failed before with
`GetOpenFdCount()` = -1, `path /proc/28/fd`). `*Process*` (nine suites,
ProcessUtilTest and ProcessTest among them, 66 tests): 60 pass, 6 on the
skip list, 0 failures. `GetChildOpenFdCount` came off the skip list.

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
  - The files Chromium's `base` reads (PRC, built: "PRC notes"):
    `/proc/self/stat`, `status`, `statm` and `maps` (and a child's, for
    `/proc/<pid>/` of the processes we started), `/proc/cpuinfo`,
    `/proc/meminfo` and `/proc/stat`, in Linux's text and field order,
    from process_get_info, process_get_maps (the address space's
    mappings), the figures any process may read and CPUID. Fields Jam OS
    has no meaning for hold Linux's neutral value.
  - Everything else under `/proc` (`smaps`, `/proc/self/ns/user`, ...;
    `cmdline`, `task/`, `mem`, `/proc/sys/kernel/...` and `/proc/version`
    came with CBF4): **ENOENT, never a half-filled file.**
    Chromium's callers of these fail soft (the namespace check reads
    "no user namespaces"); the ENOSYS and ENOENT log (below) shows any
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
  writes get EPIPE. `SHUT_RD` and `SHUT_WR` alone were EOPNOTSUPP here;
  HS built them ("HS notes").
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
- Checked again 2026-10-18 (before CB5) against the row's tests: every
  one is in posixtest scm (`scm.c`, `scmcred.c`, `scmkid.c`) but the
  eventfd's, refused as above. Nothing CB5 runs passes one: Mojo's
  eventfd path (`channel_linux.cc`'s shared-memory upgrade) is behind
  `kMojoUseEventFd`, off by default on Linux in 155.0.8059.39.

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

**Superseded by the owner (2026-10-16)**: "Build the fork/vfork emulation
layer in libc first and use it for Chromium's launcher instead of
patching it." Neither (A) nor (B): libc gets a real copy-on-write `fork`
and an `execve` that replaces the image in place, and Chromium's launcher
runs unpatched. The plan is [FORK-PLAN](FORK-PLAN.md); CS1's additions
below stay (they serve `posix_spawn` and exec alike).

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
  Writable shared mappings (`base::MemoryMappedFile` `READ_WRITE`: the
  persistent histograms' and metrics files, some databases; CB2's
  finding) come on ext only, with `msync` the durability promise (the
  owner, 2026-10-16): [WRITEMAP-PLAN.md](WRITEMAP-PLAN.md). FAT keeps
  ENODEV.
  ICU's data and V8's snapshot are compiled in (`icu_use_data_file=false`,
  `v8_use_external_startup_data=false`), so fewer files are mapped.
- **`inotify`** (file watchers): ENOSYS, and Chromium's watchers that
  need it fail soft (*unsure*: CB2's run with the log on lists any that
  doesn't).
- **`/tmp`**: always there in shell mode and empty at boot (M13.6's U7b:
  `/jam/tmp`, an ext `/data`'s `/data/tmp`, or an ext volume in memory),
  and every POSIX program's list grants it writable (the word `posix`);
  `XDG_RUNTIME_DIR` stays unset (Chromium warns).

### 8. The network

- **netstack's limits (CN1)**: sockets an opener 16 to **256**, sockets
  in all 48 to **512**, TCP connections an opener 64 to **512** and in
  all 256 to **1,024**, each with netstack's memory measured at the new
  caps (its heap, rings and lwIP's pools; BENCH.md gets the figure). The
  totals still bound netstack's memory; one program may take half.
  General: every program gets the higher caps. Built: ordinary programs
  together may hold 384 sockets and 768 connections (the rest is the
  network services' reserve); see the CN1 notes and BENCH.md's figures.
- **Names**: `getaddrinfo` on worker threads to `/svc/dns` (A records:
  IPv6 is off anyway), through Alpine's `disable-dns_config_service.patch`
  (Fuchsia's stub configuration service), so Chromium's own resolver and
  DNS-over-HTTPS stay off.
- **TLS**: BoringSSL with Chromium's compiled-in root store: nothing from
  Jam OS. **Netlink**: `socket(AF_NETLINK)` is EAFNOSUPPORT; Chromium's
  network change notifier then assumes one connection that never changes
  (CB3 checked: one log line a run, no retry loop).

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
  would have; a later ALSA or PulseAudio shim could). **Built
  (2026-10-15)** as additions to `<audio.h>` (the M12 output library,
  which already had the first half): see the [CA1 notes](#ca1-notes).
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
- **The power** (CJ1, CR-e): a new root-resource right, `RIGHT_ROOT_JIT`
  (the next free bit, 27), given when a program's list says `right jit`
  (`WANT_RIGHT_JIT` beside `klog`, `sysinfo`, `clock` and `debug` in
  `user/include/wants.h`). Since CR-e (CJR's design question 2, its option
  (b)) it comes alone, never on the program's root: a startup role of its
  own, `SR_JIT` (`user/include/roles.h`), a duplicate of the starter's
  root narrowed to `RIGHT_ROOT_JIT`, `RIGHT_DUPLICATE` and
  `RIGHT_TRANSFER` (`DP_JIT_RIGHTS`, `<dataprog.h>`), so its holder may
  hand it on, and nothing else of its root goes with it.
- **JIT VMOs** (CJ1): a new call, `vmo_make_jit(vmo, root, *out)`, in
  `vmo_make_exec`'s shape: a plain paged VMO holding nothing (one handle,
  no page, no mapping, no pin; never a pager VMO, a pager VMO's child or a
  kept one) becomes a JIT VMO, and `*out` is the same handle with
  `RIGHT_EXEC` added to a fresh VMO's rights (read, write, map, resize,
  duplicate and transfer: a duplicate can only narrow rights, so the
  handle must carry transfer for its creator to hand one on, CR1 finding
  4). Needs `RIGHT_ROOT_JIT` on root. A JIT VMO can never be
  sealed, verified, made executable the old way, or given a pager, and is
  never pinned for a device (`ERR_NOT_SUPPORTED`, a pager VMO's answer;
  CJF, CJR's design question 1): JIT memory is written only by threads,
  so the stricter modes below can see and switch every writer, and a
  decommit of code pages never leaves a pinned page behind.
- **W+X only there** (CJ1): `perms_ok` in `kernel/mm/aspace.c` allows
  write together with execute only for a mapping of a JIT VMO, and only
  as far as the handle's rights allow (`ASPACE_CAN_*` from the rights:
  a handle without write maps execute-only). `vmar_protect` follows the
  same rule. Everything else stays W^X.
- **Marked** (CJ1, CJ2): `process_get_info` and `proc_list` give
  `PROCESS_FLAG_JIT` (the address space has mapped a JIT VMO executable:
  a map or a protect with execute, CJF; one that only reads or writes it,
  such as JIT memory handed on as shared memory, isn't marked); `ps` puts
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
  power from the list alone, as `SR_JIT` (the shell, `/svc/exec`, init's
  apps), and init's test run gives it only to an init.cfg program whose
  list asks (utest; posixtest and usbtest keep W^X whole). `pkg`
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
  power (`SR_JIT`) maps a JIT VMO made for it (`vmo_make_jit`); then `PROT_EXEC` is
  allowed on it in `mmap` and `mprotect`, alone or with `PROT_WRITE`.
  Without the power `MAP_JIT` is `EPERM`, and execute anywhere else stays
  `EACCES`. The switching the plan asks for is `mprotect`: a program may
  keep the pages writable and executable (V8's default on x64) or move
  them between `PROT_READ | PROT_WRITE` and `PROT_READ | PROT_EXEC`
  (macOS's per-thread `pthread_jit_write_protect_np` is for arm64 and has
  no x86 use until the stricter modes below). A `MAP_FIXED` of private
  anonymous memory over JIT memory, with `MAP_JIT` or without it (CR-e:
  Linux's decommit by remap, which V8's WebAssembly code space does through
  PartitionAlloc), decommits in place (a reservation cut and committed again
  keeps one VMO) and stays JIT memory; anything else mapped over it
  replaces it. `prctl(PR_SET_MDWE, PR_MDWE_REFUSE_EXEC_GAIN)` (Linux's
  memory-deny-write-execute) gives the power up for good: libc closes
  every `SR_JIT` handle, under the table's lock (CJR2 finding 4);
  `PR_GET_MDWE` says it is in force exactly when the process holds no
  power. A forked child gets a duplicate
  of `SR_JIT` (libjam's fork), and an exec of the same program
  (`/proc/self/exe`) keeps it; posix_spawn and an exec of any other program
  pass none. Unlike Linux's, it doesn't reach a program `/svc/exec` starts
  (a caller whose list says `svc exec`): that one gets its own approval,
  the power with it if its list asks (CJR2 finding 3; Chromium's list has
  no `svc exec`). `MAP_JIT` is in `<sys/mman.h>` through an override of musl's
  `arch/x86_64/bits/mman.h`, and an override of its `src/mman/mmap.c`
  keeps the `EPERM` (upstream turns an anonymous mapping's `EPERM` into
  `ENOMEM`, a fix-up for Linux) (user/posix/musl/OVERRIDES).
- **V8** (CR-e, built): V8 already tells its page allocator when a
  reservation will hold code (`kNoAccessWillJitLater`, made for macOS's
  `MAP_JIT`: its code range and its WebAssembly code space). In Chromium
  that allocator is gin's, over PartitionAlloc's, which passes `MAP_JIT`
  on macOS; ports/chromium's patch 0913 passes it on Jam OS too (no Linux
  path says which reservation holds code), and for memory asked
  executable from the start (`kReadExecute`, `kReadWriteExecute`). Its decommit of code pages, a
  `MAP_FIXED` remap without `MAP_JIT`, needs no patch: libc keeps that JIT
  memory (above).
- **Only renderers** (CR-e, built: FORK-PLAN Q4's (A)): the browser holds
  the power (content_shell's list says `right jit`) and keeps it to hand
  on; every child it forks keeps `SR_JIT` across its exec of
  `/proc/self/exe`; patch 0914 has every process type but the renderer
  give it up at its start (`PR_SET_MDWE`, in content's
  `ContentMainRunnerImpl::Initialize`), and gin run V8 `--jitless` unless
  the process is a renderer still holding it. So `ps` marks the renderers
  alone `[jit]`. A renderer whose V8 is told `--jitless` (`--js-flags`:
  the browser's for a site that may not JIT, or the owner's) gives it up
  too (CJR2 finding 1). The browser holds it and could use it (CJR2
  finding 2): only a launcher outside the browser (CR-f's) can take that
  away.
- **And SwiftShader's GPU process** (CR-e, built, the main session's
  revision of the rule above): WebGL in software is SwiftShader (patch
  0915: linked in as ANGLE's Vulkan), whose Reactor compiles each shader
  and draw state to code in the GPU process, in `MAP_JIT` memory. A GPU
  process started with `--use-angle=swiftshader` or `swiftshader-webgl`
  (what the browser gives it for `--enable-unsafe-swiftshader`, Chromium's
  own opt-in to software WebGL) keeps the power; every other GPU process
  gives it up as before, and V8 stays `--jitless` there. `ps` then marks
  the GPU process too. Shaders are a web page's code compiled in a process
  with the power, as V8's are in a renderer: one more reason for CR-f's
  sandbox.
- **Stricter modes later** through the same power: a per-thread switch
  with Intel's protection keys (the PC has PKU; the kernel would enable
  PKRU in XCR0 and give `pkey_alloc`/`pkey_mprotect`), or dual mapping
  (a write-only view and an execute-only view of the same VMO, which the
  rights already allow) with a V8 patch.
- **A security review of its own** (CJR) before CR-e is used on the PC:
  done 2026-10-16 for CJ1 and CJ2's first half
  ([CR-REVIEW-JIT.md](CR-REVIEW-JIT.md): no program without the power gets
  W+X memory, no JIT memory becomes approved code; 3 Low, one fixed; pins
  and handing the power to renderers are design questions). V8's patch and
  the launcher (CB5) get a look of their own when they land: CR-e's
  ("CR-e notes") are ready for it.

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
the recipe's lint): the musl ones: `no-execinfo.patch` (left out again
since BT: libc has `<execinfo.h>`), `no-mallinfo.patch`, `no-res-ninit-nclose.patch`,
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
2. Dropped: the `posix_spawn` launcher branch (section 5). The owner
   (2026-10-16): Chromium's own launcher runs unpatched on libc's `fork`
   ([FORK-PLAN](FORK-PLAN.md)).
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
| M13 | **F1b** (read-only shared file mappings; `posix_spawn` of programs on file systems); the futex error pass-through (done, 39aec118); R2 and J as planned (not gates); M13.5's `fork`, `vfork` and an `exec` in place ([FORK-PLAN](FORK-PLAN.md), for CB5) | M13.5's shell (mksh) |
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
| **CR-e. The JIT** (built 2026-10-19: [CR-e notes](#cr-e-notes)) | renderers run V8 with its compilers and WebAssembly under `right jit`; JetStream and Speedometer scores against `--jitless` in BENCH.md; every other process still `--jitless`; CJR's findings fixed | ktests (CJ1), posixtest (`MAP_JIT`), cr-test.sh `chrome` with a Wasm page | CJ1, CJ2, CJR |
| **CR-f. Jam OS's sandbox** | each renderer in its own job with a narrowed namespace; a test renderer that tries to open `/jam`, connect out or reach the compositor fails each; a renderer that eats memory hits its job's limit, not the system's | its own plan | CR-d |
| **Later** | the GPU (G3, G4: ANGLE and Vulkan, then WebGL on it; WebGL in software, SwiftShader, since CR-e), video beyond the open codecs (never H.264 in the open build), CJK fonts, input methods, drag and drop | | |

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
branch in Chromium's launcher, not an emulated `vfork` (section 5;
superseded by the owner on 2026-10-16: Chromium's own launcher on libc's
`fork`, [FORK-PLAN](FORK-PLAN.md)); the first builds have no sound, then a mixer back end (section 11); the JIT
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
| **CL2. `/proc/self` and the self-image** | section 3's `/proc` and section 6's self-image: the program's path in the hand-over and libjam's spawn, `SR_SELF_IMAGE` from every starter, `/proc/self/exe` (readlink, and spawn from the image), `/proc/self/fd` (list, readlink, reopen with the read-only twin), ENOENT for the rest; built, see "CL2 notes" | a new user/posix/sys/procfs.c, the path lines in `user/posix/sys/path.c`, `user/posix/sys/spawn.c` (the self branch), `user/include/posixfds.h`, `user/include/roles.h`, `user/lib/spawn.c` (the path and the role), the shell's and init's start of programs (the role, a line each) | nothing for `/proc`; the self-image of a `/data` program after M12.5's PX | posixtest: readlink of `/proc/self/exe` from a boot-image program and a `/data` one; a spawn of `/proc/self/exe` (shared pages: the child adds no text pages to the job); a memfd reopened read-only can't be mapped writable; the rest of `/proc` ENOENT (PRC's files: "PRC notes") | **Opus** |
| **PRC. `/proc`'s files for `base`** (built 2026-10-16) | section 3's files `base` reads, found missing by CB2's first `base_unittests` run: `/proc/self/stat`, `status`, `statm`, `maps`, `/proc/<pid>/stat` of our children, `/proc/cpuinfo`, `/proc/meminfo`, `/proc/stat`; built, see "PRC notes" | `user/posix/sys/procfs.c`, new `user/posix/sys/procinfo.c`, `procinfo.h`, `cpuinfo.c`; small hooks in `proc.c`, `mem.c`, `time.c`, `misc.c`, `kposix.c`, `layer.h`; kernel: `process_get_maps` (138) in `abi/syscalls.def`, `kernel/abi/sysc_proc.c`, `kernel/mm/aspace.c`, `process_info` (creation time, name) and `sys_public_info` (free pages) in `kernel/include/jam/abi.h` | CL2 | posixtest `procinfo`; utest `process_maps`; ktest `aspace_maps_shapes`; tools/shell-tests/posix-procinfo.txt | **Opus** (a kernel call) |
| **CL3. Raw calls and the ENOSYS log** | section 3's log and calls: `prctl` names, affinity, thread priorities, futex operations checked, `membarrier` | `user/posix/sys/calls.def` and the handlers' files it touches (`user/posix/sys/misc.c`, `user/posix/sys/thread.c`), a new log file in user/posix/sys/ | nothing | posixtest: each call's cases; the log names an unknown call once | Sonnet 5.5 |
| **CU1. Descriptor passing on socketpairs** (built 2026-10-14: section 4's "CU1 as built") | section 4: the framing, every hand-over kind on GS1's tables, more than 64 descriptors, `MSG_CTRUNC`, `MSG_CMSG_CLOEXEC`, `shutdown(SHUT_RDWR)`, seals travelling and the write-seal rule | the translation file GS1 made (widened), `user/posix/sys/pipe.c` (the socketpair's send and receive), `user/posix/sys/sockio.c`, `user/posix/sys/sockconn.c` (shutdown), `user/posix/sys/memfd.c` (the seal rule), posixtest's socketpair section | G2's GS1 merged | posixtest: 1 byte with 64 descriptors, 128, 254 (EINVAL); every kind passed and used on the other side (a pipe written, a memfd mapped, a file read at its offset, an eventfd signalled); the read-only twin passed on stays read-only; too little room; a peer gone mid-send; Mojo's pattern (non-blocking, partial writes, descriptors with the first byte) between two processes; a fuzz loop of mutated headers (no crash, no handle leaked) | **Opus** |
| **CU2. Named `AF_UNIX` sockets** | section 4's broker, `/svc/unix`: `bind`, `listen`, `accept`, `connect` on socket files and abstract names | the broker (new, user/services/), its IDL (new), `user/posix/sys/socket.c`'s `AF_UNIX` paths, init's start of it | CU1; M13.6's U4 (socket files on `/jam`) | posixtest: a listener and two connectors, a socket file removed, a connector without the file's view refused, abstract names; utest for the broker's limits | **Opus** |
| **CE1. The exec sealer** | `/svc/exec` in init: a program `posix_spawn`s an approved `/data` program (its own open of the file; init checks the allow line and page table, seals the file's pages with `vmo_make_exec_verified`) and the child gets its own approval only, never its parent's (the owner's decision, 2026-10-12); built, see "CE1 notes" | `abi/idl/exec.idl` (new), `user/services/init/exec.c` (new), `user/include/dataprog.h` + `user/lib/dataprog.c` (PX's helpers moved from the shell), `user/posix/sys/spawn.c` (`file_program`), `user/posix/sys/proc.c` (a child's job for SIGKILL), `user/lib/spawn.c` (`before_start`), utest `execsvc.c` with `user/tests/exectest` and `user/tests/execwide` | M12.5's PX, M13's F1b, R2 | utest `exec_*`; `tools/posix-mmap-test.sh` case `spawn` (the allowed run starts and exits 7); `allow.txt` and pager-test `exec` unchanged | **Opus** (a trust boundary) |
| **CS1. Launching** | section 5: `addclosefrom_np`, argv and environments in a VMO, `/proc/self/exe` spawns through CL2's image, a child's argv of 64 KiB; built, see "CS1 notes" | `user/posix/sys/spawn.c`, `user/posix/sys/spawnfd.c`, `user/lib/spawn.c` (the VMO), the POSIX start code (`user/posix/crt/`) | CL2's self-image part (or beside it, merged after) | posixtest: a child with 500 arguments and 64 KiB of environment sees them all; `addclosefrom_np` leaves exactly the remapped descriptors (checked through `/proc/self/fd`) | **Opus** |
| **BT. Stack traces** | libc's `<execinfo.h>` (`backtrace`, `backtrace_symbols`, `backtrace_symbols_fd`) by the program's own unwind tables, so Chromium's and V8's `StackTrace` take their glibc path and Alpine's no-execinfo patch and our 0911 are gone; built, see "BT notes" | `user/posix/glibc/` (new), `user/posix/include/execinfo.h`, `user/posix/posix.mk` (unwind tables in musl, the layer and libjam), posixtest's execinfo section, ports/chromium's patches | nothing | posixtest `execinfo`: nested frames, a thread, a signal handler, qsort's comparator, the lines | Sonnet 5.5 |
| **CP1. Programs this big** | section 6's limits: `VMO_VERIFIED_MAX` 1 GiB, `PROGRAM_MAX`, `FILE_MAX`; `allow`'s progress line; a 300 MB generated test program; built, see "CP1 notes" | `kernel/include/jam/abi.h` (the constant), `kernel/object/vmo_verify.c` if a size is assumed there, `user/services/shell/sh_allow.c` (the constant and the line), `user/lib/spawn.c` (the constant), a generated test program (new) | M12.5's PX merged (it rewrites `sh_allow.c`) | ktest: a 1 GiB verified table; the shell's allow script with the 300 MB program (allowed, run, a changed page refused); cr-test.sh `hello` with the big program | **Opus** (small) |
| **CN1. netstack's caps** | section 8: the new caps, netstack's memory measured at them; built, see "CN1 notes" | `user/include/net.h`, `user/services/netstack/stack.h` and lwIP's options file if pools must grow, BENCH.md's line | nothing | utest: 256 sockets from one opener, the 257th refused; 512 in all; 1,024 TCP connections to the test host; `tools/net-test.sh` unchanged | Sonnet 5.5 |
| **CB1. The Chromium build** | section 14 and 15: ports/chromium (the recipe, Alpine's patches, our toolchain patch, args for `base_unittests` and `headless_shell`), the builder's kept tree and `package --from` in tools/ports.py, the persistent tree on WSL behind its lock; builds `base_unittests` and `headless_shell`; reports disk, peak memory and time | ports/chromium (new), tools/ports.py (the two options), tools/cr/ (the build wrapper, new) | CT1, CT2; the main session's CPU-weighted slots in `tools/remote-test.sh` | the build finishes; the syscall scan passes (or lists each site); `make check` (the recipe's lint) | **Opus** (the build glue decides much) |
| **CB2. `base_unittests` (CR-b)** | cr-test.sh's `base` case; the run with the ENOSYS log on; every failure triaged to a layer (a finding for its track) or fixed in the recipe's test list with a reason | cr-test.sh's case, ports/chromium's test list | CB1, CL1 (stage 1 at least), CL3, CP1 | cr-test.sh `base` | Sonnet 5.5 |
| **CB3. `headless_shell` (CR-c1)** | the single-process headless screenshot of a local page and an HTTPS page; the findings; built 2026-10-18, see "CB3 notes" | cr-test.sh's `headless` case, the recipe's args | CB2; CL1 stage 2 (V8's cage); M13's F1b (the `.pak` maps) | cr-test.sh `headless` | Sonnet 5.5 |
| **CB4. `content_shell` on Wayland (CR-c2)** | `content_shell --single-process` on the desktop; keys, mouse, wheel, a `<select>` list; screenshots; built 2026-10-18, see "CB4 notes" | cr-test.sh's `shell-wl` case | CB3; G2's GS1, GC1b, GT (GC3 for the list), GP2 | cr-test.sh `shell-wl` | Sonnet 5.5 |
| **CB5. Multi-process (CR-c3)** (built 2026-10-19, see "CB5 notes") | Chromium's own launcher, unpatched, on libc's `fork` and `exec` in place ([FORK-PLAN](FORK-PLAN.md)); `--no-zygote --no-sandbox`, every process type started and talking over Mojo; a renderer killed | cr-test.sh's `multi` case | CB4; CU1, CS1, CL2, CN1; M12.5's PX; FORK-PLAN's FK1 to FK3 and FL1a to FL4 | cr-test.sh `multi` | **Opus** (processes cross) |
| **RL. Record locks across processes** (D9; built 2026-10-18: [RL notes](#rl-notes)) | `fcntl`'s, `F_OFD_` and `flock` locks kept by the file service per inode (`file.lock`, `getlock`, `lockowner`), so SQLite and leveldb in Chrome's processes meet each other's; `EDEADLK`; released at close and at a process's death; kept across extfs's restarts | `abi/idl/file.idl` (its own commit), `user/include/rlock.h` and `user/lib/rlock.c` (new), `user/services/extfs/locks.c` (new) and its lines in extfs's `main.c`, `files.c`, `adopt.c`, `request.c`, `state.c`, `keeper.c`, `extfs.h`; libc's `reclock.c` (new) and its lines in `dup.c`, `fd.c`, `file.c`, `files.h`, `kinds.h` | IE2 (extfs's queue) | posixtest `locks`; utest `extfs_locks`, `extfs_restart_locks`, `fs_no_locks`; `tools/extfs-svc-test.sh`; base's `FileLockingTest` (7) after a relink | **Opus** |
| **CB6. `chrome` (CR-d)** | the `chrome` target, its args, its package (programs, paks, licences, initial preferences: Q4), the profile on `/jam`, the singleton on CU2; `pkg install chromium` | ports/chromium (the target's lines), cr-test.sh's `chrome` case | CB5; GC2, GC3, CU2, CA1 (sound), M13.6 (its tracks in section 16), M13.7's `pkg` and PO13 | cr-test.sh `chrome`; the PC check | Sonnet 5.5 |
| **PO13. The package area on `/jam`** (built: [M13.7's PO13 notes](M13.7-PLAN.md#po13-notes)) | M13.7's prefix on `/jam` (after M13.6), the guard on it, big packages (a 250 MB program: staged files on `/jam`, init's hashing in steps) | M13.7's files named in its plan's PO5 and PO6 notes (the prefix constant, the guard's line for extfs) | M13.7's PO5 and PO6; M13.6's U4 and U8 | tools/pkg-test.sh (PO8, to come) with a big package on an ext test disk | Sonnet 5.5 |
| **CA1. Sound** | section 11: libjam's audio output API and Chromium's back end patch | `<audio.h>` (a header of its own), user/lib/audio.c and audio_flow.c, ports/chromium's audio patch | CB4 (a browser to test it with); the API part at once (**done 2026-10-15**, see "CA1 notes") | utest: a stream's frames reach the mixer (`tools/mixer-test.sh`'s way); cr-test.sh `chrome` playing a WebM's sound into the mixer's capture | Sonnet 5.5 |
| **CJ1. JIT memory in the kernel** | section 12's kernel part: `RIGHT_ROOT_JIT`, `vmo_make_jit`, W+X only on a JIT VMO's mapping within its handle's rights, the flag in `process_get_info` | `kernel/object/vmo.c` (the kind), `kernel/abi/vmo_sys.c` (the call), `kernel/mm/aspace.c` (`perms_ok` and protect), `abi/syscalls.def`, `kernel/include/jam/abi.h`, new ktests | CK2 merged (`aspace.c`); CK1 merged (`vmo.c`) | ktests: W+X refused on every other VMO; a JIT VMO without the power refused; an execute-only duplicate can't write; a write-only one can't run; a JIT VMO can't be sealed, verified or paged; full ktest 4 and 8; soak 2 | **Opus** |
| **CJ2. JIT for programs** (built 2026-10-19: its Chromium half with CR-e, [CR-e notes](#cr-e-notes)) | section 12's rest: `right jit` (wants, checkwants, allow's and pkg's words, init's slice), `ps`'s mark, libc's `MAP_JIT` (this half done 2026-10-14, see "CJ2 notes"); **still to do, after CB5**: V8's patch, the power to renderers only (with an unpatched launcher: [FORK-PLAN](FORK-PLAN.md#q4-the-jit-power-for-renderers-only-with-chromiums-own-launcher)'s Q4), `--jitless` for the rest | `user/include/wants.h`, the wants parser in libos, `tools/checkwants.py`, `user/services/shell/sh_allow.c`'s prompt, init's grant, `user/posix/sys/mem.c` (`MAP_JIT`), ports/chromium's V8 lines (and Q4's lines in content's child start, if so answered) | CJ1; CB5 | posixtest: `MAP_JIT` with and without the power; a JIT program writes code and runs it; cr-test.sh `chrome` with a Wasm page (CR-e) | **Opus** |
| **CR1. Kernel review** | CK1, CK2, CK3, CP1's kernel line, CJ1 if merged: findings first (docs/CR-REVIEW-KERNEL.md, new), then High and Medium fixed with a test each; hostile tests (a TiB of reservations from a job with 16 MiB, 60,000 mappings in a storm of protects from 8 threads while another unmaps); **done 2026-10-15** ([CR-REVIEW-KERNEL.md](CR-REVIEW-KERNEL.md): 0 High, 3 Medium, 3 Low, 4 notes; two Mediums and every Low fixed, one Medium a design question) | the files its fixes need | CK1 to CK3 merged | its own; full ktest 4 and 8; soak 4 | **Opus** |
| **CR2. POSIX and IPC review** | CL1, CL2, CL3, CU1, CU2, CS1: the descriptor passing (what crosses, what leaks, what a hostile peer can make the receiver do), the self-image (can it run anything the owner didn't approve?), `/proc`'s reopen; findings in [CR-REVIEW-POSIX](CR-REVIEW-POSIX.md) | the files its fixes need | CL1, CL2, CU1, CS1 merged | its own; posixtest; a fuzz loop between two processes | **Opus** |
| **CJR. JIT review** | CJ1 and CJ2 against the owner's rule: can any program without the power get W+X, can a JIT VMO become approved code, can a handle hand-on widen rights; findings first, fixes with tests; **done 2026-10-16** for CJ1 and CJ2's first half ([CR-REVIEW-JIT.md](CR-REVIEW-JIT.md): 0 High, 0 Medium, 3 Low, 4 notes; finding 1 fixed, two design questions); **CJF** (2026-10-16) fixed findings 2 and 3 and took design question 1's (b), JIT VMOs never pinned; **CJR2** (2026-10-19) read CR-e's half and SWS's (its part "CJR2": 0 High, 1 Medium, 4 Low, 4 notes; the Medium, a renderer told `--jitless` keeping the power, fixed in patch 0914, and Low 4, `PR_SET_MDWE` missing a second handle, in libos; two design questions) | the files its fixes need | CJ1, CJ2 merged | its own | **Opus** |
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
after CB3 and G2's pieces; **CB5** after CB4, CU1, CS1, PX and
[FORK-PLAN](FORK-PLAN.md)'s libc `fork` and `exec` (its own order); **CR2**
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
6. **Signals and faults**: V8's WebAssembly trap handler, its CPU
   profiler, Chromium's sampling profiler and its crash stack dumps need a
   fault delivered to the faulting thread and a signal to a chosen thread
   wherever it is; M13's signal thread does neither. The owner's rule
   (2026-10-16): build that layer, don't turn the features off.
   [SIGNALS-PLAN.md](SIGNALS-PLAN.md) does (tracks SG1 to SG6: an upcall
   into the faulting thread in the kernel, the rest in libc), independent
   of CB3 and CB4 and meant to land before CR-e. The workarounds are
   temporary: jitless (no Wasm traps) and no profiler until then, and
   Chromium's fault stack dumps don't run (SIGABRT's do); if CR-e comes
   first, the trap handler is off (`--no-wasm-trap-handler`: bounds checks
   in code instead) until SG3 and SG4 merge, and SG6 removes the flag.
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
- **Still to do**: `pkg`'s words when PO5 and PO6 land (section 12). The
  Chromium half (V8's `MAP_JIT`, the power to renderers only, `--jitless`
  for the rest, a Wasm page) is CR-e's: "CR-e notes", below.

## CR-e notes

Milestone CR-e, the JIT (2026-10-19): CJ2's Chromium half and the power as
a handle of its own; section 12 has the design as built.

- **The power alone** (CJR's design question 2, option (b); FORK-PLAN Q4
  (A)): `SR_JIT` (`user/include/roles.h`), a root narrowed to
  `DP_JIT_RIGHTS` (`RIGHT_ROOT_JIT`, `RIGHT_DUPLICATE`, `RIGHT_TRANSFER`),
  given by the shell's `run`, `/svc/exec`, init's apps and init's test run
  to a program whose list says `right jit`; `dp_root_rights` no longer puts
  `RIGHT_ROOT_JIT` on the root, so the handle is the only place the power
  is. libc's `MAP_JIT` takes it from there; libjam's fork gives a forked
  child a duplicate (`dup_roles`); libc's exec in place gives it to
  `/proc/self/exe` again, never to another program (posix_spawn gives
  none, as before).
- **Giving it up**: Linux's own call, `prctl(PR_SET_MDWE,
  PR_MDWE_REFUSE_EXEC_GAIN)` (memory-deny-write-execute, Linux 6.3), so
  the Chromium side is a call Linux knows. libc closes `SR_JIT` under its
  memory lock (`startup_handle_drop`); `PR_GET_MDWE` reads
  `PR_MDWE_REFUSE_EXEC_GAIN` exactly when the process holds no power (Jam
  OS's W^X is that rule for every process without it), clearing it is
  `EPERM` then, and `PR_MDWE_NO_INHERIT` is `EINVAL` (a child free of it
  can't be given the power back). JIT memory already mapped stays (Linux
  would refuse execute added to it): Chromium sets it before it has any.
  The table has no lock against another thread's fork at that moment, so
  the call is for a process's start, while it is one thread (Chromium's).
- **Decommit**: Linux programs decommit by mapping fresh anonymous memory
  over a range (`MAP_FIXED`, `PROT_NONE`): PartitionAlloc's
  `DecommitAndZeroSystemPages`, which gin's page allocator uses for V8's
  WebAssembly code space, which V8 makes executable again later. libc now
  decommits JIT memory in place for that too, with `MAP_JIT` or without
  (where the kernel refuses the protection, a forked child's execute-only
  copy, it is replaced by plain memory as before). A layer, not a patch.
- **Chromium's patches** (ports/chromium/patches, each hunk's reason in its
  header): 0913, PartitionAlloc's page allocator passes `MAP_JIT` for
  `kInaccessibleWillJitLater` on Jam OS as it does on macOS (V8's code
  range and WebAssembly code space ask so; no Linux path says which
  reservation holds code, and making every reservation JIT memory would
  also keep PartitionAlloc's pools out of a forked child); 0914, content's
  `ContentMainRunnerImpl::Initialize` has every process type but the
  renderer give the power up at its start, and gin runs V8 `--jitless`
  unless the process is a renderer still holding it (the browser keeps the
  power to hand on but never runs compiled code; a single-process renderer
  is jitless). content_shell's list says `right jit`.
- **Tests**: jittest's handed-on checks (a forked child keeps the power,
  `/proc/self/exe` exec'd keeps it, another program exec'd and a
  posix_spawn child get none, `PR_SET_MDWE` gives it up there and in what
  it starts after) and its decommit checks; utest `fork_jit` (the power in
  the child, used); posixtest's jit section (`PR_GET_MDWE`, `PR_SET_MDWE`
  without the power); cr-test.sh `jit` (content_shell loads a page whose
  hot loop and WebAssembly module are timed with the JIT and `--jitless`;
  `ps` marks the renderer alone `[jit]`) and `bench` (JetStream 2.2 and
  Speedometer 3.1 parts, both ways: [BENCH](BENCH.md)).
- **Measured** (cr-test `jit`, [BENCH](BENCH.md#chromiums-jit-cr-e-qemu-on-the-pc-2026-10-19)):
  the page's loop 6.0x faster than `--jitless` in one long call, 29x once
  TurboFan has compiled it; WebAssembly as fast as TurboFan's JavaScript;
  a WebAssembly out-of-bounds load is the `RuntimeError` it should be (V8's
  trap handler, SG's fault signals); a function called often reaches
  TurboFan in the background. `ps` marks the two renderers (the page's and
  a spare) `[jit]`, never the browser, the GPU process or the services.
  The long call stays at Maglev's speed with V8's compiles in the
  background (they take tens of milliseconds of wall time while the main
  thread loops, a fraction of one when it is idle), and reaches TurboFan's
  with them on the main thread (`--no-concurrent-recompilation
  --no-concurrent-osr`): V8's tiering as far as can be seen, an open
  question until the same build runs on Linux. The page is told it is
  visible but not focused (`document.hasFocus()` false), with no change
  when V8's efficiency mode is forced off.
- **base_unittests**: FileLockingTest (7) passes since RL and came off the
  skip list; FilePathWatcher* and ProcessMetrics* pass too (177 of 177 on
  ext, batch 19204004). base_unittests' list says `right jit` (it holds
  the power, as a renderer does), and with 0913's `MAP_JIT` for memory
  asked executable from the start, PartitionAllocPageAllocatorTest's three
  JIT cases (`AllocInaccessibleWillJitLater`, `AllocReadWriteExecute`,
  `ReadExecutePages`) pass and came off the skip list: the suite 15 of 15
  and its 3 self-skips (CFI, MTE), batch 19213223.
- **Benchmarks** (track SWS, batch 19205938, [BENCH](BENCH.md#jetstream-22-and-speedometer-31-parts)):
  JetStream 2.2's 14 JavaScript tests score 918.9 with the JIT and 127.4
  `--jitless` (7.2x: 22x on richards, 1.04x on json-parse-inspector, which
  is mostly the JSON parser); its 5 WebAssembly tests 225.4 (none
  `--jitless`); six of Speedometer 3.1's suites, 5 times, 18.3 and 12.9
  (1.4x: a page's DOM, style, layout and drawing don't run in V8). The
  whole run, five content_shells in one boot, is about 6 minutes.
- **WebGL in software** (track SWS, 2026-10-19): SwiftShader, the
  software Vulkan Chromium uses on Linux with `--use-angle=swiftshader`,
  under ANGLE's Vulkan back end. Linux opens it as a Vulkan driver (ICD,
  `libvk_swiftshader.so`) through Khronos's loader; Jam OS links programs
  static and has no `dlopen`, so patch 0915 makes SwiftShader the
  program's Vulkan: args.gn turns on ANGLE's Vulkan and SwiftShader with
  no shared libvulkan and no custom loader (ANGLE calls `vk*` directly, as
  on macOS), and `angle_vulkan_loader_dir` names a group of Jam OS's
  (`//build/toolchain/jamos/vulkan`) that is SwiftShader's Vulkan source
  set plus the three `VK_KHR_display` calls ANGLE's display for a bare
  screen links and SwiftShader built for Chromium leaves out (answered as
  the loader answers for a driver without the extension: no displays).
  The rest of 0915: ANGLE's translator links SwiftShader's SPIRV-Tools,
  not Chromium's (both would be in the program, the same symbols of two
  versions; ANGLE only links the validator, which a release build never
  calls); no `data_deps` on the shared library and its ICD file (ANGLE's
  and `ui/gl`'s); Wayland's headers for SwiftShader's Vulkan target (a
  Linux build finds them in the sysroot); and Reactor, SwiftShader's
  compiler, maps its code `MAP_JIT` (macOS's path: Linux maps a memfd,
  which Jam OS never makes executable). The GN change is 1,094 ninja steps
  for content_shell (most of SwiftShader was built already in the kept
  tree), a few minutes at `-j20`. The GPU process keeps the JIT power when
  it runs SwiftShader (section 12, patch 0914), which it does for
  `--enable-unsafe-swiftshader` (with `--disable-gpu`: software
  compositing, WebGL by SwiftShader, `--use-angle=swiftshader-webgl` on the
  GPU process's command line). `--disable-gpu` alone stays as it was: no
  WebGL, the GPU process holds no power (Chromium 155's automatic fallback
  to SwiftShader is off by default).
- **Tests**: cr-test.sh `webgl` (tools/cr/webgl.html): content_shell,
  every process type its own, draws a band with a shader (left half red,
  right half green, by `gl_FragCoord`) on a cleared ground; the page
  reads three pixels back and the screenshot has them
  where the canvas is; WebGL's renderer is "ANGLE (Google, Vulkan 1.3.0
  (SwiftShader Device (Subzero) (0x0000C0DE)), SwiftShader driver)", WebGL
  2 too; `ps` marks one child more `[jit]` than without the flag (the GPU
  process, launched first, with the two renderers); without it no WebGL
  context. Batch 19213457.
- **Open, noted and not chased**: the long single call's on-stack
  replacement stays at Maglev's speed (110 ms against 23 ms with
  `--no-concurrent-osr`): time it on Linux with the same build;
  `document.hasFocus()` is false in a fresh content_shell window (Wayland
  activation: CB6, G2); SwiftShader in the GPU process is Chromium's
  "unsafe" software WebGL (shaders from any page compiled to code there),
  which Jam OS gives its power only on the opt-in, until CR-f's sandbox;
  SwiftShader's Wayland surfaces are built but untried (ANGLE draws
  WebGL offscreen and Chromium composites it).

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
  2 KiB startup message refused less until CS1, which carries more in a
  VMO); a hand-over's kept data at most 64 KiB (CR2F, below).
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
- **The hand-over is rewritten, not moved (CR2F, 2026-10-16;
  [CR-REVIEW-POSIX](CR-REVIEW-POSIX.md) finding 1, Q1 decided A).** The
  child's namespace may reach what its caller's can't, so it believes no
  name the caller wrote: init reads the hand-over (`posixfds_read`, every
  rule checked) and writes the child its own, a file without its path
  (usable, `ENOTSUP` to give on, as with `SCM_RIGHTS`), no directories nor
  duplicates of them, the cwd `/`, the program's path the sealed one, and
  `POSIXFDS_SECURE`, which the crt makes `AT_SECURE` 1 (glibc's unsecure
  variables left out of the environment; musl's own checks). Every child
  is marked: init can't tell which program calls (since SX, the caller's
  image tells it: only a raised child is marked and starts in `/`, the
  others get the caller's directory, "SX" above). A POSIX child gets a
  hand-over even when its caller wrote none (0 to 2 then `/dev/null`); a
  malformed one refuses the start. Later (option C): names as objects, a
  directory channel for the cwd, so a `make` started this way can inherit
  its directory and give redirected files on. utest `exec_handover`.

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
  page: 262,144 hashes in 2048 buddy pages (8 MiB), listed by 4 pages
  of addresses (CR1 made the list pages, where it was one 32 KiB heap
  block, physically contiguous and uncharged), all charged to the job of the caller of
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
  size is PO13's ("big packages: a 250 MB program"; built since: 1 GiB a
  file, 2 GiB a package, and `tools/pkg-test.sh`'s 300 MiB `bigpkg`). So
  cr-test.sh `hello` runs as it was (a small C++ program from a package,
  now through the raised limits); the 300 MiB program is
  `bigprog-test.sh`'s.
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
self-image. CL2 left every other name ENOENT; PRC (2026-10-16, "PRC notes"
below) added the files Chromium's `base` reads (`stat`, `status`, `statm`,
`maps`, `cpuinfo`, `meminfo`, `/proc/stat`) and the neutral values it fills
where Jam OS has no meaning.

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

## PRC notes

Track PRC, `/proc`'s files for Chromium's `base` (2026-10-16): CB2's first
`base_unittests` run logged `/proc/self/status`, `maps`, `stat`,
`/proc/<pid>/stat`, `/proc/cpuinfo`, `/proc/meminfo` and `/proc/self/ns/user`
as ENOENT. This is section 3's `/proc` beyond CL2's `exe` and `fd`:
user/posix/sys/procfs.c (names, opens, listing; a "generated file" kind),
procinfo.c and cpuinfo.c (the text).

- **What `base` does with each.**
  - `stat` (ProcessMetrics, `Process::CreationTime`, `GetNumberOfThreads`,
    ProcessUtilTest's zombie check): the 52 fields of proc(5), parsed after
    the last `)`. State `R` for ourselves, `S` for a live child, `Z` for one
    that ended and is not reaped (field 52 its code as `wait` gives it);
    gone (ENOENT, and ESRCH on a file opened before) once reaped.
    `CreationTime` is `btime` of `/proc/stat` plus `starttime` in ticks.
  - `status` (BeingDebugged reads `TracerPid`, memory metrics read `VmRSS`,
    `VmSwap`, `VmHWM`): Linux's keys in Linux's order.
  - `statm`, `maps` (stack traces: "Failed to read /proc/self/maps"; V8's
    shared-library list), `cpuinfo` (`SysInfo::CPUModelName`, the first
    `model name`), `meminfo` (`SysInfo::AmountOfAvailablePhysicalMemory`,
    the memory pressure monitor: MemTotal, MemFree, MemAvailable) and `stat`
    (system): as above.
  - `/proc/self/ns/user` stays **ENOENT**: Linux has no such file without
    user namespaces, and the sandbox code's "are unprivileged user
    namespaces supported" check then says no, which is right (Jam OS has
    none; the sandbox is its own grants). With `--no-sandbox` it isn't
    asked.
- **What is real, and from where.**
  - A process: `process_get_info` (state, live threads, CPU time, pid, and
    new in PRC `start_ns`, the uptime at `process_create`, and `name`, and
    in CBF4 `faults`, the address space's page-fault count);
    our process group, session and parent from the hand-over; a child's
    from libc's table of children (proc.c: `lx_child_facts`).
  - The address space: **the one kernel call PRC adds, `process_get_maps`
    (138, `RIGHT_INSPECT` on a process)**: the mappings from an address on,
    in order, up to 256 a call (`struct map_info`: base, length, the VMO's
    koid and offset, the pages present in the page tables now, the
    permissions). The pages present are counted by walking the page tables
    (cost: the pages present; a mapping with no read, a reservation not
    touched, costs nothing), under the address space's region lock like
    `process_info.mappings`. It serves `maps`, VmSize and VmRSS (stat,
    status, statm) for ourselves and for each child (through its process
    handle). libc's table of its mappings (mem.c: `mem_kind_at`) says which
    are shared (`s`), on a file's own pages (the file's offset), or the
    break (`[heap]`); our program headers say which are the image (named by
    `/proc/self/exe`'s path, with its offset). RssAnon, RssFile and
    RssShmem are the present pages of the other three groups.
  - CPUs: the count from `sys_public_info`; everything else from CPUID as
    the CPU we run on answers it (family, model, stepping, brand, cache,
    address sizes, Linux's names for the feature flags); `cpu MHz` the
    TSC's rate from the time page, `bogomips` twice it.
  - Memory: `sys_public_info`, which gained **`mem_free_pages`** (the page
    allocator's free lists and per-CPU stashes; it does not drain the
    kernel heap's magazines, so a program asking cannot slow it: the pages
    cached there count as used). MemTotal and MemFree are those;
    MemAvailable is MemFree (no page cache is kept apart). `sysinfo(2)`'s
    freeram is the same figure now.
  - The clock: `btime` is the wall clock now minus the uptime.
- **Neutral values** (Linux's "none", where Jam OS has no meaning):
  stat: tty 0 and tpgid -1, flags `PF_RANDOMIZE`, fault counts 0, stime 0
  (the kernel keeps one CPU time: utime has all of it), cutime the CPU time
  of the children we reaped, cstime 0, priority 20, nice 0, itrealvalue 0,
  rsslim unlimited, startstack and the stack and instruction pointers 0,
  wchan 0, exit_signal 17, processor 0, rt_priority and policy 0, the
  data, brk, argument and environment addresses 0; the signal sets are ours
  (pending, blocked, ignored, caught; a child's 0). status: no tracer, uid
  and gid 1000, no supplementary groups, no capabilities, `Seccomp: 0`,
  `NoNewPrivs: 0`, all CPUs allowed, no context switches counted, VmLck,
  VmPin, VmLib, VmStk, VmPTE and VmSwap 0 (the main stack is not told apart:
  it is in VmData), VmPeak and VmHWM the largest VmSize and VmRSS this
  process has seen read here (a child's: its current ones). maps: device
  `00:00` and inode 0 on every line; no `[stack]`, `[vdso]` or `[vvar]`, and
  no names for files mapped with `mmap` (only the image and the break are
  named); a child's lines have no names. cpuinfo: `microcode 0x0` (an MSR),
  physical id 0, siblings and cpu cores the CPUs online, core id, apicid
  and initial apicid the CPU's number (a CPU's APIC id needs a root
  handle), bugs empty. meminfo: every field but the memory figures 0
  (`Hugepagesize` 2048 kB, `DirectMap4k` all of it). stat (system): the CPU
  times 0 (`cpu_stat` needs a root handle), `processes` our pid.
- **A read is a pass.** The text is made when a read starts at offset 0
  (the first read, or `lseek(0)` and read, or `pread` at 0) and kept for
  the reads at later offsets of that pass; `lseek(SEEK_END)` makes it to
  say its length. So a program reading with a small buffer sees one file,
  and one that reads again sees the new state. Files are 0444, size 0 as
  Linux's, not writable (EACCES), not directories (ENOTDIR).
  `/proc/self/fd/N` of one reads `/proc/<pid>/<name>`.
- **Who is listed.** `/proc` lists `self`, our pid, each child not yet
  reaped (the first 256), and `cpuinfo`, `meminfo`, `stat`. A child's
  directory has `maps`, `stat`, `statm` and `status` (no `exe`; `fd` came
  with PR, [PR notes](#pr-notes-2026-10-18)); any other pid is ENOENT, as
  before. A process reading its own `maps` sees the
  buffers its read allocates (musl maps any allocation over 7 KiB): as on
  Linux.
- **Not built** (each shows up in the ENOENT log if something wants it):
  `smaps` and `smaps_rollup`, `limits`, `oom_score_adj`,
  `auxv`, `io`, `uptime`, `loadavg`, `diskstats`, other pids' files.
  (`cmdline`, `task/`, `mem` and a thread's files came with CBF4.)
- **Tests**: posixtest's `procinfo` section (in the `init` run; also
  tools/shell-tests/posix-procinfo.txt from the shell): the files against
  proc(5) and what the process really does (threads made and ended,
  memory mapped, touched and given back, CPU burned, a protect that splits
  a mapping, a guard, `MAP_SHARED`, the break, the signal masks, CPUID's
  vendor, family and feature bits, `sysconf` and `sysinfo` against
  meminfo, `btime` against the clocks), and a child's from live through
  zombie (exit code 5 << 8, and 9 for SIGKILL) to reaped. utest's
  `process_maps` (the call's fields, the cursor, the refusals, a process
  that never ran), `sys_public_info` (free pages follow 16 MiB committed
  and given back) and `posix_ids` (the creation time and name). ktest
  `aspace_maps_shapes` (the list, the cursor, protect's splits, the pages
  present).

## CS1 notes

Track CS1, launching (2026-10-15): section 5's Jam OS half.

- **argv and the environment in a VMO** (`SR_ARGS`, 21, and `struct
  startup_args` in `<roles.h>`). libjam's spawn keeps them in the startup
  message while they fit its 2 KiB and 128 strings; past that it writes
  them into a VMO of their own (made and written here through a mapping,
  sent read-only) and the message carries none. The child's libos start
  (user/lib/start.c, `take_args`) checks the header, copies the strings
  into a VMO of its own with room for argv's and environ's pointers in
  front, maps it for good and closes `SR_ARGS`: nothing the starter still
  holds can change them under it, and argv's strings stay writable (a
  process title). The limit is Linux's usual `ARG_MAX`: the strings and 8
  bytes a pointer, at most 2 MiB (more is `ERR_OUT_OF_RANGE`, `E2BIG`).
  The `SR_ARGS` handle takes one of the message's 16 places: the extras
  give way first (an error), then `SR_SELF_IMAGE` (left out). A malformed
  VMO leaves the program with argc 0 and a line in the log, as a malformed
  message does. The POSIX crt builds Linux's block in a VMO of its own when
  it outgrows 512 longs on the stack. So CE1's 2 KiB cap is gone:
  `/svc/exec` now takes what its IDL says (12 KiB, 128 strings; raising
  that is an IDL change, not needed by Chromium, whose children are
  `/proc/self/exe`'s and never go through `/svc/exec`).
- **The program's path without a hand-over.** `SR_ARGS` also names the
  path (`exe_len`) for a program libjam's spawn gives `SR_SELF_IMAGE` and
  no POSIX hand-over (nor a `before_start`, which passes one on): the
  shell's background runs from `/data`, a native starter's. libc's
  `/proc/self/exe` takes the hand-over's path first, then this
  (`startup_exe`), and only then looks through argv[0]'s file, which for a
  250 MB program read and compared every page. The cost is one small VMO
  for such a start.
- **`posix_spawn_file_actions_addclosefrom_np`** (glibc 2.34's): in our
  `<spawn.h>` (an override of musl's header, user/posix/include/spawn.h)
  and musl's posix_spawn override, a new file action (6) that drops every
  slot of the child's table from N up, in order with the others: a `dup2`
  before it above N is gone, one after it stays.
- **The self-spawn check's margin** (CL2's 95 against 162 pages): the
  child's own pages are most of what a start costs, so posixtest now
  measures the same program started from the boot image beside it (which
  never copies its code) and checks the self-image children against that
  (98 and 98 against 100 from `/data`, 93 against 93 from the boot image,
  with 165 pages of code).
- **Tests**: posixtest's `launch` section (the `init` run): 503 arguments
  (62 KiB) and 64 KiB of environment seen whole by a child from the boot
  image and from `/proc/self/exe`; 3 MiB `E2BIG`; closefrom checked
  through the child's `/proc/self/fd`. Its `self-image` section (from
  `/data`, posix-mmap-test.sh `spawn`) adds a child started from the image
  with no hand-over, whose `startup_exe` and `/proc/self/exe` are its
  path.

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
  channel: libc keeps one per process; one per program since CR2F,
  below), names of at most 107 bytes. What waits at a listener is charged
  to the broker's job until accept reads it: at most 64 small messages a
  listener.
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
- **Counted per program (CR2F, 2026-10-16; [CR-REVIEW-POSIX](CR-REVIEW-POSIX.md)
  finding 3, Q2 decided A).** libos's grants give `/svc/unix` by name as
  an opener of the program's own, made with `svc.connect` on the
  starter's channel when the namespace is sent and marked as handing out
  no more; `"*"` gives the starter's as it is (init, the shells, their
  tests hold the shared channel). So one program binds 32 listeners at
  most whatever it opens, and a listener counts against its opener until
  it closes (the broker holds the opener's slot till then). Abstract
  names stay one table (per session later, if a port needs it). Left: a
  `svc exec` program keeping 64 approved `svc unix` children alive takes
  every opener; after a broker restart a program with an opener of its
  own has no `/svc/unix` until it is started again.
- **Tests**: posixtest's `unix` section (in the init run;
  `tools/shell-tests/posix-unix.txt` alone, in shell mode); utest's
  `unix_names`, `unix_backlog`, `unix_limits` (the broker's protocol and
  limits). The `wayland` section's refusals changed with it: a bind to
  `/svc/wayland` is `EADDRINUSE`, `listen` unbound `EINVAL`, a connect to
  an empty path (an abstract name of NULs) `ECONNREFUSED`.

## CA1 notes

Track CA1's API half (2026-10-15); the Chromium back end waits for CB4.
The output library of M12 (`<audio.h>`, user/lib/audio.c: a mixer stream
behind `audio_open`, blocking `audio_write`, `audio_drain`,
`audio_discard`, volume, stats) already was most of section 11's API, so
this adds what a program that writes on a clock of its own needs, and
nothing is renamed.

- **What was added** (`<audio.h>`; the new pacing calls are in
  user/lib/audio_flow.c): `audio_open_fmt` (rate, 1 or 2 channels,
  `AUDIO_S16` or `AUDIO_F32`, a name for `vol`) and
  `audio_s16_from_f32` (round to nearest with the resampler's TPDF
  dither, except that a float made from a 16-bit value comes back exactly
  and silence stays silent; clips; NaN is 0); `audio_try_write` (as many
  frames as the ring has room for, never waits); `audio_room`,
  `audio_room_max`, `audio_queued` (no call into the mixer; room is in the
  caller's frames, a little under what the ring holds when the rate is
  converted); `audio_wait_room(min, deadline)`, and for a thread that
  waits on something else too, `audio_arm_room` plus `audio_room_event`
  (the event's `MIXER_SIG_SPACE`); `audio_start` and `audio_pause` (the
  mixer's start and stop: a stream is otherwise started only when its ring
  fills or at a drain, so a writer that has pre-rolled starts it itself);
  `audio_position` (frames written and heard at 48 kHz, `latency_ns` to a
  frame written now: the ring ahead of it plus the resampler's delay, the
  mixer's `lead`, and `at`, the kernel clock when it was asked: one call
  into the mixer). `audio_discard` on a stream that isn't started drops
  the ring at once. In `mixer_client.c`/`<mixer.h>`: `mixer_wait_room`
  (with a size), `mixer_arm_room`, `mixer_room`, `mixer_peer_closed`.
- **What the mixer gives, and what it costs a browser**: the stream's
  lead is 4 periods (171 ms), so a frame written now is heard 171 ms
  later at the least, and `audio_position` says so; the ring is 1.37 s,
  the mixer takes a period (2048 frames, 43 ms) at a time and wakes a
  waiter for room once a period; at most 4 streams a program (one opener
  per program), 16 in all. A **pause** loses the lead: when nothing else
  plays the mixer closes the device at once and the frames it had taken
  ahead of the speaker (up to 171 ms) are not played, so a resume skips
  them (tools/audio-api-test.sh measures 8147 frames skipped and none
  repeated). Chromium's `Stop()` throws its queue away anyway
  (`audio_pause` then `audio_discard`), so it doesn't care; a player that
  must resume exactly would need the mixer to keep the lead on a stop.
  Left alone: it is the mixer's, not the library's.
- **Tests**: utest `audio_out` (11 cases; the conversions and the
  refusals need no device, the other nine skip without one) and
  tools/audio-api-test.sh (`audio-api.txt`): the cases from the shell on
  intel-hda into a WAV, which holds the 16-bit and the float pattern
  sample for sample, the resampled mono tone, a paced tone with no gap,
  two streams at once, and the pause case. mixer-test, play-test,
  beep-test, music-test, audio-quality-test and splash-test, which use the
  library, pass unchanged.
- **What Chromium's patch will need** (written once CB4 gives a browser;
  an `AudioManagerBase` subclass and an `AudioOutputStream` of about 500
  lines, Fuchsia's pair as the model):
  - `AudioManagerJam`: `HasAudioOutputDevices` is whether `audio_open_fmt`
    gets `ERR_NOT_FOUND`; one output device; `HasAudioInputDevices` false
    (input stays unbuilt); preferred output parameters stereo, 48000 Hz,
    480 frames (10 ms) a buffer, which is what `audio_open_fmt(48000, 2,
    AUDIO_F32)` takes without conversion (other rates work, through the
    resampler).
  - The stream: `Open` is `audio_open_fmt` (name "chromium"); a pump on
    the audio thread's task runner, a timer at half a buffer: it asks
    `audio_room`, and only when a whole buffer fits calls `OnMoreData`
    with `delay = latency_ns` and `delay_timestamp` from `at` (the kernel
    clock; `TimeTicks::Now()` is `clock_gettime(CLOCK_MONOTONIC)`, check
    that musl's call layer reads the same clock), interleaves the bus
    (`AudioBus::ToInterleaved<Float32SampleTypeTraits>`) and
    `audio_try_write`s it. `Start` pre-rolls about 90 ms (two mixer
    periods) and then calls `audio_start`; `Stop` is `audio_pause` and
    `audio_discard` and cancels the timer; `Close` is `audio_close` and the
    manager's release; `SetVolume(v)` is `audio_set_volume` with
    200 log10(v) centibels (below -96 dB: -960); an `ERR_PEER_CLOSED` from
    a write or `audio_position` is `OnError` (the mixer was given up on:
    open the stream again).
  - A wait for room that can be cancelled is the timer; a back end that
    prefers a thread waits with `audio_wait_room` in 10 ms slices, or on
    `audio_room_event` and its own stop event together.
  - Build: the patch's code includes `<audio.h>`, which includes `<os.h>`:
    the sysroot has `<jamos/libjam.h>` (it puts musl's `PAGE_SIZE` aside)
    but no `os.h`, so the GN flags need `-I user/include -I build/uinc -I
    drivers/include`, or a `<jamos/audio.h>` that includes `libjam.h` and
    then `<audio.h>` is added with the sysroot's headers (decide with CB4);
    libjam.a already has the library. The `use_jamos_audio` argument picks
    the manager in `audio_manager_*.cc` and drops the ALSA/Pulse/CRAS
    guards.
  - The program that hosts the `AudioManager` (the audio service's
    process, or the browser's if the service runs in-process) needs
    `svc audio` in its list; a child started through `/svc/exec` gets its
    own approval only (CE1), so the launcher's list for that process must
    say it. More than four streams in one process are refused
    (`ERR_NO_RESOURCES`): Chromium's output mixer shares a physical stream
    among same-parameter streams, and falls back to a fake output if
    `Open` fails.

## BT notes

Track BT, stack traces (2026-10-16): glibc's `<execinfo.h>` in libc, so
Chromium's `StackTrace` takes its glibc path instead of being patched out
(the owner's rule: fix the layer, not Chromium).

- **What libc has now**: `backtrace()`, `backtrace_symbols()` and
  `backtrace_symbols_fd()` (user/posix/glibc/execinfo.c; the header,
  user/posix/include/execinfo.h, is new, not an override of musl's). The
  directory is a new one in posix.mk: glibc's extensions musl lacks,
  built with the layer's flags but, unlike the layer's objects, not
  renamed `__jl_*` (they are libc's public names). They are in libc.a as
  one member, linked only by a program that calls them.
- **Its own unwinder, not `_Unwind_Backtrace`** (the plan was libunwind's).
  `backtrace()` reads the program's `.eh_frame` through `.eh_frame_hdr`'s
  index (found from `__ehdr_start`'s program headers) and runs the call
  frame instructions for each return address: about 550 lines of C, x86-64
  only, no libunwind (its tables read over every function of posixtest
  on the Mac first: 1,841 functions, a row for each with its return address
  at CFA-8, none without one but the assembly: `_start`, `__unmapself`,
  `__restore_rt` and libjam's call stubs). Why: the cross GCC's libgcc has an unwinder but no
  way to find a frame (it looks only at frames `crtbegin.o` registers,
  and jamos-cc links none: checked with `nm`, `unwind-dw2-fde.o` not
  `-dip`), so a GCC-linked C program, posixtest included, could not use
  `_Unwind_Backtrace`; and a crash handler wants a walk with no lock, no
  malloc and no call (libunwind finds a frame through `dl_iterate_phdr`
  and a cache under a lock). Backtrace's entry is assembly (it saves the
  callee-saved registers before any prologue changes them) and the walk
  stays on the caller's stack. A frame whose CFA is an expression, a frame
  without a table (`_start`, hand-written assembly) or a corrupt one ends
  the trace; a pc without a table is still the last one stored (libgcc's
  way). The stack is read only where a table says a caller's registers
  were saved; CFAs must rise, so a trace ends.
- **Unwind tables in musl, the layer and libjam's objects in libc.a**
  (posix.mk: `-fasynchronous-unwind-tables`; musl's configure and the
  layer had turned them off). On Jam OS a signal handler runs at a call
  boundary in libc (sig.h), so a trace from a handler passes `run_handler`,
  `deliver`, `sig_call`, the call layer's entry and musl's `raise` before
  the function that raised the signal: all need tables. The same tables
  let a trace cross a `qsort` comparator, a thread's start, `main`'s
  caller, and C++ exceptions unwind through musl. Cost: `.eh_frame` is
  about a tenth of a program's code (posixtest: 12 KiB to 70 KiB on
  576 KiB of code, the file 0.5% bigger); no code changes. Not done: the
  start code's `_start` has no CFI (a trace ends there, as it should).
  Faults are not signals on Jam OS (the kernel ends the process), so
  Chromium's SIGSEGV/SIGILL dump handlers never run; `abort()` is a
  signal (SIGABRT) and does.
- **The lines**: a program on Jam OS keeps no symbol table in memory (the
  loader maps segments only), so `backtrace_symbols()` prints what glibc
  prints for a non-PIE program with no symbol it can find: `name() [0xaddr]`
  with argv[0] for an address in the program's segments, `[0xaddr]` for
  any other. Naming functions would mean reading the program's file
  (`/proc/self/exe`) and its `.symtab`; Chromium's `StackTrace` does that
  itself (base/third_party/symbolize) and only falls back to
  `backtrace_symbols()` where it is not built in, so nothing waits for it.
  `backtrace_symbols_fd()` allocates nothing (one `writev` a line).
- **Chromium**: the upstream tree uses `<execinfo.h>` in three files
  (grep of the pristine 155 tree): `base/debug/stack_trace_posix.cc`
  (`HAVE_BACKTRACE`, by `__GLIBC__`), V8's `external-reference-table.cc`
  (DEBUG builds only) and SwiftShader's LLVM config (not built here).
  Alpine's `no-execinfo.patch` (our 0001) and our own 0911 (a definition
  for the build without `backtrace()`) are gone; the new 0911 adds
  `defined(__jamos__)` to the one condition in stack_trace_posix.cc
  (`__jamos__` is what the toolchain, 0901, already defines for the target;
  `__GLIBC__` itself is not defined, which would turn on every other
  glibc-only path Alpine's patches avoid). With it `stack_trace.cc`,
  `operator<<` and the unit tests are upstream's again, so base_unittests'
  `StackTrace.*` tests now run on Jam OS (CB2 triages any failure: they
  need `/proc/self/maps` and the program's file for names, `symbol_level =
  0` and a stripped program give addresses; with `USE_SYMBOLIZE` on,
  as the build has it, Chromium's own reader names the frames and
  `backtrace_symbols()` is not called). The kept tree takes the change
  as a patch change: `stack_trace.cc`, `stack_trace_unittest.cc`,
  V8's `external-reference-table.cc` and SwiftShader's config go back to
  upstream's bytes and `stack_trace_posix.cc` changes, so only their
  objects and the links rebuild (BT did not run that build: the tree is
  CB2's and CB3's). Checked instead, on the PC: the new patch applies to
  the pristine file with no fuzz, and that file compiled with the
  build's own command line for it (from `ninja -t commands`, the source
  and output swapped, libc's new header ahead of the sysroot's, nothing
  in the tree written) gives an object that calls `backtrace` and holds
  `ProcessBacktrace` and the stream handler. The first build of the
  tree after this merges does the rest.
- **Tests**: posixtest's `execinfo` section (the `init` run): four nested
  functions give exactly their return addresses; a short array; a second
  thread; a trace from a signal handler reaches the raising function and
  its caller; a `qsort` comparator's trace reaches `qsort`'s caller;
  `backtrace_symbols` and `backtrace_symbols_fd` lines.
- **A kept build fails it (2026-10-16, track XR)**: after BT merged,
  posixtest's execinfo section failed on the main checkout's PC tree
  (`execinfo.c:194`, `:195`, `:227`, `:235`: a trace through musl's or the
  layer's frames too short) and passed everywhere else. No code had
  regressed: that tree was built before BT, and the Makefile then had no
  rule that rebuilds an object when its flags change, so musl's and the
  layer's objects that BT's sources did not touch kept being compiled
  without unwind tables (`.eh_frame` missing in `qsort.c.o`, `raise.c.o`,
  `calls.c.o`, `kill.c.o` of `~/work/jamos`'s libc.a; every dir of the PC
  from before the 16th was the same). The flag stamps (MKF: `build/flags/`)
  are the fix, and `make check` now also refuses a libc.a whose signal,
  thread-start, qsort, execinfo or libjam members have no `.eh_frame`
  (`tools/checkeh.py`). Checked on the PC in one tree: built with the
  flags off, then the flags back on without a rebuild (the old Makefile):
  `execinfo.c:183` fails both times; with MKF merged the same tree
  rebuilds and passes.

## CN1 notes

Track CN1, netstack's caps (2026-10-18): the caps themselves were raised
on 2026-10-12 (`85cd9e62`, with loopback in `ac11dafa`; merged "untested in
QEMU", its memory only computed). This round ran them, tested the caps
over the card and measured netstack's memory at them. No change to
`net.h`, `stack.h` or lwIP's options was needed: every pool held.

- **The caps, as built**: sockets an opener 256 (the 257th is
  `ERR_NO_RESOURCES`), in all 512, ordinary programs together 384;
  connections an opener 512, in all 1,024, ordinary programs together
  768 (the rest is `/svc/net-sys`'s reserve, which only the network's own
  services may ask for: checkwants' `SERVICES_ONLY`); ring bytes 64 MiB an
  opener, 128 MiB in all, 96 MiB ordinary. Listeners stay 4 an opener, 16
  in all.
- **1,024 connections to a host cannot be had by a program**: the most a
  shell program (or Chromium) can hold, with any number of processes, is
  the ordinary 768. The test host takes **768** from two programs at once
  (`tcptest many`, below: 512 from one, the 513th refused, 256 from
  another, its 257th refused) and the full **1,024** is tested in the
  utests with a fake host (`netsock_mem`: 512 sockets and 1,024
  connections open at once through `/svc/net` and `/svc/net-sys`, a
  1,025th refused; `nettcp_limits` the same with handshakes in flight).
  *Open question for the owner*: is 768 enough for a browser? (Chromium's
  socket pool limits, from memory, are 256 sockets in a pool and 6 to a
  host, so a normal browser needs a few hundred.)
- **Tests added**: utest `netsock_mem` (netstack's job pages and handles
  after each stage, closing gives every handle back; `netdrv_job_used` in
  the harness); `tcptest many <address> <port> <conns> <bytes> <hold s>
  <full 0|1>` with `tools/tcp-test.sh`'s new part (`5037:64` and
  `5038:65536` servers on the peer; the peer must count 858 connections
  right); the existing `netsock_caps`, `netsock_limits` and `nettcp_limits`
  passed unchanged at the caps. `tools/net-test.sh` is untouched.
- **Memory**, measured (BENCH.md): netstack's job is 208 KiB idle; 512
  datagram sockets 6.4 MiB; 512 sockets with 1,024 open idle connections
  18.8 MiB (3 pages an idle socket, 5 after the first bytes, about 93 KiB
  with both default rings used); 768 over the card 9.5 MiB idle and 15.5
  MiB after 64 bytes each way. The ring budget (128 MiB) bounds the rest:
  about 95 MiB if every connection's rings fill. lwIP's heap in use is 0
  with them idle.
- **Findings**: the card's tx ring drops some frames of a 512-connection
  burst (lwIP sends them again; `net stats` "tx ring full" 600 to 1,100)
  and 512 connections open in 12 ms of guest time; the peer's TCP
  (`tools/tcppeer.py`) goes on resending to connections whose program
  died (a million resends in one failed run: harmless, but the run's
  other counts are meaningless); a program's ring pages are charged to
  netstack's job, not the program's (`ps` shows the program at 320 KiB
  holding 512 connections).

## RL notes

Track RL, record locks across processes (2026-10-18; the main session's
D9, for CB6: Chrome's profile databases are SQLite and leveldb, which lock
files with `fcntl` across the browser's processes). Before it libc granted
every lock and kept none (`dup.c`), so `F_SETLK` never said `EAGAIN` and
`F_GETLK` always `F_UNLCK`: the 7 `FileLockingTest` tests on the skip list.

- **The protocol** (`abi/idl/file.idl`, its own commit; the ABI stamp
  moves): three methods on an open file, not on `fs` (a lock is on an
  open, and the file channel is what the service closes it with):
  `lock` (take, change or drop a lock; with `FILE_LOCK_WAIT` a conflict
  also gives a wake channel), `getlock` (`F_GETLK`: the first lock in the
  way, its bytes and its holder's pid) and `lockowner` (an owner for an
  open file description's locks, and its token).
- **The table** (`<rlock.h>`, `user/lib/rlock.c`, a library so fat could
  take it later): Linux's rules: a write lock conflicts with any other
  owner's lock over a byte, reads don't; an owner's new lock replaces its
  own where they overlap (split, joined to its neighbours of its type);
  POSIX and OFD locks share one space, flock's has its own; `EDEADLK` by
  Linux's ten-step walk of the waits (POSIX owners only, as Linux). 256
  locks on a volume (`RL_LOCKS`), 64 owners (`XF_LOCK_OWNERS`), 64 waits;
  past them `ENOLCK`. A change is built in the table's spare copy and made
  current by one store, so a service killed in the middle leaves its
  successor the table before or after the change; a request run again
  sets what it set (no change).
- **The owners.** A POSIX (`fcntl`) lock's owner is the process that wrote
  the request (the kernel's `CHANNEL_READ_PID`: no pid is sent, so no
  process can speak for another; extfs now reads file requests with the
  pid, as it did fs requests: one more system call only when a reply
  waits, which the port's wait has usually sent). Each lock remembers the
  open it was set through; when that open's channel closes (the process
  closed its last description of the file, or died and the kernel closed
  it) every POSIX lock of that process on the file goes. libc adds
  Linux's close rule: any `close()` (and `dup2` over a descriptor) of a
  file the process holds fcntl locks on unlocks the whole file
  (`lock_closing`, from `fd.c`), even through another description. An
  OFD or flock lock's owner is a `lockowner`: a random 64-bit number and a
  token, a channel whose peer the description keeps; `fork` gives the
  child's copy a duplicate (file.c's fork hook), so the locks go when the
  last copy closes, in whichever process (Linux's rule for a
  description).
- **Waiting.** `F_SETLKW` and a `flock` without `LOCK_NB` ask with
  `FILE_LOCK_WAIT`; a conflict gives a wake channel whose peer the service
  closes when a lock on the file is cut or changes type (or the service
  ends). libc waits for its `SIG_PEER_CLOSED` (an interruptible wait:
  `EINTR`, restarted under `SA_RESTART` by signal.c's rule) and asks
  again; without a wake (none free, or a reply sent again after a
  restart) it asks again after 10 ms, and after a second whatever comes.
  The service counts the caller as waiting until it asks again; a waiter
  that gave up closed its end, and its record is swept before the next
  deadlock check, so an interrupted wait makes no false `EDEADLK`.
- **A dead holder** never holds anyone up: a lock found in the way whose
  open's channel or owner's token is closed already goes first, before the
  close's packet is served (the order a kill and the next request reach
  the port in doesn't matter).
- **Restarts (my call): locks survive.** The table and the owners'
  numbers are in extfs's state (`struct xf_locks`, layout 8), each token
  with the keeper (`XF_KEEP_LOCK`); a successor checks the table
  (`rl_check`; one that fails is a fresh start, which closes every open
  file, so no holder loses a lock without its file's `ERR_PEER_CLOSED`),
  takes the tokens back, and drops the owners whose token didn't come back
  or closed while no instance ran; a POSIX lock goes with its open as
  before. The waits are the instance's memory: their wake channels close
  with it, and the waiters ask the successor again. Why not `ENOLCK`
  after a restart: SQLite and leveldb treat a lost lock as corruption
  risk, and the state already keeps every open file; the table is 20 KiB.
- **fat and the boot image (my call): the old rule**, granted and never
  kept, `F_GETLK` `F_UNLCK`: their services answer `ERR_NOT_SUPPORTED`
  (utest `fs_no_locks`) and libc doesn't ask a mount with one writer a
  file (`fobj.c`'s `multi`). Not `ENOLCK`: SQLite on a fat `/data` would
  then fail to open its databases, where today it works; and fat lets one
  process at a time write a file, so two writers never meet there (a
  reader beside a writer still sees no lock). fat with real locks is a
  small track whenever it matters (the table library is ready).
- **Not kept** (Linux would keep them): locks across `execve` (the
  hand-over opens each file afresh, and the old opens close), and a
  description handed to a child by `posix_spawn` or `SCM_RIGHTS` (an open
  of its own, with none of the description's OFD or flock locks). Neither
  is something SQLite, leveldb or base's tests do.
- **libc-test `functional/fcntl` stays listed** (run alone: still
  `XFAIL`, status 1), most likely for another cause, read from the code
  rather than traced: its file is `tmpfile()`'s, unlinked at once, and a
  forked child opens a file again by its path at its first use (FL2),
  which fails for an unlinked file, so the child's `fcntl` would be
  `ENOENT`, not `EAGAIN`. The reason in `expected-failures.txt` says so
  now. A forked child's use of an unlinked file it inherited is FL2's
  limit (reads and writes too), not the locks'.
- **Tests**: posixtest's `locks` section (in the `init` run;
  `tools/shell-tests/posix-locks.txt` alone): between processes, conflicts
  and `F_GETLK`'s pid, a split and a join seen by the other process,
  `F_SETLKW` and `flock` woken 201 ms into a 200 ms hold, `EDEADLK` for
  whichever process closes the circle, an `EINTR` wait forgotten, the
  close rule (another open's close, a dup's), `SIGKILL`, fork's rules,
  OFD locks, SQLite's PENDING/RESERVED/SHARED pattern, and base's
  `FileLockingTest` pattern (a `posix_spawn`ed `/proc/self/exe` child
  locks; its exit and its `SIGKILL` let go); on a fat `/data` the old
  rule. utest `extfs_locks` (the rules, rights, a full table and a split
  refused there with nothing changed, the owners' limit), and
  `extfs_restart_locks` (a lock request and a `lockowner` ended at their
  commit; every lock and owner carried on; a fresh start); both in
  `tools/extfs-svc-test.sh`. The 7 `FileLockingTest` tests check exactly
  the spawned child's case (lock, unlock, close, exit, terminate, shared
  and exclusive): they come off the skip list after the next relink and
  base run (not run here: CB5 holds `~/cr`).
- **For the merge**: extfs's state layout 8 (WR2 and IR took 6 and 7), the dispatch line
  for `XF_KEY_LOCK_BIT` in `main.c`, `files.c`'s `xf_file_ops` and
  `close_file` lines, request.c's `take` (file requests read with the
  pid), `adopt.c`'s keeper slots and check.

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
- **A zygote** (fork without exec, Chromium's Linux default): possible
  once [FORK-PLAN](FORK-PLAN.md)'s `fork` is in; with Jam OS's sandbox
  (CR-f) if at all.
- **Security updates**: M13.7's PO12 audit with Chromium's CVEs, and a
  rebuild at each Alpine stable bump.
- **More**: CJK fonts, input methods, drag and drop, the microphone and
  camera, printing to a file, Chromium's DevTools profiler (with signals
  to a thread).
