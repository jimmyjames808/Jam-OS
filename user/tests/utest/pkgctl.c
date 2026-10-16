/* utest: init's approvals of a package's programs (user/services/init/
 * pkgapprove.c, abi/idl/pkgctl.idl; docs/M13.7-PLAN.md section 9), over
 * fat on a RAM disk mounted at /pkgt, with the test vectors' key (its seed
 * is public: tools/portslib/sign.py's test_key) so that a test can sign
 * manifests of its own. A good package is approved: one line, tagged, and
 * its table, the very bytes the package shipped; again, no second line;
 * staged at `.new`, the lines name the final path; another package's
 * line on the path refuses it; revoke takes the lines and the table back.
 * Refused, with nothing written: a changed file (a byte, a length, gone),
 * a changed table (signed), another key, no signature, a list that isn't
 * the program's own, a list no program from /data may have, a replayed
 * manifest of another version. The settle of a journal: an install cut
 * before its commit point undone, one after it finished, a remove
 * finished, a torn journal left alone. The want `mount /data rw pkg`. And
 * init's own channel (initctl.pkg_channel): it answers, and refuses the
 * vectors' package (the build's key, if it has one, isn't the test's). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <dataprog.h>
#include <idl/initctl.h>
#include <idl/pkgctl.h>
#include <monocypher-ed25519.h>
#include <pkgfmt.h>
#include <sha256.h>
#include <update.h>
#include <wants.h>
#include "fattest.h"
#include "pkgapprove.h"
#include "pkgcases.h"
#include "ramdisk.h"
#include "utest.h"

#define MNT     "/pkgt"
#define VEC     MNT "/pkg/bin/vec"
#define ALLOW   MNT "/etc/allow"
#define TEXTCAP 8192u                  /* a vector's manifest, and room to change it */

static const struct pkga_place at = {
    .prefix = MNT "/pkg", .etc = MNT "/etc", .allow = ALLOW, .tables = MNT "/etc/allow.d",
};
static struct ramdisk disk;
static struct fatrun  run;
static uint8_t        sk[64], pk[32];  /* the vectors' key */
static uint8_t        sk2[64], pk2[32];   /* their other one */

static void test_key(const char *label, uint8_t s[64], uint8_t p[32])
{
    char phrase[96];
    uint8_t seed[32];
    snprintf(phrase, sizeof(phrase), "jamos %s key: public, for tests only", label);
    sha256(phrase, strlen(phrase), seed);
    crypto_ed25519_key_pair(s, p, seed);   /* wipes seed */
}

/* A RAM disk's fat at /pkgt, and the keys (the first must be test.pub). */
static bool up(void)
{
    handle_t h;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&run, &disk, false))
        return false;
    CHECK_ST(jam_handle_duplicate(run.fs, RIGHT_SAME, &h), OK);
    CHECK_ST(ns_mount(MNT, h), OK);
    test_key("ports vectors", sk, pk);
    test_key("ports vectors, another", sk2, pk2);
    const struct pkgv_file *pub = pkgv_find("test.pub");
    uint8_t want[PKG_KEY_BYTES];
    CHECK(pub);
    CHECK_ST(update_key_parse(pub->data, pub->len, want), OK);
    CHECK(!memcmp(want, pk, sizeof(pk)));
    return true;
}

static bool down(void)
{
    CHECK_ST(ns_unmount(MNT), OK);
    return fat_stop(&run) && ramdisk_destroy(&disk);
}

/* ---- files ------------------------------------------------------------------------- */

static status_t wfile(const char *path, const void *data, size_t n)
{
    struct jfile f;
    size_t done = 0;
    status_t st = file_open(path, FS_WRITE | FS_CREATE | FS_TRUNCATE, &f);
    if (st != OK)
        return st;
    st = file_write(&f, 0, data, n, &done);
    file_close(&f);
    return st == OK && done != n ? ERR_IO : st;
}

/* Every directory above path, from MNT down. */
static bool mkdirs(const char *path)
{
    char d[PKG_PATH_MAX + 64];
    for (const char *s = strchr(path + strlen(MNT) + 1, '/'); s; s = strchr(s + 1, '/')) {
        snprintf(d, sizeof(d), "%.*s", (int)(s - path), path);
        status_t st = fs_mkdir(d);
        CHECK(st == OK || st == ERR_ALREADY_EXISTS);
    }
    return true;
}

