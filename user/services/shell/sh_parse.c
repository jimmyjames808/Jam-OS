/* Parsing a line: cutting it at ; && || | (sh_segments), and one simple
 * command's text into words with quotes, escapes and $ references
 * (sh_split_words). */
#include "sh_core.h"

/* ---- words: quotes, escapes, $ -------------------------------------------------------- */

void sh_words_free(struct sh_words *w)
{
    for (int i = 0; i < w->argc; i++)
        free(w->argv[i]);
    w->argc = 0;
}

/* Append the value of the $ reference at *pp (just after '$') to b. */
static void expand_var(const char **pp, struct sh_buf *b)
{
    const char *p = *pp;
    char name[SH_NAME_MAX];
    size_t n = 0;
    if (*p == '?') {
        char num[16];
        snprintf(num, sizeof(num), "%d", sh_status());
        sh_buf_add(b, num, strlen(num));
        *pp = p + 1;
        return;
    }
    bool brace = *p == '{';
    if (brace)
        p++;
    while (sh_name_char(*p, n == 0) && n < sizeof(name) - 1)
        name[n++] = *p++;
    name[n] = '\0';
    if (brace) {
        if (*p != '}') {   /* not a reference after all */
            sh_buf_add(b, "${", 2);
            *pp = *pp + 1;
            return;
        }
        p++;
    }
    if (!n) {
        sh_buf_add(b, "$", 1);   /* a lone $ */
        *pp = brace ? *pp + 1 : p;
        return;
    }
    const char *v = sh_getvar(name);
    if (v)
        sh_buf_add(b, v, strlen(v));
    *pp = p;
}

/* "..." at *pp (on the opening quote): \" \\ \$ escaped, $ expanded.
 * false if it doesn't close (said). */
static bool double_quoted(const char **pp, struct sh_buf *b)
{
    const char *p = *pp + 1;
    while (*p && *p != '"') {
        if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
            sh_buf_add(b, p + 1, 1);
            p += 2;
        } else if (*p == '$') {
            p++;
            expand_var(&p, b);
        } else {
            sh_buf_add(b, p++, 1);
        }
    }
    if (*p != '"') {
        sh_tty("sh: no closing \"\n");
        return false;
    }
    *pp = p + 1;
    return true;
}

/* One word from *pp (not on a blank) into b; *quoted if it had quotes or
 * escapes. false on a syntax error (said). */
static bool one_word(const char **pp, struct sh_buf *b, bool *quoted)
{
    const char *p = *pp;
    while (*p && *p != ' ' && *p != '\t') {
        char c = *p;
        if (c == '\\' && p[1]) {
            sh_buf_add(b, p + 1, 1);
            p += 2;
            *quoted = true;
        } else if (c == '\'') {
            const char *e = strchr(p + 1, '\'');
            if (!e) {
                sh_tty("sh: no closing '\n");
                return false;
            }
            sh_buf_add(b, p + 1, (size_t)(e - p - 1));
            p = e + 1;
            *quoted = true;
        } else if (c == '"') {
            if (!double_quoted(&p, b))
                return false;
            *quoted = true;
        } else if (c == '$') {
            p++;
            expand_var(&p, b);
        } else {
            sh_buf_add(b, p++, 1);
        }
    }
    *pp = p;
    return true;
}

bool sh_split_words(const char *s, struct sh_words *w)
{
    w->argc = 0;
    const char *p = s;
    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            return true;
        if (w->argc == SH_MAX_WORDS) {
            sh_tty("sh: more than %d words\n", SH_MAX_WORDS);
            return false;
        }
        struct sh_buf b = { 0 };
        bool quoted = false;
        if (!one_word(&p, &b, &quoted)) {
            free(b.p);
            return false;
        }
        if (!b.n && !quoted) {   /* $UNSET alone: no word at all */
            free(b.p);
            continue;
        }
        char *word = malloc(b.n + 1);
        if (!word) {
            free(b.p);
            return false;
        }
        if (b.n)
            memcpy(word, b.p, b.n);
        word[b.n] = '\0';
        free(b.p);
        w->quoted[w->argc] = quoted;
        w->argv[w->argc++] = word;
        w->argv[w->argc] = NULL;
    }
}

/* ---- segments: ; && || | --------------------------------------------------------------- */

/* An empty command next to | && || is a mistake; next to ; it's fine. */
static bool missing_command(const struct sh_seg *segs, int n)
{
    for (int i = 0; i < n; i++) {
        const char *t = segs[i].text;
        while (*t == ' ' || *t == '\t')
            t++;
        bool empty = !*t;
        bool needs = segs[i].op == SH_OP_PIPE || segs[i].op == SH_OP_AND ||
                     segs[i].op == SH_OP_OR ||
                     (i > 0 && (segs[i - 1].op == SH_OP_PIPE || segs[i - 1].op == SH_OP_AND ||
                                segs[i - 1].op == SH_OP_OR));
        if (empty && needs) {
            sh_tty("sh: a command is missing next to | && or ||\n");
            return true;
        }
    }
    return false;
}

int sh_segments(char *line, struct sh_seg *segs)
{
    int n = 0;
    char *start = line, *p = line;
    char q = 0;
    bool word_start = true;
    for (;;) {
        char c = *p;
        /* Inside quotes, up to the closing one. An unclosed quote reaches
         * the NUL and ends the line like any other end, so its segment
         * counts against SH_MAX_SEGS; sh_split_words reports the missing
         * quote. */
        if (q && c) {
            if (c == '\\' && q == '"' && p[1]) {
                p += 2;
                continue;
            }
            if (c == q)
                q = 0;
            p++;
            continue;
        }
        if (c == '\\' && p[1]) {
            p += 2;
            word_start = false;
            continue;
        }
        if (c == '\'' || c == '"') {
            q = c;
            p++;
            word_start = false;
            continue;
        }
        if (c == '#' && word_start)
            *p = c = '\0';
        int op = !c ? SH_OP_END : c == ';' ? SH_OP_SEMI
               : c == '|' ? (p[1] == '|' ? SH_OP_OR : SH_OP_PIPE)
               : c == '&' ? (p[1] == '&' ? SH_OP_AND : -1) : -2;
        if (op == -1) {
            sh_tty("sh: & (running in the background) is not supported\n");
            return -1;
        }
        if (op == -2) {
            word_start = c == ' ' || c == '\t';
            p++;
            continue;
        }
        if (n == SH_MAX_SEGS) {
            sh_tty("sh: too many commands on one line\n");
            return -1;
        }
        *p = '\0';
        segs[n].text = start;
        segs[n].op = op;
        n++;
        if (op == SH_OP_END)
            break;
        p += op == SH_OP_AND || op == SH_OP_OR ? 2 : 1;
        start = p;
        word_start = true;
    }
    return missing_command(segs, n) ? -1 : n;
}
