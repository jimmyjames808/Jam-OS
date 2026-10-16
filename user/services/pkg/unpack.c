/* A package as its bytes arrive (pkg.h). The archive is a ustar file whose
 * members, in order, are the signed manifest, each file the manifest names
 * and each program's page-hash table (<pkgfmt.h>); this reads it in pieces
 * of any size, from the network or a stick, and holds no more than a
 * 512-byte block, the manifest and one file's write buffer.
 *
 * The manifest is parsed first, with the signature checked, so nothing of
 * a package that the ports key did not sign is believed. Every later byte
 * is held to it by pkg_reader: each header is rebuilt from the manifest and
 * compared whole, each file's SHA-256 is checked as it ends, and a
 * program's table must be the one its own bytes make. When staging, each
 * file is written at `<path>.new` (the prefix is bin/pkg's alone to write)
 * once its hash has been checked, synced, and read back from the disk
 * against the manifest's hash again.
 *
 * Besides the format, the manifest must keep to what the database needs: a
 * package's files may not be under db/ or end in ".new" (the staging name),
 * and its name may not be one the database keeps. */
#include <sha256.h>
#include "pkg.h"

#define GATHER (64u * 1024)   /* bytes held before a file write */

enum ustate { U_FIRST, U_MANIFEST, U_MPAD, U_HDR, U_DATA, U_PAD, U_TAIL };

struct stage {                /* the file being written */
    struct jfile f;
    bool         open;
    char         path[PK_PATH];
    uint8_t     *buf;
    size_t       held;
    uint64_t     at;          /* the file offset of buf[0] */
};

struct pk_unpack {
    struct pk_unpack_use use;
    enum ustate          st;
    uint8_t              blk[PKG_BLOCK];   /* a header, or padding, being collected */
    uint32_t             fill;
    uint8_t             *mtext;
    uint32_t             mlen, mgot;
    struct pkg_manifest *m;
    uint8_t             *tables;
    struct pkg_reader    rd;
    struct pkg_member    mem;
    uint64_t             left;             /* bytes of the member's data to come */
    uint8_t              tail[PKG_END_BYTES + 1];
    uint32_t             tailn;
    struct sha256        whole;
    uint64_t             total;
    struct stage         sg;
    char                 dir[PK_PATH];     /* the last directory made */
    char                 why[160];
};

struct pk_unpack *pk_unpack_new(const struct pk_unpack_use *use)
{
    struct pk_unpack *u = calloc(1, sizeof(*u));
    if (!u)
        return NULL;
    u->use = *use;
    u->m = calloc(1, sizeof(*u->m));
    if (!u->m) {
        free(u);
        return NULL;
    }
    sha256_init(&u->whole);
    return u;
}

static void stage_abandon(struct stage *sg)
{
    if (sg->open)
        file_close(&sg->f);
    sg->open = false;
    free(sg->buf);
    sg->buf = NULL;
}

void pk_unpack_free(struct pk_unpack *u)
{
    if (!u)
        return;
    stage_abandon(&u->sg);
    free(u->mtext);
    free(u->m);
    free(u->tables);
    free(u);
}

const struct pkg_manifest *pk_unpack_manifest(const struct pk_unpack *u)
{
    return u->mtext && u->mgot == u->mlen && u->st > U_MANIFEST ? u->m : NULL;
}

const uint8_t *pk_unpack_text(const struct pk_unpack *u, uint32_t *len)
{
    *len = u->mlen;
    return u->mtext;
}

void pk_unpack_take(struct pk_unpack *u, struct pkg_manifest **m, uint8_t **text, uint32_t *len)
{
    *m = u->m;
    *text = u->mtext;
    *len = u->mlen;
    u->m = NULL;
    u->mtext = NULL;
}

const char *pk_unpack_why(const struct pk_unpack *u)
{
    return u->why;
}

static status_t refuse(struct pk_unpack *u, status_t st, const char *why)
{
    snprintf(u->why, sizeof(u->why), "%s", why);
    return st;
}

/* ---- the manifest ---------------------------------------------------------------------- */

