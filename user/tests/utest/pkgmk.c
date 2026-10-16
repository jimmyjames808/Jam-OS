/* utest: the shared parts of bin/pkg's tests (pkgmk.h). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <stdarg.h>
#include <check.h>
#include <monocypher-ed25519.h>
#include <sha256.h>
#include <update.h>
#include "fattest.h"
#include "pkgcases.h"
#include "pkgmk.h"
#include "ramdisk.h"
#include "utest.h"

const struct pkga_place mk_at = {
    .prefix = MK_PREFIX, .etc = MK_MNT "/etc", .allow = MK_ALLOW, .tables = MK_MNT "/etc/allow.d",
};
uint8_t mk_sk[64], mk_pk[32];

static struct ramdisk disk;
static struct fatrun  run;

static void test_key(uint8_t s[64], uint8_t p[32])
{
    uint8_t seed[32];
    const char *phrase = "jamos ports vectors key: public, for tests only";
    sha256(phrase, strlen(phrase), seed);
    crypto_ed25519_key_pair(s, p, seed);   /* wipes seed */
}

bool mk_up(void)
{
    handle_t h;
    if (!ramdisk_create(&disk, 16 * 2048) || !fat_start(&run, &disk, false))
        return false;
    CHECK_ST(jam_handle_duplicate(run.fs, RIGHT_SAME, &h), OK);
    CHECK_ST(ns_mount(MK_MNT, h), OK);
    test_key(mk_sk, mk_pk);
    return true;
}

bool mk_down(void)
{
    CHECK_ST(ns_unmount(MK_MNT), OK);
    return fat_stop(&run) && ramdisk_destroy(&disk);
}

static status_t ctl_approve(void *ctx, uint32_t flags, const void *text, size_t len,
                            struct pk_verdict *v)
{
    (void)ctx;
    struct pkga_result r;
    (void)pkga_approve(&mk_at, mk_pk, text, len, flags, &r);
    v->verdict = r.verdict;
    v->programs = r.programs;
    v->program = r.program;
    snprintf(v->why, sizeof(v->why), "%s", r.why);
    return OK;
}

static status_t ctl_revoke(void *ctx, const char *name, const char *version, uint32_t revision,
                           uint32_t *lines)
{
    (void)ctx;
    return pkga_revoke(&mk_at, (struct pkg_str){ name, (uint32_t)strlen(name) },
                       (struct pkg_str){ version, (uint32_t)strlen(version) }, revision, lines);
}

void mk_env(struct pk_env *e)
{
    memset(e, 0, sizeof(*e));
    e->prefix = MK_PREFIX;
    memcpy(e->key, mk_pk, sizeof(mk_pk));
    e->have_key = true;
    e->ctl = (struct pk_ctl){ NULL, ctl_approve, ctl_revoke };
}

/* ---- packages --------------------------------------------------------------------------- */

static uint64_t up512(uint64_t n)
{
    return (n + PKG_BLOCK - 1) & ~(uint64_t)(PKG_BLOCK - 1);
}

#define VEC_SIZE 4100u

/* bin/vec of good.jpk: an ELF with a list note ("posix", "mount /data r"). */
static const uint8_t *vec_elf(void)
{
    const struct pkgv_file *v = pkgv_find("good.jpk");
    uint32_t len;
    if (!v || pkg_first_header(v->data, &len) != OK)
        return NULL;
    return v->data + PKG_BLOCK + up512(len) + PKG_BLOCK;
}

/* A program's bytes: bin/vec's with a field the loader ignores (e_flags)
 * set from the package, so that each version's programs hash differently. */
static uint8_t *program_bytes(const struct mkpkg *p, const char *path, size_t *n)
{
    const uint8_t *elf = vec_elf();
    uint8_t d[SHA256_BYTES];
    char seed[300];
    if (!elf)
        return NULL;
    snprintf(seed, sizeof(seed), "%s %s %u %s", p->name, p->version, p->revision, path);
    sha256(seed, strlen(seed), d);
    uint8_t *b = malloc(VEC_SIZE);
    if (!b)
        return NULL;
    memcpy(b, elf, VEC_SIZE);
    memcpy(b + 0x30, d, 4);
    *n = VEC_SIZE;
    return b;
}

