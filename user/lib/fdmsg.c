/* The POSIX hand-over (<posixfds.h>): its writer, for a starter (spawn's
 * spawn_args.handover, the shell, the POSIX layer's posix_spawn), and its
 * reader, for the child's crt.
 *
 * Both sides check a message with the same functions (check_msg and the
 * ones it calls), so what the writer sends is exactly what the reader
 * takes. The reader checks a whole message, its handles' object types
 * included, before it gives the sink any descriptor of it, and on any
 * failure drops the whole hand-over: what the sink was given (reset),
 * the message's handles and every message still queued.
 *
 * One message buffer for both, the most a channel message holds (so no
 * message can be too big to read and drop): static, under buf_lock, held
 * from the first message to the last. Its pages are committed only as
 * they are touched. No heap, no printf, no thread-local storage: the
 * reader runs before a POSIX program's C library does. */
#include <os.h>
#include <posixfds.h>
#include "fork.h"

_Static_assert(sizeof(struct posixfds_state) == 32, "posixfds_state has no padding");
_Static_assert(sizeof(struct posixfds_msg) == 64, "posixfds_msg has no padding");
_Static_assert(sizeof(struct posixfds_fd) == 24, "posixfds_fd has no padding");
_Static_assert(POSIXFDS_MSG_MAX <= CHANNEL_MAX_BYTES, "a message fits a channel message");

#define PAD8(n) (((n) + 7u) & ~7u)

/* What a kind is (posixfds.h's enum). */
enum data_rule { DATA_ANY, DATA_PATH, DATA_NAME, DATA_FD };
static const struct {
    uint8_t  nhandles;                        /* exactly this many */
    uint8_t  data;                            /* enum data_rule */
    bool     offset;                          /* may have an offset */
    uint32_t types[POSIXFDS_KIND_HANDLES];    /* each handle's OBJ_* */
} kinds[POSIXFDS_KIND_COUNT] = {
    [POSIXFDS_FILE] = { 2, DATA_ANY, true, { OBJ_CHANNEL, OBJ_VMO } },
    [POSIXFDS_DIR] = { 0, DATA_PATH, true, { 0 } },
    [POSIXFDS_PIPE] = { 1, DATA_ANY, false, { OBJ_CHANNEL } },
    [POSIXFDS_SOCKET] = { 4, DATA_ANY, false, { OBJ_CHANNEL, OBJ_VMO, OBJ_EVENT, OBJ_EVENT } },
    [POSIXFDS_LISTENER] = { 1, DATA_ANY, false, { OBJ_CHANNEL } },
    [POSIXFDS_TTY] = { 2, DATA_ANY, false, { OBJ_CHANNEL, OBJ_CHANNEL } },
    [POSIXFDS_PTY_MASTER] = { 2, DATA_ANY, false, { OBJ_CHANNEL, OBJ_CHANNEL } },
    [POSIXFDS_DEV] = { 0, DATA_NAME, false, { 0 } },
    [POSIXFDS_ETC] = { 0, DATA_PATH, false, { 0 } },
    [POSIXFDS_MEMFD] = { 1, DATA_ANY, false, { OBJ_VMO } },
    [POSIXFDS_EVENTFD] = { 1, DATA_ANY, false, { OBJ_EVENT } },
    [POSIXFDS_DUP] = { 0, DATA_FD, false, { 0 } },
    [POSIXFDS_SEQPACKET] = { 1, DATA_ANY, false, { OBJ_CHANNEL } },
};

static lock_t   buf_lock;                              /* guards everything below */
static _Alignas(8) uint8_t buf[POSIXFDS_MSG_MAX];      /* the message being built or read */
static handle_t hbuf[CHANNEL_MAX_HANDLES];             /* ... its handles */
static uint64_t seen[POSIXFDS_FD_MAX / 64];            /* descriptors already in the hand-over */

/* A forked child (fork.c): a spawn another thread of its parent was
 * writing is no spawn of the child's. */
void fdmsg_forked(void)
{
    buf_lock = (lock_t){ 0 };
}

int posixfds_kind_handles(uint32_t kind, uint32_t types[POSIXFDS_KIND_HANDLES])
{
    if (kind == 0 || kind >= POSIXFDS_KIND_COUNT)
        return -1;
    for (unsigned i = 0; types && i < POSIXFDS_KIND_HANDLES; i++)
        types[i] = kinds[kind].types[i];
    return kinds[kind].nhandles;
}

