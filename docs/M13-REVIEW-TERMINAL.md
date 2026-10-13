# M13 R2: the terminal, sessions, SSH and the shell

M13's track R2 ([M13-PLAN.md](M13-PLAN.md), "Reviews and the join"): the
independent review of T1, T2, X1, X2, S1 and S2, at main bf129f4e, by an
agent that wrote none of it. Read in full:

- **T1 and T2**: the console's front end and both back ends
  (`user/services/console/`: `main.c`, `clients.c`, `keys.c`, `tty.c`,
  `paste.c`, `conn.c`, `connin.c`, `winback.c` and the window's drawing
  where it decides when a frame goes out), the line discipline
  (`user/lib/ldisc.c`, `<ldisc.h>`), keys into bytes (`user/lib/keybytes.c`),
  `abi/idl/console.idl`'s `tty` protocol and libc's terminal descriptor
  (`user/posix/sys/tty.c`);
- **X1**: init's sessions (`user/services/init/sessions.c`), the `session`
  protocol, libc's pseudo-terminals and `jamos_session_open`
  (`user/posix/sys/pty.c`), `allow`'s refusal of `svc session`
  (`user/services/shell/sh_allow.c`);
- **X2**: init's host key and sshd's start (`user/services/init/ssh.c`,
  its grants in `user/services/init/shell.c`), the tinyssh port's own files
  and overrides (`user/ports/tinyssh/`: `sshd.c`, `helper.c`, `tinysshd.c`,
  `channel.c`, `subprocess_auth.c`, `subprocess_sign.c`), and where the
  namespace's views (`user/lib/ns.c`, `user/lib/fsview.c`) decide what
  sshd and its children can read;
