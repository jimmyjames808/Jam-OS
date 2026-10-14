/* A package's archive (<pkgfmt.h>, the plan's section 6): a ustar file
 * written in exactly one form, so a reader does not parse headers, it
 * rebuilds each one from the (path, mode, size) the signed manifest
 * promises and compares all 512 bytes. Whatever another writer would
 * vary (owners, times, pax or GNU headers, links, directories, devices,
 * the checksum's spellings) is refused by that one comparison; a path
 * with `..` or an absolute path cannot be in the manifest, so cannot be
 * in the archive. The programs' page-hash tables (the format of
 * /data/etc/allow.d, <dataprog.h>) are computed from the bytes just read
 * and compared with the shipped ones. Two readers share the member rules:
 * the whole package in memory, and the stream (pkg_reader) for packages
 * that do not fit. */
#include "pkgint.h"

#define MAGIC          "JAMPAGES"
#define TABLE_VERSION  1u
#define NAME_MAX_BYTES 100u
#define PREFIX_MAX     155u

uint64_t pkg_table_size(uint64_t size)
{
    return PKG_TABLE_HEAD + 32ull * ((size + PKG_PAGE - 1) / PKG_PAGE);
}

uint64_t pkg_tables_bytes(const struct pkg_manifest *m)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < m->nprograms; i++)
        sum += m->programs[i].table_size;
    return sum;
}

bool pkg_abi_is(const struct pkg_manifest *m, const char *abi)
{
    return abi && strlen(abi) == 16 && !memcmp(m->abi, abi, 16);
}

/* ---- the header ------------------------------------------------------------------------- */

/* digits octal digits of v, then a NUL, at dst. */
static void put_octal(uint8_t *dst, unsigned digits, uint64_t v)
{
    for (unsigned i = digits; i--;) {
        dst[i] = (uint8_t)('0' + (v & 7));
        v >>= 3;
    }
    dst[digits] = 0;
}

status_t pkg_header_make(struct pkg_str path, unsigned mode, uint64_t size,
                         uint8_t out[PKG_BLOCK])
{
    if ((mode != 0644 && mode != 0755) || size >= (1ull << 33) || path.n == 0)
        return ERR_INVALID_ARGS;
    for (uint32_t i = 0; i < path.n; i++)
        if ((uint8_t)path.p[i] <= 0x20 || (uint8_t)path.p[i] > 0x7e)
            return ERR_INVALID_ARGS;
    uint32_t name_at = 0, prefix_n = 0;
    if (path.n > NAME_MAX_BYTES) {
        /* split at the last '/' within the first 156 bytes */
        uint32_t at = path.n - 1 < PREFIX_MAX ? path.n - 1 : PREFIX_MAX;
        while (at > 0 && path.p[at] != '/')
            at--;
        if (at == 0 || path.p[at] != '/' || path.n - at - 1 > NAME_MAX_BYTES || at == path.n - 1)
            return ERR_INVALID_ARGS;
        name_at = at + 1;
        prefix_n = at;
    }
    memset(out, 0, PKG_BLOCK);
    memcpy(out, path.p + name_at, path.n - name_at);
    put_octal(out + 100, 7, mode);
    put_octal(out + 108, 7, 0);
    put_octal(out + 116, 7, 0);
    put_octal(out + 124, 11, size);
    put_octal(out + 136, 11, 0);
    memset(out + 148, ' ', 8);
    out[156] = '0';
    memcpy(out + 257, "ustar\0" "00", 8);
    put_octal(out + 329, 7, 0);
    put_octal(out + 337, 7, 0);
    memcpy(out + 345, path.p, prefix_n);
    uint32_t sum = 0;
    for (unsigned i = 0; i < PKG_BLOCK; i++)
        sum += out[i];
    put_octal(out + 148, 6, sum);
    out[155] = ' ';
    return OK;
}

status_t pkg_first_header(const uint8_t h[PKG_BLOCK], uint32_t *manifest_size)
{
    uint64_t size = 0;
    uint8_t want[PKG_BLOCK];
    for (unsigned i = 0; i < 11; i++) {
        if (h[124 + i] < '0' || h[124 + i] > '7')
            return ERR_INVALID_ARGS;
        size = size << 3 | (uint64_t)(h[124 + i] - '0');
    }
    if (size < 1 || size > PKG_MANIFEST_MAX)
        return ERR_OUT_OF_RANGE;
    status_t st = pkg_header_make((struct pkg_str){ "+MANIFEST", 9 }, 0644, size, want);
    if (st != OK || memcmp(want, h, PKG_BLOCK))
        return ERR_INVALID_ARGS;
    *manifest_size = (uint32_t)size;
    return OK;
}

