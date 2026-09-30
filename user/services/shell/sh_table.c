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
    C(date, C_INFO, "date [-u] [-r] [-d @secs]",
      "the date and time from the real-time clock, in $TZ (default Australia/Sydney).\n"
      "  -u: UTC. -r: the raw clock and how it was read. -d @secs: that Unix time.\n"
      "  TZ: Australia/Sydney (AEST/AEDT),\n"
      "  UTC, or an offset like +10, +9:30, -5. RTC=local (default: the clock keeps\n"
      "  local time, as Windows sets it) or RTC=utc. Example: TZ=UTC date"),
    C(lscpu, C_INFO, "lscpu [-e]",
      "the CPU: model, P-cores, E-cores, threads (-e: one line per CPU)"),
    C(free, C_INFO, "free", "memory: total, used, free"),
    C(ps, C_INFO, "ps [-k]",
      "processes: id, threads, CPU time, memory of its job, name (indented by job).\n"
      "  -k: the kernel's own listing (jobs with pages, handles, threads) in the log"),
    C(top, C_INFO, "top [-d seconds] [-n frames]",
      "live CPU use per CPU and per process, and memory; q or Ctrl+C quits"),
    C(sysmon, C_INFO, "sysmon", "the graphical system monitor (bin/sysmon); q quits"),
    C(whoami, C_INFO, "whoami", "the user (there is one: jam)"),
    C(hostname, C_INFO, "hostname", "this machine's name"),
    C(dmesg, C_INFO, "dmesg", "the whole kernel log (64 KiB); pipe it: dmesg | grep usb"),
    C(history, C_SHELL, "history", "the lines typed (up/down recall them)"),
    C(devices, C_SYSTEM, "devices", "PCI functions and the drivers devmgr bound (alias lspci)"),
    C(usb, C_SYSTEM, "usb", "USB devices from usb-bus (alias lsusb)"),
    C(pci, C_SYSTEM, "pci", "the kernel's PCI report: BARs, MSI/MSI-X (the old Devices entry)"),
    C(memmap, C_SYSTEM, "memmap", "the loader's memory map"),
    C(log, C_INFO, "log [lines]", "the last lines of the kernel log (default 20)"),
    C(mem, C_SYSTEM, "mem", "physical memory from the kernel, and the shell's job"),
    C(kill, C_SYSTEM, "kill <name>",
      "kill the first process with that name (see ps): a service init runs or a\n"
      "  USB driver (hid-6.1:0); whoever supervises it starts it again"),
    C(clear, C_SHELL, "clear", "clear the screen (also Ctrl+L)"),
    C(reboot, C_SYSTEM, "reboot",
      "restart the machine (what was written to /data is synced first)"),
    C(run, C_SYSTEM, "run <prog|path> [args]",
      "start /boot/bin/<prog> (or a path), wait, say how it ended; Ctrl+C kills it.\n"
      "  Typing a program's name does the same. Exported variables are its environment;\n"
      "  in a pipe its printf output goes down the pipe: run utest | grep passed.\n"
      "  It sees the mounts the shell has. Only programs in /boot can run so far"),
    C(ktest, C_TESTS, "ktest [prefix] [options]",
      "kernel tests (as the boot menu's All tests).\n"
      "  loops=N: the set N times. seed=S: in the order shuffled from S (loop k uses\n"
      "  S+k-1; its seed replays it). shuffle: a seed from the clock. keep: record a\n"
      "  failure and go on (else the first one panics). load: with the stress test's\n"
      "  threads running; tests that need an idle machine are skipped"),
    C(soak, C_TESTS, "soak [minutes] [loops=N] [seed=S]",
      "soak test: the kernel tests again and again in a shuffled order under load,\n"
      "  utest between the loops, files written and read back on /data and any writable\n"
      "  stick (pull and plug sticks while it runs); ends with SOAK RESULTS. Default 3\n"
      "  minutes; loops=N: N loops instead. seed=S: the first loop's order (each loop\n"
      "  prints its seed). halt: stop on the panic screen at the first failure.\n"
      "  idle: no load. Ctrl+C ends it after the step in progress"),
    C(bench, C_TESTS, "bench", "kernel benchmark"),
    C(stress, C_TESTS, "stress <seconds>", "stress test (1..600)"),
    C(utest, C_TESTS, "utest", "the user-space test suite (bin/utest) and its result line"),
    C(usbtest, C_TESTS, "usbtest", "the USB checks (bin/usbtest) and their result line"),
    C(demo, C_TESTS, "demo [seconds]",
      "the visual demo: fractals on every CPU (default 76 s; any key stops it)"),
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
