#!/usr/bin/env python3
"""Generate protocol code from abi/idl/*.idl (ARCHITECTURE.md "IPC").

    genidl.py gen     write the generated headers
    genidl.py check   exit 1 (and name them) if any is missing, stale or
                      left over from a protocol that no longer exists

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

    protocol <name> <id>
    <ordinal> <method> (<type> <arg>, ...) -> (<type> <result>, ...) [later] [idempotent]

name, method, arg, result   C identifiers (lower case by convention)
id                          1..65535, unique over all protocols
ordinal                     1..65535, unique in the protocol
type                        u8 u16 u32 u64 i8 i16 i32 i64, or u8[N] (a
                            fixed-size byte array, 1 <= N <= 4096), or
                            `handle` (RESULTS only, at most 8 per method)

Either list may be empty: `()`.

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
answers any other with an error. The client's side doesn't change.

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

Handles travel only server -> client, in replies: the server's
handler fills `handle_t *out_x` and the reply moves the handles to the
client (drv_channel_call_h reserves their slots before the request goes
out, so a full handle table fails the call up front). A reply with the
wrong number of handles is ERR_INTERNAL and whatever arrived is closed; a
failed reply closes the server's handles; a client passing NULL for a
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
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*$")
SCALARS = {"u8": ("uint8_t", 1), "u16": ("uint16_t", 2), "u32": ("uint32_t", 4),
           "u64": ("uint64_t", 8), "i8": ("int8_t", 1), "i16": ("int16_t", 2),
           "i32": ("int32_t", 4), "i64": ("int64_t", 8)}
ARRAY = re.compile(r"u8\[(\d+)\]$")
ARRAY_MAX = 4096
MSG_MAX = 8192          # bytes of one request or reply: both sit on a kernel stack
HDR = 8                 # txid + ordinal / txid + status
RESERVED = {"ch", "deadline_ns", "timeout_ns", "ctx", "ops"}
# Names of the protocol's own functions (<proto>_<name>): no method may take one.
RESERVED_METHODS = ("dispatch", "dispatch_on", "serve", "serve_one", "take_slot", "run_slot",
                    "idempotent", "ops")
KEYWORDS = ("later", "idempotent")   # after a method's results
C_KEYWORDS = set("""auto break case char const continue default do double else enum extern
    float for goto if inline int long register restrict return short signed sizeof static
    struct switch typedef union unsigned void volatile while _Bool _Alignas _Alignof _Atomic
    _Noreturn _Static_assert _Thread_local bool true false NULL""".split())
BANNER = "Generated by tools/genidl.py from {src}. Do not edit:\n" \
         " * change the .idl and run `make idl`."


REP_HANDLES_MAX = 8


class Field:
    def __init__(self, ctype, name, size, array, handle=False):
        self.ctype, self.name, self.size, self.array = ctype, name, size, array
        self.handle = handle

    def member(self):
        return f"{self.ctype} {self.name}[{self.array}];" if self.array else \
               f"{self.ctype} {self.name};"

    def in_param(self):
        return f"const uint8_t {self.name}[{self.array}]" if self.array else \
               f"{self.ctype} {self.name}"

    def out_param(self):
        return f"uint8_t out_{self.name}[{self.array}]" if self.array else \
               f"{self.ctype} *out_{self.name}"


class Method:
    def __init__(self, ordinal, name, args, results, doc, later, idempotent):
        self.ordinal, self.name, self.args, self.results, self.doc = \
            ordinal, name, args, results, doc
        self.later = later    # the server may answer after its handler returned
        self.idempotent = idempotent   # running it again changes nothing more


class Protocol:
    def __init__(self, src, name, pid, methods):
        self.src, self.name, self.pid, self.methods = src, name, pid, methods

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
        a = ARRAY.match(t)
        if t == "handle":
            if not allow_handles:
                fail(src, lineno, f"{what}: handle arguments aren't supported (if a call fails "
                                  "it can't tell whether they were moved); return handles "
                                  "as results, or send them with channel_write")
            fields.append(Field("handle_t", name, 0, 0, handle=True))
        elif a:
            n = int(a.group(1))
            if not 1 <= n <= ARRAY_MAX:
                fail(src, lineno, f"{what}: array size {n} not in 1..{ARRAY_MAX}")
            fields.append(Field("uint8_t", name, n, n))
        elif t in SCALARS:
            ctype, size = SCALARS[t]
            fields.append(Field(ctype, name, size, 0))
        else:
            fail(src, lineno, f"{what}: unknown type '{t}' (want {' '.join(SCALARS)}, u8[N] "
                              "or handle)")
    return fields


def parse(src):
    name, pid, methods, doc = None, None, [], []
    ordinals, names = set(), set()
    for lineno, raw in enumerate(open(src), 1):
        line = raw.strip()
        if line.startswith("#"):
            doc.append(line[1:].strip())
            continue
        line = line.split("#", 1)[0].strip()
        if not line:
            doc = []
            continue
        if line.startswith("protocol"):
            toks = line.split()
            if name is not None:
                fail(src, lineno, "a second 'protocol' line")
            if len(toks) != 3 or not IDENT.match(toks[1]) or toks[1] in C_KEYWORDS or \
                    not toks[2].isdigit():
                fail(src, lineno, "want 'protocol <name> <id>'")
            name, pid = toks[1], int(toks[2])
            if not 1 <= pid <= 65535:
                fail(src, lineno, f"protocol id {pid} not in 1..65535")
            if os.path.basename(src) != name + ".idl":
                fail(src, lineno, f"protocol '{name}' must live in {name}.idl")
            doc = []
            continue
        if name is None:
            fail(src, lineno, "the first statement must be 'protocol <name> <id>'")
        m = re.match(r"(\d+)\s+([A-Za-z_][A-Za-z0-9_]*)\s*(\(.*?\))\s*->\s*(\(.*?\))"
                     r"((?:\s+[a-z]+)*)$", line)
        if not m:
            fail(src, lineno, "want '<ordinal> <method> (<args>) -> (<results>) [later] "
                              "[idempotent]'")
        ordinal, mname = int(m.group(1)), m.group(2)
        words = m.group(5).split()
        for w in words:
            if w not in KEYWORDS:
                fail(src, lineno, f"unknown keyword '{w}' (want {' or '.join(KEYWORDS)})")
            if words.count(w) > 1:
                fail(src, lineno, f"'{w}' twice")
        if mname in C_KEYWORDS:
            fail(src, lineno, f"bad method name '{mname}'")
        if not 1 <= ordinal <= 65535:
            fail(src, lineno, f"ordinal {ordinal} not in 1..65535")
        if ordinal in ordinals:
            fail(src, lineno, f"ordinal {ordinal} used twice")
        if mname in names:
            fail(src, lineno, f"method '{mname}' defined twice")
        if mname in RESERVED_METHODS or mname.startswith("reply_") or \
                any(mname.endswith(x) for x in ("_until", "_within", "_call", "_send",
                                                "_result")):
            fail(src, lineno, f"method name '{mname}' is reserved "
                              f"({', '.join(RESERVED_METHODS)}, reply_*, *_until, *_within, "
                              "*_call, *_send, *_result)")
        ordinals.add(ordinal)
        names.add(mname)
        args = parse_fields(src, lineno, m.group(3), f"{mname} arguments")
        results = parse_fields(src, lineno, m.group(4), f"{mname} results", allow_handles=True)
        if sum(1 for f in results if f.handle) > REP_HANDLES_MAX:
            fail(src, lineno, f"{mname}: more than {REP_HANDLES_MAX} handle results")
        for size, what in ((sum(f.size for f in args), "request"),
                           (sum(f.size for f in results), "reply")):
            if HDR + size > MSG_MAX:
                fail(src, lineno, f"{mname}: {what} is {HDR + size} bytes (max {MSG_MAX})")
        methods.append(Method(ordinal, mname, args, results, doc, "later" in words,
                              "idempotent" in words))
        doc = []
    if name is None:
        sys.exit(f"{src}: no 'protocol' line")
    if not methods:
        sys.exit(f"{src}: no methods")
    return Protocol(src, name, pid, sorted(methods, key=lambda m: m.ordinal))


def c_comment(lines, indent=""):
    if not lines:
        return []
    if len(lines) == 1:
        return [f"{indent}/* {lines[0]} */"]
    out = [f"{indent}/* {lines[0]}"]
    out += [f"{indent} * {l}" if l else f"{indent} *" for l in lines[1:]]
    out[-1] += " */"
    return out


def gen_common():
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
        "/* Handles a server reads with a request (all of them are closed: no",
        " * method takes handle arguments). */",
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
        " * status. No txid (n < 4): no reply. A reply that can't be written",
        " * (the client is gone, or its queue is full) is dropped. */",
        "static inline void idl_reply_status(handle_t ch, const void *req, uint32_t n, "
        "status_t st)",
        "{",
        "    if (n < sizeof(uint32_t))",
        "        return;",
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
        " * block every later one), close its handles, answer ERR_INVALID_ARGS. */",
        "static inline status_t idl_drain(handle_t ch, uint32_t n, uint32_t nh)",
        "{",
        "    uint32_t idl_txid = 0, idl_got = 0;",
        "    status_t idl_st = idl_discard(ch, n, nh, &idl_txid, &idl_got);",
        "    if (idl_st == OK && idl_got >= sizeof(uint32_t)) {",
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
        "};",
        "",
        "/* Server: send the reply waiting in s, if any, and take the next request",
        " * into s->q, in one system call (drv_channel_reply_wait), waiting for one",
        " * if none is queued. OK: s->n bytes of a request without handles are in",
        " * s->q. A request carrying handles, or too big for s->q, is answered",
        " * ERR_INVALID_ARGS (idl_drain) and the next one taken. A reply that can't",
        " * go out (the client is gone, or never called) is dropped and its",
        " * handles closed, as idl_reply_write's. Otherwise the wait's status:",
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
        "        /* No room for handles: a request that carries any stays queued",
        "         * (ERR_BUFFER_TOO_SMALL) for idl_drain. */",
        "        struct channel_reply_wait_args idl_a = {",
        "            .h = idl_reply ? s->ch : HANDLE_INVALID,",
        "            .wait = s->ch,",
        "            .bytes = (uint64_t)(uintptr_t)s->q,",
        "            .bytes_cap = s->qcap,",
        "            .actual_bytes = (uint64_t)(uintptr_t)&idl_o.n,",
        "            .actual_handles = (uint64_t)(uintptr_t)&idl_o.nh,",
        "            .deadline_ns = DEADLINE_NEVER,",
        "        };",
        "        if (idl_reply) {",
        "            idl_a.rbytes = (uint64_t)(uintptr_t)s->r;",
        "            idl_a.rn = s->rn;",
        "            idl_a.rh = (uint64_t)(uintptr_t)s->rhs;",
        "            idl_a.rhn = s->rhn;",
        "            idl_a.reply_status = (uint64_t)(uintptr_t)&idl_o.rs;",
        "        }",
        "        status_t idl_st = drv_channel_reply_wait(&idl_a);",
        "        if (idl_o.rs != OK)",
        "            idl_close_all(s->rhs, s->rhn);   /* not sent: they're still ours */",
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
        "    uint32_t    nh;      /* handles in hs: moved once the reply is tried */",
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
        "    a->rbytes = (uint64_t)(uintptr_t)rep->r;",
        "    a->rn = rep->rn;",
        "    a->rh = (uint64_t)(uintptr_t)rep->hs;",
        "    a->rhn = rep->nh;",
        "    a->reply_status = (uint64_t)(uintptr_t)rs;",
        "    a->mark = (uint64_t)(uintptr_t)rep->mark;",
        "    return true;",
        "}",
        "",
        "/* After a channel_reply_wait that tried rep's reply (its status rs: 1 if",
        " * the kernel never got to it): nothing waits any more, and handles that",
        " * didn't go are closed (as idl_reply_write's: a client that is gone, or",
        " * whose queue is full, loses its reply). true: the reply failed and the",
        " * call ended there, nothing taken: make it again without the reply. */",
        "static inline bool idl_reply_tried(struct idl_reply *rep, status_t rs)",
        "{",
        "    if (rs != OK)",
        "        idl_close_all(rep->hs, rep->nh);",
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
        "            .bytes = (uint64_t)(uintptr_t)slot->q,",
        "            .bytes_cap = slot->qcap,",
        "            .handles = (uint64_t)(uintptr_t)slot->hs,",
        "            .handles_cap = slot->hcap,",
        "            .actual_bytes = (uint64_t)(uintptr_t)slot->n,",
        "            .actual_handles = (uint64_t)(uintptr_t)slot->nh,",
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
        " * into slot->r without its handler seeing it. The reply's length. */",
        "static inline uint32_t idl_refuse(const struct idl_slot *slot)",
        "{",
        "    struct idl_rep_hdr *idl_r = (struct idl_rep_hdr *)slot->r;",
        "    idl_r->txid = ((const struct idl_req_hdr *)slot->q)->txid;",
        "    idl_r->status = ERR_INVALID_ARGS;",
        "    return sizeof(*idl_r);",
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
        " * handles to txn. The handles are moved in every case: sent, or closed",
        " * when the write fails (ERR_PEER_CLOSED: the client is gone; a client",
        " * that gave up waiting still gets the reply, as a message of its own). */",
        "static inline status_t idl_reply_write(struct idl_txn txn, void *rep, uint32_t n,",
        "                                       handle_t *hs, uint32_t nh)",
        "{",
        "    ((struct idl_rep_hdr *)rep)->txid = txn.txid;",
        "    status_t idl_st = drv_channel_write(txn.ch, rep, n, hs, nh);",
        "    if (idl_st != OK)",
        "        idl_close_all(hs, nh);",
        "    return idl_st;",
        "}",
        "",
        "/* ---- asynchronous calls ------------------------------------------------ */",
        "",
        "/* Client: the next txid from the counter *last, never 0. Use one counter",
        " * per channel end, and only asynchronous calls on that end (a blocking",
        " * call's txid comes from the kernel and could match one of these). */",
        "static inline uint32_t idl_txid_next(uint32_t *last)",
        "{",
        "    if (++*last == 0)",
        "        ++*last;",
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


def fill_request(m, U, q):
    """Lines that fill request struct `q` from m's arguments (txid set by the caller)."""
    out = [f"    {q}.ordinal = {U}_{m.name.upper()};"]
    for f in m.args:
        if f.array:
            out.append(f"    for (uint32_t idl_i = 0; idl_i < {f.array}; idl_i++)")
            out.append(f"        {q}.{f.name}[idl_i] = {f.name}[idl_i];")
        else:
            out.append(f"    {q}.{f.name} = {f.name};")
    return out