/* The file's text (NUL-terminated) into out; its length, or -1 if none. */
static long rtext(const char *path, char *out, size_t cap)
{
    handle_t v;
    uint64_t size;
    if (file_read_vmo(path, cap - 1, &v, &size) != OK)
        return -1;
    status_t st = jam_vmo_read(v, 0, out, size);
    jam_handle_close(v);
    if (st != OK)
        return -1;
    out[size] = '\0';
    return (long)size;
}

static bool exists(const char *path)
{
    return fs_stat(path, NULL, NULL, NULL) == OK;
}

static uint64_t up512(uint64_t n)
{
    return (n + PKG_BLOCK - 1) & ~(uint64_t)(PKG_BLOCK - 1);
}

/* The manifest of a vector package (pointers into it). */
static bool manifest_of(const char *name, const uint8_t **text, uint32_t *len)
{
    const struct pkgv_file *v = pkgv_find(name);
    CHECK(v);
    CHECK_ST(pkg_first_header(v->data, len), OK);
    *text = v->data + PKG_BLOCK;
    return true;
}

/* A vector package's files written under the prefix, each at its path
 * and suffix (".new": staged); data (if not NULL): bin/vec's bytes
 * instead. Also its +TABLES member for bin/vec into *table (may be NULL). */
static bool stage(const char *name, const char *suffix, const uint8_t *vec,
                  const uint8_t **table)
{
    const struct pkgv_file *v = pkgv_find(name);
    const uint8_t *text;
    uint32_t len;
    CHECK(v && manifest_of(name, &text, &len));
    struct pkg_manifest *m = malloc(sizeof(*m));
    CHECK(m);
    CHECK_ST(pkg_manifest_parse_unchecked(text, len, m, NULL), OK);
    uint64_t off = PKG_BLOCK + up512(len);
    for (uint32_t i = 0; i < m->nfiles; i++) {
        const struct pkg_file *f = &m->files[i];
        char path[PKG_PATH_MAX + 64];
        snprintf(path, sizeof(path), "%s/%.*s%s", at.prefix, (int)f->path.n, f->path.p, suffix);
        off += PKG_BLOCK;
        bool is_vec = pkg_str_eq(f->path, "bin/vec");
        CHECK(mkdirs(path));
        CHECK_ST(wfile(path, is_vec && vec ? vec : v->data + off, f->size), OK);
        off += up512(f->size);
    }
    if (table)
        *table = v->data + off + PKG_BLOCK;   /* the first table: bin/vec's */
    free(m);
    return true;
}

/* bin/vec as good.jpk has it. */
static const uint8_t *vec_bytes(void)
{
    const uint8_t *text;
    uint32_t len;
    if (!manifest_of("good.jpk", &text, &len))
        return NULL;
    return pkgv_find("good.jpk")->data + PKG_BLOCK + up512(len) + PKG_BLOCK;
}
#define VEC_SIZE 4100u

/* ---- manifests --------------------------------------------------------------------- */

static char body[TEXTCAP], signed_text[TEXTCAP];

/* good.jpk's manifest without its signature line, into body, with up to
 * two changes (old -> new; NULL: none); then signed with s into
 * signed_text. Its length. */
static size_t resign(const char *old1, const char *new1, const char *old2, const char *new2,
                     const uint8_t s[64])
{
    const uint8_t *text;
    uint32_t len;
    if (!manifest_of("good.jpk", &text, &len) || len >= TEXTCAP)
        return 0;
    char raw[TEXTCAP];
    memcpy(raw, text, len);
    raw[len] = '\0';
    char *sig = strstr(raw, "\nsignature ");
    if (!sig)
        return 0;
    sig[1] = '\0';
    snprintf(body, sizeof(body), "%s", raw);
    const char *olds[] = { old1, old2 }, *news[] = { new1, new2 };
    for (int k = 0; k < 2; k++) {
        if (!olds[k])
            continue;
        char *p = strstr(body, olds[k]);
        if (!p)
            return 0;
        char rest[TEXTCAP];
        snprintf(rest, sizeof(rest), "%s", p + strlen(olds[k]));
        snprintf(p, sizeof(body) - (size_t)(p - body), "%s%s", news[k], rest);
    }
    uint8_t mac[64];
    char hex[129];
    size_t n = strlen(body);
    crypto_ed25519_sign(mac, s, (const uint8_t *)body, n);
    for (int i = 0; i < 64; i++)
        snprintf(hex + 2 * i, 3, "%02x", mac[i]);
    snprintf(signed_text, sizeof(signed_text), "%ssignature %s\n", body, hex);
    return strlen(signed_text);
}