static void sort_files(struct mkpkg *p)
{
    for (unsigned i = 1; i < p->nfiles; i++) {
        struct mk_file t = p->files[i];
        unsigned j = i;
        for (; j > 0 && strcmp(p->files[j - 1].path, t.path) > 0; j--)
            p->files[j] = p->files[j - 1];
        p->files[j] = t;
    }
}

#define BODY_CAP 8192u

/* The manifest's text, signed, into m (BODY_CAP bytes); its length. */
static size_t manifest_text(struct mkpkg *p, uint8_t tables[MK_FILES][512],
                            uint32_t tsize[MK_FILES], char *m)
{
    size_t n = 0;
    char hex[129];
    n += (size_t)snprintf(m + n, BODY_CAP - n, "jamos-pkg 1\nname %s\nversion %s\nrevision %u\n"
                          "abi 0123456789abcdef\nlicence MIT\nsummary a test package\n", p->name,
                          p->version, p->revision);
    if (p->runs)
        n += (size_t)snprintf(m + n, BODY_CAP - n, "runs %s\n", p->runs);
    for (unsigned i = 0; i < p->nfiles; i++) {
        uint8_t d[SHA256_BYTES];
        sha256(p->data[i], p->size[i], d);
        sha256_hex(d, hex);
        n += (size_t)snprintf(m + n, BODY_CAP - n, "file %s %s %lu %s\n", p->files[i].path,
                              p->files[i].mode == 0755 ? "755" : "644", (unsigned long)p->size[i],
                              hex);
    }
    for (unsigned i = 0; i < p->nfiles; i++)
        if (p->files[i].program)
            n += (size_t)snprintf(m + n, BODY_CAP - n, "program %s %s\n", p->files[i].path,
                                  p->list ? p->list : "posix\\x0amount /data r\\x0a");
    for (unsigned i = 0; i < p->nfiles; i++) {
        if (!p->files[i].program)
            continue;
        uint8_t d[SHA256_BYTES];
        sha256(tables[i], tsize[i], d);
        sha256_hex(d, hex);
        n += (size_t)snprintf(m + n, BODY_CAP - n, "table %s %u %s\n", p->files[i].path, tsize[i],
                              hex);
    }
    uint8_t sig[64];
    crypto_ed25519_sign(sig, mk_sk, (const uint8_t *)m, n);
    for (int i = 0; i < 64; i++)
        snprintf(hex + 2 * i, 3, "%02x", sig[i]);
    n += (size_t)snprintf(m + n, BODY_CAP - n, "signature %s\n", hex);
    return n;
}

/* The page tables of the programs (a table is 64 + 32 a page: 192 bytes for
 * 4100). */
static bool make_tables(struct mkpkg *p, uint8_t (*tables)[512], uint32_t *tsize)
{
    for (unsigned i = 0; i < p->nfiles; i++) {
        if (!p->files[i].program)
            continue;
        struct pkg_tabler t;
        tsize[i] = (uint32_t)pkg_table_size(p->size[i]);
        CHECK(tsize[i] <= 512);
        pkg_tabler_begin(&t, p->size[i], tables[i], NULL);
        CHECK(pkg_tabler_add(&t, p->data[i], p->size[i]) && pkg_tabler_end(&t));
    }
    return true;
}

/* One member into the archive at *at. */
static bool member(struct mkpkg *p, size_t *at, const char *path, unsigned mode, const void *d,
                   size_t n)
{
    CHECK_ST(pkg_header_make((struct pkg_str){ path, (uint32_t)strlen(path) }, mode, n,
                             p->jpk + *at), OK);
    *at += PKG_BLOCK;
    memcpy(p->jpk + *at, d, n);
    *at += (size_t)up512(n);
    return true;
}