/* ---- the checks, both sides' ---------------------------------------------------- */

/* n bytes at p from `from` on are zero (a field's padding). */
static bool zeros(const uint8_t *p, uint32_t from, uint32_t n)
{
    for (uint32_t i = from; i < n; i++)
        if (p[i])
            return false;
    return true;
}

/* A string of len bytes (its NUL last, none before), at least 2. */
static bool string_ok(const char *s, uint32_t len)
{
    return len >= 2 && s[len - 1] == '\0' && strnlen(s, len) == len - 1;
}

static bool state_ok(const struct posixfds_state *s)
{
    return s->umask <= POSIXFDS_UMASK && s->ppid < POSIXFDS_PID_MAX &&
           s->pgid < POSIXFDS_PID_MAX && s->sid < POSIXFDS_PID_MAX &&
           !(s->sigmask & POSIXFDS_SIG_NEVER) && !(s->sigignore & POSIXFDS_SIG_NEVER);
}

static bool state_zero(const struct posixfds_state *s)
{
    return !s->umask && !s->ppid && !s->pgid && !s->sid && !s->sigmask && !s->sigignore;
}

/* A DUP record's data: the number of a descriptor already in the
 * hand-over (so not its own), and the record has no status of its own. */
static bool dup_ok(const struct posixfds_fd *r, const uint8_t *data)
{
    uint32_t target;
    if (r->data_len != sizeof(target) || r->status)
        return false;
    memcpy(&target, data, sizeof(target));
    return target < POSIXFDS_FD_MAX && (seen[target / 64] & 1ull << (target % 64));
}

/* A record and its data (data_len bytes, checked to be there); its fd is
 * marked in `seen`. */
static bool record_ok(const struct posixfds_fd *r, const uint8_t *data)
{
    if (r->fd < 0 || r->fd >= POSIXFDS_FD_MAX || r->kind == 0 || r->kind >= POSIXFDS_KIND_COUNT)
        return false;
    uint64_t bit = 1ull << (r->fd % 64);
    if (seen[r->fd / 64] & bit)
        return false;   /* a descriptor twice */
    if (kinds[r->kind].data == DATA_FD && !dup_ok(r, data))
        return false;
    seen[r->fd / 64] |= bit;
    if (r->nhandles != kinds[r->kind].nhandles || (r->status & ~POSIXFDS_STATUS) ||
        (r->status & POSIXFDS_O_ACCMODE) == POSIXFDS_O_ACCMODE ||
        (r->offset && !kinds[r->kind].offset) || r->data_len > POSIXFDS_DATA_MAX)
        return false;
    const char *s = (const char *)data;
    switch (kinds[r->kind].data) {
    case DATA_PATH:
        return string_ok(s, r->data_len) && s[0] == '/';
    case DATA_NAME:
        return string_ok(s, r->data_len) && !strchr(s, '/');
    }
    return true;
}

/* The handles a record names are of its kind's object types. */
static bool types_ok(uint32_t kind, const handle_t *hs)
{
    for (unsigned i = 0; i < kinds[kind].nhandles; i++) {
        struct handle_info info;
        if (jam_handle_info(hs[i], &info) != OK || info.type != kinds[kind].types[i])
            return false;
    }
    return true;
}

/* An absolute path of len bytes (its NUL included) at *at in buf (n
 * bytes), zero-padded; *at moves past it. */
static bool path_ok(uint32_t len, uint32_t n, uint32_t *at)
{
    if (len < 2 || len > POSIXFDS_PATH_MAX || len > n - *at)
        return false;
    const char *s = (const char *)buf + *at;
    uint32_t padded = PAD8(len);
    if (!string_ok(s, len) || s[0] != '/' || padded > n - *at || !zeros(buf + *at, len, padded))
        return false;
    *at += padded;
    return true;
}

/* The header of message seq (n bytes in buf, nh handles); *at: where the
 * records start. */
