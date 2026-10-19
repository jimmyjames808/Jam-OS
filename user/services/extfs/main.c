/* extfs: the ext filesystem service (docs/M13.6-PLAN.md's U4). One process
 * serves one partition: it takes a `block` channel, mounts the ext volume
 * on it with the ext library (the journal replayed first: the recovery)
 * and serves the `fs` protocol, plus a `file` channel per open file and,
 * given the mount's pager, the pages of the files it maps (pager.c).
 * <extfssvc.h> has the startup handles, extfs.h the map of the files.
 *
 * Mounting: a volume the library may write (Jam OS's feature set, a
 * journal, no errors recorded) is served read-write unless the block
 * channel is read-only; anything else it can read is served read-only,
 * and the log says why. Formatting is off unless extfs was started with
 * EXTFS_ARG_FORMAT; with it a writable partition whose first 4 KiB are
 * all zeros is made an ext volume. Nothing else ever is.
 *
 * A restart: every start mounts from the disk (the replay is the
 * recovery); one started with a dead instance's state carries on from it
 * (adopt.c), its clients seeing nothing but a slower answer.
 *
 * Exit: 0 once there is nothing left to serve (the fs channel's client
 * closed it, fsctl.stop, or the disk went away), after every file is
 * closed and the journal closed (the volume clean); 1 when the volume
 * can't be served but might be later; EXTFS_EXIT_NO_VOLUME when the
 * partition holds nothing extfs can serve; EXTFS_EXIT_TEST a test power. */
#include <wallclock.h>
#include "extfs.h"

#define CACHE_BYTES  (2u << 20)        /* the block cache */
#define NAMES        16384u            /* names the directory hash holds */
#define REVOKES      16384u            /* revoke records a replay may hold */
#define JNL_MEM_MAX  (8u << 20)        /* the most the journal's region may take */
#define MKFS_WORK    (64u << 10)
#define IDLE_NS      NS_PER_S          /* the journal's tail moves after a second idle */
#define LABEL        "jam"

_Static_assert(EXTFS_SR_SERVE == SR_DRIVER(DR_SERVE), "extfs serves on a driver's DR_SERVE role");

struct xf_vol vol;

static struct ext_dev dev;

/* ---- the mount --------------------------------------------------------------------- */

/* The superblock's block size, read before the cache is made for it. */
static status_t block_size_of(uint32_t *out)
{
    struct ext_super sb;
    status_t st = dev.read(NULL, 1024, sizeof(sb), &sb);
    if (st != OK)
        return st;
    if (sb.magic != 0xef53 || sb.log_block_size > 6) {
        *out = 0;
        return OK;
    }
    *out = 1024u << sb.log_block_size;
    return OK;
}

static status_t format(void)
{
    struct ext_mkfs p = { .uid = FS_ONE_USER, .gid = FS_ONE_USER, .label = LABEL,
                          .flags = vol.ram ? EXT_MKFS_ZEROED : 0 };   /* a new VMO is zeros */
    struct ext_time t;
    time_now(&t);
    p.now = t.sec;
    if (test_fixed_time())
        memcpy(p.uuid, "jam-os-fixed-ext", 16);
    else
        os_random(p.uuid, sizeof(p.uuid));
    void *work = malloc(MKFS_WORK);
    if (!work)
        return ERR_NO_MEMORY;
    printf("extfs %s: the partition is blank: making an ext volume\n", vol.name);
    status_t st = ext_mkfs(&dev, &p, work, MKFS_WORK);
    free(work);
    if (st != OK)
        printf("extfs %s: making the volume failed: %s\n", vol.name, status_str(st));
    return st;
}

/* No ext volume: format it if asked and blank, else nothing to serve. */
static status_t mount_blank(bool *no_volume)
{
    bool blank = false;
    if (!vol.may_format || vol.read_only) {
        printf("extfs %s: no ext volume (%s: nothing written)\n", vol.name,
               vol.read_only ? "read-only" : "formatting is off");
        *no_volume = true;
        return ERR_NOT_FOUND;
    }
    status_t st = disk_is_blank(&blank);
    if (st == OK && !blank) {
        printf("extfs %s: no ext volume, and the partition is not blank: not formatting it\n",
               vol.name);
        *no_volume = true;
        return ERR_NOT_FOUND;
    }
    return st == OK ? format() : st;
}

/* Why a mounted volume is only read, for the log. */
static const char *ro_why(uint32_t ro)
{
    if (ro & EXT_RO_ASKED)
        return "the partition is read-only";
    if (ro & EXT_RO_ERRORS)
        return "errors are recorded on it (e2fsck clears them)";
    if (ro & EXT_RO_DAMAGED)
        return "damage found while mounting";
    if (ro & EXT_RO_RECOVER)
        return "its journal needs replaying";
    if (ro & EXT_RO_FEATURES)
        return "it has features read here but not written";
    if (ro & EXT_RO_LACKS)
        return "it lacks a feature Jam OS writes with";
    return "?";
}

