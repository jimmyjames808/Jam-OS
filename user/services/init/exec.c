/* init's sealer: /svc/exec (abi/idl/exec.idl; docs/CR-PLAN.md, track CE1).
 * A program starts another program on /data, one the owner approved; init
 * seals it against its approval and starts it with its own approval's
 * namespace and powers, the way the shell starts a program typed at it
 * (sh_dataprog.c), by the same rules (libos's <dataprog.h>).
 *
 * Why init starts the child. The owner's rule (2026-10-12) is that the
 * child gets its own approval only: what its approved list asks for,
 * whatever its parent has. A parent can't give what it doesn't hold, and
 * what it is handed it could keep, so the parent never touches the child's
 * authority: init resolves the list in its own namespace (spawn's ns, as
 * the shell's grants are), duplicates the root with the list's powers,
 * makes the process and starts it. The parent gets the process (wait and
 * inspect: no RIGHT_WRITE, which would let it make threads in a process
 * holding more than it does) and the child's job (RIGHT_MANAGE, to kill it
 * and limit it). What the parent does give is its own: the job the child
 * goes in (a kill of the parent's job takes it), argv and the environment,
 * the hand-over (its descriptors, written by the parent onto a channel
 * whose other end init holds, and moved message for message onto the
 * child's startup channel) and the child's end of its signal channel.
 *
 * Two steps, so that a refusal comes before the parent's hand-over moves
 * anything out of its own table (posix_spawn finds its program first:
 * user/posix/sys/spawn.c):
 *   open   the path, the caller's own open of the file (its views decide
 *          what it can name) and an empty channel end for the hand-over.
 *          The file must be the one init's /data has at the path (the same
 *          id and size); a line of the approvals must name the path, and
 *          its page table be the table of a file of this size. The file is
 *          mapped (FILE_MAP_EXEC: a VMO only init holds, whose pages
 *          /data's service supplies) and sealed with the table
 *          (vmo_make_exec_verified, RIGHT_ROOT_VMEX: init's root); its list
 *          is read from the sealed VMO with vmo_read and refused if it asks
 *          for what no program from /data may have. The answer is an
 *          exec_prog channel: the sealed program, waiting.
 *   start  on that channel: the job, argv and environment, the signal end;
 *          the child is made and started (above), the channel closed.
 * From the seal on, the kernel checks every page of the program against
 * the approval as it is fetched (at the start, or whenever the program
 * touches it), so only the bytes the owner approved ever run: a file
 * changed since (on another computer: fat won't write a file that is
 * mapped to run) is refused at the start, or ends the child at the page.
 *
 * Who serves it: a thread of its own (the worker), with a port of its
 * own, so that a slow /data, a pager that answers late or a caller's file
 * channel that never answers holds up the next caller, never init's loop.
 * Every call on a channel of a caller's is made with a deadline (CALL_WAIT).
 *
 * Bounds: EXEC_OPENERS_MAX openers; an opener that leaves answers unread
 * past the protocol's bound is cut off (sessions.c's rule). EXEC_PROGRAMS_MAX
 * sealed programs waiting in all, EXEC_PROGRAMS_PER_OPENER per opener, each
 * dropped after EXEC_WAIT_NS: a caller can hold at most that many sealed
 * VMOs (and their tables, the kernel's, charged to init's job) for that
 * long. A start's hand-over is at most HANDOVER_MAX messages. The child's
 * own memory is charged to its job, inside the caller's; what spawn makes
 * for it (its writable segments' copies and its stack's pages) is charged
 * to init's job, as a child of the shell's is to the shell's.
 *
 * Port keys: K_SHARED (the shared channel), K_OPENER + i, K_PROG + i. */
#include <dataprog.h>
#include <fs_idl.h>
#include <idl/exec.h>
#include <idl/svc.h>
#include <os.h>
#include <wants.h>
#include "init.h"

