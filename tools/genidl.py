#!/usr/bin/env python3
"""Generate protocol code from abi/idl/*.idl (ARCHITECTURE.md "IPC").

    genidl.py gen     write the generated headers
    genidl.py check   exit 1 (and name them) if any is missing, stale or
                      left over from a protocol that no longer exists
    genidl.py selftest  run genidl on small trees of its own: what it must
                      refuse, and a stale output failing `check` (SELFTESTS)

Outputs, committed like the syscall glue (so they can be read and grepped):
    drivers/include/idl/common.h    what every protocol shares
    drivers/include/idl/<name>.h    one per protocol: message structs,
                                    client stubs, server dispatch

Everything is `static inline` in the headers, over <jam/driver.h>'s
drv_channel_* calls only, so the same header works in a driver (drivers
see nothing but driver.h and these) and in an ordinary user program (libos
implements drv_*).

The language, one statement per line, `#` starts a comment (comment lines
right above a method are copied into the header):

    protocol <name> <id> [kept]
    <ordinal> <method> (<type> <arg>, ...) -> (<type> <result>, ...) [later] [idempotent]
    <ordinal> <event> (<type> <field>, ...) event         to the server
    <ordinal> <event> -> (<type> <field>, ...) event      to the client
    const <NAME> <value>                                  a constant

name, method, arg, result   C identifiers (lower case by convention)
id                          1..65535, listed in abi/idl/ids.txt (the registry
                            of every protocol id, hand-written ones too)
ordinal                     1..65535, unique in the protocol
type                        u8 u16 u32 u64 i8 i16 i32 i64, or u8[N] (a
                            fixed-size byte array, 1 <= N <= 4096), or
                            u8[<=N] (up to N bytes), u16[<=N] .. i64[<=N]
                            (up to N of those scalars), or str[<=N] (a
                            string of up to N bytes of UTF-8), or `handle`
                            (RESULTS only, at most 8 per method)

Either list may be empty: `()`.

A `const` line (the M12 plan's D3) gives the protocol above it a number:
its flags, levels and states, kept with its methods. NAME is in capitals;
the value an integer (decimal or 0x hex, maybe negative) or one shifted
(`1 << 3`). The header has it as <PROTO>_<NAME> (`u` or `ull` suffixed,
in brackets when negative or shifted), with the comment above it. A request or reply is at most 16 KiB
(MSG_MAX, header included). Bulk data goes through a shared VMO.

Arrays of varying length, u8[<=N] .. i64[<=N] and str[<=N] (1 <= N <= 65535, within
the message's 16 KiB): on the wire, after the message's fixed part (its
other fields, in a packed struct) and in the order declared, a u16 count
of elements, zero bytes up to a multiple of the element's size from the
message's start (none for bytes), the elements. In C each is two
parameters, a pointer and a length in elements (`x`, `x_len`; a field may
not be named `x_len` beside `x`):
    a client's argument, a handler's    const uint32_t *x, uint32_t x_len
                                        (str: const char *, no NUL)
    a client's result                   uint32_t out_x[N], uint32_t *out_x_len
                                        (str: char out_x[N + 1], NUL-terminated)
    a handler's result                  uint32_t *out_x (room for N, in the
                                        reply), uint32_t *out_x_len
    reply_<method>'s result             as a client's argument
A str is well-formed UTF-8 (<utf8.h>'s rules) without a NUL. The
generated server refuses a request whose array is past its N, runs past
the message or leaves bytes over, whose padding isn't zero, or whose str
isn't one (ERR_INVALID_ARGS, before any handler sees it); the generated
client refuses the same in its own arguments before sending
(ERR_INVALID_ARGS) and in a reply (ERR_INTERNAL); a handler's or
reply_<method>'s result that breaks them is answered ERR_INTERNAL. A
handler's argument points into the request, and a handler's result into
the reply, each aligned to its element's size (the request and reply
buffers given to <proto>_dispatch_on are 8-aligned): copy what it keeps
(a `later` handler especially). Records stay byte arrays of a C struct (the
M12 plan's D2).

Events (`event`): one-way messages, never answered, in either direction
in one protocol (the M12 plan's D1), sharing the protocol's ordinals. On
the wire a request's form with txid 0: a message with txid 0 is an event,
any other a request or a reply. Their fields are a method's (arrays of
varying length too) and handles (at most 8): an event's handles are moved
in every case: sent, lost with a write that failed (the kernel closes
them), or closed by the stub when it never writes (a field refused).
The queue's limit is the flow control: <proto>_<event>(ch, fields...)
returns ERR_SHOULD_WAIT when the peer's queue is full. Generated:
    <proto>_<event>(ch, fields...)      the sender's stub, either way
    struct <proto>_<event>_msg          its fixed part
    to the server: a handler in struct <proto>_ops, void (*e)(ctx,
        fields...), which owns the handles it gets; the server's loops read
        an event's handles (a protocol whose events carry handles gets
        <proto>_dispatch_h, which takes them; <proto>_dispatch_on is it
        without handles), run its handler, answer nothing
    to the client: struct <proto>_events (a handler per event),
        <proto>_event_dispatch(&events, ctx, msg, &m) for a message read
        with idl_reply_read whose txid is 0, and <proto>_read_event(ch,
        &events, ctx) for a channel read for events alone. A blocking
        call's reply is taken by its txid, past events queued before it.
In a protocol with events, a server never answers a message with txid 0
(an event that breaks its format, has no handler, or goes the other way
is dropped, its handles closed; so is a call sent with txid 0), and it
answers an event sent with a txid ERR_INVALID_ARGS (its sender may be
waiting). <PROTO>_REQ_MAX and <PROTO>_REP_MAX count the events to the
server and to the client.

Message buffers: a generated function keeps them on its stack up to
IDL_STACK_MAX (4 KiB) together; a method whose request and reply pass it
(its client calls, _send, reply_<method>) and a protocol whose biggest two
pass it (<proto>_serve, <proto>_serve_one) take theirs from the heap: per
call, or once per <proto>_serve (ERR_NO_MEMORY if they can't). Handlers
fill fixed arrays of such a method straight in the reply.

The file's own comment, above the `protocol` line, must state the
protocol's restart rule: what a client sees when the server dies and is
started again, in a paragraph that begins `Restart:` or `Reconnect:`
(also with a note in brackets before the colon: `Reconnect (devmgr's
supervision):`). Either the restart is not seen (a service that outlives
its process, ARCHITECTURE.md "Services that outlive their process"), or
it is ERR_PEER_CLOSED and the paragraph says what to open again and what
is lost. A file without one is refused. The comment is copied to the top
of the generated header (a comment may not hold `*/`).

`kept` marks a protocol whose server outlives its process (its restart
rule says the restart "is not seen": the server reads requests into slots
of its state, and a successor runs the one in progress again,
<proto>_take_slot and _run_slot). genidl insists the two agree: a kept
protocol's rule says so, and a protocol whose rule says so is kept. A
dead instance's handles die with it, so nothing to a kept protocol's
server may carry handles (an event's, and handle arguments when they
come). The header has <PROTO>_KEPT.

A file may hold several protocols, one for each kind of channel of one
service (an opener's and a stream's): each `protocol` line starts one,
and the methods after it are its own. The first is named after the file
(audio.idl: `audio`, then `audio_stream`); each later one states its own
restart rule in the comment above its line. One header holds them all
(<idl/audio.h>). Each has its own id and ordinals, so a server of one
answers another's request ERR_NOT_SUPPORTED. No two protocols anywhere
may define the same C name (`a` with a method `b_c`, `a_b` with `c`).

A client calls a method four ways:
    <proto>_<method>(ch, args..., &results...)    waits for ever
    <proto>_<method>_until(ch, deadline_ns, ...)  until a deadline
    <proto>_<method>_within(ch, timeout_ns, ...)  for a timeout from when the
        call starts, by the kernel's clock: no clock read first
    <proto>_<method>_call(ch, within, t, ...)     the body of the two before
        (t a deadline, or a timeout if `within`)

A server runs <proto>_serve(ch, &ops, ctx): each reply goes out in the
system call that waits for the next request (drv_channel_reply_wait, in
idl_serve_next), one system call per request. <proto>_serve_one reads,
answers and writes one request, for a loop of the server's own (one that
waits on a port for many channels).

A server whose requests must outlive it (a service with a state VMO,
<svcstate.h>) reads each one into a slot of memory it chooses (struct
idl_slot): <proto>_take_slot(ch, slot, &reply) reads one request there,
its length written last in the same system call, so a death leaves it
either queued or wholly in the slot; <proto>_run_slot(ch, slot, &ops,
ctx, ...) runs the request in a slot, its reply built in the slot too.
The reply waits (struct idl_reply) and goes out with the server's next
system call: the next take (idl_take_after), its wait on a port
(idl_wait_after) or idl_reply_flush, with a mark the kernel sets in the
slot once it went out. The loop and a
successor's re-run of the request in progress are then the same code.
<PROTO>_REQ_MAX and <PROTO>_REP_MAX size the slots.

`idempotent`: running the method again on the state its first run left
gives the same result and the same answer (a read, a write at an explicit
offset, a stat). Recorded in <proto>_idempotent(wire ordinal), for a server
that keeps no state across a restart: it may run such a request again and
answers any other with an error. The client's side doesn't change. Read
by nothing yet (only utest): the note is kept for the drivers that outlive
their process (the milestone after M12), whose requests would be run again
without a slot (the M12 review P's finding 9).

`later`: the server may answer the method later, after its handler has
returned and the loop has served other requests (a `recv` that waits for
data, a wait for a device). The handler gets the request's `struct
idl_txn` (the channel it came on and its txid) and either answers now as
usual or returns IDL_LATER, keeps the txn and answers with
<proto>_reply_<method>(txn, status, results...) when it can, from
anywhere in the server. Nothing changes for the client. A server that
dispatches by hand uses <proto>_dispatch_on(ch, ...) so the txn has its
channel.

Every method also gets, for code that must not block (ARCHITECTURE.md
"How a service waits"):
    <proto>_<method>_send(ch, txid, args...)
        the request, written without waiting, with a txid of the caller's
        own (idl_txid_next). The reply comes back on ch like any message:
        bind ch to a port, read it with idl_reply_read (its txid says which
        call it answers) and decode it with
    <proto>_<method>_result(rep, &msg, &results...)
    <proto>_reply_<method>(txn, status, results...)
        a reply from anywhere (a later method's, or one a hand-written loop
        answers itself)
Don't mix the two kinds of client calls on one channel end: a reply to an
asynchronous call whose txid happens to equal a blocking call's waiting
on the same end would go to that call (channel_call's txids are the
kernel's).

Handles travel server -> client in replies, and either way in events
(above). In replies, the server's
handler fills `handle_t *out_x` and the reply moves the handles to the
client (drv_channel_call_h reserves their slots before the request goes
out, so a full handle table fails the call up front). A reply with the
wrong number of handles is ERR_INTERNAL and whatever arrived is closed; a
failed reply loses the server's handles (the kernel closes them); a client passing NULL for a
handle result gets it closed. Handle ARGUMENTS are refused: if a call
fails, it can't tell whether the kernel already moved them, so ownership
would be ambiguous. Pass handles with channel_write or as results.

Wire format (packed little-endian structs, no padding):
    request  u32 txid (channel_call stamps it), u32 ordinal, the args
    reply    u32 txid (echoed), i32 status, then the results only if
             status is OK
The ordinal on the wire is (id << 16) | ordinal, so a request sent to a
server of another protocol fails with ERR_NOT_SUPPORTED instead of being
taken for a different method. A request of the wrong size, one carrying
handles, or one too big for any method gets ERR_INVALID_ARGS; one shorter
than 4 bytes (no txid) is dropped without a reply.

Run from the repository root (the Makefile does)."""
import glob
import os
import re
import sys

IDL_DIR = "abi/idl"
OUT_DIR = "drivers/include/idl"
IDS_FILE = os.path.join(IDL_DIR, "ids.txt")   # the protocol id registry
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*$")
# The start of a file comment's restart rule ("Restart:", "Reconnect (note):").
RESTART_RULE = re.compile(r"(Restart|Reconnect)( \([^)]*\))?:")
SCALARS = {"u8": ("uint8_t", 1), "u16": ("uint16_t", 2), "u32": ("uint32_t", 4),
           "u64": ("uint64_t", 8), "i8": ("int8_t", 1), "i16": ("int16_t", 2),
           "i32": ("int32_t", 4), "i64": ("int64_t", 8)}
ARRAY = re.compile(r"u8\[(\d+)\]$")
VAR = re.compile(r"(u8|u16|u32|u64|i8|i16|i32|i64|str)\[<=(\d+)\]$")   # varying: up to N elements
ARRAY_MAX = 4096
# Bytes of one request or reply (the M12 reviews' S-4: two 4 KiB paths and
# more in one request). The kernel takes up to 64 KiB a message; the bound
# is the server's, which holds its protocol's biggest request and reply at
# once (<proto>_serve, <proto>_serve_one; a slot's room).
MSG_MAX = 16384
# Bytes of message buffers a generated function keeps on its stack (tasks
# have 32 KiB stacks, <jam/task.h>): a method or protocol needing more takes
# them from the heap (drv_malloc), once per call, or once per <proto>_serve.
STACK_MAX = 4096
HDR = 8                 # txid + ordinal / txid + status
RESERVED = {"ch", "deadline_ns", "timeout_ns", "ctx", "ops"}
# Names of the protocol's own functions (<proto>_<name>): no method may take one.
RESERVED_METHODS = ("dispatch", "dispatch_on", "dispatch_h", "serve", "serve_one", "take_slot",
                    "run_slot", "idempotent", "ops", "events", "event_dispatch", "read_event")
KEYWORDS = ("later", "idempotent", "event")   # after a method's results
C_KEYWORDS = set("""auto break case char const continue default do double else enum extern
    float for goto if inline int long register restrict return short signed sizeof static
    struct switch typedef union unsigned void volatile while _Bool _Alignas _Alignof _Atomic
    _Noreturn _Static_assert _Thread_local bool true false NULL""".split())
# common.h's helpers for varying arrays, as C.
VAR_HELPERS = r"""/* Client: idl_rep_status for a reply with varying arrays: on success at
 * least `fixed` bytes (the arrays follow, checked as they are read). */
static inline status_t idl_rep_status_var(const void *rep, uint32_t n, uint32_t fixed)
{
    const struct idl_rep_hdr *h = (const struct idl_rep_hdr *)rep;
    if (n < sizeof(*h) || h->status > 0)
        return ERR_INTERNAL;
    if (h->status != OK)
        return n == sizeof(*h) ? h->status : ERR_INTERNAL;
    return n >= fixed ? OK : ERR_INTERNAL;
}

/* ---- varying arrays (u8[<=N], str[<=N], u32[<=N] ...) ------------------ */

/* On the wire, after the message's fixed part, in the order declared: a u16
 * count of elements, zero bytes up to a multiple of the element's size
 * (from the message's start), the elements. */

/* Put len elements of sz bytes from p at *m + *o, if len <= max; *o moves
 * past them. false: too many (or none to read: p NULL). */
static inline bool idl_put(void *m, uint32_t *o, const void *p, uint32_t len, uint32_t max,
                           uint32_t sz)
{
    uint8_t *idl_b = (uint8_t *)m;
    uint16_t idl_c = (uint16_t)len;
    if (len > max || (len && !p))
        return false;
    __builtin_memcpy(idl_b + *o, &idl_c, 2);
    *o += 2;
    while (*o % sz)
        idl_b[(*o)++] = 0;
    if (len)
        __builtin_memcpy(idl_b + *o, p, len * sz);
    *o += len * sz;
    return true;
}

/* Read the array at *o in the message m of n bytes (*o <= n): *p at its
 * elements (aligned to sz, if m is 8-aligned), *len of them. false: it
 * runs past n, has more than max, or a padding byte isn't 0. */
static inline bool idl_get(const void *m, uint32_t n, uint32_t *o, uint32_t max, uint32_t sz,
                           const void **p, uint32_t *len)
{
    const uint8_t *idl_b = (const uint8_t *)m;
    uint16_t idl_c;
    if (n - *o < 2)
        return false;
    __builtin_memcpy(&idl_c, idl_b + *o, 2);
    *o += 2;
    while (*o % sz) {
        if (*o >= n || idl_b[*o])
            return false;
        (*o)++;
    }
    if (idl_c > max || (n - *o) / sz < idl_c)
        return false;
    *p = idl_b + *o;
    *len = idl_c;
    *o += idl_c * sz;
    return true;
}

/* Server: the array a handler wrote at offset `from` of the reply m (len
 * elements of sz bytes) to its place at *o (never after `from`); *o moves
 * past it. */
static inline void idl_place(void *m, uint32_t *o, uint32_t from, uint32_t len, uint32_t sz)
{
    uint8_t *idl_b = (uint8_t *)m;
    uint16_t idl_c = (uint16_t)len;
    __builtin_memcpy(idl_b + *o, &idl_c, 2);
    *o += 2;
    while (*o % sz)
        idl_b[(*o)++] = 0;
    if (len && *o != from)
        __builtin_memmove(idl_b + *o, idl_b + from, len * sz);
    *o += len * sz;
}

/* Is s (n bytes) a str: well-formed UTF-8 (Unicode's Table 3-7: no overlong
 * forms, no surrogates, nothing past U+10FFFF; the rules of <utf8.h>) with
 * no NUL? */
static inline bool idl_str_ok(const char *s, uint32_t n)
{
    const uint8_t *idl_b = (const uint8_t *)s;
    for (uint32_t idl_i = 0; idl_i < n;) {
        uint8_t idl_c = idl_b[idl_i];
        uint32_t idl_len;
        uint8_t idl_lo = 0x80, idl_hi = 0xbf;
        if (idl_c == 0)
            return false;
        if (idl_c < 0x80) {
            idl_i++;
            continue;
        }
        if (idl_c >= 0xc2 && idl_c <= 0xdf)
            idl_len = 2;
        else if (idl_c >= 0xe0 && idl_c <= 0xef)
            idl_len = 3;
        else if (idl_c >= 0xf0 && idl_c <= 0xf4)
            idl_len = 4;
        else
            return false;   /* a continuation byte, C0, C1 or F5..FF */
        if (idl_c == 0xe0)
            idl_lo = 0xa0;   /* below: overlong */
        else if (idl_c == 0xed)
            idl_hi = 0x9f;   /* above: a surrogate */
        else if (idl_c == 0xf0)
            idl_lo = 0x90;   /* below: overlong */
        else if (idl_c == 0xf4)
            idl_hi = 0x8f;   /* above: past U+10FFFF */
        if (n - idl_i < idl_len || idl_b[idl_i + 1] < idl_lo || idl_b[idl_i + 1] > idl_hi)
            return false;
        for (uint32_t idl_k = 2; idl_k < idl_len; idl_k++)
            if (idl_b[idl_i + idl_k] < 0x80 || idl_b[idl_i + idl_k] > 0xbf)
                return false;
        idl_i += idl_len;
    }
    return true;
}"""
BANNER = "Generated by tools/genidl.py from {src}. Do not edit:\n" \
         " * change the .idl and run `make idl`."