status_t mount(bool *no_volume)
{
    static void *cache, *jmem, *names;
    static size_t cache_size, jsize;
    struct ext_fs *fs = &vol.fs;
    uint32_t bs = 0;
    status_t st = block_size_of(&bs);
    if (st == OK && !bs) {
        st = mount_blank(no_volume);
        if (st == OK)
            st = block_size_of(&bs);
    }
    if (st != OK)
        return st;
    if (!bs) {
        *no_volume = true;
        return ERR_NOT_FOUND;
    }
    uint32_t nbufs = CACHE_BYTES / bs < EXT_CACHE_MIN ? EXT_CACHE_MIN : CACHE_BYTES / bs;
    cache_size = ext_mem_size(nbufs, bs);
    cache = cache ? cache : malloc(cache_size);
    names = names ? names : malloc(ext_names_mem(NAMES));
    if (!cache || !names)
        return ERR_NO_MEMORY;
    uint32_t flags = vol.read_only ? EXT_MOUNT_READ_ONLY : 0;
    /* A first look (reads only): the journal's size, for its region. */
    st = ext_mount(fs, &dev, cache, cache_size, EXT_MOUNT_READ_ONLY);
    if (st != OK) {
        printf("extfs %s: can't mount: %s (%s)\n", vol.name, status_str(st),
               fs->why ? fs->why : "?");
        *no_volume = st == ERR_NOT_SUPPORTED;
        return st;
    }
    uint64_t jblocks = 1024;
    struct ext_inode j;
    if (fs->has_journal && ext_iget(fs, fs->sb.journal_inum, 0, &j) == OK)
        jblocks = ext_isize(&j) >> fs->block_log;
    jsize = ext_jnl_mem_size(bs, jblocks > UINT32_MAX ? UINT32_MAX : (uint32_t)jblocks, REVOKES);
    if (jsize > JNL_MEM_MAX) {
        printf("extfs %s: a journal of %lu blocks is bigger than extfs keeps track of: "
               "read-only\n", vol.name, (unsigned long)jblocks);
        jsize = ext_jnl_mem_size(bs, 1024, REVOKES);
        flags = EXT_MOUNT_READ_ONLY;
    }
    jmem = jmem ? jmem : malloc(jsize);
    if (!jmem)
        return ERR_NO_MEMORY;
    st = ext_jnl_init(&vol.jnl, jmem, jsize, 0);
    if (st == OK && test_lazy_tail())
        vol.jnl.tail_blocks = UINT32_MAX;
    request_init();
    if (st == OK)
        st = ext_jnl_mount(fs, &vol.jnl, &dev, cache, cache_size, flags);
    if (st != OK) {
        printf("extfs %s: can't mount: %s (%s)\n", vol.name, status_str(st),
               fs->why ? fs->why : "?");
        *no_volume = st == ERR_NOT_SUPPORTED;
        return st;
    }
    (void)ext_names_init(fs, names, ext_names_mem(NAMES));
    vol.mounted = true;
    if (writable() && tx_begin() == OK) {   /* the free counts, as a writer's mount wants */
        if (ext_count_free(fs) == OK)
            (void)tx_commit();
        else
            tx_abort();
    }
    printf("extfs %s: mounted, %lu MiB, %u-byte blocks%s%s%s", vol.name,
           (unsigned long)((fs->blocks << fs->block_log) >> 20), fs->block_size,
           writable() ? "" : ", read-only: ", writable() ? "" : ro_why(fs->ro),
           vol.jnl.replayed ? "" : "\n");
    if (vol.jnl.replayed)
        printf("; the journal: %u transaction(s), %u block(s) replayed\n",
               (unsigned)vol.jnl.replayed, (unsigned)vol.jnl.replayed_blocks);
    return OK;
}

/* ---- the loop ---------------------------------------------------------------------- */

static status_t arm_fs(handle_t serve)
{
    return jam_port_bind(vol.port, serve, XF_KEY_FS, SIG_READABLE | SIG_PEER_CLOSED,
                         PORT_BIND_ONCE);
}

static status_t arm_own(void)
{
    return vol.own ? jam_port_bind(vol.port, vol.own, XF_KEY_OWN, SIG_READABLE | SIG_PEER_CLOSED,
                                   PORT_BIND_ONCE)
                   : OK;
}

static bool own_held;

