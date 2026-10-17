/* fswatch: file watching in a file service, the part every service shares
 * (abi/idl/fswatch.idl, docs/INOTIFY-PLAN.md sections 2.2 to 2.7;
 * user/lib/fswatch.c).
 *
 * A service that serves `fs.watch` keeps one struct fsw per volume. The
 * service walks paths, knows its inodes and makes the changes; this
 * library keeps the rest: the watcher channels and their bells, the
 * watches by inode, a queue of events for each channel, the ids, the
 * cookies, and `take`'s records.
 *
 * **The table** (struct fsw_table) is what outlives the service's process:
 * the watcher channels' records and the watches, fixed-size, no pointers,
 * in memory the service chooses (its state VMO, <svcstate.h>). A successor
 * calls fsw_adopt on the dead instance's table; the rest of struct fsw
 * (the index by inode, the queues, the handles) is the instance's own and
 * is made again: the index from the table, each channel's queue starting
 * with IN_Q_OVERFLOW (its events died with the instance), the handles
 * given back by the service (fsw_chan_handles, from its keeper).
 *
 * **Channels.** fs.watch's handler makes one (fsw_chan_new: the volume's
 * and the process's limits), whose first message must be the bell
 * (fsw_chan_bell reads it: fswatch_bell.give); then the channel's requests
 * go to fsw_add (after the service walked the path), fsw_remove and
 * fsw_take. A channel whose client is gone, or that broke the rule, ends
 * with fsw_chan_close: its watches end with it, no events.
 *
 * **Events.** The service says what changed, at the request that changed
 * it, in the order of 2.2's table: fsw_event for a change seen by both the
 * directory (with the entry's name) and the inode itself, fsw_dir_event and
 * fsw_self_event for one side alone, fsw_gone for an inode that went,
 * fsw_end for a watch the service ends (out of its view's reach, Q2).
 * Each costs one test while nothing is watched on the volume
 * (fsw_watching). Linux's rules are kept here: a watch gets an event only
 * if its mask has the bit (IN_IGNORED and IN_Q_OVERFLOW always), IN_ISDIR
 * on a directory's events, IN_ONESHOT ending a watch after its first
 * event, IN_EXCL_UNLINK, an event equal to the channel's last one dropped,
 * and a full queue (FSWATCH_QUEUE_BYTES of records) taking one
 * IN_Q_OVERFLOW and dropping the rest until a take makes room. The bell is
 * rung when an empty queue gets an event; the client clears it.
 *
 * **Order** (2.3, "a close before the next request"): struct fsw_order
 * keeps packets taken off the service's port, so that before it serves a
 * request the service can take every packet already queued (fsw_order_drain)
 * and handle the closes among them first (fsw_order_pick). It leans on one
 * kernel behaviour, which utest port_order pins: a channel's peer-closed
 * packet is queued in the close's own system call.
 *
 * None of this is thread-safe: one thread owns a volume's struct fsw. */
#pragma once

#include <idl/fswatch.h>
#include <os.h>

#define FSW_CHANNELS     32u     /* watcher channels on a volume */
#define FSW_PER_PROCESS  8u      /* ... from one process (fs.watch's writer) */
#define FSW_WATCHES      4096u   /* watches on a volume */
#define FSW_PER_CHANNEL  1024u   /* ... on one channel */
#define FSW_SLOT_BITS    12u     /* an id: its slot below these bits, its count above */
#define FSW_COUNT_MAX    0xfffffu
#define FSW_TABLE_VERSION 1u

/* An inode as the service names it: ext's number and generation (a
 * service without generations says 0). */
struct fsw_key {
    uint64_t ino;
    uint32_t gen;
};

/* A watcher channel's record. */
struct fsw_chan_rec {
    uint32_t live;       /* 1: in use */
    uint32_t pid;        /* the process that asked (fs.watch's writer) */
    uint32_t view;       /* the view's flags (FS_VIEW_*, <fsview.h>) */
    uint32_t root_gen;   /* the view's root, the inode ... */
    uint64_t root;       /* ... and its number */
    uint32_t belled;     /* 1: its bell was given (the service keeps it before
                          * its next system call: its keeper) */
    uint32_t watches;    /* how many it has */
    uint32_t keep[2];    /* the service's own: its keeper's places for the
                          * channel and the bell */
};

