/* <jwl.h>: libjwl, Jam OS's own Wayland library (user/lib/jwl_*.c), used
 * by the compositor and by every client. Three layers:
 *
 *   - the tables (struct jwl_interface, struct jwl_message): every
 *     interface's requests and events with their signatures. Generated
 *     from the upstream XML by tools/genwl.py into user/lib/jwl_<protocol>.c
 *     and <jwl/<protocol>.h>; the structs below are the contract between
 *     that generator and this library;
 *   - the codec (jwl_wire.c) and the object map (jwl_map.c): one Wayland
 *     message checked against its signature and decoded into a fixed array
 *     of arguments, or encoded from one; ids to objects, with Wayland's
 *     rules for which ids each side may create;
 *   - the transport (jwl_transport.c): a connection over a channel, its
 *     messages batched into channel messages behind a 16-byte header of
 *     Jam OS's own, handles where Linux passes file descriptors, and flow
 *     control so a client that never reads can't make the compositor pay.
 *
 * The bytes of a message are exactly Wayland's (the Wayland book, "Wire
 * Format"): the object's id (u32); a u32 with the message's size in bytes
 * (header included) in its upper 16 bits and the opcode in its lower 16;
 * then the arguments, each a whole number of 32-bit words in host byte
 * order: int, uint, fixed (signed 24.8), object (an id, 0 for null where
 * the XML allows it), new_id (an id; when the XML names no interface, the
 * interface's name as a string and its version first), string (a u32
 * length counting the NUL, the bytes, the NUL, padding to 4; length 0 is
 * a null string) and array (a u32 length, the bytes, padding). An fd is
 * not in the bytes: it is the next handle of the batch.
 *
 * Strict on everything the wire format defines, and fail closed: a size
 * that isn't a multiple of 4 or overruns, a message over JWL_MSG_MAX, an
 * unknown object or opcode, a message newer than the object's version, a
 * string without its NUL or with one inside, a null where the XML allows
 * none, an object of the wrong interface, a new id that isn't the next one
 * or a free one in the sender's range, bytes left over after the
 * arguments, too few or too many handles: each is a protocol error naming
 * an object and a wl_display error code. The padding bytes' values are
 * not checked (the wire format leaves them undefined); libjwl writes 0.
 *
 * Threads: a connection, its map and its messages belong to one thread
 * (the compositor's loop, a client's loop); nothing here takes a lock. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <os.h>

/* ---- the tables (the contract with tools/genwl.py) -----------------------------
 *
 * A signature is one letter per argument of the XML, in order:
 *     i int    u uint    f fixed    s string    o object    n new_id
 *     a array  h fd (a handle here)
 * with '?' before an argument the XML marks allow-null (only 's' and 'o'),
 * and, before the letters, the version the message came in ("since") as a
 * decimal number when it is above 1 ("4ii" is wl_surface.damage_buffer).
 *
 * types[i] is the interface of argument i when it is an 'o' or an 'n'
 * that the XML gives one, and NULL otherwise (and for every other
 * letter): an 'o' with NULL takes any object; an 'n' with NULL is the
 * untyped new_id of wl_registry.bind, ONE argument whose wire form is the
 * interface's name (a string), its version (a uint) and the id. So
 * wl_registry.bind is "un" with types { NULL, NULL }, not libwayland's
 * expanded "usun". types may be NULL for a message without 'o' or 'n'.
 *
 * The requests and events arrays are indexed by opcode. */
struct jwl_message {
    const char *name;                         /* the XML name: "attach" */
    const char *signature;                    /* as above: "?oii" */
    const struct jwl_interface *const *types; /* per argument; see above */
};

struct jwl_interface {
    const char *name;                         /* "wl_surface" */
    uint32_t version;                         /* the newest version these tables know */
    uint16_t nrequests, nevents;
    const struct jwl_message *requests, *events;
};

/* Every signature of iface well formed (letters, '?' only before s or o,
 * at most JWL_ARGS_MAX arguments, a since from 1 to iface's version) and
 * its counts matching its arrays. OK or ERR_INVALID_ARGS; for the
 * generator's selftest and each side's start. */
status_t jwl_interface_check(const struct jwl_interface *iface);
/* The same interface: the same table, or one of the same name (a ported
 * library may carry tables of its own). */
bool     jwl_interface_same(const struct jwl_interface *a, const struct jwl_interface *b);

/* ---- limits ------------------------------------------------------------------- */