static void own_event(void)
{
    if (!vol.own)
        return;
    if (fs_unread_full(vol.own)) {
        own_held = true;
        idl_hold(&vol.held);
        return;
    }
    struct xf_chan c = { .ch = vol.own, .id = XF_CHAN_OWN, .proto = XF_PROTO_FS };
    views_whole(&c.ctx);
    files_reap();
    status_t st = serve_one(&c);
    if (st == OK || st == ERR_SHOULD_WAIT)
        st = arm_own();
    if (st != OK) {
        jam_handle_close(vol.own);
        vol.own = HANDLE_INVALID;
    }
}

static void own_unheld(void)
{
    if (!own_held)
        return;
    if (vol.own && fs_unread_full(vol.own)) {
        idl_hold(&vol.held);
        return;
    }
    own_held = false;
    if (arm_own() != OK) {
        jam_handle_close(vol.own);
        vol.own = HANDLE_INVALID;
    }
}

/* After a second with no packet, the journal's tail moves past what is
 * home, so the next start replays nothing. Once per run of commits: a
 * tail with nothing to move doesn't count them down. Mapped files' age
 * check (wback.c) wakes the loop too, without counting as a packet. */
static uint32_t idle_seen = UINT32_MAX;   /* the commits counted at the last idle */

static bool idle_due(void)
{
    return vol.fs.jnl && !vol.fs.ro && vol.jnl.since_tail && vol.jnl.since_tail != idle_seen;
}

static void idle(void)
{
    if (idle_due())
        (void)ext_jnl_checkpoint(&vol.fs);
    idle_seen = vol.jnl.since_tail;
}

/* The order rule (docs/INOTIFY-PLAN.md 2.3): a client's close comes to the
 * port as a packet queued by the close itself, but a request it sends next
 * on another channel may be served first (its channel's packet was queued
 * earlier, or the close folded into an earlier packet of the file's, which
 * keeps that packet's signals: utest port_order). While anything is
 * watched, before a file's request or a watcher's (the fs ones reap every
 * closed file already: files_reap), every packet on the port is taken and
 * kept in order, the closed files among them, judged by their channels'
 * own signals, are handled first, and the loop serves the rest after. */
#define ORDER_ROOM (XF_MAX_FILES + XF_VIEWS + FSW_CHANNELS + XF_DIRS + 8)   /* every binding */
static struct port_packet order_q[ORDER_ROOM];
static struct fsw_order   order = { .q = order_q, .cap = ORDER_ROOM };

static bool a_close(void *ctx, const struct port_packet *p)
{
    (void)ctx;
    return (p->key & XF_KEY_FILE_BIT) && files_closed(p->key);
}

void order_closes(void)
{
    struct port_packet p;
    (void)fsw_order_drain(&order, vol.port);
    while (fsw_order_pick(&order, a_close, NULL, &p))
        files_close_now(p.key);
    files_held_closes();
}

/* The next packet: one kept by the order rule first (the answer waiting
 * goes out on its own then), else the port's. */
static status_t next_packet(uint64_t deadline, struct port_packet *pkt)
{
    if (!order.n)
        return xf_wait(deadline, pkt);
    reply_flush();
    return fsw_order_next(&order, vol.port, 0, pkt);
}

static status_t run(handle_t serve, handle_t ctl)
{
    struct xf_chan fs = { .ch = serve, .id = XF_CHAN_FS, .proto = XF_PROTO_FS };
    const struct xf_chan fc = { .ch = ctl, .id = XF_CHAN_CTL, .proto = XF_PROTO_CTL };
    views_whole(&fs.ctx);
    status_t st = arm_fs(serve);
    if (st == OK && !vol.ram)
        st = jam_port_bind(vol.port, vol.block, XF_KEY_BLOCK, SIG_PEER_CLOSED, PORT_BIND_ONCE);
    if (st == OK && ctl)
        st = jam_port_bind(vol.port, ctl, XF_KEY_CTL, SIG_READABLE, PORT_BIND_PERSISTENT);
    if (st == OK)
        st = arm_own();
    bool fs_held = false;
    uint64_t quiet = now();   /* the last packet */
    while (st == OK && !vol.disk_gone && !ctl_stopping()) {
        struct port_packet pkt;
        if (idl_held_due(&vol.held)) {
            if (fs_held && fs_unread_full(serve)) {
                idl_hold(&vol.held);
            } else if (fs_held) {
                fs_held = false;
                st = arm_fs(serve);
                if (st != OK)
                    break;
            }
            own_unheld();
            views_unheld();
            dirs_unheld();
            files_unheld();
            watch_unheld();
        }
        wback_tick();   /* when due: at most once a second, busy or not */
        uint64_t idle_at = idle_due() ? quiet + IDLE_NS : DEADLINE_NEVER;
        uint64_t deadline = wback_due() < idle_at ? wback_due() : idle_at;
        st = next_packet(idl_held_deadline(&vol.held, deadline), &pkt);
        if (st == ERR_TIMED_OUT) {
            if (now() >= idle_at)
                idle();
            st = OK;
            continue;
        }
        if (st != OK)
            break;
        quiet = now();
        if (pkt.key == XF_KEY_CTL) {
            while (!ctl_stopping() && !vol.disk_gone && serve_one(&fc) == OK)
                ;
        } else if (pkt.key == XF_KEY_BLOCK) {
            vol.disk_gone = true;
        } else if (pkt.key == XF_KEY_OWN) {
            own_event();
        } else if (pkt.key == XF_KEY_PAGER) {
            pager_event();
        } else if (pkt.key & XF_KEY_FILE_BIT) {
            files_event(pkt.key);
        } else if (pkt.key & XF_KEY_VIEW_BIT) {
            views_event(pkt.key);
        } else if (pkt.key & XF_KEY_WATCH_BIT) {
            watch_event(pkt.key);
        } else if (pkt.key & XF_KEY_LOCK_BIT) {
            locks_event(pkt.key);
        } else if (pkt.key & XF_KEY_DIR_BIT) {
            dirs_event(pkt.key);
        } else if (pkt.key == XF_KEY_FS) {
            if (fs_unread_full(serve)) {
                fs_held = true;
                idl_hold(&vol.held);
                continue;
            }
            files_reap();
            st = serve_one(&fs);
            if (st == ERR_PEER_CLOSED)
                return OK;
            if (st == OK || st == ERR_SHOULD_WAIT)
                st = arm_fs(serve);
        }
    }
    return st;
}

