/* The package journal (<pkgfmt.h>, the plan's section 9): the install,
 * upgrade or removal in progress, in /data/pkg/db/journal, so a power cut
 * at any step can be settled at the next start. The text is strict and
 * canonical (the writer writes exactly what the reader accepts) and ends
 * with the SHA-256 of everything before it: a write the cut left short
 * does not parse. It is not signed: only `pkg` and init can write
 * /data/pkg/db, and whoever reads it trusts nothing it names, since
 * every file it points at is checked against its signed manifest. */
#include "pkgint.h"

static const char *const op_names[PKG_OPS] = { "install", "upgrade", "remove" };
static const char *const step_names[PKG_STEPS] = {
    "begin", "staged", "approved", "swapped", "revoked", "removed", "recorded",
};

/* The steps of each op, as bits of enum pkg_step. */
#define B(s) (1u << (s))
static const unsigned steps_of[PKG_OPS] = {
    [PKG_OP_INSTALL] = B(PKG_STEP_BEGIN) | B(PKG_STEP_STAGED) | B(PKG_STEP_APPROVED) |
                       B(PKG_STEP_SWAPPED) | B(PKG_STEP_RECORDED),
    [PKG_OP_UPGRADE] = B(PKG_STEP_BEGIN) | B(PKG_STEP_STAGED) | B(PKG_STEP_APPROVED) |
                       B(PKG_STEP_SWAPPED) | B(PKG_STEP_REVOKED) | B(PKG_STEP_RECORDED),
    [PKG_OP_REMOVE] = B(PKG_STEP_BEGIN) | B(PKG_STEP_REVOKED) | B(PKG_STEP_REMOVED) |
                      B(PKG_STEP_RECORDED),
};

const char *pkg_op_name(enum pkg_op op)
{
    return (unsigned)op < PKG_OPS ? op_names[op] : "?";
}

const char *pkg_step_name(enum pkg_step step)
{
    return (unsigned)step < PKG_STEPS ? step_names[step] : "?";
}

enum pkg_step pkg_journal_next(enum pkg_op op, enum pkg_step step)
{
    if ((unsigned)op >= PKG_OPS || (unsigned)step >= PKG_STEPS || !(steps_of[op] & B(step)))
        return PKG_STEPS;
    for (unsigned s = (unsigned)step + 1; s < PKG_STEPS; s++)
        if (steps_of[op] & B(s))
            return (enum pkg_step)s;
    return PKG_STEPS;
}

static bool valid_revision(uint32_t r)
{
    return r >= 1 && r <= 999999999u;
}

/* The rules on a journal's fields, for the reader (once filled) and the
 * writer alike. */
static status_t journal_valid(const struct pkg_journal *j, const char **why)
{
    bool new_pkg = j->op != PKG_OP_REMOVE, old_pkg = j->op != PKG_OP_INSTALL;
    NEED((unsigned)j->op < PKG_OPS && (unsigned)j->step < PKG_STEPS, "an op or step out of range");
    NEED(steps_of[j->op] & B(j->step), "a step this op does not have");
    NEED(pkg_f_name(j->name), "the journal's package name");
    NEED(!new_pkg || (pkg_f_version(j->version) && valid_revision(j->revision)),
         "the journal's version or revision");
    NEED(!old_pkg || (pkg_f_version(j->old_version) && valid_revision(j->old_revision)),
         "the journal's old version or revision");
    NEED(j->nbusy <= PKG_BUSY_MAX && (j->nbusy == 0 || j->op == PKG_OP_UPGRADE),
         "busy lines where an upgrade alone has them, or too many");
    for (uint32_t i = 0; i < j->nbusy; i++) {
        NEED(!pkg_path_problem(j->busy[i]), "a busy line's path");
        NEED(i == 0 || pkg_increasing(j->busy[i - 1], j->busy[i]),
             "busy lines not in strictly increasing order");
    }
    return OK;
}

/* ---- reading ------------------------------------------------------------------------------ */

static bool name_index(struct pkg_str w, const char *const *names, unsigned n, unsigned *out)
{
    for (unsigned i = 0; i < n; i++)
        if (pkg_str_eq(w, names[i])) {
            *out = i;
            return true;
        }
    return false;
}

/* The next line is `key <one field>`. */
static bool value_line(struct pkg_lines *ls, const char *key, struct pkg_str *f)
{
    struct pkg_str line, w[1];
    if (!pkg_next_line(ls, &line) || !pkg_words(line, key, w, 1, 1, NULL))
        return false;
    *f = w[0];
    return true;
}

/* `old <version> <revision> <sha256>` */
static bool old_line(struct pkg_lines *ls, struct pkg_journal *j)
{
    struct pkg_str line, w[3];
    if (!pkg_next_line(ls, &line) || !pkg_words(line, "old", w, 3, 3, NULL))
        return false;
    j->old_version = w[0];
    return pkg_f_revision(w[1], &j->old_revision) && pkg_f_sha(w[2], j->old_manifest);
}

/* `version`, `revision` and `manifest` lines of the new package. */
static bool new_lines(struct pkg_lines *ls, struct pkg_journal *j)
{
    struct pkg_str f;
    return value_line(ls, "version", &j->version) && value_line(ls, "revision", &f) &&
           pkg_f_revision(f, &j->revision) && value_line(ls, "manifest", &f) &&
           pkg_f_sha(f, j->manifest);
}