static bool header_ok(const struct posixfds_msg *m, uint32_t n, uint32_t nh, uint16_t seq,
                      uint32_t *at)
{
    uint16_t may = (uint16_t)(POSIXFDS_LAST | (seq ? 0u : POSIXFDS_SECURE));
    if (n < sizeof(*m) || m->txid || m->magic != POSIXFDS_MAGIC ||
        m->version != POSIXFDS_VERSION || (m->flags & ~may) || m->seq != seq ||
        m->nhandles != nh || m->reserved)
        return false;
    *at = sizeof(*m);
    if (seq)
        return !m->cwd_len && !m->exe_len && state_zero(&m->state) && m->nfds;
    return path_ok(m->cwd_len, n, at) && (!m->exe_len || path_ok(m->exe_len, n, at)) &&
           state_ok(&m->state);
}

/* The message in buf (n bytes) and hbuf (nh handles) is message seq of a
 * hand-over, whole and exact; types: check the handles' object types. */
static bool check_msg(uint32_t n, uint32_t nh, uint16_t seq, bool types)
{
    struct posixfds_msg m;
    uint32_t at, h = 0;
    if (n < sizeof(m))
        return false;
    memcpy(&m, buf, sizeof(m));
    if (!header_ok(&m, n, nh, seq, &at))
        return false;
    for (unsigned i = 0; i < m.nfds; i++) {
        struct posixfds_fd r;
        if (n - at < sizeof(r))
            return false;
        memcpy(&r, buf + at, sizeof(r));
        at += sizeof(r);
        uint32_t padded = PAD8(r.data_len);
        if (r.data_len > POSIXFDS_DATA_MAX || padded > n - at || !record_ok(&r, buf + at) ||
            !zeros(buf + at, r.data_len, padded) || r.nhandles > nh - h ||
            (types && !types_ok(r.kind, &hbuf[h])))
            return false;
        at += padded;
        h += r.nhandles;
    }
    return at == n && h == nh;
}

/* ---- the writer ------------------------------------------------------------------- */

void posixfds_discard(const struct posixfds_out *out)
{
    for (unsigned i = 0; out && i < out->nfds; i++) {
        int k = posixfds_kind_handles(out->fds[i].kind, NULL);
        for (int j = 0; k > 0 && j < k && out->fds[i].handles; j++)
            jam_handle_close(out->fds[i].handles[j]);
    }
}

/* A string s onto the message in buf (*n bytes), its NUL and padding too;
 * its length with the NUL. */
static uint32_t add_string(const char *s, uint32_t *n)
{
    uint32_t len = (uint32_t)strlen(s) + 1;
    memcpy(buf + *n, s, len);
    memset(buf + *n + len, 0, PAD8(len) - len);
    *n += PAD8(len);
    return len;
}

/* Start message seq in buf: its header (the first message's state,
 * directory and program's path too); *n: its length so far. */
static void begin_msg(const struct posixfds_out *out, uint16_t seq, uint32_t *n)
{
    struct posixfds_msg m = { .magic = POSIXFDS_MAGIC, .version = POSIXFDS_VERSION, .seq = seq };
    *n = sizeof(m);
    if (!seq) {
        m.flags = out->secure ? (uint16_t)POSIXFDS_SECURE : 0;
        m.state = out->state;
        m.cwd_len = add_string(out->cwd, n);
        if (out->exe)
            m.exe_len = add_string(out->exe, n);
    }
    memcpy(buf, &m, sizeof(m));
}

/* Descriptor d's record, data and handles onto the message in buf (*n
 * bytes, *nh handles). */
static void add_record(const struct posixfds_desc *d, uint32_t *n, uint32_t *nh)
{
    struct posixfds_fd r = {
        .fd = d->fd, .kind = d->kind, .nhandles = (uint16_t)kinds[d->kind].nhandles,
        .status = d->status, .data_len = d->data_len, .offset = d->offset,
    };
    struct posixfds_msg *m = (struct posixfds_msg *)buf;
    memcpy(buf + *n, &r, sizeof(r));
    *n += sizeof(r);
    if (d->data_len)
        memcpy(buf + *n, d->data, d->data_len);
    memset(buf + *n + d->data_len, 0, PAD8(d->data_len) - d->data_len);
    *n += PAD8(d->data_len);
    for (unsigned j = 0; j < r.nhandles; j++)
        hbuf[(*nh)++] = d->handles[j];
    m->nfds++;
    m->nhandles = *nh;
}

/* The descriptors' shapes before anything is built: the kinds, sizes and
 * handles (the full rules are checked on each built message). */
