/* The update manifest's parser (<update.h>): six lines, strictly; and the
 * public key file's. The signature's check is updsig.c's (it alone links
 * Monocypher in).
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

/* A word of exactly 2n lower-case hex digits, as n bytes into out. */
static bool take_hex(struct cur *c, uint8_t *out, size_t n)
{
    if (word_len(c) != 2 * n)
        return false;
    for (size_t i = 0; i < 2 * n; i++)
        if (!hex_char(c->p[i]))
            return false;
    for (size_t i = 0; i < n; i++)
        out[i] = (uint8_t)(hex_val(c->p[2 * i]) << 4 | hex_val(c->p[2 * i + 1]));
    c->p += 2 * n;
    return true;
}

/* "<name> <size> <sha>\n" */
static status_t take_file(struct cur *c, unsigned f, struct update_manifest *m)
{
    if (!take_text(c, update_file_name(f)) || !take_text(c, " "))
        return ERR_INVALID_ARGS;
    status_t st = take_size(c, &m->file[f].size);
    if (st != OK)
        return st;
    if (!take_text(c, " ") || !take_hex(c, m->file[f].sha256, SHA256_BYTES) || !take_text(c, "\n"))
        return ERR_INVALID_ARGS;
    return OK;
}

/* The format line: version 1 only; another number is a later format. */
static status_t take_format(struct cur *c)
{
    if (!take_text(c, "jamos-update "))
        return ERR_INVALID_ARGS;
    if (take_text(c, "1\n"))
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
    for (unsigned f = 0; f < UPDATE_FILES; f++)
        if ((st = take_file(&c, f, &m)) != OK)
            return st;
    m.signed_len = (size_t)(c.p - (const uint8_t *)text);
    if (!take_text(&c, "signature"))
        return ERR_INVALID_ARGS;
    if (take_text(&c, " ")) {
        if (!take_hex(&c, m.signature, UPDATE_SIG_BYTES))
            return ERR_INVALID_ARGS;
        m.has_signature = true;
    }
    if (!take_text(&c, "\n") || c.p != c.end)
        return ERR_INVALID_ARGS;
    *out = m;
    return OK;
}

status_t update_key_parse(const void *text, size_t len, uint8_t key[UPDATE_KEY_BYTES])
{
    struct cur c = { text, (const uint8_t *)text + len };
    uint8_t k[UPDATE_KEY_BYTES];
    if (!text || !take_text(&c, "ed25519 ") || !take_hex(&c, k, UPDATE_KEY_BYTES) ||
        !take_text(&c, "\n") || c.p != c.end)
        return ERR_INVALID_ARGS;
    memcpy(key, k, sizeof(k));
    return OK;
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
        [UPDATE_NO_KEY] = "this build has no update key: updates are off",
        [UPDATE_UNSIGNED] = "the manifest is not signed",
        [UPDATE_BAD_SIGNATURE] = "the signature isn't this build's key's (or the manifest "
                                 "changed)",
    };
    return why < UPDATE_WHY_COUNT ? words[why] : "?";
}