#define JWL_MSG_MAX        4096u        /* a message's bytes, its 8-byte header included */
#define JWL_STRING_MAX     4096u        /* a string's bytes with its NUL; an array's bytes */
#define JWL_ARGS_MAX       20u          /* arguments of one message */
#define JWL_HEADER_BYTES   16u          /* the transport header of a batch */
#define JWL_BATCH_MAX      16384u       /* a batch's bytes, its header included */
#define JWL_BATCH_HANDLES  16u          /* handles one batch carries */
#define JWL_WINDOW         32u          /* batches the compositor sends ahead of the acks */
#define JWL_HELD_MAX       65536u       /* bytes of batches held back past the window */
#define JWL_OBJECTS_MAX    4096u        /* ids in each side's range on one connection */
#define JWL_MAGIC          0x314c574au  /* "JWL1" as bytes: the transport's version */

#define JWL_DISPLAY_ID     1u           /* wl_display, on every connection */
#define JWL_CLIENT_ID_MAX  0xfeffffffu  /* client ids: 1 .. this (Wayland's split) */
#define JWL_SERVER_ID_BASE 0xff000000u  /* compositor ids: this and up */

/* wl_display.error's codes (wayland.xml), which the codec and transport
 * send themselves. */
#define JWL_ERROR_INVALID_OBJECT 0u   /* an id that names nothing, or the wrong thing */
#define JWL_ERROR_INVALID_METHOD 1u   /* an unknown opcode, or a malformed message */
#define JWL_ERROR_NO_MEMORY      2u   /* a cap reached */
#define JWL_ERROR_IMPLEMENTATION 3u   /* the compositor's own failure */

/* ---- arguments and messages ---------------------------------------------------- */

struct jwl_array {
    uint32_t    size;    /* bytes */
    const void *data;    /* size bytes; NULL when size is 0 */
};

/* The untyped new_id ('n' with types[i] NULL): an object of any interface
 * the connection knows, at a version the sender chose. */
struct jwl_new_any {
    const struct jwl_interface *iface;   /* decoded: found by name among the known */
    uint32_t version;                    /* 1 .. iface->version */
    uint32_t id;                         /* the new object's id */
};

/* One argument, by its letter. Strings and arrays decoded point into the
 * batch: valid until the next jwl_conn_next on the connection. */
union jwl_arg {
    int32_t          i;     /* 'i' */
    uint32_t         u;     /* 'u' */
    int32_t          f;     /* 'f': fixed, signed 24.8 */
    const char      *s;     /* 's': NUL-terminated UTF-8; NULL for a null string */
    uint32_t         o;     /* 'o': an object's id; 0 for null */
    uint32_t         n;     /* 'n' typed: the new object's id (jwl_conn_send writes it) */
    struct jwl_new_any any; /* 'n' untyped */
    struct jwl_array a;     /* 'a' */
    handle_t         h;     /* 'h': whoever holds the message owns the handle */
};

/* A decoded message. */
struct jwl_msg {
    uint32_t id;                          /* the object it was sent on */
    uint16_t opcode;                      /* index into its interface's requests or events */
    uint16_t nargs;                       /* arguments in args[] */
    const struct jwl_interface *iface;    /* the object's interface */
    const struct jwl_message   *msg;      /* the request or event */
    void    *data;                        /* the object's data in the map */
    uint32_t version;                     /* the object's version */
    uint32_t nhandles;                    /* 'h' arguments among args[] */
    bool     dead_target;                 /* for an object this side destroyed (dropped) */
    union jwl_arg args[JWL_ARGS_MAX];
};

/* Close every handle argument of m (a message nobody takes). */
void jwl_msg_close_handles(struct jwl_msg *m);

/* What was wrong, for wl_display.error: the object to name, the code,
 * and a line of text. */
struct jwl_error {
    uint32_t object;
    uint32_t code;          /* JWL_ERROR_* */
    char     text[120];
};

/* ---- the object map (jwl_map.c) ------------------------------------------------
 *
 * One per connection on each side: id to (interface, version, data). The
 * client makes ids from 1 up (1 is wl_display), the compositor from
 * JWL_SERVER_ID_BASE up; each side's range is an array of at most
 * JWL_OBJECTS_MAX entries. A new id from the peer must be the next one
 * after the highest so far, or one that is free again (Wayland's rule, as
 * libwayland's allocator makes them): anything else is a protocol error.
 *
 * Freeing an id is a handshake for the client's ids: the client destroys
 * an object (its entry becomes a ZOMBIE: events still on their way to it
 * are decoded, their handles closed, and dropped), the compositor frees
 * the id and says wl_display.delete_id, and only then is the id free for
 * the client to use again. The compositor's ids have no handshake: freed
 * at once on the compositor; a zombie on the client until reused. */