static void words(int argc, char **argv)
{
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], EXTFS_ARG_FORMAT))
            vol.may_format = true;
        else if (!strcmp(argv[i], EXTFS_ARG_FUA))
            vol.fua = true;
        else if (!strcmp(argv[i], EXTFS_ARG_KILLED) || !strcmp(argv[i], EXTFS_ARG_CRASHED))
            vol.ended = argv[i];
        else if (!test_word(argv[i]))
            printf("extfs %s: an argument it doesn't know: %s\n", vol.name, argv[i]);
    }
}

int main(int argc, char **argv)
{
    vol.name = argc >= 2 ? argv[1] : "extfs";
    words(argc, argv);
    bool no_volume = false, adopted = false;
    handle_t serve = startup_handle(EXTFS_SR_SERVE), block = startup_handle(EXTFS_SR_BLOCK);
    handle_t ram = startup_handle(EXTFS_SR_RAM);
    if (serve == HANDLE_INVALID || (block == HANDLE_INVALID && ram == HANDLE_INVALID)) {
        printf("extfs %s: no %s channel: nothing to do\n", vol.name,
               serve == HANDLE_INVALID ? "fs" : "block");
        return 1;
    }
    vol.serve = serve;
    vol.ctl = startup_handle(EXTFS_SR_CTL);
    vol.own = startup_handle(EXTFS_SR_OWN);
    vol.keep = startup_handle(SR_KEEP);
    vol.pager = startup_handle(EXTFS_SR_PAGER);   /* none: nothing is mapped (a test's extfs) */
    vol.at[XF_AT_MAIN] = now();
    status_t st = jam_port_create(&vol.port);
    if (st == OK)
        st = block != HANDLE_INVALID ? disk_open(block, &dev) : disk_open_ram(ram, &dev);
    vol.at[XF_AT_DISK] = now();
    if (st == OK)
        st = state_open(&adopted);
    vol.at[XF_AT_STATE] = now();
    if (st == OK)
        st = adopt(adopted, &no_volume);
    reply_flush();
    if (st != OK && vol.disk_gone) {
        printf("extfs %s: the disk went while mounting: stopping\n", vol.name);
        return 0;
    }
    if (st != OK) {
        printf("extfs %s: not serving: %s\n", vol.name, status_str(st));
        return no_volume ? EXTFS_EXIT_NO_VOLUME : 1;
    }
    pager_start();
    st = run(serve, vol.ctl);
    reply_flush();
    if (!vol.disk_gone)
        wback_stop();   /* (done already after an fsctl.stop) */
    files_close_all();
    defer_run();
    if (st == OK || vol.disk_gone)
        watch_stop();   /* served to the end: the watchers told now (their bells rung) */
    if (vol.disk_gone) {
        printf("extfs %s: the disk is gone: stopping\n", vol.name);
        return 0;
    }
    if (vol.fs.jnl && !vol.fs.ro) {
        status_t st2 = ext_jnl_close(&vol.fs);
        if (st == OK)
            st = st2;
    }
    if (st != OK)
        printf("extfs %s: stopping: %s\n", vol.name, status_str(st));
    return st == OK ? 0 : 1;
}