/* The lines after the first, the `end` line not among them. */
static bool journal_lines(struct pkg_lines *ls, struct pkg_journal *j)
{
    struct pkg_str f, line;
    unsigned v;
    if (!value_line(ls, "op", &f) || !name_index(f, op_names, PKG_OPS, &v))
        return false;
    j->op = (enum pkg_op)v;
    if (!value_line(ls, "name", &j->name))
        return false;
    if (j->op != PKG_OP_REMOVE && !new_lines(ls, j))
        return false;
    if (j->op != PKG_OP_INSTALL && !old_line(ls, j))
        return false;
    if (!value_line(ls, "step", &f) || !name_index(f, step_names, PKG_STEPS, &v))
        return false;
    j->step = (enum pkg_step)v;
    j->nbusy = 0;
    while (pkg_peek_line(ls, &line) && pkg_line_is(line, "busy")) {
        if (j->nbusy == PKG_BUSY_MAX || !value_line(ls, "busy", &j->busy[j->nbusy]))
            return false;
        j->nbusy++;
    }
    return !pkg_peek_line(ls, &line);
}

status_t pkg_journal_parse(const void *text, size_t len, struct pkg_journal *j, const char **why)
{
    const char *t = text;
    size_t flen = strlen(PKG_FIRST_JOURNAL);
    if (!t || len == 0 || len > PKG_JOURNAL_MAX)
        return pkg_fail(why, ERR_OUT_OF_RANGE, "the journal is empty or over its size bound");
    NEED(len > flen && !memcmp(t, PKG_FIRST_JOURNAL, flen) && t[flen] == '\n',
         "the first line is not a journal's");
    for (size_t i = 0; i < len; i++)
        NEED(t[i] == '\n' || ((uint8_t)t[i] >= 0x20 && (uint8_t)t[i] <= 0x7e),
             "a byte that is not printable ASCII");
    NEED(t[len - 1] == '\n', "the last line has no newline");
    size_t end = len - 1;
    while (end > 0 && t[end - 1] != '\n')
        end--;
    uint8_t want[SHA256_BYTES], got[SHA256_BYTES];
    NEED(end > flen + 1 && len - 1 - end == 4 + 2 * SHA256_BYTES && !memcmp(t + end, "end ", 4) &&
             pkg_hex((struct pkg_str){ t + end + 4, 2 * SHA256_BYTES }, want, SHA256_BYTES),
         "no 'end <sha256>' last line");
    sha256(t, end, got);
    NEED(!memcmp(got, want, SHA256_BYTES), "the journal's SHA-256 is not its text's (a torn write)");
    struct pkg_lines ls = { t + flen + 1, t + end };
    NEED(journal_lines(&ls, j), "a journal line");
    return journal_valid(j, why);
}

/* ---- writing ------------------------------------------------------------------------------- */

static void put_line(struct pkg_out *o, const char *key, struct pkg_str v)
{
    pkg_put_str(o, key);
    pkg_put_str(o, " ");
    pkg_put_bytes(o, v.p, v.n);
    pkg_put_str(o, "\n");
}

/* `version`, `revision` and `manifest` of the new package. */
static void put_new(struct pkg_out *o, const struct pkg_journal *j)
{
    put_line(o, "version", j->version);
    pkg_put_str(o, "revision ");
    pkg_put_uint(o, j->revision);
    pkg_put_str(o, "\nmanifest ");
    pkg_put_hex(o, j->manifest, SHA256_BYTES);
    pkg_put_str(o, "\n");
}

static void put_old(struct pkg_out *o, const struct pkg_journal *j)
{
    pkg_put_str(o, "old ");
    pkg_put_bytes(o, j->old_version.p, j->old_version.n);
    pkg_put_str(o, " ");
    pkg_put_uint(o, j->old_revision);
    pkg_put_str(o, " ");
    pkg_put_hex(o, j->old_manifest, SHA256_BYTES);
    pkg_put_str(o, "\n");
}

size_t pkg_journal_write(const struct pkg_journal *j, char *out, size_t cap)
{
    struct pkg_out o = { out, cap, 0 };
    uint8_t sum[SHA256_BYTES];
    if (journal_valid(j, NULL) != OK)
        return 0;
    pkg_put_str(&o, PKG_FIRST_JOURNAL "\nop ");
    pkg_put_str(&o, op_names[j->op]);
    pkg_put_str(&o, "\n");
    put_line(&o, "name", j->name);
    if (j->op != PKG_OP_REMOVE)
        put_new(&o, j);
    if (j->op != PKG_OP_INSTALL)
        put_old(&o, j);
    pkg_put_str(&o, "step ");
    pkg_put_str(&o, step_names[j->step]);
    pkg_put_str(&o, "\n");
    for (uint32_t i = 0; i < j->nbusy; i++)
        put_line(&o, "busy", j->busy[i]);
    if (o.n > cap || cap - o.n < 4 + 2 * SHA256_BYTES + 1)
        return 0;
    sha256(out, o.n, sum);
    pkg_put_str(&o, "end ");
    pkg_put_hex(&o, sum, SHA256_BYTES);
    pkg_put_str(&o, "\n");
    return o.n > PKG_JOURNAL_MAX ? 0 : o.n;
}