/* Is the first part of path (up to the first '/') "db", as FAT reads it? */
static bool under_db(struct pkg_str path)
{
    uint32_t n = 0;
    while (n < path.n && path.p[n] != '/')
        n++;
    return n == 2 && (path.p[0] | 0x20) == 'd' && (path.p[1] | 0x20) == 'b';
}

/* What the format allows but the database can't hold; NULL if it is fine. */
const char *pk_manifest_problem(const struct pkg_manifest *m)
{
    if (pk_name_reserved(m->name))
        return "its name is one pkg keeps for itself";
    for (uint32_t i = 0; i < m->nfiles; i++) {
        struct pkg_str p = m->files[i].path;
        if (under_db(p))
            return "a file of it is under db/, where pkg keeps its records";
        if (pk_ends_fold(p, ".new"))
            return "a file of it ends in .new, the name files are staged under";
    }
    return NULL;
}

static status_t manifest_in(struct pk_unpack *u)
{
    struct pk_env *e = u->use.e;
    const char *why = NULL;
    if (!e->have_key)
        return refuse(u, ERR_ACCESS_DENIED, "this build has no ports key");
    status_t st = pkg_manifest_parse(u->mtext, u->mlen, e->key, u->m, &why);
    if (st != OK)
        return refuse(u, st, why ? why : "the manifest is not a valid one");
    if (e->abi[0] && !pkg_abi_is(u->m, e->abi))
        return refuse(u, ERR_ACCESS_DENIED, "it is built for another version of Jam OS");
    why = pk_manifest_problem(u->m);
    if (why)
        return refuse(u, ERR_ACCESS_DENIED, why);
    if (u->use.msha && memcmp(u->use.msha, u->m->sha256, SHA256_BYTES))
        return refuse(u, ERR_ACCESS_DENIED, "it is not the package that was shown");
    if (!u->use.stage)
        return PK_FEED_END;
    uint64_t cap = pkg_tables_bytes(u->m);
    u->tables = malloc(cap + 1);
    if (!u->tables)
        return refuse(u, ERR_NO_MEMORY, "out of memory");
    return pkg_reader_init(&u->rd, u->m, u->tables, (size_t)cap);
}

/* ---- staging a file --------------------------------------------------------------------- */

/* Directories above path made, once per directory (files come in order). */
static status_t stage_dirs(struct pk_unpack *u, const char *path)
{
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path) : 0;
    if (strlen(u->dir) == n && !memcmp(u->dir, path, n))
        return OK;
    status_t st = pk_mkdirs(u->use.e, path);
    if (st == OK)
        snprintf(u->dir, sizeof(u->dir), "%.*s", (int)n, path);
    return st;
}

static status_t stage_open(struct pk_unpack *u, const struct pkg_file *f)
{
    struct pk_env *e = u->use.e;
    struct stage *sg = &u->sg;
    pk_path(e, f->path, ".new", sg->path);
    status_t st = stage_dirs(u, sg->path);
    if (st != OK)
        return st;
    if (pk_cut(e, "create", false) != PK_GO)
        return ERR_IO;
    st = file_open_mode(sg->path, FS_WRITE | FS_CREATE | FS_TRUNCATE | FS_GATHER, f->mode,
                        &sg->f);
    if (st != OK)
        return st;
    sg->open = true;
    sg->buf = malloc(GATHER);
    sg->held = 0;
    sg->at = 0;
    return sg->buf ? OK : ERR_NO_MEMORY;
}

/* The held bytes written (a change to the prefix: it asks pk_cut). */
static status_t stage_flush(struct pk_unpack *u)
{
    struct stage *sg = &u->sg;
    if (!sg->held)
        return OK;
    int c = pk_cut(u->use.e, "write", true);
    if (c == PK_DIE)
        return ERR_IO;
    size_t done = 0, m = c == PK_TEAR ? sg->held / 2 : sg->held;
    status_t st = m ? file_write(&sg->f, sg->at, sg->buf, m, &done) : OK;
    if (st == OK && done != m)
        st = ERR_IO;
    sg->at += m;
    sg->held = 0;
    return c == PK_TEAR ? ERR_IO : st;
}