REP_HANDLES_MAX = 8
EVENT_HANDLES_MAX = 8   # handles in one event (IDL_READ_HANDLES, IDL_REP_HANDLES)


class Field:
    """One argument, result or event field. kind: "scalar" (u8 .. i64),
    "fixed" (u8[N]), "var" (u8[<=N], str[<=N], u32[<=N] ...: ctype the
    element's, size its bytes, array N) or "handle"."""
    def __init__(self, kind, ctype, name, size, array=0, is_str=False):
        self.kind, self.ctype, self.name, self.size, self.array = kind, ctype, name, size, array
        self.handle = kind == "handle"
        self.var = kind == "var"
        self.str = is_str
        self.max = array if self.var else 0   # a varying array's N
        if self.var:
            self.array = 0
        self.wire = 0 if self.var or self.handle else size   # bytes in the packed struct

    def member(self):
        """The field in the message's packed struct (handles travel apart)."""
        return f"{self.ctype} {self.name}[{self.array}];" if self.array else \
               f"{self.ctype} {self.name};"

    def in_params(self):
        """The C parameters that pass it in (a client's argument, a server's
        reply_<method> result, a handler's argument): a varying array as a
        pointer and its length in elements (a str's bytes: no NUL)."""
        if self.var:
            return [f"const {self.ctype} *{self.name}", f"uint32_t {self.name}_len"]
        return [f"const uint8_t {self.name}[{self.array}]" if self.array else
                f"{self.ctype} {self.name}"]

    def out_params(self):
        """The C parameters that take it out at a client: a varying array
        into room for all N elements (a str's with a NUL after them: N + 1)
        and its length."""
        if self.var:
            n = f"{self.max} + 1" if self.str else self.max
            return [f"{self.ctype} out_{self.name}[{n}]", f"uint32_t *out_{self.name}_len"]
        return [f"uint8_t out_{self.name}[{self.array}]" if self.array else
                f"{self.ctype} *out_{self.name}"]

    def handler_out_params(self):
        """A handler's: a varying array into room for N elements, in the
        reply (a str's without a NUL)."""
        if self.var:
            return [f"{self.ctype} *out_{self.name}", f"uint32_t *out_{self.name}_len"]
        return self.out_params()


def in_params(fields):
    return [p for f in fields for p in f.in_params()]


def out_params(fields):
    return [p for f in fields for p in f.out_params()]


def handler_out_params(fields):
    return [p for f in fields for p in f.handler_out_params()]


def wire_size(fields):
    """Bytes of a message's fixed part: the header and the packed struct
    (all of it, without varying arrays)."""
    return HDR + sum(f.wire for f in fields)


def align(n, a):
    return (n + a - 1) // a * a


def var_layout(fields):
    """Where each varying array's elements start when every one before it
    is full (the room a handler fills, in the reply), and the biggest
    message: [(field, offset)], max bytes. On the wire each is a u16 count,
    zeros to the element's size, the elements, after the fixed part."""
    o, out = wire_size(fields), []
    for f in fields:
        if f.var:
            o = align(o + 2, f.size)
            out.append((f, o))
            o += f.max * f.size
    return out, o


class Method:
    """A method (a call, answered) or an event (one way, never answered):
    event "in" goes to the server (its fields are args), "out" to the
    client (its fields are results)."""
    def __init__(self, ordinal, name, args, results, doc, later, idempotent, event=None):
        self.ordinal, self.name, self.args, self.results, self.doc = \
            ordinal, name, args, results, doc
        self.later = later    # the server may answer after its handler returned
        self.idempotent = idempotent   # running it again changes nothing more
        self.event = event

    @property
    def fields(self):
        return self.args if self.event == "in" else self.results


class Protocol:
    def __init__(self, src, lineno, name, pid, methods, doc):
        self.src, self.lineno, self.name, self.pid, self.methods = src, lineno, name, pid, methods
        self.doc = doc   # the comment above the protocol line, copied into the header
        self.consts = []   # (NAME, its C value, the comment above it)
        self.kept = False  # `kept`: its restart is not seen (requests replayed from slots)

    def wire(self, m):
        return (self.pid << 16) | m.ordinal


def fail(src, lineno, msg):
    sys.exit(f"{src}:{lineno}: {msg}")


def parse_fields(src, lineno, text, what, allow_handles=False):
    text = text.strip()
    if not (text.startswith("(") and text.endswith(")")):
        fail(src, lineno, f"{what}: want '(<type> <name>, ...)' or '()'")
    body = text[1:-1].strip()
    fields = []
    if not body:
        return fields
    for part in body.split(","):
        toks = part.split()
        if len(toks) != 2:
            fail(src, lineno, f"{what}: bad field '{part.strip()}' (want '<type> <name>')")
        t, name = toks
        if not IDENT.match(name) or name in C_KEYWORDS:
            fail(src, lineno, f"{what}: bad name '{name}'")
        if name in RESERVED or name.startswith("idl_") or name.startswith("out_"):
            fail(src, lineno, f"{what}: name '{name}' is reserved "
                              f"({', '.join(sorted(RESERVED))}, idl_*, out_*)")
        if any(f.name == name for f in fields):
            fail(src, lineno, f"{what}: '{name}' twice")
        a, v = ARRAY.match(t), VAR.match(t)
        if v:
            n = int(v.group(2))
            if not 1 <= n <= 65535:
                fail(src, lineno, f"{what}: {t}: N not in 1..65535 (the count is a u16)")
            if v.group(1) == "str":
                fields.append(Field("var", "char", name, 1, n, is_str=True))
            else:
                ctype, size = SCALARS[v.group(1)]
                fields.append(Field("var", ctype, name, size, n))
        elif t == "handle":
            if not allow_handles:
                fail(src, lineno, f"{what}: handle arguments aren't supported (if a call fails "
                                  "it can't tell whether they were moved); return handles "
                                  "as results, or send them with channel_write")
            fields.append(Field("handle", "handle_t", name, 0))
        elif a:
            n = int(a.group(1))
            if not 1 <= n <= ARRAY_MAX:
                fail(src, lineno, f"{what}: array size {n} not in 1..{ARRAY_MAX}")
            fields.append(Field("fixed", "uint8_t", name, n, n))
        elif t in SCALARS:
            ctype, size = SCALARS[t]
            fields.append(Field("scalar", ctype, name, size))
        else:
            fail(src, lineno, f"{what}: unknown type '{t}' (want {' '.join(SCALARS)}, u8[N], "
                              "u8[<=N], str[<=N] or handle)")
    for f in fields:
        if f.var and any(g.name == f.name + "_len" for g in fields):
            fail(src, lineno, f"{what}: '{f.name}_len' is {f.name}'s length")
    return fields


# A restart rule saying the server's restart is not seen: a `kept` protocol's.
NOT_SEEN = re.compile(r"restart is\s+not\s+seen")


def restart_paragraphs(lines):
    """The paragraphs of a comment that state a restart rule, each as one
    line."""
    out, cur = [], None
    for l in lines + [""]:
        if RESTART_RULE.match(l):
            cur = [l]
        elif cur is not None and l:
            cur.append(l)
        elif cur is not None:
            out.append(" ".join(cur))
            cur = None
    return out


def tidy(lines):
    """A comment's lines without blank ones at either end."""
    while lines and not lines[0]:
        lines = lines[1:]
    while lines and not lines[-1]:
        lines = lines[:-1]
    return lines


def parse_method(src, lineno, line, p, doc):
    """A method line of protocol p."""
    m = re.match(r"(\d+)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(\(.*?\))?\s*(?:->\s*(\(.*?\)))?"
                 r"((?:\s+[a-z]+)*)$", line)
    want = ("want '<ordinal> <method> (<args>) -> (<results>) [later] [idempotent]', or an "
            "event: '<ordinal> <name> (<fields>) event' (to the server) or "
            "'<ordinal> <name> -> (<fields>) event' (to the client)")
    if not m:
        fail(src, lineno, want)
    ordinal, mname = int(m.group(1)), m.group(2)
    words = m.group(5).split()
    for w in words:
        if w not in KEYWORDS:
            fail(src, lineno, f"unknown keyword '{w}' (want {', '.join(KEYWORDS)})")
        if words.count(w) > 1:
            fail(src, lineno, f"'{w}' twice")
    if mname in C_KEYWORDS:
        fail(src, lineno, f"bad method name '{mname}'")
    if not 1 <= ordinal <= 65535:
        fail(src, lineno, f"ordinal {ordinal} not in 1..65535")
    if any(x.ordinal == ordinal for x in p.methods):
        fail(src, lineno, f"ordinal {ordinal} used twice")
    if any(x.name == mname for x in p.methods):
        fail(src, lineno, f"method '{mname}' defined twice")
    if mname in RESERVED_METHODS or mname.startswith("reply_") or \
            any(mname.endswith(x) for x in ("_until", "_within", "_call", "_send",
                                            "_result")):
        fail(src, lineno, f"method name '{mname}' is reserved "
                          f"({', '.join(RESERVED_METHODS)}, reply_*, *_until, *_within, "
                          "*_call, *_send, *_result)")
    if "event" in words:
        if len(words) > 1:
            fail(src, lineno, f"{mname}: an event is never answered: no 'later' or 'idempotent'")
        if (m.group(3) is None) == (m.group(4) is None):
            fail(src, lineno, f"{mname}: an event's fields go one way: '(<fields>) event' to "
                              "the server, or '-> (<fields>) event' to the client")
        way = "in" if m.group(3) is not None else "out"
        fields = parse_fields(src, lineno, m.group(3) or m.group(4), f"{mname} fields",
                              allow_handles=True)
        if way == "in" and p.kept and any(f.handle for f in fields):
            fail(src, lineno, f"{mname}: '{p.name}' is kept: its requests are replayed from "
                              "slots by a successor, which can't have a dead instance's "
                              "handles, so nothing to its server carries any")
        if sum(1 for f in fields if f.handle) > EVENT_HANDLES_MAX:
            fail(src, lineno, f"{mname}: more than {EVENT_HANDLES_MAX} handles")
        if method_max(fields) > MSG_MAX:
            fail(src, lineno, f"{mname}: the event is {method_max(fields)} bytes (max {MSG_MAX})")
        p.methods.append(Method(ordinal, mname, fields if way == "in" else [],
                                fields if way == "out" else [], doc, False, False, way))
        return
    if m.group(3) is None or m.group(4) is None:
        fail(src, lineno, want)
    args = parse_fields(src, lineno, m.group(3), f"{mname} arguments")
    results = parse_fields(src, lineno, m.group(4), f"{mname} results", allow_handles=True)
    if sum(1 for f in results if f.handle) > REP_HANDLES_MAX:
        fail(src, lineno, f"{mname}: more than {REP_HANDLES_MAX} handle results")
    for size, what in ((method_max(args), "request"), (method_max(results), "reply")):
        if size > MSG_MAX:
            fail(src, lineno, f"{mname}: {what} is {size} bytes (max {MSG_MAX})")
    p.methods.append(Method(ordinal, mname, args, results, doc, "later" in words,
                            "idempotent" in words))


CONST = re.compile(r"const\s+([A-Z][A-Z0-9_]*)\s+(-?(?:0x[0-9a-fA-F]+|\d+))"
                   r"(?:\s*<<\s*(\d+))?$")


def parse_const(src, lineno, line, p, doc):
    """A `const <NAME> <value>` line of protocol p: an integer (decimal or
    0x hex, may be negative), or one shifted left (`1 << 3`), as
    <PROTO>_<NAME> in the header."""
    m = CONST.match(line)
    if not m:
        fail(src, lineno, "want 'const <NAME> <value>': NAME in capitals, the value an integer "
                          "(decimal or 0x hex, maybe negative) or '<integer> << <bits>'")
    name, text, shift = m.group(1), m.group(2), m.group(3)
    if any(c[0] == name for c in p.consts):
        fail(src, lineno, f"const {name} twice")
    v = int(text, 0)
    if shift is not None:
        if v < 0 or int(shift) > 63:
            fail(src, lineno, f"const {name}: shift a positive value by 0..63 bits")
        v <<= int(shift)
    if not -(1 << 63) <= v < (1 << 64):
        fail(src, lineno, f"const {name}: {v} doesn't fit 64 bits")
    if shift is not None:
        c = f"({text}{'u' if v < (1 << 32) else 'ull'} << {shift})"
    elif v < 0:
        c = f"({text}{'' if v >= -(1 << 31) else 'll'})"
    else:
        c = f"{text}{'u' if v < (1 << 32) else 'ull'}"
    p.consts.append((name, c, doc))


def parse(src):
    """The protocols of one .idl file, in the order it defines them."""
    protos, p = [], None
    doc = []       # comment lines right above a statement (none: a blank line between)
    above = []     # every comment line since the last statement, paragraphs apart
    for lineno, raw in enumerate(open(src), 1):
        line = raw.strip()
        if line.startswith("#"):
            text = line[1:].strip()
            if "*/" in text:
                fail(src, lineno, "a comment can't hold '*/' (it is copied into C comments)")
            doc.append(text)
            above.append(text)
            continue
        line = line.split("#", 1)[0].strip()
        if not line:
            doc = []
            if above and above[-1]:
                above.append("")
            continue
        if line.startswith("protocol"):
            toks = line.split()
            kept = toks[3:] == ["kept"]
            if len(toks) != 3 + kept or not IDENT.match(toks[1]) or toks[1] in C_KEYWORDS or \
                    not toks[2].isdigit():
                fail(src, lineno, "want 'protocol <name> <id> [kept]'")
            name, pid = toks[1], int(toks[2])
            if not 1 <= pid <= 65535:
                fail(src, lineno, f"protocol id {pid} not in 1..65535")
            base = os.path.basename(src)
            if not protos and base != name + ".idl":
                fail(src, lineno, f"the first protocol of {base} must be '{base[:-4]}' "
                                  f"('{name}' lives in {name}.idl, or after its first)")
            for q in protos:
                if q.name == name or q.pid == pid:
                    fail(src, lineno, f"protocol '{name}' {pid}: '{q.name}' {q.pid} is in "
                                      "this file already")
            if not any(RESTART_RULE.match(l) for l in above):
                where = "the file's comment" if not protos else "the comment above it"
                fail(src, lineno, f"protocol '{name}': {where} states no restart rule "
                                  "(a paragraph beginning 'Restart:' or 'Reconnect:')")
            unseen = NOT_SEEN.search(" ".join(restart_paragraphs(above)))
            if kept and not unseen:
                fail(src, lineno, f"protocol '{name}' is kept, but its restart rule doesn't "
                                  "say the restart is not seen ('... restart is not seen')")
            if unseen and not kept:
                fail(src, lineno, f"protocol '{name}': its restart rule says the restart is not "
                                  "seen, so mark it kept ('protocol <name> <id> kept')")
            if p is not None and not p.methods:
                fail(src, lineno, f"protocol '{p.name}' has no methods")
            p = Protocol(src, lineno, name, pid, [], tidy(above))
            p.kept = kept
            protos.append(p)
            doc, above = [], []
            continue
        if p is None:
            fail(src, lineno, "the first statement must be 'protocol <name> <id>'")
        if line.startswith("const ") or line == "const":
            parse_const(src, lineno, line, p, doc)
        else:
            parse_method(src, lineno, line, p, doc)
        doc, above = [], []
    if not protos:
        sys.exit(f"{src}: no 'protocol' line")
    if not p.methods:
        sys.exit(f"{src}: protocol '{p.name}' has no methods")
    for q in protos:
        q.methods.sort(key=lambda m: m.ordinal)
    return protos


class IdEntry:
    def __init__(self, lineno, pid, name, where, note):
        self.lineno, self.pid, self.name, self.where, self.note = lineno, pid, name, where, note


def parse_ids(path):
    """The protocol id registry (abi/idl/ids.txt): one entry per id."""
    entries, ids, names = [], {}, {}
    try:
        lines = open(path).read().splitlines()
    except FileNotFoundError:
        sys.exit(f"{path}: missing (the protocol id registry)")
    for lineno, raw in enumerate(lines, 1):
        line, _, note = raw.partition("#")
        toks = line.split()
        if not toks:
            continue
        if len(toks) != 3:
            fail(path, lineno, "want '<id> <name> <where> [# what]'")
        try:
            pid = int(toks[0], 0)
        except ValueError:
            fail(path, lineno, f"bad id '{toks[0]}'")
        name, where = toks[1], toks[2]
        if not 0 <= pid <= 65535:
            fail(path, lineno, f"id {pid} not in 0..65535")
        if not IDENT.match(name) or name in C_KEYWORDS:
            fail(path, lineno, f"bad name '{name}'")
        if pid in ids:
            fail(path, lineno, f"id {pid} listed twice (line {ids[pid]})")
        if name in names:
            fail(path, lineno, f"'{name}' listed twice (line {names[name]})")
        if pid == 0:
            if name != "none" or where != "-":
                fail(path, lineno, "id 0 is reserved: '0 none -'")
        elif not os.path.isfile(where):
            fail(path, lineno, f"'{where}' doesn't exist")
        ids[pid], names[name] = lineno, lineno
        entries.append(IdEntry(lineno, pid, name, where, note.strip()))
    if 0 not in ids:
        sys.exit(f"{path}: id 0 must be listed as reserved: '0 none -'")
    return entries


