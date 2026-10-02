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
 *   sh_allow.c     programs from /data: the owner's approvals
 *   sh_kernel.c    the kernel's debug commands and its log
 *   sh_vfs.c       paths and files
 *   sh_num.c       numbers, sizes, seconds
 *   sh_time.c      calendar, time zones, the real-time clock
 *   sh_sysinfo.c   system, CPU and process figures
 *   sh_text.c      lines and counts for the text commands */
#pragma once

#include <os.h>
#include <wallclock.h>

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
void sh_drop_typeahead(void);          /* forget the keys kept for the next line */
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
/* The kernel log on the screen while a command whose output is the log
 * runs (console.show_log): true before it, false after; `only` a process
 * name (its lines and the kernel's), or NULL for every line. Nested pairs
 * count once (the outermost's `only`); on a console that shows the log
 * anyway it changes nothing. */
void        sh_show_log(bool on, const char *only);
unsigned    sh_history_count(void);            /* lines ever remembered */
const char *sh_history_at(unsigned i);         /* i-th of the last 32, NULL if gone */

/* ---- handles from init (sh_handles.c) ---------------------------------------------- */

handle_t sh_root(void);
handle_t sh_pci(void);          /* RES_PCI, for pci_enum (`devices`), or 0 */
handle_t sh_devmgr(void);       /* devmgr's query channel (the newest), or 0 */
/* The first hda driver (abi/idl/hda.idl) with a path to a jack set up, as
 * the mixer hands it out (a query channel: no output stream), for the
 * caller to close; or HANDLE_INVALID (cmd/hda.c). */
handle_t sh_hda(void);
/* init's control channel (abi/idl/initctl.idl: kill, sync, reboot, mount), or 0
 * (a shell that init didn't start has none). */
handle_t sh_initctl(void);
/* The mixer's `audio` and `audioctl` channels, or 0 (no mixer). */
handle_t sh_audio(void);
handle_t sh_audio_ctl(void);
/* The music player's channel (abi/idl/music.idl), or 0 (no player). */
handle_t sh_music(void);
/* The line init left for this shell to print at its start (what happened
 * to the boot before, if it panicked), or "": once, then always "". */
const char *sh_boot_note(void);

/* ---- programs (sh_program.c) ------------------------------------------------------- */

/* Start argv[0] (a name: /boot/bin/<name>; else a path on any mount),
 * wait for it, say how it ended; its status. It gets what its list asks
 * for (<wants.h>) and its terminal (a PROGRAM-level console channel); its
 * job is killed when it ends. */
int sh_run_program(int argc, char **argv);
/* A helper: the boot-image program at path (a bootfs name) doing one job
 * for a command, started with its list, the extras x (moved; x has room
 * for 2 more after them), an output channel whose lines the shell prints
 * as its own, and a stop channel (SR_USER + 2): Ctrl+C writes a byte
 * there and kills the helper's job only if it hasn't ended 3 s later
 * (`play` fades out and says where it stopped). Says nothing of its own
 * but a failure to start (126); else the helper's exit code (137: it was
 * killed). */
int sh_run_helper(const char *path, int argc, const char *const *argv, struct spawn_handle *x,
                  unsigned nx);
/* A test program (utest, usbtest, hdatest, mixtest): run it as
 * sh_run_program does (its list asks for what it tests), then show its
 * result line from the kernel log; its status. */
int sh_run_test_program(int argc, char **argv);

/* ---- programs from /data (sh_allow.c) ---------------------------------------------- */

struct wants;
/* abs is a file path on /data. */
bool     sh_on_data(const char *abs);
/* The program file at path read into a VMO only we hold and made
 * executable (*vmo, *size), its SHA-256 as hex (65 bytes with the NUL) and
 * its list (<wants.h>), all from those bytes. ERR_ACCESS_DENIED: this
 * shell can't make code executable (not the shell init started);
 * ERR_INVALID_ARGS: not an ELF file, or its list is broken. */
status_t sh_program_file(const char *path, handle_t *vmo, uint64_t *size, char *hex,
                         struct wants *w);
/* The first thing w asks for that no program from /data may have (a
 * service's name, "devmgr", or "right debug"), or NULL: devmgr's channels
 * and init's reach drivers, devices, the filesystems unguarded and every
 * service, past every view; the kernel's debug commands can stop the
 * machine (panic, the crash tests, stress). */