#define OPENERS      EXEC_OPENERS_MAX
#define PROGRAMS     EXEC_PROGRAMS_MAX
#define PER_OPENER   EXEC_PROGRAMS_PER_OPENER
#define ROUND        8                       /* requests served on one channel per packet */
#define CALL_WAIT    (5 * NS_PER_S)          /* a call on a caller's file channel */
#define POSIX_STACK  (8ull << 20)            /* a POSIX program's stack, as the shell gives */
#define HANDOVER_MAX 128                     /* hand-over messages moved at most */
#define STRINGS_MAX  EXEC_STRINGS_MAX
/* What the caller gets of the child's process and of its job. */
#define PROC_RIGHTS  (RIGHT_WAIT | RIGHT_INSPECT | RIGHT_DUPLICATE | RIGHT_TRANSFER)
#define JOB_GIVEN    (RIGHT_MANAGE | RIGHT_WAIT | RIGHT_INSPECT | RIGHT_DUPLICATE | \
                      RIGHT_TRANSFER)

enum { K_SHARED = 0x00, K_OPENER = 0x100, K_PROG = 0x200 };
_Static_assert(K_OPENER + OPENERS <= K_PROG, "opener keys below the programs'");

/* A sealed program waiting for its start. */
struct prog {
    handle_t     ch;          /* our end of its exec_prog channel; 0: a free slot */
    handle_t     vmo;         /* the sealed VMO: read, map, execute (ours alone) */
    handle_t     handover;    /* the end the caller's hand-over arrives on */
    uint64_t     size;        /* the program's bytes */
    uint64_t     expires;     /* uptime ns: dropped then, unstarted */
    int          opener;      /* the opener that sealed it, while that one is open; -1 */
    bool         done;        /* its start was answered: free it */
    char        *path;        /* its path (malloc'd) */
    struct wants w;           /* its list, read from the sealed VMO */
};

static handle_t port;                    /* the worker's */
static handle_t shared_srv;              /* /svc/exec's shared channel, our end */
static handle_t openers[OPENERS];        /* our ends of the openers' channels (0: free) */
static int      opener_ids[OPENERS];     /* each one's index, as its handlers' ctx */
static struct prog progs[PROGRAMS];
static int      prog_ids[PROGRAMS];
static uint8_t  worker_stack[64 << 10] __attribute__((aligned(16)));
static uint8_t  relay_buf[CHANNEL_MAX_BYTES] __attribute__((aligned(8)));   /* the worker's */

static void close_if(handle_t *h)
{
    if (*h)
        jam_handle_close(*h);
    *h = HANDLE_INVALID;
}

/* Is h a handle of this kind (with these rights)? */
static bool kind_ok(handle_t h, uint32_t type, rights_t need)
{
    struct handle_info hi;
    return jam_handle_info(h, &hi) == OK && hi.type == type && (hi.rights & need) == need;
}

/* ---- the programs ------------------------------------------------------------------ */

static void prog_free(struct prog *p)
{
    unsigned i = (unsigned)(p - progs);
    if (p->ch)
        (void)jam_port_unbind(port, p->ch, K_PROG + i);
    close_if(&p->ch);
    close_if(&p->vmo);
    close_if(&p->handover);
    free(p->path);
    memset(p, 0, sizeof(*p));
    p->opener = -1;
}

/* A free slot for opener's next program, or NULL (*st: why). */
static struct prog *prog_new(int opener, status_t *st)
{
    struct prog *free_slot = NULL;
    unsigned mine = 0;
    for (unsigned i = 0; i < PROGRAMS; i++) {
        if (!progs[i].ch && !progs[i].vmo && !free_slot)
            free_slot = &progs[i];
        mine += progs[i].ch && progs[i].opener == opener;
    }
    *st = free_slot && mine < PER_OPENER ? OK : ERR_NO_RESOURCES;
    return *st == OK ? free_slot : NULL;
}

