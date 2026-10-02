/* init's half of `update` (<update.h>, docs/M9-PLAN.md "update: a new
 * build from the Mac"): a fetched build checked against its manifest and
 * made the kernel's stored copy, the one `reboot` and a panic start.
 *
 * The fetcher parses what the network sent and holds no power; init holds
 * the power (kexec_load, RIGHT_ROOT_KEXEC) and parses nothing from the
 * network but the manifest, with the strict parser. An offer arrives on a
 * channel init made (initctl.update_offer), as one struct update_offer
 * with the kernel's and the boot image's VMOs. init copies each file into
 * a VMO only it holds, hashing exactly the bytes it writes, so nothing the
 * sender does to its own VMOs afterwards changes what was checked; then
 * every length and SHA-256 must be the manifest's, and only then are the
 * copies handed to kexec_load, with this boot's command line (its
 * hardware switches; kexec_next_cmdline). A refusal at any step leaves the
 * stored kernel as it was. On success /esp's files are noted as seen
 * (reboot.c), so the next `reboot` starts the fetched build instead of
 * reloading the stick's. Nothing is written to the stick.
 *
 * One offer channel at a time; each takes one offer, gets one answer
 * (struct update_answer) and is closed. The copy and the hash run in
 * init's loop: a few hundred milliseconds for a build, bounded by
 * UPDATE_FILE_MAX per file, and asked for only by the owner. */
#include <update.h>
#include "init.h"

#define CHUNK (64u << 10)   /* bytes copied and hashed at a time */

static handle_t offer;      /* our end of the offer channel (0: none) */
static handle_t offer_port;
static uint64_t offer_key;

static void drop_offer(void)
{
    if (!offer)
        return;
    (void)jam_port_unbind(offer_port, offer, offer_key);   /* closing undoes it anyway */
    jam_handle_close(offer);
    offer = HANDLE_INVALID;
}