- **S1 and S2**: the shell's programs, descriptors, redirections, pipelines
  and Ctrl+C (`user/services/shell/sh_program.c`, `sh_posix.c`,
  `sh_exec.c`, `sh_parse.c`, `sh_io.c`, `sh_jobs.c`), and the quiet-run
  change (bca66e2a, f49f93f4: `run:` lines only with the boot word
  `runlines`, a command given arguments it doesn't take refused).

Looked at first, as the plan asks: who can get a shell (`svc session`, its
refusal, sshd's grants), the host key's handling, the line discipline's
bounds, the console's two modes; and the owner's four findings from the
real PC on 2026-10-11, each checked against the code (items 1 to 4 below).

Severity, as in the other M13 reviews: **High** is a crash, a hole, or a
hang or breakage that a program can cause for others; **Medium** is wrong
behaviour the system or a test tier will meet, or a CODING-GUIDE rule
broken on a path a hostile program reaches; **Low** is a wart, a narrow
case, or a design point.

Counts: **1 High, 5 Medium, 6 Low.** The High and the Mediums are fixed,
one commit each with a test that fails without it; finding 4 (no defect)
gets the test it was missing; the Lows are reported.

## Findings

| # | Sev | Where | What is wrong | Failure scenario | Fix | Outcome |
|---|---|---|---|---|---|---|
| 1 | Medium | `user/services/console/tty.c:179` (out_turn's budget) against `clients.c:166` (client_event) | **The owner's "last output late".** A POSIX program's output reaches the console as messages on its tty's data channel and is shown in turns of at most 32 messages or 10 ms (`ttys_pump`), while a client's `console.write` (the shell's, a call) is served whole at the next packet. Whatever the program left queued when it ended is shown after the shell's lines that follow: its `run:` line, `echo` after `;`, the prompt. | `pxio seq 3000; echo done` (or `cat` of any file of more than a screen) in a window or over SSH: the program ends with up to 1,024 messages still queued (the channel's cap); the shell sees the end and writes; the console shows `done` and the prompt, then the last few hundred lines of the file under them. The tail looks late, or lost above the prompt once it scrolls. Short outputs are right only because the port's packets happen to be in order. | Before it serves any client request, the console shows the tty output already queued (at most a channel's 1,024 messages per tty, so a program writing flat out can't hold the shell's writes back for longer than its queue), unless the back end is congested, in which case the request waits as it does for congestion today. | Fixed (utest `con_tty_order`; posix.txt `seq-order`) |
| 2 | Medium | `user/services/shell/sh_posix.c:174` (open_source, SPEC_TERM) | **The owner's "typing into a pipeline's first stage".** Every POSIX program the shell starts opens its own `console.open_tty`, and the console gives the keys to the newest tty. In `a \| b` both are started at once and `b`'s tty is the newest, so what is typed goes to `b`'s terminal even when `b` reads the pipe and `a` reads the terminal (S2's notes list it as a known limit). | `pxio cat \| pxio count`, then typing `one`, `two`, Ctrl+D: nothing reaches `cat`, `count` waits on the pipe for ever, only Ctrl+C ends the line. Every `prog \| filter` where `prog` reads the keyboard. | One terminal for the line, as on Unix: the first program of a pipeline that needs its terminal opens it, the shell keeps a duplicate of both handles, and every later program of the same line is handed duplicates of the same pair (the same tty client in the console, so the same focus, line discipline and type-ahead). The shell lets its copy go when the line's programs have all ended (`sh_pipe_settle`). | Fixed (posix.txt `pipe-typed`) |
| 3 | High | `user/lib/ns.c:605` (parse_grant: `:r` is `FS_VIEW_READ_ONLY` alone), `user/lib/fsview.c:62` (fs_view_check only judges changes), `user/services/init/ssh.c:99` (`/data/etc/ssh/.ed25519.sk`) | **The owner's "sshd's host key is readable by anything that can read /data".** A view guards `etc` against changes only; reads are never refused. Every program whose list says `mount /data r` or `rw` (or `mount * r`), every program the owner `allow`s from /data, and every POSIX program such a program starts can open `/data/etc/ssh/.ed25519.sk`, the SSH server's secret host key. | A program run from the shell (a port, a game, anything allowed from /data) reads the 64-byte key and sends it off over its own network service, or prints it; whoever has it can pose as this machine to the Mac's `ssh` (the fingerprint the owner pinned in known_hosts matches), and see or change everything typed in a session. | A new view flag, `FS_VIEW_HIDE_KEYS` (4): every request at or under the top-level `etc`'s `ssh` directory (open for reading too, stat, readdir, and the changes) is `ERR_ACCESS_DENIED`, with `etc` and `ssh` compared as FAT compares them (case, trailing dots and spaces, leading spaces). Every view a grant makes (`:r`, `:w`) has it; a new grant form, `:rk`, a read-only view that shows the keys, is written only in C by the two that need it: init's grants for sshd and sshd's for its tinysshds and their helpers (no list can ask for it: `<wants.h>` has `r` and `rw` only). A view is never wider than its channel, so a program given `:r` can't get `:rk` for a child. The whole directory, not the one file: FAT's short names (`ED2551~1.SK`) would let a request by another name past a check on the file's name. | Fixed (utest `views_hide_keys`, `ns_grant_keys`) |
| 4 | Low | `user/services/init/shell.c:371` (ended: the service's job killed), `user/posix/sys/spawn.c` (children in the spawner's job), `user/services/netstack/tcpsock.c:593` | **The owner's "killing sshd should end its sessions".** Checked: it does. `kill sshd` kills the listener; init's `ended` kills its job, which holds every tinysshd (posix_spawn puts children in the spawner's job) and their helpers; each tinysshd's end closes its session's master, so the session's console hangs up and its shell ends (2), and init kills the shell's job 2 s later; netstack aborts the connections the listener's opener made (a reset to the client). init starts sshd again. No test covered it, and the plan's sign-off still said the sessions go on (the owner's answer is the other way). | None today. Without a test, a change that moved tinysshd out of sshd's job (an opener per connection, X2's idea for keeping sessions) would quietly change it. | sshcheck.py gains the case: an interactive session runs `kill sshd` on itself; the client's connection ends, the server comes back, and `ps` from a new login shows nothing of the old session. The sign-off line in M13-PLAN.md says what the owner chose. | Test added (ssh-test.sh `kill sshd`) |
| 5 | Medium | `user/services/console/tty.c:583` (tty_event), `user/services/console/clients.c:171` (client_event) | The console serves a tty channel's and a client channel's requests without the unread bound (CODING-GUIDE, "A loop that serves never blocks": ask `<proto>_unread_full` before each `serve_one`). | A POSIX program writes `tty.get_attr` requests (or `console.size`, on its console channel) and never reads the answers: the console writes up to 1,024 answers into each of its channels (8 ttys, its client channel), all charged to the console's job, and keeps serving it. The memory is bounded but the console pays for it; past the job's budget every client's answers fail, the shell's `console.write` among them. | A client or tty that has left its protocol's bound of answers unread broke the protocol: the tty is hung up (as for an output message past `TTY_OUT_MAX`), a client channel closed (as when its client goes). | Fixed (utest `con_tty_unread`) |
| 6 | Medium | `user/services/shell/sh_program.c:607` (start_program: `args[20]`) | A program gets at most 18 arguments after its name; the rest of the line's words (up to `SH_MAX_WORDS`, 64) are dropped without a word. | `pxio args 1 2 ... 25`, or any ported utility given many files: `rm`, `cat`, `grep pat f1 ... f20` get the first 18 and report success on those; `cp a1 ... a18 a19 dir/` copies onto `a18` instead of into `dir`. | The argument array holds every word a line can have (`SH_MAX_WORDS`); spawn refuses a message too big for it, which is said (`run: can't start ...`). | Fixed (posix.txt `args-25`) |
| 7 | Low | `user/services/shell/sh_exec.c:81` (exec_argv) | The quiet-run change refuses arguments to any command whose usage has no space: since f49f93f4 the commands that take them say so (`utest [only <prefix>]`). Left: `true` and `false` (POSIX's ignore any arguments, and scripts write `true x`), `env` (POSIX's runs a command: `env A=1 prog` is now "takes no arguments" rather than ignored, which is the better of the two). | A line copied from a Unix script with `true something` fails with status 2. | Design question 3. | Reported |
| 8 | Low | `user/services/init/sessions.c:337` (start_shell) | `session.open` waits for the new console's `console.new_client` answer (up to `START_WAIT`, 2 s) inside init's loop, against "a loop that serves never blocks". | The console is init's own fresh process and answers in microseconds, so only a machine under heavy load sees init stall for the wait; an opener can make at most 8 sessions at once. | Not changed: making it a `later` method is the clean fix (the opener's answer after the console's). | Reported |
| 9 | Low | `user/ports/tinyssh/sshd.c:109` | tinysshd, the process that parses the network's bytes before any login, holds `/svc/session` and (after the fix for 3) a view that shows the host key. tinyssh's own design keeps the secret key out of that process's memory (the signing helper), not out of its reach: upstream's tinysshd runs as root. | A memory-safety bug in tinyssh's pre-login parser would be a shell and the key, as upstream's would be root. | Design question 2: give the session only after login (a channel sshd hands over once the auth helper said yes) and the key only to the sign helper. | Reported |
| 10 | Low | `user/services/console/tty.c:630` (ttys_resized), with 2's fix | The `tty` channel's events (the size, the `signal` event) are read by whichever process reads first; with one terminal for a pipeline, a resize's SIGWINCH reaches one of its programs, not each. | `ls -R / \| less` resized while `ls` still runs: `less` may not redraw. | Note for the join: an event per reader needs the console to know the readers (Unix's process group). | Reported |
| 11 | Low | `user/services/console/connin.c:143` | Over a connection, a message from the master (up to 64 KiB) goes into the line discipline whole before `ttys_deliver` runs, so in raw mode what is past `LD_READY_MAX` (16 KiB) in one message is dropped even when the reader keeps up. | Pasting more than 16 KiB at once into a raw-mode program over SSH loses the tail (the window's paste is bounded the same way: T2's notes). | Typing in pieces of the ready queue's room, delivering between them. | Reported |
| 12 | Low | `abi/idl/console.idl` (open_tty), `user/services/console/tty.c:492` | A program can open all 8 of the console's ttys on its PROGRAM channel; while it runs, the next program the shell starts on that terminal can't get one (`sh: can't set up the program's terminal`). | Only a foreground program, only while it runs (its job's end closes them). | A per-client cap (2) would leave room. | Reported |

## Not findings (checked and correct)

- **Who can get a shell.** `/svc/session` is in init's namespace and the
  shells' (NS_ALL); a program gets it only through its list's `svc
  session`, which `tools/checkwants.py` allows only for services, ports and
  tests in the boot image and `allow` refuses for a program on /data
  (`sh_wants_refused`, also checked at each start of a /data program in
  `sh_dataprog.c`). sshd opens a session only from tinyssh's channel
  request, which upstream accepts only after the user's key was checked
  (the auth helper's exit code over `/data/etc/authorized_keys`, account
  `jam` alone) and the signature verified. A session's shell gets what a
  terminal's shell gets, no more (`terms_shell_powers`, NS_ALL), and the
  opener gets the shell's process without `RIGHT_WRITE`.
- **The line discipline's bounds.** The line is `LD_MAX_CANON` with room
  for the delimiter, the ready queue a ring with every push checked
  against both its bytes and its record count, `ld_unread` against
  `LD_REC_MAX` (so the console's `msg` buffer always holds a record),
  erase walks back over UTF-8 continuation bytes only inside the line.
  Switching to raw mode moves the half-typed line in through the same
  checks. No length from a client reaches it unchecked: the console reads
  input at most `CHANNEL_MAX_BYTES` and output at most `TTY_OUT_MAX`.
- **The console's two modes.** The connection back end never drops program
  output (the ring and `congested`), drops only kernel lines, with a
  notice; a hang-up ends the loop; `endidle` waits for the ring to empty
  and for every tty to be released, so the master gives every byte before
  its peer closes. A tty that closes shows what it wrote before it goes
  (`tty_closing`), and messages written before a close stay readable
  (the kernel keeps a closed writer's messages on the reader's queue).
- **Sessions' lifetimes.** Slots, openers, the grace and the reuse of an
  ending session's slot are consistent; every handle of a session is closed
  in `slot_free`; a stale packet is told apart by asking the process.
- **The redirections** (`sh_parse.c`, `sh_posix.c`): descriptor numbers
  bounded before use, at most `SH_MAX_REDIR` (the state's file slots
  match), a file opened once and its open given to the program, every
  source's handles closed on a failure.
- **The quiet run** (bca66e2a with f49f93f4): `runlines` reaches init
  (`init_words`, kept by kexec; `USERBOOT_MAX_WORDS` 10 holds every word
  that can come at once) and every shell init starts (terminals, sessions);
  the scripts' `run:` lines come on every test boot.

## Questions for the owner

1. **The host key's view (finding 3).** The fix hides `/data/etc/ssh` from
   every view but the SSH server's, with a grant form (`:rk`) only C code
   writes. Other secrets under `etc` (the shell's history, `allow`) stay
   readable by programs with `/data`: hide all of `etc` from views instead,
   with sshd's `:rk` and logd's needs checked? (`settings` is read by
   programs today.)
2. **tinysshd's reach (finding 9).** Worth splitting so the pre-login
   process holds neither the session service nor the key view?
3. **`true` and `false` with arguments (finding 7)**: accept and ignore
   them, as POSIX does?
