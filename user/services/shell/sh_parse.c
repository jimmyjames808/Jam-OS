/* Parsing a line: cutting it at ; && || | & (sh_segments), and one simple
 * command's text into words with quotes, escapes and $ references, and
 * the redirections among them (sh_split_words). */
#include "sh_core.h"

/* ---- words: quotes, escapes, $ -------------------------------------------------------- */

void sh_words_free(struct sh_words *w)
{
    for (int i = 0; i < w->argc; i++)
        free(w->argv[i]);
    w->argc = 0;
    for (int i = 0; i < w->nredir; i++)
        free(w->redir[i].path);
    w->nredir = 0;
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
    while (*p && *p != ' ' && *p != '\t' && *p != '<' && *p != '>') {
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

/* The file a redirection names, at *pp: one word, expanded as any is;
 * malloc'd. NULL (said) if there is none. op: the operator, for the
 * message. */
static char *redirect_target(const char **pp, const char *op)
{
    const char *p = *pp;
    while (*p == ' ' || *p == '\t')
        p++;
    struct sh_buf b = { 0 };
    bool quoted = false;
    char *name = NULL;
    if (*p && one_word(&p, &b, &quoted) && (b.n || quoted)) {
        name = malloc(b.n + 1);
        if (name) {
            if (b.n)
                memcpy(name, b.p, b.n);
            name[b.n] = '\0';
            *pp = p;
        }
    } else {
        sh_tty("sh: %s needs a file name\n", op);
    }
    free(b.p);
    return name;
}

/* The descriptor number before a < or > at p (digits, no blank between),
 * moving p past them; ~0 if there are none. */
static unsigned long redirect_fd(const char **pp)
{
    const char *p = *pp, *d = p;
    while (*d >= '0' && *d <= '9' && d - p < 8)
        d++;
    if (d == p || (*d != '<' && *d != '>'))
        return ~0ul;
    unsigned long fd = 0;
    for (; p < d; p++)
        fd = fd * 10 + (unsigned long)(*p - '0');
    *pp = p;
    return fd;
}

/* The redirection at *pp (on a word's first character), if one starts
 * there: [N]< file, [N]> file, [N]>> file, [N]>&M or [N]<&M, N and M one
 * of 0 to 2. 0: none there (*pp unchanged); 1: taken into w; -1: a
 * syntax error (said). */
static int redirection(const char **pp, struct sh_words *w)
{
    const char *p = *pp;
    unsigned long fd = redirect_fd(&p);
    if (*p != '<' && *p != '>')
        return 0;
    char c = *p++;
    struct sh_redir r = { .kind = c == '<' ? SH_REDIR_IN : SH_REDIR_OUT };
    if (c == '>' && *p == '>') {
        r.kind = SH_REDIR_APPEND;
        p++;
    } else if (*p == '&') {
        r.kind = SH_REDIR_DUP;
        p++;
    }
    if (fd == ~0ul)
        fd = c == '<' ? 0 : 1;
    if (fd > 2) {
        sh_tty("sh: only descriptors 0, 1 and 2 can be redirected\n");
        return -1;
    }
    if (w->nredir == SH_MAX_REDIR) {
        sh_tty("sh: more than %d redirections\n", SH_MAX_REDIR);
        return -1;
    }
    r.fd = (uint8_t)fd;
    if (r.kind == SH_REDIR_DUP) {
        bool ends = !p[1] || p[1] == ' ' || p[1] == '\t' || p[1] == '<' || p[1] == '>';
        if (*p < '0' || *p > '2' || !ends) {
            sh_tty("sh: %c& needs a descriptor, 0 to 2\n", c);
            return -1;
        }
        r.from = (uint8_t)(*p++ - '0');
    } else if (!(r.path = redirect_target(&p, r.kind == SH_REDIR_IN ? "<" : ">"))) {
        return -1;
    }
    w->redir[w->nredir++] = r;
    *pp = p;
    return 1;
}

/* One word at *pp into w; false (said) if it can't be taken. */
static bool add_word(const char **pp, struct sh_words *w)
{
    if (w->argc == SH_MAX_WORDS) {
        sh_tty("sh: more than %d words\n", SH_MAX_WORDS);
        return false;
    }
    struct sh_buf b = { 0 };
    bool quoted = false;
    char *word = one_word(pp, &b, &quoted) ? malloc(b.n + 1) : NULL;
    if (word) {
        if (b.n)
            memcpy(word, b.p, b.n);
        word[b.n] = '\0';
    }
    free(b.p);
    if (!word)
        return false;
    if (!word[0] && !quoted) {   /* $UNSET alone: no word at all */
        free(word);
        return true;
    }
    w->quoted[w->argc] = quoted;
    w->argv[w->argc++] = word;
    w->argv[w->argc] = NULL;
    return true;
}

bool sh_split_words(const char *s, struct sh_words *w)
{
    w->argc = 0;
    w->nredir = 0;
    const char *p = s;
    for (;;) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            return true;
        int red = redirection(&p, w);
        if (red < 0 || (!red && !add_word(&p, w)))
            break;
    }
    sh_words_free(w);
    return false;
}

/* ---- segments: ; && || | --------------------------------------------------------------- */

/* An empty command next to | && || or before & is a mistake; next to ;
 * (or after &) it's fine. */
static bool missing_command(const struct sh_seg *segs, int n)
{
    for (int i = 0; i < n; i++) {
        const char *t = segs[i].text;
        while (*t == ' ' || *t == '\t')
            t++;
        bool empty = !*t;
        bool needs = segs[i].op == SH_OP_PIPE || segs[i].op == SH_OP_AND ||
                     segs[i].op == SH_OP_OR || segs[i].op == SH_OP_BG ||
                     (i > 0 && (segs[i - 1].op == SH_OP_PIPE || segs[i - 1].op == SH_OP_AND ||
                                segs[i - 1].op == SH_OP_OR));
        if (empty && needs) {
            sh_tty("sh: a command is missing next to | && || or &\n");
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
        if (c == '&' && p > line && (p[-1] == '>' || p[-1] == '<')) {
            p++;   /* the & of a redirection (2>&1), not a background & */
            word_start = false;
            continue;
        }
        int op = !c ? SH_OP_END : c == ';' ? SH_OP_SEMI
               : c == '|' ? (p[1] == '|' ? SH_OP_OR : SH_OP_PIPE)
               : c == '&' ? (p[1] == '&' ? SH_OP_AND : SH_OP_BG) : -1;
        if (op == -1) {
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
