/* The shell's command layer (everything outside main.c).
 *
 * main.c keeps the console I/O, the line editor and its original commands
 * (devices usb run log mem kill ktest bench stress clear reboot panic).
 * It hands every line to sh_line() (sh_exec.c), which does the rest:
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
 * Commands live in cmds_*.c and are listed in the table in sh_exec.c,
 * with their help. Output goes through sh_put/sh_say (captured when the
 * command's output feeds a pipe); input from a pipe is sh_stdin(). */
#pragma once

#include <os.h>

#define SH_MS 1000000ull
#define SH_S  1000000000ull

/* ---- provided by main.c (the glue block at its end) ------------------------- */

void     sh_put_raw(const char *s, size_t n);   /* main.c put(): capture-aware */
void     sh_flush(void);
bool     sh_get_key(struct input_key_event *ev, uint64_t deadline);
bool     sh_is_ctrl(const struct input_key_event *ev, char letter);
void     sh_main_command(char *line);           /* main.c's own commands */
unsigned sh_history_count(void);                /* lines ever remembered */
const char *sh_history_at(unsigned i);          /* i-th of the last 32, NULL if gone */
handle_t sh_console(void);
handle_t sh_root(void);
handle_t sh_devmgr(void);

/* ---- called by main.c ------------------------------------------------------------ */

void sh_init(void);
/* Run one typed line (it may be modified). */
void sh_line(char *line);
/* main.c's put(): true if the bytes went into a pipe instead of the screen. */
bool sh_capture(const char *s, size_t n);
/* main.c's unknown-command case: try /boot/bin/<name>, else say so. */
void sh_unknown(int argc, char **argv);
/* Tab: complete the word before *pos (a command name, else a path). May
 * print the choices on new lines. true: redraw the line. */
bool sh_complete(char *line, unsigned *len, unsigned *pos, unsigned cap);

/* ---- for the commands ------------------------------------------------------------ */

void sh_put(const char *s, size_t n);
void sh_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Straight to the screen even inside a pipe (status lines, errors). */
void sh_tty(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Output goes to a pipe (not the screen) right now. */
bool sh_piped(void);
/* This command's input from a pipe, or false (no pipe before it). */
bool sh_stdin(const char **data, size_t *len);
/* The command's input: file argv[i] if there is one (i < argc), else the
 * pipe. false (after an error message naming `who`) if neither. */
bool sh_input(const char *who, int argc, char **argv, int i, const char **data, size_t *len);

/* Ctrl+C: poll the keyboard; true once Ctrl+C was pressed during this
 * line (then every loop should stop). Other keys are dropped, except that
 * sh_poll_key returns them (top uses q). */
bool sh_interrupted(void);
int  sh_poll_key(uint64_t deadline);   /* the next key's codepoint, -1 on timeout */
/* Sleep up to ns; false if interrupted. */
bool sh_sleep(uint64_t ns);

/* Run argv as a command (argc == 1: as a whole line, so `watch 'ps | grep x'`
 * works). Returns its status. */
int sh_run_words(int argc, char **argv);
int sh_status(void);   /* $? */

/* variables */
const char *sh_getvar(const char *name);   /* NULL if unset */
void        sh_setvar(const char *name, const char *value, int exported);   /* -1: keep flag */
void        sh_unsetvar(const char *name);

/* numbers */
bool sh_parse_u64(const char *s, uint64_t *out);
/* "12.5 MiB"-style size into buf. */
const char *sh_human(uint64_t bytes, char *buf, size_t cap);

/* files (sh_vfs.c): absolute, normalised paths; /boot is the bootfs
 * (read-only). M8 mounts /data next to it. */
#define SH_PATH_MAX 128
struct sh_dirent {
    char     name[64];
    bool     dir;
    uint64_t size;
};
const char *sh_cwd(void);
bool        sh_chdir(const char *path);
/* in relative to the cwd -> out absolute and normalised; false if too long. */
bool        sh_resolve(const char *in, char *out, size_t cap);
/* ERR_NOT_FOUND, or OK and *dir / *size. */
status_t    sh_stat(const char *abs, bool *dir, uint64_t *size);
/* Entries of directory abs (sorted, at most cap); -1 if not a directory. */
int         sh_readdir(const char *abs, struct sh_dirent *out, int cap);
/* A whole file's bytes (read-only, stay valid). */
status_t    sh_read(const char *abs, const void **data, uint64_t *size);
/* A bootfs path for abs ("/boot/bin/x" -> "bin/x"), or NULL if abs isn't on bootfs. */
const char *sh_bootfs_name(const char *abs);

/* ---- the commands (cmds_*.c) ----------------------------------------------------- */

#define SH_CMD(name) int shc_##name(int argc, char **argv)
/* info */
SH_CMD(uname); SH_CMD(version); SH_CMD(uptime); SH_CMD(date); SH_CMD(lscpu); SH_CMD(free);
SH_CMD(ps); SH_CMD(top); SH_CMD(whoami); SH_CMD(hostname); SH_CMD(dmesg); SH_CMD(history);
/* files and text */
SH_CMD(pwd); SH_CMD(cd); SH_CMD(ls); SH_CMD(find); SH_CMD(cat); SH_CMD(hexdump); SH_CMD(wc);
SH_CMD(head); SH_CMD(tail); SH_CMD(grep); SH_CMD(sort); SH_CMD(uniq); SH_CMD(seq);
/* shell */
SH_CMD(help); SH_CMD(echo); SH_CMD(set); SH_CMD(unset); SH_CMD(export); SH_CMD(env);
SH_CMD(alias); SH_CMD(unalias); SH_CMD(time); SH_CMD(sleep); SH_CMD(repeat); SH_CMD(watch);
SH_CMD(true); SH_CMD(false); SH_CMD(type); SH_CMD(run);
