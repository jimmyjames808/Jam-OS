/* The package journal (<pkgfmt.h>) against damage: what the writer writes
 * the reader takes back and writes again byte for byte; a write cut at
 * any length, any changed byte and every broken rule is refused; and no
 * mutant that is accepted is anything but the canonical text. */
#include <os.h>
#include <pkgfmt.h>
#include "pkgcases.h"
#include "pkgcases_int.h"

#define S(lit) ((struct pkg_str){ lit, sizeof(lit) - 1 })
#define H1 "1111111111111111111111111111111111111111111111111111111111111111"
#define H2 "2222222222222222222222222222222222222222222222222222222222222222"

static unsigned bad;

#define EXPECT(c, ...)                             \
    do {                                           \
        if (!(c)) {                                \
            printf("pkgfmt: journal: " __VA_ARGS__); \
            printf("\n");                          \
            bad++;                                 \
        }                                          \
    } while (0)

/* body (lines each ended by '\n') and its "end" line into out; the length. */
static size_t seal(char *out, size_t cap, const char *body)
{
    uint8_t sum[SHA256_BYTES];
    size_t n = strlen(body);
    if (n + 4 + 2 * SHA256_BYTES + 2 > cap)
        return 0;
    memcpy(out, body, n);
    sha256(out, n, sum);
    memcpy(out + n, "end ", 4);
    n += 4;
    for (unsigned i = 0; i < SHA256_BYTES; i++) {
        out[n++] = "0123456789abcdef"[sum[i] >> 4];
        out[n++] = "0123456789abcdef"[sum[i] & 15];
    }
    out[n++] = '\n';
    return n;
}

/* Parse text of len bytes in a buffer of exactly that size. */
static status_t parse_exact(const char *text, size_t len, struct pkg_journal *j, const char **why)
{
    char *copy = malloc(len ? len : 1);
    if (!copy)
        return ERR_NO_MEMORY;
    memcpy(copy, text, len);
    status_t st = pkg_journal_parse(copy, len, j, why);
    free(copy);
    return st;
}

static void fill(struct pkg_journal *j, enum pkg_op op, enum pkg_step step, unsigned nbusy)
{
    static const struct pkg_str busy[] = { S("bin/vim"), S("share/vim/vimrc"), S("share/zz") };
    memset(j, 0, sizeof(*j));
    j->op = op;
    j->step = step;
    j->name = S("vim");
    j->version = S("9.1.0");
    j->revision = 2;
    j->old_version = S("9.0.0");
    j->old_revision = 1;
    for (unsigned i = 0; i < SHA256_BYTES; i++) {
        j->manifest[i] = (uint8_t)(i * 7 + 1);
        j->old_manifest[i] = (uint8_t)(i * 5 + 3);
    }
    j->nbusy = nbusy;
    for (unsigned i = 0; i < nbusy; i++)
        j->busy[i] = busy[i];
}

static void round_trip(struct pkg_journal *j, enum pkg_op op, enum pkg_step step, unsigned nbusy)
{
    char text[2048], again[2048];
    struct pkg_journal *back = malloc(sizeof(*back));   /* j stays what was filled */
    const char *why = NULL;
    if (!back)
        return;
    fill(j, op, step, nbusy);
    size_t n = pkg_journal_write(j, text, sizeof(text));
    EXPECT(n > 0, "%s %s with %u busy lines was not written", pkg_op_name(op), pkg_step_name(step), nbusy);
    char *copy = n ? malloc(n) : NULL;   /* the structure points into it */
    if (!copy) {
        free(back);
        return;
    }
    memcpy(copy, text, n);
    EXPECT(pkg_journal_parse(copy, n, back, &why) == OK, "the written text was refused: %s", why);
    EXPECT(back->op == op && back->step == step && back->nbusy == nbusy &&
               !pkg_str_cmp(back->name, j->name) && (op == PKG_OP_REMOVE || back->revision == 2) &&
               (op == PKG_OP_INSTALL || back->old_revision == 1), "a field changed on the way");
    EXPECT(pkg_journal_write(back, again, sizeof(again)) == n && !memcmp(text, again, n),
           "the text read back was written differently");
    free(copy);
    free(back);
    for (size_t cap = 0; cap < n; cap++)
        if (pkg_journal_write(j, again, cap) != 0) {
            EXPECT(false, "written into %zu bytes of %zu", cap, n);
            break;
        }
}