def check_ids(path, entries, protos):
    """Every .idl protocol is in the registry under its name, id and file,
    and every registry line that names an .idl is one of them."""
    by_name = {e.name: e for e in entries}
    for p in protos:
        e = by_name.get(p.name)
        if e is None:
            fail(p.src, p.lineno, f"protocol '{p.name}' is not in {path}: give it a free id "
                                  f"there ('<id> {p.name} {p.src}')")
        if e.pid != p.pid or e.where != p.src:
            fail(p.src, p.lineno, f"protocol '{p.name}' {p.pid} is listed in {path}:{e.lineno} "
                                  f"as {e.pid} in {e.where}")
    defined = {(p.name, p.src) for p in protos}
    for e in entries:
        if e.where.endswith(".idl") and (e.name, e.where) not in defined:
            fail(path, e.lineno, f"{e.where} defines no protocol '{e.name}'")


def gen_ids(entries):
    """The registry as common.h's IDL_ID_* lines, by id."""
    out = ["/* The protocol id registry (abi/idl/ids.txt): every protocol's id, the",
           " * hand-written ones' too (the top half of the word after their txid). */"]
    w = max(len(e.name) for e in entries) + len("IDL_ID_")
    for e in sorted(entries, key=lambda e: e.pid):
        what = e.where if e.where != "-" else ""
        if e.note:
            what = f"{what}: {e.note}" if what else e.note
        out.append(f"#define {'IDL_ID_' + e.name.upper():<{w}} 0x{e.pid:04x}u   /* {what} */")
    return out


def c_comment(lines, indent=""):
    if not lines:
        return []
    if len(lines) == 1:
        return [f"{indent}/* {lines[0]} */"]
    out = [f"{indent}/* {lines[0]}"]
    out += [f"{indent} * {l}" if l else f"{indent} *" for l in lines[1:]]
    out[-1] += " */"
    return out


def gen_common(entries):
    return "\n".join([
        f"/* {BANNER.format(src='tools/genidl.py')}",
        " *",
        " * What every generated protocol header (<idl/<name>.h>) shares: the",
        " * message headers and the server's handling of messages no method can",
        " * take. The format is described at the top of tools/genidl.py. */",
        "#pragma once",
        "",
        "#include <jam/driver.h>",
        "",
    ] + gen_ids(entries) + [
        "",
        "/* Every request starts with this, every reply with idl_rep_hdr. */",
        "struct idl_req_hdr {",
        "    uint32_t txid;       /* stamped by channel_call */",
        "    uint32_t ordinal;    /* (protocol id << 16) | method ordinal */",
        "} __attribute__((packed));",
        "",
        "struct idl_rep_hdr {",
        "    uint32_t txid;       /* the request's, echoed */",
        "    int32_t  status;     /* OK: the results follow */",
        "} __attribute__((packed));",
        "",
        "/* Bytes of message buffers a generated function keeps on its stack;",
        " * one that needs more (a method's request and reply, a protocol's",
        " * biggest two for a server loop) takes them from the heap, once per call",
        " * (ERR_NO_MEMORY if it can't), or once per <proto>_serve. */",
        f"#define IDL_STACK_MAX {STACK_MAX}",
        "/* n rounded up to 8: where a second buffer starts after a first of n bytes. */",
        "#define IDL_ROUND8(n) (((n) + 7u) & ~7u)",
        "",
        "/* Handles a server reads with a message (only an event takes any: a",
        " * method's request with handles is refused, its handles closed). */",
        "#define IDL_READ_HANDLES 8",
        "/* Handles one reply can carry (methods' handle results). */",
        f"#define IDL_REP_HANDLES {REP_HANDLES_MAX}",
        "",
        "/* Client: the status of a reply of n bytes to a call that wants `want`",
        " * bytes back on success. A server that breaks the format (a positive",
        " * status, a wrong size) is ERR_INTERNAL. */",
        "static inline status_t idl_rep_status(const void *rep, uint32_t n, uint32_t want)",
        "{",
        "    const struct idl_rep_hdr *h = (const struct idl_rep_hdr *)rep;",
        "    if (n < sizeof(*h) || h->status > 0)",
        "        return ERR_INTERNAL;",
        "    if (h->status != OK)",
        "        return n == sizeof(*h) ? h->status : ERR_INTERNAL;",
        "    return n == want ? OK : ERR_INTERNAL;",
        "}",
        "",
    ] + VAR_HELPERS.split("\n") + ["",
        "/* Client: one call on ch: the request q (qn bytes) out, the reply into r",
        " * (rcap bytes, *rn of them) and its handles into rh (rhcap slots, *rhn of",
        " * them). t is a deadline, or with `within` a timeout from when the call",
        " * starts, by the kernel's clock (no clock read here). */",
        "static inline status_t idl_call(handle_t ch, void *q, uint32_t qn, void *r, uint32_t rcap,",
        "                                uint32_t *rn, handle_t *rh, uint32_t rhcap, uint32_t *rhn,",
        "                                bool within, uint64_t t)",
        "{",
        "    if (within)",
        "        return drv_channel_call_within(ch, q, qn, r, rcap, rn, rh, rhcap, rhn, t);",
        "    return drv_channel_call_h(ch, q, qn, r, rcap, rn, rh, rhcap, rhn, t);",
        "}",
        "",
        "static inline void idl_close_all(const handle_t *hs, uint32_t n)",
        "{",
        "    for (uint32_t idl_i = 0; idl_i < n; idl_i++)",
        "        drv_handle_close(hs[idl_i]);",
        "}",
        "",
        "/* Server: answer the request whose first n bytes are at req with a bare",
        " * status. No txid (n < 4), or txid 0 (an event): no reply. A reply that can't be written",
        " * (the client is gone, or its queue is full) is dropped. */",
        "static inline void idl_reply_status(handle_t ch, const void *req, uint32_t n, "
        "status_t st)",
        "{",
        "    if (n < sizeof(uint32_t) || !((const struct idl_req_hdr *)req)->txid)",
        "        return;   /* txid 0: an event, never answered */",
        "    struct idl_rep_hdr idl_r;",
        "    idl_r.txid = ((const struct idl_req_hdr *)req)->txid;",
        "    idl_r.status = st;",
        "    drv_channel_write(ch, &idl_r, sizeof(idl_r), NULL, 0);",
        "}",
        "",
        "/* Take the next message off ch, which didn't fit the caller's buffers",
        " * (n bytes, nh handles), and drop it, closing its handles. *txid gets its",
        " * txid and *got its length (under 4 bytes: no txid, *txid 0). OK, or",
        " * drv_channel_read's status (ERR_SHOULD_WAIT or ERR_BUFFER_TOO_SMALL:",
        " * another reader took it meanwhile), or ERR_NO_MEMORY. */",
        "static inline status_t idl_discard(handle_t ch, uint32_t n, uint32_t nh, uint32_t *txid,",
        "                                   uint32_t *got)",
        "{",
        "    uint8_t *idl_b = (uint8_t *)drv_malloc(n ? n : 1);",
        "    handle_t *idl_hs = (handle_t *)drv_malloc(nh ? nh * sizeof(handle_t) : 1);",
        "    status_t idl_st = idl_b && idl_hs ? OK : ERR_NO_MEMORY;",
        "    uint32_t idl_n = 0, idl_nh = 0;",
        "    *txid = 0;",
        "    *got = 0;",
        "    if (idl_st == OK)",
        "        idl_st = drv_channel_read(ch, idl_b, n, &idl_n, idl_hs, nh, &idl_nh);",
        "    if (idl_st == OK) {",
        "        idl_close_all(idl_hs, idl_nh);",
        "        *got = idl_n;",
        "        if (idl_n >= sizeof(uint32_t))",
        "            *txid = ((const struct idl_req_hdr *)idl_b)->txid;",
        "    }",
        "    drv_free(idl_hs);",
        "    drv_free(idl_b);",
        "    return idl_st;",
        "}",
        "",
        "/* Server: the next message didn't fit (n bytes, nh handles): bigger than",
        " * any request of the protocol. Take it off the queue anyway (or it would",
        " * block every later one), close its handles, answer ERR_INVALID_ARGS",
        " * (but not a txid of 0: an event's, never answered). */",
        "static inline status_t idl_drain(handle_t ch, uint32_t n, uint32_t nh)",
        "{",
        "    uint32_t idl_txid = 0, idl_got = 0;",
        "    status_t idl_st = idl_discard(ch, n, nh, &idl_txid, &idl_got);",
        "    if (idl_st == OK && idl_got >= sizeof(uint32_t) && idl_txid) {",
        "        struct idl_rep_hdr idl_r = { idl_txid, ERR_INVALID_ARGS };",
        "        drv_channel_write(ch, &idl_r, sizeof(idl_r), NULL, 0);",
        "    } else if (idl_st == ERR_BUFFER_TOO_SMALL || idl_st == ERR_SHOULD_WAIT) {",
        "        idl_st = OK;   /* another reader took it meanwhile: look again */",
        "    }",
        "    return idl_st;",
        "}",
        "",
        "/* ---- serving ----------------------------------------------------------- */",
        "",
        "/* A <proto>_serve loop between its system calls: the reply waiting to go",
        " * out, and the request just taken. */",
        "struct idl_serve {",
        "    handle_t  ch;     /* the channel served (not owned) */",
        "    void     *q;      /* the request buffer: qcap bytes */",
        "    uint32_t  qcap;",
        "    uint32_t  n;      /* the request's bytes, once taken */",
        "    void     *r;      /* the reply: rn bytes, 0 when there is none to send */",
        "    uint32_t  rn;",
        "    handle_t *rhs;    /* the reply's handles: rhn of them, moved once it is sent */",
        "    uint32_t  rhn;",
        "    handle_t *hs;     /* the request's handles, nh of them, into room for hcap:",
        "                       * hcap 0 (a protocol whose events carry none) leaves a",
        "                       * request with handles queued, for idl_drain */",
        "    uint32_t  hcap;",
        "    uint32_t  nh;",
        "};",
        "",
        "/* Server: send the reply waiting in s, if any, and take the next request",
        " * into s->q, in one system call (drv_channel_reply_wait), waiting for one",
        " * if none is queued. OK: s->n bytes of a request are in s->q, and its",
        " * s->nh handles in s->hs. A request carrying more than s->hcap handles,",
        " * or too big for s->q, is answered ERR_INVALID_ARGS (idl_drain) and the",
        " * next one taken. A reply that can't",
        " * go out (the client is gone, or never called) is dropped (its handles",
        " * gone all the same, as idl_reply_write's). Otherwise the wait's status:",
        " * ERR_PEER_CLOSED when the client is gone for good and nothing is",
        " * queued, ERR_CANCELED when the thread is being killed. */",
        "static inline status_t idl_serve_next(struct idl_serve *s)",
        "{",
        "    for (;;) {",
        "        bool idl_reply = s->rn != 0;",
        "        /* The reply's status (1: never a status, the reply wasn't tried)",
        "         * and the request's sizes, in a row: the kernel writes them in one",
        "         * copy. */",
        "        struct { status_t rs; uint32_t n, nh; } idl_o = { 1, 0, 0 };",
        "        /* A request with more handles than s->hcap (0 for most protocols)",
        "         * stays queued (ERR_BUFFER_TOO_SMALL) for idl_drain. */",
        "        struct channel_reply_wait_args idl_a = {",
        "            .h = idl_reply ? s->ch : HANDLE_INVALID,",
        "            .wait = s->ch,",
        "            .rd_bytes = (uint64_t)(uintptr_t)s->q,",
        "            .rd_cap = s->qcap,",
        "            .rd_handles = (uint64_t)(uintptr_t)s->hs,",
        "            .rd_hcap = s->hcap,",
        "            .rd_actual = (uint64_t)(uintptr_t)&idl_o.n,",
        "            .rd_hactual = (uint64_t)(uintptr_t)&idl_o.nh,",
        "            .deadline_ns = DEADLINE_NEVER,",
        "        };",
        "        if (idl_reply) {",
        "            idl_a.wr_bytes = (uint64_t)(uintptr_t)s->r;",
        "            idl_a.wr_len = s->rn;",
        "            idl_a.wr_handles = (uint64_t)(uintptr_t)s->rhs;",
        "            idl_a.wr_nhandles = s->rhn;",
        "            idl_a.reply_status = (uint64_t)(uintptr_t)&idl_o.rs;",
        "        }",
        "        status_t idl_st = drv_channel_reply_wait(&idl_a);",
        "        /* The reply's handles are gone whatever the status (a send's",
        "         * handles are always moved): never closed here, as their values",
        "         * may already be another thread's new handles. */",
        "        s->rn = s->rhn = 0;",
        "        if (idl_reply && idl_o.rs != 1 && idl_o.rs != OK && idl_o.rs != ERR_PEER_CLOSED)",
        "            continue;   /* the reply failed and nothing was read: wait again */",
        "        if (idl_st == ERR_BUFFER_TOO_SMALL) {",
        "            idl_st = idl_drain(s->ch, idl_o.n, idl_o.nh);",
        "            if (idl_st == OK)",
        "                continue;",
        "        }",
        "        if (idl_st != OK)",
        "            return idl_st;",
        "        s->n = idl_o.n;",
        "        s->nh = idl_o.nh;",
        "        return OK;",
        "    }",
        "}",
        "",
        "/* ---- requests in slots (a server whose requests outlive it) ------------ */",
        "",
        "/* A request slot: memory the server chooses (a service's state VMO,",
        " * <svcstate.h>), which one request is read into and its reply built in,",
        " * so that the request in progress outlives the process: a successor finds",
        " * it there and runs it again with the same code (<proto>_run_slot). */",
        "struct idl_slot {",
        "    void     *q;      /* the request: qcap bytes of room, the protocol's REQ_MAX or more */",
        "    uint32_t  qcap;",
        "    uint32_t *n;      /* its length: written by the kernel after its bytes, 0 before */",
        "    uint32_t *nh;     /* how many handles it carried ... */",
        "    handle_t *hs;     /* ... into room for hcap of them (no method takes any) */",
        "    uint32_t  hcap;",
        "    void     *r;      /* its reply: the protocol's REP_MAX bytes of room */",
        "};",
        "",
        "/* idl_take's end: what a read into slot that returned st leaves there. */",
        "static inline status_t idl_taken(handle_t ch, const struct idl_slot *slot, status_t st)",
        "{",
        "    if (st == ERR_BUFFER_TOO_SMALL) {",
        "        /* The kernel wrote the size of what stays queued: no request here. */",
        "        uint32_t idl_n = *slot->n, idl_nh = *slot->nh;",
        "        __atomic_store_n(slot->n, 0, __ATOMIC_RELEASE);",
        "        *slot->nh = 0;",
        "        return idl_drain(ch, idl_n, idl_nh);",
        "    }",
        "    if (st == OK && *slot->n < sizeof(uint32_t)) {",
        "        idl_close_all(slot->hs, *slot->nh);",
        "        __atomic_store_n(slot->n, 0, __ATOMIC_RELEASE);",
        "        *slot->nh = 0;",
        "    }",
        "    return st;",
        "}",
        "",
        "/* Server: read the next message on ch into slot. Its length (*slot->n) is",
        " * zeroed first and written by the kernel after the bytes, in the one",
        " * system call that takes the message off the queue, and a thread finishes",
        " * the system call it is in before it dies: so a process killed at any",
        " * point leaves the message either still queued or wholly in the slot.",
        " * OK: a message was taken. *slot->n 4 or more: a request to run",
        " * (<proto>_run_slot), with *slot->nh handles in slot->hs, the caller's to",
        " * close (no method takes any: <proto>_run_slot refuses it). *slot->n 0:",
        " * nothing to run, the message dealt with as <proto>_serve_one deals with",
        " * it: one too big for the slot or with more than hcap handles answered",
        " * ERR_INVALID_ARGS (idl_drain), one under 4 bytes (no txid) dropped, its",
        " * handles closed. Otherwise channel_read's status: ERR_SHOULD_WAIT when",
        " * nothing is queued, ERR_PEER_CLOSED when the client is gone for good. */",
        "static inline status_t idl_take(handle_t ch, const struct idl_slot *slot)",
        "{",
        "    __atomic_store_n(slot->n, 0, __ATOMIC_RELEASE);",
        "    *slot->nh = 0;",
        "    status_t idl_st = drv_channel_read(ch, slot->q, slot->qcap, slot->n, slot->hs,",
        "                                       slot->hcap, slot->nh);",
        "    return idl_taken(ch, slot, idl_st);",
        "}",
        "",
        "/* A reply that waits to go out with the server's next system call (a",
        " * take, idl_take_after; a wait on its port, idl_wait_after; or",
        " * idl_reply_flush), so that answering costs no system call of its own.",
        " * The kernel sets *mark (a slot's, <svcstate.h>) once the reply went",
        " * out, in that same call: a successor knows exactly whether it did. */",
        "struct idl_reply {",
        "    handle_t    ch;      /* the request's channel; HANDLE_INVALID: nothing waits */",
        "    const void *r;       /* the reply (a slot's reply area): rn bytes */",
        "    uint32_t    rn;",
        "    uint32_t    nh;      /* handles in hs: gone once the reply is tried */",
        "    handle_t    hs[IDL_REP_HANDLES];",
        "    uint64_t   *mark;    /* set to 1 by the kernel once it went out; NULL: none */",
        "};",
        "",
        "/* The reply half of a channel_reply_wait for the reply waiting in rep (its",
        " * status into *rs). false: nothing waits (a->h HANDLE_INVALID). */",
        "static inline bool idl_reply_args(const struct idl_reply *rep, struct channel_reply_wait_args *a,",
        "                                  status_t *rs)",
        "{",
        "    a->h = HANDLE_INVALID;",
        "    if (!rep || rep->ch == HANDLE_INVALID)",
        "        return false;",
        "    a->h = rep->ch;",
        "    a->wr_bytes = (uint64_t)(uintptr_t)rep->r;",
        "    a->wr_len = rep->rn;",
        "    a->wr_handles = (uint64_t)(uintptr_t)rep->hs;",
        "    a->wr_nhandles = rep->nh;",
        "    a->reply_status = (uint64_t)(uintptr_t)rs;",
        "    a->mark = (uint64_t)(uintptr_t)rep->mark;",
        "    return true;",
        "}",
        "",
        "/* After a channel_reply_wait that tried rep's reply (its status rs: 1 if",
        " * the kernel never got to it): nothing waits any more, and its handles",
        " * are gone whatever rs says (a send's handles are always moved; a client",
        " * that is gone, or whose queue is full, loses its reply). Never closed",
        " * here: their values may already be another thread's new handles. true:",
        " * the reply failed and the call ended there, nothing taken: make it again",
        " * without the reply. */",
        "static inline bool idl_reply_tried(struct idl_reply *rep, status_t rs)",
        "{",
        "    rep->ch = HANDLE_INVALID;",
        "    rep->nh = 0;",
        "    return rs != 1 && rs != OK && rs != ERR_PEER_CLOSED;",
        "}",
        "",
        "/* Server: idl_take, sending the reply waiting in *rep first in the same",
        " * system call (channel_reply_wait, its wait 0: it never waits), whatever",
        " * channel that reply is for. The reply's mark is set once it went out,",
        " * before the read. Results as idl_take's (ERR_SHOULD_WAIT: nothing is",
        " * queued, and the reply went out); *rep is empty after. */",
        "static inline status_t idl_take_after(handle_t ch, const struct idl_slot *slot,",
        "                                      struct idl_reply *rep)",
        "{",
        "    for (;;) {",
        "        status_t idl_rs = 1;   /* never a status: the reply wasn't tried */",
        "        __atomic_store_n(slot->n, 0, __ATOMIC_RELEASE);",
        "        *slot->nh = 0;",
        "        struct channel_reply_wait_args idl_a = {",
        "            .wait = ch,",
        "            .rd_bytes = (uint64_t)(uintptr_t)slot->q,",
        "            .rd_cap = slot->qcap,",
        "            .rd_handles = (uint64_t)(uintptr_t)slot->hs,",
        "            .rd_hcap = slot->hcap,",
        "            .rd_actual = (uint64_t)(uintptr_t)slot->n,",
        "            .rd_hactual = (uint64_t)(uintptr_t)slot->nh,",
        "            .flags = CHANNEL_REPLY_WAIT_TIMEOUT,",
        "            .deadline_ns = 0,",
        "        };",
        "        bool idl_reply = idl_reply_args(rep, &idl_a, &idl_rs);",
        "        status_t idl_st = drv_channel_reply_wait(&idl_a);",
        "        if (idl_reply && idl_reply_tried(rep, idl_rs))",
        "            continue;",
        "        if (idl_st == ERR_TIMED_OUT)",
        "            idl_st = ERR_SHOULD_WAIT;   /* a wait of 0: nothing was queued */",
        "        return idl_taken(ch, slot, idl_st);",
        "    }",
        "}",
        "",
        "/* Server: drv_port_wait, sending the reply waiting in *rep first in the",
        " * same system call (channel_reply_wait on the port). Results as",
        " * port_wait's; *rep is empty after. */",
        "static inline status_t idl_wait_after(handle_t port, uint64_t deadline_ns,",
        "                                      struct port_packet *pkt, struct idl_reply *rep)",
        "{",
        "    for (;;) {",
        "        status_t idl_rs = 1;",
        "        struct channel_reply_wait_args idl_a = {",
        "            .wait = port, .packet = (uint64_t)(uintptr_t)pkt, .deadline_ns = deadline_ns,",
        "        };",
        "        if (!idl_reply_args(rep, &idl_a, &idl_rs))",
        "            return drv_port_wait(port, deadline_ns, pkt);",
        "        status_t idl_st = drv_channel_reply_wait(&idl_a);",
        "        if (!idl_reply_tried(rep, idl_rs))",
        "            return idl_st;",
        "    }",
        "}",
        "",
        "/* Server: the reply waiting in *rep (if any) out now, in a system call of",
        " * its own: a channel_reply_wait on the reply's own channel with no room",
        " * for a message and a wait of 0, so a request queued there stays queued",
        " * (but one of 0 bytes and no handles, which no protocol answers, is taken",
        " * and so dropped). The reply's mark is set once it went out. true: a",
        " * message may be queued on that channel (or it can't be told: the reply",
        " * failed); false: nothing is, or nothing waited. */",
        "static inline bool idl_reply_flush(struct idl_reply *rep)",
        "{",
        "    status_t idl_rs = 1;",
        "    struct channel_reply_wait_args idl_a = {",
        "        .flags = CHANNEL_REPLY_WAIT_TIMEOUT, .deadline_ns = 0,",
        "    };",
        "    if (!idl_reply_args(rep, &idl_a, &idl_rs))",
        "        return false;",
        "    idl_a.wait = rep->ch;",
        "    status_t idl_st = drv_channel_reply_wait(&idl_a);",
        "    if (idl_reply_tried(rep, idl_rs))",
        "        return true;",
        "    return idl_st == OK || idl_st == ERR_BUFFER_TOO_SMALL;",
        "}",
        "",
        "/* Server: the request in slot carried handles: answered ERR_INVALID_ARGS",
        " * into slot->r without its handler seeing it. The reply's length (0 for",
        " * txid 0: an event's, never answered). */",
        "static inline uint32_t idl_refuse(const struct idl_slot *slot)",
        "{",
        "    struct idl_rep_hdr *idl_r = (struct idl_rep_hdr *)slot->r;",
        "    idl_r->txid = ((const struct idl_req_hdr *)slot->q)->txid;",
        "    idl_r->status = ERR_INVALID_ARGS;",
        "    return idl_r->txid ? sizeof(*idl_r) : 0;   /* txid 0: an event, never answered */",
        "}",
        "",
        "/* ---- answering later (a `later` method, or a loop of your own) ------- */",
        "",
        "/* A request to be answered later: the channel it came on (not owned:",
        " * keep it open until the reply is written) and its txid. A `later`",
        " * method's handler gets it; <proto>_reply_<method> answers it. */",
        "struct idl_txn {",
        "    handle_t ch;",
        "    uint32_t txid;",
        "};",
        "",
        "/* What a `later` method's handler returns when it answers with",
        " * <proto>_reply_<method> (now or later) instead of through its results.",
        " * Never a status on the wire. */",
        "#define IDL_LATER 1",
        "",
        "/* Server: write the reply rep (n bytes; its txid is set here) with nh",
        " * handles to txn. The handles are gone in every case, sent or not (a",
        " * send's handles are always moved; ERR_PEER_CLOSED: the client is gone;",
        " * a client that gave up waiting still gets the reply, as a message of",
        " * its own): the caller never closes them after this. */",
        "static inline status_t idl_reply_write(struct idl_txn txn, void *rep, uint32_t n,",
        "                                       handle_t *hs, uint32_t nh)",
        "{",
        "    ((struct idl_rep_hdr *)rep)->txid = txn.txid;",
        "    return drv_channel_write(txn.ch, rep, n, hs, nh);",
        "}",
        "",
        "/* ---- asynchronous calls ------------------------------------------------ */",
        "",
        "/* Client: the next txid from the counter *last: never 0, and never with",
        " * CHANNEL_TXID_KERNEL (bit 31) set, which marks the txids the kernel",
        " * stamps on blocking calls: after 0x7fffffff it wraps to 1. Use one",
        " * counter per channel end. */",
        "static inline uint32_t idl_txid_next(uint32_t *last)",
        "{",
        "    *last = (*last + 1) & ~CHANNEL_TXID_KERNEL;",
        "    if (*last == 0)",
        "        *last = 1;",
        "    return *last;",
        "}",
        "",
        "/* A reply read off a channel, for <proto>_<method>_result. */",
        "struct idl_msg {",
        "    uint32_t txid;                    /* its txid: which call it answers */",
        "    uint32_t n;                       /* its bytes */",
        "    uint32_t nh;                      /* handles in hs, still to be taken */",
        "    handle_t hs[IDL_REP_HANDLES];",
        "};",
        "",
        "/* Client: close the handles of a reply nobody takes (its txid matches no",
        " * call: a reply to a call that gave up). */",
        "static inline void idl_msg_drop(struct idl_msg *m)",
        "{",
        "    idl_close_all(m->hs, m->nh);",
        "    m->nh = 0;",
        "}",
        "",
        "/* Client: read the next reply off ch into rep (cap bytes: the protocol's",
        " * <PROTO>_REP_MAX) and *m, then find its call by m->txid and decode it",
        " * with that method's <proto>_<method>_result (or idl_msg_drop it).",
        " * ERR_SHOULD_WAIT: nothing queued; ERR_PEER_CLOSED: the server is gone",
        " * (every call still waiting fails). A message too big for rep or with",
        " * more than IDL_REP_HANDLES handles, or without a txid, is no reply of",
        " * the protocol: dropped, its handles closed, ERR_INTERNAL with m->txid",
        " * set (0 if it had none) so its call can fail. */",
        "static inline status_t idl_reply_read(handle_t ch, void *rep, uint32_t cap, struct idl_msg *m)",
        "{",
        "    m->txid = m->n = m->nh = 0;",
        "    status_t idl_st = drv_channel_read(ch, rep, cap, &m->n, m->hs, IDL_REP_HANDLES, &m->nh);",
        "    if (idl_st == ERR_BUFFER_TOO_SMALL) {",
        "        uint32_t idl_got = 0;",
        "        idl_st = idl_discard(ch, m->n, m->nh, &m->txid, &idl_got);",
        "        m->n = m->nh = 0;",
        "        return idl_st == ERR_NO_MEMORY ? idl_st : ERR_INTERNAL;",
        "    }",
        "    if (idl_st != OK)",
        "        return idl_st;",
        "    if (m->n < sizeof(uint32_t)) {",
        "        idl_msg_drop(m);",
        "        return ERR_INTERNAL;",
        "    }",
        "    m->txid = ((const struct idl_rep_hdr *)rep)->txid;",
        "    return OK;",
        "}",
        "",
    ])