static status_t approve(const void *text, size_t len, uint32_t flags, struct pkga_result *r)
{
    status_t st = pkga_approve(&at, pk, text, len, flags, r);
    if (st != OK)
        printf("utest: %s: refused (as it may be): %s\n", utest_cur, r->why);
    return st;
}

static status_t approve_vector(const char *name, uint32_t flags, struct pkga_result *r)
{
    const uint8_t *text;
    uint32_t len;
    if (!manifest_of(name, &text, &len))
        return ERR_INTERNAL;
    return approve(text, len, flags, r);
}

/* The line good.jpk's bin/vec gets, with this tag ("vec 1.0-r1"). */
static void vec_line(const char *tag, char *out, size_t cap)
{
    struct wants w;
    char hex[DP_HEX_LEN + 1];
    uint8_t d[SHA256_BYTES];
    (void)wants_parse("posix\nmount /data r\n", 20, &w);
    sha256(vec_bytes(), VEC_SIZE, d);
    sha256_hex(d, hex);
    snprintf(out, cap, "%s\t%s\t%s\tpkg=%s", hex, VEC, w.text, tag);
}

static bool nothing_written(void)
{
    CHECK(!exists(ALLOW));
    CHECK(!exists(MNT "/etc/allow.d"));
    return true;
}

/* ---- the tests --------------------------------------------------------------------- */

