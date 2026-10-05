/* The boot menu's check (<bootmenu.h>): a Limine config, line by line,
 * against the subset of Limine's syntax Jam OS's menu uses, then the
 * entries as a whole (the default, the previous build, each kernel's boot
 * image), then the files they name. Everything the header doesn't allow
 * is refused.
 *
 * The bytes come from the network (a fetched build's menu, checked
 * against its signed SHA-256 before it gets here, but the check doesn't
 * rely on that) and from the repository: it reads only within len, and
 * holds what it found in fixed tables. It calls no library function,
 * not even memcmp: tools/menucheck.c builds this file for the Mac as it
 * is. */
#include <bootmenu.h>

/* An entry as read. */
struct entry {
    uint32_t       line;       /* its `/` line */
    uint8_t        depth;      /* 1: `/`, 2: `//` */
    bool           plus;       /* `/+`: shown open */
    bool           dir;        /* `//` entries follow it */
    bool           options;    /* it has an option line */
    const uint8_t *name;       /* its name, name_len bytes, in the text */
    size_t         name_len;
    bool           protocol;   /* the options seen */
    bool           cmdline;
    bool           comment;
    int            kernel;     /* path's index in files[] (-1: none) */
    int            module0;    /* the first module_path's (-1: none) */
    unsigned       modules;
};

/* A file the menu names. */
struct file {
    char     path[BOOTMENU_PATH_MAX + 1];   /* NUL-terminated */
    size_t   len;
    uint32_t line;                          /* where it is first named */
};

/* The check under way. */
struct menu {
    uint32_t                line;       /* the line being read, from 1 */
    bool                    timeout;    /* the timeout line was seen */
    int                     top;        /* the last `/` entry's index (-1: none) */
    struct entry            e[BOOTMENU_ENTRIES];
    unsigned                ne;
    struct file             f[BOOTMENU_FILES];
    unsigned                nf;
    struct bootmenu_result *out;
};