def fill_request(m, U, q, sep="."):
    """Lines that fill request struct `q` (q.x, or q->x with sep "->") from
    m's arguments (txid set by the caller)."""
    out = [f"    {q}{sep}ordinal = {U}_{m.name.upper()};"]
    for f in m.args:
        if f.handle or f.var:
            continue
        if f.array:
            out.append(f"    for (uint32_t idl_i = 0; idl_i < {f.array}; idl_i++)")
            out.append(f"        {q}{sep}{f.name}[idl_i] = {f.name}[idl_i];")
        else:
            out.append(f"    {q}{sep}{f.name} = {f.name};")
    return out


def method_max(fields):
    """Bytes of the biggest message with these fields."""
    return var_layout(fields)[1]


def has_var(fields):
    return any(f.var for f in fields)


def general(m):
    """Does m need the general code (buffers of its own size, on the heap
    when big; varying arrays) rather than the plain one (its two structs on
    the stack)?"""
    return has_var(m.args) or has_var(m.results) or \
        method_max(m.args) + method_max(m.results) > STACK_MAX


def buffers(sizes, fail_ret):
    """Lines declaring byte buffers idl_b0, idl_b1, ... of these sizes: on the
    stack, or one allocation when together they pass STACK_MAX (fail_ret:
    the line that returns when it can't be had). Returns (lines, free line
    or None)."""
    if sum(sizes) <= STACK_MAX:
        return [f"    _Alignas(8) uint8_t idl_b{i}[{n}];" for i, n in enumerate(sizes)], None
    total = " + ".join(f"IDL_ROUND8({n})" for n in sizes[:-1]) + \
        (" + " if len(sizes) > 1 else "") + str(sizes[-1])
    out = [f"    uint8_t *idl_b0 = (uint8_t *)drv_malloc({total});",
           "    if (!idl_b0)",
           f"        {fail_ret}"]
    for i in range(1, len(sizes)):
        out.append(f"    uint8_t *idl_b{i} = idl_b{i - 1} + IDL_ROUND8({sizes[i - 1]});")
    return out, "    drv_free(idl_b0);"


def copy_results(m, cond, r):
    """Lines that copy reply `r`'s plain results to the caller's out_ pointers
    (when `cond` holds, if given)."""
    out = []
    pre = f"{cond} && " if cond else ""
    for f in m.results:
        if f.handle or f.var:
            continue
        if f.array:
            out.append(f"    for (uint32_t idl_i = 0; {pre}out_{f.name} && "
                       f"idl_i < {f.array}; idl_i++)")
            out.append(f"        out_{f.name}[idl_i] = {r}{f.name}[idl_i];")
        else:
            out.append(f"    if ({pre}out_{f.name})")
            out.append(f"        *out_{f.name} = {r}{f.name};")
    return out


def put_vars(fields, buf, off, err, ind="    "):
    """Lines that append fields' varying arrays to the message in buf (its
    length in `off`), setting idl_st to err if one is too long (or not a
    str), once idl_st is OK."""
    out = []
    for f in fields:
        if not f.var:
            continue
        ok = f"idl_put({buf}, &{off}, {f.name}, {f.name}_len, {f.max}, {f.size})"
        if f.str:
            ok += f" ||\n{ind}                         !idl_str_ok({f.name}, {f.name}_len)"
            ok = f"!{ok}"
            out.append(f"{ind}if (idl_st == OK && ({ok}))")
        else:
            out.append(f"{ind}if (idl_st == OK && !{ok})")
        out.append(f"{ind}    idl_st = {err};")
    return out


def get_vars(fields, buf, n, err, ind="    "):
    """Lines that read fields' varying arrays from the message in buf (n
    bytes; idl_o past its fixed part) into idl_p<k> and idl_l<k>, setting
    idl_st to err if they don't fill the message exactly, once idl_st is
    OK."""
    out = []
    for k, f in enumerate(f for f in fields if f.var):
        out.append(f"{ind}const void *idl_p{k} = NULL;")
        out.append(f"{ind}uint32_t idl_l{k} = 0;")
    for k, f in enumerate(f for f in fields if f.var):
        out.append(f"{ind}if (idl_st == OK && !idl_get({buf}, {n}, &idl_o, {f.max}, {f.size}, "
                   f"&idl_p{k}, &idl_l{k}))")
        out.append(f"{ind}    idl_st = {err};")
        if f.str:
            out.append(f"{ind}if (idl_st == OK && !idl_str_ok((const char *)idl_p{k}, idl_l{k}))")
            out.append(f"{ind}    idl_st = {err};")
    if any(f.var for f in fields):
        out.append(f"{ind}if (idl_st == OK && idl_o != {n})")
        out.append(f"{ind}    idl_st = {err};")
    return out


def copy_vars(m, cond):
    """Lines that copy the varying results read by get_vars to the caller's
    out_ arrays (a str's with a NUL after it) and lengths."""
    out = []
    pre = f"{cond} && " if cond else ""
    for k, f in enumerate(f for f in m.results if f.var):
        out.append(f"    if ({pre}out_{f.name}) {{")
        out.append(f"        if (idl_l{k})")
        out.append(f"            __builtin_memcpy(out_{f.name}, idl_p{k}, idl_l{k} * {f.size});")
        if f.str:
            out.append(f"        out_{f.name}[idl_l{k}] = 0;")
        out.append("    }")
        out.append(f"    if ({pre}out_{f.name}_len)")
        out.append(f"        *out_{f.name}_len = idl_l{k};")
    return out


def gen_call_general(p, m):
    """The body of <proto>_<method>_call in buffers of the method's own size."""
    P, U = p.name, p.name.upper()
    qmax, rmax = method_max(m.args), method_max(m.results)
    out, free = buffers([qmax, rmax], "return ERR_NO_MEMORY;")
    out.append(f"    struct {P}_{m.name}_req *idl_q = (struct {P}_{m.name}_req *)idl_b0;")
    out.append(f"    const struct {P}_{m.name}_rep *idl_r = (const struct {P}_{m.name}_rep *)idl_b1;")
    out.append("    uint32_t idl_qn = sizeof(*idl_q), idl_o = sizeof(*idl_r);")
    out.append("    uint32_t idl_got[2] = { 0, 0 };   /* bytes, handles: in a row, one copy-out */")
    out.append("    status_t idl_st = OK;")
    out.append("    idl_q->txid = 0;")
    out += fill_request(m, U, "idl_q", "->")
    out += put_vars(m.args, "idl_b0", "idl_qn", "ERR_INVALID_ARGS")
    hres = [f for f in m.results if f.handle]
    if hres:
        out.append(f"    handle_t idl_rh[{len(hres)}];")
    rh = (f"idl_rh, {len(hres)}, &idl_got[1]") if hres else "NULL, 0, &idl_got[1]"
    out.append("    if (idl_st == OK)")
    out.append(f"        idl_st = idl_call(ch, idl_b0, idl_qn, idl_b1, {rmax}, &idl_got[0], {rh},")
    out.append("                          idl_within, idl_t);")
    if hres:
        # ERR_BUFFER_TOO_SMALL writes the reply's handle count, but none of
        # its handles reach our table (nor idl_rh): nothing to close then.
        out.append("    if (idl_st != OK)")
        out.append("        idl_got[1] = 0;   /* handles arrive only with OK */")
    status = "idl_rep_status_var" if has_var(m.results) else "idl_rep_status"
    out.append("    if (idl_st == OK)")
    out.append(f"        idl_st = {status}(idl_b1, idl_got[0], sizeof(*idl_r));")
    out += get_vars(m.results, "idl_b1", "idl_got[0]", "ERR_INTERNAL")
    if hres:
        out.append(f"    if (idl_st == OK && idl_got[1] != {len(hres)})")
        out.append("        idl_st = ERR_INTERNAL;")
        out.append("    if (idl_st != OK)")
        out.append("        idl_close_all(idl_rh, idl_got[1]);")
    for k, f in enumerate(hres):
        out.append(f"    if (idl_st == OK) {{")
        out.append(f"        if (out_{f.name})")
        out.append(f"            *out_{f.name} = idl_rh[{k}];")
        out.append("        else")
        out.append(f"            drv_handle_close(idl_rh[{k}]);")
        out.append("    }")
    out += copy_results(m, "idl_st == OK", "idl_r->")
    out += copy_vars(m, "idl_st == OK")
    if not has_var(m.results):
        out.append("    (void)idl_o;")
    if free:
        out.append(free)
    out.append("    return idl_st;")
    return out