static status_t descs_ok(const struct posixfds_out *out)
{
    if (out->nfds > POSIXFDS_FD_MAX)
        return ERR_OUT_OF_RANGE;
    if (!out->cwd || strnlen(out->cwd, POSIXFDS_PATH_MAX) == POSIXFDS_PATH_MAX ||
        (out->exe && strnlen(out->exe, POSIXFDS_PATH_MAX) == POSIXFDS_PATH_MAX))
        return ERR_INVALID_ARGS;
    for (unsigned i = 0; i < out->nfds; i++) {
        const struct posixfds_desc *d = &out->fds[i];
        int k = posixfds_kind_handles(d->kind, NULL);
        if (k < 0 || d->data_len > POSIXFDS_DATA_MAX || (d->data_len && !d->data) ||
            (k && !d->handles))
            return ERR_INVALID_ARGS;
        for (int j = 0; j < k; j++)
            if (d->handles[j] == HANDLE_INVALID)
                return ERR_INVALID_ARGS;
    }
    return OK;
}

/* Check and send the message in buf (n bytes, nh handles in hbuf; last:
 * mark it). A failed check closes the handles: the write's own rule. */
static status_t send_msg(handle_t ch, uint32_t n, uint32_t nh, bool last)
{
    struct posixfds_msg *m = (struct posixfds_msg *)buf;
    if (last)
        m->flags |= POSIXFDS_LAST;
    if (!check_msg(n, nh, m->seq, true)) {
        for (uint32_t i = 0; i < nh; i++)
            jam_handle_close(hbuf[i]);
        return ERR_INVALID_ARGS;
    }
    return jam_channel_write(ch, buf, n, hbuf, nh);
}

/* Build and send every message. After a failure, the descriptors from
 * *next on were in no message (their handles are still to close). */
static status_t write_all(handle_t ch, const struct posixfds_out *out, unsigned *next)
{
    uint32_t n, nh = 0;
    uint16_t seq = 0;
    begin_msg(out, seq, &n);
    for (*next = 0; *next < out->nfds; (*next)++) {
        const struct posixfds_desc *d = &out->fds[*next];
        uint32_t size = sizeof(struct posixfds_fd) + PAD8(d->data_len);
        unsigned k = kinds[d->kind].nhandles;
        if (((struct posixfds_msg *)buf)->nfds &&
            (size > POSIXFDS_MSG_MAX - n || k > CHANNEL_MAX_HANDLES - nh)) {
            status_t st = send_msg(ch, n, nh, false);
            if (st != OK)
                return st;
            begin_msg(out, ++seq, &n);
            nh = 0;
        }
        add_record(d, &n, &nh);
    }
    return send_msg(ch, n, nh, true);
}

status_t posixfds_write(handle_t ch, const struct posixfds_out *out)
{
    status_t st = descs_ok(out);
    if (st != OK) {
        posixfds_discard(out);
        return st;
    }
    lock_take(&buf_lock);
    memset(seen, 0, sizeof(seen));
    unsigned next = 0;
    st = write_all(ch, out, &next);
    lock_give(&buf_lock);
    /* The descriptors in the message that failed went with it (sent or
     * closed); the ones from `next` on were never added to one. */
    if (st != OK && next < out->nfds) {
        struct posixfds_out rest = *out;
        rest.fds = out->fds + next;
        rest.nfds = out->nfds - next;
        posixfds_discard(&rest);
    }
    return st;
}

/* ---- the reader ------------------------------------------------------------------- */

/* The next message on ch, without waiting, into buf and hbuf. */
static status_t read_msg(handle_t ch, uint32_t *n, uint32_t *nh)
{
    struct channel_read_args a = {
        .h = ch,
        .rd_cap = sizeof(buf),
        .rd_bytes = (uint64_t)(uintptr_t)buf,
        .rd_actual = (uint64_t)(uintptr_t)n,
        .rd_handles = (uint64_t)(uintptr_t)hbuf,
        .rd_hcap = CHANNEL_MAX_HANDLES,
        .rd_hactual = (uint64_t)(uintptr_t)nh,
    };
    *n = *nh = 0;
    return jam_channel_read(&a);
}

/* Read and drop what is queued on ch now, handles and all (bounded: a
 * starter still writing can't keep us here). */
