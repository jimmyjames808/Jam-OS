/* /bin/sh until Jam OS has a POSIX shell: `sh -c 'command'` for a simple
 * command only, which is what musl's system and popen ask of it
 * (docs/M13-PLAN.md section 5, "B3 notes").
 *
 * The command is split into words as a POSIX shell splits a simple
 * command with no expansions: at blanks, with '...' (taken as it is),
 * "..." (where \ escapes $ ` " \ and a newline) and \ outside quotes.
 * Anything a real shell would treat specially (| & ; < > ( ) $ ` * ? [
 * ~ # and a newline outside quotes) is refused: exit 2 with a line on
 * standard error, never a guess at what was meant. The first word is
 * looked up in PATH and run with execvp (spawn, wait, exit: our status
 * is the command's); 127 if it isn't found, 126 if it can't be run. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#define WORDS_MAX 256

static int refuse(const char *why)
{
    fprintf(stderr, "sh: %s (only sh -c with a simple command, until a POSIX shell)\n", why);
    return 2;
}

/* One character inside "...": its escapes. *s is past the '\'. */
static char dquote_escape(const char **s)
{
    char c = **s;
    if (c == '$' || c == '`' || c == '"' || c == '\\' || c == '\n') {
        (*s)++;
        return c;
    }
    return '\\';
}

/* The next word of *s into out (as long as *s); false at the end.
 * *bad: the character that made the command not simple, or 0. */
static bool next_word(const char **s, char *out, char *bad)
{
    const char *p = *s;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p)
        return false;
    char *o = out;
    while (*p && *p != ' ' && *p != '\t' && !*bad) {
        char c = *p++;
        if (c == '\'') {
            while (*p && *p != '\'')
                *o++ = *p++;
            *bad = *p ? 0 : '\'';
            p += *p != 0;
        } else if (c == '"') {
            while (*p && *p != '"') {
                c = *p++;
                if (c == '$' || c == '`')
                    *bad = c;
                *o++ = c == '\\' ? dquote_escape(&p) : c;
            }
            *bad = *bad ? *bad : *p ? 0 : '"';
            p += *p != 0;
        } else if (c == '\\') {
            if (*p && *p != '\n')
                *o++ = *p++;
        } else if (strchr("|&;<>()$`*?[~#\n", c)) {
            *bad = c;
        } else {
            *o++ = c;
        }
    }
    *o = '\0';
    *s = p;
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[1], "-c"))
        return refuse("no script or interactive shell");
    const char *s = argv[2];
    char *buf = malloc(strlen(s) + 1), *words[WORDS_MAX + 1];
    int n = 0;
    char bad = 0;
    for (char *w = buf; w && n < WORDS_MAX && next_word(&s, w, &bad) && !bad; n++) {
        words[n] = w;
        w += strlen(w) + 1;
    }
    if (!buf)
        return refuse("out of memory");
    if (bad) {
        char what[32];
        snprintf(what, sizeof(what), "'%c' is not supported", bad);
        return refuse(what);
    }
    if (n == WORDS_MAX)
        return refuse("too many words");
    if (n == 0)
        return 0;   /* an empty command succeeds */
    words[n] = NULL;
    execvp(words[0], words);
    int e = errno;
    fprintf(stderr, "sh: %s: %s\n", words[0], strerror(e));
    return e == ENOENT || e == ENOTDIR ? 127 : 126;
}