def gen_client_sync(p, m):
    P, U = p.name, p.name.upper()
    params = ["handle_t ch", "bool idl_within", "uint64_t idl_t"] + in_params(m.args) + \
             out_params(m.results)
    out = [""]
    out.append(f"/* {P}_{m.name}_until and _within: idl_t is a deadline, or with idl_within a")
    out.append(" * timeout from when the call starts (the kernel's clock). */")
    out.append(f"static inline status_t {P}_{m.name}_call({', '.join(params)})")
    out.append("{")
    if general(m):
        out += gen_call_general(p, m)
        out.append("}")
        return out + gen_call_wrappers(p, m)
    out.append(f"    struct {P}_{m.name}_req idl_q;")
    out.append(f"    struct {P}_{m.name}_rep idl_r;")
    out.append("    uint32_t idl_n = 0;")
    out.append("    idl_q.txid = 0;")
    out += fill_request(m, U, "idl_q")
    hres = [f for f in m.results if f.handle]
    if hres:
        out.append(f"    handle_t idl_rh[{len(hres)}];")
        out.append("    uint32_t idl_got[2] = { 0, 0 };   /* bytes, handles: in a row, one copy-out */")
        out.append("    status_t idl_st = idl_call(ch, &idl_q, sizeof(idl_q), &idl_r, sizeof(idl_r), "
                   "&idl_got[0],")
        out.append(f"                               idl_rh, {len(hres)}, &idl_got[1], idl_within, "
                   "idl_t);")
        out.append("    uint32_t idl_rhn = idl_st == OK ? idl_got[1] : 0;   /* handles arrive only with OK */")
        out.append("    idl_n = idl_got[0];")
        out.append("    if (idl_st == OK)")
        out.append("        idl_st = idl_rep_status(&idl_r, idl_n, sizeof(idl_r));")
        out.append(f"    if (idl_st == OK && idl_rhn != {len(hres)})")
        out.append("        idl_st = ERR_INTERNAL;")
        out.append("    if (idl_st != OK)")
        out.append("        idl_close_all(idl_rh, idl_rhn);")
    else:
        out.append("    status_t idl_st = idl_call(ch, &idl_q, sizeof(idl_q), &idl_r, sizeof(idl_r), "
                   "&idl_n, NULL, 0,")
        out.append("                               NULL, idl_within, idl_t);")
        out.append("    if (idl_st == OK)")
        out.append("        idl_st = idl_rep_status(&idl_r, idl_n, sizeof(idl_r));")
    for k, f in enumerate(hres):
        out.append(f"    if (idl_st == OK) {{")
        out.append(f"        if (out_{f.name})")
        out.append(f"            *out_{f.name} = idl_rh[{k}];")
        out.append("        else")
        out.append(f"            drv_handle_close(idl_rh[{k}]);")
        out.append("    }")
    out += copy_results(m, "idl_st == OK", "idl_r.")
    out.append("    return idl_st;")
    out.append("}")
    return out + gen_call_wrappers(p, m)


def gen_call_wrappers(p, m):
    """<proto>_<method>, _until and _within, over _call."""
    P = p.name
    out = []
    rest = in_params(m.args) + out_params(m.results)
    names = [re.match(r".*?(\w+)(\[.*\])?$", x).group(1) for x in rest]
    for suffix, time, call in (("_until", "uint64_t deadline_ns", "false, deadline_ns"),
                               ("_within", "uint64_t timeout_ns", "true, timeout_ns"),
                               ("", None, "false, DEADLINE_NEVER")):
        if suffix == "_until":
            out += c_comment(m.doc)
        params = ["handle_t ch"] + ([time] if time else []) + rest
        out.append(f"static inline status_t {P}_{m.name}{suffix}({', '.join(params)})")
        out.append("{")
        out.append(f"    return {P}_{m.name}_call({', '.join(['ch', call] + names)});")
        out.append("}")
    return out


def gen_client_async(p, m):
    P, U = p.name, p.name.upper()
    out = [""]
    params = ["handle_t ch", "uint32_t idl_txid"] + in_params(m.args)
    out.append(f"/* {P}_{m.name} without waiting: the request, with the caller's txid (not 0).")
    out.append(f" * The reply comes on ch: idl_reply_read, then {P}_{m.name}_result. */")
    out.append(f"static inline status_t {P}_{m.name}_send({', '.join(params)})")
    out.append("{")
    if general(m):
        out.append("    if (!idl_txid)")
        out.append("        return ERR_INVALID_ARGS;")
        bufs, free = buffers([method_max(m.args)], "return ERR_NO_MEMORY;")
        out += bufs
        out.append(f"    struct {P}_{m.name}_req *idl_q = (struct {P}_{m.name}_req *)idl_b0;")
        out.append("    uint32_t idl_qn = sizeof(*idl_q);")
        out.append("    status_t idl_st = OK;")
        out.append("    idl_q->txid = idl_txid;")
        out += fill_request(m, U, "idl_q", "->")
        out += put_vars(m.args, "idl_b0", "idl_qn", "ERR_INVALID_ARGS")
        out.append("    if (idl_st == OK)")
        out.append("        idl_st = drv_channel_write(ch, idl_b0, idl_qn, NULL, 0);")
        if free:
            out.append(free)
        out.append("    return idl_st;")
        out.append("}")
    else:
        out.append(f"    struct {P}_{m.name}_req idl_q;")
        out.append("    if (!idl_txid)")
        out.append("        return ERR_INVALID_ARGS;")
        out.append("    idl_q.txid = idl_txid;")
        out += fill_request(m, U, "idl_q")
        out.append("    return drv_channel_write(ch, &idl_q, sizeof(idl_q), NULL, 0);")
        out.append("}")
    hres = [f for f in m.results if f.handle]
    params = ["const void *idl_rep", "struct idl_msg *idl_m"] + out_params(m.results)
    out.append("")
    out.append(f"/* The status and results of a reply to {P}_{m.name}_send (read with")
    out.append(" * idl_reply_read). The reply's handles are taken in every case: moved to")
    out.append(" * the results, or closed (on a failure, or for a NULL result). */")
    out.append(f"static inline status_t {P}_{m.name}_result({', '.join(params)})")
    out.append("{")
    out.append(f"    const struct {P}_{m.name}_rep *idl_r = (const struct {P}_{m.name}_rep *)idl_rep;")
    if has_var(m.results):
        out.append("    uint32_t idl_o = sizeof(*idl_r);")
        out.append("    status_t idl_st = idl_rep_status_var(idl_rep, idl_m->n, sizeof(*idl_r));")
        out += get_vars(m.results, "idl_rep", "idl_m->n", "ERR_INTERNAL")
    else:
        out.append("    status_t idl_st = idl_rep_status(idl_rep, idl_m->n, sizeof(*idl_r));")
    out.append(f"    if (idl_st == OK && idl_m->nh != {len(hres)})")
    out.append("        idl_st = ERR_INTERNAL;")
    out.append("    if (idl_st != OK) {")
    out.append("        idl_msg_drop(idl_m);")
    out.append("        return idl_st;")
    out.append("    }")
    for k, f in enumerate(hres):
        out.append(f"    if (out_{f.name})")
        out.append(f"        *out_{f.name} = idl_m->hs[{k}];")
        out.append("    else")
        out.append(f"        drv_handle_close(idl_m->hs[{k}]);")
    out.append("    idl_m->nh = 0;")
    if not m.results:
        out.append("    (void)idl_r;")
    out += copy_results(m, None, "idl_r->")
    out += copy_vars(m, None)
    out.append("    return OK;")
    out.append("}")
    return out


def gen_server_reply(p, m):
    P = p.name
    hres = [f for f in m.results if f.handle]
    params = ["struct idl_txn idl_txn", "status_t idl_st"] + in_params(m.results)
    out = [""]
    out.append(f"/* Answer the {P}.{m.name} request kept in txn: idl_st and, if it is OK, the")
    out.append(" * results (handles are moved in every case: sent, or closed). A positive")
    out.append(" * status is ERR_INTERNAL, and so is OK with a handle result left")
    if has_var(m.results):
        out.append(" * HANDLE_INVALID, or a varying result past its N (or a str that isn't")
        out.append(" * one). Returns the write's status (idl_reply_write). */")
    else:
        out.append(" * HANDLE_INVALID. Returns the write's status (idl_reply_write). */")
    out.append(f"static inline status_t {P}_reply_{m.name}({', '.join(params)})")
    out.append("{")
    if general(m):
        return out + gen_reply_general(p, m) + ["}"]
    out.append(f"    struct {P}_{m.name}_rep idl_r;")
    if hres:
        out.append(f"    handle_t idl_hs[{len(hres)}] = {{ {', '.join(f.name for f in hres)} }};")
    out.append("    if (idl_st > 0)")
    out.append("        idl_st = ERR_INTERNAL;")
    if hres:
        ok = " && ".join(f"{f.name} != HANDLE_INVALID" for f in hres)
        out.append(f"    if (idl_st == OK && !({ok}))")
        out.append("        idl_st = ERR_INTERNAL;")
    out.append("    idl_r.status = idl_st;")
    out.append("    if (idl_st != OK)" + (" {" if hres else ""))
    for k, f in enumerate(hres):
        out.append(f"        if (idl_hs[{k}] != HANDLE_INVALID)")
        out.append(f"            drv_handle_close(idl_hs[{k}]);")
    out.append("        return idl_reply_write(idl_txn, &idl_r, sizeof(struct idl_rep_hdr), "
               "NULL, 0);")
    if hres:
        out.append("    }")
    for f in m.results:
        if f.handle:
            continue
        if f.array:
            out.append(f"    for (uint32_t idl_i = 0; idl_i < {f.array}; idl_i++)")
            out.append(f"        idl_r.{f.name}[idl_i] = {f.name}[idl_i];")
        else:
            out.append(f"    idl_r.{f.name} = {f.name};")
    hs = ("idl_hs", len(hres)) if hres else ("NULL", 0)
    out.append(f"    return idl_reply_write(idl_txn, &idl_r, sizeof(idl_r), {hs[0]}, {hs[1]});")
    out.append("}")
    return out


def gen_reply_general(p, m):
    """The body of <proto>_reply_<method> in a buffer of the reply's own size."""
    P = p.name
    hres = [f for f in m.results if f.handle]
    out = []
    if hres:
        out.append(f"    handle_t idl_hs[{len(hres)}] = {{ {', '.join(f.name for f in hres)} }};")
    close = [f"        idl_close_all(idl_hs, {len(hres)});"] if hres else []
    rmax = method_max(m.results)
    free = None
    if rmax <= STACK_MAX:
        out.append(f"    _Alignas(8) uint8_t idl_b0[{rmax}];")
    else:   # no memory: the client still gets an answer, and the handles are closed
        out.append(f"    uint8_t *idl_b0 = (uint8_t *)drv_malloc({rmax});")
        out.append("    if (!idl_b0) {")
        out.append("        struct idl_rep_hdr idl_h = { 0, ERR_NO_MEMORY };")
        out += close
        out.append("        return idl_reply_write(idl_txn, &idl_h, sizeof(idl_h), NULL, 0);")
        out.append("    }")
        free = "    drv_free(idl_b0);"
    out.append(f"    struct {P}_{m.name}_rep *idl_r = (struct {P}_{m.name}_rep *)idl_b0;")
    out.append("    uint32_t idl_rn = sizeof(*idl_r);")
    out.append("    if (idl_st > 0)")
    out.append("        idl_st = ERR_INTERNAL;")
    if hres:
        ok = " && ".join(f"{f.name} != HANDLE_INVALID" for f in hres)
        out.append(f"    if (idl_st == OK && !({ok}))")
        out.append("        idl_st = ERR_INTERNAL;")
    for f in m.results:
        if f.handle or f.var:
            continue
        if f.array:
            out.append(f"    for (uint32_t idl_i = 0; idl_i < {f.array}; idl_i++)")
            out.append(f"        idl_r->{f.name}[idl_i] = {f.name}[idl_i];")
        else:
            out.append(f"    idl_r->{f.name} = {f.name};")
    out += put_vars(m.results, "idl_b0", "idl_rn", "ERR_INTERNAL")
    out.append("    idl_r->status = idl_st;")
    out.append("    if (idl_st != OK) {")
    for k, f in enumerate(hres):
        out.append(f"        if (idl_hs[{k}] != HANDLE_INVALID)")
        out.append(f"            drv_handle_close(idl_hs[{k}]);")
    out.append("        idl_rn = sizeof(struct idl_rep_hdr);")
    out.append("    }")
    hs = (f"idl_st == OK ? idl_hs : NULL, idl_st == OK ? {len(hres)} : 0") if hres else "NULL, 0"
    out.append(f"    status_t idl_ws = idl_reply_write(idl_txn, idl_b0, idl_rn, {hs});")
    if free:
        out.append(free)
    out.append("    return idl_ws;")
    return out


def gen_dispatch_case(p, m):
    P, U = p.name, p.name.upper()
    out = [f"    case {U}_{m.name.upper()}: {{"]
    out.append(f"        const struct {P}_{m.name}_req *idl_q = "
               f"(const struct {P}_{m.name}_req *)req;")
    out.append(f"        struct {P}_{m.name}_rep *idl_r = (struct {P}_{m.name}_rep *)rep;")
    g = general(m)   # fixed arrays straight into the reply (no copy of them on the stack)
    room = dict(var_layout(m.results)[0])   # where each varying result is written
    for f in m.results:
        if f.handle:
            out.append(f"        handle_t out_{f.name} = HANDLE_INVALID;")
        elif f.var:
            out.append(f"        {f.ctype} *out_{f.name} = ({f.ctype} *)((uint8_t *)rep + {room[f]});")
            out.append(f"        uint32_t out_{f.name}_len = 0;")
        elif f.array and g:
            out.append(f"        for (uint32_t idl_i = 0; idl_i < {f.array}; idl_i++)")
            out.append(f"            idl_r->{f.name}[idl_i] = 0;")
        elif f.array:
            out.append(f"        uint8_t out_{f.name}[{f.array}];")
            out.append(f"        for (uint32_t idl_i = 0; idl_i < {f.array}; idl_i++)")
            out.append(f"            out_{f.name}[idl_i] = 0;")
        else:
            out.append(f"        {f.ctype} out_{f.name} = 0;")
    if not m.results:
        out.append("        (void)idl_r;")
    vargs = [f for f in m.args if f.var]
    if vargs:
        out.append("        uint32_t idl_o = sizeof(*idl_q);")
        out.append("        status_t idl_st = n < sizeof(*idl_q) ? ERR_INVALID_ARGS : OK;")
        out += get_vars(m.args, "req", "n", "ERR_INVALID_ARGS", "        ")
        out.append("        if (idl_st != OK)")
    else:
        out.append("        if (n != sizeof(*idl_q))")
    out.append("            return sizeof(*idl_h);")
    out.append(f"        if (!ops->{m.name}) {{")
    out.append("            idl_h->status = ERR_NOT_SUPPORTED;")
    out.append("            return sizeof(*idl_h);")
    out.append("        }")
    call = ["ctx"] + (["idl_txn"] if m.later else [])
    k = 0
    for f in m.args:
        if f.var:
            cast = "const char" if f.str else f"const {f.ctype}"
            call += [f"({cast} *)idl_p{k}", f"idl_l{k}"]
            k += 1
        else:
            call.append(f"idl_q->{f.name}")
    for f in m.results:
        if f.var:
            call += [f"out_{f.name}", f"&out_{f.name}_len"]
        elif f.array:
            call.append(f"idl_r->{f.name}" if g else f"out_{f.name}")
        else:
            call.append(f"&out_{f.name}")
    if m.later:
        out.append("        struct idl_txn idl_txn = { ch, idl_h->txid };")
    decl = "idl_st =" if vargs else "status_t idl_st ="
    out.append(f"        {decl} ops->{m.name}({', '.join(call)});")
    hres = [f for f in m.results if f.handle]
    if m.later:
        out.append(f"        /* IDL_LATER: the handler answers with {P}_reply_{m.name}. */")
        out.append("        if (idl_st == IDL_LATER)" + (" {" if hres else ""))
        for f in hres:
            out.append(f"            if (out_{f.name} != HANDLE_INVALID)")
            out.append(f"                drv_handle_close(out_{f.name});")
        out.append("            return 0;")
        if hres:
            out.append("        }")
    out.append("        idl_h->status = idl_st > 0 ? ERR_INTERNAL : idl_st;")
    for f in m.results:   # a handler's varying result past N, or not a str
        if f.var:
            bad = f"out_{f.name}_len > {f.max}"
            if f.str:
                bad += f" || !idl_str_ok(out_{f.name}, out_{f.name}_len)"
            out.append(f"        if (idl_h->status == OK && ({bad}))")
            out.append("            idl_h->status = ERR_INTERNAL;")
    if hres:
        ok = " && ".join(f"out_{f.name} != HANDLE_INVALID" for f in hres)
        out.append(f"        if (idl_h->status == OK && !({ok}))")
        out.append("            idl_h->status = ERR_INTERNAL;   "
                   "/* a handle result left unset */")
        out.append("        if (idl_h->status != OK) {")
        for f in hres:
            out.append(f"            if (out_{f.name} != HANDLE_INVALID)")
            out.append(f"                drv_handle_close(out_{f.name});")
        out.append("            return sizeof(*idl_h);")
        out.append("        }")
        for k, f in enumerate(hres):
            out.append(f"        rhs[{k}] = out_{f.name};")
        out.append(f"        *rhn = {len(hres)};")
    else:
        out.append("        if (idl_h->status != OK)")
        out.append("            return sizeof(*idl_h);")
    for f in m.results:
        if f.handle or f.var or (f.array and g):
            continue
        if f.array:
            out.append(f"        for (uint32_t idl_i = 0; idl_i < {f.array}; idl_i++)")
            out.append(f"            idl_r->{f.name}[idl_i] = out_{f.name}[idl_i];")
        else:
            out.append(f"        idl_r->{f.name} = out_{f.name};")
    if has_var(m.results):
        out.append("        uint32_t idl_rn = sizeof(*idl_r);")
        for f in m.results:
            if f.var:
                out.append(f"        idl_place(rep, &idl_rn, {room[f]}, out_{f.name}_len, {f.size});")
        out.append("        return idl_rn;")
    else:
        out.append("        return sizeof(*idl_r);")
    out.append("    }")
    return out