static size_t cstr_len(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* Are the n bytes at p exactly s? */
static bool span_is(const uint8_t *p, size_t n, const char *s)
{
    if (n != cstr_len(s))
        return false;
    for (size_t i = 0; i < n; i++)
        if (p[i] != (uint8_t)s[i])
            return false;
    return true;
}

static bool refuse(struct menu *m, uint32_t why)
{
    m->out->why = why;
    m->out->line = m->line;
    return false;
}

/* Every byte printable ASCII but '$', every line ended by '\n' and at
 * most BOOTMENU_LINE_MAX bytes. */
static bool bytes_ok(struct menu *m, const uint8_t *p, size_t len)
{
    size_t start = 0;   /* where the line begins */
    m->line = 1;
    for (size_t i = 0; i < len; i++) {
        if (p[i] == '\n') {
            m->line++;
            start = i + 1;
            continue;
        }
        if (p[i] < 0x20 || p[i] > 0x7e || p[i] == '$')
            return refuse(m, BOOTMENU_BAD_BYTE);
        if (i - start >= BOOTMENU_LINE_MAX)
            return refuse(m, BOOTMENU_LONG_LINE);
    }
    if (p[len - 1] != '\n')
        return refuse(m, BOOTMENU_NO_NEWLINE);   /* m->line: the last line's */
    return true;
}

/* "key: value" in [p, end): the key (a-z and _), one space after the
 * colon, a value of a byte or more. */
static bool split_option(const uint8_t *p, const uint8_t *end, const uint8_t **key,
                         size_t *key_len, const uint8_t **val, size_t *val_len)
{
    const uint8_t *k = p;
    while (p < end && ((*p >= 'a' && *p <= 'z') || *p == '_'))
        p++;
    if (p == k || end - p < 3 || p[0] != ':' || p[1] != ' ')
        return false;
    *key = k;
    *key_len = (size_t)(p - k);
    *val = p + 2;
    *val_len = (size_t)(end - p - 2);
    return true;
}

/* `timeout: <1..BOOTMENU_TIMEOUT_MAX>` or `timeout: no`. */
static bool timeout_ok(const uint8_t *v, size_t n)
{
    if (span_is(v, n, "no"))
        return true;
    if (!n || n > 3 || v[0] == '0')
        return false;
    unsigned t = 0;
    for (size_t i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9')
            return false;
        t = t * 10 + (unsigned)(v[i] - '0');
    }
    return t <= BOOTMENU_TIMEOUT_MAX;
}

/* An unindented line: a global option, before the first entry. */
static bool global_line(struct menu *m, const uint8_t *p, const uint8_t *end)
{
    const uint8_t *k, *v;
    size_t kn, vn;
    if (m->ne)
        return refuse(m, BOOTMENU_GLOBAL_IN_ENTRY);
    if (!split_option(p, end, &k, &kn, &v, &vn) || !span_is(k, kn, "timeout"))
        return refuse(m, BOOTMENU_UNKNOWN_GLOBAL);
    if (m->timeout)
        return refuse(m, BOOTMENU_GLOBAL_TWICE);
    if (!timeout_ok(v, vn))
        return refuse(m, BOOTMENU_BAD_TIMEOUT);
    m->timeout = true;
    return true;
}

/* A `/` or `//` line: a new entry. */
static bool entry_line(struct menu *m, const uint8_t *p, const uint8_t *end)
{
    unsigned depth = 0;
    while (p < end && *p == '/') {
        depth++;
        p++;
    }
    bool plus = p < end && *p == '+';
    p += plus;
    size_t n = (size_t)(end - p);
    if (depth > 2 || !n || n > BOOTMENU_NAME_MAX)
        return refuse(m, BOOTMENU_BAD_ENTRY);
    if (m->ne == BOOTMENU_ENTRIES)
        return refuse(m, BOOTMENU_TOO_MANY);
    if (depth == 2) {
        if (m->top < 0)
            return refuse(m, BOOTMENU_PARENTLESS);
        if (plus)
            return refuse(m, BOOTMENU_BAD_PLUS);
        if (m->e[m->top].options)
            return refuse(m, BOOTMENU_OPTION_IN_DIR);
        m->e[m->top].dir = true;
    } else {
        m->top = (int)m->ne;
    }
    m->e[m->ne++] = (struct entry){
        .line = m->line, .depth = (uint8_t)depth, .plus = plus, .name = p, .name_len = n,
        .kernel = -1, .module0 = -1,
    };
    return true;
}

/* A file name's bytes: lower case only, as Jam OS names its files, so
 * no question of how Limine's FAT driver matches case arises. */
static bool name_char(uint8_t ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
           ch == '-';
}

/* "/a/b": names of name_char()s, none starting with '.', after single
 * slashes; no slash at the end. */
static bool plain_path(const uint8_t *p, size_t n)
{
    if (!n || p[0] != '/' || p[n - 1] == '/')
        return false;
    for (size_t i = 1; i < n; i++) {
        bool name_starts = p[i - 1] == '/';
        bool bad = p[i] == '/' ? name_starts   /* "//" */
                               : (!name_char(p[i]) || (name_starts && p[i] == '.'));
        if (bad)
            return false;
    }
    return true;
}

/* "boot():/a/b" (v, n bytes): the path's index in m->f (added if new). */
static bool add_file(struct menu *m, const uint8_t *v, size_t n, int *index)
{
    static const char res[] = "boot():";
    size_t r = sizeof(res) - 1;
    if (n <= r || !span_is(v, r, res) || n - r > BOOTMENU_PATH_MAX || !plain_path(v + r, n - r))
        return refuse(m, BOOTMENU_BAD_PATH);
    const uint8_t *p = v + r;
    n -= r;
    for (unsigned i = 0; i < m->nf; i++) {
        if (span_is(p, n, m->f[i].path)) {
            *index = (int)i;
            return true;
        }
    }
    if (m->nf == BOOTMENU_FILES)
        return refuse(m, BOOTMENU_TOO_MANY);
    struct file *f = &m->f[m->nf];
    for (size_t i = 0; i < n; i++)
        f->path[i] = (char)p[i];
    f->path[n] = '\0';
    f->len = n;
    f->line = m->line;
    *index = (int)m->nf++;
    return true;
}

/* An option once only: *seen is set. */
static bool once(struct menu *m, bool *seen)
{
    if (*seen)
        return refuse(m, BOOTMENU_OPTION_TWICE);
    *seen = true;
    return true;
}

/* An indented line: an option of the last entry. */
static bool option_line(struct menu *m, const uint8_t *p, const uint8_t *end)
{
    const uint8_t *k, *v;
    size_t kn, vn;
    if (!m->ne)
        return refuse(m, BOOTMENU_OPTION_OUTSIDE);
    while (p < end && *p == ' ')
        p++;
    struct entry *e = &m->e[m->ne - 1];
    e->options = true;
    if (!split_option(p, end, &k, &kn, &v, &vn))
        return refuse(m, BOOTMENU_UNKNOWN_OPTION);
    if (span_is(k, kn, "protocol")) {
        if (!once(m, &e->protocol))
            return false;
        return span_is(v, vn, "limine") || refuse(m, BOOTMENU_BAD_PROTOCOL);
    }
    if (span_is(k, kn, "path") || span_is(k, kn, "kernel_path")) {
        if (e->kernel >= 0)
            return refuse(m, BOOTMENU_OPTION_TWICE);
        return add_file(m, v, vn, &e->kernel);
    }
    if (span_is(k, kn, "module_path")) {
        int i;
        if (e->modules == BOOTMENU_MODULES)
            return refuse(m, BOOTMENU_TOO_MANY);
        if (!add_file(m, v, vn, &i))
            return false;
        e->module0 = e->modules++ ? e->module0 : i;
        return true;
    }
    if (span_is(k, kn, "cmdline") || span_is(k, kn, "kernel_cmdline"))
        return once(m, &e->cmdline);
    if (span_is(k, kn, "comment"))
        return once(m, &e->comment);
    return refuse(m, BOOTMENU_UNKNOWN_OPTION);
}

/* One line, [p, end) without its '\n'. */
static bool one_line(struct menu *m, const uint8_t *p, const uint8_t *end)
{
    const uint8_t *q = p;
    while (q < end && *q == ' ')
        q++;
    if (q == end || *p == '#')
        return true;   /* blank, or a comment */
    if (*p == '/')
        return entry_line(m, p, end);
    if (*p == ' ')
        return option_line(m, p, end);
    return global_line(m, p, end);
}

/* Is b (an entry's first module) the boot image beside kernel k: k's
 * path with bootfs.img for its ending jamos.elf? */
static bool paired(const struct file *k, const struct file *b)
{
    static const char kend[] = "jamos.elf", bend[] = "bootfs.img";
    size_t kn = sizeof(kend) - 1, bn = sizeof(bend) - 1, stem = k->len - kn;
    if (b->len != stem + bn)
        return false;
    for (size_t i = 0; i < stem; i++)
        if (k->path[i] != b->path[i])
            return false;
    return span_is((const uint8_t *)b->path + stem, bn, bend);
}

static bool ends_jamos(const struct file *k)
{
    static const char kend[] = "jamos.elf";
    size_t kn = sizeof(kend) - 1;
    return k->len >= kn && span_is((const uint8_t *)k->path + k->len - kn, kn, kend);
}

/* Each entry whole: a directory has entries, an entry that boots has a
 * protocol, a Jam OS kernel and its boot image. */
static bool entries_ok(struct menu *m)
{
    for (unsigned i = 0; i < m->ne; i++) {
        const struct entry *e = &m->e[i];
        m->line = e->line;
        if (e->plus && !e->dir)
            return refuse(m, BOOTMENU_BAD_PLUS);
        if (e->dir)
            continue;
        if (!e->protocol)
            return refuse(m, BOOTMENU_NO_PROTOCOL);
        if (e->kernel < 0)
            return refuse(m, BOOTMENU_NO_PATH);
        if (!ends_jamos(&m->f[e->kernel]))
            return refuse(m, BOOTMENU_NOT_JAMOS);
        if (e->module0 < 0 || !paired(&m->f[e->kernel], &m->f[e->module0]))
            return refuse(m, BOOTMENU_BAD_PAIR);
        m->out->entries++;
    }
    return true;
}

/* The default entry boots this build, "Jam OS (previous build)" the
 * previous one (entries_ok has passed: their boot images are paired). */
static bool roles_ok(struct menu *m)
{
    const struct entry *d = &m->e[0];
    m->line = d->line;
    if (d->dir || !span_is((const uint8_t *)m->f[d->kernel].path, m->f[d->kernel].len,
                           BOOTMENU_KERNEL))
        return refuse(m, BOOTMENU_BAD_DEFAULT);
    for (unsigned i = 0; i < m->ne; i++) {
        const struct entry *e = &m->e[i];
        if (e->depth != 1 || e->dir || !span_is(e->name, e->name_len, BOOTMENU_PREV_NAME))
            continue;
        m->line = e->line;
        const struct file *k = &m->f[e->kernel];
        if (!span_is((const uint8_t *)k->path, k->len, BOOTMENU_PREV_KERNEL))
            return refuse(m, BOOTMENU_BAD_PREVIOUS);
        return true;
    }
    m->line = 0;
    return refuse(m, BOOTMENU_NO_PREVIOUS);
}

/* Every file the menu names is there. */
static bool files_ok(struct menu *m, bootmenu_exists_fn exists, void *ctx)
{
    for (unsigned i = 0; i < m->nf; i++) {
        if (exists(ctx, m->f[i].path))
            continue;
        m->line = m->f[i].line;
        for (size_t j = 0; j <= m->f[i].len; j++)
            m->out->path[j] = m->f[i].path[j];
        return refuse(m, BOOTMENU_NO_FILE);
    }
    return true;
}

bool bootmenu_check(const void *text, size_t len, bootmenu_exists_fn exists, void *ctx,
                    struct bootmenu_result *out)
{
    struct menu m = { .top = -1, .out = out };
    *out = (struct bootmenu_result){ .why = BOOTMENU_OK };
    const uint8_t *p = text, *end = p + len;
    if (!text || !len)
        return refuse(&m, BOOTMENU_EMPTY);
    if (len > BOOTMENU_MAX)
        return refuse(&m, BOOTMENU_TOO_BIG);
    if (!bytes_ok(&m, p, len))
        return false;
    for (m.line = 1; p < end; m.line++) {
        const uint8_t *nl = p;
        while (*nl != '\n')
            nl++;   /* bytes_ok: the last byte is '\n' */
        if (!one_line(&m, p, nl))
            return false;
        p = nl + 1;
    }
    m.line = 0;
    if (!m.ne)
        return refuse(&m, BOOTMENU_NO_ENTRY);
    if (!m.timeout)
        return refuse(&m, BOOTMENU_NO_TIMEOUT);
    if (!entries_ok(&m) || !roles_ok(&m) || !files_ok(&m, exists, ctx)) {
        out->entries = 0;
        return false;
    }
    out->line = 0;
    return true;
}

const char *bootmenu_why_str(uint32_t why)
{
    static const char *const words[BOOTMENU_WHYS] = {
        [BOOTMENU_OK] = "passes",
        [BOOTMENU_EMPTY] = "it is empty",
        [BOOTMENU_TOO_BIG] = "it is over 64 KiB",
        [BOOTMENU_NO_NEWLINE] = "its last line has no newline",
        [BOOTMENU_BAD_BYTE] = "a byte that isn't printable ASCII (or is '$')",
        [BOOTMENU_LONG_LINE] = "a line over 255 bytes",
        [BOOTMENU_UNKNOWN_GLOBAL] = "a global option this check can't vouch for (only timeout)",
        [BOOTMENU_GLOBAL_TWICE] = "timeout twice",
        [BOOTMENU_BAD_TIMEOUT] = "timeout isn't 1..600 seconds or no",
        [BOOTMENU_NO_TIMEOUT] = "no timeout line",
        [BOOTMENU_GLOBAL_IN_ENTRY] = "an unindented option after the first entry",
        [BOOTMENU_OPTION_OUTSIDE] = "an indented line before the first entry",
        [BOOTMENU_BAD_ENTRY] = "an entry line that isn't / or // and a name of 1..100 bytes",
        [BOOTMENU_PARENTLESS] = "a // entry with no / entry above it",
        [BOOTMENU_TOO_MANY] = "too many entries, files or modules",
        [BOOTMENU_UNKNOWN_OPTION] = "an entry option this check can't vouch for",
        [BOOTMENU_OPTION_TWICE] = "an option twice in one entry",
        [BOOTMENU_OPTION_IN_DIR] = "a directory with options of its own",
        [BOOTMENU_BAD_PLUS] = "/+ on an entry with no // entries",
        [BOOTMENU_BAD_PROTOCOL] = "a protocol other than limine",
        [BOOTMENU_NO_PROTOCOL] = "an entry without protocol",
        [BOOTMENU_BAD_PATH] = "a path that isn't boot():/ and a plain file name",
        [BOOTMENU_NO_PATH] = "an entry without path",
        [BOOTMENU_NOT_JAMOS] = "a kernel whose name doesn't end in jamos.elf",
        [BOOTMENU_BAD_PAIR] = "a kernel whose first module isn't its own bootfs.img",
        [BOOTMENU_NO_ENTRY] = "no entry",
        [BOOTMENU_BAD_DEFAULT] = "the first entry (the default) doesn't boot /boot/jamos.elf",
        [BOOTMENU_NO_PREVIOUS] = "no \"Jam OS (previous build)\" entry",
        [BOOTMENU_BAD_PREVIOUS] = "\"Jam OS (previous build)\" boots another kernel",
        [BOOTMENU_NO_FILE] = "a file it names isn't on the stick",
    };
    return why < BOOTMENU_WHYS ? words[why] : "?";
}