enum jwl_side { JWL_CLIENT, JWL_SERVER };
enum jwl_state { JWL_FREE, JWL_LIVE, JWL_ZOMBIE };

struct jwl_object {
    const struct jwl_interface *iface;  /* NULL while free */
    void    *data;                      /* the owner's, set by jwl_map_set_data */
    uint32_t version;                   /* the bound or inherited version */
    uint8_t  state;                     /* enum jwl_state */
    bool     deleted;                   /* client: delete_id came while still live */
    uint16_t reserved;
    uint32_t next_free;                 /* free list of our own range: an index + 1, 0 ends it */
};

struct jwl_range {
    struct jwl_object *v;               /* n entries, room for cap */
    uint32_t n, cap;
    uint32_t free_head;                 /* our own range: index + 1 of a free entry; 0: none */
};

struct jwl_map {
    enum jwl_side side;                 /* which end this map is */
    struct jwl_range r[2];              /* [0] client ids from 1, [1] compositor ids */
    uint32_t live;                      /* live objects in both ranges */
    const struct jwl_interface *const *known;  /* what an untyped new_id may name */
    unsigned nknown;
};

/* An empty map for side, with wl_display (display, version 1) at id 1;
 * known (nknown entries, kept by pointer) are the interfaces an untyped
 * new_id may name. ERR_NO_MEMORY. */
status_t jwl_map_init(struct jwl_map *m, enum jwl_side side, const struct jwl_interface *display,
                      const struct jwl_interface *const *known, unsigned nknown);
void     jwl_map_free(struct jwl_map *m);
/* The entry of id in any state, or NULL when id is outside both ranges'
 * entries (never made). */
struct jwl_object *jwl_map_entry(const struct jwl_map *m, uint32_t id);
/* The live object id, or NULL. */
struct jwl_object *jwl_map_get(const struct jwl_map *m, uint32_t id);
/* A new id in our own range for an object of iface at version (the first
 * free entry, else the next): OK with *out; ERR_NO_RESOURCES at
 * JWL_OBJECTS_MAX; ERR_NO_MEMORY. */
status_t jwl_map_new(struct jwl_map *m, const struct jwl_interface *iface, uint32_t version,
                     void *data, uint32_t *out);
/* May the peer make id now? OK; ERR_INVALID_ARGS: 0, outside the peer's
 * range, live, or past the next one; ERR_NO_RESOURCES: the range is full. */
status_t jwl_map_check_new(const struct jwl_map *m, uint32_t id);
/* Enter the peer's new id (jwl_map_check_new first). ERR_NO_MEMORY. */
status_t jwl_map_insert(struct jwl_map *m, uint32_t id, const struct jwl_interface *iface,
                        uint32_t version, enum jwl_state state);
/* The live object id is destroyed, by the rules above. ERR_NOT_FOUND: not
 * live; ERR_INVALID_ARGS: wl_display. */
status_t jwl_map_remove(struct jwl_map *m, uint32_t id);
/* Client: the compositor's delete_id. ERR_INVALID_ARGS: not one of our
 * ids that is live or a zombie. */
status_t jwl_map_delete_id(struct jwl_map *m, uint32_t id);
/* Undo a jwl_map_new whose message was never sent: the id is free at once. */
void     jwl_map_unmake(struct jwl_map *m, uint32_t id);
status_t jwl_map_set_data(struct jwl_map *m, uint32_t id, void *data);

/* ---- the codec (jwl_wire.c) ----------------------------------------------------- */

/* Bytes coming in: buf[at .. len) is what is left of a batch, and its
 * handles h[hat .. nh) are what is left of its handles. */
struct jwl_in {
    const uint8_t  *buf;
    size_t          len, at;
    const handle_t *h;
    unsigned        nh, hat;
};

/* Bytes going out: messages are appended at buf[len], handles at h[nh]. */
struct jwl_out {
    uint8_t  *buf;
    size_t    cap, len;
    handle_t *h;
    unsigned  hcap, nh;
};

