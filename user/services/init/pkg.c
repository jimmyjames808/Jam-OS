/* init's side of the packages (docs/M13.7-PLAN.md section 9, track PO5):
 * the pkgctl channel (abi/idl/pkgctl.idl) that the shell hands to bin/pkg,
 * and the settle of a package transaction cut short, once the volume that
 * holds the packages is mounted at boot (/data, or /jam when the Jam OS
 * drive's volume is there: each has its own journal).
 *
 * The work is pkgapprove.c's, on paths under /data/pkg or /jam/pkg (the
 * approvals are /data's), with the ports key in
 * this build's own boot image (/boot/ports.pub: PORTS_KEY_FILE; a build
 * without one approves nothing and settles nothing). It reads and hashes
 * programs of up to 1 GiB, in pieces, and writes files, so it runs on a worker
 * thread, as update.c's check does, and the loop goes on serving: a
 * request on the channel is read only when the worker is idle (the next
 * one waits in the channel), handed to the worker, and answered from the
 * loop when the worker's packet comes (KEY_PKG). The worker touches only
 * its struct job; its `done` is the hand-back (RELEASE there, ACQUIRE
 * here).
 *
 * One channel at a time (initctl.pkg_channel): a newer one closes the
 * older, unless the older one's call is being worked on (ERR_BAD_STATE).
 * The settle at boot is a job like a request's, with no one to answer:
 * its line goes in the log. */
#include <dataprog.h>
#include <idl/pkgctl.h>
#include <update.h>
#include "init.h"
#include "pkgapprove.h"

#define PORTS_KEY_FILE "ports.pub"     /* the ports key's public half, in bootfs */
#define PKG_PREFIX     DATA_MOUNT "/pkg"   /* a stick on its own */
#define PKG_JAM_PREFIX JAM_MOUNT "/pkg"    /* with the Jam OS drive's volume (pkgctl's JAM) */
#define WORKER_STACK   (64u << 10)
#define WORKER_END     (2 * NS_PER_S)  /* its packet came: it ends within this */
#define QUEUE_TRIES    100             /* a full port: tries 10 ms apart */

/* The two places the packages may be: [0] on /data, [1] on /jam. The
 * approvals are /data's in both. */
static const struct pkga_place places[2] = {
    { .prefix = PKG_PREFIX, .etc = DP_ALLOW_DIR, .allow = DP_ALLOW_FILE, .tables = DP_TABLE_DIR },
    { .prefix = PKG_JAM_PREFIX, .etc = DP_ALLOW_DIR, .allow = DP_ALLOW_FILE,
      .tables = DP_TABLE_DIR },
};

enum job_kind { JOB_APPROVE, JOB_REVOKE, JOB_SETTLE };

/* One request (or the settle) on the worker. */
struct job {
    enum job_kind      kind;
    struct idl_txn     txn;          /* the call to answer (not JOB_SETTLE) */
    uint32_t           flags;        /* approve's */
    struct mapped_text {
        handle_t vmo;                /* our copy of the manifest */
        uint64_t len;                /* its bytes */
    } manifest;
    char               name[PKGCTL_NAME_MAX + 1], version[PKGCTL_NAME_MAX + 1];
    uint32_t           revision;     /* revoke's */
    struct pkga_result r;            /* approve's answer */
    uint32_t           lines;        /* revoke's */
    status_t           st;           /* revoke's */
    bool               jam;          /* the settle's: of /jam/pkg (else /data/pkg) */
    struct pkga_settled settled;     /* the settle's */
    bool               done;         /* the worker's hand-back */
};

static handle_t     ch;              /* our end of the pkgctl channel (0: none) */
static handle_t     port;
static uint64_t     port_key;
static struct job  *busy;            /* on the worker (NULL: none) */
static handle_t     worker;
static bool         worker_stuck;    /* one didn't end: its stack can't be used again */
static unsigned     settle_due;      /* a volume came while the worker was busy: bit 0 /data, 1 /jam */
static uint8_t      worker_stack[WORKER_STACK];

/* This build's ports key. ERR_NOT_FOUND: built without one. */
static status_t ports_key(uint8_t key[PKG_KEY_BYTES])
{
    const struct bootfs_view *fs;
    const void *text;
    uint64_t len = 0;
    status_t st = bootfs_default(&fs);
    if (st == OK)
        st = bootfs_lookup(fs, PORTS_KEY_FILE, &text, &len);
    return st == OK ? update_key_parse(text, (size_t)len, key) : st;
}