static void drain(handle_t ch)
{
    for (unsigned i = 0; i < CHANNEL_MAX_QUEUED; i++) {
        uint32_t n, nh;
        if (read_msg(ch, &n, &nh) != OK)
            return;
        for (uint32_t j = 0; j < nh; j++)
            jam_handle_close(hbuf[j]);
    }
}

/* Nothing is queued on ch after the last message: the starter wrote the
 * whole hand-over before we started, so anything more is part of a
 * malformed one. */
static bool nothing_after(handle_t ch)
{
    struct handle_info info;
    return jam_handle_info(ch, &info) == OK && info.queued == 0;
}

/* Give the checked message in buf to the sink, record by record. A
 * refusal closes the handles of the records after it. */
static status_t deliver(const struct posixfds_sink *sink)
{
    const struct posixfds_msg *m = (const struct posixfds_msg *)buf;
    uint32_t at = sizeof(*m) + PAD8(m->cwd_len) + PAD8(m->exe_len), h = 0;
    status_t st = OK;
    for (unsigned i = 0; i < m->nfds; i++) {
        struct posixfds_fd r;
        memcpy(&r, buf + at, sizeof(r));
        at += sizeof(r);
        if (st == OK)
            st = sink->fd(sink->ctx, &r, buf + at, &hbuf[h]);
        else
            for (unsigned j = 0; j < r.nhandles; j++)
                jam_handle_close(hbuf[h + j]);
        at += PAD8(r.data_len);
        h += r.nhandles;
    }
    return st;   /* every handle went with a record: check_msg */
}

/* The messages from the first to the last, each checked then delivered;
 * the first's state, directory, program's path (empty: none) and secure
 * mark into *state, cwd, exe and *secure. */
static status_t read_all(handle_t ch, const struct posixfds_sink *sink,
                         struct posixfds_state *state, char *cwd, char *exe, bool *secure,
                         bool *given)
{
    for (uint16_t seq = 0;; seq++) {
        uint32_t n, nh;
        status_t st = read_msg(ch, &n, &nh);
        if (seq == 0 && (st == ERR_SHOULD_WAIT || st == ERR_PEER_CLOSED))
            return ERR_NOT_FOUND;
        if (st != OK)
            return ERR_INVALID_ARGS;   /* cut short */
        const struct posixfds_msg *m = (const struct posixfds_msg *)buf;
        bool last = m->flags & POSIXFDS_LAST;
        if (!check_msg(n, nh, seq, true) || (last && !nothing_after(ch))) {
            for (uint32_t j = 0; j < nh; j++)
                jam_handle_close(hbuf[j]);
            return ERR_INVALID_ARGS;
        }
        if (seq == 0) {
            *state = m->state;
            *secure = m->flags & POSIXFDS_SECURE;
            memcpy(cwd, buf + sizeof(*m), m->cwd_len);
            exe[0] = '\0';
            if (m->exe_len)
                memcpy(exe, buf + sizeof(*m) + PAD8(m->cwd_len), m->exe_len);
        }
        *given |= m->nfds > 0;
        st = deliver(sink);
        if (st != OK || last)
            return st;
        if (seq == UINT16_MAX)
            return ERR_INVALID_ARGS;
    }
}

status_t posixfds_read(handle_t ch, const struct posixfds_sink *sink,
                       struct posixfds_state *state, char cwd[POSIXFDS_PATH_MAX])
{
    struct posixfds_state s;
    static char dir[POSIXFDS_PATH_MAX], exe[POSIXFDS_PATH_MAX];   /* under buf_lock */
    bool given = false, secure = false;
    lock_take(&buf_lock);
    memset(seen, 0, sizeof(seen));
    status_t st = read_all(ch, sink, &s, dir, exe, &secure, &given);
    if (st != OK && st != ERR_NOT_FOUND) {
        if (given)
            sink->reset(sink->ctx);
        drain(ch);
    }
    if (st == OK) {
        *state = s;
        memcpy(cwd, dir, strnlen(dir, POSIXFDS_PATH_MAX - 1) + 1);
        if (exe[0] && sink->exe)
            sink->exe(sink->ctx, exe);
        if (secure && sink->secure)
            sink->secure(sink->ctx);
    }
    lock_give(&buf_lock);
    return st;
}
