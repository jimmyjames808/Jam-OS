/* init's half of `update` (<update.h>, docs/M9-PLAN.md "update: a new
 * build from the Mac"): a fetched build checked against its manifest and
 * made the kernel's stored copy, the one `reboot` and a panic start.
 *
 * The fetcher parses what the network sent and holds no power; init holds
 * the power (kexec_load, RIGHT_ROOT_KEXEC) and parses nothing from the
 * network but the manifest, with the strict parser. An offer arrives on a
 * channel init made (initctl.update_offer), as one struct update_offer
 * with the kernel's and the boot image's VMOs. First the signature: the
 * manifest must be signed by the key in this build's own boot image
 * (UPDATE_KEY_FILE; a build without one refuses every offer), over
 * exactly the bytes before its signature line, before any size or hash in
 * it is used. Then init copies each file into
 * a VMO only it holds, hashing exactly the bytes it writes, so nothing the
 * sender does to its own VMOs afterwards changes what was checked; then
 * every length and SHA-256 must be the manifest's, and only then are the
 * copies handed to kexec_load, with this boot's command line (its
 * hardware switches; kexec_next_cmdline). A refusal at any step leaves the
 * stored kernel as it was. On success /esp's files are noted as seen
 * (reboot.c), so the next `reboot` starts the fetched build instead of
 * reloading the stick's. Nothing is written to the stick.
 *
 * Who does what (the service-loop rule): the loop reads the offer, parses
 * the manifest, checks its signature (one Ed25519 check over at most 1 KiB,
 * well under a millisecond) and compares the lengths, all quick; the copy
 * and the hash
 * (a few hundred milliseconds for a build, up to UPDATE_FILE_MAX per file)
 * run on a worker thread, so the loop goes on serving meanwhile. The
 * worker touches only its struct check and the VMOs in it, and when it is
 * done it queues a packet with the offer channel's key; the loop then
 * calls kexec_load itself (init's power is used from the loop only),
 * notes /esp, answers and closes the channel.
 *
 * One offer channel at a time; each takes one offer, gets one answer
 * (struct update_answer) and is closed. While a check runs, a new offer
 * channel is refused (ERR_BAD_STATE). */
#include <update.h>
#include "init.h"

#define CHUNK        (64u << 10)        /* bytes copied and hashed at a time */
#define WORKER_STACK (16u << 10)
#define WORKER_END   (2 * NS_PER_S)     /* its packet came: it ends within this */
#define QUEUE_TRIES  100                /* a full port: tries 10 ms apart */

/* An offer being checked: the loop's until the worker starts, the
 * worker's until it sets `done` (RELEASE), the loop's again once it has
 * read `done` (ACQUIRE). */
struct check {
    struct update_offer   *o;                    /* the message (heap) */
    handle_t               hs[UPDATE_FILES];     /* the sender's VMOs */
    struct update_manifest m;                    /* parsed by the loop */
    handle_t               mine[UPDATE_FILES];   /* init's copies (0: not made) */
    struct update_answer   a;
    uint64_t               t0;                   /* uptime ns: the offer read */
    uint64_t               hash_ms;              /* the worker's copy and hash */
    uint64_t               verify_us;            /* the signature's check */
    bool                   done;                 /* the worker has finished */
};

static handle_t offer;      /* our end of the offer channel (0: none) */
static handle_t offer_port;
static uint64_t offer_key;
static struct check *busy;  /* the check on the worker (NULL: none) */
static handle_t worker;     /* its thread */
static bool worker_stuck;   /* a worker didn't end: its stack can't be used again */
static uint8_t worker_stack[WORKER_STACK];

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
    if (busy || worker_stuck)
        return ERR_BAD_STATE;   /* the last offer's check isn't finished */
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

/* The worker's job: both files copied into c->mine and their SHA-256s
 * compared with the manifest's (a refusal goes in c->a). */
static void hash_files(struct check *c)
{
    for (uint32_t f = 0; f < UPDATE_FILES; f++) {
        uint8_t digest[SHA256_BYTES];
        status_t st = copy_and_hash(c->hs[f], c->m.file[f].size, &c->mine[f], digest);
        if (st != OK) {
            refuse(&c->a, UPDATE_SHORT_VMO, f, st);
            return;
        }
        if (memcmp(digest, c->m.file[f].sha256, SHA256_BYTES)) {
            refuse(&c->a, UPDATE_BAD_HASH, f, ERR_INVALID_ARGS);
            return;
        }
    }
}