const char *sh_wants_refused(const struct wants *w);
/* Ready to run if the owner allowed exactly this file (a line of
 * /data/etc/allow with its path and hash): sh_program_file's outputs.
 * false (said why on the screen) otherwise. */
bool     sh_allowed_program(const char *path, handle_t *vmo, uint64_t *size, struct wants *w);
/* Call fn for every approval (its hash, path and list); how many, or a
 * negative ERR_* if the file can't be read (none at all is 0). */
int      sh_allow_each(void (*fn)(const char *hash, const char *path, const char *text,
                                  void *ctx), void *ctx);
/* Write an approval for path (replacing its old one), or remove those
 * whose path or file name is `name` (*removed: how many). */
status_t sh_allow_add(const char *path, const char *hash_hex, const char *text);
status_t sh_allow_remove(const char *name, unsigned *removed);

/* ---- the kernel (sh_kernel.c) ------------------------------------------------------ */

/* A kernel debug command; its output arrives as kernel log lines. < 0: an
 * error (said, as "<cmd>: <status>"). */
int64_t  sh_kcmd(const char *cmd);
/* The kernel log from position `from` to its end (as much of it as the
 * kernel's ring still holds, 8 MiB at most; malloc'd, the caller frees):
 * *got bytes, *first the position of its first byte. */
status_t sh_klog_read(uint64_t from, char **buf, size_t *got, uint64_t *first);
/* Where the kernel log ends now. */
uint64_t sh_klog_end(void);

/* ---- numbers (sh_num.c) ------------------------------------------------------------ */

bool sh_parse_u64(const char *s, uint64_t *out);
/* "1.5" seconds -> ns; false if it isn't a number. */
bool sh_parse_seconds(const char *s, uint64_t *ns);
/* "12.5 MiB"-style size into buf. */
const char *sh_human(uint64_t bytes, char *buf, size_t cap);
/* Centibels (tenths of a dB) as "-30.0" into buf. */
const char *sh_db(int32_t cb, char *buf, size_t size);
/* "-20", "-20.5", "0" (a sign, digits, one decimal; more are ignored) as
 * centibels into *cb; at most 1000.0 dB either way. */
bool sh_parse_db(const char *s, int32_t *cb);

/* ---- settings (sh_time.c: /data/etc/settings, <settings.h>) ------------------------ */

/* key = value into the settings file; a failure is said (naming `who`),
 * the change itself stands until the next boot. */
void sh_keep_setting(const char *who, const char *key, const char *value);

/* ---- time (sh_time.c) -------------------------------------------------------------- */

/* $TZ as a zone (<wallclock.h>), or the system's zone if $TZ isn't set;
 * if it isn't understood, says so (naming `who`) and gives UTC. */
bool sh_local_tz(struct tz *tz, const char *who);
/* The time now (UTC seconds) from the system's clock; false (said) if
 * there is none. */
bool sh_clock_now(int64_t *utc, const char *who);
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
/* The git commit this boot's build was made from (bootfs's build.txt, the
 * Makefile's: "2079f35", "2079f35-dirty") into out (cap bytes), or
 * "unknown" if the boot image has none. */
void sh_build_git(char *out, size_t cap);
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
/* sh_readdir gave n entries of abs, cap at most: say on the terminal if
 * there are more, which `who` (the command) then didn't show. */
void        sh_readdir_cut(const char *who, const char *abs, int n, int cap);
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
SH_CMD(sysmon); SH_CMD(jamjar);
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
SH_CMD(devices); SH_CMD(usb); SH_CMD(hda); SH_CMD(beep); SH_CMD(play); SH_CMD(vol);
SH_CMD(music); SH_CMD(net); SH_CMD(ping);
SH_CMD(pci); SH_CMD(memmap); SH_CMD(mem); SH_CMD(kill);
SH_CMD(reboot); SH_CMD(kernel); SH_CMD(update); SH_CMD(run); SH_CMD(allow);
/* tests */
SH_CMD(ktest); SH_CMD(soak); SH_CMD(bench); SH_CMD(stress); SH_CMD(utest); SH_CMD(usbtest); SH_CMD(hdatest);
SH_CMD(mixtest);
SH_CMD(demo); SH_CMD(crash); SH_CMD(panic);