bool mk_build(struct mkpkg *p)
{
    static uint8_t tables[MK_FILES][512];
    uint32_t tsize[MK_FILES] = { 0 };
    char *text = malloc(BODY_CAP);
    sort_files(p);
    CHECK(text);
    for (unsigned i = 0; i < p->nfiles; i++) {
        if (p->files[i].program) {
            p->data[i] = program_bytes(p, p->files[i].path, &p->size[i]);
        } else {
            p->size[i] = strlen(p->files[i].text);
            p->data[i] = malloc(p->size[i] + 1);
            if (p->data[i])
                memcpy(p->data[i], p->files[i].text, p->size[i]);
        }
        CHECK(p->data[i]);
    }
    CHECK(make_tables(p, tables, tsize));
    p->mlen = (uint32_t)manifest_text(p, tables, tsize, text);
    size_t total = PKG_BLOCK + up512(p->mlen) + PKG_END_BYTES;
    for (unsigned i = 0; i < p->nfiles; i++)
        total += PKG_BLOCK + up512(p->size[i]) +
                 (p->files[i].program ? PKG_BLOCK + up512(tsize[i]) : 0);
    p->jpk = calloc(1, total);
    CHECK(p->jpk);
    size_t at = 0;
    CHECK(member(p, &at, "+MANIFEST", 0644, text, p->mlen));
    for (unsigned i = 0; i < p->nfiles; i++)
        CHECK(member(p, &at, p->files[i].path, p->files[i].mode, p->data[i], p->size[i]));
    for (unsigned i = 0; i < p->nfiles; i++) {
        char tp[300];
        snprintf(tp, sizeof(tp), "+TABLES/%s", p->files[i].path);
        if (p->files[i].program)
            CHECK(member(p, &at, tp, 0644, tables[i], tsize[i]));
    }
    CHECK_EQ(at + PKG_END_BYTES, total);
    p->jlen = total;
    sha256(p->jpk, p->jlen, p->jsha);
    sha256(p->jpk + PKG_BLOCK, p->mlen, p->msha);
    free(text);
    return true;
}

void mk_free(struct mkpkg *p)
{
    free(p->jpk);
    for (unsigned i = 0; i < MK_FILES; i++)
        free(p->data[i]);
    memset(p->data, 0, sizeof(p->data));
    p->jpk = NULL;
}

/* ---- sources ---------------------------------------------------------------------------- */

static status_t src_run(void *ctx, pk_feed_fn feed, void *fctx)
{
    struct mk_src *s = ctx;
    size_t n = s->stop_at && s->stop_at < s->n ? s->stop_at : s->n;
    size_t chunk = s->chunk ? s->chunk : n ? n : 1;
    for (size_t at = 0; at < n;) {
        size_t k = n - at < chunk ? n - at : chunk;
        status_t st = feed(fctx, s->d + at, k);
        if (st == PK_FEED_END)
            return OK;
        if (st != OK)
            return st;
        at += k;
    }
    return OK;
}

void mk_source(struct mk_src *s, const uint8_t *d, size_t n, size_t chunk, struct pk_source *out)
{
    *s = (struct mk_src){ d, n, chunk, 0 };
    *out = (struct pk_source){ s, src_run };
}

void mk_job(struct pk_job *j, enum pkg_op op, const struct mkpkg *p, struct mk_src *s,
            struct pk_source *src)
{
    mk_source(s, p->jpk, p->jlen, 3000, src);
    *j = (struct pk_job){ .op = op, .name = p->name, .text = p->jpk + PKG_BLOCK, .len = p->mlen,
                          .src = src, .size = p->jlen, .sha = p->jsha };
}

/* ---- the disk's side -------------------------------------------------------------------- */

bool mk_exists(const char *path)
{
    return fs_stat(path, NULL, NULL, NULL) == OK;
}

const char *mk_text(const char *path)
{
    static char buf[16384];
    handle_t v;
    uint64_t size;
    buf[0] = '\0';
    if (file_read_vmo(path, sizeof(buf) - 1, &v, &size) != OK)
        return buf;
    if (jam_vmo_read(v, 0, buf, size) == OK)
        buf[size] = '\0';
    jam_handle_close(v);
    return buf;
}

const char *mk_allow(void)
{
    return mk_text(MK_ALLOW);
}

unsigned mk_allow_count(const char *needle)
{
    unsigned n = 0;
    for (const char *p = strstr(mk_allow(), needle); p; p = strstr(p + 1, needle))
        n++;
    return n;
}