/* The worker thread: hash_files, then tell the loop (a packet with the
 * offer channel's key: update_event sees `done`). */
static void worker_main(void *arg)
{
    struct check *c = arg;
    uint64_t t0 = now();
    hash_files(c);
    c->hash_ms = (now() - t0) / NS_PER_MS;
    __atomic_store_n(&c->done, true, __ATOMIC_RELEASE);   /* update_event's ACQUIRE load */
    struct port_packet pkt = { .key = offer_key, .type = PORT_PACKET_USER };
    for (int i = 0; i < QUEUE_TRIES && jam_port_queue(offer_port, &pkt) != OK; i++)
        jam_nanosleep(now() + 10 * NS_PER_MS);   /* a full port: the loop is far behind */
}

/* This build's update key: the public key file in our boot image
 * (UPDATE_KEY_FILE). ERR_NOT_FOUND: the build was made without one;
 * ERR_INVALID_ARGS: the file isn't a key. */
static status_t build_key(uint8_t key[UPDATE_KEY_BYTES])
{
    const struct bootfs_view *fs;
    const void *text;
    uint64_t len = 0;
    status_t st = bootfs_default(&fs);
    if (st == OK)
        st = bootfs_lookup(fs, UPDATE_KEY_FILE, &text, &len);
    return st == OK ? update_key_parse(text, (size_t)len, key) : st;
}

/* The manifest, parsed (its format only), and its signature: key signed
 * exactly its bytes before the signature line. Nothing else it says is
 * used before this has passed. False (c->a filled in) if refused. */
static bool check_manifest(struct check *c, const uint8_t key[UPDATE_KEY_BYTES])
{
    const struct update_offer *o = c->o;
    status_t st = update_manifest_parse(o->manifest, o->manifest_len, &c->m);
    if (st != OK) {
        refuse(&c->a, UPDATE_BAD_MANIFEST, 0, st);
        return false;
    }
    if (!c->m.has_signature) {
        refuse(&c->a, UPDATE_UNSIGNED, 0, ERR_ACCESS_DENIED);
        return false;
    }
    uint64_t t0 = now();
    st = update_manifest_verify(&c->m, o->manifest, key);
    c->verify_us = (now() - t0) / 1000;
    if (st != OK) {
        refuse(&c->a, UPDATE_BAD_SIGNATURE, 0, st);
        return false;
    }
    return true;
}

/* The quick checks of an offer of n bytes with nh handles, in the loop:
 * false (c->a filled in) if refused. A build without a key refuses every
 * offer, whatever it holds. */
static bool check_offer(struct check *c, uint32_t n, uint32_t nh)
{
    const struct update_offer *o = c->o;
    if (n != sizeof(*o) || nh != UPDATE_FILES || o->txid || o->magic != UPDATE_OFFER_MAGIC ||
        (o->flags & ~UPDATE_OFFER_CHECK_ONLY) || o->manifest_len > UPDATE_MANIFEST_MAX) {
        refuse(&c->a, UPDATE_BAD_OFFER, 0, ERR_INVALID_ARGS);
        return false;
    }
    uint8_t key[UPDATE_KEY_BYTES];
    status_t st = build_key(key);
    if (st != OK) {
        refuse(&c->a, UPDATE_NO_KEY, 0, st);
        return false;
    }
    if (!check_manifest(c, key))
        return false;
    memcpy(c->a.version, c->m.version, sizeof(c->a.version));
    memcpy(c->a.git, c->m.git, sizeof(c->a.git));
    for (uint32_t f = 0; f < UPDATE_FILES; f++) {
        if (o->bytes[f] != c->m.file[f].size) {
            refuse(&c->a, UPDATE_BAD_SIZE, f, ERR_INVALID_ARGS);
            return false;
        }
    }
    return true;
}