def copy_results(m, cond, r):
    """Lines that copy reply `r`'s plain results to the caller's out_ pointers
    (when `cond` holds, if given)."""
    out = []
    pre = f"{cond} && " if cond else ""
    for f in m.results:
        if f.handle:
            continue
        if f.array:
            out.append(f"    for (uint32_t idl_i = 0; {pre}out_{f.name} && "
                       f"idl_i < {f.array}; idl_i++)")
            out.append(f"        out_{f.name}[idl_i] = {r}{f.name}[idl_i];")
        else:
            out.append(f"    if ({pre}out_{f.name})")
            out.append(f"        *out_{f.name} = {r}{f.name};")
    return out


def gen_client_sync(p, m):
    P, U = p.name, p.name.upper()
    params = ["handle_t ch", "bool idl_within", "uint64_t idl_t"] + \
             [f.in_param() for f in m.args] + [f.out_param() for f in m.results]
    out = [""]
    out.append(f"/* {P}_{m.name}_until and _within: idl_t is a deadline, or with idl_within a")
    out.append(" * timeout from when the call starts (the kernel's clock). */")
    out.append(f"static inline status_t {P}_{m.name}_call({', '.join(params)})")
    out.append("{")
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
        out.append("    uint32_t idl_rhn = idl_got[1];")
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
    rest = [f.in_param() for f in m.args] + [f.out_param() for f in m.results]
    names = [f.name for f in m.args] + [f"out_{f.name}" for f in m.results]
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
    params = ["handle_t ch", "uint32_t idl_txid"] + [f.in_param() for f in m.args]
    out.append(f"/* {P}_{m.name} without waiting: the request, with the caller's txid (not 0).")
    out.append(f" * The reply comes on ch: idl_reply_read, then {P}_{m.name}_result. */")
    out.append(f"static inline status_t {P}_{m.name}_send({', '.join(params)})")
    out.append("{")
    out.append(f"    struct {P}_{m.name}_req idl_q;")
    out.append("    if (!idl_txid)")
    out.append("        return ERR_INVALID_ARGS;")
    out.append("    idl_q.txid = idl_txid;")
    out += fill_request(m, U, "idl_q")
    out.append("    return drv_channel_write(ch, &idl_q, sizeof(idl_q), NULL, 0);")
    out.append("}")
    hres = [f for f in m.results if f.handle]
    params = ["const void *idl_rep", "struct idl_msg *idl_m"] + [f.out_param() for f in m.results]
    out.append("")
    out.append(f"/* The status and results of a reply to {P}_{m.name}_send (read with")
    out.append(" * idl_reply_read). The reply's handles are taken in every case: moved to")
    out.append(" * the results, or closed (on a failure, or for a NULL result). */")
    out.append(f"static inline status_t {P}_{m.name}_result({', '.join(params)})")
    out.append("{")
    out.append(f"    const struct {P}_{m.name}_rep *idl_r = (const struct {P}_{m.name}_rep *)idl_rep;")
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
    out.append("    return OK;")
    out.append("}")
    return out