/* The earliest drop, or DEADLINE_NEVER. */
static uint64_t next_expiry(void)
{
    uint64_t t = DEADLINE_NEVER;
    for (unsigned i = 0; i < PROGRAMS; i++)
        if (progs[i].ch && progs[i].expires < t)
            t = progs[i].expires;
    return t;
}

static void expire(uint64_t t)
{
    for (unsigned i = 0; i < PROGRAMS; i++)
        if (progs[i].ch && progs[i].expires <= t)
            prog_free(&progs[i]);
}

/* ---- open: the seal ---------------------------------------------------------------- */

/* path (len bytes, no NUL: a str) as a C string into out, if it is a
 * cleaned absolute path of a file on /data. */
static bool path_ok(const char *path, uint32_t len, char out[FS_PATH_MAX])
{
    char clean[FS_PATH_MAX];
    if (len >= FS_PATH_MAX)
        return false;
    memcpy(out, path, len);
    out[len] = '\0';
    return dp_on_data(out) && fs_path_clean(out, len, clean) == OK && !strcmp(clean, out + 1);
}

/* The size and id of the file init's own /data has at path. */
static status_t own_file(const char *path, uint64_t *size, uint64_t *id)
{
    struct jfile f;
    uint8_t dir = 0;
    uint64_t mtime;
    status_t st = file_open(path, FS_READ, &f);
    if (st != OK)
        return st == ERR_IS_DIR ? ERR_ACCESS_DENIED : st;
    st = file_stat_until(f.ch, now() + CALL_WAIT, size, &dir, &mtime, id);
    file_close(&f);
    return st == OK && dir ? ERR_ACCESS_DENIED : st;
}

/* Is `file` (the caller's) the file at path in init's /data, of *size
 * bytes? ERR_ACCESS_DENIED: another file, or not a file channel at all
 * (*why says which). */
static status_t same_file(handle_t file, const char *path, uint64_t *size, const char **why)
{
    uint64_t c_size, c_mtime, c_id, id;
    uint8_t c_dir = 0;
    status_t st = own_file(path, size, &id);
    if (st != OK) {
        *why = "nothing init can run at its path";
        return st == ERR_NOT_FOUND ? st : ERR_ACCESS_DENIED;
    }
    if (file_stat_until(file, now() + CALL_WAIT, &c_size, &c_dir, &c_mtime, &c_id) != OK) {
        *why = "the caller's file doesn't answer as a file";
        return ERR_ACCESS_DENIED;
    }
    if (c_dir || c_id != id || c_size != *size) {
        *why = "the caller's file isn't the file at its path";
        return ERR_ACCESS_DENIED;
    }
    return OK;
}

/* What a table's or the seal's refusal is to the caller: the approval
 * doesn't cover this file (not allowed, or changed since), or /data
 * couldn't read it. */
static status_t refusal(status_t st)
{
    switch (st) {
    case ERR_NOT_FOUND:      /* no table: an approval from before tables */
    case ERR_OUT_OF_RANGE:   /* a table of another size: it changed */
    case ERR_INVALID_ARGS:   /* a broken table */
    case ERR_NOT_SUPPORTED:  /* not a pager's VMO (a /data with no pages, or not /data's) */
    case ERR_BAD_STATE:      /* a VMO someone else holds too, or mapped already */
    case ERR_ACCESS_DENIED:
        return ERR_ACCESS_DENIED;
    case ERR_NO_MEMORY:
    case ERR_NO_RESOURCES:
        return st;
    }
    return ERR_IO;
}

/* Seal the program at path for p: the checks of open (exec.idl). file is
 * the caller's (closed here). A refusal is said in the log, with why. */