def intro(P):
    """What a header's top comment says of protocol P's functions."""
    return [f" * Client: {P}_<method>(ch, args..., &results...)",
            f" * (and {P}_<method>_until with a deadline, _within with a timeout), or",
            f" * {P}_<method>_send and {P}_<method>_result without waiting. Server:",
            f" * fill a struct {P}_ops and run {P}_serve(ch, &ops, ctx), or",
            f" * {P}_serve_one / {P}_dispatch_on for a loop of your own, or",
            f" * {P}_take_slot / {P}_run_slot for requests read into slots that outlive",
            f" * the server; {P}_reply_<method> answers a request later."]


def gen_file(protos):
    """The header of one .idl file: its protocols, in its order."""
    first = protos[0]
    out = [f"/* {BANNER.format(src=os.path.relpath(first.src))}", " *"]
    out += ([f" * {l}" if l else " *" for l in first.doc] + [" *"]) if first.doc else []
    if len(protos) == 1:
        lines = intro(first.name)
        lines[0] = f" * Protocol `{first.name}` (id {first.pid}). " + lines[0][3:]
    else:
        names = ", ".join(f"`{p.name}` (id {p.pid})" for p in protos[:-1])
        last = protos[-1]
        lines = [f" * Protocols {names} and `{last.name}` (id {last.pid}): one for each",
                 " * kind of channel, so a message of one protocol sent on a channel of",
                 " * another is refused (ERR_NOT_SUPPORTED). For each <proto>:"]
        lines += intro("<proto>")
    lines[-1] += " */"
    out += lines + ["#pragma once", "", "#include <idl/common.h>", ""]
    for i, p in enumerate(protos):
        if i:
            rule = f"/* ==== protocol `{p.name}` (id {p.pid}) "
            out += ["", rule + "=" * max(4, 79 - len(rule)), " *"]
            out += [f" * {l}" if l else " *" for l in p.doc]
            out[-1] += " */"
            out.append("")
        out += gen_protocol(p)
    return "\n".join(out)


def takes_handles(p):
    """Does an event to p's server carry handles (so its loops read them)?"""
    return any(f.handle for m in p.methods if m.event == "in" for f in m.args)


def gen_event_send(p, e):
    """<proto>_<event>(ch, fields...): the event written on ch, by whichever
    end it goes from."""
    P, U = p.name, p.name.upper()
    fields = e.fields
    hs = [f for f in fields if f.handle]
    params = ["handle_t ch"] + in_params(fields)
    to = "the server" if e.event == "in" else "the client"
    out = [""]
    out += c_comment(e.doc)
    out.append(f"/* The event {P}.{e.name}, to {to}: written on ch, never answered.")
    out.append(" * ERR_SHOULD_WAIT: the peer's queue is full (its flow control).")
    if hs:
        out.append(" * The handles are gone in every case: sent, closed, or lost with a")
        out.append(" * write that failed (the caller never closes them after this).")
    out[-1] += " */"
    out.append(f"static inline status_t {P}_{e.name}({', '.join(params)})")
    out.append("{")
    if hs:
        out.append(f"    handle_t idl_hs[{len(hs)}] = {{ {', '.join(f.name for f in hs)} }};")
    close = [f"        idl_close_all(idl_hs, {len(hs)});"] if hs else []
    size = method_max(fields)
    free = None
    if size <= STACK_MAX:
        out.append(f"    _Alignas(8) uint8_t idl_b0[{size}];")
    else:
        out.append(f"    uint8_t *idl_b0 = (uint8_t *)drv_malloc({size});")
        out.append("    if (!idl_b0) {")
        out += close
        out.append("        return ERR_NO_MEMORY;")
        out.append("    }")
        free = "    drv_free(idl_b0);"
    out.append(f"    struct {P}_{e.name}_msg *idl_q = (struct {P}_{e.name}_msg *)idl_b0;")
    out.append("    uint32_t idl_qn = sizeof(*idl_q);")
    out.append("    status_t idl_st = OK;")
    out.append("    idl_q->txid = 0;   /* an event */")
    out.append(f"    idl_q->ordinal = {U}_{e.name.upper()};")
    for f in fields:
        if f.handle or f.var:
            continue
        if f.array:
            out.append(f"    for (uint32_t idl_i = 0; idl_i < {f.array}; idl_i++)")
            out.append(f"        idl_q->{f.name}[idl_i] = {f.name}[idl_i];")
        else:
            out.append(f"    idl_q->{f.name} = {f.name};")
    out += put_vars(fields, "idl_b0", "idl_qn", "ERR_INVALID_ARGS")
    if hs:
        # Closed only when the kernel never got them (a field refused): a
        # send's handles are gone whatever its status, and closing them
        # after it could close another thread's new handles.
        out.append("    if (idl_st != OK)")
        out.append(f"        idl_close_all(idl_hs, {len(hs)});   /* never sent */")
        out.append("    else")
        out.append(f"        idl_st = drv_channel_write(ch, idl_b0, idl_qn, idl_hs, {len(hs)});")
    else:
        out.append("    if (idl_st == OK)")
        out.append("        idl_st = drv_channel_write(ch, idl_b0, idl_qn, NULL, 0);")
    if free:
        out.append(free)
    out.append("    return idl_st;")
    out.append("}")
    return out


def event_call(e, fields_src, hs):
    """The handler call's arguments for event e, its varying fields read by
    get_vars and its handles in hs[]."""
    call, k, h = ["ctx"], 0, 0
    for f in e.fields:
        if f.var:
            cast = "const char" if f.str else f"const {f.ctype}"
            call += [f"({cast} *)idl_p{k}", f"idl_l{k}"]
            k += 1
        elif f.handle:
            call.append(f"{hs}[{h}]")
            h += 1
        else:
            call.append(f"{fields_src}->{f.name}")
    return call


def gen_event_case(p, e, hs, nh, ind="    "):
    """The switch case that decodes event e (n bytes at req, nh handles in
    hs) and runs its handler in table `tab`. Returns lines; on any refusal
    the handles are closed."""
    P, U = p.name, p.name.upper()
    k = sum(1 for f in e.fields if f.handle)
    out = [f"{ind}case {U}_{e.name.upper()}: {{"]
    out.append(f"{ind}    const struct {P}_{e.name}_msg *idl_e = (const struct {P}_{e.name}_msg *)idl_m;")
    out.append(f"{ind}    uint32_t idl_o = sizeof(*idl_e);")
    out.append(f"{ind}    status_t idl_st = idl_n < sizeof(*idl_e) ? ERR_INVALID_ARGS : OK;")
    out += get_vars(e.fields, "idl_m", "idl_n", "ERR_INVALID_ARGS", ind + "    ")
    if not has_var(e.fields):
        out.append(f"{ind}    if (idl_o != idl_n)")
        out.append(f"{ind}        idl_st = ERR_INVALID_ARGS;")
    out.append(f"{ind}    if (idl_st == OK && {nh} != {k})")
    out.append(f"{ind}        idl_st = ERR_INVALID_ARGS;")
    out.append(f"{ind}    if (idl_st == OK && !idl_tab->{e.name})")
    out.append(f"{ind}        idl_st = ERR_NOT_SUPPORTED;")
    out.append(f"{ind}    if (idl_st != OK) {{")
    out.append(f"{ind}        idl_close_all({hs}, {nh});")
    out.append(f"{ind}        return idl_st;")
    out.append(f"{ind}    }}")
    out.append(f"{ind}    idl_tab->{e.name}({', '.join(event_call(e, 'idl_e', hs))});")
    out.append(f"{ind}    return OK;")
    out.append(f"{ind}}}")
    return out


def gen_events_in(p):
    """<proto>_event_in: an event to the server, handled (a helper of the
    dispatch)."""
    P = p.name
    ins = [m for m in p.methods if m.event == "in"]
    out = [f"/* Run the event (txid 0) of n bytes at m with nh handles in hs: OK, or",
           " * ERR_NOT_SUPPORTED (no such event, or no handler) or ERR_INVALID_ARGS",
           " * (it breaks the event's format), its handles closed. */",
           f"static inline status_t {P}_event_in(const struct {P}_ops *idl_tab, void *ctx,",
           " " * len(f"static inline status_t {P}_event_in(") +
           "const void *idl_m, uint32_t idl_n, const handle_t *idl_hs, uint32_t idl_nh)",
           "{",
           "    switch (((const struct idl_req_hdr *)idl_m)->ordinal) {"]
    for e in ins:
        out += gen_event_case(p, e, "idl_hs", "idl_nh")
    out += ["    }",
            "    idl_close_all(idl_hs, idl_nh);",
            "    return ERR_NOT_SUPPORTED;",
            "}",
            ""]
    return out


def gen_events_out(p):
    """The client's side of the events from the server: struct <proto>_events,
    <proto>_event_dispatch and <proto>_read_event."""
    P, U = p.name, p.name.upper()
    outs = [m for m in p.methods if m.event == "out"]
    out = ["",
           f"/* The client's handlers of the events from the server: a NULL one drops",
           " * its event (handles closed). A handler owns the handles it is given;",
           " * its arrays point into the message (copy what it keeps). */",
           f"struct {P}_events {{"]
    for e in outs:
        params = ["void *ctx"] + in_params(e.fields)
        out.append(f"    void (*{e.name})({', '.join(params)});")
    out += ["};",
            "",
            "/* Client: run the event read with idl_reply_read into msg and *m (an",
            " * event has txid 0, a reply any other): OK, its handler ran with its",
            " * handles; otherwise it is dropped, its handles closed:",
            " * ERR_NOT_SUPPORTED (no such event, or no handler), ERR_INVALID_ARGS",
            " * (it breaks the format, or isn't an event). */",
            f"static inline status_t {P}_event_dispatch(const struct {P}_events *idl_tab, void *ctx,",
            " " * len(f"static inline status_t {P}_event_dispatch(") +
            "const void *idl_m, struct idl_msg *m)",
            "{",
            "    uint32_t idl_n = m->n, idl_nh = m->nh;",
            "    const handle_t *idl_hs = m->hs;",
            "    m->nh = 0;   /* each path below takes them */",
            "    if (idl_n < sizeof(struct idl_req_hdr) || ((const struct idl_req_hdr *)idl_m)->txid) {",
            "        idl_close_all(idl_hs, idl_nh);",
            "        return ERR_INVALID_ARGS;",
            "    }",
            "    switch (((const struct idl_req_hdr *)idl_m)->ordinal) {"]
    for e in outs:
        out += gen_event_case(p, e, "idl_hs", "idl_nh")
    out += ["    }",
            "    idl_close_all(idl_hs, idl_nh);",
            "    return ERR_NOT_SUPPORTED;",
            "}",
            ""]
    size = p.rep_max
    out += [f"/* Client: read the next message on ch and run it as an event of the",
            f" * server's ({P}_event_dispatch's results). ERR_SHOULD_WAIT: nothing is",
            " * queued; ERR_PEER_CLOSED: the server is gone; ERR_INTERNAL: a message",
            " * that is no event (a reply to a call that gave up), dropped. For a",
            " * channel whose replies come with idl_reply_read, call",
            f" * {P}_event_dispatch for the messages with txid 0 instead. */",
            f"static inline status_t {P}_read_event(handle_t ch, const struct {P}_events *idl_tab, "
            "void *ctx)",
            "{"]
    if size <= STACK_MAX:
        out.append(f"    _Alignas(8) uint8_t idl_b[{U}_REP_MAX];")
    else:
        out += [f"    uint8_t *idl_b = (uint8_t *)drv_malloc({U}_REP_MAX);",
                "    if (!idl_b)",
                "        return ERR_NO_MEMORY;"]
    out += ["    struct idl_msg idl_m;",
            f"    status_t idl_st = idl_reply_read(ch, idl_b, {U}_REP_MAX, &idl_m);",
            "    if (idl_st == OK && idl_m.txid) {",
            "        idl_msg_drop(&idl_m);",
            "        idl_st = ERR_INTERNAL;",
            "    } else if (idl_st == OK) {",
            f"        idl_st = {P}_event_dispatch(idl_tab, ctx, idl_b, &idl_m);",
            "    }"]
    if size > STACK_MAX:
        out.append("    drv_free(idl_b);")
    out += ["    return idl_st;", "}"]
    return out