static status_t stage_write(struct pk_unpack *u, const uint8_t *p, size_t n)
{
    struct stage *sg = &u->sg;
    while (n) {   /* each turn fills the buffer or writes it */
        size_t k = GATHER - sg->held < n ? GATHER - sg->held : n;
        memcpy(sg->buf + sg->held, p, k);
        sg->held += k;
        p += k;
        n -= k;
        if (sg->held == GATHER) {
            status_t st = stage_flush(u);
            if (st != OK)
                return st;
        }
    }
    return OK;
}

/* The file whole: written, synced, closed, and read back against the
 * manifest's size and hash. */
static status_t stage_commit(struct pk_unpack *u, const struct pkg_file *f)
{
    struct stage *sg = &u->sg;
    status_t st = stage_flush(u);
    if (st == OK)
        st = file_sync(&sg->f);
    stage_abandon(sg);
    if (st != OK)
        return st;
    uint64_t size = 0;
    uint8_t sha[SHA256_BYTES];
    st = pk_hash_file(sg->path, &size, sha);
    if (st == OK && (size != f->size || memcmp(sha, f->sha256, SHA256_BYTES)))
        st = refuse(u, ERR_IO, "a file read back from the disk is not what was written");
    return st;
}

/* ---- the members ------------------------------------------------------------------------ */

/* Bytes of the block being collected, up to want in all: how many of the n
 * given were used, and whether the block is whole. */
static bool collect(struct pk_unpack *u, const uint8_t *p, size_t n, uint32_t want,
                    size_t *used)
{
    size_t k = want - u->fill < n ? want - u->fill : n;
    memcpy(u->blk + u->fill, p, k);
    u->fill += (uint32_t)k;
    *used = k;
    if (u->fill < want)
        return false;
    u->fill = 0;
    return true;
}

static bool all_zero(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return false;
    return true;
}

/* A member's header is in blk. */
static status_t member_begin(struct pk_unpack *u)
{
    struct pk_env *e = u->use.e;
    if (e->stopped && e->stopped(e->stop_ctx))
        return ERR_CANCELED;
    status_t st = pkg_reader_next(&u->rd, u->blk, &u->mem);
    if (st != OK)
        return refuse(u, st, "a header of the package is not the one its manifest promises");
    u->left = u->mem.size;
    if (!u->mem.table)
        st = stage_open(u, &u->m->files[u->mem.index]);
    if (st != OK)
        return st;
    u->st = u->left ? U_DATA : U_PAD;
    return OK;
}

/* A member's padding is in blk (u->mem.padding bytes). */
static status_t member_end(struct pk_unpack *u)
{
    status_t st = pkg_reader_done(&u->rd, u->blk, u->mem.padding);
    if (st != OK)
        return refuse(u, st, st == ERR_ACCESS_DENIED ? "a file of the package is not the one its "
                      "manifest names" : "a file of the package is cut short or badly padded");
    if (!u->mem.table)
        st = stage_commit(u, &u->m->files[u->mem.index]);
    u->st = U_HDR;
    return st;
}

static status_t take_data(struct pk_unpack *u, const uint8_t *p, size_t n, size_t *used)
{
    size_t k = u->left < n ? (size_t)u->left : n;
    status_t st = pkg_reader_data(&u->rd, p, k);
    if (st != OK)
        return refuse(u, st, st == ERR_ACCESS_DENIED ? "a program's page table is not the one its "
                      "bytes make" : "a file of the package is longer than its manifest says");
    if (!u->mem.table)
        st = stage_write(u, p, k);
    *used = k;
    u->left -= k;
    if (st == OK && !u->left)
        u->st = U_PAD;
    return st;
}