/* The manifest's bytes, mapped from our copy (*addr; unmap with its len). */
static uint8_t *map_manifest(const struct job *j, uint64_t *len)
{
    uint64_t addr = 0;
    *len = (j->manifest.len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (jam_vmar_map(startup_handle(SR_SELF_VMAR), j->manifest.vmo, 0, *len, VMAR_READ,
                     &addr) != OK)
        return NULL;
    return (uint8_t *)(uintptr_t)addr;
}

static void run_approve(struct job *j)
{
    uint8_t key[PKG_KEY_BYTES];
    status_t st = ports_key(key);
    if (st != OK) {
        j->r = (struct pkga_result){ .verdict = ERR_ACCESS_DENIED, .program = PKGA_NONE };
        snprintf(j->r.why, sizeof(j->r.why), "this build has no ports key (%s)",
                 st == ERR_NOT_FOUND ? "no /boot/" PORTS_KEY_FILE : status_str(st));
        return;
    }
    uint64_t len;
    uint8_t *text = map_manifest(j, &len);
    if (!text) {
        j->r = (struct pkga_result){ .verdict = ERR_NO_MEMORY, .program = PKGA_NONE };
        snprintf(j->r.why, sizeof(j->r.why), "can't map the manifest");
        return;
    }
    pkga_approve(&places[(j->flags & PKGCTL_JAM) != 0], key, text, j->manifest.len,
                 j->flags & ~PKGCTL_JAM, &j->r);
    jam_vmar_unmap(startup_handle(SR_SELF_VMAR), (uint64_t)(uintptr_t)text, len);
}

static void run_settle(struct job *j)
{
    uint8_t key[PKG_KEY_BYTES];
    pkga_settle(&places[j->jam], ports_key(key) == OK ? key : NULL, &j->settled);
}

static void worker_main(void *arg)
{
    struct job *j = arg;
    if (j->kind == JOB_APPROVE) {
        run_approve(j);
    } else if (j->kind == JOB_REVOKE) {
        struct pkg_str name = { j->name, (uint32_t)strlen(j->name) };
        struct pkg_str version = { j->version, (uint32_t)strlen(j->version) };
        j->st = pkga_revoke(&places[0], name, version, j->revision, &j->lines);   /* /data's */
    } else {
        run_settle(j);
    }
    __atomic_store_n(&j->done, true, __ATOMIC_RELEASE);   /* pkg_event's ACQUIRE load */
    struct port_packet pkt = { .key = port_key, .type = PORT_PACKET_USER };
    for (int i = 0; i < QUEUE_TRIES && jam_port_queue(port, &pkt) != OK; i++)
        jam_sleep_until(now() + 10 * NS_PER_MS);   /* a full port: the loop is far behind */
}

/* The job answered (or, for the settle, said) and freed. */
static void finish(struct job *j)
{
    if (j->kind == JOB_APPROVE) {
        const struct pkga_result *r = &j->r;
        if (r->verdict == OK)
            printf("init: pkg: approved %u program%s\n", r->programs, r->programs == 1 ? "" : "s");
        else
            printf("init: pkg: refused: %s\n", r->why);
        (void)pkgctl_reply_approve(j->txn, OK, r->verdict, r->programs, r->program, r->why,
                                   (uint32_t)strlen(r->why));
    } else if (j->kind == JOB_REVOKE) {
        printf("init: pkg: revoke %s %s-r%u: %u line%s%s%s\n", j->name, j->version, j->revision,
               j->lines, j->lines == 1 ? "" : "s", j->st == OK ? "" : ", ",
               j->st == OK ? "" : status_str(j->st));
        (void)pkgctl_reply_revoke(j->txn, j->st, j->lines);
    } else if (j->settled.journal) {
        printf("init: pkg: settle: %s\n", j->settled.what);
    }
    if (j->manifest.vmo)
        jam_handle_close(j->manifest.vmo);
    free(j);
}

/* Hand j to the worker (or, if no thread can start, do it here). */
static void start(struct job *j)
{
    busy = j;
    if (!worker_stuck && thread_spawn("pkg", worker_main, j, worker_stack, sizeof(worker_stack),
                                      &worker) == OK)
        return;
    busy = NULL;
    worker_main(j);   /* its packet comes and finds nothing busy: ignored */
    finish(j);
}

/* The worker said it is done: wait for its thread to end, then answer. */
static void worker_done(void)
{
    struct job *j = busy;
    busy = NULL;
    signals_t seen;
    status_t st = jam_object_wait_one(worker, SIG_TERMINATED, now() + WORKER_END, &seen);
    jam_handle_close(worker);
    worker = HANDLE_INVALID;
    if (st != OK) {
        init_say("init: pkg: the worker didn't end (%s): the rest on init's loop",
                 status_str(st));
        worker_stuck = true;
    }
    finish(j);
}

/* ---- the channel ------------------------------------------------------------------- */

static void drop_channel(void)
{
    if (!ch)
        return;
    (void)jam_port_unbind(port, ch, port_key);
    jam_handle_close(ch);
    ch = HANDLE_INVALID;
}

status_t pkg_channel_new(handle_t to_port, uint64_t key, handle_t *client)
{
    if (busy && busy->kind != JOB_SETTLE)
        return ERR_BAD_STATE;   /* the last call is being worked on */
    drop_channel();
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    port = to_port;
    port_key = key;
    st = jam_port_bind(port, mine, key, SIG_READABLE | SIG_PEER_CLOSED, PORT_BIND_PERSISTENT);
    if (st != OK) {
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    ch = mine;
    *client = theirs;
    return OK;
}

/* pkgctl.approve: the manifest copied into a VMO only init holds (what the
 * worker checks can't change under it), then the worker. */
static status_t op_approve(void *ctx, struct idl_txn txn, uint32_t flags, uint64_t manifest_bytes,
                           handle_t manifest, int32_t *out_verdict, uint32_t *out_programs,
                           uint32_t *out_program, char *out_why, uint32_t *out_why_len)
{
    (void)ctx, (void)out_verdict, (void)out_programs, (void)out_program, (void)out_why;
    (void)out_why_len;   /* answered by finish, later */
    if (!manifest_bytes || manifest_bytes > PKG_MANIFEST_MAX) {
        jam_handle_close(manifest);
        return ERR_OUT_OF_RANGE;
    }
    struct job *j = calloc(1, sizeof(*j));
    handle_t copy = HANDLE_INVALID;
    uint64_t len = (manifest_bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1), addr = 0;
    status_t st = j ? jam_vmo_create(len, 0, &copy) : ERR_NO_MEMORY;
    if (st == OK)
        st = jam_vmar_map(startup_handle(SR_SELF_VMAR), copy, 0, len, VMAR_READ | VMAR_WRITE,
                          &addr);
    if (st == OK) {
        st = jam_vmo_read(manifest, 0, (void *)(uintptr_t)addr, manifest_bytes);
        jam_vmar_unmap(startup_handle(SR_SELF_VMAR), addr, len);
    }
    jam_handle_close(manifest);
    if (st != OK) {
        if (copy)
            jam_handle_close(copy);
        free(j);
        return st == ERR_OUT_OF_RANGE || st == ERR_INVALID_ARGS ? ERR_OUT_OF_RANGE : st;
    }
    j->kind = JOB_APPROVE;
    j->txn = txn;
    j->flags = flags;
    j->manifest.vmo = copy;
    j->manifest.len = manifest_bytes;
    start(j);
    return IDL_LATER;
}

static status_t op_revoke(void *ctx, struct idl_txn txn, const char *name, uint32_t name_len,
                          const char *version, uint32_t version_len, uint32_t revision,
                          uint32_t *out_lines)
{
    (void)ctx, (void)out_lines;   /* answered by finish, later */
    struct job *j = calloc(1, sizeof(*j));
    if (!j)
        return ERR_NO_MEMORY;
    if (!name_len || name_len > PKGCTL_NAME_MAX || !version_len ||
        version_len > PKGCTL_NAME_MAX) {
        free(j);
        return ERR_INVALID_ARGS;
    }
    memcpy(j->name, name, name_len);
    memcpy(j->version, version, version_len);
    if (strlen(j->name) != name_len || strlen(j->version) != version_len) {   /* a NUL in one */
        free(j);
        return ERR_INVALID_ARGS;
    }
    j->kind = JOB_REVOKE;
    j->txn = txn;
    j->revision = revision;
    start(j);
    return IDL_LATER;
}

static const struct pkgctl_ops ops = { .approve = op_approve, .revoke = op_revoke };

void pkg_data_came(handle_t to_port, uint64_t key, bool jam)
{
    if (busy) {
        settle_due |= 1u << jam;
        return;
    }
    port = to_port;
    port_key = key;
    struct job *j = calloc(1, sizeof(*j));
    if (!j) {
        printf("init: pkg: no memory to settle the journal\n");
        return;
    }
    j->kind = JOB_SETTLE;
    j->jam = jam;
    start(j);
}

void pkg_event(void)
{
    if (busy) {
        if (!__atomic_load_n(&busy->done, __ATOMIC_ACQUIRE))
            return;   /* the channel spoke meanwhile: read once the worker is done */
        worker_done();
    }
    for (unsigned v = 0; v < 2 && !busy; v++)
        if (settle_due & 1u << v) {
            settle_due &= ~(1u << v);
            pkg_data_came(port, port_key, v);
        }
    /* One request at a time: the next is read once this one is answered
     * (the worker's packet brings us back here). */
    status_t st = ch && !busy ? pkgctl_serve_one(ch, &ops, NULL) : OK;
    if (!ch || busy)
        return;
    if (st == OK) {
        /* More may be queued: the binding fires on a new edge only. */
        if (port_again(port, ch, port_key, SIG_READABLE | SIG_PEER_CLOSED) != OK)
            drop_channel();
    } else if (st != ERR_SHOULD_WAIT) {
        drop_channel();   /* bin/pkg is gone */
    }
}
