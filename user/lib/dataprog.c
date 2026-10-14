/* Programs on /data as Jam OS checks them (<dataprog.h>): the page table,
 * the program's list read in pieces, the approvals and their tables, and
 * the refusals. Moved here from the shell (PX's sh_image.c and the table
 * read of sh_allow.c) so that init's /svc/exec (exec.c) seals a program by
 * exactly the shell's rules.
 *
 * The list is read without the whole file: the ELF header, the program
 * headers and each PT_NOTE segment are read through the caller's reader
 * and put together as a small ELF file of their own (the header, the
 * program headers with each note's offset moved to where its bytes now
 * are, then the notes), which wants_read parses by its usual rules. So a
 * list is accepted or refused exactly as wants_read would over the whole
 * file, and a verified program's list is read with vmo_read, a few pages,
 * wherever the linker put it. */
#include <dataprog.h>
#include <sha256.h>
#include <wants.h>

#define TABLE_CHUNK (64u << 10)   /* a table's hashes read at a time */

#define PT_NOTE   4
#define MAX_PHDRS 64   /* wants_read's and the loader's limit */

/* The ELF64 header and program header fields read here (wants.c and
 * spawn.c read the same layout). */
struct ehdr {
    uint8_t  ident[16];                  /* "\x7fELF", class 2: 64-bit */
    uint16_t type, machine;              /* (checked by the loader) */
    uint32_t version;                    /* (not checked) */
    uint64_t entry, phoff, shoff;        /* entry; program / section header offsets */
    uint32_t flags;                      /* (none on x86-64) */
    uint16_t ehsize, phentsize, phnum;   /* this header's size; a program header's; how many */
    uint16_t shentsize, shnum, shstrndx; /* section headers (not used) */
};

struct phdr {
    uint32_t type, flags;                /* PT_*; PF_* */
    uint64_t offset, vaddr, paddr;       /* in the file; in memory (not used) */
    uint64_t filesz, memsz;              /* bytes in the file; in memory (not used) */
    uint64_t align;                      /* (not used) */
};

_Static_assert(sizeof(struct ehdr) == 64, "ELF64 header");
_Static_assert(sizeof(struct phdr) == 56, "ELF64 program header");

/* ---- the page table ------------------------------------------------------------------ */

uint64_t dp_ptable_pages(uint64_t size)
{
    return size / PAGE_SIZE + (size % PAGE_SIZE ? 1 : 0);
}

void dp_ptable_head(uint64_t size, const uint8_t sha[SHA256_BYTES], struct dp_ptable_head *out)
{
    memset(out, 0, sizeof(*out));
    memcpy(out->magic, DP_PTABLE_MAGIC, sizeof(out->magic));
    out->version = DP_PTABLE_VERSION;
    out->page_size = (uint32_t)PAGE_SIZE;
    out->size = size;
    out->pages = dp_ptable_pages(size);
    memcpy(out->sha256, sha, SHA256_BYTES);
}

status_t dp_ptable_check(const struct dp_ptable_head *h, uint64_t file_bytes, uint64_t size,
                         const uint8_t sha[SHA256_BYTES])
{
    if (memcmp(h->magic, DP_PTABLE_MAGIC, sizeof(h->magic)) || h->version != DP_PTABLE_VERSION ||
        h->page_size != PAGE_SIZE || memcmp(h->sha256, sha, SHA256_BYTES))
        return ERR_INVALID_ARGS;
    /* Its own numbers agree with each other and with the file's length:
     * only then is it a table at all, of whatever size. */
    uint64_t hashes, total;
    if (h->pages != dp_ptable_pages(h->size) ||
        __builtin_mul_overflow(h->pages, (uint64_t)SHA256_BYTES, &hashes) ||
        __builtin_add_overflow(hashes, (uint64_t)sizeof(*h), &total) || total != file_bytes)
        return ERR_INVALID_ARGS;
    return h->size == size ? OK : ERR_OUT_OF_RANGE;
}