def gen_protocol(p):
    """Protocol p's part of its header: from its id to its server loop."""
    P, U = p.name, p.name.upper()
    calls = [m for m in p.methods if not m.event]
    ins = [m for m in p.methods if m.event == "in"]
    outs = [m for m in p.methods if m.event == "out"]
    hin = takes_handles(p)
    out = [f"#define {U}_PROTOCOL_ID {p.pid}u"]
    if p.kept:
        out.append(f"#define {U}_KEPT 1u   /* its restart is not seen; nothing to its server "
                   "carries handles */")
    for m in p.methods:
        out.append(f"#define {U}_{m.name.upper():<16} 0x{p.wire(m):08x}u")
    out.append("")
    if p.consts:
        out.append("/* Constants (the .idl's `const` lines). */")
        for name, c, doc in p.consts:
            out += c_comment(doc)
            out.append(f"#define {U}_{name} {c}")
        out.append("")

    req_max = p.req_max = max([method_max(m.args) for m in calls + ins] + [HDR])
    rep_max = p.rep_max = max([method_max(m.results) for m in calls + outs] + [HDR])
    out.append("/* Messages (packed: no padding bytes ever cross the channel). */")
    for m in p.methods:
        if m.event:
            out.append(f"struct {P}_{m.name}_msg {{   /* an event: txid 0 */")
            out.append("    uint32_t txid;")
            out.append("    uint32_t ordinal;")
            out += [f"    {f.member()}" for f in m.fields if f.wire]
            out.append("} __attribute__((packed));")
            continue
        out.append(f"struct {P}_{m.name}_req {{")
        out.append("    uint32_t txid;")
        out.append("    uint32_t ordinal;")
        out += [f"    {f.member()}" for f in m.args if f.wire]
        out.append("} __attribute__((packed));")
        out.append(f"struct {P}_{m.name}_rep {{")
        out.append("    uint32_t txid;")
        out.append("    int32_t  status;")
        out += [f"    {f.member()}" for f in m.results if f.wire]
        out.append("} __attribute__((packed));")
    if ins or outs:
        out += ["", f"#define {U}_REQ_MAX {req_max}u   /* bytes: the biggest message to the server */",
                f"#define {U}_REP_MAX {rep_max}u   /* bytes: the biggest message to the client */", ""]
    else:
        out += ["", f"#define {U}_REQ_MAX {req_max}u   /* bytes: the biggest request */",
                f"#define {U}_REP_MAX {rep_max}u   /* bytes: the biggest reply */", ""]

    out.append("/* ---- client ---------------------------------------------------------- */")
    for m in calls:
        out += gen_client_sync(p, m)
    out.append("")
    out.append("/* ---- client, asynchronous (tools/genidl.py) --------------------------- */")
    for m in calls:
        out += gen_client_async(p, m)
    out.append("")
    if ins or outs:
        out.append("/* ---- events (one way, txid 0, never answered) -------------------------- */")
        for e in ins + outs:
            out += gen_event_send(p, e)
        if outs:
            out += gen_events_out(p)
        out.append("")

    out.append("/* ---- server ---------------------------------------------------------- */")
    out.append("")
    out.append(f"/* Handlers: return OK and fill the results, or an ERR_* for the client.")
    out.append(" * A NULL handler answers ERR_NOT_SUPPORTED. A `later` method's handler")
    out.append(" * also gets the request's txn, and may return IDL_LATER and answer it")
    if ins:
        out.append(f" * with {P}_reply_<method> (now, or later from anywhere). An event's")
        out.append(" * handler returns nothing (nothing is answered; a NULL one drops it),")
        out.append(" * owns the handles it is given, and its arrays point into the message. */")
    else:
        out.append(f" * with {P}_reply_<method> (now, or later from anywhere). */")
    out.append(f"struct {P}_ops {{")
    for m in p.methods:
        if m.event == "out":
            continue
        if m.event == "in":
            out.append(f"    void (*{m.name})({', '.join(['void *ctx'] + in_params(m.args))});")
            continue
        params = ["void *ctx"] + (["struct idl_txn idl_txn"] if m.later else []) + \
                 in_params(m.args) + handler_out_params(m.results)
        out.append(f"    status_t (*{m.name})({', '.join(params)});")
    out.append("};")
    for m in calls:
        out += gen_server_reply(p, m)
    out.append("")
    if ins:
        out += gen_events_in(p)
    later = [m.name for m in calls if m.later]
    name = f"{P}_dispatch_h" if hin else f"{P}_dispatch_on"
    pad = " " * len(f"static inline uint32_t {name}(")
    head = [
        f"/* Decode the request of n bytes at req, which came on ch, call its handler,",
        f" * encode the reply into rep ({U}_REP_MAX bytes) and the handles it carries",
        " * into rhs (IDL_REP_HANDLES slots; *rhn of them). Returns the reply's",
        " * length: 0 means no reply (the request has no txid, or a `later`",
        " * handler answers it itself). No I/O; the caller sends the reply with the",
        " * handles, or closes them if it never sends it. */",
    ]
    if ins:
        head[3:] = [" * length: 0 means no reply (the message has no txid, a `later` handler",
                    " * answers it itself, or it is an event: txid 0, never answered; one no",
                    " * handler takes is dropped). No I/O; the caller sends the reply with the",
                    " * handles, or closes them if it never sends it." + ("" if hin else " */")]
        if hin:
            head += [" * The idl_nh handles at idl_hs (an event's) are taken in every case:",
                     " * given to its handler, or closed. */"]
    elif outs:
        head[3:] = [" * length: 0 means no reply (the message has no txid, a `later` handler",
                    " * answers it itself, or it has txid 0: an event of the other way, or",
                    " * none, never answered). No I/O; the caller sends the reply with the",
                    " * handles, or closes them if it never sends it. */"]
    out += head
    out.append(f"static inline uint32_t {name}(handle_t ch, const struct {P}_ops *ops, void *ctx,")
    if hin:
        out.append(pad + "const void *req, uint32_t n, const handle_t *idl_hs, uint32_t idl_nh,")
        out.append(pad + "void *rep, handle_t *rhs, uint32_t *rhn)")
    else:
        out.append(pad + "const void *req, uint32_t n, void *rep, handle_t *rhs,")
        out.append(pad + "uint32_t *rhn)")
    out += ["{",
            "    struct idl_rep_hdr *idl_h = (struct idl_rep_hdr *)rep;",
            "    *rhn = 0;",
            "    (void)rhs;"]
    if not later:
        out.append("    (void)ch;")
    if not calls and not ins:   # only events to the client: nothing to run
        out += ["    (void)ops;", "    (void)ctx;"]
    if ins or outs:
        close = ["        idl_close_all(idl_hs, idl_nh);"] if hin else []
        out += ["    if (n < sizeof(struct idl_req_hdr) || !((const struct idl_req_hdr *)req)->txid) {",
                "        /* txid 0: an event (or no txid): never answered */"]
        if ins:
            out += ["        if (n >= sizeof(struct idl_req_hdr))",
                    f"            (void){P}_event_in(ops, ctx, req, n, " +
                    ("idl_hs, idl_nh);" if hin else "NULL, 0);"),
                    "        else" if hin else None]
            if hin:
                out.append("    " + close[0])
        out += ["        return 0;", "    }"]
        out = [l for l in out if l is not None]
        if hin:
            out += ["    if (idl_nh) {   /* a request with handles: only an event takes any */",
                    "        idl_close_all(idl_hs, idl_nh);",
                    "        idl_h->txid = ((const struct idl_req_hdr *)req)->txid;",
                    "        idl_h->status = ERR_INVALID_ARGS;",
                    "        return sizeof(*idl_h);",
                    "    }"]
        out += ["    idl_h->txid = ((const struct idl_req_hdr *)req)->txid;",
                "    idl_h->status = ERR_INVALID_ARGS;",
                "    switch (((const struct idl_req_hdr *)req)->ordinal) {"]
        for e in ins:   # an event with a txid: refused, the sender may be waiting
            out.append(f"    case {U}_{e.name.upper()}:")
        if ins:
            out.append("        return sizeof(*idl_h);   /* an event with a txid */")
    else:
        out += ["    if (n < sizeof(uint32_t))",
                "        return 0;",
                "    idl_h->txid = ((const struct idl_req_hdr *)req)->txid;",
                "    idl_h->status = ERR_INVALID_ARGS;",
                "    if (n < sizeof(struct idl_req_hdr))",
                "        return sizeof(*idl_h);",
                "    switch (((const struct idl_req_hdr *)req)->ordinal) {"]
    for m in calls:
        out += gen_dispatch_case(p, m)
    out += [
        "    }",
        "    idl_h->status = ERR_NOT_SUPPORTED;",
        "    return sizeof(*idl_h);",
        "}",
        "",
    ]
    if hin:
        pad2 = " " * len(f"static inline uint32_t {P}_dispatch_on(")
        out += [
            f"/* {P}_dispatch_h without handles (a loop of your own that reads none:",
            " * an event that carries some is then dropped). */",
            f"static inline uint32_t {P}_dispatch_on(handle_t ch, const struct {P}_ops *ops, void *ctx,",
            pad2 + "const void *req, uint32_t n, void *rep, handle_t *rhs,",
            pad2 + "uint32_t *rhn)",
            "{",
            f"    return {P}_dispatch_h(ch, ops, ctx, req, n, NULL, 0, rep, rhs, rhn);",
            "}",
            "",
        ]
    out += [
        f"/* {P}_dispatch_on without the channel" +
        (f" (a `later` handler's txn then has none: use {P}_dispatch_on). */" if later else
         " (the protocol has no `later` method). */"),
        f"static inline uint32_t {P}_dispatch(const struct {P}_ops *ops, void *ctx, "
        "const void *req, uint32_t n,",
        " " * len(f"static inline uint32_t {P}_dispatch(") +
        "void *rep, handle_t *rhs, uint32_t *rhn)",
        "{",
        f"    return {P}_dispatch_on(HANDLE_INVALID, ops, ctx, req, n, rep, rhs, rhn);",
        "}",
        "",
        f"/* Take the next message on ch into slot (idl_take), a slot with room for",
        f" * the protocol's biggest request (ERR_INVALID_ARGS if it has less). The",
        f" * reply waiting in *rep (NULL or empty: none) goes out first in the same",
        f" * system call (idl_take_after). It never waits: ERR_SHOULD_WAIT when",
        f" * nothing is queued. */",
        f"static inline status_t {P}_take_slot(handle_t ch, const struct idl_slot *slot,",
        " " * len(f"static inline status_t {P}_take_slot(") + "struct idl_reply *rep)",
        "{",
        f"    if (slot->qcap < {U}_REQ_MAX)",
        "        return ERR_INVALID_ARGS;",
        "    if (rep && rep->ch != HANDLE_INVALID)",
        "        return idl_take_after(ch, slot, rep);",
        "    return idl_take(ch, slot);",
        "}",
        "",
    ]
    if hin:
        out += [
            f"/* Run the message in slot (taken by {P}_take_slot, or found there by a",
            f" * successor), which came on ch: as {P}_dispatch_h, its reply built in",
            " * slot->r and the handles that reply carries put into rhs",
            " * (IDL_REP_HANDLES slots; *rhn of them). The handles it carried",
            " * (*slot->nh in slot->hs) are taken: given to an event's handler or",
            " * closed, *slot->nh 0 after. Returns the reply's length: 0 means no reply",
            " * (no txid, an event, or a `later` handler answers it itself). No I/O;",
            " * the caller sends the reply with the handles, or closes them if it",
            " * never sends it. */",
            f"static inline uint32_t {P}_run_slot(handle_t ch, const struct idl_slot *slot,",
            " " * len(f"static inline uint32_t {P}_run_slot(") +
            f"const struct {P}_ops *ops, void *ctx, handle_t *rhs, uint32_t *rhn)",
            "{",
            "    uint32_t idl_nh = *slot->nh;",
            "    *slot->nh = 0;",
            f"    return {P}_dispatch_h(ch, ops, ctx, slot->q, *slot->n, slot->hs, idl_nh, slot->r,",
            "                          rhs, rhn);",
            "}",
            "",
        ]
    else:
        out += [
            f"/* Run the request in slot (taken by {P}_take_slot, or found there by a",
            f" * successor), which came on ch: as {P}_dispatch_on, its reply built",
            " * in slot->r and the handles that reply carries put into rhs",
            " * (IDL_REP_HANDLES slots; *rhn of them). A request that carried handles is",
            " * answered ERR_INVALID_ARGS without its handler seeing it. Returns the",
            " * reply's length: 0 means no reply (no txid, or a `later` handler",
            " * answers it itself). No I/O; the caller sends the reply with the handles,",
            " * or closes them if it never sends it. */",
            f"static inline uint32_t {P}_run_slot(handle_t ch, const struct idl_slot *slot,",
            " " * len(f"static inline uint32_t {P}_run_slot(") +
            f"const struct {P}_ops *ops, void *ctx, handle_t *rhs, uint32_t *rhn)",
            "{",
            "    *rhn = 0;",
            "    if (*slot->n >= sizeof(uint32_t) && *slot->nh)",
            "        return idl_refuse(slot);",
            f"    return {P}_dispatch_on(ch, ops, ctx, slot->q, *slot->n, slot->r, rhs, rhn);",
            "}",
            "",
        ]
    idem = [m for m in p.methods if m.idempotent]
    out += [
        "/* Is the method with this wire ordinal idempotent (its keyword in the",
        " * .idl): run again on the state its first run left, it gives the same",
        " * result and the same answer? A server that keeps nothing across a",
        " * restart may run such a request again; any other it answers with an",
        " * error. (No server reads it yet: it is for the drivers that outlive",
        " * their process, after M12.) */",
        f"static inline bool {P}_idempotent(uint32_t ordinal)",
        "{",
    ]
    if idem:
        out.append("    switch (ordinal) {")
        out += [f"    case {U}_{m.name.upper()}:" for m in idem]
        out += ["        return true;", "    default:", "        return false;", "    }"]
    else:
        out += ["    (void)ordinal;", "    return false;   /* no method is marked */"]
    out += [
        "}",
        "",
    ]
    return out + gen_loops(p)


def gen_loops(p):
    """<proto>_serve_one and <proto>_serve. Their buffers (the protocol's
    biggest request and reply) are on the stack, or from the heap when
    together they pass STACK_MAX."""
    P, U = p.name, p.name.upper()
    dispatch = f"{P}_dispatch_on(ch, ops, ctx, idl_q, idl_n, idl_r, idl_rhs, &idl_rhn);"
    one = [
        "/* Take one message off ch and answer it. OK once a message was handled",
        " * (its reply may still have been dropped: the client is gone, or never",
        " * called); otherwise drv_channel_read's status: ERR_SHOULD_WAIT when",
        " * nothing is queued, ERR_PEER_CLOSED when the client is gone for good.",
        f"static inline status_t {P}_serve_one(handle_t ch, const struct {P}_ops *ops, void *ctx)",
        "{",
    ]
    loop = [
        "/* Serve ch until the client closes it (OK), or a wait or read fails",
        " * (that status: ERR_CANCELED when the driver is being killed). Each reply",
        " * goes out in the system call that takes the next request",
        " * (idl_serve_next).",
        f"static inline status_t {P}_serve(handle_t ch, const struct {P}_ops *ops, void *ctx)",
        "{",
    ]
    if takes_handles(p):
        return gen_loops_h(p, one, loop)
    if p.req_max + p.rep_max <= STACK_MAX:
        one[3] += " */"
        loop[3] += " */"
        bufs = [f"    _Alignas(8) uint8_t idl_q[{U}_REQ_MAX];",
                f"    _Alignas(8) uint8_t idl_r[{U}_REP_MAX];"]
        return one + bufs + [
            "    handle_t idl_hs[IDL_READ_HANDLES];",
            "    uint32_t idl_n = 0, idl_nh = 0;",
            "    status_t idl_st = drv_channel_read(ch, idl_q, sizeof(idl_q), &idl_n, idl_hs, "
            "IDL_READ_HANDLES,",
            "                                       &idl_nh);",
            "    if (idl_st == ERR_BUFFER_TOO_SMALL)",
            "        return idl_drain(ch, idl_n, idl_nh);",
            "    if (idl_st != OK)",
            "        return idl_st;",
            "    if (idl_nh) {",
            "        idl_close_all(idl_hs, idl_nh);",
            "        idl_reply_status(ch, idl_q, idl_n, ERR_INVALID_ARGS);",
            "        return OK;",
            "    }",
            "    handle_t idl_rhs[IDL_REP_HANDLES];",
            "    uint32_t idl_rhn = 0;",
            f"    uint32_t idl_rn = {dispatch}",
            "    if (!idl_rn)",
            "        idl_close_all(idl_rhs, idl_rhn);   /* no reply: never sent */",
            "    else   /* the reply's handles are gone whatever the write's status */",
            "        (void)drv_channel_write(ch, idl_r, idl_rn, idl_rhs, idl_rhn);",
            "    return OK;",
            "}",
            "",
        ] + loop + bufs + [
            "    handle_t idl_rhs[IDL_REP_HANDLES];",
            "    struct idl_serve idl_s = {",
            "        .ch = ch, .q = idl_q, .qcap = sizeof(idl_q), .r = idl_r, .rhs = idl_rhs,",
            "    };",
            "    for (;;) {",
            "        status_t idl_st = idl_serve_next(&idl_s);",
            "        if (idl_st == ERR_PEER_CLOSED)",
            "            return OK;",
            "        if (idl_st != OK)",
            "            return idl_st;",
            f"        idl_s.rn = {P}_dispatch_on(ch, ops, ctx, idl_q, idl_s.n, idl_r, idl_rhs, "
            "&idl_s.rhn);",
            "    }",
            "}",
            "",
        ]
    one[3:4] = [" * nothing is queued, ERR_PEER_CLOSED when the client is gone for good;",
                " * ERR_NO_MEMORY with the message still queued: no room for the buffers",
                f" * (taken from the heap for each message: {U}_REQ_MAX + {U}_REP_MAX pass",
                " * IDL_STACK_MAX). */"]
    loop[3:4] = [" * (idl_serve_next). Its buffers come from the heap, once (ERR_NO_MEMORY",
                 f" * if they can't): {U}_REQ_MAX + {U}_REP_MAX pass IDL_STACK_MAX. */"]
    bufs = [f"    uint8_t *idl_q = (uint8_t *)drv_malloc(IDL_ROUND8({U}_REQ_MAX) + {U}_REP_MAX);",
            "    if (!idl_q)",
            "        return ERR_NO_MEMORY;",
            f"    uint8_t *idl_r = idl_q + IDL_ROUND8({U}_REQ_MAX);"]
    return one + bufs + [
        "    handle_t idl_hs[IDL_READ_HANDLES];",
        "    uint32_t idl_n = 0, idl_nh = 0;",
        f"    status_t idl_st = drv_channel_read(ch, idl_q, {U}_REQ_MAX, &idl_n, idl_hs, "
        "IDL_READ_HANDLES,",
        "                                       &idl_nh);",
        "    if (idl_st == ERR_BUFFER_TOO_SMALL) {",
        "        idl_st = idl_drain(ch, idl_n, idl_nh);",
        "    } else if (idl_st == OK && idl_nh) {",
        "        idl_close_all(idl_hs, idl_nh);",
        "        idl_reply_status(ch, idl_q, idl_n, ERR_INVALID_ARGS);",
        "    } else if (idl_st == OK) {",
        "        handle_t idl_rhs[IDL_REP_HANDLES];",
        "        uint32_t idl_rhn = 0;",
        f"        uint32_t idl_rn = {dispatch}",
        "        if (!idl_rn)",
        "            idl_close_all(idl_rhs, idl_rhn);   /* no reply: never sent */",
        "        else   /* the reply's handles are gone whatever the write's status */",
        "            (void)drv_channel_write(ch, idl_r, idl_rn, idl_rhs, idl_rhn);",
        "    }",
        "    drv_free(idl_q);",
        "    return idl_st;",
        "}",
        "",
    ] + loop + bufs + [
        "    handle_t idl_rhs[IDL_REP_HANDLES];",
        "    struct idl_serve idl_s = {",
        f"        .ch = ch, .q = idl_q, .qcap = {U}_REQ_MAX, .r = idl_r, .rhs = idl_rhs,",
        "    };",
        "    status_t idl_st;",
        "    while ((idl_st = idl_serve_next(&idl_s)) == OK)",
        f"        idl_s.rn = {P}_dispatch_on(ch, ops, ctx, idl_q, idl_s.n, idl_r, idl_rhs, "
        "&idl_s.rhn);",
        "    drv_free(idl_q);",
        "    return idl_st == ERR_PEER_CLOSED ? OK : idl_st;",
        "}",
        "",
    ]


def gen_loops_h(p, one, loop):
    """gen_loops for a protocol whose events to the server carry handles:
    each message is read with up to IDL_READ_HANDLES of them, and
    <proto>_dispatch_h takes them (an event's handler, or closed)."""
    P, U = p.name, p.name.upper()
    heap = p.req_max + p.rep_max > STACK_MAX
    if heap:
        one[3:4] = [" * nothing is queued, ERR_PEER_CLOSED when the client is gone for good;",
                    " * ERR_NO_MEMORY with the message still queued: no room for the buffers",
                    f" * (taken from the heap for each message: {U}_REQ_MAX + {U}_REP_MAX pass",
                    " * IDL_STACK_MAX). */"]
        loop[3:4] = [" * (idl_serve_next). Its buffers come from the heap, once (ERR_NO_MEMORY",
                     f" * if they can't): {U}_REQ_MAX + {U}_REP_MAX pass IDL_STACK_MAX. */"]
        bufs = [f"    uint8_t *idl_q = (uint8_t *)drv_malloc(IDL_ROUND8({U}_REQ_MAX) + {U}_REP_MAX);",
                "    if (!idl_q)",
                "        return ERR_NO_MEMORY;",
                f"    uint8_t *idl_r = idl_q + IDL_ROUND8({U}_REQ_MAX);"]
        free = ["    drv_free(idl_q);"]
    else:
        one[3] += " */"
        loop[3] += " */"
        bufs = [f"    _Alignas(8) uint8_t idl_q[{U}_REQ_MAX];",
                f"    _Alignas(8) uint8_t idl_r[{U}_REP_MAX];"]
        free = []
    return one + bufs + [
        "    handle_t idl_hs[IDL_READ_HANDLES];",
        "    uint32_t idl_n = 0, idl_nh = 0;",
        f"    status_t idl_st = drv_channel_read(ch, idl_q, {U}_REQ_MAX, &idl_n, idl_hs, "
        "IDL_READ_HANDLES,",
        "                                       &idl_nh);",
        "    if (idl_st == ERR_BUFFER_TOO_SMALL) {",
        "        idl_st = idl_drain(ch, idl_n, idl_nh);",
        "    } else if (idl_st == OK) {",
        "        handle_t idl_rhs[IDL_REP_HANDLES];",
        "        uint32_t idl_rhn = 0;",
        f"        uint32_t idl_rn = {P}_dispatch_h(ch, ops, ctx, idl_q, idl_n, idl_hs, idl_nh, idl_r,",
        "                                         idl_rhs, &idl_rhn);",
        "        if (!idl_rn)",
        "            idl_close_all(idl_rhs, idl_rhn);   /* no reply: never sent */",
        "        else   /* the reply's handles are gone whatever the write's status */",
        "            (void)drv_channel_write(ch, idl_r, idl_rn, idl_rhs, idl_rhn);",
        "    }",
    ] + free + [
        "    return idl_st;",
        "}",
        "",
    ] + loop + bufs + [
        "    handle_t idl_rhs[IDL_REP_HANDLES], idl_hs[IDL_READ_HANDLES];",
        "    struct idl_serve idl_s = {",
        f"        .ch = ch, .q = idl_q, .qcap = {U}_REQ_MAX, .r = idl_r, .rhs = idl_rhs,",
        "        .hs = idl_hs, .hcap = IDL_READ_HANDLES,",
        "    };",
        "    status_t idl_st;",
        "    while ((idl_st = idl_serve_next(&idl_s)) == OK)",
        f"        idl_s.rn = {P}_dispatch_h(ch, ops, ctx, idl_q, idl_s.n, idl_hs, idl_s.nh, idl_r,",
        "                                  idl_rhs, &idl_s.rhn);",
    ] + free + [
        "    return idl_st == ERR_PEER_CLOSED ? OK : idl_st;",
        "}",
        "",
    ]