/* A watch's record. Its slot is its place in fsw_table.w. */
struct fsw_watch_rec {
    uint64_t ino;        /* the inode watched */
    uint32_t gen;
    uint32_t mask;       /* IN_* events, and IN_ONESHOT, IN_EXCL_UNLINK */
    uint32_t id;         /* its slot | its count << FSW_SLOT_BITS; the count
                          * stays while the slot is free */
    uint16_t chan;       /* its channel + 1; 0: a free slot */
    uint16_t flags;      /* FSW_W_DIR */
};
#define FSW_W_DIR 1u     /* the inode is a directory */

/* What outlives the service's process. Zeroed and its version set, it is
 * empty (fsw_init does that). */
struct fsw_table {
    uint32_t version;    /* FSW_TABLE_VERSION */
    uint32_t watches;    /* watches in use */
    uint32_t next;       /* where the next add looks for a free slot */
    uint32_t reserved;
    struct fsw_chan_rec  chans[FSW_CHANNELS];
    struct fsw_watch_rec w[FSW_WATCHES];
};

/* A queued event: a record's fields, its name's len bytes right after. */
struct fsw_event {
    struct fsw_event *next;
    uint32_t          id, mask, cookie;
    uint16_t          len;
};

/* A channel's part that dies with the process. */
struct fsw_chan {
    handle_t          ch;      /* the service's end of the channel */
    handle_t          bell;    /* its bell; HANDLE_INVALID: not given (yet) */
    struct fsw_event *head, *tail;
    uint32_t          bytes;   /* records' bytes queued (FSWATCH_RECORD_FIXED + name each) */
    bool              over_queued;   /* `over` is in the queue */
    struct fsw_event  over;    /* its one IN_Q_OVERFLOW (never allocated, so a
                                * full heap still says events were lost) */
};

#define FSW_HASH 1024u   /* the index's buckets: a power of two */

struct fsw {
    struct fsw_table *t;
    /* The service's, set when it likes (NULL: none): does the view of
     * channel rec hide entry `name` of directory dir (2.6's filter)?
     * Asked for every directory event, on channels whose view has flags. */
    bool  (*hidden)(void *ctx, const struct fsw_chan_rec *rec, const struct fsw_key *dir,
                    const char *name, uint32_t len);
    void   *ctx;
    /* The library's. */
    uint32_t        cookie;                 /* the last cookie handed out */
    uint16_t        bucket[FSW_HASH];       /* an inode's bucket: its first watch's slot + 1 */
    uint16_t        chain[FSW_WATCHES];     /* the next watch of the bucket: slot + 1 */
    struct fsw_chan chans[FSW_CHANNELS];
};

/* Set w up over table t, empty (t's memory is cleared). */
void     fsw_init(struct fsw *w, struct fsw_table *t);
/* A successor: set w up over the dead instance's table t. Every live
 * channel's queue starts with IN_Q_OVERFLOW; its handles are
 * HANDLE_INVALID until the service gives them back (fsw_chan_handles), or
 * ends it (fsw_chan_close). False: t is of another version or broken, and
 * w is set up empty over it (the service closes every watcher channel it
 * kept). */
bool     fsw_adopt(struct fsw *w, struct fsw_table *t);

/* Is anything watched on the volume? (A change's one test.) */
static inline bool fsw_watching(const struct fsw *w)
{
    return w->t->watches != 0;
}

/* ---- Channels --------------------------------------------------------------- */

/* fs.watch: a new channel for process pid, through a view with these flags
 * and root. ch is the service's end, now the library's (closed by
 * fsw_chan_close, or here on an error). *out: its number. ERR_NO_RESOURCES:
 * FSW_CHANNELS are in use, or FSW_PER_PROCESS are pid's. */
status_t fsw_chan_new(struct fsw *w, uint32_t pid, uint32_t view, const struct fsw_key *root,
                      handle_t ch, uint32_t *out);
/* A successor's: channel c's handles, back from the service's keeper (bell
 * HANDLE_INVALID if it was never given). Its queue (the overflow) rings
 * the bell. */
void     fsw_chan_handles(struct fsw *w, uint32_t c, handle_t ch, handle_t bell);
/* Channel c's next message, which must be its bell (fswatch_bell.give: an
 * event with RIGHT_SIGNAL). OK: c has its bell. ERR_SHOULD_WAIT: nothing
 * queued yet. ERR_PEER_CLOSED: the client is gone. Anything else (the
 * message was another, or the bell is refused): the service ends the
 * channel (fsw_chan_close). */
