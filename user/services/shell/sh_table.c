/* The command table: every command the shell has, in the order `help`
 * lists them within a category, with its usage and help (the first line
 * of the help is what the list shows; `help <command>` shows it all). */
#include "sh.h"

enum { C_INFO, C_FILES, C_TEXT, C_SHELL, C_SYSTEM, C_TESTS, C_COUNT };

const char *const sh_categories[C_COUNT] = {
    "Information", "Files (/boot: the boot image, read-only; /data: the stick)",
    "Text (read a file or a pipe)",
    "Shell", "System", "Tests",
};
const unsigned sh_ncategories = C_COUNT;

#define C(n, cat, usage, help) { #n, shc_##n, cat, usage, help }

static const struct sh_cmd cmds[] = {
    C(help, C_SHELL, "help [command]", "the commands by category, or one command's usage"),
    C(uname, C_INFO, "uname [-a]", "the system's name (-a: with version, machine and CPU)"),
    C(version, C_INFO, "version", "the Jam OS version"),
    C(uptime, C_INFO, "uptime", "the time, how long since boot, CPU use since boot"),
    C(date, C_INFO, "date [-u] [-d @secs] | -r | -z <zone>",
      "the date and time, in $TZ or else the system's zone (the setting timezone).\n"
      "  -u: UTC. -d @secs: that Unix time. -r: the real-time clock as it reads, and\n"
      "  the system's clock (init sets it from the RTC at boot; the setting rtc says\n"
      "  whether the RTC keeps local time, as Windows sets it, or UTC). -z: the\n"
      "  system's zone from now on, kept in /data/etc/settings. Zones: Australia/Sydney\n"
      "  (AEST/AEDT), Australia/Perth, Europe/London, ..., UTC, or an offset like +10,\n"
      "  +9:30, -5. Example: TZ=UTC date"),
    C(lscpu, C_INFO, "lscpu [-e]",
      "the CPU: model, P-cores, E-cores, threads (-e: one line per CPU)"),
    C(free, C_INFO, "free", "memory: total, used, free"),
    C(ps, C_INFO, "ps [-k]",
      "processes: id, threads, CPU time, memory of its job, name (indented by job).\n"
      "  -k: the kernel's own listing (jobs with pages, handles, threads) in the log"),
    C(top, C_INFO, "top [-d seconds] [-n frames]",
      "live CPU use per CPU and per process, and memory; q or Ctrl+C quits"),
    C(whoami, C_INFO, "whoami", "the user (there is one: jam)"),
    C(hostname, C_INFO, "hostname", "this machine's name"),
    C(dmesg, C_INFO, "dmesg", "the whole kernel log (up to 4 MiB); pipe it: dmesg | grep usb"),
    C(history, C_SHELL, "history", "the lines typed (up/down recall them)"),
    C(jobs, C_SHELL, "jobs",
      "the programs started with & (prog &, run prog args &): number, pid, state,\n"
      "  command. Such a program runs on while you type; it gets no keys and no\n"
      "  screen (Ctrl+C is the foreground program's), and what it prints is shown as\n"
      "  it comes. When it ends, the next prompt says so: [2] done: utest (exit 0).\n"
      "  kill %2 ends it; all of them end with the shell. At most 8 at once; a\n"
      "  pipeline, a shell command or an alias can't go in the background"),
    C(term, C_SHELL, "term [command]",
      "open another terminal: a window of its own with a shell of its own (as\n"
      "  Super+Enter does); with a command, its shell runs it first, as if typed,\n"
      "  and stays. Needs the compositor (not a nocomp boot); at most 8 terminals\n"
      "  in all. Close one with its window's close box or exit"),
    C(launch, C_SHELL, "launch <app>",
      "start one of the desktop's apps as its search box does (jamjar): init starts\n"
      "  only those, from the boot image, with what each one's list asks for and a\n"
      "  window, no terminal; what it prints goes to the log"),
    C(notify, C_SHELL, "notify [-b button]... [-w] <title> [body...]",
      "a notice on the desktop: a card in the top right that fades after 5 s, or\n"
      "  with buttons (-b, up to 3) stays until one is pressed. -w: wait for it and\n"
      "  say which (Ctrl+C stops waiting). Needs the compositor (not a nocomp boot).\n"
      "  Example: notify -b Yes -b No -w Tea? Kettle is on"),
    C(exit, C_SHELL, "exit",
      "end this shell and close its terminal (any of them; Super+Enter opens\n"
      "  another). The programs it started with & end with it. Without the\n"
      "  desktop (a nocomp boot) the one terminal stays"),
    C(devices, C_SYSTEM, "devices", "PCI functions and the drivers devmgr bound (alias lspci)"),
    C(usb, C_SYSTEM, "usb", "USB devices from usb-bus (alias lsusb)"),
    C(hda, C_SYSTEM, "hda [gain [dB] | bits [n] | jacks]",
      "the HD Audio codecs and their widget graphs, read now by drv/hda\n"
      "  (the lines it logged at boot; pipe it: hda | grep pin), then the path to the\n"
      "  headphones, the gain it plays at and the jacks. hda gain: the gain; hda gain\n"
      "  -20: set it (rounded to the amp's step, at most 0 dB; the driver starts at\n"
      "  -30 dB). The path is unmuted only while a stream plays (beep). hda bits 16:\n"
      "  the largest sample size. hda jacks: each jack plugged in or not (the driver\n"
      "  logs every change: dmesg | grep plugged)"),
    C(beep, C_SYSTEM, "beep [hz] [ms]",
      "a tone in the headphones (default 440 Hz for 300 ms; 20-20000 Hz, up to 5000 ms),\n"
      "  at a quarter of full scale with 5 ms fades, through the mixer at `hda gain`.\n"
      "  Ctrl+C stops it. Turn the headphones' own volume down before the first one"),
    C(play, C_SYSTEM, "play [-v dB | -n] <file.wav|file.mp3>",
      "play a WAV or MP3 file in the headphones (e.g. play /data/song.mp3), at `hda\n"
      "  gain`. PCM WAV: 8-, 16-, 24- or 32-bit, mono or stereo, 8000-192000 Hz. MP3\n"
      "  (and MP2): CBR or VBR, mono or stereo, 8000-48000 Hz; ID3 tags skipped. Both\n"
      "  resampled to 48 kHz. -v -20: 20 dB down for this file only. -n: decode only,\n"
      "  and say how long it took. Ctrl+C stops it. Copy files to the stick from a Mac"),
    C(vol, C_SYSTEM, "vol [<id>|master <dB>]",
      "the mixer's volumes: alone, the master and every stream playing (id, volume,\n"
      "  state, underruns, name); vol 3 -6: stream 3 at -6 dB; vol master -10: all of\n"
      "  them. 0 dB is the most, -96 dB is silence. hda gain is the codec's level below"),
    C(music, C_SYSTEM,
      "music start [folder] | stop | next | prev | pause | status | vol <dB> | sleep <min>",
      "the background music player: plays every .mp3 and .wav under a folder\n"
      "  (default /data/music, any depth) in shuffle, forever, while the shell goes on.\n"
      "  start again: the new folder instead. stop: stops it (Ctrl+C doesn't). next:\n"
      "  skip the track playing; prev: back one (or to its start after 3 s); pause:\n"
      "  pause or go on. status: the track (Artist - Title, from its path), how far\n"
      "  in, the folder, how many tracks. vol -10: its volume (0 dB the most). sleep\n"
      "  30: stop in 30 minutes, fading out (sleep off). jamjar: the same in a window.\n"
      "  Each track's start is a line in the log. e.g. music start /data/music/OnTheSpot"),
    C(jamjar, C_SYSTEM, "jamjar [root=<folder>]",
      "the music player's window (bin/jamjar): the library (artists, albums, tracks)\n"
      "  from /usb0/music or /data/music, search, play, pause, next, back, volume, a\n"
      "  sleep timer, jam roulette; keys and the mouse, ? for the keys. q quits it and\n"
      "  the music plays on (it drives the same player as `music`)"),
    C(net, C_SYSTEM, "net [stats]",
      "the network: the address, gateway and DNS servers, the link (speed, VLAN,\n"
      "  MAC) and the frames in and out, as netstack sees them. net stats: every count\n"
      "  netstack keeps and the network card's own. The address comes from DHCP\n"
      "  (bin/dhcp's lease is in the log), or from net.address in /data/etc/settings\n"
      "  (e.g. 10.2.21.50/24 10.2.21.1 10.2.21.1; then there is no DHCP)"),
    C(ping, C_SYSTEM, "ping <address|name> [-c count] [-s size]",
      "ICMP echo requests to an IPv4 address, one a second (default 4, 56 data\n"
      "  bytes): a line per reply with its round trip and TTL, or \"no reply\" after\n"
      "  1 s, then a summary. A name is resolved first (as `host`, its first\n"
      "  address). Ctrl+C stops it. e.g. ping 1.1.1.1 -c 10, ping one.one.one.one"),
    C(host, C_SYSTEM, "host <name>",
      "a name's IPv4 addresses from the resolver (bin/dns, asking the DNS servers\n"
      "  `net` shows: from DHCP, or net.address in /data/etc/settings), and how long\n"
      "  they may be kept. Ctrl+C stops the wait. e.g. host one.one.one.one"),
    C(fetch, C_SYSTEM, "fetch <url> [file | -]",
      "download a file over plain HTTP (http:// only: https needs TLS, which Jam OS\n"
      "  doesn't have yet), e.g. fetch http://10.2.21.174:8000/big.bin. Into the URL's\n"
      "  last name here (or file, or a folder; -: the output), or into a pipe: fetch\n"
      "  <url> | head. Follows 3 redirects, says progress, the size, time and MB/s;\n"
      "  Ctrl+C stops it. Saved as <file>.part until it is whole (bin/fetch)"),
    C(serve, C_SYSTEM, "serve [<file> [port] | stop [port]]",
      "serve one file over HTTP to any computer on the network, in the background\n"
      "  (bin/serve, a service init runs: the shell stays free), e.g. serve\n"
      "  /data/big.bin, then on the Mac: curl http://<address>:8080/ -o big.bin. Every\n"
      "  path asked gets that file (GET and HEAD, ranges); nothing else is ever served.\n"
      "  Port 8080 unless given (any, 80 too); up to 4 files on 4 ports. serve alone:\n"
      "  what is served (file, port, clients, requests, bytes). serve stop [port]:\n"
      "  stop it (all of them without a port). One log line per request"),
    C(speed, C_SYSTEM, "speed <host> [port] [-r] [-u] [-t seconds] | -l [port]",
      "network throughput against tools/speed.py on the Mac (`python3 tools/speed.py\n"
      "  server` there; port 5201): sends for 5 s (-t) over TCP and says MB/s and\n"
      "  Mbit/s; -r: the Mac sends, this receives; -u: UDP datagrams instead (lost ones\n"
      "  counted). -l: wait for the Mac's `speed.py client <address>` (needs the listen\n"
      "  permission: bin/speed's list has it). Ctrl+C stops it (status 130). QEMU's\n"
      "  numbers are QEMU's"),
    C(pci, C_SYSTEM, "pci", "the kernel's PCI report: BARs, MSI/MSI-X (the old Devices entry)"),
    C(memmap, C_SYSTEM, "memmap", "the loader's memory map"),
    C(iommu, C_SYSTEM, "iommu",
      "the IOMMU (VT-d): the units, DMA translation, interrupt remapping and the\n"
      "  invalidation queue, each device's domain and mapped pages, the DMA fault\n"
      "  counts and the interrupt remapping table. Empty-looking without iommu=on"),
    C(log, C_INFO, "log [lines]", "the last lines of the kernel log (default 20)"),
    C(mem, C_SYSTEM, "mem", "physical memory from the kernel, and the shell's job"),
    C(kill, C_SYSTEM, "kill <name> | %<n> | <pid>",
      "kill the first process with that name (see ps): a service init runs or a\n"
      "  USB driver (hid-6.1:0); whoever supervises it starts it again.\n"
      "  kill %2, or the pid `jobs` shows: end background program 2 (prog &)"),
    C(clear, C_SHELL, "clear", "clear the screen (also Ctrl+L)"),
    C(reboot, C_SYSTEM, "reboot [-f]",
      "restart the machine into the kernel on the stick (or the one `update` loaded),\n"
      "  by kexec (no firmware); -f: through the firmware and the boot menu (the\n"
      "  stick's build). /data is synced first"),
    C(kernel, C_SYSTEM, "kernel load",
      "read /esp's kernel and boot image now and store them for the next reboot\n"
      "  (and a panic): after `make flash`, load, then `reboot` reads nothing"),
    C(update, C_SYSTEM, "update [-n | -m | -w] [-r] [-f] [server address]",
      "take the build the Mac serves: on the Mac `make`, then tools/update-server.py\n"
      "  left running; here `update` fetches it (bin/update), init checks its signature\n"
      "  (the key in this build) and each file's size and SHA-256 (old -> new is shown),\n"
      "  writes it and its boot menu to the stick (the stick's build kept as \"Jam OS\n"
      "  (previous build)\") and loads it, then stops: `reboot` starts it now (kexec),\n"
      "  `reboot -f` through the firmware (the stick boots it too). The server is\n"
      "  net.host in /data/etc/settings (e.g. 10.2.21.174).\n"
      "  -m: into memory only, the stick untouched: `reboot` runs it, a power-off\n"
      "  brings back the stick's. -n: fetch and check only, nothing loaded or written.\n"
      "  -w: the same as plain `update`. -r: reboot into it at once. A build whose\n"
      "  network default (VLAN 21 or untagged: local.mk) isn't this one's is refused;\n"
      "  -f takes it"),
    C(run, C_SYSTEM, "run <prog|path> [args]",
      "start /boot/bin/<prog> (or a path), wait, say how it ended; Ctrl+C kills it.\n"
      "  Typing a program's name does the same. Exported variables are its environment;\n"
      "  in a pipe its printf output goes down the pipe: run utest | grep passed.\n"
      "  It gets what its list asks for (services, mounts) and its terminal. A program\n"
      "  on /data runs once `allow` has marked it, and only as the file was then.\n"
      "  run prog &: in the background (see jobs)"),
    C(allow, C_SYSTEM, "allow <file> | -l | -r <name>",
      "let a program on /data run: shows what it asks for (services, mounts) and\n"
      "  asks y/n; y keeps its hash and list in /data/etc/allow. A changed file is\n"
      "  refused until allowed again. -l: the allowed ones. -r: take one back"),
    C(ktest, C_TESTS, "ktest [prefix] [options]",
      "kernel tests (as the boot menu's All tests).\n"
      "  loops=N: the set N times. seed=S: in the order shuffled from S (loop k uses\n"
      "  S+k-1; its seed replays it). shuffle: a seed from the clock. keep: record a\n"
      "  failure and go on (else the first one panics). load: with the stress test's\n"
      "  threads running, two per CPU (load=N: N); tests that need an idle machine\n"
      "  are skipped"),
    C(soak, C_TESTS, "soak [minutes] [loops=N] [seed=S]",
      "soak test: the kernel tests again and again in a shuffled order under load,\n"
      "  utest between the loops, files written and read back on /data and any writable\n"
      "  stick (pull and plug sticks while it runs), an idle loop before and after;\n"
      "  ends with SOAK RESULTS. Default 3\n"
      "  minutes; loops=N: N loops instead. seed=S: the first loop's order (each loop\n"
      "  prints its seed). halt: stop on the panic screen at the first failure.\n"
      "  load=N: N kernel load threads (default two per CPU). idle: no load.\n"
      "  Ctrl+C ends it after the step in progress"),
    C(bench, C_TESTS, "bench", "kernel benchmark"),
    C(stress, C_TESTS, "stress <seconds>", "stress test (1..600)"),
    C(storm, C_TESTS, "storm <from> <to> <kills/s> [service...] | storm mixer <kills/s> <seconds>",
      "copy a file while services are killed at a fixed rate, then say whether the\n"
      "  copy is whole: the bytes, MB/s, the kills, kill to first answer (median, p99,\n"
      "  worst: from the kill to the killed service's first answer to a call), both\n"
      "  files' SHA-256 read back, MATCH or DIFFERENT. Killed by default: each side's\n"
      "  filesystem service (fat-usb0, fat-data), taking turns; 0 kills/s: none, the\n"
      "  baseline. e.g. storm /usb0/big.bin /data/big.bin 10. storm mixer 2 60: the\n"
      "  mixer twice a second for a minute while music plays, then the kills and the\n"
      "  mixer's own restart lines (the least audio left written ahead, late periods).\n"
      "  The result line goes to the log too. Ctrl+C stops it"),
    C(utest, C_TESTS, "utest", "the user-space test suite (bin/utest) and its result line"),
    C(usbtest, C_TESTS, "usbtest", "the USB checks (bin/usbtest) and their result line"),
    C(hdatest, C_TESTS, "hdatest",
      "the HD Audio output stream checks (bin/hdatest) and their result line; it kills\n"
      "  and restarts the hda driver once"),
    C(mixtest, C_TESTS, "mixtest",
      "the mixer checks (bin/mixtest) and their result line: tone programs played at\n"
      "  once, one killed; it kills and restarts the mixer and the hda driver once"),
    C(crash, C_TESTS, "crash [name [yes]]",
      "the kernel's crash tests: alone, the list; \"crash <name> yes\" runs one\n"
      "  (each panics the machine on purpose, bp excepted)"),
    C(panic, C_TESTS, "panic", "panic the kernel (a test: its screen must show)"),
    C(pwd, C_FILES, "pwd", "the current directory"),
    C(cd, C_FILES, "cd [dir]", "change directory (no argument: $HOME)"),
    C(ls, C_FILES, "ls [-l] [path...]", "list a directory (-l: sizes)"),
    C(find, C_FILES, "find [dir] [-name text]", "every file below dir (names containing text)"),
    C(mkdir, C_FILES, "mkdir [-p] <dir>...", "make directories (-p: and the ones above them)"),
    C(touch, C_FILES, "touch <file>...", "make an empty file where there is none"),
    C(write, C_FILES, "write [-a] <file> <text...>",
      "put the text (and a newline) into a file, replacing it; -a: add to it.\n"
      "  Without text, the pipe's input: ls -l | write /data/list.txt"),
    C(cp, C_FILES, "cp <from> <to>", "copy a file, to a new name or into a directory"),
    C(mv, C_FILES, "mv <from> <to>", "rename, or move into a directory (within one mount)"),
    C(rm, C_FILES, "rm [-r] <path>...",
      "remove files and empty directories (-r: a directory with all it holds)"),
    C(df, C_FILES, "df", "the mounts: size, used, free, volume label"),
    C(sync, C_FILES, "sync", "make sure everything written is on the stick"),
    C(mount, C_FILES, "mount [-w|-r /usbN]",
      "the mounts, and which can be written. Another USB stick shows up\n"
      "  read-only at /usb0, /usb1, ...: -w makes it writable, -r read-only again"),
    C(cat, C_TEXT, "cat [file...]", "print files (or the pipe)"),
    C(hexdump, C_TEXT, "hexdump [-s offset] [-n bytes] [file]",
      "hex and ASCII, 16 bytes a line (also hd)"),
    C(sha256sum, C_TEXT, "sha256sum [file...]",
      "the SHA-256 of files (or the pipe), as the Mac's shasum -a 256 prints it"),
    C(wc, C_TEXT, "wc [-l|-w|-c] [file]", "count lines, words, bytes"),
    C(head, C_TEXT, "head [-n N] [file]", "the first N lines (default 10)"),
    C(tail, C_TEXT, "tail [-n N] [file]", "the last N lines (default 10)"),
    C(grep, C_TEXT, "grep [-i] [-v] [-c] [-n] <pattern> [file]",
      "lines matching pattern: text, with ^ $ . * as in a regular expression.\n"
      "  -i any case, -v the others, -c count them, -n with line numbers"),
    C(sort, C_TEXT, "sort [-r] [-n] [-u] [file]", "sort lines (-r reverse, -n numeric, -u unique)"),
    C(uniq, C_TEXT, "uniq [-c] [file]", "drop repeated adjacent lines (-c: count them)"),
    C(seq, C_TEXT, "seq [first] last", "the numbers first..last, one a line"),
    C(echo, C_SHELL, "echo [-n] [-e] [text...]",
      "print the words ($NAME expanded; -n no newline, -e \\n \\t)"),
    C(set, C_SHELL, "set [NAME value]",
      "set a shell variable (or NAME=value); no arguments: list them"),
    C(unset, C_SHELL, "unset NAME...", "remove variables"),
    C(export, C_SHELL, "export [NAME[=value]...]", "variables given to programs `run` starts"),
    C(env, C_SHELL, "env", "the exported variables (a program's environment)"),
    C(alias, C_SHELL, "alias [name[=text]]", "list, show or define aliases: alias ll='ls -l'"),
    C(unalias, C_SHELL, "unalias name...", "remove aliases"),
    C(type, C_SHELL, "type name...", "what a name is: alias, builtin or program (also which)"),
    C(time, C_SHELL, "time <command...>", "run a command and say how long it took"),
    C(sleep, C_SHELL, "sleep <seconds>", "wait (Ctrl+C stops it; 0.5 works)"),
    C(repeat, C_SHELL, "repeat <n> <command...>", "run a command n times (Ctrl+C stops)"),
    C(watch, C_SHELL, "watch [-n seconds] <command...>",
      "run a command every n seconds (default 2) until Ctrl+C\n"
      "  (quote a pipe: watch -n 1 'ps | grep hid')"),
    C(true, C_SHELL, "true", "status 0"),
    C(false, C_SHELL, "false", "status 1"),
};

/* Extra names for commands (not user aliases: `type` says builtin). */
static const char *const synonyms[][2] = {
    { "hd", "hexdump" }, { "which", "type" }, { "printenv", "env" }, { "?", "help" },
};

const struct sh_cmd *sh_cmd_at(size_t i)
{
    return i < sizeof(cmds) / sizeof(cmds[0]) ? &cmds[i] : NULL;
}

const struct sh_cmd *sh_find_cmd(const char *name)
{
    for (size_t i = 0; i < sizeof(synonyms) / sizeof(synonyms[0]); i++)
        if (!strcmp(synonyms[i][0], name)) {
            name = synonyms[i][1];
            break;
        }
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++)
        if (!strcmp(cmds[i].name, name))
            return &cmds[i];
    return NULL;
}
