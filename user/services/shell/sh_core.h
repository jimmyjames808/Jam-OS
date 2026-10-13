/* The shell's machinery: what main.c, sh_io.c, sh_parse.c, sh_exec.c and
 * sh_complete.c share with each other. Commands include only sh.h. */
#pragma once

#include "sh.h"

/* ---- main.c --------------------------------------------------------------------------- */

/* Bytes for the screen (buffered until sh_flush or the buffer fills). */
void sh_console_write(const char *s, size_t n);
/* One key-down (or repeat) event; false on timeout. */
bool sh_get_key(struct input_key_event *ev, uint64_t deadline);
/* Ctrl+<letter>, from a terminal (the control character) or a keyboard. */
bool sh_is_ctrl(const struct input_key_event *ev, char letter);

/* ---- sh_io.c: growing buffers, where output goes, Ctrl+C ----------------------------- */

#define SH_PIPE_MAX (4u << 20)   /* a pipe holds at most this; so does any sh_buf */

struct sh_buf {
    char  *p;      /* malloc'd, NULL while empty */
    size_t n, cap; /* bytes in it; its size */
    bool   full;   /* something was dropped (SH_PIPE_MAX, or out of memory) */
};
void sh_buf_add(struct sh_buf *b, const char *s, size_t n);

/* Where a command's output goes (NULL: the screen) and its input from a
 * pipe (have_in false: none). A pipeline sets them for each stage. */
struct sh_stdio {
    struct sh_buf *out;       /* the pipe to the next stage, NULL: the screen */
    const char    *in;        /* the previous stage's output */
    size_t         in_len;    /* its length */
    bool           have_in;   /* there is a previous stage */
    /* POSIX programs of a pipeline run at once (sh_exec.c sets these for
     * each stage; sh_program.c and sh_posix.c use them): */
    bool           defer;        /* a POSIX program here is left running: sh_pipe_settle waits */
    bool           direct_out;   /* its stdout is a real pipe to the next stage's program */
    handle_t       direct_in;    /* the read end of the previous stage's such pipe, or 0 */
    handle_t       direct_next;  /* set by the program: the read end of its direct_out pipe */
};
struct sh_stdio sh_stdio_get(void);
void            sh_stdio_set(struct sh_stdio io);

/* A new line starts: forget an earlier Ctrl+C. */
void sh_io_new_line(void);
/* Take in every key typed so far: Ctrl+C and Ctrl+\ are counted (below),
 * the others kept for the next line. */
void sh_poll_keys(void);
/* How many times Ctrl+C and Ctrl+\ were pressed since the shell started
 * (a running program compares with the count it has acted on). */
unsigned sh_ctrl_c_hits(void);
unsigned sh_quit_hits(void);
/* Forget this line's Ctrl+C: a POSIX program took the signal and went on
 * (or ended its own way), so the rest of the line runs. */
void sh_cancel_clear(void);
/* A POSIX program starts (+1) or ends (-1): while there is one its
 * terminal echoes ^C itself, and the shell does not. */
void sh_io_posix_tty(int delta);
/* While on, sh_tty says nothing (looking at a command without running it). */
void sh_quiet(bool on);
/* The next key that was typed while a command ran (sh_interrupted kept
 * it); false if there is none. */
bool sh_typeahead(struct input_key_event *ev);
/* Ctrl+C was seen during this line (no polling, unlike sh_interrupted). */
bool sh_cancelled(void);

/* ---- sh_parse.c ----------------------------------------------------------------------- */

#define SH_MAX_WORDS 64
#define SH_MAX_SEGS  32

/* A redirection: `N<file`, `N>file`, `N>>file` or `N>&M`, N and M one of
 * the descriptors 0 to 2 (a program's standard ones). Only a POSIX
 * program takes them (sh_posix.c). */
#define SH_MAX_REDIR 6
enum sh_redir_kind { SH_REDIR_IN, SH_REDIR_OUT, SH_REDIR_APPEND, SH_REDIR_DUP };
struct sh_redir {
    uint8_t kind;   /* enum sh_redir_kind */
    uint8_t fd;     /* the descriptor it redirects */
    uint8_t from;   /* SH_REDIR_DUP: the descriptor it becomes a copy of */
    char   *path;   /* the file named (malloc'd); NULL for SH_REDIR_DUP */
};