bool t_pkgctl_good(void)
{
    static char text[16384], line[1024];
    struct pkga_result r;
    const uint8_t *shipped;
    if (!up() || !stage("good.jpk", "", NULL, &shipped))
        return false;
    CHECK_ST(approve_vector("good.jpk", 0, &r), OK);
    CHECK_EQ(r.programs, 1u);
    CHECK_EQ(r.program, PKGA_NONE);
    vec_line("vec 1.0-r1", line, sizeof(line));
    CHECK(rtext(ALLOW, text, sizeof(text)) > 0);
    char want[1100];
    snprintf(want, sizeof(want), "%s\n", line);
    CHECK(!strcmp(text, want));
    /* The table written is init's own, and the very bytes shipped. */
    char tpath[128], hex[DP_HEX_LEN + 1];
    memcpy(hex, line, DP_HEX_LEN);
    hex[DP_HEX_LEN] = '\0';
    snprintf(tpath, sizeof(tpath), MNT "/etc/allow.d/%s", hex);
    static uint8_t table[256];
    handle_t tv;
    uint64_t tsize;
    CHECK_ST(file_read_vmo(tpath, sizeof(table), &tv, &tsize), OK);
    CHECK_EQ(tsize, 128u);
    CHECK_ST(jam_vmo_read(tv, 0, table, tsize), OK);
    jam_handle_close(tv);
    CHECK(!memcmp(table, shipped, tsize));
    CHECK_ST(dp_ptable_check((const struct dp_ptable_head *)table, tsize, VEC_SIZE,
                             ((const struct dp_ptable_head *)table)->sha256), OK);
    /* The tag, as the shell reads it. */
    char copy[1100], *h, *p, *t;
    snprintf(copy, sizeof(copy), "%s", line);
    CHECK(dp_allow_split(copy, &h, &p, &t));
    const char *tag = dp_allow_tag(t);
    CHECK(tag && !strcmp(tag, "vec 1.0-r1") && !strcmp(p, VEC) && !strchr(t, '\t'));
    /* Again: no second line. The owner's own lines stay as they are. */
    char mine[1200];
    snprintf(mine, sizeof(mine), "# a comment\n%s\t/pkgt/x\tnothing\n%s\n", hex, line);
    CHECK_ST(wfile(ALLOW, mine, strlen(mine)), OK);
    CHECK_ST(approve_vector("good.jpk", 0, &r), OK);
    CHECK(rtext(ALLOW, text, sizeof(text)) > 0 && !strcmp(text, mine));
    /* Staged: read at .new, the line names the path. */
    CHECK_ST(fs_unlink(ALLOW), OK);
    CHECK_ST(fs_unlink(VEC), OK);
    CHECK(stage("good.jpk", ".new", NULL, NULL));
    CHECK_ST(approve_vector("good.jpk", 0, &r), ERR_NOT_FOUND);   /* not at its path */
    CHECK(!exists(ALLOW));
    CHECK_ST(approve_vector("good.jpk", PKGA_STAGED, &r), OK);
    CHECK(rtext(ALLOW, text, sizeof(text)) > 0 && !strcmp(text, want));
    CHECK_ST(approve_vector("good.jpk", 2, &r), ERR_INVALID_ARGS);   /* a flag no one knows */
    /* Revoke: the line and its table go; the owner's line for the same
     * hash would have kept the table. */
    uint32_t lines = 9;
    struct pkg_str vecname = { "vec", 3 }, v10 = { "1.0", 3 }, v11 = { "1.1", 3 };
    CHECK_ST(pkga_revoke(&at, vecname, v11, 1, &lines), OK);
    CHECK_EQ(lines, 0u);
    CHECK_ST(pkga_revoke(&at, vecname, v10, 2, &lines), OK);
    CHECK_EQ(lines, 0u);
    CHECK_ST(pkga_revoke(&at, vecname, v10, 1, &lines), OK);
    CHECK_EQ(lines, 1u);
    CHECK(rtext(ALLOW, text, sizeof(text)) == 0);
    CHECK(!exists(tpath));
    CHECK_ST(pkga_revoke(&at, vecname, v10, 1, &lines), OK);
    CHECK_EQ(lines, 0u);
    CHECK_ST(pkga_revoke(&at, vecname, v10, 0, &lines), ERR_INVALID_ARGS);
    CHECK_ST(pkga_revoke(&at, (struct pkg_str){ "a b", 3 }, v10, 1, &lines), ERR_INVALID_ARGS);
    /* Another package's line on the path: refused, nothing changed. */
    snprintf(mine, sizeof(mine), "%s\t%s\tnothing\tpkg=other 2-r1\n", hex, VEC);
    CHECK_ST(wfile(ALLOW, mine, strlen(mine)), OK);
    CHECK_ST(approve_vector("good.jpk", PKGA_STAGED, &r), ERR_ACCESS_DENIED);
    CHECK_EQ(r.program, 0u);
    CHECK(rtext(ALLOW, text, sizeof(text)) > 0 && !strcmp(text, mine));
    return down();
}

bool t_pkgctl_changed_file(void)
{
    struct pkga_result r;
    static uint8_t vec[VEC_SIZE + 1];
    if (!up())
        return false;
    memcpy(vec, vec_bytes(), VEC_SIZE);
    vec[300] ^= 1;   /* past its ELF headers and note: only the hash says */
    CHECK(stage("good.jpk", "", vec, NULL));
    CHECK_ST(approve_vector("good.jpk", 0, &r), ERR_ACCESS_DENIED);
    CHECK_EQ(r.program, 0u);
    CHECK(strstr(r.why, "SHA-256"));
    CHECK(nothing_written());
    vec[300] ^= 1;
    vec[VEC_SIZE] = 0;   /* one byte longer */
    CHECK_ST(wfile(VEC, vec, VEC_SIZE + 1), OK);
    CHECK_ST(approve_vector("good.jpk", 0, &r), ERR_ACCESS_DENIED);
    CHECK(nothing_written());
    CHECK_ST(fs_unlink(VEC), OK);
    CHECK_ST(approve_vector("good.jpk", 0, &r), ERR_NOT_FOUND);
    CHECK(nothing_written());
    return down();
}

bool t_pkgctl_changed_table(void)
{
    struct pkga_result r;
    if (!up() || !stage("good.jpk", "", NULL, NULL))
        return false;
    /* Signed, but not the table the program makes. */
    CHECK_ST(approve_vector("p-table-signed.jpk", 0, &r), ERR_ACCESS_DENIED);
    CHECK(strstr(r.why, "page table"));
    CHECK(nothing_written());
    /* The table line's size changed and signed: the manifest's own rule
     * (a table's size follows from its file's) refuses it first. */
    CHECK(resign("table bin/vec 128 ", "table bin/vec 160 ", NULL, NULL, sk));
    CHECK_ST(approve(signed_text, strlen(signed_text), 0, &r), ERR_INVALID_ARGS);
    CHECK(nothing_written());
    return down();
}

