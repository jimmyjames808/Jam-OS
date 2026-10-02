/* The update manifest's parser (<update.h>): seven lines, strictly; and
 * the network default line of a build.txt, by the same rules.
 *
 * The bytes come from the network (through bin/update, which parses them
 * to know what to fetch, and again in init, which trusts nobody's parse
 * but its own), so the parser reads only within len, copies nothing it
 * hasn't checked, and refuses everything the format doesn't allow rather
 * than skipping it. A cursor walks the text; each step takes one exact
 * token or fails. */
#include <update.h>

/* The cursor over the manifest. */
struct cur {
    const uint8_t *p;   /* the next byte */
    const uint8_t *end; /* one past the last */
};

static bool take_text(struct cur *c, const char *s)
{
    size_t n = strlen(s);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, s, n))
        return false;
    c->p += n;
    return true;
}

static bool version_char(uint8_t ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '.' || ch == '_' || ch == '+' || ch == '-';
}

static bool hex_char(uint8_t ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
}

/* The bytes up to the next space or newline (not taken): how many. */
static size_t word_len(const struct cur *c)
{
    size_t n = 0;
    while (c->p + n < c->end && c->p[n] != ' ' && c->p[n] != '\n')
        n++;
    return n;
}

/* A word of 1..max bytes, each accepted by ok, into out (NUL-terminated). */
static bool take_word(struct cur *c, bool (*ok)(uint8_t), char *out, size_t max)
{
    size_t n = word_len(c);
    if (!n || n > max)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!ok(c->p[i]))
            return false;
    memcpy(out, c->p, n);
    out[n] = '\0';
    c->p += n;
    return true;
}

/* git: 7..40 hex digits, then "-dirty" or nothing. */
static bool git_ok(const char *g)
{
    size_t n = 0;
    while (hex_char((uint8_t)g[n]))
        n++;
    return n >= 7 && n <= 40 && (!g[n] || !strcmp(g + n, "-dirty"));
}

static bool git_char(uint8_t ch)
{
    return hex_char(ch) || (ch >= 'a' && ch <= 'z') || ch == '-';
}

/* A size: decimal, no leading zero, 1..UPDATE_FILE_MAX. */
static status_t take_size(struct cur *c, uint64_t *out)
{
    size_t n = word_len(c);
    if (!n || n > 10 || (n > 1 && c->p[0] == '0'))
        return ERR_INVALID_ARGS;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (c->p[i] < '0' || c->p[i] > '9')
            return ERR_INVALID_ARGS;
        v = v * 10 + (uint64_t)(c->p[i] - '0');   /* 10 digits can't wrap 64 bits */
    }
    c->p += n;
    if (!v || v > UPDATE_FILE_MAX)
        return ERR_OUT_OF_RANGE;   /* no file is empty */
    *out = v;
    return OK;
}

static uint8_t hex_val(uint8_t ch)
{
    return ch <= '9' ? (uint8_t)(ch - '0') : (uint8_t)(ch - 'a' + 10);
}

static bool take_sha(struct cur *c, uint8_t out[SHA256_BYTES])
{
    if (word_len(c) != 2 * SHA256_BYTES)
        return false;
    for (size_t i = 0; i < 2 * SHA256_BYTES; i++)
        if (!hex_char(c->p[i]))
            return false;
    for (size_t i = 0; i < SHA256_BYTES; i++)
        out[i] = (uint8_t)(hex_val(c->p[2 * i]) << 4 | hex_val(c->p[2 * i + 1]));
    c->p += 2 * SHA256_BYTES;
    return true;
}

/* A network default: "untagged", or "vlan" and a VLAN id 1..4094 in
 * decimal with no leading zero. */
static bool net_ok(const char *w)
{
    if (!strcmp(w, "untagged"))
        return true;
    if (strncmp(w, "vlan", 4) || w[4] < '1' || w[4] > '9')
        return false;
    uint32_t v = 0;
    size_t n = 0;
    for (const char *p = w + 4; *p; p++, n++) {
        if (n == 4 || *p < '0' || *p > '9')
            return false;
        v = v * 10 + (uint32_t)(*p - '0');
    }
    return v <= 4094;
}

static bool net_char(uint8_t ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
}

/* "net <default>\n" */
static bool take_net(struct cur *c, char out[UPDATE_NET_MAX + 1])
{
    return take_text(c, "net ") && take_word(c, net_char, out, UPDATE_NET_MAX) &&
           net_ok(out) && take_text(c, "\n");
}