def gen_server_reply(p, m):
    P = p.name
    hres = [f for f in m.results if f.handle]
    params = ["struct idl_txn idl_txn", "status_t idl_st"] + [f.in_param() for f in m.results]
    out = [""]
    out.append(f"/* Answer the {P}.{m.name} request kept in txn: idl_st and, if it is OK, the")
    out.append(" * results (handles are moved in every case: sent, or closed). A positive")
    out.append(" * status is ERR_INTERNAL, and so is OK with a handle result left")
    out.append(" * HANDLE_INVALID. Returns the write's status (idl_reply_write). */")
    out.append(f"static inline status_t {P}_reply_{m.name}({', '.join(params)})")
    out.append("{")
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


def gen_dispatch_case(p, m):
    P, U = p.name, p.name.upper()
    out = [f"    case {U}_{m.name.upper()}: {{"]
    out.append(f"        const struct {P}_{m.name}_req *idl_q = "
               f"(const struct {P}_{m.name}_req *)req;")
    out.append(f"        struct {P}_{m.name}_rep *idl_r = (struct {P}_{m.name}_rep *)rep;")
    for f in m.results:
        if f.handle:
            out.append(f"        handle_t out_{f.name} = HANDLE_INVALID;")
        elif f.array:
            out.append(f"        uint8_t out_{f.name}[{f.array}];")
            out.append(f"        for (uint32_t idl_i = 0; idl_i < {f.array}; idl_i++)")
            out.append(f"            out_{f.name}[idl_i] = 0;")
        else:
            out.append(f"        {f.ctype} out_{f.name} = 0;")
    if not m.results:
        out.append("        (void)idl_r;")
    out.append("        if (n != sizeof(*idl_q))")
    out.append("            return sizeof(*idl_h);")
    out.append(f"        if (!ops->{m.name}) {{")
    out.append("            idl_h->status = ERR_NOT_SUPPORTED;")
    out.append("            return sizeof(*idl_h);")
    out.append("        }")
    call = ["ctx"] + (["idl_txn"] if m.later else []) + [f"idl_q->{f.name}" for f in m.args] + \
           [f"out_{f.name}" if f.array else f"&out_{f.name}" for f in m.results]
    if m.later:
        out.append("        struct idl_txn idl_txn = { ch, idl_h->txid };")
    out.append(f"        status_t idl_st = ops->{m.name}({', '.join(call)});")
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
        if f.handle:
            continue
        if f.array:
            out.append(f"        for (uint32_t idl_i = 0; idl_i < {f.array}; idl_i++)")
            out.append(f"            idl_r->{f.name}[idl_i] = out_{f.name}[idl_i];")
        else:
            out.append(f"        idl_r->{f.name} = out_{f.name};")
    out.append("        return sizeof(*idl_r);")
    out.append("    }")
    return out