static void next_steps(void)
{
    static const enum pkg_step install[] = { PKG_STEP_BEGIN, PKG_STEP_STAGED, PKG_STEP_APPROVED,
                                             PKG_STEP_SWAPPED, PKG_STEP_RECORDED };
    static const enum pkg_step upgrade[] = { PKG_STEP_BEGIN, PKG_STEP_STAGED, PKG_STEP_APPROVED,
                                             PKG_STEP_SWAPPED, PKG_STEP_REVOKED, PKG_STEP_RECORDED };
    static const enum pkg_step remove_[] = { PKG_STEP_BEGIN, PKG_STEP_REVOKED, PKG_STEP_REMOVED,
                                             PKG_STEP_RECORDED };
    static const struct { enum pkg_op op; const enum pkg_step *s; unsigned n; } seq[] = {
        { PKG_OP_INSTALL, install, 5 }, { PKG_OP_UPGRADE, upgrade, 6 },
        { PKG_OP_REMOVE, remove_, 4 },
    };
    for (unsigned i = 0; i < 3; i++) {
        for (unsigned k = 0; k + 1 < seq[i].n; k++)
            EXPECT(pkg_journal_next(seq[i].op, seq[i].s[k]) == seq[i].s[k + 1],
                   "%s: the step after %s", pkg_op_name(seq[i].op), pkg_step_name(seq[i].s[k]));
        EXPECT(pkg_journal_next(seq[i].op, PKG_STEP_RECORDED) == PKG_STEPS, "a step after recorded");
    }
    EXPECT(pkg_journal_next(PKG_OP_INSTALL, PKG_STEP_REVOKED) == PKG_STEPS,
           "install has no revoked step");
    EXPECT(pkg_journal_next(PKG_OP_REMOVE, PKG_STEP_STAGED) == PKG_STEPS, "remove has no staged step");
}

/* The writer refuses what the reader would. */
static void write_refusals(struct pkg_journal *j)
{
    char text[2048];
    fill(j, PKG_OP_INSTALL, PKG_STEP_REVOKED, 0);
    EXPECT(pkg_journal_write(j, text, sizeof(text)) == 0, "install at revoked written");
    fill(j, PKG_OP_INSTALL, PKG_STEP_BEGIN, 1);
    EXPECT(pkg_journal_write(j, text, sizeof(text)) == 0, "an install with a busy line written");
    fill(j, PKG_OP_UPGRADE, PKG_STEP_BEGIN, 2);
    j->busy[1] = S("bin/a");   /* below the first */
    EXPECT(pkg_journal_write(j, text, sizeof(text)) == 0, "busy lines out of order written");
    fill(j, PKG_OP_UPGRADE, PKG_STEP_BEGIN, 1);
    j->busy[0] = S("../x");
    EXPECT(pkg_journal_write(j, text, sizeof(text)) == 0, "a busy line with .. written");
    fill(j, PKG_OP_UPGRADE, PKG_STEP_BEGIN, 0);
    j->revision = 0;
    EXPECT(pkg_journal_write(j, text, sizeof(text)) == 0, "revision 0 written");
    fill(j, PKG_OP_REMOVE, PKG_STEP_BEGIN, 0);
    j->name = S("Vim");
    EXPECT(pkg_journal_write(j, text, sizeof(text)) == 0, "a name with a capital written");
}

static const char upgrade_body[] =
    "jamos-pkg-journal 1\nop upgrade\nname vim\nversion 9.1.0\nrevision 2\nmanifest " H1
    "\nold 9.0.0 1 " H2 "\nstep swapped\nbusy bin/vim\nbusy share/z\n";

/* upgrade_body with its first `from` made `to`, sealed; refused. */
static void broken(struct pkg_journal *j, const char *what, const char *from, const char *to)
{
    char body[1024], text[1200];
    const char *at = strstr(upgrade_body, from);
    if (!at) {
        EXPECT(false, "%s: the test's own mistake", what);
        return;
    }
    size_t pre = (size_t)(at - upgrade_body), fl = strlen(from), tl = strlen(to);
    memcpy(body, upgrade_body, pre);
    memcpy(body + pre, to, tl);
    memcpy(body + pre + tl, at + fl, strlen(at + fl) + 1);
    size_t n = seal(text, sizeof(text), body);
    EXPECT(parse_exact(text, n, j, NULL) != OK, "%s was accepted", what);
}