bool t_pkgctl_wrong_key(void)
{
    struct pkga_result r;
    if (!up() || !stage("good.jpk", "", NULL, NULL))
        return false;
    CHECK_ST(approve_vector("p-otherkey.jpk", 0, &r), ERR_ACCESS_DENIED);
    CHECK_ST(approve_vector("p-unsigned.jpk", 0, &r), ERR_ACCESS_DENIED);
    CHECK_ST(approve_vector("p-manifest.jpk", 0, &r), ERR_ACCESS_DENIED);   /* changed after */
    CHECK_ST(approve_vector("p-domain.jpk", 0, &r), ERR_INVALID_ARGS);      /* an index's */
    size_t n = resign(NULL, NULL, NULL, NULL, sk2);
    CHECK(n);
    CHECK_ST(approve(signed_text, n, 0, &r), ERR_ACCESS_DENIED);
    /* The good manifest under another key than the build's. */
    const uint8_t *text;
    uint32_t len;
    CHECK(manifest_of("good.jpk", &text, &len));
    CHECK_ST(pkga_approve(&at, pk2, text, len, 0, &r), ERR_ACCESS_DENIED);
    CHECK(nothing_written());
    /* And with the right one, signed here: it is the key, not the bytes. */
    CHECK_EQ(resign(NULL, NULL, NULL, NULL, sk), len);
    CHECK(!memcmp(signed_text, text, len));   /* Ed25519 signs alike every time */
    return down();
}

bool t_pkgctl_refused_list(void)
{
    struct pkga_result r;
    if (!up() || !stage("good.jpk", "", NULL, NULL))
        return false;
    /* The manifest's list isn't the program's own (its note says r). */
    CHECK_ST(approve_vector("p-list.jpk", 0, &r), ERR_ACCESS_DENIED);
    CHECK(strstr(r.why, "own list"));
    CHECK(nothing_written());
    /* Lists no program from /data may have, signed: refused before any
     * file is read. */
    static const char *const bad[] = {
        "posix\\x0asvc session\\x0a", "posix\\x0amount /data rw pkg\\x0a",
        "posix\\x0aright debug\\x0a", "posix\\x0asvc init\\x0a", "mount /data r\\x0a",
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char line[96];
        snprintf(line, sizeof(line), "program bin/vec %s\n", bad[i]);
        CHECK(resign("program bin/vec posix\\x0amount /data r\\x0a\n", line, NULL, NULL, sk));
        CHECK_ST(approve(signed_text, strlen(signed_text), 0, &r), ERR_ACCESS_DENIED);
    }
    CHECK(nothing_written());
    return down();
}