static status_t seal(struct prog *p, const char *path, handle_t file)
{
    char hex[DP_HEX_LEN + 1];
    uint64_t size = 0, msize = 0;
    handle_t pv = HANDLE_INVALID, table = HANDLE_INVALID;
    const char *why = "not the file at its path";
    status_t st = same_file(file, path, &size, &why), raw = st;
    if (st == OK && (raw = dp_allow_hash(path, hex)) != OK) {
        why = "no approval for it";
        st = ERR_ACCESS_DENIED;   /* no line for it (or no approvals to read) */
    }
    if (st == OK) {
        why = "its file can't be mapped to run";
        st = raw = file_map_until(file, now() + CALL_WAIT, FILE_MAP_EXEC, &pv, &msize);
        if (st != OK && st != ERR_BUSY)
            st = refusal(st);
        else if (st == OK && (!kind_ok(pv, OBJ_VMO, 0) || msize != size))
            st = ERR_ACCESS_DENIED;   /* not a VMO of this file */
    }
    close_if(&file);
    if (st == OK && (st = raw = dp_table_read(hex, msize, &table)) != OK) {
        why = st == ERR_OUT_OF_RANGE ? "changed since it was allowed (another size)" :
              st == ERR_NOT_FOUND ? "its approval has no page table (run it once from the shell)" :
              "its approval's page table is broken";
        st = refusal(st);
    }
    if (st == OK) {
        /* pv is ours alone and was never mapped or read: from here on the
         * kernel checks every page it publishes. pv is gone whatever
         * happens. */
        why = "can't be sealed";
        st = raw = jam_vmo_make_exec_verified(pv, startup_handle(SR_RESOURCE), table, 0,
                                              &p->vmo);
        pv = HANDLE_INVALID;
        if (st != OK)
            st = refusal(st);
    }
    close_if(&pv);
    close_if(&table);
    if (st == OK) {
        st = raw = dp_wants_vmo(p->vmo, msize, &p->w);
        why = st == ERR_IO ? "changed since it was allowed (or /data can't read it)" :
              "not a program it can run";
        if (st == ERR_IO)
            st = ERR_ACCESS_DENIED;   /* a page isn't the approved one: it changed */
        else if (st == ERR_INVALID_ARGS)
            st = ERR_WRONG_TYPE;      /* no program spawn can load */
    }
    const char *bad = st == OK ? dp_wants_refused(&p->w) : NULL;
    if (bad) {
        printf("init: exec: %s asks for %s, which no program from /data may have\n", path, bad);
        st = ERR_ACCESS_DENIED;
    } else if (st != OK) {
        printf("init: exec: %s refused: %s (%s)\n", path, why, status_str(raw));
    }
    p->size = msize;
    return st;
}

static status_t op_open(void *ctx, const char *path, uint32_t path_len, handle_t file,
                        handle_t handover, handle_t *out_program, uint8_t *out_posix,
                        char *out_wants, uint32_t *out_wants_len)
{
    int opener = *(const int *)ctx;
    char abs[FS_PATH_MAX];
    struct prog *p = NULL;
    handle_t mine = HANDLE_INVALID, theirs = HANDLE_INVALID;
    status_t st = kind_ok(file, OBJ_CHANNEL, 0) && kind_ok(handover, OBJ_CHANNEL, 0) &&
                  path_ok(path, path_len, abs) ? OK : ERR_INVALID_ARGS;
    if (st == OK)
        p = prog_new(opener, &st);
    if (st == OK) {
        p->vmo = HANDLE_INVALID;
        st = seal(p, abs, file);   /* file is closed there */
        file = HANDLE_INVALID;
    }
    if (st == OK && !(p->path = malloc(strlen(abs) + 1)))
        st = ERR_NO_MEMORY;
    if (st == OK)
        memcpy(p->path, abs, strlen(abs) + 1);
    if (st == OK)
        st = jam_channel_create(&mine, &theirs);
    unsigned i = p ? (unsigned)(p - progs) : 0;
    if (st == OK)
        st = jam_port_bind(port, mine, K_PROG + i, SIG_READABLE | SIG_PEER_CLOSED,
                           PORT_BIND_PERSISTENT);
    close_if(&file);
    if (st != OK) {
        close_if(&mine);
        close_if(&theirs);
        close_if(&handover);
        if (p)
            prog_free(p);
        return st;
    }
    p->ch = mine;
    p->handover = handover;
    p->opener = opener;
    p->expires = now() + EXEC_WAIT_NS;
    *out_program = theirs;
    *out_posix = p->w.posix;
    *out_wants_len = (uint32_t)strlen(p->w.text);
    memcpy(out_wants, p->w.text, *out_wants_len);
    return OK;
}

