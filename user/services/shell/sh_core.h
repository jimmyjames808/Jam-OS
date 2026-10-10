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
};
struct sh_stdio sh_stdio_get(void);
void            sh_stdio_set(struct sh_stdio io);

/* A new line starts: forget an earlier Ctrl+C. */
void sh_io_new_line(void);
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