bool t_pkgctl_other_version(void)
{
    static char text[16384], v11[TEXTCAP];
    static uint8_t vec[VEC_SIZE], table[128];
    struct pkga_result r;
    if (!up())
        return false;
    /* Version 1.1: bin/vec with another byte (the same list), its own hash
     * and table, signed. */
    memcpy(vec, vec_bytes(), VEC_SIZE);
    vec[300] ^= 1;
    uint8_t d[SHA256_BYTES], td[SHA256_BYTES];
    char hex[DP_HEX_LEN + 1], thex[DP_HEX_LEN + 1], file[160], tline[160];
    struct pkg_tabler t;
    pkg_tabler_begin(&t, VEC_SIZE, table, NULL);
    CHECK(pkg_tabler_add(&t, vec, VEC_SIZE) && pkg_tabler_end(&t));
    sha256(vec, VEC_SIZE, d);
    sha256(table, sizeof(table), td);
    sha256_hex(d, hex);
    sha256_hex(td, thex);
    snprintf(file, sizeof(file), "file bin/vec 755 4100 %s\n", hex);
    snprintf(tline, sizeof(tline), "table bin/vec 128 %s\n", thex);
    CHECK(resign("version 1.0\n", "version 1.1\n", NULL, NULL, sk));
    {   /* body is 1.0's with the version changed: the file and table lines too */
        char *a = strstr(body, "file bin/vec 755 4100 "), *b = a ? strchr(a, '\n') : NULL;
        char *c = strstr(body, "table bin/vec 128 "), *e = c ? strchr(c, '\n') : NULL;
        CHECK(a && b && c && e && c > b);
        char out[TEXTCAP];
        snprintf(out, sizeof(out), "%.*s%s%.*s%s%s", (int)(a - body), body, file,
                 (int)(c - (b + 1)), b + 1, tline, e + 1);
        snprintf(body, sizeof(body), "%s", out);
        uint8_t mac[64];
        crypto_ed25519_sign(mac, sk, (const uint8_t *)body, strlen(body));
        char sig[129];
        for (int i = 0; i < 64; i++)
            snprintf(sig + 2 * i, 3, "%02x", mac[i]);
        snprintf(v11, sizeof(v11), "%ssignature %s\n", body, sig);
    }
    /* 1.0's files: 1.1's manifest refused, 1.0's approved. */
    CHECK(stage("good.jpk", "", NULL, NULL));
    CHECK_ST(approve(v11, strlen(v11), 0, &r), ERR_ACCESS_DENIED);
    CHECK(nothing_written());
    CHECK_ST(approve_vector("good.jpk", 0, &r), OK);
    /* 1.1's files staged: 1.0's manifest (a replay of another version) is
     * refused, 1.1's approved beside 1.0's line (either may run during
     * the swap), then 1.0's revoked. */
    CHECK_ST(wfile(VEC ".new", vec, VEC_SIZE), OK);
    CHECK_ST(approve_vector("good.jpk", PKGA_STAGED, &r), ERR_ACCESS_DENIED);
    CHECK_ST(approve(v11, strlen(v11), PKGA_STAGED, &r), OK);
    char l10[1024], want[2100];
    vec_line("vec 1.0-r1", l10, sizeof(l10));
    CHECK(rtext(ALLOW, text, sizeof(text)) > 0);
    snprintf(want, sizeof(want), "%s\n%s\t%s\t/data (read)\tpkg=vec 1.1-r1\n", l10, hex, VEC);
    CHECK(!strcmp(text, want));
    uint32_t lines;
    CHECK_ST(pkga_revoke(&at, (struct pkg_str){ "vec", 3 }, (struct pkg_str){ "1.0", 3 }, 1,
                         &lines), OK);
    CHECK_EQ(lines, 1u);
    CHECK(rtext(ALLOW, text, sizeof(text)) > 0 && !strcmp(text, want + strlen(l10) + 1));
    return down();
}

/* ---- the settle -------------------------------------------------------------------- */

/* A journal for good.jpk's vec: op, step; old (upgrade, remove) is 1.0's
 * manifest too. */
static bool journal(enum pkg_op op, enum pkg_step step)
{
    const uint8_t *text;
    uint32_t len;
    CHECK(manifest_of("good.jpk", &text, &len));
    struct pkg_journal *j = calloc(1, sizeof(*j));
    static char out[4096];
    CHECK(j);
    j->op = op;
    j->step = step;
    j->name = (struct pkg_str){ "vec", 3 };
    if (op != PKG_OP_REMOVE) {
        j->version = (struct pkg_str){ "1.0", 3 };
        j->revision = 1;
        sha256(text, len, j->manifest);
    }
    if (op != PKG_OP_INSTALL) {
        j->old_version = (struct pkg_str){ "1.0", 3 };
        j->old_revision = 1;
        sha256(text, len, j->old_manifest);
    }
    size_t n = pkg_journal_write(j, out, sizeof(out));
    free(j);
    CHECK(n);
    CHECK(mkdirs(MNT "/pkg/db/journal"));
    CHECK_ST(wfile(MNT "/pkg/db/journal", out, n), OK);
    return true;
}

static bool db(const char *suffix)
{
    const uint8_t *text;
    uint32_t len;
    char path[64];
    CHECK(manifest_of("good.jpk", &text, &len));
    snprintf(path, sizeof(path), MNT "/pkg/db/vec%s", suffix);
    CHECK(mkdirs(path));
    CHECK_ST(wfile(path, text, len), OK);
    return true;
}

static bool settle(bool journal_was, bool forward, status_t st)
{
    struct pkga_settled s;
    pkga_settle(&at, pk, &s);
    printf("utest: %s: settle: %s\n", utest_cur, s.what);
    CHECK_EQ(s.journal, journal_was);
    CHECK_EQ(s.forward, forward);
    CHECK_ST(s.st, st);
    return true;
}