/* ---- the page-hash table ------------------------------------------------------------------ */

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void put64(uint8_t *p, uint64_t v)
{
    for (unsigned i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

void pkg_tabler_begin(struct pkg_tabler *t, uint64_t size, uint8_t *out, const uint8_t *expect)
{
    sha256_init(&t->whole);
    sha256_init(&t->page);
    t->size = size;
    t->seen = 0;
    t->pages_done = 0;
    t->fill = 0;
    t->out = out;
    t->expect = expect;
    t->ok = true;
}

/* Put (or compare) the n bytes of the table at off. */
static void table_bytes(struct pkg_tabler *t, uint64_t off, const uint8_t *b, size_t n)
{
    if (t->out)
        memcpy(t->out + off, b, n);
    else if (memcmp(t->expect + off, b, n))
        t->ok = false;
}

/* The page being filled is whole (or the last, to be zero-padded): its hash. */
static void page_done(struct pkg_tabler *t)
{
    static const uint8_t zeros[64];
    uint8_t h[SHA256_BYTES];
    while (t->fill < PKG_PAGE) {
        uint32_t take = PKG_PAGE - t->fill < sizeof(zeros) ? PKG_PAGE - t->fill : sizeof(zeros);
        sha256_add(&t->page, zeros, take);
        t->fill += take;
    }
    sha256_done(&t->page, h);
    table_bytes(t, PKG_TABLE_HEAD + (uint64_t)SHA256_BYTES * t->pages_done, h, sizeof(h));
    t->pages_done++;
    sha256_init(&t->page);
    t->fill = 0;
}

bool pkg_tabler_add(struct pkg_tabler *t, const void *data, size_t n)
{
    const uint8_t *p = data;
    if (n > t->size - t->seen)
        return false;
    while (n) {
        size_t take = PKG_PAGE - t->fill < n ? PKG_PAGE - t->fill : n;
        sha256_add(&t->whole, p, take);
        sha256_add(&t->page, p, take);
        t->fill += (uint32_t)take;
        t->seen += take;
        p += take;
        n -= take;
        if (t->fill == PKG_PAGE)
            page_done(t);
    }
    return true;
}

bool pkg_tabler_end(struct pkg_tabler *t)
{
    uint8_t head[PKG_TABLE_HEAD];
    if (t->seen != t->size)
        return false;
    if (t->fill)
        page_done(t);
    memset(head, 0, sizeof(head));
    memcpy(head, MAGIC, 8);
    put32(head + 8, TABLE_VERSION);
    put32(head + 12, PKG_PAGE);
    put64(head + 16, t->size);
    put64(head + 24, t->pages_done);
    sha256_done(&t->whole, head + 32);
    table_bytes(t, 0, head, sizeof(head));
    return t->ok;
}

/* ---- the members ----------------------------------------------------------------------- */

/* Member idx of the manifest's list (the files, then each program's
 * table): its canonical header into h, its size, and what it is. */
struct expect {
    uint64_t size;
    bool     table;
    uint32_t index;      /* the file's, or the program's for a table */
};

static status_t member_header(const struct pkg_manifest *m, uint32_t idx, uint8_t h[PKG_BLOCK],
                              struct expect *e)
{
    if (idx < m->nfiles) {
        const struct pkg_file *f = &m->files[idx];
        *e = (struct expect){ f->size, false, idx };
        return pkg_header_make(f->path, f->mode, f->size, h);
    }
    uint32_t i = idx - m->nfiles;
    if (i >= m->nprograms)
        return ERR_BAD_STATE;
    const struct pkg_program *p = &m->programs[i];
    char path[8 + PKG_PATH_MAX];
    memcpy(path, "+TABLES/", 8);
    memcpy(path + 8, p->path.p, p->path.n);
    *e = (struct expect){ p->table_size, true, i };
    return pkg_header_make((struct pkg_str){ path, 8 + p->path.n }, 0644, p->table_size, h);
}

static uint32_t padding_of(uint64_t size)
{
    return (uint32_t)((PKG_BLOCK - size % PKG_BLOCK) % PKG_BLOCK);
}

static bool all_zero(const uint8_t *p, size_t n)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++)
        acc |= p[i];
    return acc == 0;
}

