# CR research: what Chromium's source says it needs

Status (2026-10-11): a quick research pass (about an hour) to firm up the
[roadmap's Chromium plan](ROADMAP.md#now) before CR0. Docs only. It reads
Chromium's main branch online (about version 155, the one
[Alpine packages](https://gitlab.alpinelinux.org/alpine/aports/-/blob/master/community/chromium/APKBUILD)
today) and Alpine's musl build. Every claim has a link; guesses are marked
*unsure*. Links to Chromium files point at `main`, so line numbers drift.

Link text is the file's path in Chromium's tree (or "v8 path" in V8's).
"apk" below means a file in Alpine's
[aports chromium directory](https://gitlab.alpinelinux.org/alpine/aports/-/tree/master/community/chromium).

## The answer in one paragraph

The Linux route is right: Alpine already builds Chromium's Linux code
against musl with about ten small musl patches, most of them in the
seccomp sandbox that Jam OS won't use. The first target is `content_shell`
built as a static, non-component Linux build with GPU, sandbox, zygote,
NaCl-era leftovers, printing, PDF and the crash reporter off, running
`--no-sandbox --no-zygote --disable-gpu` with V8 `--jitless`, first
`--single-process --ozone-platform=headless`, then multi-process on
Wayland. What Jam OS must add, beyond what M13, M13.6 and G2 already
plan, is mostly in libc and the kernel's address space: **`AF_UNIX`
socketpairs with `SCM_RIGHTS`** (Mojo), **a launch path for Chromium's
fork-then-exec** (an emulated `vfork`, or a small `posix_spawn` patch),
**`/proc/self/exe`** and a few `/proc/self` reads, **memfd seals and a
read-only reopen**, **read-only shared file mappings**, **huge
`PROT_NONE` reservations** (V8's cage and PartitionAlloc's pools), and a
**clang + libc++ + Rust toolchain** for a musl target. Dynamic linking
(M14.5) is **not** on the path for software rendering: ANGLE links
statically by default and nothing else needed is loaded with `dlopen`.
The roadmap's CA store is not needed either: Linux Chromium verifies
TLS with its own built-in root store.

## 1. The Linux route or the Fuchsia route

- **The Linux route needs less Jam-specific work.** Linux Chromium
  expects POSIX, `AF_UNIX` and Wayland (all planned in M13 and G2). The
  Fuchsia port expects Fuchsia's whole platform instead:
  - process launch through `fdio_spawn_etc` and namespace entries
    ([base/process/launch_fuchsia.cc](https://chromium.googlesource.com/chromium/src/+/main/base/process/launch_fuchsia.cc));
  - Mojo over Zircon channels
    ([mojo/core/channel_fuchsia.cc](https://chromium.googlesource.com/chromium/src/+/main/mojo/core/channel_fuchsia.cc)):
    the one piece that fits Jam OS's channels well;
  - fonts from the `fuchsia.fonts.Provider` FIDL service, not fontconfig
    ([skia/ext/font_utils.cc](https://chromium.googlesource.com/chromium/src/+/main/skia/ext/font_utils.cc));
  - display through Flatland
    ([ui/ozone/platform/flatland](https://chromium.googlesource.com/chromium/src/+/main/ui/ozone/platform/flatland/ozone_platform_flatland.cc)),
    audio through `fuchsia.media`
    ([media/audio/fuchsia](https://chromium.googlesource.com/chromium/src/+/main/media/audio/fuchsia/audio_manager_fuchsia.cc)),
    and the Fuchsia SDK to build at all
    ([docs/fuchsia/build_instructions.md](https://chromium.googlesource.com/chromium/src/+/main/docs/fuchsia/build_instructions.md)).

  So the Fuchsia route means writing a Zircon system call library over
  Jam OS's calls, fdio (POSIX over FIDL) in place of M13's musl layer, and
  a dozen or more FIDL services (io, fonts, Flatland, sysmem, media, name
  lookup). Its shipped product was WebEngine, not the browser
  ([fuchsia_web/README.md](https://chromium.googlesource.com/chromium/src/+/main/fuchsia_web/README.md));
  *unsure*: whether the full Views browser UI was ever usable there
  (Views itself is enabled for Fuchsia:
  [build/config/ui.gni](https://chromium.googlesource.com/chromium/src/+/main/build/config/ui.gni)).
- **"target_os=linux with a musl sysroot" is viable as the starting
  point.** Alpine does exactly that: `is_musl=true`, `use_sysroot=false`,
  the "unbundle" toolchain, `use_custom_libcxx=true`, system clang 23
  ([APKBUILD](https://gitlab.alpinelinux.org/alpine/aports/-/blob/master/community/chromium/APKBUILD),
  `_configure`). `is_musl` is not upstream yet; it comes from a shared
  patch set (copium's
  [cr147-is-musl-libcxx](https://codeberg.org/selfisekai/copium/src/branch/main/cr147-is-musl-libcxx.patch),
  "Introduce is_musl build arg, indicate musl to libc++").
  Chromium tracks musl in a metabug
  ([issue 40244828](https://issues.chromium.org/issues/40244828)).
- **Alpine's patches.** 20 local patches plus 23 from
  [copium](https://codeberg.org/selfisekai/copium) (shared by several
  distributions). By subject (all from apk):
  - **musl itself, about 10, all small (14 to 130 lines)**: missing
    execinfo.h (backtraces), `mallinfo`, `res_ninit`/`res_nclose`,
    `TEMP_FAILURE_RETRY`, `pvalloc`; `fstatat64` in
    linux_syscall_support; libc vs kernel header clashes (`prctl`,
    `dev_t`, `__BEGIN_DECLS`); libc++ told it is on musl; fortify
    false positives (Skia, Blink, SwiftShader).
  - **musl and the seccomp sandbox, 4**: musl's different system calls
    (`musl-sandbox.patch`), thread id caching, `tkill` for V8's profiler,
    `CLONE_SETTLS`. None matter with Jam OS's own sandbox.
  - **DNS, 1**: `disable-dns_config_service.patch` builds Fuchsia's stub
    DNS config service in place of Linux's (which needs `res_ninit`), so
    names go through `getaddrinfo`. A precedent for borrowing a Fuchsia
    file into a Linux build.
  - **The rest**: unbundling system libraries, newer Rust/TypeScript/node,
    tests. Not needed if we build Chromium's bundled copies.

## 2. Process launching

- **`base::LaunchProcess` on Linux is plain `fork()` then `execvp()`**
  ([base/process/launch_posix.cc](https://chromium.googlesource.com/chromium/src/+/main/base/process/launch_posix.cc)).
  With `clone_flags` set (only the namespace sandbox) it uses `clone`
  through `ForkWithFlags`. No `posix_spawn` path on Linux; macOS's
  launcher is `posix_spawn` throughout
  ([base/process/launch_mac.cc](https://chromium.googlesource.com/chromium/src/+/main/base/process/launch_mac.cc)).
- **What the child does between fork and exec** (same file): clear
  `FD_CLOEXEC` on chosen descriptors, open `/dev/null` and `dup2` it to
  stdin, `setpgid`, `getrlimit`/`setrlimit`, reset every signal handler
  and the mask, swap `environ`, shuffle descriptors with `dup2`, close
  the rest (by listing `/proc/self/fd`, or looping to the descriptor
  limit if that fails), `prctl(PR_SET_NO_NEW_PRIVS)` and
  `PR_SET_PDEATHSIG`, `chdir`, an optional `pre_exec_delegate`, then
  `execvp`. `GetAppOutput` has a second, smaller fork-exec in the same
  file.
- **The executable is `/proc/self/exe`.** Children are started by
  exec'ing the literal path `/proc/self/exe` (`CHILD_ALLOW_SELF`,
  [content/browser/child_process_host_impl.cc](https://chromium.googlesource.com/chromium/src/+/main/content/browser/child_process_host_impl.cc));
  `--browser-subprocess-path` overrides it.
- **`--no-zygote` avoids fork-without-exec**: children then go through
  `base::LaunchProcess`
  ([content/browser/child_process_launcher_helper_linux.cc](https://chromium.googlesource.com/chromium/src/+/main/content/browser/child_process_launcher_helper_linux.cc)),
  and the switch's own comment says `--no-sandbox` must go with it
  ([content/public/common/content_switches.cc](https://chromium.googlesource.com/chromium/src/+/main/content/public/common/content_switches.cc);
  background: [docs/linux/zygote.md](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/zygote.md)).
  `--single-process` launches no children at all (same switches file).
- **Does an emulated `vfork` (record descriptor changes until exec)
  cover every path?** Mostly, with care:
  - It covers `LaunchProcess` and `GetAppOutput` **only if it records
    more than descriptors**: also `sigaction`, the signal mask,
    `setpgid`, `setrlimit`, `prctl`, `chdir` and `environ`, because in a
    shared-memory child those would otherwise change the parent. The
    `/proc/self/fd` listing must show the child's recorded table.
  - Chromium calls `fork()`, not `vfork()`: clang knows `vfork` returns
    twice but not `fork` (*unsure*: whether a `fork` emulated with
    setjmp is miscompiled here; the child writes its own stack, e.g.
    `fd_shuffle1`). The safer form: patch the two call sites to call
    `vfork`, or give Jam OS a `posix_spawn` branch modelled on macOS's.
  - **Not covered**: the zygote (avoid with `--no-zygote`); Crashpad's
    launcher, which forks, calls `setsid`, then `posix_spawn`s the
    handler and `_exit`s
    ([crashpad util/posix/spawn_subprocess.cc](https://chromium.googlesource.com/crashpad/crashpad/+/main/util/posix/spawn_subprocess.cc)):
    leave crash reporting off, or patch it to one `posix_spawn` with
    `POSIX_SPAWN_SETSID`; `pre_exec_delegate` (sandbox only).

## 3. IPC

- **Mojo's Linux channel is a `socketpair(AF_UNIX, SOCK_STREAM)`**
  ([mojo/public/cpp/platform/platform_channel.cc](https://chromium.googlesource.com/chromium/src/+/main/mojo/public/cpp/platform/platform_channel.cc))
  carried by `sendmsg`/`recvmsg` with `SCM_RIGHTS`, `MSG_NOSIGNAL`,
  `MSG_DONTWAIT` and `MSG_CTRUNC` checked
  ([mojo/public/cpp/platform/socket_utils_posix.cc](https://chromium.googlesource.com/chromium/src/+/main/mojo/public/cpp/platform/socket_utils_posix.cc)),
  plus `shutdown(SHUT_RDWR)`
  ([mojo/core/channel_posix.cc](https://chromium.googlesource.com/chromium/src/+/main/mojo/core/channel_posix.cc)).
  The child gets its end as an inherited descriptor named on its command
  line. Mojo now runs on ipcz
  ([third_party/ipcz/README.md](https://chromium.googlesource.com/chromium/src/+/main/third_party/ipcz/README.md)),
  still over these channels. Only socketpairs are needed: no named
  `AF_UNIX` sockets and no loopback.
- **An optional Linux fast path** adds a memfd ring with seals, an
  `eventfd2` or futex for wake-ups, all by raw `syscall()`
  ([mojo/core/channel_linux.cc](https://chromium.googlesource.com/chromium/src/+/main/mojo/core/channel_linux.cc));
  M13's call table answers raw `syscall()` too
  ([M13-PLAN.md](M13-PLAN.md#1-how-musl-meets-jam-os)), so each number
  used needs an entry or a clean `-ENOSYS`.
- **Shared memory** is `memfd_create` (`MFD_CLOEXEC | MFD_ALLOW_SEALING`,
  then `MFD_NOEXEC_SEAL` if allowed), `ftruncate`, `F_ADD_SEALS`
  (shrink, grow, seal), and a **read-only twin made by reopening
  `/proc/self/fd/N` with `O_RDONLY`**; without memfd it falls back to an
  unlinked file in `/dev/shm`
  ([base/memory/platform_shared_memory_region_posix.cc](https://chromium.googlesource.com/chromium/src/+/main/base/memory/platform_shared_memory_region_posix.cc)).
  Jam OS's libc can answer the reopen by duplicating the VMO handle
  without write rights.
- **D-Bus** is a build option (`use_dbus`, on for Linux:
  [build/config/features.gni](https://chromium.googlesource.com/chromium/src/+/main/build/config/features.gni));
  `content_shell` doesn't need it (*unsure* that `use_dbus=false` still
  builds cleanly on Linux). `use_udev` and `use_gio` sit beside it.
- **Wayland** needs the same descriptor passing; Chromium builds its own
  libwayland (`use_system_libwayland = false`,
  [third_party/wayland/features.gni](https://chromium.googlesource.com/chromium/src/+/main/third_party/wayland/features.gni)),
  so G2's option (A), the shim in libc ([G2-PLAN.md](G2-PLAN.md#1-the-shim-libwayland-on-channels)),
  serves it unchanged, while an override in a libwayland port would not.

## 4. /proc and other Linux-only reads

- **Required:**
  - `readlink("/proc/self/exe")` for the executable's own path:
    failure is `NOTREACHED` (fatal)
    ([base/base_paths_posix.cc](https://chromium.googlesource.com/chromium/src/+/main/base/base_paths_posix.cc));
    and exec of the path `/proc/self/exe` (section 2).
  - `/proc/self/fd/N` reopen for read-only shared memory (section 3),
    unless the `/dev/shm` fallback works instead.
- **Optional, fails soft:**
  - `/proc/self/fd` listing (falls back to closing every descriptor up to
    the limit: section 2).
  - `/proc/self/maps`: only V8's `RemapPages` fallback after `mremap`
    (it returns false and V8 copies)
    ([v8 src/base/platform/platform-linux.cc](https://chromium.googlesource.com/v8/v8/+/main/src/base/platform/platform-linux.cc)).
  - `/proc/self/stat`, `/proc/stat`, `/proc/<pid>/...` for process and
    CPU metrics
    ([base/process/internal_linux.cc](https://chromium.googlesource.com/chromium/src/+/main/base/process/internal_linux.cc));
    `/proc/cpuinfo` and `/sys/devices/...` for the CPU and machine names
    ([base/system/sys_info_linux.cc](https://chromium.googlesource.com/chromium/src/+/main/base/system/sys_info_linux.cc)).
    *Unsure*: each caller handles a missing file; the safe answer is
    `ENOENT`, never a half-filled file.
  - `/proc/sys/...` and `/proc/self/task`: the Linux sandbox only.
- **Netlink**: the network change notifier watches addresses with a
  `NETLINK_ROUTE` socket (*unsure*: that `socket(AF_NETLINK)` failing is
  only logged; not checked in this pass).

## 5. Dynamic loading in a static, non-component build

- **ANGLE links statically by default** (`use_static_angle = true`,
  [ui/gl/features.gni](https://chromium.googlesource.com/chromium/src/+/main/ui/gl/features.gni)).
  With `--disable-gpu`, compositing is software and nothing GL is
  loaded (*unsure*: no GL load is attempted at all on Wayland in that
  mode).
- **SwiftShader is no longer a WebGL fallback** without
  `--enable-unsafe-swiftshader`
  ([intent to remove](https://groups.google.com/a/chromium.org/g/blink-dev/c/yhFguWS_3pM/m/oLrB5up_BwAJ);
  [docs/gpu/swiftshader.md](https://chromium.googlesource.com/chromium/src/+/main/docs/gpu/swiftshader.md)),
  so software rendering means no WebGL, which is fine for CR's first
  goal. `enable_swiftshader=false`, `use_dawn=false` (same gni).
- **What does `dlopen` on Linux, and its switch:**
  - libva: `use_vaapi=false`
    ([media/media_options.gni](https://chromium.googlesource.com/chromium/src/+/main/media/media_options.gni));
  - PulseAudio is loaded through stubs unless `link_pulseaudio`:
    `use_pulseaudio=false` (same gni);
  - libudev: `use_udev=false`; GTK/Qt: `use_gtk=false`, `use_qt6=false`
    ([build/config/linux/gtk/gtk.gni](https://chromium.googlesource.com/chromium/src/+/main/build/config/linux/gtk/gtk.gni));
  - libgbm: `use_wayland_gbm=false`
    ([ui/ozone/platform/wayland/wayland.gni](https://chromium.googlesource.com/chromium/src/+/main/ui/ozone/platform/wayland/wayland.gni));
  - NSS (which loads its own modules): `use_nss_client_certs=false`,
    `use_nss_server_certs=false`
    ([crypto/features.gni](https://chromium.googlesource.com/chromium/src/+/main/crypto/features.gni));
    Cast builds already turn them off. *Unsure*: that Linux builds
    cleanly with both off.
- **Bundled and statically linked**: fontconfig (`use_bundled_fontconfig`,
  [third_party/fontconfig/fontconfig.gni](https://chromium.googlesource.com/chromium/src/+/main/third_party/fontconfig/fontconfig.gni)),
  FreeType (`use_system_freetype = false`,
  [build/config/freetype/freetype.gni](https://chromium.googlesource.com/chromium/src/+/main/build/config/freetype/freetype.gni)),
  HarfBuzz, libwayland, ICU, BoringSSL, libc++.
- **NaCl and PPAPI are gone from main** (their directories return 404:
  `components/nacl`, `ppapi`), so there is nothing to turn off there.
- **Conclusion**: M14.5 is not on the path for software rendering. It
  comes back with G3/G4: Mesa's drivers and the Vulkan loader load
  shared objects (*unsure* how much of that can be static).

## 6. V8's JIT

- **`--jitless`** stops runtime allocation of executable memory; it
  turns off Sparkplug, Maglev and Turbofan and makes regular
  expressions interpreted
  ([v8 src/flags/flag-definitions.h](https://chromium.googlesource.com/v8/v8/+/main/src/flags/flag-definitions.h)).
  **WebAssembly is unavailable** in jitless mode
  ([V8 blog: jitless](https://v8.dev/blog/jitless)) unless V8 is built
  with the experimental DrumBrake interpreter (`v8_enable_drumbrake`,
  `--wasm-jitless`, same flags file). JavaScript runs several times
  slower in compute-heavy code (the blog's numbers).
- **With the JIT, V8 on x64 wants RWX code pages.** Its write scope is
  a no-op except on Apple's MAP_JIT and Intel PKU, and PKU "only
  achieves write protection" because execute can't be taken away
  ([v8 src/common/code-memory-access.h](https://chromium.googlesource.com/v8/v8/+/main/src/common/code-memory-access.h));
  the code range is set `kReadWriteExecute`
  ([v8 src/heap/code-range.cc](https://chromium.googlesource.com/v8/v8/+/main/src/heap/code-range.cc)).
  That collides with Jam OS's W^X everywhere. Options for CR's JIT
  layer: RWX pages under a capability, perhaps guarded by PKU (the PC
  has PKU: [HARDWARE.md](HARDWARE.md)) through `pkey_alloc` and
  `pkey_mprotect`; or a V8 patch for dual mapping (*unsure*: no
  dual-mapping mode exists on Linux today).
- **Memory calls V8 makes** ([v8 src/base/platform/platform-posix.cc](https://chromium.googlesource.com/v8/v8/+/main/src/base/platform/platform-posix.cc)):
  `mmap` `PROT_NONE` with `MAP_NORESERVE` for reservations, `MAP_FIXED`
  inside them, `mprotect` between none/R/RW/RX/RWX, `madvise`
  (`MADV_DONTNEED`, `MADV_FREE`), decommit by `mmap` `MAP_FIXED`
  `PROT_NONE`, optional `mremap(MREMAP_DONTUNMAP)` and `mseal`. No memfd
  except in tests.
- **Big reservations**: the V8 sandbox reserves 1 TB on x64 plus
  guards, falling back to a "partially reserved" sandbox of at least
  8 GB, and pointer compression wants a 4 GB cage
  ([v8 include/v8-internal.h](https://chromium.googlesource.com/v8/v8/+/main/include/v8-internal.h),
  [v8 src/sandbox/sandbox.cc](https://chromium.googlesource.com/v8/v8/+/main/src/sandbox/sandbox.cc));
  PartitionAlloc reserves multi-GiB pools up front
  ([partition_address_space.h](https://chromium.googlesource.com/chromium/src/+/main/base/allocator/partition_allocator/src/partition_alloc/partition_address_space.h);
  *unsure* of the exact `kPoolMaxSize`). Jam OS's address space is one
  flat VMAR and a paged VMO tops out at 64 GiB (INVENTORY), so
  reservations must cost nothing until touched and accept `MAP_FIXED`
  maps inside them: kernel work, close to G2's
  [Q12](G2-PLAN.md#q12-big-sparse-memory-pools).
- **Signals**: the WebAssembly trap handler catches `SIGSEGV` and
  rewrites the faulting context; V8's CPU profiler interrupts threads
  with `tkill` (Alpine's `musl-conditional-tkill.patch`). Jam OS's
  signal-thread model ([M13-PLAN.md](M13-PLAN.md)) can do neither
  (*unsure* how M13 delivers faults), so: jitless (no Wasm), and the
  DevTools profiler off.

## 7. The sandbox

- **`--no-sandbox` drops** the setuid or user-namespace sandbox, the
  PID and network namespaces, and seccomp-BPF filters per process type
  ([sandbox/linux/README.md](https://chromium.googlesource.com/chromium/src/+/main/sandbox/linux/README.md),
  [docs/linux/suid_sandbox_development.md](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/suid_sandbox_development.md)),
  and with it the brokered file access and zygote. Renderers then open
  files directly; fonts still come from the browser over Mojo
  ([docs/linux/sandbox_ipc.md](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/sandbox_ipc.md)).
- **Where a new OS's sandbox plugs in**: the process types and the
  per-OS policy layer in sandbox/policy
  ([sandbox/policy/README.md](https://chromium.googlesource.com/chromium/src/+/main/sandbox/policy/README.md),
  [sandbox/policy/sandbox_type.h](https://chromium.googlesource.com/chromium/src/+/main/sandbox/policy/sandbox_type.h)).
  Fuchsia's version is the model for Jam OS: per process type, which
  services and directories the child's namespace gets
  ([sandbox/policy/fuchsia/sandbox_policy_fuchsia.cc](https://chromium.googlesource.com/chromium/src/+/main/sandbox/policy/fuchsia/sandbox_policy_fuchsia.cc)).
  On the Linux route the closest hook is a spawn attribute that sets
  the child's namespace and grants at launch (M13's `posix_spawn`
  grants), chosen per type, while Chromium still runs as if
  `--no-sandbox`. *Unsure*: how much the Linux sandbox code must be
  compiled out versus left unused.

## 8. The build

- **Compiler**: clang only, libc++ only
  ([docs/linux/build_instructions.md](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/build_instructions.md)).
  Chromium ships a prebuilt clang (tools/clang/scripts) and builds its
  own libc++ and libc++abi (`use_custom_libcxx`, statically linked); Alpine
  uses system clang 23 through `custom_toolchain` and
  `clang_use_chrome_plugins=false`. A custom sysroot is a GN argument
  (`use_sysroot=false` plus flags, or `target_sysroot`) (*unsure* which
  is cleaner for a non-Linux sysroot). So the C++ runtime Chromium needs
  comes with it; for other ports G2's
  [Q6](G2-PLAN.md#q6-text-shaping-and-a-c-runtime) leans libstdc++,
  which Chromium can't use: libc++ (its option C) would serve both.
- **Rust is required** (`enable_rust`; Alpine builds the Rust standard
  library for its own target triple, apk `_prepare`). Jam OS needs
  `x86_64-unknown-linux-musl` std built against its sysroot; Rust's std
  calls musl, so it lands in M13's call table (*unsure*: no raw
  `syscall` instructions in std on that target).
- **Headers**: the sysroot needs Linux's UAPI headers (Alpine's
  `linux-headers`): Chromium includes `linux/futex.h`, `linux/memfd.h`
  and the like (channel_linux.cc above).
- **Static linking**: Linux builds link dynamically against libc
  (Alpine's binary depends on musl's `libc.so`); a fully static PIE
  needs link-flag changes (*unsure* how many).
- **Host**: Chromium's Linux target builds on a Linux host; the PC's
  WSL2 is the natural place. Docs ask for 100 GB of free disk and 16 GB+
  of RAM (above). Reports: about 2 hours on a 24-core Core Ultra 9 285K,
  2.5 hours on a 12-core Ryzen 3900X, both full browser builds
  ([FreeBSD forum](https://forums.freebsd.org/threads/insights-from-building-ungoogled-chromium-on-a-modern-desktop-machine.102701/)).
  Guess for the i7-14700 (28 threads, 32 GB): **3 to 4 hours for
  `content_shell` from scratch**, less with `symbol_level=0` (*unsure*);
  WSL2's default memory cap (half the RAM) should be raised or the job
  count lowered for the links. Disk: about 40 GB of source without
  history plus 10 to 20 GB per output directory (*unsure*).
- **Lean GN args for the first build** (names checked in the files
  linked above; the combination is *unsure* until tried):

  ```
  target_os="linux" target_cpu="x64" is_debug=false is_component_build=false
  is_official_build=false symbol_level=0 is_musl=true use_sysroot=false
  use_custom_libcxx=true treat_warnings_as_errors=false
  ozone_platform="wayland" ozone_platform_wayland=true ozone_platform_x11=false
  ozone_platform_headless=true use_wayland_gbm=false
  use_gtk=false use_qt6=false use_glib=false use_dbus=false use_udev=false
  use_gio=false use_vaapi=false use_pulseaudio=false use_alsa=false
  use_cups=false enable_pdf=false enable_printing=false
  enable_swiftshader=false use_dawn=false
  use_nss_client_certs=false use_nss_server_certs=false use_kerberos=false
  proprietary_codecs=false
  ```
- **`content_shell` first**
  ([content/shell/BUILD.gn](https://chromium.googlesource.com/chromium/src/+/main/content/shell/BUILD.gn)):
  it has no profile manager, extensions, sync, process singleton or
  Views browser UI; `headless_shell` (Alpine ships it) is a second
  small target. The order: `--single-process --ozone-platform=headless`
  (no IPC, no window), then `--ozone-platform=wayland`, then
  multi-process (`--no-zygote --no-sandbox`), then the `chrome` target.

## 9. Other runtime dependencies

- **Fonts**: bundled fontconfig, FreeType and HarfBuzz (section 5), so
  G2's font ports aren't needed for Chromium itself; it needs font files
  and a fonts.conf on disk, and fontconfig's cache directory
  (G2's notes on its locks and `mmap` apply,
  [G2-PLAN.md](G2-PLAN.md#4-fonts)).
- **Audio**: Linux picks CRAS, PulseAudio, then ALSA, or a fake manager
  with `--disable-audio-output`
  ([media/audio/linux/audio_manager_linux.cc](https://chromium.googlesource.com/chromium/src/+/main/media/audio/linux/audio_manager_linux.cc),
  66 lines). A Jam OS back end is an `AudioManager` plus an output stream:
  Fuchsia's pair is about 600 lines
  ([manager](https://chromium.googlesource.com/chromium/src/+/main/media/audio/fuchsia/audio_manager_fuchsia.cc),
  [stream](https://chromium.googlesource.com/chromium/src/+/main/media/audio/fuchsia/audio_output_stream_fuchsia.cc)),
  so a mixer back end is a few hundred lines in Chromium's tree; output
  only at first.
- **TLS**: BoringSSL, and on Linux certificates are verified only by the
  built-in verifier with the **Chrome Root Store compiled in**
  (`chrome_root_store_only` is true for Linux,
  [net/features.gni](https://chromium.googlesource.com/chromium/src/+/main/net/features.gni);
  the roots: [net/data/ssl/chrome_root_store](https://chromium.googlesource.com/chromium/src/+/main/net/data/ssl/chrome_root_store/root_store.textproto)).
  NSS only adds user-installed certificates
  ([docs/linux/cert_management.md](https://chromium.googlesource.com/chromium/src/+/main/docs/linux/cert_management.md)).
  **No system CA store is needed** for Chromium (other ports still want
  one).
- **DNS**: Alpine's way, the system resolver through `getaddrinfo` on
  worker threads, which M13 sends to `/svc/dns` (A records only). The
  built-in async DNS and DNS-over-HTTPS need a DNS config service; the
  Fuchsia stub swap above turns that off.
- **Network limits**: one network-service process holds every socket.
  netstack's 16 sockets and 64 connections per opener (INVENTORY) will
  be hit by ordinary browsing (Chromium allows 6 per host, 256 in all
  by default; *unsure* of today's numbers).
- **Files**:
  - atomic rename-over and `fsync` for its important files
    ([base/files/important_file_writer.cc](https://chromium.googlesource.com/chromium/src/+/main/base/files/important_file_writer.cc));
  - SQLite with `fcntl` locks and **`mmap` of the database on by
    default** (`mmap_enabled_ = true`,
    [sql/database.h](https://chromium.googlesource.com/chromium/src/+/main/sql/database.h)):
    SQLite falls back to `read` when `mmap` fails, so a file mapping
    must be coherent with `write` (M12.5's PF2) or fail; **a copy that
    pretends to be `MAP_SHARED` would corrupt databases**;
  - read-only `MAP_SHARED` mappings of ICU data, the V8 snapshot and
    `.pak` resources
    ([base/files/memory_mapped_file_posix.cc](https://chromium.googlesource.com/chromium/src/+/main/base/files/memory_mapped_file_posix.cc));
    `icu_use_data_file=false` (Alpine sets it) and
    `v8_use_external_startup_data=false` remove two of them (*unsure*
    for the paks);
  - symlinks and a named `AF_UNIX` socket for the `chrome` target's
    process singleton
    ([chrome/browser/process_singleton_posix.cc](https://chromium.googlesource.com/chromium/src/+/main/chrome/browser/process_singleton_posix.cc)),
    not for `content_shell`;
  - inotify for file watchers
    ([base/files/file_path_watcher_inotify.cc](https://chromium.googlesource.com/chromium/src/+/main/base/files/file_path_watcher_inotify.cc);
    *unsure* which watchers are fatal when it fails); extended
    attributes only for download metadata (*unsure*).
- **Threads**: `gettid`, futexes by raw `syscall()` (PartitionAlloc,
  Mojo), `prctl(PR_SET_NAME)`, per-thread `setpriority`,
  `sched_setaffinity`, `CLOCK_THREAD_CPUTIME_ID`; all through musl or
  M13's raw call table (*unsure* of the full list: a first run with
  every `-ENOSYS` logged is the way to get it).
- **Inline system calls**: linux_syscall_support.h issues the
  `syscall` instruction directly, which on Jam OS would run a Jam OS
  call with Linux's arguments. Seen in the sandbox and breakpad (both
  off here) (*unsure* there are no others: the build should
  check for the instruction in the final binary).

## 10. The revised layer list

In order; sizes are guesses (*unsure*) in lines of new code. Items
marked **new** are not in any plan yet.

| # | Layer | What | Size |
|---|---|---|---|
| 1 | M13 finished, M12.5 PF2/PX, M12.7 stage 2 | already planned; PX matters more than thought: every child execs the same 150 to 250 MB binary (*unsure* of the size), so text must be demand-paged and shared, and `allow`'s hash of it cached | as planned |
| 2 | M13.6, Unix filesystem | rename-over, locks, `fsync`, symlinks (for `chrome` later) | as planned |
| 3 | **Toolchain (new, CR1)** | clang and lld for x86_64 linux-musl over Jam OS's sysroot, Linux UAPI headers, compiler-rt, libc++ (Chromium's own), Rust std for the musl target, GN args, a WSL2 build recipe for M13.7 | 1-2k lines of build glue and patches |
| 4 | **Address space (new, kernel and libc)** | free `PROT_NONE` reservations of GiBs to a TB, `MAP_FIXED` inside them, `madvise` decommit, `mprotect` R/RW/RX changes | 1-3k, mostly kernel |
| 5 | **libc additions (new)** | `/proc/self/exe` (readlink and exec), `/proc/self/fd` (list, reopen read-only), memfd seals, read-only `MAP_SHARED` file maps (F1), the raw-call entries Chromium uses, `ENOENT` for the rest of `/proc` | 1-2k |
| 6 | **`AF_UNIX` with `SCM_RIGHTS` (new)** | socketpairs carrying bytes and descriptors, `MSG_DONTWAIT`, `MSG_CTRUNC`, `shutdown`; named sockets later for `chrome` | 1.5-3k |
| 7 | **Launching** | M13.5's emulated `vfork` with a full record of the child's calls, or a `posix_spawn` branch in Chromium's launcher (about 300 lines, macOS's shape); `--no-zygote` | 0.5-1.5k |
| 8 | G2 for Chromium | the libc Wayland socket (G2's A), `wl_subcompositor` (Chromium binds it; `wl_compositor`, `wl_shm` and `xdg_wm_base` are its hard requirements: [wayland_connection.cc](https://chromium.googlesource.com/chromium/src/+/main/ui/ozone/platform/wayland/host/wayland_connection.cc)), popups, clipboard, cursors; Chromium brings its own libwayland, xkbcommon and fonts code | compositor 2-4k |
| 9 | CR2: content_shell | the musl patches (Alpine's), Jam OS's GN args, `--single-process` headless, then Wayland, then multi-process | 1-3k of patches |
| 10 | Network limits | netstack's per-opener socket caps raised | small |
| 11 | Audio back end | a Jam OS `AudioManager` over the mixer | 0.5-1k |
| 12 | V8's JIT | RWX under a capability (PKU-guarded?) or stay jitless; Wasm with it | kernel small, design large |
| 13 | Jam OS's sandbox | per-type namespaces and grants at spawn, modelled on Fuchsia's policy | 1-2k |
| 14 | `chrome` | the full browser: process singleton (symlinks, named `AF_UNIX`), profile, Views | patches |

Not on the path: M14.5 (until G3/G4), a CA store, D-Bus, NSS,
SwiftShader, a fork for the zygote.

## The biggest risks

1. **Address space**: V8 and PartitionAlloc reserve huge sparse ranges
   up front; Jam OS's flat VMAR and 64 GiB VMOs may not express that
   cheaply. A kernel change, not a libc one.
2. **One big static binary per process**: without shared, demand-paged
   text (M12.5 PX) each renderer costs its whole binary in RAM and load
   time.
3. **W^X versus V8's JIT**: jitless is fine to start, but real speed and
   WebAssembly need RWX or a V8 change.
4. **The build itself**: a non-Linux sysroot under a `target_os="linux"`
   build, static linking, Rust std for it, 3 to 4 hours per clean build
   on the PC; every musl patch has to be rebased with each Chromium
   release (Alpine carries them every release, apk).
5. **Hidden Linux assumptions** found only at run time: raw
   `syscall()` numbers, inline `syscall` instructions, `/proc` files,
   netlink, signals into a specific thread. A logging `-ENOSYS` and a
   check for the instruction in the binary turn them into a list.
6. **fork semantics**: an emulated `vfork` standing in for `fork()` in
   code compiled as `fork()` (section 2); patching Chromium's launcher to
   `posix_spawn` is the fallback.