def c_names(p):
    """Every name protocol p's header defines at file scope."""
    P, U = p.name, p.name.upper()
    names = [f"{P}_{x}" for x in ("ops", "dispatch_on", "dispatch", "take_slot", "run_slot",
                                  "idempotent", "serve_one", "serve")]
    names += [f"{U}_{x}" for x in ("PROTOCOL_ID", "REQ_MAX", "REP_MAX")]
    names += [f"{U}_{c[0]}" for c in p.consts]
    if p.kept:
        names.append(f"{U}_KEPT")
    if any(m.event == "in" for m in p.methods):
        names.append(f"{P}_event_in")
    if takes_handles(p):
        names.append(f"{P}_dispatch_h")
    if any(m.event == "out" for m in p.methods):
        names += [f"{P}_events", f"{P}_event_dispatch", f"{P}_read_event"]
    for m in p.methods:
        if m.event:
            names += [f"{P}_{m.name}", f"{P}_{m.name}_msg", f"{U}_{m.name.upper()}"]
            continue
        names += [f"{P}_{m.name}{x}" for x in ("", "_until", "_within", "_call", "_send",
                                               "_result", "_req", "_rep")]
        names += [f"{P}_reply_{m.name}", f"{U}_{m.name.upper()}"]
    return names


def check_c_names(protos):
    """No two protocols define the same C name (protocol `a` with a method
    `b_c` and protocol `a_b` with a method `c` would)."""
    seen = {}
    for p in protos:
        for n in c_names(p):
            if n in seen:
                q = seen[n]
                fail(p.src, p.lineno, f"protocol '{p.name}' defines {n} twice" if q is p else
                     f"protocol '{p.name}' and '{q.name}' ({q.src}) both define {n}")
            seen[n] = p


def outputs_for(idl_dir, out_dir, ids_file):
    """{path: text} of every generated file, from the .idl files in idl_dir."""
    files = [parse(f) for f in sorted(glob.glob(os.path.join(idl_dir, "*.idl")))]
    protos = [p for f in files for p in f]
    names = {}
    for p in protos:
        if p.name in names:
            fail(p.src, p.lineno, f"protocol '{p.name}' is also defined in {names[p.name]}")
        names[p.name] = p.src
    check_c_names(protos)
    ids = {}
    for p in protos:
        if p.pid in ids:
            sys.exit(f"{p.src}: protocol id {p.pid} is also {ids[p.pid]}'s")
        ids[p.pid] = p.name
    entries = parse_ids(ids_file)
    check_ids(ids_file, entries, protos)
    outputs = {os.path.join(out_dir, "common.h"): gen_common(entries)}
    for f in files:
        if f[0].name == "common":
            sys.exit(f"{f[0].src}: 'common' is reserved")
        outputs[os.path.join(out_dir, f[0].name + ".h")] = gen_file(f)
    return outputs


def run(mode, idl_dir=IDL_DIR, out_dir=OUT_DIR, ids_file=IDS_FILE):
    """`gen` or `check`. Exits with the reason on a bad .idl, and in `check`
    on an output that is stale, missing or left over."""
    outputs = outputs_for(idl_dir, out_dir, ids_file)
    stale = []
    for path, text in outputs.items():
        try:
            old = open(path).read()
        except FileNotFoundError:
            old = None
        if old == text:
            continue
        if mode == "gen":
            os.makedirs(out_dir, exist_ok=True)
            open(path, "w").write(text)
            print(f"genidl: wrote {path}")
        else:
            stale.append(path)
    for path in sorted(glob.glob(os.path.join(out_dir, "*.h"))):
        if path not in outputs:
            if mode == "gen":
                os.remove(path)
                print(f"genidl: removed {path} (no .idl for it)")
            else:
                stale.append(path + " (no .idl for it)")
    if stale:
        sys.exit("genidl: generated files are stale or edited by hand "
                 f"({', '.join(stale)}):\nrun `make idl` and commit the result")


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("gen", "check", "selftest"):
        sys.exit(__doc__)
    if sys.argv[1] == "selftest":
        selftest()
    else:
        run(sys.argv[1])


# ---- self-test (`make check`) ------------------------------------------------

RULE = "# Reconnect: a test.\n\n"


def t_ok(body, name="t", pid=40):
    """A one-protocol file that must pass, with its registry line."""
    return {f"{name}.idl": f"{RULE}protocol {name} {pid}\n{body}"}


# What genidl must refuse (or accept): (what, {file: text}, registry or None
# for one made from the files' protocol lines, a piece of the error or None).
SELFTESTS = [
    ("a good file", t_ok("1 a (u32 x) -> (u32 y)\n"), None, None),
    ("not in the registry", t_ok("1 a () -> ()\n"), "0 none -\n", "is not in"),
    ("another id in the registry", t_ok("1 a () -> ()\n"),
     "0 none -\n41 t abi/idl/t.idl\n", "is listed in"),
    ("a registry line for no protocol", t_ok("1 a () -> ()\n"),
     "0 none -\n40 t abi/idl/t.idl\n41 u abi/idl/t.idl\n", "defines no protocol 'u'"),
    ("an id listed twice", t_ok("1 a () -> ()\n"),
     "0 none -\n40 t abi/idl/t.idl\n40 u abi/idl/ids.txt\n", "listed twice"),
    ("a name listed twice", t_ok("1 a () -> ()\n"),
     "0 none -\n40 t abi/idl/t.idl\n41 t abi/idl/ids.txt\n", "listed twice"),
    ("a missing file", t_ok("1 a () -> ()\n"),
     "0 none -\n40 t abi/idl/t.idl\n41 u abi/idl/nothing.h\n", "doesn't exist"),
    ("no id 0", t_ok("1 a () -> ()\n"), "40 t abi/idl/t.idl\n", "id 0"),
    ("a comment ending C's", t_ok("# a */ b\n1 a () -> ()\n"), None, "can't hold '*/'"),
    ("no restart rule", {"t.idl": "# t.\n\nprotocol t 40\n1 a () -> ()\n"}, None,
     "no restart rule"),
    ("two protocols in a file", {"t.idl": f"{RULE}protocol t 40\n1 a () -> ()\n\n"
                                          f"{RULE}protocol t_b 41\n1 a () -> ()\n"}, None, None),
    ("the second without a restart rule", {"t.idl": f"{RULE}protocol t 40\n1 a () -> ()\n"
                                                    "# b.\nprotocol t_b 41\n1 a () -> ()\n"},
     None, "the comment above it states no restart rule"),
    ("a file not named after its first", {"t.idl": f"{RULE}protocol u 40\n1 a () -> ()\n"},
     None, "must be 't'"),
    ("a protocol twice in a file", {"t.idl": f"{RULE}protocol t 40\n1 a () -> ()\n"
                                             f"{RULE}protocol t 41\n1 a () -> ()\n"},
     None, "in this file already"),
    ("a protocol in two files", {"t.idl": f"{RULE}protocol t 40\n1 a () -> ()\n"
                                          f"{RULE}protocol u 41\n1 a () -> ()\n",
                                 "u.idl": f"{RULE}protocol u 42\n1 a () -> ()\n"},
     None, "also defined in"),
    ("a protocol without methods", {"t.idl": f"{RULE}protocol t 40\n"
                                             f"{RULE}protocol t_b 41\n1 a () -> ()\n"},
     None, "has no methods"),
    ("two protocols' C names", {"t.idl": f"{RULE}protocol t 40\n1 b_c () -> ()\n"
                                         f"{RULE}protocol t_b 41\n1 c () -> ()\n"},
     None, "both define t_b_c"),
    ("a method's macro twice", t_ok("1 req_max () -> ()\n"), None, "defines T_REQ_MAX twice"),
    ("a 16 KiB request", t_ok("1 a (u8[4096] a, u8[4096] b, u8[4096] c, u8[4088] d) -> ()\n"),
     None, None),
    ("a request past 16 KiB", t_ok("1 a (u8[4096] a, u8[4096] b, u8[4096] c, u8[4089] d) -> ()\n"),
     None, "request is 16385 bytes (max 16384)"),
    ("a reply past 16 KiB", t_ok("1 a () -> (u8[4096] a, u8[4096] b, u8[4096] c, u8[4096] d)\n"),
     None, "reply is 16392 bytes"),
    ("arrays of varying length", t_ok("1 a (u8[<=16] a, str[<=4096] b) -> (str[<=1] c)\n"),
     None, None),
    ("a str up to 16 KiB", t_ok("1 a (str[<=16374] a) -> ()\n"), None, None),
    ("a str past 16 KiB", t_ok("1 a (str[<=16375] a) -> ()\n"), None, "request is 16385"),
    ("an array of none", t_ok("1 a (u8[<=0] a) -> ()\n"), None, "N not in 1..65535"),
    ("an array past a u16", t_ok("1 a () -> (u8[<=65536] a)\n"), None, "N not in 1..65535"),
    ("a str[N]", t_ok("1 a (str[8] a) -> ()\n"), None, "unknown type 'str[8]'"),
    ("x and x_len", t_ok("1 a (str[<=8] x, u32 x_len) -> ()\n"), None, "is x's length"),
    ("arrays of every scalar", t_ok("1 a (u16[<=2] a, u32[<=2] b, u64[<=2] c, i8[<=2] d, "
                                    "i16[<=2] e, i32[<=2] f) -> (i64[<=2025] g)\n"), None, None),
    ("u64s up to 16 KiB", t_ok("1 a (u8 x, u64[<=2046] a) -> ()\n"), None, None),
    ("u64s past 16 KiB, padding counted", t_ok("1 a (u8 x, u64[<=2047] a) -> ()\n"), None,
     "request is 16392"),
    ("events both ways", t_ok("1 a (u32 x, str[<=8] s, handle h) event\n"
                              "2 b -> (u8[<=4] d, handle h, handle i) event\n3 c () -> ()\n"),
     None, None),
    ("only events", t_ok("1 a () event\n2 b -> () event\n"), None, None),
    ("only events to the client", t_ok("1 b -> (u8 x, u64[<=3] v) event\n"), None, None),
    ("only events to the server", t_ok("1 a (u8 x, handle h) event\n"), None, None),
    ("an event both ways", t_ok("1 a (u32 x) -> (u32 y) event\n"), None, "go one way"),
    ("an event no way", t_ok("1 a event\n"), None, "go one way"),
    ("an event answered later", t_ok("1 a (u32 x) event later\n"), None, "never answered"),
    ("a method without results", t_ok("1 a (u32 x)\n"), None, "want '<ordinal>"),
    ("nine handles", t_ok("1 a (handle a, handle b, handle c, handle d, handle e, handle f, "
                          "handle g, handle h, handle i) event\n"), None, "more than 8"),
    ("an event past 16 KiB", t_ok("1 a -> (u8[4096] a, u8[4096] b, u8[4096] c, u8[4096] d) "
                                  "event\n"), None, "the event is 16392 bytes"),
    ("an event's names", t_ok("1 a () event\n2 a_msg () -> ()\n"), None, "defines t_a_msg"),
    ("consts", t_ok("const K 1\n# b.\nconst B_2 0xffffffffff\nconst C -5\nconst D 1 << 40\n"
                    "const F -0x80000001\n1 a () -> ()\nconst E 0x10\n"), None, None),
    ("a const twice", t_ok("const A 1\nconst A 2\n1 a () -> ()\n"), None, "const A twice"),
    ("a const in lower case", t_ok("const a 1\n1 a () -> ()\n"), None, "want 'const"),
    ("a const with no value", t_ok("const A\n1 a () -> ()\n"), None, "want 'const"),
    ("a const that isn't a number", t_ok("const A B\n1 a () -> ()\n"), None, "want 'const"),
    ("a const past 64 bits", t_ok("const A 0x10000000000000000\n1 a () -> ()\n"), None,
     "doesn't fit"),
    ("a const shifted too far", t_ok("const A 1 << 64\n1 a () -> ()\n"), None, "0..63"),
    ("a const before a protocol", {"t.idl": f"{RULE}const A 1\nprotocol t 40\n1 a () -> ()\n"},
     None, "the first statement"),
    ("a const's macro and a method's", t_ok("const A 1\n1 a () -> ()\n"), None, "defines T_A twice"),
    ("kept", {"t.idl": "# Restart: the server's restart is not\n# seen: kept.\n\n"
                       "protocol t 40 kept\n1 a () -> ()\n2 b (u32 x) event\n"
                       "3 c -> (handle h) event\n"}, None, None),
    ("kept, with a handle to its server", {"t.idl": "# Restart: the restart is not seen.\n\n"
                                                     "protocol t 40 kept\n1 a (handle h) event\n"},
     None, "is kept"),
    ("kept, but its rule says otherwise", {"t.idl": f"{RULE}protocol t 40 kept\n1 a () -> ()\n"},
     None, "doesn't say the restart is not seen"),
    ("not seen, but not kept", {"t.idl": "# Restart: its restart is not seen.\n\n"
                                         "protocol t 40\n1 a () -> ()\n"}, None, "mark it kept"),
    ("another word", {"t.idl": f"{RULE}protocol t 40 kep\n1 a () -> ()\n"}, None,
     "want 'protocol <name> <id> [kept]'"),
]


def registry_for(files):
    lines = ["0 none -"]
    for fname, text in files.items():
        for m in re.finditer(r"^protocol\s+(\w+)\s+(\d+)", text, re.M):
            lines.append(f"{m.group(2)} {m.group(1)} abi/idl/{fname}")
    return "\n".join(lines) + "\n"


def in_tree(files, ids, fn):
    """Run fn() in a fresh tree holding files (and ids) under abi/idl/:
    None if it passed, else the text it exited with."""
    import contextlib
    import io
    import tempfile
    here = os.getcwd()
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, IDL_DIR))
        for fname, text in files.items():
            open(os.path.join(d, IDL_DIR, fname), "w").write(text)
        open(os.path.join(d, IDS_FILE), "w").write(ids if ids is not None
                                                   else registry_for(files))
        os.chdir(d)
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                fn()
            return None
        except SystemExit as e:
            return str(e.code)
        finally:
            os.chdir(here)


def compiled(root):
    """With $CC set (make check sets it): the headers just generated compile
    as a driver's would, warnings as errors. Exits with the errors if not."""
    import subprocess
    cc = os.environ.get("CC")
    if not cc:
        return
    with open("x.c", "w") as f:
        for h in sorted(glob.glob(os.path.join(OUT_DIR, "*.h"))):
            f.write(f"#include <idl/{os.path.basename(h)}>\n")
    r = subprocess.run([cc, "-std=gnu17", "-O2", "-Wall", "-Wextra", "-Werror", "-Wvla",
                        "-ffreestanding", "-fsyntax-only", "-Idrivers/include",
                        "-I" + os.path.join(root, "drivers/include"),
                        "-I" + os.path.join(root, "kernel/include"), "x.c"],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit("the generated code doesn't compile:\n" + r.stderr[-2000:])


def selftest():
    bad = 0
    root = os.getcwd()

    def gen_and_compile():
        run("gen")
        compiled(root)
    for what, files, ids, want in SELFTESTS:
        got = in_tree(files, ids, gen_and_compile)
        if (want is None) != (got is None) or (want is not None and want not in got):
            print(f"genidl selftest: {what}: wanted {want or 'a pass'!r}, got {got or 'a pass'!r}")
            bad += 1

    # A generated file that is stale, edited, missing or left over fails `check`.
    def edited(change):
        def fn():
            run("gen")
            change(os.path.join(OUT_DIR, "t.h"))
            run("check")
        return fn
    for what, change, want in (
            ("an edited output", lambda p: open(p, "a").write("\n"), "stale"),
            ("a missing output", os.remove, "stale"),
            ("a left-over output", lambda p: open(p[:-3] + "x.h", "w").write(""), "no .idl")):
        got = in_tree(t_ok("1 a () -> ()\n"), None, edited(change))
        if got is None or want not in got:
            print(f"genidl selftest: {what}: wanted {want!r}, got {got or 'a pass'!r}")
            bad += 1
    if bad:
        sys.exit(f"genidl selftest: {bad} of {len(SELFTESTS) + 3} failed")
    print(f"genidl selftest: {len(SELFTESTS) + 3} passed")


if __name__ == "__main__":
    main()