def gen_protocol(p):
    P, U = p.name, p.name.upper()
    src = os.path.relpath(p.src)
    out = [f"/* {BANNER.format(src=src)}",
           " *",
           f" * Protocol `{P}` (id {p.pid}). Client: {P}_<method>(ch, args..., &results...)",
           f" * (and {P}_<method>_until with a deadline, _within with a timeout), or",
           f" * {P}_<method>_send and {P}_<method>_result without waiting. Server:",
           f" * fill a struct {P}_ops and run {P}_serve(ch, &ops, ctx), or",
           f" * {P}_serve_one / {P}_dispatch_on for a loop of your own, or",
           f" * {P}_take_slot / {P}_run_slot for requests read into slots that outlive",
           f" * the server; {P}_reply_<method> answers a request later. */",
           "#pragma once", "", "#include <idl/common.h>", "",
           f"#define {U}_PROTOCOL_ID {p.pid}u"]
    for m in p.methods:
        out.append(f"#define {U}_{m.name.upper():<16} 0x{p.wire(m):08x}u")
    out.append("")

    req_max = max(HDR + sum(f.size for f in m.args) for m in p.methods)
    rep_max = max(HDR + sum(f.size for f in m.results) for m in p.methods)
    out.append("/* Messages (packed: no padding bytes ever cross the channel). */")
    for m in p.methods:
        out.append(f"struct {P}_{m.name}_req {{")
        out.append("    uint32_t txid;")
        out.append("    uint32_t ordinal;")
        out += [f"    {f.member()}" for f in m.args if not f.handle]
        out.append("} __attribute__((packed));")
        out.append(f"struct {P}_{m.name}_rep {{")
        out.append("    uint32_t txid;")
        out.append("    int32_t  status;")
        out += [f"    {f.member()}" for f in m.results if not f.handle]
        out.append("} __attribute__((packed));")
    out += ["", f"#define {U}_REQ_MAX {req_max}u   /* bytes: the biggest request */",
            f"#define {U}_REP_MAX {rep_max}u   /* bytes: the biggest reply */", ""]

    out.append("/* ---- client ---------------------------------------------------------- */")
    for m in p.methods:
        out += gen_client_sync(p, m)
    out.append("")
    out.append("/* ---- client, asynchronous (tools/genidl.py) --------------------------- */")
    for m in p.methods:
        out += gen_client_async(p, m)
    out.append("")

    out.append("/* ---- server ---------------------------------------------------------- */")
    out.append("")
    out.append(f"/* Handlers: return OK and fill the results, or an ERR_* for the client.")
    out.append(" * A NULL handler answers ERR_NOT_SUPPORTED. A `later` method's handler")
    out.append(" * also gets the request's txn, and may return IDL_LATER and answer it")
    out.append(f" * with {P}_reply_<method> (now, or later from anywhere). */")
    out.append(f"struct {P}_ops {{")
    for m in p.methods:
        params = ["void *ctx"] + (["struct idl_txn idl_txn"] if m.later else []) + \
                 [f.in_param() for f in m.args] + [f.out_param() for f in m.results]
        out.append(f"    status_t (*{m.name})({', '.join(params)});")
    out.append("};")
    for m in p.methods:
        out += gen_server_reply(p, m)
    out.append("")
    later = [m.name for m in p.methods if m.later]
    out += [
        f"/* Decode the request of n bytes at req, which came on ch, call its handler,",
        f" * encode the reply into rep ({U}_REP_MAX bytes) and the handles it carries",
        " * into rhs (IDL_REP_HANDLES slots; *rhn of them). Returns the reply's",
        " * length: 0 means no reply (the request has no txid, or a `later`",
        " * handler answers it itself). No I/O; the caller sends the reply with the",
        " * handles, or closes them if it can't. */",
        f"static inline uint32_t {P}_dispatch_on(handle_t ch, const struct {P}_ops *ops, "
        "void *ctx,",
        " " * len(f"static inline uint32_t {P}_dispatch_on(") +
        "const void *req, uint32_t n, void *rep, handle_t *rhs,",
        " " * len(f"static inline uint32_t {P}_dispatch_on(") + "uint32_t *rhn)",
        "{",
        "    struct idl_rep_hdr *idl_h = (struct idl_rep_hdr *)rep;",
        "    *rhn = 0;",
        "    (void)rhs;",
        "    (void)ch;" if not later else None,
        "    if (n < sizeof(uint32_t))",
        "        return 0;",
        "    idl_h->txid = ((const struct idl_req_hdr *)req)->txid;",
        "    idl_h->status = ERR_INVALID_ARGS;",
        "    if (n < sizeof(struct idl_req_hdr))",
        "        return sizeof(*idl_h);",
        "    switch (((const struct idl_req_hdr *)req)->ordinal) {",
    ]
    out = [l for l in out if l is not None]
    for m in p.methods:
        out += gen_dispatch_case(p, m)
    out += [
        "    }",
        "    idl_h->status = ERR_NOT_SUPPORTED;",
        "    return sizeof(*idl_h);",
        "}",
        "",
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
        f"/* Run the request in slot (taken by {P}_take_slot, or found there by a",
        f" * successor), which came on ch: as {P}_dispatch_on, its reply built",
        " * in slot->r and the handles that reply carries put into rhs",
        " * (IDL_REP_HANDLES slots; *rhn of them). A request that carried handles is",
        " * answered ERR_INVALID_ARGS without its handler seeing it. Returns the",
        " * reply's length: 0 means no reply (no txid, or a `later` handler",
        " * answers it itself). No I/O; the caller sends the reply with the handles,",
        " * or closes them if it can't. */",
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
        " * error. */",
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
        "/* Take one message off ch and answer it. OK once a message was handled",
        " * (its reply may still have been dropped: the client is gone, or never",
        " * called); otherwise drv_channel_read's status: ERR_SHOULD_WAIT when",
        " * nothing is queued, ERR_PEER_CLOSED when the client is gone for good. */",
        f"static inline status_t {P}_serve_one(handle_t ch, const struct {P}_ops *ops, void *ctx)",
        "{",
        f"    _Alignas(8) uint8_t idl_q[{U}_REQ_MAX];",
        f"    _Alignas(8) uint8_t idl_r[{U}_REP_MAX];",
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
        f"    uint32_t idl_rn = {P}_dispatch_on(ch, ops, ctx, idl_q, idl_n, idl_r, idl_rhs, "
        "&idl_rhn);",
        "    if (!idl_rn || drv_channel_write(ch, idl_r, idl_rn, idl_rhs, idl_rhn) != OK)",
        "        idl_close_all(idl_rhs, idl_rhn);   /* not sent: they're still ours */",
        "    return OK;",
        "}",
        "",
        "/* Serve ch until the client closes it (OK), or a wait or read fails",
        " * (that status: ERR_CANCELED when the driver is being killed). Each reply",
        " * goes out in the system call that takes the next request",
        " * (idl_serve_next). */",
        f"static inline status_t {P}_serve(handle_t ch, const struct {P}_ops *ops, void *ctx)",
        "{",
        f"    _Alignas(8) uint8_t idl_q[{U}_REQ_MAX];",
        f"    _Alignas(8) uint8_t idl_r[{U}_REP_MAX];",
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
    return "\n".join(out)


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("gen", "check"):
        sys.exit(__doc__)
    protos = [parse(f) for f in sorted(glob.glob(os.path.join(IDL_DIR, "*.idl")))]
    ids = {}
    for p in protos:
        if p.pid in ids:
            sys.exit(f"{p.src}: protocol id {p.pid} is also {ids[p.pid]}'s")
        ids[p.pid] = p.name
    outputs = {os.path.join(OUT_DIR, "common.h"): gen_common()}
    for p in protos:
        if p.name == "common":
            sys.exit(f"{p.src}: 'common' is reserved")
        outputs[os.path.join(OUT_DIR, p.name + ".h")] = gen_protocol(p)
    stale = []
    for path, text in outputs.items():
        try:
            old = open(path).read()
        except FileNotFoundError:
            old = None
        if old == text:
            continue
        if sys.argv[1] == "gen":
            os.makedirs(OUT_DIR, exist_ok=True)
            open(path, "w").write(text)
            print(f"genidl: wrote {path}")
        else:
            stale.append(path)
    for path in sorted(glob.glob(os.path.join(OUT_DIR, "*.h"))):
        if path not in outputs:
            if sys.argv[1] == "gen":
                os.remove(path)
                print(f"genidl: removed {path} (no .idl for it)")
            else:
                stale.append(path + " (no .idl for it)")
    if stale:
        sys.exit("genidl: generated files are stale or edited by hand "
                 f"({', '.join(stale)}):\nrun `make idl` and commit the result")


if __name__ == "__main__":
    main()