/* ---- the whole package in memory ----------------------------------------------------------- */

/* The archive from `at` to the end is exactly the two end blocks. */
static bool ends_right(const uint8_t *d, size_t len, size_t at)
{
    return len >= at && len - at == PKG_END_BYTES && all_zero(d + at, PKG_END_BYTES);
}

/* One member at *at: header, bytes, padding; the hashes. progs[] is where
 * each program's file bytes are. */
static status_t whole_member(const struct pkg_manifest *m, uint32_t idx, const uint8_t *d,
                             size_t len, size_t *at, size_t *progs, const char **why)
{
    uint8_t want[PKG_BLOCK], sum[SHA256_BYTES];
    struct expect e;
    NEED(member_header(m, idx, want, &e) == OK, "the manifest names a member no header can");
    NEED(len - *at >= PKG_BLOCK && !memcmp(d + *at, want, PKG_BLOCK),
         "a member's header is not the one the manifest promises");
    *at += PKG_BLOCK;
    uint32_t pad = padding_of(e.size);
    NEED(len - *at >= e.size && len - *at - e.size >= pad, "the archive ends inside a member");
    const uint8_t *body = d + *at;
    NEED(all_zero(body + e.size, pad), "a member's padding is not NUL");
    sha256(body, e.size, sum);
    if (!e.table) {
        const struct pkg_file *f = &m->files[e.index];
        if (memcmp(sum, f->sha256, SHA256_BYTES))
            return pkg_fail(why, ERR_ACCESS_DENIED, "a file is not the bytes signed");
        if (f->program != PKG_NO_PROGRAM)
            progs[f->program] = *at;
    } else {
        const struct pkg_program *p = &m->programs[e.index];
        const struct pkg_file *f = &m->files[p->file];
        struct pkg_tabler t;
        if (memcmp(sum, p->table_sha256, SHA256_BYTES))
            return pkg_fail(why, ERR_ACCESS_DENIED, "a table is not the bytes signed");
        pkg_tabler_begin(&t, f->size, NULL, body);
        if (!pkg_tabler_add(&t, d + progs[e.index], f->size) || !pkg_tabler_end(&t))
            return pkg_fail(why, ERR_ACCESS_DENIED, "a table is not its program's own");
    }
    *at += e.size + pad;
    return OK;
}

static status_t program_lists(const struct pkg_manifest *m, const uint8_t *d, const size_t *progs,
                              const char **why)
{
    for (unsigned i = 0; i < m->nprograms; i++) {
        uint8_t list[PKG_LIST_MAX];
        size_t n;
        const struct pkg_program *p = &m->programs[i];
        NEED(pkg_list_decode(p->list, list, &n) == OK, "a program's list");
        if (!pkg_elf_list_is(d + progs[i], m->files[p->file].size, list, n))
            return pkg_fail(why, ERR_ACCESS_DENIED, "a program's ELF note is not its list");
    }
    return OK;
}

status_t pkg_package_verify(const void *data, size_t len, const uint8_t key[PKG_KEY_BYTES],
                            const char *abi, struct pkg_manifest *m, const char **why)
{
    const uint8_t *d = data;
    uint32_t n;
    size_t progs[PKG_PROGRAMS_MAX], at;
    if (!d || len > PKG_ARCHIVE_MAX || len < PKG_BLOCK)
        return pkg_fail(why, ERR_OUT_OF_RANGE, "not 512 bytes to 1 GiB");
    NEED(pkg_first_header(d, &n) == OK, "the first member is not a canonical +MANIFEST header");
    NEED(len - PKG_BLOCK >= n, "the archive ends inside +MANIFEST");
    status_t st = pkg_manifest_parse(d + PKG_BLOCK, n, key, m, why);
    if (st != OK)
        return st;
    if (abi && !pkg_abi_is(m, abi))
        return pkg_fail(why, ERR_NOT_SUPPORTED, "built for another ABI than this system's");
    at = PKG_BLOCK + n;
    uint32_t pad = padding_of(n);
    NEED(len - at >= pad && all_zero(d + at, pad), "+MANIFEST's padding is not NUL");
    at += pad;
    for (uint32_t i = 0; i < m->nfiles + m->nprograms; i++)
        if ((st = whole_member(m, i, d, len, &at, progs, why)) != OK)
            return st;
    NEED(ends_right(d, len, at), "the archive does not end with its two end blocks");
    return program_lists(m, d, progs, why);
}

