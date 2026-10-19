# CR3: the layers review (CU1 and CU2 since CR2, CN1, CB5, FJ1, PR, CA1, FONTS)

The independent review of the Chromium-facing layers merged since the CR1
and CR2 reviews ([CR-REVIEW-KERNEL](CR-REVIEW-KERNEL.md) read main
f3255c61, [CR-REVIEW-POSIX](CR-REVIEW-POSIX.md) main d84583da): the layers
the sandbox (CR-f) will stand on. Read at main bcc76f8c by an agent that
wrote none of it. Findings only: no code was changed, and each fix is a
track of its own, with its test.

- **CU1 since CR2**: descriptor passing on socketpairs as it is now:
  `user/posix/sys/sockpair.c` (the half-close 1a1b6b07, SO_PASSCRED
  931e41e2, FL2's fork state, LH's heap), `fdpass.c` and `fdpass.h` with
  the kinds CB5 and DL2 added (a directory by its path 2d67cc9a, a /proc
  file 7edf206e and kind 14 19f00760, a file unlinked while open moved
  b6f09dea, a directory as its channel e1dc06f8), the adopters a record
  reaches (`dir.c`'s, `procfs.c`'s, `dev.c`'s, `etc.c`'s, `memfd.c`'s,
  `file.c`'s give), and the kernel's rule for channel ends in a queue
  (`kernel/object/channel_send.c`, `check_carried`).
- **CU2 since CR2**: the broker after CR2F (7c121328:
  `user/services/unix/main.c`, `abi/idl/unix.idl`, `user/lib/ns.c`'s own
  openers) and libc's socket files (`user/posix/sys/unixsock.c`).
- **CN1** (a9f05623) and the network grants: `user/include/net.h`'s and
  `user/include/dns.h`'s caps, the CN1 notes, and what reaches each of
  Chromium's processes.
- **CB5** (539f8c9e) and what it stands on: `fork` (`user/lib/fork.c`'s
  roles, `user/posix/sys/fork.c`'s job), the exec in place of
  `/proc/self/exe` (`user/posix/sys/spawn.c`'s `exec_replace` and
  `exec_handover`, `user/lib/spawn.c`'s `gather_handles`), POSIXFDS_PROC,
  SR_ARGS; the wants of `ports/chromium/wants/content_shell`.
- **FJ1** (ac8ac3c8: a job per forked child, kill that never waits,
  AT_SECURE over an exec in place de02a914, the log as descriptors 1 and 2)
  and **PR** (199a3127: `/proc/<child>/fd` through `signal.idl`'s
  `list_fds`, `user/posix/sys/proc.c`), from their notes and code.
- **CA1** (810b540d, 2e166505): libjam's `audio_open_on`
  (`user/lib/audio.c`), the mixer's openers and streams
  (`user/services/mixer/clients.c`, `streams.c`, `output.c`,
  `abi/idl/audio.idl`).
- **FONTS** (9b00e42c): libc's `/etc/fonts/fonts.conf`
  (`user/posix/sys/etc.c`), fonts-jamos's `fonts.conf`
  (`ports/fonts-jamos/override/fonts.conf`), the `mount /pkg r` grant.
- **DL2** (fd4ead0f) only where it meets the above: kind 15 over
  `SCM_RIGHTS` and the current directory an exec hands on. Directory
  handles in depth are DR's review (docs/DIRHANDLE-REVIEW.md).

What it was read against: every record, handle and name that crosses a
process boundary as hostile input (a socketpair's peer, a connector or
listener of a named socket, a child answering its parent, a client of a
shared service); and, for the launch, what each of Chromium's processes
holds after fork and the exec in place, against what it needs.

Severity, as CR2's: **High** lets one program use another's authority, or
corrupts memory or a handle; **Medium** lets one program stall or deny
another, or take over what another shows or reads; **Low** a narrow case,
a slow path, or a design point.

**0 High, 3 Medium, 5 Low, 5 notes.** None is fixed here. The renderer's
whole inheritance (the browser's namespace, the JIT power and the rest) is
by design until CR-f: it is listed in "What the sandbox (CR-f) must
close", not counted as findings.

## What held up

- **No handle crosses that its sender doesn't hold.** A socketpair part
  carries kernel handles, so a sender gives only what it has; the record's
  handles must be its kind's object types (`types_are`); the kernel
  refuses a channel end into its own pair or into a queue that holds an end
  already (`check_carried`), so no reference cycle can outlive its
  processes. A file record's path and a directory channel's text are
  dropped at receipt (`fdpass_adopt`), so neither is resolved with the
  receiver's rights.
- **Rights don't widen on the way.** A memfd goes as a duplicate of its VMO
  with the description's rights, without `RIGHT_WRITE` once write-sealed
  or read-only (`vmo_for`); a record's status asking `O_RDWR` over a
  read-only handle gives a description whose writes the kernel refuses.
  libc's memfd VMOs are kept, so a receiver can't shrink them under the
  sender. (CR2's findings 8 and 9, seals that are the sender's word and
  `F_SEAL_GROW` not in the rights, still stand as listed.)
- **Descriptors in flight when a process dies.** A dead sender's queued
  sends still read; a dead receiver's end closes and the kernel closes
  every handle queued on it (after unlocking, `channel.c`'s rule); parts
  of a send that never finished are dropped by the next send's first part
  or plain bytes, or held until the description closes (bounded:
  253 descriptors a description, note 3). `MSG_CTRUNC` is set for no room,
  a part that breaks the format, an adopter's refusal and a full
  descriptor table, and the dropped handles are closed every time.
- **The broker after CR2F.** The shared channel answers `svc.connect`
  only and an opener channel never does (`no_ops`), so a program given
  `/svc/unix` by name can't make itself more openers; its listeners count
  until they close. Keys are 128 random bits, compared whole.
- **The exec in place is a clean cut.** `process_replace` closes every
  handle but the startup message's, so the new image holds exactly what
  `exec_replace` lists: its process, thread and address space, its own
  job without `RIGHT_MANAGE`, the boot image, the namespace, its argument
  VMO, its own image, its signal channel and (for `/proc/self/exe`) the
  JIT power. Nothing the forked child held before the exec (the browser's
  descriptors, `SR_CONSOLE`, `SR_STDOUT`, its mappings) survives it but by
  the hand-over. A fork gives no root resource (`SR_RESOURCE` has no
  transfer right). This is what makes narrowing at the exec workable
  (question 5).
- **FJ1.** Each forked child is in a job of its own, which it holds
  without `RIGHT_MANAGE`; SIGKILL is that job's kill, so a renderer can't
  outlive its kill through its own forks; a child that stops reading its
  signal channel can't hold its parent's `kill` (standard signals owed
  once, real-time ones EAGAIN). A raised image stays raised over an exec
  in place.
- **PR.** A process reaches only its own children's `/proc` (no call turns
  a pid into a handle); a child's descriptor list is its own word, matched
  by question number, and a lie changes only that listing.
- **SR_ARGS at an exec in place** is written by the process for its own
  new image, and read by CR2's fixed `take_args`.
- **The mixer against a hostile stream.** It reads a client's ring with
  `vmo_read` (no fault if the ring changes), clamps a client's `write`
  counter to the ring (`take`: "its own loss"), gives the client a ring it
  can't resize, and refuses every format but 48 kHz, 2 channels, 16 bits.
- **The fonts themselves.** libc's `/etc/fonts/fonts.conf` is fixed text,
  and what it includes is under `/pkg`, a read-only view of the package
  area, which no `mount /data rw` or `/jam` grant can write (PO5's guard on
  every writable view): the font files and the packages' `fonts.conf` are
  the installer's alone. What any program can write is what those files
  name on `/data` (finding 3).
- **Name records** (`/dev`, `/etc`, `/proc` files, kind 14) are opened in
  the receiver's own libc: their content is the receiver's own, and a
  `/proc` path for a pid the receiver can't see is ENOENT (note 1).

## Findings

| # | Sev | Where | What is wrong | The failure | Suggested fix |
|---|---|---|---|---|---|
| 1 | Medium | `user/include/net.h:73-81`, `90-95`; `user/include/dns.h:20-23`; `user/services/mixer/internal.h:79`, `user/include/mixer.h:41-49`; `user/lib/ns.c:673-683` (`own_channel`: `/svc/unix` alone); `user/lib/audio.c:328` (`audio_open_on`) | The shared services count by opener channel, and every holder of `/svc/net`, `/svc/dns` or `/svc/audio` may open as many openers as the tables have: 24 ordinary openers of netstack with 384 sockets, 768 connections and 96 MiB of rings between them (256, 512 and 64 MiB to one opener), 12 of the resolver, 24 of the mixer with 16 streams in all. CR2's Q2 gave `/svc/unix` an opener of each program's own; these three kept the shared channel. Every Chromium child holds all three (the browser's namespace), and Chromium's audio service is built to take all 16 streams (CA1: four openers of four) | A compromised renderer opens two `/svc/net` openers and 768 connections, or the 24 openers: the network service's next connection is `ERR_NO_RESOURCES` (pages stop loading) and so is every other program's on the machine; 12 resolver openers and no name resolves for anyone; 24 mixer openers or 16 streams and no program plays sound (the mixer's header says its cap "stops a runaway loop, not a program set on taking them all"). Chromium playing several pages' sound takes every stream legitimately, and `play` or the splash then gets none. The rings are netstack's pages (CN1 notes), so the memory a renderer holds there is out of reach of its job's limit, CR-f's milestone | Decided (Q1: A) and fixed by SB2 ([SANDBOX-PLAN](SANDBOX-PLAN.md#sb2-notes)): openers of each program's own for net, net-listen, dns, audio and audioctl, the mixer's 8 streams an opener; rings later (SBN). Suggested: Question 1: an opener of each program's own for net, dns and audio (`own_channel`), caps an opener can't fill alone, and the mixer's per-opener count raised so Chromium's audio service fits one opener; rings charged to the opener's job later |
| 2 | Medium | `user/posix/sys/unixsock.c:184-197` (`file_key`: an `O_RDONLY` open), `166-180` (`file_make`: a plain file, `0777` less the umask); `abi/idl/unix.idl` (a key is the whole check) | A socket file is a plain file holding a bearer key: reading it is enough to connect, and writing it decides which listener a path leads to. Linux asks for write permission on the socket inode to connect, and a socket inode can't be rewritten | (a) A program with a read-only view (`mount /data r`, `/pkg`) connects to a socket another program bound there. (b) A program that can write the directory (every POSIX program's shared `/tmp`; any `mount /data rw`) puts its own listener's key into another program's socket file, or makes the file first at a path the other will use (its bind is then `EADDRINUSE`, and a program like Chromium's `ProcessSingleton` reads that as "already running" and connects): every later connector reaches the impostor. For CB6: Chromium's singleton socket lives in a `/tmp` directory on Linux, so any POSIX program would receive each new launch's command line, or connect and make the running browser open URLs | Question 2: connect opens the socket file for writing (no truncation), so a read-only view can't connect; CB6 keeps the singleton's socket and directory under the profile on `/jam`, never in the shared `/tmp` |
| 3 | Medium | `ports/fonts-jamos/override/fonts.conf:24-25` (`cachedir` under XDG, then `/data/.cache/fontconfig`), `:121` (includes `/data/.config/fontconfig/fonts.conf`); `user/posix/sys/etc.c` (every program's default file includes that one); `user/posix/crt/start.c:167-173` (`FONTCONFIG_FILE` kept under AT_SECURE: CR2 Q1) | Every font user's fontconfig, the browser's included, reads a configuration file and cache files that any program with `mount /data rw` may write: today every Chromium child, and most approved programs | A program with `/data` writes `/data/.config/fontconfig/fonts.conf`: a `<dir>` with its own font and a `<match>` that makes `system-ui` and `sans-serif` that font, so the browser's address bar and every page draw the attacker's glyphs (a URL shown as another); a `<cachedir>` it chose, with cache files it wrote, which fontconfig maps and whose offsets it follows as trusted input (memory corruption in the browser, unproven here; fontconfig's cache checks are partial). A compromised renderer so reaches the next browser start. Through `/svc/exec`, a caller's `FONTCONFIG_FILE` points a raised font user at its own configuration | Question 4: caches made at install into the package area (read-only through `/pkg`), no writable shared `cachedir`, the owner's own font settings in a place a settings grant writes, and `FONTCONFIG_FILE` dropped for AT_SECURE children |
| 4 | Low | `user/posix/sys/fdpass.c:253-254`, `342-343` (`POSIXFDS_DIR` passes as `FDPASS_DIR`), `user/posix/sys/dir.c:789-801` (`dir_adopt_fd`), `sockpair.c:156-159` (the policy) | A text directory record (kind 2: FAT, the boot image, past a process's 64 channels) is believed over a socket: the receiver gets a directory description of whatever absolute path the sender wrote, resolved in the receiver's namespace with the receiver's rights. CU1's rule ("a path is only a name in ours") was applied to files and to kind 15's text, not to kind 2 | A sender names a directory it can't reach (a narrowed renderer naming `/jam/...`) and the receiver holds it. Harmless while no receiver acts on such a descriptor for its sender or hands it back as a channel: none does in CB5's flows (the browser sends the storage service its leveldb directories; nothing sends the browser one). Under CR-f a narrowed storage service receiving a text directory finds nothing there (its namespace lacks the path) | Question 3: refuse kind 2 over sockets (EINVAL at the send, the log line naming it), keep it for children |
| 5 | Low | `user/posix/sys/etc.c:348-359` (`etc_adopt`) and `152-170` (`block_text`: `fs_statfs_within` with `FILE_TIMEOUT`); `sockpair.c:594` (`deliver_fds`, under the description's lock) | A `/sys/dev/block/0:<n>/size` record makes the receiver call `fs.statfs` on its own mount number n while it adopts, inside `recvmsg` with the socket's lock held | A peer sends 253 such records naming the receiver's slowest mount (a fat stick, or a usb-storage volume whose calls are retried for up to 40 s): each `recvmsg`, `MSG_DONTWAIT` or not, makes 253 calls, and every other thread on that socket waits. CR2's finding 2 in another place; the services are the receiver's own, but slow | Make the text at the first read, not at the adopt (or refuse `/sys` files over sockets: nothing sends them) |
| 6 | Low | `user/posix/sys/sockpair.c:924-937` (`send_fds`: the records made anew each try), `user/posix/sys/file.c:539-559` (`give_ours` moves the only open) | A file unlinked while open (CB5's swap file) is sent by moving the sender's own open into the records. When the write is refused after the records were made (a part refused for room after the look, a multi-part send's later part on a full channel), the records are dropped, and the moved open with them | The retry (a blocking send's next round, or Mojo's resend after `EAGAIN`) gives the file again, finds no path and no open (`ENOENT`), and the send fails: Mojo drops the channel, CB5's stop 4 again. Needs a full channel at the wrong moment | Keep the moved handles until a write has taken them and give them back to the description otherwise |
| 7 | Low | `user/posix/sys/file.c:565-566` (`kind_give`: `ENOTSUP` for a file without its path) | A file received over a socket can't be given on (CU1's decision: no path, no reopen) | A process that forwards a descriptor it was sent (an ipcz node proxying a route while it is reduced, a broker) fails the send and loses its channel on the first file; memfds, channel ends and directories forward. Not met in CB5; CR-f's narrower processes may meet it | Give a received file on as duplicates of its channel and buffer (the receiver's access is already what it was sent) |
| 8 | Low | `user/posix/sys/proc.c:119` (`FDS_WAIT_NS` 3 s), `706-760` (`lx_child_fds`) | `/proc/<child>/fd` asks the child, and waits up to 3 s for an answer it may never send | A renderer that never answers `list_fds` holds each `getdents` of its fd directory 3 s (Chrome's task manager's descriptor column, base's `GetOpenFdCount`) | A shorter wait, or the last answer kept |

## Notes

1. **Name records resolve in the receiver's own libc** (`dev_adopt`,
   `etc_adopt`, `proc_adopt`). A sender can name `/proc/self/mem`, the
   starter's log (`dev.c`'s log name) or another child's `/proc/<pid>/maps`,
   and the receiver gets its own: its own memory, its own log, a child of
   its own. Nothing of the sender's crosses; given on, the record is
   resolved again by the next receiver.
2. **A directory channel from a socket is believed as a directory, whoever
   serves it** (`chan_adopt`): a peer can send a channel it serves itself,
   and the receiver's `*at` calls on that descriptor go to the peer (as
   Linux's go to whatever file system the descriptor is on). Its mount name
   is the sender's word (used for the receiver's features and read-only
   look-ups), and `..` past the view's root goes on in the receiver's own
   namespace, as Linux's `..` at a mount's root does. DR's review covers
   directory channels.
3. **Parts of an unfinished send are held** until another send's first
   part or plain bytes come, or the description closes
   (`sockpair.c:452`): up to 253 handles a description of a sender that
   died mid-send.
4. **The kernel log is ambient.** Every process may `debug_write`
   (rate-limited, each line marked with its writer) and `debug_report`,
   whose 160 lines are one count for every process together
   (`kernel/object/process.c:100`, `495`): a renderer can spend the test
   harness's results box. The log's reading needs `RIGHT_ROOT_KLOG`.
5. **A refusal at receipt is fatal to Mojo.** Mojo takes `MSG_CTRUNC` or a
   short handle count as a broken channel, as CB5's stops showed. Every
   adopter that can refuse at receipt (a `/proc` file of a pid the
   receiver can't see, an unknown `/dev` name, a full table) breaks the
   receiver's channel; CR-f's narrower namespaces make such refusals
   likelier. Adopting lazily, failing at first use, would keep the channel.
   Also: a mixer stream's holder may signal its own `MIXER_SIG_DATA`
   (`user/services/mixer/streams.c:483-497`), which makes an idle mixer open
   the output again, and `set_title` shows its text on the desktop: small
   things a renderer holding `/svc/audio` can do.

## What the sandbox (CR-f) must close

What a renderer holds today, after `fork` and the exec of
`/proc/self/exe` (CB5), that it doesn't need. Every child of the browser
(renderers, the GPU process, the network, storage and audio services)
holds the same; the last column says which of them needs each. Sources:
`exec_replace`'s `.ns = NS_ALL` and its extras
(`user/posix/sys/spawn.c:578-622`), `user/lib/spawn.c:466-530`
(`gather_handles`), `user/lib/fork.c:72-79` (`dup_roles`), and
`ports/chromium/wants/content_shell`.

| What | What it lets a renderer do | Needed by |
|---|---|---|
| `/data` read-write (the whole volume) | read and change the owner's files and every other program's data on it; plant font settings and caches the browser reads at its next start (finding 3); write socket files (finding 2) | none as a whole: the network service its own profile files, the rest through the browser (storage gets directories from it, CB5 stop 2) |
| `/jam` (CB6's profile) | the same on the Jam OS drive: the profile's cookies and logins | the browser; the network service its own files |
| `/tmp` (one for every POSIX program) | other programs' temporary files and socket files (finding 2) | none (shared memory is memfd) |
| `/pkg` read-only | read every package's files | the browser (fonts); renderers get fonts from the browser's font service |
| `/svc/net` | connect anywhere: the local network, services listening on loopback, the internet, with the page's data; use up the ordinary openers, sockets, connections and 96 MiB of netstack's memory (finding 1) | the network service |
| `/svc/dns` | resolve any name (and so send data out in names); use up its openers (finding 1) | the network service |
| `/svc/wayland` | make windows of its own (a fake dialog), the compositor's resources; the clipboard only with the keyboard focus and an input serial (`data.c`) | the browser; to check: Ozone/Wayland's GPU side hands its buffers to the browser over Mojo, so the GPU process may need none (section 13 of CR-PLAN gives it one; question 6) |
| `/svc/audio` | streams and openers for the whole machine (finding 1); a title on the desktop | the audio service |
| `/svc/unix` (CB6) | children share the browser's own opener (a grant by name, then the whole namespace): bind its 32 listeners so the browser's next bind fails; connect to any socket whose file it can read, the browser's own singleton too, which takes a connector's command line and opens its URLs | the browser |
| `SR_JIT` (every forked child: `dup_roles`; kept by the exec of `/proc/self/exe`: `spawn.c:597-601`) | make W+X JIT memory | renderers (not with `--jitless`); the GPU process with SwiftShader (CJR2 L2: the browser gives it up at start) |
| Its own job, with no limits (`user/posix/sys/fork.c:374-382`), and its own image | fork and spawn itself without bound, with any number of threads, pages, handles and queued message bytes | everyone needs a job; CR-f sets its limits (question 5) |
| The current directory as a directory channel (`exec_handover` passes it whole: `spawn.c:530`, `spawnfd_export_cwd(..., false)` in `user/posix/sys/spawnfd.c:319-349`) | a capability to the browser's working directory on `/data` or `/jam`, which a narrowed namespace would not take away | none: `/` as text when narrowed (DL2 already does so for narrowed spawns) |
| Descriptors 0 to 2 (Chromium's launcher keeps them in every child) | the browser's terminal when it was started from a shell (its `tty` and data channels: read what is typed there, change its modes, write to it), else the kernel log | none: the log, or nothing |
| The boot image (`SR_BOOTFS`, read-only) | read every boot-image program and file | none (every program has it today; it holds nothing secret) |
| Descriptors a peer gives (CU1) | give the browser, GPU or network service a directory by name (finding 4), make it call `fs.statfs` (finding 5), or a directory or file whose calls the renderer itself answers (note 2) | (inputs to watch, not grants) |
| Memory it puts in services' jobs | netstack's rings (up to 96 MiB), the mixer's rings, the broker's waiting connections (CR2's 15), outside its own job's limit | CR-f's limits don't reach these (question 1) |

Before the exec, the forked child holds everything the browser holds (all
its descriptors and mappings, `SR_CONSOLE`, `SR_STDOUT`): that is the
browser's own code until the exec, so CR-f's narrowing belongs in the
exec, where `process_replace` cuts away everything not handed over, and
not in a hook at the fork.

## Questions for the owner

**Q1 (finding 1): how should net, dns and audio count a program?**
Options: (A) as `/svc/unix` since CR2's Q2: a grant naming the service
gives the program an opener of its own (`own_channel` in
`user/lib/ns.c`), which makes no more; a program's children given its
namespace whole share it; each service's per-opener cap well under its
ordinary total (netstack: a quarter, say 96 sockets, 192 connections and
24 MiB of rings an opener). (B) the services count by the writer's pid or
job (the kernel's `CHANNEL_READ_PID`), which survives a program sharing
one channel among its processes but needs the services to keep a table
per pid. (C) as now: an approved program is trusted not to take them
all, and CR-f takes the grants away from renderers. For audio, under (A)
the Chromium back end can't open three more openers; the per-opener
stream cap would become 8 (half the mixer's 16), so Chromium's audio
service keeps 8 streams on its one opener and other programs keep 8.
Netstack's rings are its own pages whichever is chosen; charging them to
the opener's job means client-made ring VMOs that netstack reads with
`vmo_read`, as the mixer does: a larger change. My recommendation: (A)
for all three now (small, the same code path as `/svc/unix`), the audio
cap 8, and the rings' charge as a later netstack track that CR-f's plan
names.

**Q2 (finding 2): what should connecting to a socket file need?** Options:
(A) libc's connect opens the socket file for writing (`O_RDWR`, no
truncation): a read-only view can't connect, as Linux asks for write
permission; rewriting another program's socket file still needs the write
access that (A) asks for anyway, so CB6 must keep the singleton's file in
a directory only the browser writes. (B) the broker binds a key to the
file's identity (libc sends the volume and id with the key at bind and at
connect), so a key copied into another file connects nobody; a rewrite of
the same file still does. (C) as now, documented. My recommendation: (A),
and CB6 puts the singleton's socket and its directory under the profile on
`/jam`, not in `/tmp`; (B) only if a port shares socket directories.

**Q3 (finding 4): should a text directory cross a socket?** (A) refuse
kind 2 over sockets (the sender's `sendmsg` EINVAL, its log line naming the
path): only FAT, the boot image and a process past its 64 directory
channels make them, and the browser's profile is on ext; (B) keep it, and
list it for CR-f. My recommendation: (A) once CB6 shows the browser never
sends a text directory on its profile (a profile on a FAT `/data` would
then break the storage service's fsyncs: CB5's stop 2), else (B) until
then.

**Q4 (finding 3): where do fontconfig's caches and the owner's font
settings live?** My recommendation: bin/pkg builds the caches when it
installs a fonts package (fontconfig's cache tool, run at install, into the
package area, read-only to everyone through `/pkg`); fonts-jamos's
`fonts.conf` names that cache directory and no writable one (a program that
wants a private cache sets `XDG_CACHE_HOME` to its own data: the browser's
profile); the owner's own font settings come from a place a settings grant
writes, not `/data/.config`; and the crt drops `FONTCONFIG_FILE` and
`FONTCONFIG_PATH` for AT_SECURE children (since FONTS, every program finds
the system fonts without them).

**Q5: where does CR-f narrow a child that Chromium forks and execs, and
who sets its job's limits?** The type is known only at the exec
(`--type=renderer` in its argv); the child's job is made at the fork and
an exec in place keeps it (`process_replace` keeps the process's job), and
the child holds its job without `RIGHT_MANAGE`. Options: (A) libc's execve
in the child (still the browser's own code) looks the type up in a
per-type table: the namespace `exec_replace` sends, `SR_JIT` only for
renderers (and the GPU process with SwiftShader), the current directory
`/` as text, descriptors 0 to 2 on the log; and it asks its parent once,
on its signal channel before its exec notice, to set that type's limits
on its job (the parent's libc ignores a second ask and any ask after the
notice); (B) the fork sets one set of limits on every forked child of a
program marked sandboxed, and the exec narrows the namespace by type; (C)
a launcher patch calling `posix_spawn` with grants and a job attribute
(section 13's first idea; against the owner's no-launcher-patch rule).
My recommendation: (A); it relies on the exec's clean cut, and its one new
message is answered by trusted code on both sides.

**Q6: does the GPU process need `/svc/wayland`?** Section 13 gives it one.
My recommendation: check before CR-f's table is written: in Chromium's
Ozone/Wayland the GPU side passes its buffers to the browser over Mojo, and
the browser alone talks to the compositor; if so, the GPU process gets no
`/svc/wayland` (crmulti and crwebgl would show it).

## Tests

This review ran nothing (findings only). Each fix should come with a test
that fails without it:

- finding 1: a utest program that opens `/svc/net`, `/svc/dns` and
  `/svc/audio` openers until refused, then a second program that must
  still get one of each;
- finding 2: posixtest unix: a child with a read-only view of a directory
  holding a socket file can't connect; a rewritten socket file is refused
  (whatever Q2 decides);
- finding 3: the fonts case with a `/data/.config/fontconfig/fonts.conf`
  that maps sans-serif to the cut Inter: the browser must not pick it up;
- findings 4 to 6: posixtest scm: a text directory record from the peer
  section's fuzzer, 253 `/sys` size records received with `MSG_DONTWAIT`
  within a deadline, an unlinked file sent on a channel kept full until
  the first try is refused;
- finding 8: posixtest procinfo: a child that never answers, and a
  `getdents` of its fd directory within the new bound.