bool t_pkgctl_settle(void)
{
    struct pkga_result r;
    if (!up())
        return false;
    CHECK(settle(false, false, OK));   /* no journal: nothing */
    /* An install cut before its commit point: undone. */
    CHECK(stage("good.jpk", ".new", NULL, NULL) && db(".new") &&
          journal(PKG_OP_INSTALL, PKG_STEP_STAGED));
    CHECK(settle(true, false, OK));
    CHECK(!exists(VEC ".new") && !exists(MNT "/pkg/share/vec/notes.txt.new"));
    CHECK(!exists(MNT "/pkg/db/vec.new") && !exists(MNT "/pkg/db/journal"));
    /* The same, at `approved` (half the files swapped): finished. */
    CHECK(stage("good.jpk", ".new", NULL, NULL) && db(".new"));
    CHECK_ST(approve_vector("good.jpk", PKGA_STAGED, &r), OK);
    CHECK_ST(fs_rename(VEC ".new", VEC), OK);
    CHECK(journal(PKG_OP_INSTALL, PKG_STEP_APPROVED));
    CHECK(settle(true, true, OK));
    CHECK(exists(VEC) && !exists(VEC ".new") && exists(MNT "/pkg/share/vec/notes.txt"));
    CHECK(exists(MNT "/pkg/db/vec") && !exists(MNT "/pkg/db/vec.new"));
    CHECK(!exists(MNT "/pkg/db/journal"));
    CHECK(exists(ALLOW));
    /* A remove cut at its start: revoked, its files and record gone. */
    CHECK(journal(PKG_OP_REMOVE, PKG_STEP_BEGIN));
    CHECK(settle(true, true, OK));
    char text[256];
    CHECK(rtext(ALLOW, text, sizeof(text)) == 0);
    CHECK(!exists(VEC) && !exists(MNT "/pkg/share/vec/notes.txt"));
    CHECK(!exists(MNT "/pkg/db/vec") && !exists(MNT "/pkg/db/journal"));
    /* A torn journal: left for pkg, nothing touched. */
    CHECK(journal(PKG_OP_REMOVE, PKG_STEP_BEGIN));
    CHECK_ST(wfile(MNT "/pkg/db/journal", "jamos-pkg-journal 1\nop remove\n", 30), OK);
    CHECK(settle(true, false, ERR_INVALID_ARGS));
    CHECK(exists(MNT "/pkg/db/journal"));
    /* No key (a build without one): left as it is. */
    struct pkga_settled s;
    CHECK(journal(PKG_OP_REMOVE, PKG_STEP_BEGIN));
    pkga_settle(&at, NULL, &s);
    CHECK(s.journal && s.st == ERR_NOT_SUPPORTED && exists(MNT "/pkg/db/journal"));
    return down();
}

/* ---- the want, and init's channel -------------------------------------------------- */

