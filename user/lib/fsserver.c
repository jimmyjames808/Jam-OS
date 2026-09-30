/* fsserver: the loop an `fs` service runs (<fsserver.h>).
 *
 * Every channel is bound to the port for SIG_READABLE | SIG_PEER_CLOSED,
 * persistently: a packet arrives when its queue goes from empty to not
 * empty, so a round reads until the queue is empty again. A round that
 * stops early (FSSERVER_ROUND) queues a packet for itself, which puts the
 * rest of that channel behind whatever else is waiting. A packet for a slot
 * that has been closed (or reused) since finds nothing to read and is
 * harmless. */
#include <fsserver.h>

/* Port keys: an fs channel's slot, or FILE_KEY + a file's slot. */
#define FILE_KEY 0x100u

/* The file channel's client end keeps what a client needs and can pass on. */
#define BUF_RIGHTS (RIGHT_TRANSFER | RIGHT_READ | RIGHT_MAP)

status_t fsserver_init(struct fsserver *s)
{
    return jam_port_create(&s->port);
}

/* Ask for another round on the channel with this key. */
static void again(struct fsserver *s, uint64_t key)
{
    struct port_packet pkt = { .key = key, .type = PORT_PACKET_USER };
    /* A full port: the channel waits for its next message instead. */
    (void)jam_port_queue(s->port, &pkt);
}

static status_t watch(struct fsserver *s, handle_t ch, uint64_t key)
{
    status_t st = jam_port_bind(s->port, ch, key, SIG_READABLE | SIG_PEER_CLOSED,
                                PORT_BIND_PERSISTENT);
    if (st == OK)
        again(s, key);   /* what was queued before the binding existed */
    return st;
}

status_t fsserver_add_fs(struct fsserver *s, handle_t ch)
{
    status_t st = ERR_NO_RESOURCES;
    for (unsigned i = 0; i < FSSERVER_MAX_FS; i++) {
        if (s->fs[i])
            continue;
        st = watch(s, ch, i);
        if (st != OK)
            break;
        s->fs[i] = ch;
        return OK;
    }
    jam_handle_close(ch);
    return st;
}

status_t fsserver_open(struct fsserver *s, void *ctx, bool writable, handle_t *client,
                       handle_t *client_buf, struct fsserver_file **out)
{
    struct fsserver_file *f = NULL;
    for (unsigned i = 0; i < FSSERVER_MAX_FILES && !f; i++)
        if (!s->files[i].ch)
            f = &s->files[i];
    if (!f)
        return ERR_NO_RESOURCES;
    handle_t mine = HANDLE_INVALID, theirs = HANDLE_INVALID, buf = HANDLE_INVALID;
    handle_t their_buf = HANDLE_INVALID;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st == OK)
        st = jam_vmo_create(FSSERVER_BUF_SIZE, 0, HANDLE_INVALID, &buf);
    if (st == OK)
        st = jam_handle_duplicate(buf, BUF_RIGHTS | (writable ? RIGHT_WRITE : 0), &their_buf);
    if (st == OK)
        st = watch(s, mine, FILE_KEY + (uint64_t)(f - s->files));
    if (st != OK) {
        handle_t made[] = { mine, theirs, buf, their_buf };
        for (unsigned i = 0; i < sizeof(made) / sizeof(made[0]); i++)
            if (made[i])
                jam_handle_close(made[i]);
        return st;
    }
    f->ch = mine;
    f->buf = buf;
    f->ctx = ctx;
    *client = theirs;
    *client_buf = their_buf;
    *out = f;
    return OK;
}

static void close_file(struct fsserver *s, struct fsserver_file *f)
{
    if (s->closed)
        s->closed(f);
    jam_port_unbind(s->port, f->ch, FILE_KEY + (uint64_t)(f - s->files));
    jam_handle_close(f->ch);
    jam_handle_close(f->buf);
    memset(f, 0, sizeof(*f));
}

/* One round on an fs channel. A client that is gone, or a channel that
 * fails, is dropped. */
static void serve_fs(struct fsserver *s, unsigned i)
{
    status_t st = OK;
    for (unsigned n = 0; st == OK && s->fs[i] && n < FSSERVER_ROUND; n++)
        st = fs_serve_one(s->fs[i], s->fs_ops, s);
    if (st == OK && s->fs[i]) {
        again(s, i);
    } else if (st != ERR_SHOULD_WAIT && s->fs[i]) {
        jam_port_unbind(s->port, s->fs[i], i);
        jam_handle_close(s->fs[i]);
        s->fs[i] = HANDLE_INVALID;
    }
}

static void serve_file(struct fsserver *s, unsigned i)
{
    struct fsserver_file *f = &s->files[i];
    status_t st = OK;
    for (unsigned n = 0; st == OK && f->ch && n < FSSERVER_ROUND; n++)
        st = file_serve_one(f->ch, s->file_ops, f);
    if (st == OK && f->ch)
        again(s, FILE_KEY + i);
    else if (st != ERR_SHOULD_WAIT && f->ch)
        close_file(s, f);
}

static bool in_use(const struct fsserver *s)
{
    for (unsigned i = 0; i < FSSERVER_MAX_FS; i++)
        if (s->fs[i])
            return true;
    for (unsigned i = 0; i < FSSERVER_MAX_FILES; i++)
        if (s->files[i].ch)
            return true;
    return false;
}

status_t fsserver_run(struct fsserver *s)
{
    while (in_use(s)) {
        struct port_packet pkt;
        status_t st = jam_port_wait(s->port, DEADLINE_NEVER, &pkt);
        if (st != OK)
            return st;
        if (pkt.key < FSSERVER_MAX_FS)
            serve_fs(s, (unsigned)pkt.key);
        else if (pkt.key >= FILE_KEY && pkt.key < FILE_KEY + FSSERVER_MAX_FILES)
            serve_file(s, (unsigned)(pkt.key - FILE_KEY));
    }
    return OK;
}