/* ---- the stream ---------------------------------------------------------------------------- */

status_t pkg_reader_init(struct pkg_reader *r, const struct pkg_manifest *m, uint8_t *tables,
                         size_t tables_cap)
{
    if (tables_cap < pkg_tables_bytes(m))
        return ERR_BUFFER_TOO_SMALL;
    r->m = m;
    r->tables = tables;
    r->next = 0;
    r->open = false;
    r->size = r->got = r->table_at = 0;
    return OK;
}

/* Where program i's table is in the reader's memory. */
static uint64_t table_offset(const struct pkg_manifest *m, uint32_t i)
{
    uint64_t at = 0;
    for (uint32_t k = 0; k < i; k++)
        at += m->programs[k].table_size;
    return at;
}

status_t pkg_reader_next(struct pkg_reader *r, const uint8_t h[PKG_BLOCK], struct pkg_member *out)
{
    uint8_t want[PKG_BLOCK];
    struct expect e;
    const struct pkg_manifest *m = r->m;
    if (r->open || r->next >= m->nfiles + m->nprograms)
        return ERR_BAD_STATE;
    if (member_header(m, r->next, want, &e) != OK || memcmp(want, h, PKG_BLOCK))
        return ERR_INVALID_ARGS;
    r->open = true;
    r->size = e.size;
    r->got = 0;
    sha256_init(&r->sum);
    if (e.table) {
        r->table_at = table_offset(m, e.index);
    } else if (m->files[e.index].program != PKG_NO_PROGRAM) {
        r->table_at = table_offset(m, m->files[e.index].program);
        pkg_tabler_begin(&r->tab, e.size, r->tables + r->table_at, NULL);
    }
    out->index = e.index;
    out->table = e.table;
    out->size = e.size;
    out->padding = padding_of(e.size);
    return OK;
}

status_t pkg_reader_data(struct pkg_reader *r, const void *data, size_t n)
{
    const struct pkg_manifest *m = r->m;
    if (!r->open)
        return ERR_BAD_STATE;
    if (n > r->size - r->got)
        return ERR_OUT_OF_RANGE;
    sha256_add(&r->sum, data, n);
    if (r->next >= m->nfiles) {   /* a table: it must be what the program's file made */
        if (memcmp(r->tables + r->table_at + r->got, data, n))
            return ERR_ACCESS_DENIED;
    } else if (m->files[r->next].program != PKG_NO_PROGRAM) {
        pkg_tabler_add(&r->tab, data, n);   /* within the size, checked above */
    }
    r->got += n;
    return OK;
}

status_t pkg_reader_done(struct pkg_reader *r, const void *padding, size_t n)
{
    const struct pkg_manifest *m = r->m;
    uint8_t sum[SHA256_BYTES];
    if (!r->open)
        return ERR_BAD_STATE;
    if (r->got != r->size || n != padding_of(r->size) || !all_zero(padding, n))
        return ERR_INVALID_ARGS;
    sha256_done(&r->sum, sum);
    bool table = r->next >= m->nfiles;
    const uint8_t *want = table ? m->programs[r->next - m->nfiles].table_sha256
                                : m->files[r->next].sha256;
    if (memcmp(sum, want, SHA256_BYTES))
        return ERR_ACCESS_DENIED;
    if (!table && m->files[r->next].program != PKG_NO_PROGRAM && !pkg_tabler_end(&r->tab))
        return ERR_INVALID_ARGS;
    r->open = false;
    r->next++;
    return OK;
}

status_t pkg_reader_finish(struct pkg_reader *r, const void *tail, size_t n)
{
    if (r->open || r->next != r->m->nfiles + r->m->nprograms)
        return ERR_BAD_STATE;
    return n == PKG_END_BYTES && all_zero(tail, n) ? OK : ERR_INVALID_ARGS;
}