static void damage(struct pkg_journal *j)
{
    char text[1200];
    size_t n = seal(text, sizeof(text), upgrade_body);
    EXPECT(parse_exact(text, n, j, NULL) == OK, "the hand-written upgrade was refused");
    EXPECT(j->nbusy == 2 && j->step == PKG_STEP_SWAPPED, "its fields");
    for (size_t cut = 0; cut < n; cut++)
        if (parse_exact(text, cut, j, NULL) == OK) {
            EXPECT(false, "a journal cut at %zu of %zu bytes was accepted", cut, n);
            break;
        }
    for (size_t i = 0; i < n; i++) {   /* any one changed byte: the SHA-256, or the grammar */
        char c = text[i];
        text[i] = (char)(c == 'a' ? 'b' : 'a');
        if (parse_exact(text, n, j, NULL) == OK) {
            EXPECT(false, "byte %zu changed and accepted", i);
            break;
        }
        text[i] = c;
    }
    broken(j, "another op word", "op upgrade", "op upgrad");
    broken(j, "an upgrade with the install's lines only", "old 9.0.0 1 " H2 "\n", "");
    broken(j, "a step the op has not (install's revoked is upgrade's, so remove's staged)",
           "op upgrade", "op remove");
    broken(j, "revision 02", "revision 2", "revision 02");
    broken(j, "revision 0", "revision 2", "revision 0");
    broken(j, "an upper-case hash", H1, "A" "111111111111111111111111111111111111111111111111111111111111111");
    broken(j, "a short hash", H2, "2222");
    broken(j, "busy lines out of order", "busy bin/vim\nbusy share/z", "busy share/z\nbusy bin/vim");
    broken(j, "a busy line twice", "busy share/z", "busy bin/vim");
    broken(j, "a busy path with ..", "busy share/z", "busy share/../z");
    broken(j, "an absolute busy path", "busy share/z", "busy /share/z");
    broken(j, "a line out of place", "name vim\nversion 9.1.0", "version 9.1.0\nname vim");
    broken(j, "an unknown line", "step swapped\n", "step swapped\nnote x\n");
    broken(j, "a blank line", "step swapped\n", "step swapped\n\n");
    broken(j, "two spaces", "name vim", "name  vim");
    broken(j, "a trailing space", "name vim", "name vim ");
    broken(j, "a step that is not one", "step swapped", "step swapped1");
    broken(j, "install with an old line", "op upgrade", "op install");
    broken(j, "a name with a capital", "name vim", "name Vim");
    broken(j, "a first line of another version", "jamos-pkg-journal 1", "jamos-pkg-journal 2");
    broken(j, "a non-ASCII byte", "name vim", "name v\xc3\xafm");
}

unsigned pkgc_journal(uint64_t seed, unsigned iterations)
{
    static const enum pkg_op ops[] = { PKG_OP_INSTALL, PKG_OP_UPGRADE, PKG_OP_REMOVE };
    struct pkgc_work *k = pkgc_work();
    uint64_t rng = seed;
    char canon[3][2048];
    size_t clen[3];
    if (!k)
        return 1;
    bad = 0;
    for (unsigned i = 0; i < 3; i++) {
        round_trip(k->j, ops[i], PKG_STEP_BEGIN, 0);
        round_trip(k->j, ops[i], PKG_STEP_RECORDED, 0);
    }
    round_trip(k->j, PKG_OP_UPGRADE, PKG_STEP_SWAPPED, 3);
    next_steps();
    write_refusals(k->j);
    damage(k->j);
    for (unsigned i = 0; i < 3; i++) {
        fill(k->j, ops[i], ops[i] == PKG_OP_REMOVE ? PKG_STEP_REVOKED : PKG_STEP_STAGED,
             ops[i] == PKG_OP_UPGRADE ? 3 : 0);
        clen[i] = pkg_journal_write(k->j, canon[i], sizeof(canon[i]));
    }
    for (unsigned it = 0; it < iterations && bad < 20; it++) {
        char work[2200], text[2300];
        unsigned c = it % 3;
        size_t n = clen[c] - (4 + 2 * SHA256_BYTES + 1), len = n;   /* the body alone */
        memcpy(work, canon[c], n);
        for (unsigned m = 1 + (unsigned)(pkgc_rand(&rng) % 3); m; m--) {
            size_t at = (size_t)(pkgc_rand(&rng) % (len ? len : 1));
            switch (pkgc_rand(&rng) % 4) {
            case 0: work[at] ^= (char)(1u << (pkgc_rand(&rng) & 7)); break;
            case 1: work[at] = (char)(0x20 + pkgc_rand(&rng) % 95); break;
            case 2: len = at; break;
            default: if (len + 1 < sizeof(work) - 1 && len) {
                         memmove(work + at + 1, work + at, len - at);
                         work[at] = "\n 0-/x"[pkgc_rand(&rng) % 6];
                         len++;
                     }
            }
        }
        work[len] = '\0';
        bool sealed = pkgc_rand(&rng) & 1;   /* the SHA-256 made right, so the grammar is read */
        size_t tn;
        if (sealed) {
            tn = seal(text, sizeof(text), work);
        } else {
            memcpy(text, work, len);
            memcpy(text + len, canon[c] + n, clen[c] - n);
            tn = len + clen[c] - n;
        }
        char *copy = malloc(tn ? tn : 1);   /* the structure points into it */
        if (!copy)
            return bad + 1;
        memcpy(copy, text, tn);
        if (pkg_journal_parse(copy, tn, k->j, NULL) == OK) {
            char again[2300];
            size_t an = pkg_journal_write(k->j, again, sizeof(again));
            EXPECT(an == tn && !memcmp(again, text, tn), "an accepted mutant is not canonical");
            EXPECT(sealed || (tn == clen[c] && !memcmp(text, canon[c], tn)),
                   "a changed journal with the old SHA-256 was accepted");
        }
        free(copy);
    }
    return bad;
}