/* "<name> <size> <sha>\n" */
static status_t take_file(struct cur *c, unsigned f, struct update_manifest *m)
{
    if (!take_text(c, update_file_name(f)) || !take_text(c, " "))
        return ERR_INVALID_ARGS;
    status_t st = take_size(c, &m->file[f].size);
    if (st != OK)
        return st;
    if (!take_text(c, " ") || !take_sha(c, m->file[f].sha256) || !take_text(c, "\n"))
        return ERR_INVALID_ARGS;
    return OK;
}

/* The format line: version 2 only (1 had no `net` line); another number
 * is another format. */
static status_t take_format(struct cur *c)
{
    if (!take_text(c, "jamos-update "))
        return ERR_INVALID_ARGS;
    if (take_text(c, "2\n"))
        return OK;
    size_t n = word_len(c);
    for (size_t i = 0; i < n; i++)
        if (c->p[i] < '0' || c->p[i] > '9')
            return ERR_INVALID_ARGS;
    return n ? ERR_NOT_SUPPORTED : ERR_INVALID_ARGS;
}

status_t update_manifest_parse(const void *text, size_t len, struct update_manifest *out)
{
    if (!text || len > UPDATE_MANIFEST_MAX)
        return ERR_INVALID_ARGS;
    struct cur c = { text, (const uint8_t *)text + len };
    struct update_manifest m;
    memset(&m, 0, sizeof(m));
    status_t st = take_format(&c);
    if (st != OK)
        return st;
    if (!take_text(&c, "version ") ||
        !take_word(&c, version_char, m.version, UPDATE_VERSION_MAX) || !take_text(&c, "\n"))
        return ERR_INVALID_ARGS;
    if (!take_text(&c, "git ") || !take_word(&c, git_char, m.git, UPDATE_GIT_MAX) ||
        !git_ok(m.git) || !take_text(&c, "\n"))
        return ERR_INVALID_ARGS;
    if (!take_net(&c, m.net))
        return ERR_INVALID_ARGS;
    for (unsigned f = 0; f < UPDATE_FILES; f++)
        if ((st = take_file(&c, f, &m)) != OK)
            return st;
    m.signed_len = (size_t)(c.p - (const uint8_t *)text);
    if (!take_text(&c, "signature"))
        return ERR_INVALID_ARGS;
    if (c.p < c.end && *c.p == ' ')
        return ERR_NOT_SUPPORTED;   /* signed: this build can't check a signature */
    if (!take_text(&c, "\n") || c.p != c.end)
        return ERR_INVALID_ARGS;
    *out = m;
    return OK;
}

status_t update_build_net(const void *text, size_t len, char out[UPDATE_NET_MAX + 1])
{
    const uint8_t *p = text, *end = p + (text ? len : 0);
    while (p < end) {   /* line by line: one is "net <default>" */
        const uint8_t *nl = p;
        while (nl < end && *nl != '\n')
            nl++;
        if (nl == end)
            break;   /* a last line without its '\n' is no line */
        struct cur c = { p, nl + 1 };
        char w[UPDATE_NET_MAX + 1];
        if (take_net(&c, w) && c.p == c.end) {
            memcpy(out, w, sizeof(w));
            return OK;
        }
        p = nl + 1;
    }
    return ERR_NOT_FOUND;
}

const char *update_file_name(unsigned file)
{
    return file == UPDATE_KERNEL ? "kernel" : file == UPDATE_BOOTFS ? "bootfs" : "?";
}

const char *update_why_str(uint32_t why)
{
    static const char *const words[UPDATE_WHY_COUNT] = {
        [UPDATE_ACCEPTED] = "accepted",
        [UPDATE_BAD_OFFER] = "the offer message is malformed",
        [UPDATE_BAD_MANIFEST] = "the manifest doesn't parse",
        [UPDATE_BAD_SIZE] = "a file's length isn't the manifest's",
        [UPDATE_SHORT_VMO] = "a file's VMO is shorter than its length",
        [UPDATE_BAD_HASH] = "a file's SHA-256 isn't the manifest's",
        [UPDATE_NOT_LOADED] = "the kernel refused the build",
        [UPDATE_NET_CHANGE] = "its network default isn't this build's",
    };
    return why < UPDATE_WHY_COUNT ? words[why] : "?";
}