static const struct exec_ops open_ops = { .open = op_open };

/* ---- start --------------------------------------------------------------------------- */

/* spawn's before_start: the hand-over the caller wrote on its end, moved
 * message for message (handles and all) onto the child's startup
 * channel. ctx: our end of it. */
static status_t relay(handle_t startup, void *ctx)
{
    handle_t from = *(const handle_t *)ctx;
    for (unsigned n = 0;; n++) {
        handle_t hs[IDL_HANDLES_MAX];
        uint32_t got = 0, nh = 0;
        status_t st = jam_channel_read(&(struct channel_read_args){
            .h = from, .rd_cap = sizeof(relay_buf), .rd_bytes = (uint64_t)(uintptr_t)relay_buf,
            .rd_actual = (uint64_t)(uintptr_t)&got, .rd_handles = (uint64_t)(uintptr_t)hs,
            .rd_hcap = IDL_HANDLES_MAX, .rd_hactual = (uint64_t)(uintptr_t)&nh,
        });
        if (st == ERR_SHOULD_WAIT || st == ERR_PEER_CLOSED)
            return OK;   /* all of it (the caller may have closed its end after writing) */
        if (st != OK)
            return st == ERR_BUFFER_TOO_SMALL ? ERR_INVALID_ARGS : st;   /* left, closed with it */
        if (n == HANDOVER_MAX) {
            idl_close_all(hs, nh);
            return ERR_OUT_OF_RANGE;
        }
        st = jam_channel_write(startup, relay_buf, got, hs, nh);   /* the handles go either way */
        if (st != OK)
            return st;
    }
}

/* strings (len bytes: argc strings of argv, then the environment, each
 * NUL-terminated) into argv and envp, each NULL-terminated (room for
 * STRINGS_MAX + 1). */
static bool split_strings(uint16_t argc, const uint8_t *strings, uint32_t len,
                          const char **argv, const char **envp)
{
    unsigned n = 0, a = 0, e = 0;
    for (uint32_t at = 0; at < len; n++) {
        size_t l = strnlen((const char *)strings + at, len - at);
        if (at + l == len || n == STRINGS_MAX)
            return false;   /* a string without its NUL, or too many */
        if (n < argc)
            argv[a++] = (const char *)strings + at;
        else
            envp[e++] = (const char *)strings + at;
        at += (uint32_t)l + 1;
    }
    argv[a] = envp[e] = NULL;
    return argc >= 1 && argc <= n;
}

/* The child of p in a new job inside job: made and started as the list
 * says. *proc and *sub ours on OK. */