/* One step of the state machine over p[0..n): *used bytes taken. */
static status_t step(struct pk_unpack *u, const uint8_t *p, size_t n, size_t *used)
{
    *used = 0;
    switch (u->st) {
    case U_FIRST:
        if (!collect(u, p, n, PKG_BLOCK, used))
            return OK;
        if (pkg_first_header(u->blk, &u->mlen) != OK)
            return refuse(u, ERR_INVALID_ARGS, "the package does not start with its manifest");
        u->mtext = malloc(u->mlen + 1);
        u->st = U_MANIFEST;
        return u->mtext ? OK : ERR_NO_MEMORY;
    case U_MANIFEST: {
        size_t k = u->mlen - u->mgot < n ? u->mlen - u->mgot : n;
        memcpy(u->mtext + u->mgot, p, k);
        u->mgot += (uint32_t)k;
        *used = k;
        if (u->mgot < u->mlen)
            return OK;
        u->mtext[u->mlen] = '\0';
        u->st = (u->mlen % PKG_BLOCK) ? U_MPAD : U_HDR;
        return manifest_in(u);
    }
    case U_MPAD: {
        uint32_t want = PKG_BLOCK - u->mlen % PKG_BLOCK;
        if (!collect(u, p, n, want, used))
            return OK;
        u->st = U_HDR;
        return all_zero(u->blk, want) ? OK : refuse(u, ERR_INVALID_ARGS, "bad padding");
    }
    case U_HDR:
        if (u->rd.next == u->m->nfiles + u->m->nprograms) {
            u->st = U_TAIL;
            return OK;
        }
        return collect(u, p, n, PKG_BLOCK, used) ? member_begin(u) : OK;
    case U_DATA:
        return take_data(u, p, n, used);
    case U_PAD:
        return collect(u, p, n, u->mem.padding, used) || !u->mem.padding ? member_end(u) : OK;
    case U_TAIL: {
        size_t k = sizeof(u->tail) - u->tailn < n ? sizeof(u->tail) - u->tailn : n;
        memcpy(u->tail + u->tailn, p, k);
        u->tailn += (uint32_t)k;
        *used = n;   /* anything past the cap is refused at the end */
        if (n > k)
            u->tailn = sizeof(u->tail);
        return OK;
    }
    }
    return ERR_INTERNAL;
}

status_t pk_unpack_feed(struct pk_unpack *u, const void *data, size_t n)
{
    const uint8_t *p = data;
    sha256_add(&u->whole, data, n);
    u->total += n;
    while (n) {   /* each step takes a byte at least, or changes the state */
        size_t used;
        status_t st = step(u, p, n, &used);
        if (st != OK) {
            stage_abandon(&u->sg);
            return st;
        }
        p += used;
        n -= used;
    }
    return OK;
}

status_t pk_unpack_sink(void *u, const void *data, size_t n)
{
    return pk_unpack_feed(u, data, n);
}

status_t pk_unpack_finish(struct pk_unpack *u, uint64_t *size, uint8_t sha[SHA256_BYTES])
{
    if (u->st == U_HDR && u->rd.next == u->m->nfiles + u->m->nprograms)
        u->st = U_TAIL;
    if (u->st != U_TAIL)
        return refuse(u, ERR_INVALID_ARGS, "the package ends before its last file");
    status_t st = pkg_reader_finish(&u->rd, u->tail, u->tailn);
    if (st != OK)
        return refuse(u, st, "the package does not end where its manifest says");
    sha256_done(&u->whole, sha);
    *size = u->total;
    return OK;
}

/* ---- the head ---------------------------------------------------------------------------- */

status_t pk_head(struct pk_env *e, const struct pk_source *src, struct pkg_manifest **m,
                 uint8_t **text, uint32_t *len, char why[160])
{
    struct pk_unpack_use use = { e, false, NULL };
    struct pk_unpack *u = pk_unpack_new(&use);
    if (!u)
        return ERR_NO_MEMORY;
    status_t st = src->run(src->ctx, pk_unpack_sink, u);
    if (st == OK && !pk_unpack_manifest(u))
        st = refuse(u, ERR_INVALID_ARGS, "the package ends before its manifest does");
    snprintf(why, 160, "%s", pk_unpack_why(u));
    if (st == OK)
        pk_unpack_take(u, m, text, len);
    pk_unpack_free(u);
    return st;
}