status_t fsw_chan_bell(struct fsw *w, uint32_t c);
/* Has channel c its bell? (Its requests then go to fsw_add and the rest.) */
static inline bool fsw_belled(const struct fsw *w, uint32_t c)
{
    return w->chans[c].bell != HANDLE_INVALID;
}
/* End channel c: its watches, its queue and its handles (the service frees
 * what it keeps for it first: keep[]). */
void     fsw_chan_close(struct fsw *w, uint32_t c);

/* ---- Requests (fswatch.idl), channel c's ------------------------------------- */

/* add's mask alone (before the walk): ERR_INVALID_ARGS for no event, a bit
 * not IN_*'s, or IN_MASK_ADD with IN_MASK_CREATE. */
status_t fsw_mask_check(uint32_t mask);
/* add, once the service walked the path to inode key (a directory if dir)
 * and found it may be read: fswatch.add's results and errors. */
status_t fsw_add(struct fsw *w, uint32_t c, const struct fsw_key *key, bool dir, uint32_t mask,
                 uint32_t *out_id, uint8_t *out_existed);
status_t fsw_remove(struct fsw *w, uint32_t c, uint32_t id);
/* take: the records into out (room for FSWATCH_TAKE_BYTES). */
status_t fsw_take(struct fsw *w, uint32_t c, uint32_t room, uint8_t *out_more, uint8_t *out,
                  uint32_t *out_len);

/* ---- Events ----------------------------------------------------------------- */

#define FSW_EV_DIR      1u   /* the subject is a directory: IN_ISDIR */
#define FSW_EV_UNLINKED 2u   /* the subject is a file unlinked while open: not for
                              * its old directory's IN_EXCL_UNLINK watches */

/* An event in directory dir about its entry `name` (len bytes), to the
 * directory's watches. cookie: a move's (fsw_cookie), else 0. */
void     fsw_dir_event(struct fsw *w, const struct fsw_key *dir, const char *name, uint32_t len,
                       uint32_t mask, uint32_t cookie, uint32_t flags);
/* An event of inode self, to its own watches. */
void     fsw_self_event(struct fsw *w, const struct fsw_key *self, uint32_t mask, uint32_t flags);
/* Both, in Linux's order: the directory's (dir NULL: not known), then the
 * inode's own. */
void     fsw_event(struct fsw *w, const struct fsw_key *dir, const char *name, uint32_t len,
                   const struct fsw_key *self, uint32_t mask, uint32_t flags);
/* Inode key went (its last name and its last open): its watches get
 * IN_DELETE_SELF (those that asked), then IN_IGNORED, and end. */
void     fsw_gone(struct fsw *w, const struct fsw_key *key);
/* End the watch in slot (the service's reach check, Q2): IN_MOVE_SELF if
 * `moved` and it asked, then IN_IGNORED. */
void     fsw_end(struct fsw *w, uint32_t slot, bool moved);
/* The next rename's cookie: never 0, from a random start (fsw_init). */
uint32_t fsw_cookie(struct fsw *w);
/* Each watch on inode key (any channel): fn(ctx, slot), which may end it. */
void     fsw_each(struct fsw *w, const struct fsw_key *key, void (*fn)(void *ctx, uint32_t slot),
                  void *ctx);

/* ---- Order: a close before the next request (2.3) ---------------------------- */

struct fsw_order {
    struct port_packet *q;   /* the caller's room for cap packets */
    uint32_t            cap;
    uint32_t            head, n;
};

/* The next packet: the oldest one kept, else the port's (waiting until
 * deadline, ns of uptime; 0: not at all). */
status_t fsw_order_next(struct fsw_order *o, handle_t port, uint64_t deadline,
                        struct port_packet *out);
/* Before serving a request: every packet on the port now, kept behind those
 * kept already (no waiting). How many it took; it stops at cap, the rest
 * left on the port. */
uint32_t fsw_order_drain(struct fsw_order *o, handle_t port);
/* Take out the oldest kept packet `pick` says yes to (a close, to handle
 * before the request): true and *out, or false if none. The others keep
 * their order. */
bool     fsw_order_pick(struct fsw_order *o, bool (*pick)(void *ctx, const struct port_packet *p),
                        void *ctx, struct port_packet *out);