bool t_pkgctl_want(void)
{
    struct wants w;
    CHECK_ST(wants_parse("mount /data rw pkg\n", 19, &w), OK);
    CHECK(w.n == 1 && !strcmp(w.grant[0], "/data:wp"));
    CHECK(dp_wants_refused(&w) && !strcmp(dp_wants_refused(&w), "mount /data rw pkg"));
    CHECK_ST(wants_parse("mount /jam rw pkg\n", 18, &w), OK);   /* the same on /jam */
    CHECK(w.n == 1 && !strcmp(w.grant[0], "/jam:wp"));
    CHECK(dp_wants_refused(&w) && !strcmp(dp_wants_refused(&w), "mount /jam rw pkg"));
    CHECK_ST(wants_parse("mount /data rw\n", 15, &w), OK);
    CHECK(w.n == 1 && !strcmp(w.grant[0], "/data:w") && !dp_wants_refused(&w));
    CHECK_ST(wants_parse("mount /jam rw\n", 14, &w), OK);   /* an ordinary view of /jam */
    CHECK(w.n == 1 && !strcmp(w.grant[0], "/jam:w") && !dp_wants_refused(&w));
    CHECK(dp_on_data("/jam/pkg/bin/x") && dp_on_data("/data/x") && !dp_on_data("/jam") &&
          !dp_on_data("/jam/") && !dp_on_data("/jamx/y") && !dp_on_data("/usb0/x"));
    static const char *const bad[] = { "mount /data r pkg\n", "mount /usb* rw pkg\n",
                                       "mount * rw pkg\n", "mount /data rw pkg x\n",
                                       "mount /data rw Pkg\n", "mount /data rwpkg\n",
                                       "mount /jam r pkg\n", "mount /jam rw Pkg\n",
                                       "mount /jam rwpkg\n", "mount /jamx rw pkg\n" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        CHECK_ST(wants_parse(bad[i], strlen(bad[i]), &w), ERR_INVALID_ARGS);
    return true;
}

bool t_pkgctl_init(void)
{
    handle_t ctl = svc_get(SVC_INIT), ch, ch2, v;
    if (!ctl) {
        printf("utest: %s: no init control channel (not started by the shell?): skipped\n",
               utest_cur);
        return true;
    }
    uint64_t dl = now() + 30 * NS_PER_S;
    CHECK_ST(initctl_pkg_channel_until(ctl, dl, &ch), OK);
    const uint8_t *text;
    uint32_t len;
    CHECK(manifest_of("good.jpk", &text, &len));
    CHECK_ST(jam_vmo_create(PAGE_SIZE, 0, &v), OK);
    CHECK_ST(jam_vmo_write(v, 0, text, len), OK);
    int32_t verdict = 0;
    uint32_t programs = 9, program = 0, why_len = 0, lines = 9;
    char why[PKGCTL_WHY_MAX + 1];
    handle_t dup;
    CHECK_ST(jam_handle_duplicate(v, RIGHT_SAME, &dup), OK);
    CHECK_ST(pkgctl_approve_until(ch, dl, 0, len, dup, &verdict, &programs, &program, why,
                                  &why_len), OK);
    why[why_len] = '\0';
    printf("utest: %s: init said: %s\n", utest_cur, why);
    /* No key, or the build's (never the vectors' test key): refused. */
    CHECK_ST(verdict, ERR_ACCESS_DENIED);
    CHECK(programs == 0 && program == PKGA_NONE);
    CHECK(strstr(why, "ports key") || strstr(why, "signature") || strstr(why, "key"));
    CHECK_ST(jam_handle_duplicate(v, RIGHT_SAME, &dup), OK);
    CHECK_ST(pkgctl_approve_until(ch, dl, 0, 0, dup, &verdict, &programs, &program, why,
                                  &why_len), ERR_OUT_OF_RANGE);
    CHECK_ST(jam_handle_duplicate(v, RIGHT_SAME, &dup), OK);
    CHECK_ST(pkgctl_approve_until(ch, dl, 0, PAGE_SIZE + 1, dup, &verdict, &programs, &program,
                                  why, &why_len), ERR_OUT_OF_RANGE);   /* past the VMO */
    CHECK_ST(pkgctl_revoke_until(ch, dl, "", 0, "1.0", 3, 1, &lines), ERR_INVALID_ARGS);
    /* A tag no line has: none go (or /data isn't there yet: its error). */
    status_t st = pkgctl_revoke_until(ch, dl, "utest-none", 10, "1.0", 3, 1, &lines);
    printf("utest: %s: revoke of a tag no line has: %s\n", utest_cur, status_str(st));
    CHECK(st != ERR_PEER_CLOSED && st != ERR_INVALID_ARGS && lines == 0);
    /* A newer channel closes the older. */
    CHECK_ST(initctl_pkg_channel_until(ctl, dl, &ch2), OK);
    CHECK_ST(jam_handle_duplicate(v, RIGHT_SAME, &dup), OK);
    CHECK_ST(pkgctl_approve_until(ch, dl, 0, 0, dup, &verdict, &programs, &program, why,
                                  &why_len), ERR_PEER_CLOSED);
    CHECK_ST(jam_handle_duplicate(v, RIGHT_SAME, &dup), OK);
    CHECK_ST(pkgctl_approve_until(ch2, dl, 0, 0, dup, &verdict, &programs, &program, why,
                                  &why_len), ERR_OUT_OF_RANGE);
    jam_handle_close(v);
    jam_handle_close(ch);
    jam_handle_close(ch2);
    return true;
}
