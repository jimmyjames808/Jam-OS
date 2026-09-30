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
/* Ctrl+C was seen during this line (no polling, unlike sh_interrupted). */
bool sh_cancelled(void);

/* ---- sh_parse.c ----------------------------------------------------------------------- */

#define SH_MAX_WORDS 64
#define SH_MAX_SEGS  32

struct sh_words {
    int   argc;                      /* words */
    char *argv[SH_MAX_WORDS + 1];    /* each malloc'd; NULL after the last */
    bool  quoted[SH_MAX_WORDS];   /* had quotes or escapes (never an alias) */
};
/* Split one simple command's text into words (malloc'd): quotes, escapes,
 * $ references. false on a syntax error (said). */
bool sh_split_words(const char *s, struct sh_words *w);
void sh_words_free(struct sh_words *w);

enum { SH_OP_END, SH_OP_PIPE, SH_OP_SEMI, SH_OP_AND, SH_OP_OR };

struct sh_seg {
    char *text; /* the segment, inside the line */
    int   op;   /* what follows it */
};
/* Cut a line at | ; && || outside quotes (and drop a # comment). The
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