/* The next message of in, checked against its signature with map's ids
 * (the requests of a JWL_SERVER map, the events of a JWL_CLIENT one): OK
 * with *out filled, in advanced past the message and its handles, and its
 * new ids entered in map. An object argument naming one of this side's
 * zombies is decoded as 0 (as libwayland does); a message to a zombie is
 * decoded with dead_target set, its new ids entered as zombies, for the
 * caller to drop. ERR_SHOULD_WAIT: nothing left in in. ERR_INVALID_ARGS:
 * a protocol error, described in *err; in is then unchanged (and so is
 * map, but for a message with two new ids whose second can't be entered:
 * the connection ends anyway). Handles are taken out of in by value: the
 * caller owns them (in *out on success, still in in on failure). */
status_t jwl_decode(struct jwl_in *in, struct jwl_map *map, struct jwl_msg *out,
                    struct jwl_error *err);

/* Append message (id, opcode) of signature m->signature with nargs args:
 * OK, out->len and out->nh advanced. ERR_INVALID_ARGS: args don't fit the
 * signature (the count, a null where none is allowed, a string over
 * JWL_STRING_MAX, a NULL array with a size, a new id 0); ERR_OUT_OF_RANGE:
 * the message would be over JWL_MSG_MAX; ERR_BUFFER_TOO_SMALL: no room
 * left in out (bytes or handles). On failure out is unchanged. Ids are
 * taken as given: the map is jwl_conn_send's business. */
status_t jwl_encode(struct jwl_out *out, const struct jwl_message *m, uint32_t id,
                    uint16_t opcode, const union jwl_arg *args, unsigned nargs);

/* A message's signature, parsed. */
struct jwl_sig {
    uint32_t since;                      /* the version it came in */
    unsigned n;                          /* arguments */
    char     type[JWL_ARGS_MAX];         /* each one's letter */
    bool     nullable[JWL_ARGS_MAX];
};
/* OK or ERR_INVALID_ARGS (a letter that isn't one, '?' before anything
 * but s or o, more than JWL_ARGS_MAX, a since of 0 or a broken number). */
status_t jwl_sig_parse(const char *s, struct jwl_sig *out);

/* ---- the transport (jwl_transport.c) -------------------------------------------
 *
 * One channel message is one batch: a 16-byte header, then whole Wayland
 * messages (none for a batch that only acknowledges). Header words (u32,
 * host order):
 *     0  JWL_MAGIC
 *     4  acked: from a client, how many of the compositor's batches it has
 *        read so far (mod 2^32); from the compositor, 0
 *     8  nfds: handles the batch's 'h' arguments use, equal to the
 *        handles the channel message carries
 *    12  0
 * A batch is at most JWL_BATCH_MAX bytes and JWL_BATCH_HANDLES handles; a
 * message never spans two batches.
 *
 * Flow control. A message queued on a channel is charged to its sender's
 * job until it is read, so the compositor sends at most JWL_WINDOW
 * batches a client hasn't acknowledged. Past that it keeps batches in its
 * own memory, up to JWL_HELD_MAX bytes, and then disconnects the client
 * (wl_display.error, no_memory). A client acknowledges in the header of
 * everything it sends, and jwl_conn_flush sends a batch with no messages
 * when half the window is read and nothing else went out: so a client
 * must call jwl_conn_flush after reading, every turn of its loop.
 *
 * Errors. On the compositor's side every protocol error the transport or
 * the codec finds is sent to the client as wl_display.error at once, and
 * the connection is dead (its channel is closed by jwl_conn_destroy). The
 * compositor's own checks (a role error, a bad buffer) use
 * jwl_conn_post_error the same way. On a client's side the connection is
 * just dead: a compositor that breaks the protocol is not argued with. */

struct jwl_held;

struct jwl_conn_config {
    handle_t ch;                                /* the connection's channel end (consumed) */
    enum jwl_side side;
    const struct jwl_interface *display;        /* wl_display's table */
    const struct jwl_interface *const *known;   /* for untyped new ids (kept by pointer) */
    unsigned nknown;
};

struct jwl_conn_stats {
    uint64_t batches_in, batches_out;   /* channel messages read and written */
    uint64_t msgs_in, msgs_out;         /* Wayland messages decoded and encoded */
    uint64_t dropped;                   /* messages to zombies dropped */
    uint32_t held_peak;                 /* most bytes held back at once */
};