static bool file_has_bytes(const struct mkpkg *p, unsigned i)
{
    char path[400];
    handle_t v;
    uint64_t size;
    snprintf(path, sizeof(path), "%s/%s", MK_PREFIX, p->files[i].path);
    if (file_read_vmo(path, 1u << 20, &v, &size) != OK)
        return false;
    uint8_t *b = malloc(size + 1);
    bool same = b && size == p->size[i] && jam_vmo_read(v, 0, b, size) == OK &&
                !memcmp(b, p->data[i], size);
    free(b);
    jam_handle_close(v);
    return same;
}

char mk_why[200];

static bool no(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static bool no(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(mk_why, sizeof(mk_why), fmt, ap);
    va_end(ap);
    return false;
}

static bool check_clean(void)
{
    struct fs_entry ent;
    for (uint32_t i = 0; fs_readdir(MK_PREFIX "/db", i, &ent) == OK; i++) {
        size_t n = strlen(ent.name);
        if (!strcmp(ent.name, "journal") || !strcmp(ent.name, "journal.new") ||
            (n > 4 && !strcmp(ent.name + n - 4, ".new") && strcmp(ent.name, "serial.new")))
            return no("left in db/: %s", ent.name);
    }
    return true;
}

static bool check_installed(const struct mkpkg *p)
{
    char path[400], needle[200], any[100];
    unsigned programs = 0;
    snprintf(path, sizeof(path), "%s/db/%s", MK_PREFIX, p->name);
    const char *db = mk_text(path);
    if (strlen(db) != p->mlen || memcmp(db, p->jpk + PKG_BLOCK, p->mlen))
        return no("db/%s is not its manifest", p->name);
    for (unsigned i = 0; i < p->nfiles; i++) {
        if (!file_has_bytes(p, i))
            return no("%s is not as packaged", p->files[i].path);
        snprintf(path, sizeof(path), "%s/%s.new", MK_PREFIX, p->files[i].path);
        if (mk_exists(path))
            return no("%s is still there", path);
        programs += p->files[i].program ? 1u : 0u;
    }
    snprintf(needle, sizeof(needle), "pkg=%s %s-r%u\n", p->name, p->version, p->revision);
    snprintf(any, sizeof(any), "pkg=%s ", p->name);
    if (mk_allow_count(needle) != programs)
        return no("approvals tagged %s: not %u", needle, programs);
    if (mk_allow_count(any) != programs)
        return no("approvals of %s: more than %u", p->name, programs);
    return check_clean();
}

static bool check_absent(const struct mkpkg *p)
{
    char path[400], any[100];
    snprintf(path, sizeof(path), "%s/db/%s", MK_PREFIX, p->name);
    if (mk_exists(path))
        return no("db/%s is there", p->name);
    for (unsigned i = 0; i < p->nfiles; i++) {
        snprintf(path, sizeof(path), "%s/%s", MK_PREFIX, p->files[i].path);
        if (mk_exists(path))
            return no("%s is there", path);
        snprintf(path, sizeof(path), "%s/%s.new", MK_PREFIX, p->files[i].path);
        if (mk_exists(path))
            return no("%s is there", path);
    }
    snprintf(any, sizeof(any), "pkg=%s ", p->name);
    if (mk_allow_count(any))
        return no("approvals of %s are there", p->name);
    return check_clean();
}

int mk_state(const struct mkpkg *p)
{
    char first[sizeof(mk_why)];
    if (check_installed(p))
        return 1;
    snprintf(first, sizeof(first), "%s", mk_why);
    if (check_absent(p))
        return 0;
    char second[sizeof(mk_why)];
    snprintf(second, sizeof(second), "%s", mk_why);
    snprintf(mk_why, sizeof(mk_why), "installed? %.90s / absent? %.90s", first, second);
    return -1;
}

bool mk_installed(const struct mkpkg *p)
{
    if (check_installed(p))
        return true;
    printf("utest: %s: %s is not installed: %s\n", utest_cur, p->name, mk_why);
    return false;
}

bool mk_absent(const struct mkpkg *p)
{
    if (check_absent(p))
        return true;
    printf("utest: %s: %s is not absent: %s\n", utest_cur, p->name, mk_why);
    return false;
}

bool mk_clean(void)
{
    if (check_clean())
        return true;
    printf("utest: %s: %s\n", utest_cur, mk_why);
    return false;
}