static status_t start_child(struct prog *p, const char **argv, int argc, const char **envp,
                            handle_t job, handle_t signals, handle_t *proc, handle_t *sub)
{
    const char *grants[WANTS_MAX + 1];
    for (unsigned i = 0; i < p->w.n; i++)
        grants[i] = p->w.grant[i];
    grants[p->w.n] = NULL;
    struct spawn_handle x[2] = { { SR_SIGNALS, signals } };
    rights_t xr[2] = { RIGHT_SAME };
    unsigned nx = 1;
    status_t st = jam_job_create(job, 0, sub);
    if (st == OK && p->w.rights) {
        /* The root with the list's powers, never RIGHT_TRANSFER (spawn
         * passes it on with those rights alone). */
        st = jam_handle_duplicate(startup_handle(SR_RESOURCE), RIGHT_SAME, &x[nx].h);
        if (st == OK) {
            x[nx].role = SR_RESOURCE;
            xr[nx++] = dp_root_rights(p->w.rights);
        }
    }
    if (st != OK) {
        for (unsigned i = 0; i < nx; i++)
            jam_handle_close(x[i].h);
        if (*sub)
            close_if(sub);
        return st;
    }
    struct spawn_args a = {
        .path = p->path, .vmo = p->vmo, .size = p->size, .argc = argc, .argv = argv,
        .envp = envp, .job = *sub, .extra = x, .nextra = nx, .extra_rights = xr,
        .ns = p->w.n ? grants : NULL, .stack_size = p->w.posix ? POSIX_STACK : 0,
        .before_start = relay, .before_start_ctx = &p->handover,
    };
    st = spawn(&a, proc);   /* the extras are its whatever happens */
    if (st != OK)
        close_if(sub);   /* empty: a process that never started dies with its handle */
    return st == ERR_IO ? ERR_ACCESS_DENIED : st;   /* a page read at the start isn't approved */
}

static status_t op_start(void *ctx, uint16_t argc, const uint8_t *strings, uint32_t strings_len,
                         handle_t job, handle_t signals, handle_t *out_proc, handle_t *out_job)
{
    struct prog *p = &progs[*(const int *)ctx];
    static const char *argv[STRINGS_MAX + 1], *envp[STRINGS_MAX + 1];   /* the worker's */
    handle_t proc = HANDLE_INVALID, sub = HANDLE_INVALID;
    p->done = true;   /* one start, whatever comes of it */
    status_t st = kind_ok(job, OBJ_JOB, RIGHT_WRITE) && kind_ok(signals, OBJ_CHANNEL, 0) &&
                  split_strings(argc, strings, strings_len, argv, envp) ? OK : ERR_INVALID_ARGS;
    if (st == OK) {
        st = start_child(p, argv, argc, envp, job, signals, &proc, &sub);
        signals = HANDLE_INVALID;   /* spawn's, or closed there */
    }
    close_if(&job);
    close_if(&signals);
    if (st == OK && (st = jam_handle_duplicate(proc, PROC_RIGHTS, out_proc)) == OK &&
        (st = jam_handle_duplicate(sub, JOB_GIVEN, out_job)) != OK)
        close_if(out_proc);
    if (st != OK && sub)
        jam_job_kill(sub);   /* started, but the caller can't have it */
    close_if(&proc);
    close_if(&sub);
    if (st != OK)
        printf("init: exec: %s didn't start (%s)\n", p->path, status_str(st));
    return st;
}

static const struct exec_prog_ops prog_ops = { .start = op_start };

/* ---- the channels ------------------------------------------------------------------ */

static void opener_close(unsigned i)
{
    (void)jam_port_unbind(port, openers[i], K_OPENER + i);
    close_if(&openers[i]);
    for (unsigned k = 0; k < PROGRAMS; k++)
        if (progs[k].opener == (int)i)
            progs[k].opener = -1;   /* its programs wait on by themselves */
}

static status_t on_connect(void *ctx, handle_t *out)
{
    (void)ctx;
    unsigned i = 0;
    while (i < OPENERS && openers[i])
        i++;
    if (i == OPENERS)
        return ERR_NO_RESOURCES;
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK)
        return st;
    st = jam_port_bind(port, mine, K_OPENER + i, SIG_READABLE | SIG_PEER_CLOSED,
                       PORT_BIND_PERSISTENT);
    if (st != OK) {
        jam_handle_close(mine);
        jam_handle_close(theirs);
        return st;
    }
    openers[i] = mine;
    *out = theirs;
    return OK;
}