void dp_ptable_hash(const uint8_t *bytes, uint64_t size, uint8_t *out)
{
    uint64_t whole = size / PAGE_SIZE;
    for (uint64_t i = 0; i < whole; i++)
        sha256(bytes + i * PAGE_SIZE, PAGE_SIZE, out + i * SHA256_BYTES);
    if (size % PAGE_SIZE) {
        /* The last page zero-padded, hashed in pieces (no page-sized buffer:
         * any thread may call this). */
        static const uint8_t zeros[64];
        struct sha256 s;
        uint64_t part = size % PAGE_SIZE;
        sha256_init(&s);
        sha256_add(&s, bytes + whole * PAGE_SIZE, part);
        for (uint64_t pad = PAGE_SIZE - part; pad; ) {
            uint64_t n = pad < sizeof(zeros) ? pad : sizeof(zeros);
            sha256_add(&s, zeros, n);
            pad -= n;
        }
        sha256_done(&s, out + whole * SHA256_BYTES);
    }
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;   /* sha256_hex writes lower case only */
}

bool dp_hex_bytes(const char *hex, uint8_t out[SHA256_BYTES])
{
    for (unsigned i = 0; i < SHA256_BYTES; i++) {
        int hi = hex_digit(hex[2 * i]), lo = hi < 0 ? -1 : hex_digit(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return hex[2 * SHA256_BYTES] == '\0';
}

/* ---- the list ------------------------------------------------------------------------ */

/* The ELF header and program headers as wants_read checks them. */
static bool headers_ok(const struct ehdr *eh, uint64_t size)
{
    return !memcmp(eh->ident, "\x7f" "ELF", 4) && eh->ident[4] == 2 &&
           eh->phentsize == sizeof(struct phdr) && eh->phnum <= MAX_PHDRS &&
           eh->phoff <= size && eh->phnum * sizeof(struct phdr) <= size - eh->phoff;
}

/* The bytes the small ELF file needs for the notes of ph[0, n): false if
 * a note reaches past the file or they come to more than the limit. */
static bool notes_fit(const struct phdr *ph, unsigned n, uint64_t size, uint64_t *bytes)
{
    uint64_t sum = 0;
    for (unsigned i = 0; i < n; i++) {
        if (ph[i].type != PT_NOTE)
            continue;
        if (ph[i].offset > size || ph[i].filesz > size - ph[i].offset ||
            ph[i].filesz > DP_NOTES_MAX - sum)
            return false;
        sum += ph[i].filesz;
    }
    *bytes = sum;
    return true;
}

status_t dp_image_wants(dp_read_fn read, void *ctx, uint64_t size, struct wants *w)
{
    memset(w, 0, sizeof(*w));
    struct ehdr eh;
    struct phdr ph[MAX_PHDRS];
    if (size < sizeof(eh))
        return ERR_INVALID_ARGS;
    status_t st = read(ctx, 0, &eh, sizeof(eh));
    if (st == OK && !headers_ok(&eh, size))
        st = ERR_INVALID_ARGS;
    if (st == OK && eh.phnum)
        st = read(ctx, eh.phoff, ph, eh.phnum * sizeof(struct phdr));
    uint64_t notes = 0;
    if (st == OK && !notes_fit(ph, eh.phnum, size, &notes))
        st = ERR_INVALID_ARGS;
    if (st != OK)
        return st;
    /* The small file: the header, the program headers right after it,
     * then each note where its program header now says. */
    uint64_t at = sizeof(eh) + eh.phnum * sizeof(struct phdr);
    uint8_t *small = malloc(at + notes);
    if (!small)
        return ERR_NO_MEMORY;
    for (unsigned i = 0; i < eh.phnum && st == OK; i++) {
        if (ph[i].type != PT_NOTE)
            continue;
        if (ph[i].filesz)
            st = read(ctx, ph[i].offset, small + at, ph[i].filesz);
        ph[i].offset = at;
        at += ph[i].filesz;
    }
    if (st == OK) {
        eh.phoff = sizeof(eh);
        memcpy(small, &eh, sizeof(eh));
        memcpy(small + sizeof(eh), ph, eh.phnum * sizeof(struct phdr));
        st = wants_read(small, at, w);
    }
    free(small);
    return st;
}

/* dp_image_wants' reader over a verified VMO: vmo_read only. */
static status_t vmo_reader(void *ctx, uint64_t off, void *dst, uint64_t len)
{
    return jam_vmo_read(*(const handle_t *)ctx, off, dst, len);
}

status_t dp_wants_vmo(handle_t x, uint64_t size, struct wants *w)
{
    return dp_image_wants(vmo_reader, &x, size, w);
}

/* ---- approvals ----------------------------------------------------------------------- */

bool dp_on_data(const char *abs)
{
    return !strncmp(abs, "/data/", 6) && abs[6];
}

bool dp_allow_split(char *line, char **hash, char **path, char **text)
{
    char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
    if (line[0] == '#' || !t1 || !t2 || t1 - line != DP_HEX_LEN)
        return false;
    *t1 = *t2 = '\0';
    *hash = line;
    *path = t1 + 1;
    *text = t2 + 1;
    return true;
}

/* The approvals' text, NUL-terminated, into *out (the caller frees it). */
static status_t read_allow(char **out)
{
    struct jfile f;
    uint64_t size = 0;
    size_t got = 0;
    status_t st = file_open(DP_ALLOW_FILE, FS_READ, &f);
    if (st != OK)
        return st;
    st = file_stat(&f, &size, NULL);
    if (st == OK && size > DP_ALLOW_MAX)
        st = ERR_OUT_OF_RANGE;
    char *text = st == OK ? malloc(size + 1) : NULL;
    if (st == OK && !text)
        st = ERR_NO_MEMORY;
    if (st == OK)
        st = file_read(&f, 0, text, size, &got);
    file_close(&f);
    if (st != OK) {
        free(text);
        return st;
    }
    text[got] = '\0';   /* shorter than its stat said: what was there */
    *out = text;
    return OK;
}

status_t dp_allow_hash(const char *path, char hex[DP_HEX_LEN + 1])
{
    char *all = NULL;
    status_t st = read_allow(&all);
    if (st != OK)
        return st;
    st = ERR_NOT_FOUND;
    for (char *line = all; line && *line;) {
        char *nl = strchr(line, '\n'), *hash, *p, *text;
        if (nl)
            *nl = '\0';
        if (dp_allow_split(line, &hash, &p, &text) && !strcmp(p, path)) {
            memcpy(hex, hash, DP_HEX_LEN + 1);   /* the last line wins (allow writes one) */
            st = OK;
        }
        line = nl ? nl + 1 : NULL;
    }
    free(all);
    return st;
}

/* ---- tables -------------------------------------------------------------------------- */

/* The hashes of the open table f (pages of them, after its header) into
 * a new VMO, from its offset 0, as the kernel takes them. */
static status_t table_hashes(struct jfile *f, uint64_t pages, handle_t *out)
{
    uint64_t bytes = pages * SHA256_BYTES;
    handle_t t;
    uint8_t *chunk = malloc(TABLE_CHUNK);
    status_t st = chunk ? jam_vmo_create(bytes, 0, &t) : ERR_NO_MEMORY;
    if (st != OK) {
        free(chunk);
        return st;
    }
    for (uint64_t at = 0; at < bytes && st == OK; at += TABLE_CHUNK) {
        size_t len = bytes - at < TABLE_CHUNK ? (size_t)(bytes - at) : TABLE_CHUNK, got = 0;
        st = file_read(f, sizeof(struct dp_ptable_head) + at, chunk, len, &got);
        if (st == OK && got != len)
            st = ERR_INVALID_ARGS;   /* shorter than its stat said: broken */
        if (st == OK)
            st = jam_vmo_write(t, at, chunk, len);
    }
    free(chunk);
    if (st != OK) {
        jam_handle_close(t);
        return st;
    }
    *out = t;
    return OK;
}

status_t dp_table_read(const char *hash_hex, uint64_t size, handle_t *table)
{
    uint8_t sha[SHA256_BYTES];
    if (!dp_hex_bytes(hash_hex, sha))
        return ERR_INVALID_ARGS;
    if (!size || size > DP_PROGRAM_MAX)
        return ERR_OUT_OF_RANGE;   /* no approval was ever for such a file */
    char path[sizeof(DP_TABLE_DIR) + DP_HEX_LEN + 1];
    snprintf(path, sizeof(path), DP_TABLE_DIR "/%.*s", (int)DP_HEX_LEN, hash_hex);
    struct jfile f;
    status_t st = file_open(path, FS_READ, &f);
    if (st != OK)
        return st;   /* ERR_NOT_FOUND: an approval from before tables */
    struct dp_ptable_head h;
    uint64_t bytes = 0;
    size_t got = 0;
    st = file_stat(&f, &bytes, NULL);
    if (st == OK)
        st = file_read(&f, 0, &h, sizeof(h), &got);
    if (st == OK && got != sizeof(h))
        st = ERR_INVALID_ARGS;
    if (st == OK)
        st = dp_ptable_check(&h, bytes, size, sha);
    if (st == OK)
        st = table_hashes(&f, h.pages, table);
    file_close(&f);
    return st;
}

/* ---- what a program from /data is given ---------------------------------------------- */

/* Services no program from /data may have, whatever the owner says: with
 * volumes' channel a program gets the filesystems' own channels
 * (volumes.watch: /data unguarded, so it could approve itself); with
 * devmgr's control channel any driver's hardware; with its query channel
 * usb-bus, and through it any USB device's interfaces; with init's, every
 * service and the reboot. The build allows volumes', devmgr's control
 * channel and init's only in user/tests/ (tools/checkwants.py), and
 * a file on /data is never one of the tree's tests. Nor netstack's and the
 * resolver's reserves for the network's own services (net-sys, dns-sys): a
 * program could use them up. Nor the ports below 1024 (net-low):
 * they are where the well-known services live, so a program on them could
 * pose as one of the system's to the network (a web or a time server, the
 * ssh Jam OS will have), or take its port before the service starts; only
 * a boot-image service may have them (tools/checkwants.py). Nor
 * the file server's channel: with it a program could put any file it can
 * read on the network without the listen permission. Nor the root's debug
 * power: `debug_command` panics the machine, crashes it on purpose and runs
 * the stress test. Nor init's sessions (session): each is a whole shell, with
 * init's namespace and the shell's root powers, past every view and grant;
 * the build allows it only for a service, a port or a test
 * (tools/checkwants.py). */
static const char *const refused[] = { SVC_DEVMGR, SVC_DEVMGR_CTL, SVC_VOLUMES, SVC_INIT,
                                       SVC_NET_SYS, SVC_DNS_SYS, SVC_SERVE, SVC_NET_LISTEN_LOW,
                                       SVC_SESSION };

const char *dp_wants_refused(const struct wants *w)
{
    if (w->rights & WANT_RIGHT_DEBUG)
        return "right debug";
    for (unsigned i = 0; i < w->n; i++)
        for (unsigned k = 0; k < sizeof(refused) / sizeof(refused[0]); k++)
            if (!strncmp(w->grant[i], "/svc/", 5) && !strcmp(w->grant[i] + 5, refused[k]))
                return refused[k];
    return NULL;
}

rights_t dp_root_rights(uint32_t want)
{
    rights_t r = RIGHT_DUPLICATE | RIGHT_WAIT | RIGHT_INSPECT;
    r |= want & WANT_RIGHT_KLOG ? RIGHT_ROOT_KLOG : 0;
    r |= want & WANT_RIGHT_SYSINFO ? RIGHT_ROOT_SYSINFO : 0;
    r |= want & WANT_RIGHT_CLOCK ? RIGHT_ROOT_CLOCK : 0;
    r |= want & WANT_RIGHT_DEBUG ? RIGHT_ROOT_DEBUG : 0;
    return r;
}
