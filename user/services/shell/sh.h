/* The shell: what the commands see.
 *
 * main.c reads a line (console I/O, the line editor, history) and hands it
 * to sh_line() (sh_exec.c), which does the rest:
 *
 *   words     "double quotes" ($ expanded), 'single quotes', \escapes,
 *             $NAME ${NAME} $? (last status), # comments
 *   lists     a ; b      a && b      a || b
 *   pipes     a | b | c  (every stage is a builtin run in turn: a's output
 *             is kept in memory and becomes b's input; `run prog` in a pipe
 *             gets a stdout channel, so programs can be piped too)
 *   aliases   the first word of each command (alias ll='ls -l')
 *   NAME=value on its own sets a shell variable
 *
 * Every command is one function, SH_CMD(name), in cmd/<name>.c, listed
 * with its help in the table in sh_table.c. It gets argv as typed (quotes
 * removed, $ expanded) and returns its status ($?). It prints with sh_put
 * and sh_say (into the pipe when its output feeds one) or sh_tty (always
 * the screen: errors and status lines), and reads a pipe with sh_stdin or
 * sh_input.
 *
 * The other files (sh_core.h has what only the machinery shares):
 *   main.c         console output, keys, the line editor, history
 *   sh_io.c        output, pipes, input, Ctrl+C
 *   sh_parse.c     words (quotes, $), cutting a line at ; && || |
 *   sh_exec.c      running lines, pipelines and commands
 *   sh_table.c     the command table
 *   sh_complete.c  Tab
 *   sh_vars.c      variables and aliases
 *   sh_handles.c   the handles init gives the shell (root, devmgr, ...)
 *   sh_program.c   starting programs (run, utest, ...)
 *   sh_kernel.c    the kernel's debug commands and its log
 *   sh_vfs.c       paths and files
 *   sh_num.c       numbers, sizes, seconds
 *   sh_time.c      calendar, time zones, the real-time clock
 *   sh_sysinfo.c   system, CPU and process figures
 *   sh_text.c      lines and counts for the text commands */
#pragma once

#include <os.h>

/* ---- output and input (sh_io.c) ---------------------------------------------------- */

