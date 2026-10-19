# SANDBOX plan: Jam OS's sandbox for Chromium (CR-f)

Chromium runs `--no-sandbox` on Jam OS (the owner's choice for the first
builds; [CR-PLAN](CR-PLAN.md) section 13). Its own launcher, unpatched,
forks the browser and has the child exec `/proc/self/exe` in place
([CB5 notes](CR-PLAN.md#cb5-notes-2026-10-19), [FORK-PLAN section
2](FORK-PLAN.md#2-execve-in-a-forked-child)), and `exec_replace` in
`user/posix/sys/spawn.c` gives the new image the forked child's whole
namespace (`NS_ALL`), its own job without limits, the boot image, its own
image and, for `/proc/self/exe`, the JIT power. So every renderer, the GPU
process and every utility process holds what the browser's list grants:
`/data` or `/jam` read-write, `/svc/net`, `/svc/dns`, `/svc/wayland`,
`/svc/audio`, `right jit` ([CR3's table](CR-REVIEW-LAYERS.md#what-the-sandbox-cr-f-must-close)
lists each thing and who needs it). Until that changes, the owner's rule
holds: ordinary sites only, not banking or logins that matter. The owner
(2026-10-20) moved CR-f before the iGPU work. This plan is the design;
of it only SB2 is built ([its notes](#sb2-notes)).

**In one paragraph.** Chromium keeps believing it runs without a sandbox,
and Jam OS narrows each child where the exec in place already cuts
everything away: libc's `execve` of `/proc/self/exe`, in the forked child,
reads the child's type from its command line (`--type`,
`--utility-sub-type`, `--use-angle`), looks it up in a **children's
table** that the program carries beside its list (a second ELF note,
approved with the program by `allow`), and builds the new image with that
row's grants only: a renderer gets the program's own files read-only and
nothing else, no service, no process creation, the JIT power, and its
terminal descriptors on the log. Before its exec notice the child asks its
parent once to put the row's limits (pages, handles, threads, message
bytes) on its job, so a renderer that eats memory dies alone. No Chromium
patch (option (a), the owner's "layers, not patches"). Two kernel pieces
close CJR2's two JIT findings: a process flag that a process giving up the
JIT can never lose and that passes to every process it makes (L3), and an
"armed" unstarted process with a replace-only right (FORK-PLAN's FX
half), so a small keeper process that libc forks before `main` can build
the renderers' images with the power while the browser gives it up (L2).
CR3's three Mediums (shared service openers, socket files as bearer keys,
fontconfig's shared settings) become tracks of their own; the fonts one
should run now, ahead of CR-f.

**Done when** (CR-PLAN's CR-f row, made concrete):
1. Each of Chromium's children runs in a job of its own with its type's
   limits, and holds only its row's grants: checked from outside on real
   `content_shell` and `chrome` processes by the kernel's handle listing
   (section 9), not by the children's own word.
2. A hostile test child of each type (section 9) fails to open `/jam`,
   `/data`, `/tmp` and `/boot`, to reach netstack, the resolver, the
   compositor, the mixer, the broker and init, to start a process by any
   call, to map W+X memory (but for the rows that hold the power), and to
   take more than its limits; a renderer that eats memory is killed at its
   job's limit, the browser and the other tabs go on, and the system's
   free memory never falls below the reserve.
3. The browser holds no JIT power after its start; renderers and the
   SwiftShader GPU process hold it; a process that gave it up can't get it
   back by any start, `/svc/exec`'s included.
4. Every `cr-test.sh` case passes with the sandbox on (headless, shell-wl,
   multi, jit, webgl, sound, fonts, and `chrome`'s from CB6), and the owner's
   PC check browses real sites as before.
5. An independent review-and-fix of the whole (SBR) finds no open High or
   Medium; then the owner decides whether the "ordinary sites only" rule
   lifts (Q8).

## What this plan is not

- **Not Chromium's Linux sandbox.** No seccomp-bpf, user namespaces,
  setuid helper or zygote: Jam OS has no ambient authority for them to take
  away (a process reaches what its handles reach), and `--no-zygote` stays.
  Chromium's sandbox code stays compiled and unused under `--no-sandbox`.
- **Not a launcher patch.** Section 5's `posix_spawn` branch was dropped
  for the unpatched launcher (FORK-PLAN, the owner 2026-10-16), and section
  13's "Jam OS's launcher branch applies a per-type table at spawn" became
  libc's exec doing it (section 2).
- **No per-site policy.** Chromium chooses which site goes in which
  renderer (site isolation is Chromium's); Jam OS gives every renderer the
  same row. A site that may not JIT is the browser's `--js-flags=--jitless`,
  which patch 0914 already honours (CJR2 finding 1).
- **Not the GPU itself.** The iGPU's render service is
  [GPU-PLAN](GPU-PLAN.md)'s; this plan gives the GPU process rows that take
  it with one grant (section 6).
- **Not memory accounting inside services.** What the browser, netstack or
  the mixer hold on a child's behalf stays theirs (section 4).
- **For our own Chromium build only.** No BSD runs Google's branded
  Chrome natively; FreeBSD's users run Linux Chrome through the Linux
  binary layer for Widevine, which the owner rules out ("against running
  Linux binaries natively, not against rebuilding Linux code"). So the
  table may rely on our build's facts: `--no-zygote`, `--no-sandbox`,
  patches 0913 to 0916, the package's place.

## Where it starts

| Piece | Built by | What this plan does with it |
|---|---|---|
| Fork with a job per child, `SIGKILL` its `job_kill`, the parent's table holding `RIGHT_MANAGE` (`user/posix/sys/fork.c`) | FL1b, FJ1 | the job that gets the row's limits |
| Exec in place: build, then `process_replace`, which closes every handle but the startup message's (`exec_replace`, `exec_handover` in `user/posix/sys/spawn.c`) | FK3, FL3 | the place the row is applied: CR3 found the cut clean |
| Grants: `posix_spawnattr_setgrants_np`, the grants words of `<os.h>` (`/data:r`, a service by name), views made at the send | M13, PO5 | the row's grants, the same words |
| Narrowed children get the current directory as text, never the channel | DL2 | the row's children start in `/` as text |
| The program's list (`<wants.h>`, `user/lib/wants.c`, `tools/checkwants.py`), approval by `allow`, the starters' job (`dp_job_create`, `<dataprog.h>`) | M8.6, CE1, MB | a second note beside the list; `allow` shows both |
| `SR_JIT`, a root narrowed to the JIT power; `PR_SET_MDWE` closing it (`user/posix/sys/prctl.c`, `user/lib/start.c`); patch 0914 (renderers keep it, and the SwiftShader GPU process) | CR-e, SWS, CJR2 | the row says who gets it; the kernel flag (L3) and the keeper (L2) |
| Jobs with four counted limits, a page fault that can't be paid for killing the process (`kernel/object/job.c`); libc reports such a death as `SIGKILL` (`user/posix/sys/sigfault.c`) | M5, M12 | per-type limits; Chromium sees Linux's OOM kill |
| Child notices on the signal channel, read by the parent's signal thread (`abi/idl/signal.idl`, `user/posix/sys/sigthread.c`) | FL4 | one more notice: "set my limits" |
| `/svc/unix`'s opener per program (`own_channel`, `user/lib/ns.c`) | CR2F | the same for net, dns and audio (CR3's M1) |
| The freeze that a writer may wait behind ([FR1's design question 1](FORK-REVIEW-KERNEL.md#design-questions)) | FZ (running) | closed for everyone by FZ; sandboxed children can't make freezes (no process creation) |
| Kernel debug listings (`kernel/debug/dbgcmd.c`, the shell's `ps -t`) | TD | a handle listing with each channel's peer: the outside check |

## How others do it

| System | The model | What it suggests here |
|---|---|---|
| **Fuchsia** (read in Chromium 155: `sandbox/policy/fuchsia/sandbox_policy_fuchsia.cc`) | The browser's launcher builds each child's namespace at launch from a per-type config: a filtered `/svc` (a minimal list plus the type's: fonts, the VMEX resource, sysmem for renderers; Flatland, sysmem, the Vulkan loader for the GPU; sockets and name lookup for the network), `/pkg` read-only for every sandboxed child, `/config/ssl` for the network service, the GPU's device paths, a clean environment, and a new job for each child | The shape: an explicit capability list per type at launch, nothing ambient, `/pkg` read-only to all. Note that Fuchsia's browser holds the VMEX resource it hands renderers: CJR2's L2 exists there too |
| **Linux** (155: `sandbox/policy/linux/sandbox_linux.cc`) | Each child applies its own sandbox: the zygote's namespaces, then seccomp-bpf in `SandboxLinux::InitializeSandbox`, which renderers (`renderer_main_platform_delegate_linux.cc`), utilities (`utility_main.cc`) and the GPU process (`gpu_main.cc`) call after their start-up files are open; with `--no-zygote`, as Jam OS runs it, the launcher is a plain fork and exec (`child_process_launcher_helper_linux.cc`) | The place a hook (option (b)) would go, and the two phases a hook allows: open the files, then drop |
| **Capsicum** (Watson, Anderson, Laurie and Kennaway, "Capsicum: practical capabilities for UNIX", USENIX Security 2010; *from memory, not checked here*) | A process calls `cap_enter()` and keeps only the descriptors it holds: no global namespaces (paths, pids, sysctls) after it. The paper rebuilt Chromium's renderer sandbox on FreeBSD in about 100 lines, far fewer than the other platforms' sandboxes, because Chromium already brokers a renderer's needs through the browser and passes shared memory and sockets as descriptors | Capsicum's capability mode is close to Jam OS's handles. The lesson for (a): a renderer needs almost nothing but the descriptors Chromium already passes, so its row can be nearly empty; the start-up files are the one thing a launch-time policy must still grant, where Capsicum's after-start `cap_enter` needed nothing |
| **OpenBSD's port** (*from memory, not checked here*) | `pledge(2)` promises and `unveil(2)` paths per process type, applied by the port's patches where Linux would start its sandbox, the path lists kept as files under `/etc/chromium` | The precedent for (b): a hook patch carried every release, with the policy as data |

## 1. What each process needs, and what it must lose

What each child of `content_shell` (and CB6's `chrome`) is, how the browser
names it on its command line, and what it uses. The type comes from the
child's own `argv`, written by the browser: Chromium 155's
`SetCommandLineFlagsForSandboxType` puts `--service-sandbox-type=` on every
utility, and `UtilityProcessHost` puts `--utility-sub-type=` (the service's
interface name), whatever `--no-sandbox` says.

| Process | Its command line | Needs | Gets in CR-f's table |
|---|---|---|---|
| **Browser** | no `--type` | its list: `posix`, `svc net`, `svc dns`, `svc wayland`, `mount /data rw` (chrome: `/jam` and `svc unix`), `mount /pkg r`, `svc audio`, `right jit` | its list, unchanged, minus the JIT power after its start (SB9: the keeper holds it) |
| **Renderer** | `--type=renderer` (and `--js-flags=--jitless` for a site that may not JIT) | at its start the program's own files by path: ICU's `icudtl.dat` (`InitializeICU`, every child, before any sandbox), `chrome`'s paks and locales; descriptors Chromium passes: the Mojo socket, the V8 snapshot (`FileDescriptorStore`), `content_shell`'s pak (`kShellPakDescriptor`); fonts from the browser's font service as files (a file crosses as a fresh open the sender makes, `user/posix/sys/fdpass.h`); its own `/proc`, `/dev` and `/etc` (libc's) | the program's mount read-only (`mount self r`), the JIT power (0914 still drops it for `--jitless`), nothing else |
| **GPU process, software** | `--type=gpu-process` (with `--disable-gpu`, or no GPU found) | the start-up files; its output buffers go to the browser over Mojo, and the browser alone talks to the compositor (CB5's stop 3); the shader disk cache is the browser's | `mount self r`; `svc wayland` only if SB10's check (CR3's Q6) finds a use |
| **GPU process, SwiftShader** | the same with `--use-angle=swiftshader` or `swiftshader-webgl` (what the browser writes for `--enable-unsafe-swiftshader`) | as above, plus `MAP_JIT` memory for Reactor's code (patch 0915) | `mount self r`, the JIT power |
| **Network service** | `--type=utility --utility-sub-type=network.mojom.NetworkService`; on Linux `--service-sandbox-type=none`, as Chromium's own network sandbox is off there (`IsNetworkSandboxEnabled` is the feature `NetworkServiceSandbox`, disabled by default) | `/svc/net`, `/svc/dns`; the profile's network files (cookies, HSTS, reporting, trust tokens) and the HTTP cache, opened by path because it believes it is unsandboxed; the root store is built in (patch 0907), DNS is ours (0006) | `mount self r`, `svc net`, `svc dns`, the profile's and the cache's directories read-write as path grants (SB6, Q3) |
| **Storage service** | `--utility-sub-type=storage.mojom.StorageService` | files only through the browser's `storage::mojom::Directory`, which Chromium uses whenever the service runs out of process (`GetStorageServiceRemote`), directories sent as channels (CB5's stop 2, DL2) | `mount self r` |
| **Audio service** | `--utility-sub-type=audio.mojom.AudioService`, `--service-sandbox-type=audio` | `/svc/audio` (CA1) | `mount self r`, `svc audio` |
| **Other utilities** (data decoder, and any `chrome` starts: unzip, the print compositor, ...) | `--type=utility` and their sub-type | the start-up files; their inputs come over Mojo | `mount self r` |
| **Not used** (zygote, crashpad's handler, an unknown type) | `--type=zygote` and others | nothing: `--no-zygote`, crash reporting off | nothing (the default row) |

**What every child loses**, against today: `/data`, `/jam` and `/tmp`
(but the network service's own directories), `svc net` and `svc dns` (but
the network service), `svc wayland`, `svc audio` (but the audio service),
`svc unix`, the boot image (`SR_BOOTFS`), its own image (`SR_SELF_IMAGE`:
no exec of itself), process creation (its job without `RIGHT_WRITE`), the
JIT power (but renderers and the SwiftShader GPU process), the browser's
terminal (descriptors 1 and 2 go to the log), the browser's working
directory (`/` as text), and an unlimited job.

**What stays reachable, by design or for later.** The channels the browser
gives (Mojo: the browser is every child's broker, as on every platform);
directory and file descriptors the browser sends (capabilities it chose to
give, as Linux passes a descriptor; CR3's notes 1 and 2); the kernel's
ambient calls: clocks, random numbers, futexes, making objects (charged to
the job), `debug_write` (rate-limited, each line marked with its writer)
and `debug_report` (one 160-line box shared by every process: CR3's note
4, left for later); what services hold for it (section 4). Chromium's own
brokers enforce the rest (which files a renderer may upload, which site a
renderer may read), as on Linux.

## 2. The mechanism: a layer, not a hook

| | (a) A layer: libc applies a per-type table at the exec | (b) A hook patch where Linux starts seccomp |
|---|---|---|
| Where | libc's `execve` of `/proc/self/exe` in the forked child (still the browser's code), before `process_replace` | one call, `jam_sandbox_enter(type)`, in content's child start after `PreSandboxStartup` (patch 0914's place), or in `SandboxLinux::InitializeSandbox` (whether every type reaches it under `--no-sandbox` is unchecked) |
| What the child holds before it | nothing it isn't given: the new image starts narrowed | everything the browser holds, until the call; a bug or a crash before it leaves an unsandboxed process |
| Two phases (open start-up files, then drop) | no: the row keeps the program's own files read-only for the child's life (Fuchsia's `/pkg`) | yes |
| Job limits | the child asks its parent once, before its exec notice (CR3's Q5) | the same: a process can't set its own job's limits |
| JIT power (L2) | needs a holder outside the browser either way (section 5) | the same |
| The policy | data in the program, approved with it (`allow` shows it) | data plus a patch to rebase every release (OpenBSD's way) |
| Chromium patches | none new | one, on top of 0914 |

**Recommendation: (a).** The main session called it so too (CR3's Q5 (A),
2026-10-20), and the owner's rule is "layers, not Chromium patches". What
would make (a) impossible is a child that can only be narrowed after
something it does at its start, or a type that its command line doesn't
show. Read in the 155 source, neither exists: every input of
`SandboxTypeFromCommandLine` is on the child's command line, and the
start-up reads are of the program's own files, which a read-only grant of
its mount covers. Why (a) works here when Linux needs (b): a Linux process
has ambient authority (the file system, the network calls) that only it
(seccomp) or a privileged launcher (namespaces) can take away; a Jam OS
process has only the handles its startup message carries, and the exec's
`process_replace` cuts away everything else (CR3, "What held up").

What (a) gives up, each accepted:
- **The program's files stay readable** for the child's life: public
  bytes (the package), as Fuchsia gives `/pkg` to every sandboxed child.
  With SB6's path grants the row narrows `mount self r` to the program's
  own directory.
- **Chromium still says "no sandbox"**: `chrome://sandbox`, and `chrome`'s
  warning bar for `--no-sandbox` (Q7).
- **Chromium's unsandboxed code paths run**: the network service opens its
  own files (hence SB6), and no Chromium broker is engaged that Linux's
  sandbox would engage. Found so far: only the network service.

## 3. Narrowing at the exec in place

**The children's table.** A second note in the program, "JamOS" type 2,
beside the list (type 1), made from a file beside the list
(ports/chromium/wants/content_shell.children for the port; a
`JAM_CHILDREN(...)` macro in `<wants.h>` for C programs); checked by
`tools/checkwants.py` (a grammar, and `right jit` only in a package's or a
test's rows, as the list); shown by `allow` with the list, so the owner
approves what the program's processes get; read by libc at the first exec
in place. A program without one is unchanged (`base_unittests`,
`headless_shell` until SB10 gives it one). Rows are tried in order, the
first whose every condition matches wins; the last row is the default.
Illustrative, the syntax is SB4's:

```
# content_shell's children (illustrative)
when --type=renderer                                   mount self r; right jit; limits renderer
when --type=gpu-process --use-angle=swiftshader*       mount self r; right jit; limits gpu
when --type=gpu-process                                mount self r; limits gpu
when --utility-sub-type=network.mojom.NetworkService   mount self r; svc net; svc dns; path $HOME/.config/content_shell rw; path $HOME/.cache/content_shell rw; limits utility
when --utility-sub-type=audio.mojom.AudioService       mount self r; svc audio; limits audio
when --type=utility                                    mount self r; limits utility
when *                                                 nothing
limits renderer  pages 1/4 threads 512 handles 16384 messages 4M
limits gpu       pages 1/4 threads 256 handles 16384 messages 4M
limits utility   pages 1G  threads 256 handles 8192  messages 4M
limits audio     pages 256M threads 64 handles 2048  messages 1M
```

The grants words are the list's (`mount <point> r|rw`, `svc <name>`,
`right jit`) plus `mount self r` (the mount the program's own file is on,
as a read-only view: `/pkg:r` for a package, `/data:r` for a test build run
from `/data`), `path <dir> r|rw` (SB6) and `nothing`. A row can't grant
what the browser doesn't hold (the grant is a view of the forked child's
namespace). No row can say `posix` (it would give the shared `/tmp`), or
any power but `jit`.

**Matching the command line** exactly as base's `CommandLine` reads it on
POSIX: switches start with `--` or `-`, the value after the first `=`, the
last of a repeated switch wins, a bare `--` ends the switches. The
sub-type strings are the services' interface names as `UtilityProcessHost`
writes them; SB10 takes the exact ones from crmulti's log. A mismatch
fails closed: a type libc doesn't know gets the default row, and its child
dies at its start (no ICU), visibly. `--renderer-cmd-prefix` and its kind
(the owner's debugging flags) exec another program, which the table
doesn't cover: posix_spawn's rules apply to it, as today.

**What the new image gets** (`exec_replace`, for a program with a table):

| Role or thing | Today | With a row |
|---|---|---|
| `SR_NS` | `NS_ALL` | the row's grants, made from the forked child's namespace (a view per mount at the exec: one call to each mount granted) |
| `SR_JOB` | its own job, `JOB_RIGHTS_OWN` | its own job, read only (`RIGHT_READ`: libc reads its limits), no `RIGHT_WRITE`: no process or job can be made in it |
| `SR_JIT` | a duplicate (`/proc/self/exe`) | only for a row that says `right jit` (built by the keeper after SB9) |
| `SR_BOOTFS`, `SR_SELF_IMAGE` | duplicates | none (`no_image`, as a narrowed spawn) |
| `SR_SELF_*`, `SR_ARGS`, `SR_SIGNALS` | as FL3 | the same |
| Descriptors (the hand-over) | every one without `FD_CLOEXEC` | the same, but a terminal's (1 and 2) become the log (`dev.c`'s `log`), as a program started by init has them |
| Current directory | the channel | `/` as text (DL2's rule for narrowed children) |

**The limits.** Before its exec notice, the forked child sends its parent
one new notice on its signal channel naming the row; the parent's signal
thread looks the row's limits up in its own copy of the table and sets
them with its `RIGHT_MANAGE` handle on the child's job (the table of
children holds it since FJ1). The parent takes one such notice per child
and none after the exec notice, so the new image can't ask again (CR3's
Q5 (A); considered and not taken: a `RIGHT_MANAGE` duplicate in the fork
message, which every forked child would then hold until its exec). The
notice is a change to `abi/idl/signal.idl`: the ABI stamp moves, and every
static program, Chromium's included, is relinked once (as after DL2).

**The forked child itself** holds everything until its exec: it is the
browser's own code (CR3: the narrowing belongs at the exec, not at the
fork). A forked child that never execs (none in Chromium but the launch
path itself) stays a copy of the browser.

## 4. Jobs and limits

- **Per child**: the row's four limits on the child's own job (FJ1's), a
  child of the browser's job, so every child's use also counts in the
  browser's (a child can never use more than its parent has left).
- **Per program**: the browser's own job today has a message limit of 16
  MiB (MB) and no page limit, so many renderers together can still take
  the machine. The list gains `limit pages <n>` (and `messages`), which the
  four starters apply in `dp_job_create` (Q4: the whole of Chromium at
  memory less a reserve for the desktop). The per-renderer message limit
  keeps one renderer from spending the program's whole budget; SB10
  measures Chromium's peak message bytes before the numbers are set.
- **At the limit**: a commit refused is `ENOMEM` from `mmap` (PartitionAlloc
  and V8 then crash the process on purpose), and a page fault that can't be
  paid for kills it, which its parent sees as `SIGKILL`, as Linux's OOM
  kill (`sigfault.c`). Either way the renderer dies alone; the browser reaps
  it and `chrome` shows its "Aw, Snap!" page (`content_shell` a blank view).
  Handles and threads refused are `EAGAIN` or `EMFILE`; messages past the
  limit block or fail as a full pipe.
- **Telling the child its limit**: libc answers the job's page limit as
  the machine's memory when it is lower (`sysconf(_SC_PHYS_PAGES)`,
  `sysinfo`, `/proc/meminfo`'s `MemTotal` and `MemAvailable`), as container
  runtimes do with cgroups, so V8's heap limit and PartitionAlloc size
  themselves to the job and V8's own out-of-memory handling comes first.
- **What a job can't reach**: memory a service holds for a child (netstack's
  rings, up to 96 MiB; the mixer's rings; the broker's waiting connections)
  and what the browser stores for a renderer (blobs, session history, the
  caches) stay the service's or the browser's, bounded by their own caps
  and Chromium's quotas. With CR-f only the network and audio services
  hold those services at all; netstack's rings move to the opener's job in
  a later track (SBN, below).

## 5. The JIT power: L2 and L3

**L3, the kernel flag** ([CJR2](CR-REVIEW-JIT.md#cjr2-the-jits-second-half-cr-e-swiftshader)
finding 3, design question 2 (b)). A process flag `NOJIT`: set on itself or
on an unstarted process it made by a new call (libc's `PR_SET_MDWE` sets
it after closing `SR_JIT`), never cleared; copied by `process_create` from
the creator to every process it makes; `vmo_make_jit`, and any map or
protect that would give a JIT mapping write and execute together, refuse a
`NOJIT` caller whatever handles it holds (so a JIT root received later
helps nothing); `process_get_info` reports it; `process_replace` installs
the new image's (the unstarted process's) flag. `/svc/exec` reads its
caller's flag (CBF2a's writer pid, looked up with init's root) and sets
it on the child before the start. That is Linux's MDWE across every
`execve`. It also gives init something to check about a caller that DC's
notes asked for (`base_unittests`' launching tests): a follow-up, not CR-f.

**L2, the browser gives the power up** (CJR2 finding 2, design question 1
(b)). The renderers' power can't come from the browser if the browser has
none, and it can't come from the forked child: that child is a copy of the
browser, and anything it holds the browser's attacker holds. So a third
process holds it:

- **The keeper.** libc's start, in a program whose children's table has a
  `right jit` row, forks a small keeper process before `main` (no Chromium
  code has run, no input read), then closes the browser's `SR_JIT` and sets
  its `NOJIT`. The keeper keeps the power and the browser's namespace, runs
  only libc's loop, and serves one request on a channel the browser's
  forks inherit (libc's own, never handed to an image): "build
  `/proc/self/exe` with this command line, environment and hand-over, in
  my job". It builds the image for a `right jit` row with `SR_JIT` in the
  startup message, arms it, and sends back a replace-only handle. Every
  other row is built in the forked child as in SB5 (the same code). `ps`
  shows the keeper; libc keeps it out of `waitpid(-1)`; it dies with the
  program's job.
- **Arming** (FORK-PLAN's FX row: "a handle ... that allows
  `process_replace` only (a right of its own, never write)"): a new call
  fixes an unstarted process's entry, stack and arguments and moves its
  startup channel's end into it, so the caller of `process_replace` can't
  read the startup message (and take `SR_JIT` out of it) or choose where
  the image starts; an armed process refuses `process_start` and
  `thread_create`; `RIGHT_REPLACE` on its handle is all the forked child
  needs.
- **No threads from outside** (FR1's design question 2, its flag form): a
  started process refuses `thread_create` from any other process, so the
  browser can't run a thread of its own in a renderer that holds the
  power. SB8 checks that no caller needs otherwise (spawn and fork make the
  first thread while the process is unstarted).

What stays: a compromised browser can ask the keeper for a renderer with
the power and feed it input over Mojo; it then runs code with a renderer's
reach, never W+X in the browser. Without SB9 the plan still holds
everything else (the browser keeps the power, CJR2's (a)): Q2.

**Who holds the power, after CR-f:**

| Process | `SR_JIT` | `NOJIT` | Can map W+X |
|---|---|---|---|
| Keeper | yes | no | could, never does (libc's loop only) |
| Browser | no (closed at start) | yes | no |
| A forked child before its exec | no | yes (from the browser) | no |
| Renderer (built by the keeper) | yes | no | yes |
| Renderer with `--jitless` | no (0914's `PR_SET_MDWE`) | yes | no |
| GPU process, SwiftShader (keeper) | yes | no | yes |
| GPU process, other; utilities | no (not in their row) | yes (from the forked child) | no |
| Anything any of these starts | no | from its creator | no |

## 6. The GPU process

The GPU process has two rows from the start, told apart by the ANGLE back
end the browser writes on its command line (`--use-angle`), so that the
iGPU is a table edit, not a redesign:

```
when --type=gpu-process --use-angle=swiftshader*   mount self r; right jit; limits gpu
when --type=gpu-process                            mount self r; limits gpu
```

**Now.** The second row is the software GPU process (`--disable-gpu`, or
no GPU found): `mount self r` and nothing else; its frames reach the
compositor through the browser over Mojo. CR3's Q6 is a check in SB10: run
`multi` and `webgl` with the GPU rows lacking `svc wayland`; if both pass,
they keep none. The first row is **SwiftShader** (opt-in,
`--enable-unsafe-swiftshader`): the same plus `right jit`, chosen by
`--use-angle=swiftshader*`, which only the browser writes (CJR2,
SwiftShader section); a restart after a crash is a fresh exec, so the row
is chosen again. The GPU rows' limits are a renderer's or more (its
textures).

**When the iGPU lands** ([GPU-PLAN section
6](GPU-PLAN.md#6-what-the-sandbox-must-allow-the-gpu-process)), the edit is
one grant on the second row, `svc gpu` (a client channel to drv/igpu, its
own address space and contexts: Linux's render node), and checkwants
learning the name; nothing else changes. That row already has no JIT
power, which ANV needs none of; the SwiftShader row keeps the power as now,
for a fallback. What the table relies on from GPU-PLAN: only the GPU
process's row names `svc gpu` (renderers reach the GPU through it, as on
Linux); its buffers are its own VMOs, charged to its job and its limits,
and the driver caps each client's page-table pages; a shader that never
ends costs only its context (the hang check, the reset, a ban after a few);
Mesa's own shader cache is off (Chromium keeps its own, the browser's), so
the row needs nothing on disk; buffers reach the compositor through the
browser, as now. SBG then adds sbtest's case (a renderer-type child that
opens `svc gpu` fails) and checks the outside listing (the GPU process
alone reaches drv/igpu).

## 7. SwiftShader by default, after CR-f

Today SwiftShader is opt-in because a page's shaders are compiled to
machine code in a process holding the JIT power and, until CR-f, the
browser's whole namespace. After CR-f that process is a renderer's equal: no
files but the program's, no network, no compositor, no audio, no process
creation, its own limits. What stays: the GPU process is shared by every
site (one compromised through a shader sees every tab's drawing and talks
to the browser's GPU-host interfaces), which is why Chromium itself calls the
flag "unsafe"; and without SwiftShader Jam OS has no WebGL until the iGPU.
**Recommendation**: default-on for `chrome` after SBR passes and SBH's GPU
cases hold, as a flag in the package's command line the owner can take out,
and back to a fallback when the iGPU's WebGL works (Q5).

## 8. CR3's findings and the services

[CR3](CR-REVIEW-LAYERS.md) found 0 High, 3 Medium, 5 Low. The main
session's calls on its questions (2026-10-20; the owner's to override):

- **M1, shared openers** (Q1 (A)): `/svc/net`, `/svc/dns` and `/svc/audio`
  give each program one opener of its own, as `/svc/unix` does
  (`own_channel`); an opener never makes more; netstack's per-opener caps
  stay where Chromium's socket pools fit (256 sockets a pool) while no one
  program can take the whole table. The mixer's per-opener stream cap
  rises to 8. **`audio_open_on` is limited, not removed**: libjam's call
  keeps its signature and hands back the program's one opener, so patch
  0916's back end needs no rebuild and its four openers become one with 8
  streams (the 0916 comment about 16 streams is fixed at its next change).
  A narrowed child given `svc audio` or `svc net` by its row shares the
  browser's opener (an opener can't make another). Rings charged to the
  opener's job: a later netstack track (SBN).
- **M2, socket files** (Q2 (A)): `connect` opens the socket file for
  writing (no truncation), so a read-only view can't connect, as Linux
  asks write permission; CB6 keeps the singleton's socket and directory
  under the profile on `/jam`, never in `/tmp` (the main session told CB6).
- **M3, fontconfig** (Q4): bin/pkg builds fontconfig's caches when it
  installs a fonts package, into the package area (read-only to all through
  `/pkg`); fonts-jamos's `fonts.conf` names that cache and no writable one;
  the owner's own font settings come only from where a settings grant
  writes; the crt drops `FONTCONFIG_FILE` and `FONTCONFIG_PATH` for AT_SECURE
  children. **This one can't wait for CR-f**: the browser itself reads the
  shared settings, so any approved program with `mount /data rw` can steer
  the browser's fonts today, and `chrome` (CB6) makes it the owner's daily
  browser. SB1 should run now.
- **Q3, text directories over sockets**: refuse kind 2 over sockets once
  CB6 shows the profile never sends one; in SB7.
- **Q5**: section 3 (adopted). **Q6**: section 6 (a check in SB10).
- **Lows**: L4 is Q3 (SB7); L5 (`/sys` records adopted under the socket's
  lock: make the text at the first read), L6 (a moved open lost when its
  send is refused: keep it until a write takes it), L7 (a received file
  can't be given on: give it on as duplicates of its channel and buffer,
  which narrower processes may need), L8 (`/proc/<child>/fd` waiting 3 s on
  a child that never answers: a shorter wait or the last answer) all go to
  SB7, each with CR3's test idea failing first. CR3's note 5 (a refusal at
  receipt breaks Mojo's channel; narrower namespaces make refusals likelier:
  adopt lazily, fail at first use) goes to SB7 too.

M2 is live today for any program that can read a socket file's directory;
SB3 is small and can run ahead with SB1. M1 is live for any approved program
holding the three services; SB2 can run ahead too, before CB6 if the main
session wants (it touches no Chromium file).

## 9. The hostile tests and the browse check

**The outside check** (SB8's kernel piece): a debug listing of a process's
handles (`handles <pid>`, `RIGHT_ROOT_DEBUG`, the shell's `ps -h`): each
handle's type and rights and, for a channel, the process holding its peer.
That is what a process can reach, whatever its own libc says. cr-test's new
`sandbox` case lists every Chromium process and checks: renderers and the
GPU process reach the browser and `/pkg`'s server only; the network service
also netstack, the resolver and extfs (its path grants); the audio service
also the mixer; none reaches init, the compositor, devmgr or another
program; no child's job handle has `RIGHT_WRITE`; the browser holds no
`SR_JIT` (after SB9).

**sbtest** (SBH: a test program under `user/tests/` with a children's table
of Chromium's shape; written by an agent that wrote none of SB4 to SB9).
It forks and execs itself as each type and, in each child, with raw system
calls as well as libc (a compromised child owns its libc):
- scans its whole handle table (every value, `handle_info`) against the
  row's exact set;
- opens `/jam`, `/data`, `/tmp`, `/boot`, `..` past `/pkg`'s view, and a
  directory channel's `..`: refused;
- `socket` and `connect` to the test's listener, a name lookup, Wayland,
  the mixer, `/svc/unix`, `/svc/exec`, init: refused (and with the
  services' openers by raw `svc.connect` on any channel it holds);
- `fork`, `posix_spawn`, `execve` of itself and of `/bin/true`, a raw
  `process_create` and `job_create` with each handle it holds: refused;
- `mmap(MAP_JIT)` and `vmo_make_jit`: refused but for the jit rows; after
  `PR_SET_MDWE`, refused with a JIT root sent to it afterwards;
- eats pages until killed (the parent sees `SIGKILL`; the system's free
  memory, read by the parent, stays above the reserve), threads, handles
  and queued message bytes until refused at the row's limits;
- asks its parent for limits a second time, and after its exec: ignored;
- floods `debug_write`: rate-limited (a record, not a pass/fail).

And for the keeper (SB9): a forked child asking for a jit row gets an armed
process whose startup message it can't read, whose start it can't choose,
and into which no other process can make a thread.

**On real Chromium** (SB10, cr-test's `sandbox` case): `content_shell` with
two tabs; `chrome://memory-exhaust` in one (content's debug URL: its
renderer allocates until it dies): that renderer is killed at its job's
limit, the other tab's page still answers, the browser lives, the free
memory never under the reserve; then the outside check above. **The browse
check**: every `cr-test.sh` case with the sandbox on, and the owner's PC
check (news, Wikipedia, a search engine, GitHub, a video with sound, a
WebGL page with SwiftShader).

## 10. Tracks

| Track | What | Files | Gate | Tests | Size | Model |
|---|---|---|---|---|---|---|
| **SB1. Fonts (CR3 M3)**, ahead of CR-f | fontconfig's caches built at install into the package area, no writable shared `cachedir`, the owner's settings only where a settings grant writes, `FONTCONFIG_FILE`/`FONTCONFIG_PATH` dropped for AT_SECURE children | bin/pkg's install step, `ports/fonts-jamos/override/fonts.conf`, `user/posix/sys/etc.c`, `user/posix/crt/start.c` | none | cr-test fonts with a planted `/data/.config/fontconfig/fonts.conf` mapping sans-serif to the cut Inter (fails without); posixtest AT_SECURE env | 1.5 | Opus |
| **SB2. Openers of one's own (CR3 M1)**: built, see [SB2 notes](#sb2-notes) | `own_channel` for net, dns and audio; netstack's and the resolver's per-opener caps; the mixer's per-opener cap 8; `audio_open_on` handing back the program's opener | `user/lib/ns.c`, `user/lib/audio.c`, `user/include/net.h`, `user/include/dns.h`, `user/include/mixer.h`, the services' opener counts | none | utest: one program opens openers until refused, a second still gets one of each (fails without); crsound | 1.5 | Opus |
| **SB3. Socket files need write (CR3 M2)** | `connect` opens the socket file `O_RDWR` without truncation | `user/posix/sys/unixsock.c` | CB6 merged (its singleton's place) | posixtest unix: a read-only view can't connect (fails without); a rewritten socket file is refused | 0.5 | Sonnet 5.5 |
| **SB4. The children's table** | note type 2, its grammar (rows, conditions, grants words, `mount self r`, `limits`), `JAM_CHILDREN`, libos's reader, checkwants and its selftest, `allow` showing it, the port's link step embedding `wants/<program>.children`; the list's `limit pages`/`messages` words applied by `dp_job_create` | `user/include/wants.h`, `user/lib/wants.c`, `tools/checkwants.py`, `<dataprog.h>` and its source, the shell's `allow`, tools/cr's link wrapper | none | checkwants selftest (tables it must take and refuse); utest: a table read back; `allow` shows it | 1.5 | Opus |
| **SB5. Narrowing at the exec** | section 3: the row match (base's rules), the grants as `.ns`, `SR_JOB` read only, no boot image or own image, `SR_JIT` only for jit rows, 1 and 2 to the log, `/` as text; the limits notice and the parent's answer; libc's memory figures from the job's limit | `user/posix/sys/spawn.c`, `spawnfd.c`, `sigthread.c`, `proc.c`, `procinfo.c`, `abi/idl/signal.idl` (own commit first: ABI stamp), `user/lib/spawn.c` | SB4 | posixtest exec: each row's image holds exactly its set; a second ask and an ask after the exec ignored; `MemTotal` is the limit; utest; init run, basic, cmds, commands, files, kdump save (crosses areas) | 2.5 | Opus |
| **SB6. Path grants** | a mount at a path of several names (longest whole-name prefix), `path <dir> r\|rw` (a subview mounted at its own path), `$HOME` from the program's start environment, `path self r`; `NS_NAME_MAX` raised in `abi/idl/nschan.idl` if the profile's path needs it (own commit) | `user/lib/ns.c`, `user/include/os.h`, `abi/idl/nschan.idl`, the grants code in libos | SB2 (the same `ns.c`), SB5 | utest ns: a path grant reaches only its subtree, `..` stops at it; posixtest | 1.5 | Opus |
| **SB7. libc's Lows (CR3 L4 to L8, note 5)** | Q3's refusal of text directories over sockets (after CB6's check), L5 to L8, adopting lazily | `user/posix/sys/fdpass.c`, `sockpair.c`, `file.c`, `etc.c`, `dir.c`, `proc.c` | SB5 (`proc.c`) | posixtest scm and procinfo: CR3's tests, each failing first | 2 | Opus |
| **SB8. The kernel** | L3's `NOJIT` flag; arming and `RIGHT_REPLACE` (FX's kernel half); no `thread_create` into a started process from outside; the `handles <pid>` listing with peers | `kernel/object/process.c`, `kernel/object/vmo.c`, `kernel/mm/aspace.c`, `kernel/abi/sysc_proc.c`, `kernel/debug/dbgcmd.c`, `kernel/include/jam/abi.h`, `abi/syscalls.def`, a new ktest file | FZ merged (one kernel track at a time) | ktests: the flag copied, sticky, refusing `vmo_make_jit` and W+X protect with a root sent later, installed by replace; an armed process refuses start and thread creation, its replace uses the armed entry; the listing's peers; ktest 4 and 8; soak 2 | 2.5 | Opus |
| **SB9. The keeper (L2)** | section 5: the keeper forked at start, the browser's power closed and `NOJIT` set, the request, building and arming jit rows, `PR_SET_MDWE` setting `NOJIT`, `/svc/exec` reading its caller's flag, `ps` and `waitpid` | `user/lib/start.c`, `user/posix/sys/spawn.c`, `prctl.c`, `proc.c`, a new libc file for the keeper, `user/services/init/exec.c`, `user/services/shell/cmd/ps.c` | SB5, SB8 | utest jit_mdwe grown: the browser-shaped test holds no power after start; a fork gets a jit image only through the keeper; `/svc/exec` withholds the power from a `NOJIT` caller's child (CJR2 finding 3's case, failing first); crjit | 2 | Opus |
| **SB10. Chromium's tables and cases** | `content_shell.children`, `headless_shell.children`, `chrome.children` (with CB6's profile and cache paths), the relink (build.sh), Q6's check, the network service's files, measured message and page peaks for Q4's numbers; cr-test's `sandbox` case | `ports/chromium/wants/`, `tools/cr-test.sh`, a tools/cr check script | SB5, SB6, SB9 (jit rows), CB6 merged | cr-test headless, shell-wl, multi, jit, webgl, sound, fonts, chrome, sandbox | 2 | Opus |
| **SBH. The hostile tests** | section 9's sbtest and its utest and posixtest cases, by an agent that wrote none of SB4 to SB9 | a new user/tests/sbtest, utest and posixtest cases, TESTING rows | SB5 (most), SB9 (the keeper's) | its own; fails any regression of section 9's list | 1.5 | Opus |
| **SBR. Review and fix** | the whole: kernel, libc, the table and the keeper, findings first (CR3's way), High and Medium fixed with a failing test each | the files its fixes need | SB1 to SBH merged | its own; ktest 4 and 8; soak 4; every cr-test case | 2.5 | Opus |
| **SBJ. The join** | ARCHITECTURE (Userland: the children's table, the keeper; Objects: the flag, arming; Security), CR-PLAN section 13 and the CR-f row, TESTING, ROADMAP, HISTORY; this plan deleted when HISTORY covers it | the docs | SBR | `make check` | 0.5 | Sonnet 5.5 |

**Later, not CR-f's:** **SBS** SwiftShader on by default in `chrome`'s
command line after SBR, if the owner says so (Q5; Sonnet 5.5, 0.5).
**SBG** `svc gpu` on the hardware GPU row, its sbtest case and the outside
check, with GPU-PLAN's stage 4 (G4c) (Sonnet 5.5, 0.5).
**SBN** netstack's rings as client VMOs charged to the opener's job, read
with `vmo_read` as the mixer reads (CR3's Q1; Opus, about 2).

**Order.** SB1 now (it can't wait: section 8), SB2 and SB3 beside it
(SB3 after CB6 merges). SB4, then SB5. SB8 when FZ has merged (the kernel's
files one track at a time), beside SB4 and SB5. SB6 after SB2 and SB5 (both
touch libos's namespace or the exec); SB7 after SB5. SB9 after SB5 and SB8.
SB10 after SB5 and CB6, its jit rows after SB9 and its network row after
SB6 (it can start with `mount /jam rw` for the network service and narrow
when SB6 lands). SBH beside SB10. SBR when all are merged; SBJ last. The
relink: SB5 moves the ABI stamp, so every program is rebuilt and Chromium
relinked once (SB10's build).

**Size.** SB1 to SBJ: about 22 agent-hours (SB1 1.5, SB2 1.5, SB3 0.5, SB4
1.5, SB5 2.5, SB6 1.5, SB7 2, SB8 2.5, SB9 2, SB10 2, SBH 1.5, SBR 2.5, SBJ
0.5), of which the core (SB4, SB5, SB10, SBH, SBR) is about 10; the
estimate doubles if Chromium's children turn out to open files the table
doesn't foresee (CR-PLAN's rule for hidden work). Later: SBS 0.5, SBG 0.5,
SBN 2. In lines, roughly: kernel 400 (SB8), libc and libjam 1,500, the
table's grammar and checks 400, tests 1,500.

**Risks.**
1. *A child that opens something unforeseen.* Chromium under `--no-sandbox`
   may open paths a sandboxed Chromium would ask the browser for; each is
   a crash at a narrowed child's start or first use. SB10 runs every case
   and logs every refused open by path (libc's log line), and the row
   grows only by the owner's approval of the table.
2. *Mojo breaking on a refused descriptor* (CR3's note 5): narrower
   namespaces make refusals at receipt likelier; SB7's lazy adoption.
3. *Limits too tight* make ordinary pages die: measured first (SB10), then
   4 times the measured peak with Q4's floors; the per-program limit is the
   one that protects the desktop.
4. *The keeper is a privileged server* reachable by the browser and its
   forks: its request parser is small and SBR reads it as hostile input.
5. *Rebase cost is nil* (no new patch), but a Chromium release that adds a
   process type or renames a sub-type falls to the default row: the
   release bump's checklist (CR-PLAN's security updates) gains "check the
   children's table against `--type`s in the log".

### SB2 notes

Built 2026-10-20, ahead of CR-f (CR3's M1, section 8's call).

- **Openers of one's own.** libos's grants (`own_channel`, `user/lib/ns.c`)
  give `/svc/net`, `/svc/net-listen`, `/svc/dns`, `/svc/audio` and
  `/svc/audioctl` by name as an opener of the program's own, made with
  `svc.connect` on the starter's shared channel, as `/svc/unix` since
  CR2F: `svc_open` and `svc_get` hand back that one opener, a forked or
  spawned child given the namespace whole shares it, and `"*"` still gives
  the shared channel (init, the shells). `/svc/net-listen` and
  `/svc/audioctl` are in because their openers are the same tables
  (netstack's 24, the mixer's 24) and a list from `/data` may name them.
  `/svc/net-low` is not: no program from `/data` may have it, and serve,
  given it by init, listens again on a new opener of it after netstack
  restarts.
- **An opener makes no more.** netstack's and the resolver's opener
  channels already answered only their own protocol (`svc.connect` there is
  another protocol's ordinal, `ERR_NOT_SUPPORTED`). The mixer answered
  `svc.connect` on every channel: now on the shared `audio` and
  `audioctl` channels alone. `audioctl.desk_channel`, which any audioctl
  channel may ask, makes at most `MIXER_DESK_CLIENTS` (4) openers in all,
  so it can't fill the table either.
- **The numbers.** netstack's per-opener caps are now a program's and stay
  as CN1 set them: 256 sockets, 512 TCP connections, 4 listeners, 64 MiB
  of rings and 8 waits, of the ordinary programs' 384, 768, 12, 96 MiB and
  48, so one program takes at most two thirds. Chromium 155's (read from
  its source): a soft cap of 256 sockets in each of the normal and the
  WebSocket pools (`socket_soft_cap_per_pool`), so 512 connections, 6 to a
  host (255 for WebSockets), 128 through a proxy chain; 512 connections at
  the default rings are 42 MiB. They fit exactly, so the caps stay. The
  resolver: 8 lookups in flight a program (`DNS_PER_OPENER`, of the 12
  names programs may have in flight together), 12 programs' openers.
  Chromium resolves with `getaddrinfo` on up to 64 worker threads
  (`kDefaultMaxSystemTasks`); a lookup past the 8 used to fail at once
  (`EAI_AGAIN`, the page's name not resolved), and now waits in libjam's
  `dns_lookup` for one to end (10 ms, doubling to 160 ms, until its
  deadline), so the 64 queue and other programs keep their 4 (Chromium
  has it at its next link). The mixer:
  8 streams an opener (`MIXER_STREAMS_PER_CLIENT`, was 4), 16 in all, 24
  openers.
- **Chromium's sound.** `audio_open_on` is limited, not removed (no change
  to `audio.c`): for a program given `/svc/audio` by name `svc_open` hands
  back its one opener, so patch 0916's extra openers add nothing. The
  staged binary (old libjam: `kOpeners` 3, from 16 / 4 - 1) opens 8 streams
  on `svc_get`'s opener; a ninth gets `ERR_NO_RESOURCES` there and on each
  extra opener (the same one), `JamOutput::Open` says `kNoStreams` and
  `AudioOutputStreamJamOS::Open` fails. Chromium's `AudioOutputResampler`
  then falls back to a fake (silent) output if no stream of those
  parameters has opened before, and otherwise fails the stream (the page's
  audio errors), as CA1 saw for a fifth. Built against the new `<mixer.h>`
  the back end has `kOpeners` 1 (16 / 8 - 1), the same opener again. Its
  audio service has 8, the whole browser's: only that process opens
  streams. Chromium's own cap (`AudioManagerBase`'s 16) is above it.
- **Late answers.** fetch's and bin/pkg's resolves used a fresh opener each
  with txid 1; with the program's one opener a late answer could be taken
  for the next resolve's, so each takes the next txid of a counter of the
  program's.
- **Left.** A program with an opener of its own has no network, names or
  (for `/svc/unix`) sockets after netstack, the resolver or the broker
  restarts, until it is started again: their openers die with them, and
  `svc_open` hands back the dead one (the mixer keeps its openers across a
  restart, so sound is unaffected). A `svc exec` program can still start
  approved programs, each with openers of its own, until the tables fill
  (as CR2F's left item). Netstack's rings stay its own pages (SBN).
- **Tests.** utest `svc_own_openers` (a program opens each service 9
  times, a second still gets one of each; before the grants change the
  first took every table and the second was refused, `ERR_NO_RESOURCES`);
  mixtest `openers` (one opener of each, `svc.connect` on it refused),
  `stream_cap` (8, the next refused there and on another open), `desk`
  (4 desk channels); utest `audio_out_slots` (8, the ninth refused through
  `audio_open_on` too); dnstest `openers` and `room` (a seventh asker of a
  name waits for room), and `dnstest busy` / `dnstest names`, the busy
  asker as a second program ([TESTING](TESTING.md#dhcp-and-dns-end-to-end)).

## 11. Questions for the owner

**Answered (the owner, 2026-10-20): every recommendation above taken** (layer (a); the keeper; path grants; limits from 4x the measured peak with the floors; SwiftShader default for `chrome` after SBR; children's output to the log; the one-line --no-sandbox warning patch after SBR; the ordinary-sites rule lifted after SBR and the PC check). SB1 and SB2 started ahead of CR-f the same day.

One at a time; each ends with a recommendation, which the plan assumes
until the owner says otherwise. The main session's calls on CR3's
questions (section 8) are recorded there and are the owner's to override.

### Q1. A layer (a) or a hook patch (b)

(a) libc applies the children's table at the exec in place, no Chromium
patch; (b) one call in content's child start, OpenBSD's way, with two
phases (the start-up files then dropped). *Recommendation: (a)* (section 2;
the main session's call on CR3's Q5): nothing in the 155 source needs (b),
the child never holds what it shouldn't, and the cost is only that the
program's own read-only files stay readable.

### Q2. The browser's JIT power (CJR2's L2)

(A) the keeper (section 5): libc forks it before `main`, it holds the power
and builds the renderers' images, armed by the kernel; the browser gives
the power up and can never get it back (L3). (B) init's `/svc/exec` builds
`/proc/self/exe` images (FX widened): no extra process, but init learns
every program's table and serves every Chromium launch. (C) leave it: the
browser keeps the power to hand on (CJR2's (a)), renderers alone use it.
*Recommendation: (A).* It is local to the program, its kernel half is FX's
(planned anyway), and it takes W+X out of the most powerful process.

### Q3. The network service's files

(A) path grants (SB6): the profile's and the cache's directories, each a
subview at its own path; (B) the network service gets `/jam` read-write
(without `etc`), no new namespace piece; (C) Chromium's directory transfer
(Fuchsia's `TransferableDirectory` mount, a build flag and a patch).
*Recommendation: (A).* The network service parses the most hostile input
after the renderer; (B) would leave it every file on `/jam`; (C) is a patch.

### Q4. The numbers

Per type, measured first (SB10), then 4 times the peak, with these floors:
renderer and GPU a quarter of memory, utilities 1 GiB, audio 256 MiB, 512
threads, 16,384 handle units, 4 MiB of queued messages each; the whole of
Chromium at memory less 1 GiB for the desktop, its message budget 64 MiB.
*Recommendation*: these, revised after the PC's real figures.

### Q5. SwiftShader by default after CR-f

(A) default-on for `chrome` after SBR, a flag in the package's command
line the owner can remove, back to a fallback when the iGPU's WebGL works;
(B) opt-in as now. *Recommendation: (A)* (section 7): the GPU process is
then contained as a renderer is, and without it there is no WebGL; the
residual is the GPU process seeing every site's drawing, as on any
platform where one GPU process serves all sites.

### Q6. The children's stdout and stderr

(A) the log (as CR3's Q5 says): a renderer can't read what is typed in
the browser's terminal; the owner reads children's messages with the log
viewer; (B) the terminal, as today. *Recommendation: (A).* A browser run in
the background shares its terminal with the shell, and a renderer holding
the terminal's channels could read the next password typed there.

### Q7. `chrome`'s "--no-sandbox" warning

`chrome` warns at every start that `--no-sandbox` is an unsupported flag,
and `chrome://sandbox` says no sandbox. (A) leave both: true of
Chromium's own sandbox; (B) a one-line patch dropping `--no-sandbox` from
the warning's list on Jam OS once SBR passes. *Recommendation: (B)* after
SBR: a warning that is false at every start teaches the reader to ignore
warnings. (It is a patch, against the usual rule; the owner's call.)

### Q8. The "ordinary sites only" rule

The owner's rule holds until Jam OS's sandbox lands and is reviewed.
*Recommendation*: lift it when SBR has passed and the PC check has browsed
with the sandbox on, keeping in mind what Chromium's own sandbox would add
on Linux that this one doesn't (none found for renderers; the shared GPU
process as on every platform).