static void say(const struct check *c)
{
    const struct update_answer *a = &c->a;
    const struct update_offer *o = c->o;
    if (a->why == UPDATE_ACCEPTED) {
        bool only = o->flags & UPDATE_OFFER_CHECK_ONLY;
        printf("init: update: %s (%s) checked in %u ms (signature %lu us; kernel %lu + bootfs "
               "%lu bytes hashed in %lu ms off the loop) %s\n",
               a->version, a->git, a->check_ms, (unsigned long)c->verify_us,
               (unsigned long)o->bytes[UPDATE_KERNEL], (unsigned long)o->bytes[UPDATE_BOOTFS],
               (unsigned long)c->hash_ms,
               only ? "and not loaded (check only)" : "and stored: `reboot` starts it");
        return;
    }
    bool per_file = a->why == UPDATE_BAD_SIZE || a->why == UPDATE_SHORT_VMO ||
                    a->why == UPDATE_BAD_HASH;
    printf("init: update: refused: %s%s%s (%s); the stored kernel is unchanged\n",
           update_why_str(a->why), per_file ? ": " : "", per_file ? update_file_name(a->file) : "",
           status_str(a->status));
}

/* The end of a check, in the loop: the copies loaded unless refused or
 * check-only, the answer said and written, everything let go. */
static void finish(struct check *c, uint32_t nh)
{
    bool only = c->o->flags & UPDATE_OFFER_CHECK_ONLY;
    if (c->a.why == UPDATE_ACCEPTED && !only) {
        status_t st = jam_kexec_load(shell_root(), c->mine[UPDATE_KERNEL],
                                     c->mine[UPDATE_BOOTFS], NULL, 0, 0);
        if (st == OK)
            reboot_keep_stored();
        else
            refuse(&c->a, UPDATE_NOT_LOADED, 0, st);
    }
    uint64_t ms = (now() - c->t0) / NS_PER_MS;
    c->a.check_ms = ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
    say(c);
    (void)jam_channel_write(offer, &c->a, sizeof(c->a), NULL, 0);   /* a gone sender reads nothing */
    for (uint32_t f = 0; f < UPDATE_FILES; f++) {
        if (f < nh && c->hs[f])
            jam_handle_close(c->hs[f]);
        if (c->mine[f])
            jam_handle_close(c->mine[f]);
    }
    free(c->o);
    free(c);
    drop_offer();   /* one offer per channel */
}

/* The worker said it is done: wait for its thread to end (its stack is
 * reused), then finish. */
static void worker_done(void)
{
    struct check *c = busy;
    busy = NULL;
    signals_t seen;
    status_t st = jam_object_wait_one(worker, SIG_TERMINATED, now() + WORKER_END, &seen);
    jam_handle_close(worker);
    worker = HANDLE_INVALID;
    if (st != OK) {
        init_say("init: update: the check's thread didn't end (%s): no more updates this boot",
                 status_str(st));
        worker_stuck = true;
    }
    finish(c, UPDATE_FILES);
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

/* A new offer read: refused at once, or handed to the worker (or, if no
 * thread can start, hashed here after all). */
static void start_check(struct check *c, const handle_t *hs, uint32_t n, uint32_t nh)
{
    c->t0 = now();
    c->a.magic = UPDATE_ANSWER_MAGIC;
    for (uint32_t i = UPDATE_FILES; i < nh; i++)
        jam_handle_close(hs[i]);   /* one too many: refused below */
    for (uint32_t i = 0; i < UPDATE_FILES && i < nh; i++)
        c->hs[i] = hs[i];
    if (!check_offer(c, n, nh)) {
        finish(c, nh);
        return;
    }
    busy = c;
    if (thread_spawn("update check", worker_main, c, worker_stack, sizeof(worker_stack),
                     &worker) == OK)
        return;
    busy = NULL;
    worker_main(c);   /* its packet comes, and finds nothing busy: ignored */
    finish(c, UPDATE_FILES);
}

void update_event(void)
{
    if (busy) {
        if (__atomic_load_n(&busy->done, __ATOMIC_ACQUIRE))
            worker_done();
        return;   /* else: the sender's end closed or wrote again meanwhile; seen at the end */
    }
    if (!offer)
        return;
    struct check *c = calloc(1, sizeof(*c));
    struct update_offer *o = calloc(1, sizeof(*o));
    handle_t hs[UPDATE_FILES + 1];
    uint32_t n = 0, nh = 0;
    status_t st = c && o ? read_offer(o, &n, hs, &nh) : ERR_NO_MEMORY;
    if (st == OK) {
        c->o = o;
        start_check(c, hs, n, nh);
        return;
    }
    free(o);
    free(c);
    if (st != ERR_SHOULD_WAIT)
        drop_offer();   /* the sender is gone (or we can't read): the channel ends here */
}