struct jwl_conn {
    handle_t ch;                        /* the channel end */
    enum jwl_side side;
    status_t status;                    /* OK while alive; why it died */
    struct jwl_error error;             /* the protocol error that killed it, if one did */
    struct jwl_map map;
    const struct jwl_interface *display;
    /* receiving: the batch being decoded */
    uint8_t  in[JWL_BATCH_MAX];
    uint32_t in_len, in_at;             /* bytes in in[], decoded so far */
    handle_t in_h[JWL_BATCH_HANDLES];
    uint32_t in_nh, in_hat;             /* handles carried, taken so far */
    uint32_t nread;                     /* client: compositor batches read (mod 2^32) */
    uint32_t told;                      /* client: nread as last sent in a header */
    /* sending: the batch being filled (its header written when it goes) */
    uint8_t  out[JWL_BATCH_MAX];
    uint32_t out_len;                   /* JWL_HEADER_BYTES while empty */
    handle_t out_h[JWL_BATCH_HANDLES];
    uint32_t out_nh;
    uint32_t nsent;                     /* compositor: batches written (mod 2^32) */
    uint32_t peer_acked;                /* compositor: the client's last ack */
    struct jwl_held *held, *held_tail;  /* sealed batches not written yet, oldest first */
    uint32_t held_bytes;
    struct jwl_conn_stats stats;
};

/* A connection on cfg->ch. Both tables are checked (jwl_interface_check;
 * display must be wl_display with error "ous" and delete_id "u"):
 * ERR_INVALID_ARGS. ERR_NO_MEMORY. The channel is consumed either way. */
status_t jwl_conn_create(const struct jwl_conn_config *cfg, struct jwl_conn **out);
/* Close the channel and every handle still held; free c. */
void     jwl_conn_destroy(struct jwl_conn *c);

/* The next message for this side: OK with *out (its handles are the
 * caller's). A message to a zombie is dropped here, and a client handles
 * wl_display.delete_id here: neither is returned. ERR_SHOULD_WAIT: the
 * channel is empty (wait for SIG_READABLE). Any other error: the
 * connection is dead, and that status comes back from every call after
 * (ERR_PEER_CLOSED: the peer closed; ERR_INVALID_ARGS: a protocol error,
 * in c->error, and on the compositor's side already sent). Reads at most
 * one batch per message returned, and every queued batch with no message
 * left for the caller, which the channel's 1024-message cap bounds. */
status_t jwl_conn_next(struct jwl_conn *c, struct jwl_msg *out);

/* Send message opcode on object id, which must be live and of iface:
 * requests from a client, events from the compositor. Each new id in args
 * is made here (in our range; a typed one at id's version, an untyped one
 * at args[i].any's) and written back into args. Every handle in args is
 * consumed, whatever happens. The message joins the batch being filled;
 * nothing is written to the channel until it fills or jwl_conn_flush.
 * ERR_NOT_FOUND: id isn't live; ERR_WRONG_TYPE: not an iface; ERR_NOT_SUPPORTED:
 * the opcode is newer than the object's version or doesn't exist;
 * ERR_INVALID_ARGS: args don't fit (jwl_encode; an object argument that
 * isn't live or of its type); ERR_OUT_OF_RANGE: too big; ERR_NO_RESOURCES:
 * no id left; a dead connection's status. */
status_t jwl_conn_send(struct jwl_conn *c, const struct jwl_interface *iface, uint32_t id,
                       uint16_t opcode, union jwl_arg *args, unsigned nargs);

/* The object id is destroyed (after its destructor request was sent or
 * received): on the compositor, a client's id is freed and
 * wl_display.delete_id is sent; see the map's rules for the rest. Errors
 * as jwl_map_remove's, or a dead connection's status. */
status_t jwl_conn_delete(struct jwl_conn *c, uint32_t id);

/* Write what is waiting: the batch being filled, then held batches as far
 * as the window lets (compositor), or an acknowledgement (client). OK
 * (some may still be held: the window, or a full channel); a dead
 * connection's status. Call once per loop turn. */
status_t jwl_conn_flush(struct jwl_conn *c);

/* Compositor: send wl_display.error (object, code, text cut to fit) past
 * the window, dropping everything not yet written, and the connection is
 * dead (ERR_INVALID_ARGS). An object that isn't live is named as the
 * display. Returns that status. ERR_NOT_SUPPORTED on a client's side. */
status_t jwl_conn_post_error(struct jwl_conn *c, uint32_t object, uint32_t code, const char *text);

/* Compositor: true while batches are held back (the client is behind):
 * the moment to coalesce what can be (pointer motion) instead of sending. */
bool     jwl_conn_backlogged(const struct jwl_conn *c);

/* The live object id's map entry (its data, version, interface), or NULL. */
static inline struct jwl_object *jwl_conn_object(struct jwl_conn *c, uint32_t id)
{
    return jwl_map_get(&c->map, id);
}