status_t update_offer_new(handle_t port, uint64_t key, handle_t *client)
{
    drop_offer();
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    st = jam_port_bind(port, mine, key, SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st != OK) {
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    offer = mine;
    offer_port = port;
    offer_key = key;
    *client = theirs;
    return OK;
}

/* size bytes of src into a new VMO only init holds (*out), and their
 * SHA-256: the bytes hashed are the bytes written. ERR_OUT_OF_RANGE: src
 * is shorter than size; a read's error (no RIGHT_READ, ...). */
static status_t copy_and_hash(handle_t src, uint64_t size, handle_t *out,
                              uint8_t digest[SHA256_BYTES])
{
    uint64_t have = 0;
    status_t st = jam_vmo_get_size(src, &have);
    if (st == OK && have < size)
        st = ERR_OUT_OF_RANGE;
    handle_t dst = HANDLE_INVALID;
    if (st == OK)
        st = jam_vmo_create((size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1), 0, HANDLE_INVALID, &dst);
    uint8_t *buf = st == OK ? malloc(CHUNK) : NULL;
    if (st == OK && !buf)
        st = ERR_NO_MEMORY;
    struct sha256 h;
    sha256_init(&h);
    for (uint64_t off = 0; st == OK && off < size; off += CHUNK) {
        uint64_t n = size - off < CHUNK ? size - off : CHUNK;
        st = jam_vmo_read(src, off, buf, n);
        if (st == OK) {
            sha256_add(&h, buf, (size_t)n);
            st = jam_vmo_write(dst, off, buf, n);
        }
    }
    free(buf);
    if (st != OK) {
        if (dst)
            jam_handle_close(dst);
        return st;
    }
    sha256_done(&h, digest);
    *out = dst;
    return OK;
}

/* The answer's refusal. */
static void refuse(struct update_answer *a, uint32_t why, uint32_t file, status_t st)
{
    a->why = why;
    a->file = file;
    a->status = st;
}

/* Both files copied and checked into mine[]; false (a filled in) if not. */
static bool check_files(const struct update_offer *o, const struct update_manifest *m,
                        const handle_t hs[UPDATE_FILES], handle_t mine[UPDATE_FILES],
                        struct update_answer *a)
{
    for (uint32_t f = 0; f < UPDATE_FILES; f++) {
        if (o->bytes[f] != m->file[f].size) {
            refuse(a, UPDATE_BAD_SIZE, f, ERR_INVALID_ARGS);
            return false;
        }
    }
    for (uint32_t f = 0; f < UPDATE_FILES; f++) {
        uint8_t digest[SHA256_BYTES];
        status_t st = copy_and_hash(hs[f], m->file[f].size, &mine[f], digest);
        if (st != OK) {
            refuse(a, UPDATE_SHORT_VMO, f, st);
            return false;
        }
        if (memcmp(digest, m->file[f].sha256, SHA256_BYTES)) {
            refuse(a, UPDATE_BAD_HASH, f, ERR_INVALID_ARGS);
            return false;
        }
    }
    return true;
}

/* The whole check of an offer of n bytes with nh handles; *a filled in. */
static void check(const struct update_offer *o, uint32_t n, const handle_t *hs, uint32_t nh,
                  struct update_answer *a)
{
    if (n != sizeof(*o) || nh != UPDATE_FILES || o->txid || o->magic != UPDATE_OFFER_MAGIC ||
        o->flags || o->manifest_len > UPDATE_MANIFEST_MAX) {
        refuse(a, UPDATE_BAD_OFFER, 0, ERR_INVALID_ARGS);
        return;
    }
    struct update_manifest m;
    status_t st = update_manifest_parse(o->manifest, o->manifest_len, &m);
    if (st != OK) {
        refuse(a, UPDATE_BAD_MANIFEST, 0, st);
        return;
    }
    memcpy(a->version, m.version, sizeof(a->version));
    memcpy(a->git, m.git, sizeof(a->git));
    handle_t mine[UPDATE_FILES] = { HANDLE_INVALID, HANDLE_INVALID };
    if (check_files(o, &m, hs, mine, a)) {
        st = jam_kexec_load(shell_root(), mine[UPDATE_KERNEL], mine[UPDATE_BOOTFS], NULL, 0, 0);
        if (st == OK)
            reboot_keep_stored();
        else
            refuse(a, UPDATE_NOT_LOADED, 0, st);
    }
    for (uint32_t f = 0; f < UPDATE_FILES; f++)
        if (mine[f])
            jam_handle_close(mine[f]);
}

static void say(const struct update_answer *a, const struct update_offer *o)
{
    if (a->why == UPDATE_ACCEPTED) {
        printf("init: update: %s (%s) checked in %u ms (kernel %lu bytes, bootfs %lu bytes) "
               "and stored: `reboot` starts it\n", a->version, a->git, a->check_ms,
               (unsigned long)o->bytes[UPDATE_KERNEL], (unsigned long)o->bytes[UPDATE_BOOTFS]);
        return;
    }
    bool per_file = a->why == UPDATE_BAD_SIZE || a->why == UPDATE_SHORT_VMO ||
                    a->why == UPDATE_BAD_HASH;
    printf("init: update: refused: %s%s%s (%s); the stored kernel is unchanged\n",
           update_why_str(a->why), per_file ? ": " : "", per_file ? update_file_name(a->file) : "",
           status_str(a->status));
}

/* The offer message into o (a whole one, or what there was of it). */
static status_t read_offer(struct update_offer *o, uint32_t *n, handle_t hs[UPDATE_FILES + 1],
                           uint32_t *nh)
{
    struct channel_read_args r = {
        .h = offer, .bytes_cap = sizeof(*o), .bytes = (uint64_t)(uintptr_t)o,
        .actual_bytes = (uint64_t)(uintptr_t)n, .handles = (uint64_t)(uintptr_t)hs,
        .handles_cap = UPDATE_FILES + 1, .actual_handles = (uint64_t)(uintptr_t)nh,
    };
    status_t st = jam_channel_read(&r);
    if (st == ERR_BUFFER_TOO_SMALL)
        *n = *nh = 0;   /* too big, or too many handles: left queued, refused */
    return st == ERR_BUFFER_TOO_SMALL ? OK : st;
}

void update_event(void)
{
    if (!offer)
        return;
    struct update_offer *o = calloc(1, sizeof(*o));
    handle_t hs[UPDATE_FILES + 1];
    uint32_t n = 0, nh = 0;
    status_t st = o ? read_offer(o, &n, hs, &nh) : ERR_NO_MEMORY;
    if (st == ERR_SHOULD_WAIT) {
        free(o);
        return;   /* nothing yet */
    }
    if (st == OK) {
        uint64_t t0 = now();
        struct update_answer a = { .magic = UPDATE_ANSWER_MAGIC };
        check(o, n, hs, nh, &a);
        uint64_t ms = (now() - t0) / NS_PER_MS;
        a.check_ms = ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
        say(&a, o);
        (void)jam_channel_write(offer, &a, sizeof(a), NULL, 0);   /* a gone sender reads nothing */
        for (uint32_t i = 0; i < nh; i++)
            jam_handle_close(hs[i]);
    }
    free(o);
    drop_offer();   /* one offer per channel; a closed one ends here too */
}