struct sh_words {
    int   argc;                      /* words */
    char *argv[SH_MAX_WORDS + 1];    /* each malloc'd; NULL after the last */
    bool  quoted[SH_MAX_WORDS];   /* had quotes or escapes (never an alias) */
    int   nredir;                    /* redirections, in the order written */
    struct sh_redir redir[SH_MAX_REDIR];
};
/* Split one simple command's text into words (malloc'd): quotes, escapes,
 * $ references; the unquoted < > >> and N>&M (with the word after them)
 * are redirections, not words. false on a syntax error (said). */
bool sh_split_words(const char *s, struct sh_words *w);
void sh_words_free(struct sh_words *w);

/* SH_OP_BG: a single & (the command before it runs in the background, and
 * the line goes on as after ;). */
enum { SH_OP_END, SH_OP_PIPE, SH_OP_SEMI, SH_OP_AND, SH_OP_OR, SH_OP_BG };

struct sh_seg {
    char *text; /* the segment, inside the line */
    int   op;   /* what follows it */
};
/* Cut a line at | ; && || & outside quotes (and drop a # comment). The
 * texts point into `line`, which is modified. -1 on a syntax error (said). */
int sh_segments(char *line, struct sh_seg *segs);

/* ---- sh_vars.c ------------------------------------------------------------------------ */

char *sh_strdup(const char *s);   /* malloc'd; NULL if out of memory */

/* ---- sh_exec.c, sh_complete.c, sh_handles.c ------------------------------------------- */

/* The variables, aliases and handles the shell starts with. */
void sh_init(void);
void sh_handles_init(void);
/* Run one typed line (it may be modified). */
void sh_line(char *line);
/* Tab: complete the word before *pos (a command name, else a path). May
 * print the choices on new lines. true: redraw the line. */
bool sh_complete(char *line, unsigned *len, unsigned *pos, unsigned cap);

/* ---- sh_program.c: a pipeline's programs, running at once ---------------------------- */

/* Whether the bare name (a program found as sh_exec.c finds one) is a
 * POSIX program. Says nothing. */
bool sh_program_is_posix(const char *name);
/* Linux's signal numbers, the `signal` protocol's. */
#define LINUX_SIGINT  2
#define LINUX_SIGQUIT 3
#define LINUX_SIGPIPE 13
#define LINUX_SIGTERM 15
/* A POSIX program that a signal's default action ended exits with this
 * plus the signal's number (LX_EXIT_SIGNAL, user/posix/sys/proc.h). The
 * number, or 0 if code is no such exit. */
#define EXIT_SIGNAL_BASE 0x10000
int         sh_exit_signal(int64_t code);
/* "SIGINT" for 2, else "signal N" (in a static buffer). */
const char *sh_signal_name(int signo);
/* After this long without ending, a POSIX program sent SIGINT, SIGQUIT or
 * SIGTERM has its job killed. */
#define SH_SIGNAL_GRACE_NS (2 * NS_PER_S)
/* POSIX programs started with sh_stdio.defer and not yet waited for. */
unsigned sh_pipe_running(void);
/* Wait for all of them (their output copied meanwhile, Ctrl+C and Ctrl+\
 * sent to them, their jobs killed 2 s after): the status of the one
 * started last, -1 if none was waiting. */
int sh_pipe_settle(void);

/* ---- sh_posix.c: descriptors for a POSIX program ------------------------------------- */

/* The redirections of the command about to run (n of them, in the order
 * written; r must stay valid until set again with n 0). Only a POSIX
 * program takes them: start_program refuses them for any other. */
void sh_redir_set(const struct sh_redir *r, int n);
int  sh_redir_count(void);
/* The hand-over for a POSIX program started in the foreground, into *out:
 * descriptors 0 to 2 as the redirections and the pipe the command is in
 * make them (its terminal on the program's console channel, a PROGRAM
 * client). *out_r gets the read end of the pipe its output goes down, when
 * it is in a pipe. false (said) if a file can't be opened; nothing is held
 * then. Else *out is for spawn_args.handover (spawn takes its handles);
 * it stays valid until the next call. */
bool sh_posix_prepare(struct posixfds_out *out, handle_t console, handle_t *out_r);
/* The line's programs have all ended (sh_pipe_settle): the terminal they
 * shared is let go of (the next program opens its own). */
void sh_posix_line_done(void);