/* The shared channel speaks only svc.connect. */
static uint32_t svc_only(void *ctx, const void *req, uint32_t n, void *rep, handle_t *rhs,
                         uint32_t *rhn)
{
    static const struct svc_ops none;
    return svc_dispatch(&none, ctx, req, n, rep, rhs, rhn);
}

/* Look at key's channel again soon: it may hold more than a round. */
static void again(uint64_t key)
{
    struct port_packet pkt = { .key = key, .type = PORT_PACKET_USER };
    (void)jam_port_queue(port, &pkt);   /* a full port: served with its next request */
}

static void shared_requests(void)
{
    status_t st = OK;
    for (unsigned n = 0; st == OK && n < ROUND; n++)
        st = svc_serve_request(shared_srv, svc_only, on_connect, NULL);
    if (st == OK)
        again(K_SHARED);
}

static void opener_requests(unsigned i)
{
    status_t st = OK;
    for (unsigned n = 0; st == OK && openers[i] && n < ROUND; n++)
        st = exec_unread_full(openers[i]) ? ERR_BAD_STATE
                                          : exec_serve_one(openers[i], &open_ops, &opener_ids[i]);
    if (st == OK && openers[i])
        again(K_OPENER + i);
    else if (st != ERR_SHOULD_WAIT && openers[i])
        opener_close(i);   /* it closed its end, or left its answers unread */
}

static void prog_requests(unsigned i)
{
    struct prog *p = &progs[i];
    status_t st = OK;
    for (unsigned n = 0; st == OK && p->ch && !p->done && n < ROUND; n++)
        st = exec_prog_unread_full(p->ch) ? ERR_BAD_STATE
                                          : exec_prog_serve_one(p->ch, &prog_ops, &prog_ids[i]);
    if (p->done || (st != OK && st != ERR_SHOULD_WAIT))
        prog_free(p);   /* started (or not: one try), dropped by the caller, or broken */
    else if (st == OK && p->ch)
        again(K_PROG + i);
}

static void worker_main(void *arg)
{
    (void)arg;
    for (;;) {
        struct port_packet pkt;
        status_t st = jam_port_wait(port, next_expiry(), &pkt);
        if (st == ERR_TIMED_OUT) {
            expire(now());
            continue;
        }
        if (st != OK) {
            jam_sleep_until(now() + 10 * NS_PER_MS);   /* never: the port is ours alone */
            continue;
        }
        if (pkt.key == K_SHARED)
            shared_requests();
        else if (pkt.key >= K_OPENER && pkt.key < K_OPENER + OPENERS && openers[pkt.key - K_OPENER])
            opener_requests((unsigned)(pkt.key - K_OPENER));
        else if (pkt.key >= K_PROG && pkt.key < K_PROG + PROGRAMS && progs[pkt.key - K_PROG].ch)
            prog_requests((unsigned)(pkt.key - K_PROG));
        expire(now());
    }
}

status_t exec_init(handle_t *client)
{
    handle_t cli = HANDLE_INVALID, t;
    for (unsigned i = 0; i < OPENERS; i++)
        opener_ids[i] = (int)i;
    for (unsigned i = 0; i < PROGRAMS; i++) {
        prog_ids[i] = (int)i;
        progs[i].opener = -1;
    }
    status_t st = jam_port_create(&port);
    if (st == OK)
        st = jam_channel_create(&cli, &shared_srv);
    if (st == OK)
        st = jam_port_bind(port, shared_srv, K_SHARED, SIG_READABLE, PORT_BIND_PERSISTENT);
    if (st == OK)
        st = thread_spawn("exec", worker_main, NULL, worker_stack, sizeof(worker_stack), &t);
    if (st != OK) {
        printf("init: no /svc/exec: no programs from /data for other programs this boot (%s)\n",
               status_str(st));
        close_if(&cli);
        close_if(&shared_srv);
        close_if(&port);
        return st;
    }
    jam_handle_close(t);   /* it runs for as long as init does */
    *client = cli;
    return OK;
}