void sh_put(const char *s, size_t n);
void sh_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Straight to the screen even inside a pipe (status lines, errors). */
void sh_tty(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Send what is buffered for the screen to the console now (main.c). */
void sh_flush(void);
/* Output goes to a pipe (not the screen) right now. */
bool sh_piped(void);
/* This command's input from a pipe, or false (no pipe before it). */
bool sh_stdin(const char **data, size_t *len);
/* The command's input: file argv[i] if there is one (i < argc), else the
 * pipe. false (after an error message naming `who`) if neither. */
bool sh_input(const char *who, int argc, char **argv, int i, const char **data, size_t *len);

/* Ctrl+C: poll the keyboard; true once Ctrl+C was pressed during this
 * line (then every loop should stop). Other keys are kept for the next
 * line (typing ahead); sh_poll_key returns them instead (top uses q). */
bool sh_interrupted(void);
int  sh_poll_key(uint64_t deadline);   /* the next key's codepoint, -1 on timeout */
/* Sleep up to ns; false if interrupted. */
bool sh_sleep(uint64_t ns);

/* ---- running commands (sh_exec.c) -------------------------------------------------- */

/* Run argv as a command (argc == 1: as a whole line, so `watch 'ps | grep x'`
 * works). Returns its status. */
int sh_run_words(int argc, char **argv);
int sh_status(void);   /* $? */

/* ---- the command table (sh_table.c) ------------------------------------------------ */

#define SH_CMD(name) int shc_##name(int argc, char **argv)

struct sh_cmd {
    const char *name;    /* what is typed */
    int       (*fn)(int argc, char **argv);   /* shc_<name> */
    uint8_t     cat;     /* index into sh_categories */
    const char *usage;   /* the arguments, for `help <cmd>` and usage errors */
    const char *help;    /* its first line is what `help` lists */
};

extern const char *const sh_categories[];
extern const unsigned    sh_ncategories;
/* The i-th command in table order, NULL past the end. */
const struct sh_cmd *sh_cmd_at(size_t i);
/* The command called name (or one of its extra names: hd, which, ...), or NULL. */
const struct sh_cmd *sh_find_cmd(const char *name);

/* ---- variables and aliases (sh_vars.c) --------------------------------------------- */

#define SH_MAX_VARS  64
#define SH_MAX_ALIAS 32
#define SH_NAME_MAX  32   /* a variable's or alias's name, with its NUL */

const char *sh_getvar(const char *name);   /* NULL if unset */
void        sh_setvar(const char *name, const char *value, int exported);   /* -1: keep flag */
void        sh_unsetvar(const char *name);
/* Mark a set variable exported; false if it isn't set. */
bool        sh_exportvar(const char *name);
/* Variable slot i (< SH_MAX_VARS): false if free. Listings go in slot order. */
bool        sh_var_at(int i, const char **name, const char **value, bool *exported);
/* c can be part of a variable name (first: its first character). */
bool        sh_name_char(char c, bool first);
/* "NAME=value" (NAME a valid name): the length of NAME, else 0. */
size_t      sh_assignment(const char *word);
/* "NAME=value" of every exported variable, NULL-terminated: a program's
 * environment (freed by sh_free_env). */
char      **sh_make_env(void);
void        sh_free_env(char **env);

const char *sh_alias_of(const char *name);   /* NULL if none */
bool        sh_set_alias(const char *name, const char *value);
bool        sh_unalias(const char *name);    /* false if there was none */
/* Alias slot i (< SH_MAX_ALIAS): false if free. */
bool        sh_alias_at(int i, const char **name, const char **value);

/* ---- the console and history (main.c) ---------------------------------------------- */

handle_t    sh_console(void);
unsigned    sh_history_count(void);            /* lines ever remembered */
const char *sh_history_at(unsigned i);         /* i-th of the last 32, NULL if gone */

/* ---- handles from init (sh_handles.c) ---------------------------------------------- */

handle_t sh_root(void);
handle_t sh_pci(void);          /* RES_PCI, for pci_enum (`devices`), or 0 */
handle_t sh_devmgr(void);       /* devmgr's query channel (the newest), or 0 */
handle_t sh_devmgr_ctl(void);   /* its control channel: only for test programs */
/* init's control channel (abi/idl/initctl.idl: kill, sync, reboot, mount), or 0
 * (a shell that init didn't start has none). */
handle_t sh_initctl(void);

/* ---- programs (sh_program.c) ------------------------------------------------------- */

/* Start argv[0] (a name: /boot/bin/<name>; else a path on any mount),
 * wait for it, say how it ended; its status. It gets the shell's
 * namespace, a PROGRAM-level console channel and nothing of devmgr's; its
 * job is killed when it ends. */
int sh_run_program(int argc, char **argv);
/* A test program (utest, usbtest): run it with devmgr's channels, then
 * show its result line from the kernel log; its status. */
int sh_run_test_program(int argc, char **argv);

/* ---- the kernel (sh_kernel.c) ------------------------------------------------------ */

/* A kernel debug command; its output arrives as kernel log lines. < 0: an
 * error (said, as "<cmd>: <status>"). */
int64_t  sh_kcmd(const char *cmd);
/* The kernel log from position `from` on (up to 64 KiB, malloc'd, the
 * caller frees): *got bytes, *first the position of its first byte. */
status_t sh_klog_read(uint64_t from, char **buf, size_t *got, uint64_t *first);
/* Where the kernel log ends now. */
uint64_t sh_klog_end(void);

/* ---- numbers (sh_num.c) ------------------------------------------------------------ */

bool sh_parse_u64(const char *s, uint64_t *out);
/* "1.5" seconds -> ns; false if it isn't a number. */
bool sh_parse_seconds(const char *s, uint64_t *ns);
/* "12.5 MiB"-style size into buf. */
const char *sh_human(uint64_t bytes, char *buf, size_t cap);

/* ---- time (sh_time.c) -------------------------------------------------------------- */

struct sh_tz {
    bool sydney;       /* Australia/Sydney: AEST +10, AEDT +11 (Oct..Apr) */
    int  off_min;      /* fixed zones: minutes east of UTC */
    char name[24];     /* what dates show: "UTC", "UTC+05:30" */
};

/* TZ: Australia/Sydney (also Sydney, AEST, AEDT, local), UTC/GMT, or
 * [UTC|GMT]+H[:MM] / -H[:MM]. false if not understood. */
bool sh_parse_tz(const char *s, struct sh_tz *tz);
/* $TZ; if it isn't understood, says so (naming `who`) and gives UTC. */
bool sh_local_tz(struct sh_tz *tz, const char *who);
/* The time now (UTC seconds) from the RTC ($RTC says whether it keeps
 * local time or UTC), and the raw reading; false (said) if unreadable. */
bool sh_clock_now(int64_t *utc, struct rtc_time *raw, const char *who);
/* "Thu 15 Jan 2026 12:02:03 AEDT (UTC+11:00)", or just "12:02:03". */
void sh_fmt_time(int64_t utc, const struct sh_tz *tz, char *buf, size_t cap, bool with_zone);
/* "3 days, 4:05", "1:02:03", "5 min 3 s" */
void sh_fmt_uptime(uint64_t ns, char *buf, size_t cap);

/* ---- system figures (sh_sysinfo.c) ------------------------------------------------- */

#define SH_MAX_CPUS  256
#define SH_MAX_PROCS 512

/* sys_info, cpu_stat (at most SH_MAX_CPUS) and proc_list (at most
 * SH_MAX_PROCS): false or -1 after an error message naming `who`. */
bool sh_sysinfo(struct sys_info *s, const char *who);
bool sh_cpus(struct cpu_stat *c, uint32_t *n, const char *who);
int  sh_procs(struct proc_stat *p, const char *who);
/* Tenths of a percent, clamped to 0..1000. */
unsigned    sh_permille(uint64_t part, uint64_t whole);
/* "P", "E" or "-". */
const char *sh_cpu_type(uint32_t type);
/* "m:ss.hh" */
void        sh_fmt_cpu_time(uint64_t ns, char *buf, size_t cap);

/* ---- text (sh_text.c) -------------------------------------------------------------- */

/* The lines of a text: sh_next_line walks them. */
struct sh_lines {
    const char *p, *end;   /* the next line's start; the end of the text */
};
bool sh_next_line(struct sh_lines *l, const char **s, size_t *n);
/* The line and a newline. */
void sh_put_line(const char *s, size_t n);
/* "-n N" or "-N": the count, advancing *i. */
bool sh_count_opt(int argc, char **argv, int *i, uint64_t *n);

/* ---- files (sh_vfs.c) -------------------------------------------------------------- */

/* Absolute, normalised paths over the shell's namespace (<os.h> "files"):
 * /boot is the boot image (read-only), /data the stick's data partition,
 * /esp its boot partition (read-only); the last two only while the stick's
 * filesystems are up. */
#define SH_PATH_MAX FS_PATH_MAX
#define SH_DIR_MAX  256           /* entries ls and find read from one directory */
#define SH_FILE_MAX (4u << 20)    /* the biggest file a command reads whole */
struct sh_dirent {
    char     name[FS_PATH_MAX];   /* the entry's name, without the directory */
    bool     dir;                 /* a directory */
    uint64_t size;                /* a file's bytes */
};
const char *sh_cwd(void);
bool        sh_chdir(const char *path);
/* in relative to the cwd -> out absolute and normalised; false if too long. */
bool        sh_resolve(const char *in, char *out, size_t cap);
/* dir + "/" + name into out; false if it doesn't fit. */
bool        sh_join(const char *dir, const char *name, char *out, size_t cap);
/* The last name of a path ("c" of "/a/b/c"). */
const char *sh_basename(const char *path);
/* OK and *dir / *size, or the mount's error (ERR_NOT_FOUND: no such path). */
status_t    sh_stat(const char *abs, bool *dir, uint64_t *size);
/* Entries of directory abs (sorted, at most cap); -1 if not a directory. */
int         sh_readdir(const char *abs, struct sh_dirent *out, int cap);
/* A whole file's bytes and a NUL after them: valid until the next sh_read.
 * ERR_OUT_OF_RANGE: more than SH_FILE_MAX. */
status_t    sh_read(const char *abs, const void **data, uint64_t *size);
/* n bytes into the file at abs, created if missing. how: FS_TRUNCATE (its
 * new contents), FS_APPEND (after what it has), or 0 (over its start). */
status_t    sh_write(const char *abs, const void *data, size_t n, uint32_t how);
/* The bootfs name of a file on /boot ("/boot/bin/x" -> "bin/x"), or NULL
 * if abs is elsewhere. */
const char *sh_bootfs_name(const char *abs);
/* Where `mv from to` and `cp from to` put it: to, or inside to under
 * from's own name if to is a directory. false if the path is too long. */
bool        sh_dest(const char *from_abs, const char *to, char *out, size_t cap);
/* abs is "/" or a mount point itself ("/data"). */
bool        sh_is_mount(const char *abs);
/* Why a file call failed, in words ("no such file or directory"). */
const char *sh_why(status_t st);

/* ---- the commands (cmd/<name>.c) --------------------------------------------------- */

/* information */
SH_CMD(uname); SH_CMD(version); SH_CMD(uptime); SH_CMD(date); SH_CMD(lscpu); SH_CMD(free);
SH_CMD(ps); SH_CMD(top); SH_CMD(whoami); SH_CMD(hostname); SH_CMD(dmesg); SH_CMD(log);
SH_CMD(sysmon);
/* files and text */
SH_CMD(pwd); SH_CMD(cd); SH_CMD(ls); SH_CMD(find); SH_CMD(mkdir); SH_CMD(rm); SH_CMD(mv);
SH_CMD(cp); SH_CMD(touch); SH_CMD(write); SH_CMD(df); SH_CMD(sync); SH_CMD(mount);
SH_CMD(cat); SH_CMD(hexdump); SH_CMD(wc);
SH_CMD(head); SH_CMD(tail); SH_CMD(grep); SH_CMD(sort); SH_CMD(uniq); SH_CMD(seq);
/* shell */
SH_CMD(help); SH_CMD(history); SH_CMD(clear); SH_CMD(echo); SH_CMD(set); SH_CMD(unset);
SH_CMD(export); SH_CMD(env); SH_CMD(alias); SH_CMD(unalias); SH_CMD(type); SH_CMD(time);
SH_CMD(sleep); SH_CMD(repeat); SH_CMD(watch); SH_CMD(true); SH_CMD(false);
/* system */
SH_CMD(devices); SH_CMD(usb); SH_CMD(hda); SH_CMD(pci); SH_CMD(memmap); SH_CMD(mem); SH_CMD(kill);
SH_CMD(reboot); SH_CMD(run);
/* tests */
SH_CMD(ktest); SH_CMD(soak); SH_CMD(bench); SH_CMD(stress); SH_CMD(utest); SH_CMD(usbtest); SH_CMD(demo);
SH_CMD(crash); SH_CMD(panic);
