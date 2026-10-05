#!/usr/bin/env python3
"""Generate Jam OS's Wayland tables and stubs from upstream's XML
(docs/G1-PLAN.md, "The generator: tools/genwl.py").

    genwl.py gen        write the generated files
    genwl.py check      exit 1 (and name them) if any is missing, stale or
                        left over from a protocol file that no longer exists
    genwl.py selftest   the generator's own tests (tools/genwl-tests/; they
                        compile and run C with the Mac's compiler, $HOSTCC
                        or cc)

Input: third_party/wayland-protocols/<file>.xml, upstream's interface files,
vendored unmodified (README.md there names the releases). Each file holds
one protocol, named as the file with '_' for '-' (xdg-shell.xml holds
xdg_shell). The XML is data: nothing of libwayland's code is used.

Outputs, committed like genidl's (so they can be read and grepped) and
checked by every build:
    user/include/jwl/<protocol>.h   per interface: its version, opcodes,
                                    enums, and typed stubs (below)
    user/lib/jwl_<protocol>.c       the tables libjwl's codec reads

The tables are the contract with libjwl (<jwl.h>, the plan's "contract the
first stage shares"): per interface a struct jwl_interface (name, upstream's
version, requests, events), per message a struct jwl_message:
    name        the XML's name
    signature   one letter per argument: i int, u uint, f fixed, s string,
                o object, n new_id, a array, h fd (a handle on Jam OS), with
                '?' before one the XML lets be null (strings and objects
                only), and, if the message came after version 1, its
                version first: wl_surface.damage_buffer is "4iiii". A new_id
                whose interface the XML doesn't name (wl_registry.bind)
                stands for three: the interface's name (s), its version (u)
                and the id (n), as on the wire: bind is "usun".
    types       one entry per letter: the interface an o or n argument must
                be, NULL for the others (and for an n whose interface is
                the s before it). A message with no o or n argument points
                at a shared array of NULLs.
An interface with no requests (or no events) has requests (events) NULL.
The XML's own order gives the opcodes, as Wayland's wire does.

The header, for each interface <i> (in C names: jwl_<i>, JWL_<I>):
    jwl_<i>_interface               its table
    JWL_<I>_VERSION                 upstream's version: what a compositor
                                    offers is its own choice, at most this
    JWL_<I>_REQ_<R>, JWL_<I>_EV_<E> opcodes, and ..._SINCE the version each
                                    came in
    JWL_<I>_REQ_DESTRUCTORS, ..._EV_DESTRUCTORS
                                    bit n set: message n destroys its object
                                    (`type="destructor"`; the table has no
                                    field for it)
    enum jwl_<i>_<e>, JWL_<I>_<E>_<V>   the XML's enums; entries that came
                                    later also get ..._SINCE
    jwl_<i>_<r>(c, self, args...)   a client's request r on object self
    jwl_<i>_send_<e>(c, self, args...)   the compositor's event e
    struct jwl_<i>_requests, jwl_<i>_dispatch_request(h, data, self, opcode, a)
    struct jwl_<i>_events,   jwl_<i>_dispatch_event(h, data, self, opcode, a)
                                    a handler per message, called with the
                                    arguments the codec decoded and checked
The stubs do no parsing or encoding of their own: a sender fills an
argument array and calls the codec's one send function; a dispatcher reads
the array the codec decoded. So there is one parser to review and fuzz.

Argument types in the stubs: int int32_t; uint uint32_t; fixed int32_t
(signed 24.8); string const char * (NULL only where the XML allows null);
object uint32_t, an id (0 for null, where allowed); new_id uint32_t, the id
the sender chose (with `const char *interface, uint32_t version` before it
when the XML names no interface); array `const void *x, uint32_t x_size`;
fd handle_t. An argument named like a C keyword or like the stubs' own
parameters (c, self, data) or starting with jwl_ gets a trailing '_'.

What the stubs need from <jwl.h> besides the two table structs (the names
are in CODEC below, so they can follow libjwl's): status_t, handle_t, OK,
ERR_NOT_SUPPORTED, ERR_INVALID_ARGS, `struct jwl_conn`, `union jwl_arg`
with members i u f s o n h and a.data a.size, and
    status_t jwl_send(struct jwl_conn *c, uint32_t id, uint32_t opcode,
                      const struct jwl_message *m, const union jwl_arg *args)

Fail closed: an element, attribute or value this generator doesn't know is
an error, not something skipped, so a new upstream feature can't be
generated half right.

Run from the repository root (the Makefile does)."""
import glob
import importlib.util
import os
import re
import sys
import textwrap
import xml.etree.ElementTree as ET

XML_DIR = "third_party/wayland-protocols"
INC_DIR = "user/include/jwl"
LIB_DIR = "user/lib"
TEST_DIR = "tools/genwl-tests"
MARK = "Generated by tools/genwl.py"

ARGS_MAX = 20           # letters per message: libjwl decodes into a fixed array
IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*$")
ENTRY = re.compile(r"[A-Za-z0-9_]+$")
C_KEYWORDS = set("""auto break case char const continue default do double else enum extern
    float for goto if inline int long register restrict return short signed sizeof static
    struct switch typedef union unsigned void volatile while _Bool _Alignas _Alignof _Atomic
    _Noreturn _Static_assert _Thread_local bool true false NULL""".split())
RESERVED = {"c", "self", "data"}    # the stubs' own parameters

LETTER = {"int": "i", "uint": "u", "fixed": "f", "string": "s", "object": "o",
          "new_id": "n", "array": "a", "fd": "h"}
NULLABLE = {"string", "object"}
CTYPE = {"i": "int32_t", "u": "uint32_t", "f": "int32_t", "s": "const char *",
         "o": "uint32_t", "n": "uint32_t", "h": "handle_t"}

# libjwl's names for what the stubs call (see the docstring).
CODEC = {"conn": "struct jwl_conn", "arg": "union jwl_arg", "send": "jwl_send",
         "members": {"i": ["i"], "u": ["u"], "f": ["f"], "s": ["s"], "o": ["o"],
                     "n": ["n"], "h": ["h"], "a": ["a.data", "a.size"]}}

# What each element may carry and hold (wayland.dtd, as these files use it).
ATTRS = {
    "protocol": {"name"},
    "copyright": set(),
    "description": {"summary"},
    "interface": {"name", "version", "frozen"},
    "request": {"name", "type", "since", "deprecated-since"},
    "event": {"name", "type", "since", "deprecated-since"},
    "enum": {"name", "since", "bitfield"},
    "entry": {"name", "value", "summary", "since", "deprecated-since"},
    "arg": {"name", "type", "summary", "interface", "allow-null", "enum"},
}
CHILDREN = {
    "protocol": {"copyright", "description", "interface"},
    "copyright": set(),
    "description": set(),
    "interface": {"description", "request", "event", "enum"},
    "request": {"description", "arg"},
    "event": {"description", "arg"},
    "enum": {"description", "entry"},
    "entry": {"description"},
    "arg": {"description"},
}
TEXT_OK = {"copyright", "description"}


class GenError(Exception):
    pass


def fail(where, msg):
    raise GenError(f"{where}: {msg}")


class Arg:
    def __init__(self, name, type_, iface, nullable, enum, summary):
        self.name, self.type, self.iface = name, type_, iface
        self.nullable, self.enum, self.summary = nullable, enum, summary
        self.letter = LETTER[type_]


class Message:
    def __init__(self, kind, opcode, name, since, destructor, args, summary):
        self.kind, self.opcode, self.name, self.since = kind, opcode, name, since
        self.destructor, self.args, self.summary = destructor, args, summary

    def letters(self):
        """(letter, interface name or None, nullable) per wire argument."""
        out = []
        for a in self.args:
            if a.letter == "n" and a.iface is None:
                out += [("s", None, False), ("u", None, False), ("n", None, False)]
            else:
                out.append((a.letter, a.iface, a.nullable))
        return out

    def signature(self):
        sig = "".join(("?" if null else "") + l for l, _, null in self.letters())
        return (str(self.since) if self.since > 1 else "") + sig


class Enum:
    def __init__(self, name, since, bitfield, entries, summary):
        self.name, self.since, self.bitfield = name, since, bitfield
        self.entries, self.summary = entries, summary   # [(name, value text, since, summary)]


class Interface:
    def __init__(self, name, version, summary):
        self.name, self.version, self.summary = name, version, summary
        self.requests, self.events, self.enums = [], [], []


class Protocol:
    def __init__(self, src, name, copyright_, interfaces):
        self.src, self.name, self.copyright, self.interfaces = \
            src, name, copyright_, interfaces


# ---- Parsing ----------------------------------------------------------------

def check_tree(e, where):
    """Every element, attribute and text in e is one this generator knows."""
    if e.tag not in ATTRS:
        fail(where, f"unknown element <{e.tag}>")
    for a in e.attrib:
        if a not in ATTRS[e.tag]:
            fail(where, f"<{e.tag}> has an unknown attribute '{a}'")
    if e.tag not in TEXT_OK and (e.text or "").strip():
        fail(where, f"<{e.tag}> has text")
    for c in e:
        if c.tag not in CHILDREN[e.tag]:
            fail(where, f"<{e.tag}> can't hold <{c.tag}>")
        if (c.tail or "").strip():
            fail(where, f"text after <{c.tag}>")
        check_tree(c, f"{where}, {c.tag} {c.get('name', '')}".rstrip())


def number(where, text, what, hi=0xffffffff):
    if text is None or not re.fullmatch(r"[1-9][0-9]*", text) or int(text) > hi:
        fail(where, f"{what} '{text}' is not a number in 1..{hi}")
    return int(text)


def flag(where, e, attr):
    v = e.get(attr, "false")
    if v not in ("true", "false"):
        fail(where, f"{attr}='{v}' (want true or false)")
    return v == "true"


def name_of(where, e, pattern=IDENT):
    n = e.get("name")
    if n is None or not pattern.match(n):
        fail(where, f"bad or missing name '{n}'")
    return n


def summary_of(e):
    s = e.get("summary")
    d = e.find("description")
    if s is None and d is not None:
        s = d.get("summary")
    return " ".join((s or "").split())


def parse_arg(where, e):
    name = name_of(where, e)
    where = f"{where}, arg {name}"
    t = e.get("type")
    if t not in LETTER:
        fail(where, f"unknown type '{t}'")
    iface = e.get("interface")
    if iface is not None and t not in ("object", "new_id"):
        fail(where, f"interface= on a {t}")
    if iface is not None and not IDENT.match(iface):
        fail(where, f"bad interface name '{iface}'")
    nullable = flag(where, e, "allow-null")
    if nullable and t not in NULLABLE:
        fail(where, f"allow-null on a {t} (only {', '.join(sorted(NULLABLE))})")
    enum = e.get("enum")
    if enum is not None and t not in ("int", "uint"):
        fail(where, f"enum= on a {t}")
    return Arg(name, t, iface, nullable, enum, summary_of(e))


def parse_message(where, e, opcode, version):
    name = name_of(where, e)
    where = f"{where}, {e.tag} {name}"
    since = number(where, e.get("since", "1"), "since", version)
    if "deprecated-since" in e.attrib:
        dep = number(where, e.get("deprecated-since"), "deprecated-since", version)
        if dep <= since:
            fail(where, f"deprecated-since {dep} is not after since {since}")
    kind = e.get("type")
    if kind not in (None, "destructor"):
        fail(where, f"type='{kind}' (only destructor)")
    args = [parse_arg(where, a) for a in e.findall("arg")]
    m = Message(e.tag, opcode, name, since, kind == "destructor", args, summary_of(e))
    if len(m.letters()) > ARGS_MAX:
        fail(where, f"{len(m.letters())} arguments (libjwl decodes at most {ARGS_MAX})")
    return m


def parse_enum(where, e, version):
    name = name_of(where, e)
    where = f"{where}, enum {name}"
    since = number(where, e.get("since", "1"), "since", version)
    entries, seen = [], set()
    for x in e.findall("entry"):
        ename = name_of(where, x, ENTRY)
        v = x.get("value")
        if v is None or not re.fullmatch(r"0x[0-9a-fA-F]+|0|[1-9][0-9]*", v) or \
                int(v, 0) > 0x7fffffff:
            fail(f"{where}, entry {ename}", f"value '{v}' is not a number in 0..0x7fffffff "
                 "(a C enum constant is an int)")
        if ename in seen:
            fail(where, f"entry '{ename}' twice")
        seen.add(ename)
        esince = number(f"{where}, entry {ename}", x.get("since", "1"), "since", version)
        if "deprecated-since" in x.attrib:
            number(f"{where}, entry {ename}", x.get("deprecated-since"), "deprecated-since",
                   version)
        entries.append((ename, v, esince, summary_of(x)))
    if not entries:
        fail(where, "no entries (C has no empty enum)")
    return Enum(name, since, flag(where, e, "bitfield"), entries, summary_of(e))


def parse_interface(where, e):
    name = name_of(where, e)
    where = f"{where}, interface {name}"
    version = number(where, e.get("version"), "version")
    flag(where, e, "frozen")
    i = Interface(name, version, summary_of(e))
    for kind, out in (("request", i.requests), ("event", i.events)):
        last = 1
        for e2 in e.findall(kind):
            m = parse_message(where, e2, len(out), version)
            if m.since < last:
                fail(f"{where}, {kind} {m.name}", f"since {m.since} comes after a "
                     f"{kind} of version {last} (the XML's order is the opcodes')")
            if any(o.name == m.name for o in out):
                fail(where, f"{kind} '{m.name}' twice")
            last = m.since
            out.append(m)
        if len(out) > 32:
            fail(where, f"more than 32 {kind}s (the destructor masks are 32 bits)")
    for x in e.findall("enum"):
        en = parse_enum(where, x, version)
        if any(o.name == en.name for o in i.enums):
            fail(where, f"enum '{en.name}' twice")
        i.enums.append(en)
    return i


def parse_text(src, text):
    """A Protocol from one XML file's text (src names it in errors)."""
    try:
        root = ET.fromstring(text)
    except ET.ParseError as e:
        fail(src, f"not well-formed XML ({e})")
    if root.tag != "protocol":
        fail(src, f"the root is <{root.tag}>, not <protocol>")
    check_tree(root, src)
    name = name_of(src, root)
    c = root.find("copyright")
    copyright_ = [l.strip() for l in (c.text or "").strip().split("\n")] if c is not None else []
    ifaces = [parse_interface(src, x) for x in root.findall("interface")]
    if not ifaces:
        fail(src, "no interfaces")
    return Protocol(src, name, copyright_, ifaces)


def parse_file(path):
    p = parse_text(path, open(path, encoding="utf-8").read())
    want = os.path.basename(path)[:-len(".xml")].replace("-", "_")
    if p.name != want:
        fail(path, f"holds protocol '{p.name}', but the file name says '{want}'")
    return p


# ---- Checks over all protocols ------------------------------------------------

def find_enum(ifaces, i, ref):
    iname, _, ename = ref.rpartition(".")
    owner = ifaces.get(iname) if iname else i
    if owner is None:
        return None
    return next((e for e in owner.enums if e.name == ename), None)


def resolve(protocols):
    """Interface and enum references resolve; returns {name: (protocol, interface)}."""
    where = {}
    for p in protocols:
        for i in p.interfaces:
            if i.name in where:
                fail(p.src, f"interface '{i.name}' is also in {where[i.name][0].src}")
            where[i.name] = (p, i)
    ifaces = {n: i for n, (_, i) in where.items()}
    for p in protocols:
        for i in p.interfaces:
            for m in i.requests + i.events:
                for a in m.args:
                    at = f"{p.src}, interface {i.name}, {m.kind} {m.name}, arg {a.name}"
                    if a.iface is not None and a.iface not in ifaces:
                        fail(at, f"interface '{a.iface}' is in no protocol file")
                    if a.enum is None:
                        continue
                    e = find_enum(ifaces, i, a.enum)
                    if e is None:
                        fail(at, f"enum '{a.enum}' is in no interface")
                    if e.bitfield and a.type != "uint":
                        fail(at, f"bitfield enum '{a.enum}' on a {a.type}")
    return where


def cname(n):
    """A C identifier for an XML name (a parameter or struct member)."""
    if n in C_KEYWORDS or n in RESERVED or n.startswith("jwl_"):
        return n + "_"
    return n


def slots(m):
    """Per wire argument slot: (letter, [(C type, parameter name)])."""
    out = []
    for a in m.args:
        n = cname(a.name)
        if a.letter == "n" and a.iface is None:
            out += [("s", [("const char *", "interface")]), ("u", [("uint32_t", "version")]),
                    ("n", [("uint32_t", n)])]
        elif a.letter == "a":
            out.append(("a", [("const void *", n), ("uint32_t", n + "_size")]))
        else:
            out.append((a.letter, [(CTYPE[a.letter], n)]))
    return out


def check_params(p, i, m):
    seen = set()
    for _, ps in slots(m):
        for _, n in ps:
            if n in seen or n in RESERVED:
                fail(p.src, f"interface {i.name}, {m.kind} {m.name}: two parameters named "
                     f"'{n}' in its stubs")
            seen.add(n)


def c_names(i):
    """Every file-scope C name the header defines for interface i, as
    (namespace, name): struct and enum tags are a namespace of their own in
    C (wl_shell_surface has both a request and an enum called resize)."""
    I = i.name.upper()
    out = [("", f"jwl_{i.name}_interface"), ("", f"JWL_{I}_VERSION")]
    for kind, tag, fn in (("requests", "REQ", ""), ("events", "EV", "send_")):
        msgs = getattr(i, kind)
        if not msgs:
            continue
        out += [("", f"JWL_{I}_{tag}_DESTRUCTORS"), ("tag", f"jwl_{i.name}_{kind}"),
                ("", f"jwl_{i.name}_dispatch_{kind[:-1]}")]
        for m in msgs:
            M = m.name.upper()
            out += [("", f"JWL_{I}_{tag}_{M}"), ("", f"JWL_{I}_{tag}_{M}_SINCE"),
                    ("", f"jwl_{i.name}_{fn}{m.name}")]
    for e in i.enums:
        E = e.name.upper()
        out.append(("tag", f"jwl_{i.name}_{e.name}"))
        for n, _, since, _ in e.entries:
            out.append(("", f"JWL_{I}_{E}_{n.upper()}"))
            if since > 1:
                out.append(("", f"JWL_{I}_{E}_{n.upper()}_SINCE"))
    return out


def check_names(protocols):
    """No two generated names are the same, and no parameter list repeats one."""
    seen = {}
    for p in protocols:
        for i in p.interfaces:
            for m in i.requests + i.events:
                check_params(p, i, m)
            for ns, n in c_names(i):
                if (ns, n) in seen:
                    fail(p.src, f"interface {i.name}: the generated name {n} is also "
                         f"{seen[ns, n]}'s")
                seen[ns, n] = f"interface {i.name}"


def load(paths):
    protocols = [parse_file(f) for f in paths]
    where = resolve(protocols)
    check_names(protocols)
    return protocols, where


# ---- Output helpers ---------------------------------------------------------------

def comment_text(s):
    return s.replace("*/", "* /")


def c_comment(lines, indent=""):
    """A C comment of lines, each broken at words to stay within 100 columns."""
    width = 100 - len(indent) - 6     # room for "/* " and " */"
    lines = [w for l in lines for w in (textwrap.wrap(comment_text(l), width) or [""])]
    if len(lines) == 1:
        return [f"{indent}/* {lines[0]} */"]
    out = [f"{indent}/* {lines[0]}"]
    out += [f"{indent} * {l}".rstrip() for l in lines[1:]]
    out[-1] += " */"
    return out


def wrap(head, items, tail, indent=""):
    """head + items joined by ', ' + tail, broken before 100 columns: the
    lines after the first start under the first item, or 8 columns in when
    even the first item doesn't fit after head."""
    pieces = [it + "," for it in items[:-1]] + [items[-1] + tail] if items else [tail]
    if len(indent + head + pieces[0]) > 100:
        lines, pad = [(indent + head).rstrip()], indent + " " * 8
        cur = pad + pieces[0]
    else:
        lines, pad = [], " " * len(indent + head)
        cur = indent + head + pieces[0]
    for piece in pieces[1:]:
        if len(cur) + 1 + len(piece) > 100:
            lines.append(cur)
            cur = pad + piece
        else:
            cur += " " + piece
    return lines + [cur]


def defines(pairs):
    w = max(len(n) for n, _ in pairs)
    return [f"#define {n:<{w}} {v}" for n, v in pairs]


def banner(p, what):
    lines = [f"{MARK} from {p.src}. Do not edit:",
             "change the XML or the generator and run `make wl`.", ""] + what
    if p.copyright:
        lines += ["", "Upstream's copyright and licence (the XML's own, kept here as its",
                  "licence asks):", ""] + p.copyright
    return c_comment(lines)


# ---- The header ------------------------------------------------------------------

def gen_constants(i):
    I = i.name.upper()
    out = []
    for kind, tag in (("requests", "REQ"), ("events", "EV")):
        msgs = getattr(i, kind)
        if not msgs:
            continue
        out.append(f"/* {i.name} {kind}: the opcode, and the version each came in. */")
        pairs = []
        for m in msgs:
            pairs += [(f"JWL_{I}_{tag}_{m.name.upper()}", f"{m.opcode}u"),
                      (f"JWL_{I}_{tag}_{m.name.upper()}_SINCE", f"{m.since}u")]
        mask = sum(1 << m.opcode for m in msgs if m.destructor)
        pairs.append((f"JWL_{I}_{tag}_DESTRUCTORS", f"0x{mask:08x}u"))
        out += defines(pairs)
        out.append("")
    return out


def gen_enum(i, e):
    I, E = i.name.upper(), e.name.upper()
    what = f"{i.name}.{e.name}" + (" (a bitfield)" if e.bitfield else "") + \
        (f", since {e.since}" if e.since > 1 else "") + (f": {e.summary}" if e.summary else "")
    out = c_comment([what]) + [f"enum jwl_{i.name}_{e.name} {{"]
    consts = [(f"JWL_{I}_{E}_{n.upper()} = {v},", s) for n, v, _, s in e.entries]
    w = max(len(c) for c, _ in consts)
    beside = all(len(f"    {c:<{w}} /* {comment_text(s)} */") <= 100 for c, s in consts)
    for c, s in consts:
        if s and beside:
            out.append(f"    {c:<{w}} /* {comment_text(s)} */")
        else:
            out += (c_comment([s], "    ") if s else []) + [f"    {c}"]
    out.append("};")
    later = [(f"JWL_{I}_{E}_{n.upper()}_SINCE", f"{since}u")
             for n, _, since, _ in e.entries if since > 1]
    if later:
        out += defines(later)
    out.append("")
    return out


def proto_params(m):
    return [f"{t} {n}" if not t.endswith("*") else f"{t}{n}"
            for _, ps in slots(m) for t, n in ps]


def gen_handlers(i, kind):
    msgs = getattr(i, kind)
    who, does = ("the compositor", "requests") if kind == "requests" else ("a client", "events")
    out = c_comment([f"What {who} does with each {i.name} {does[:-1]}: one handler per",
                     "message, or NULL (its dispatcher answers ERR_NOT_SUPPORTED). Each gets",
                     "the object's data (what libjwl's object map holds for it), its id",
                     "and the arguments the codec decoded and checked."])
    out.append(f"struct jwl_{i.name}_{kind} {{")
    for m in msgs:
        out += wrap(f"status_t (*{cname(m.name)})(",
                    ["void *data", "uint32_t self"] + proto_params(m), ");", "    ")
    out += ["};", ""]
    return out


def gen_sender(i, m):
    I = i.name.upper()
    tag, fn = ("REQ", "") if m.kind == "request" else ("EV", "send_")
    op = f"JWL_{I}_{tag}_{m.name.upper()}"
    table = "requests" if m.kind == "request" else "events"
    doc = f"{i.name}.{m.name}, {m.kind} {m.opcode}" + \
        (f", since {m.since}" if m.since > 1 else "") + \
        (", destroys the object" if m.destructor else "") + \
        (f": {m.summary}" if m.summary else "")
    out = c_comment([doc])
    out += wrap(f"static inline status_t jwl_{i.name}_{fn}{m.name}(",
                [f"{CODEC['conn']} *c", "uint32_t self"] + proto_params(m), ")")
    out.append("{")
    sl = slots(m)
    if sl:
        out.append(f"    const {CODEC['arg']} jwl_a[] = {{")
        for letter, ps in sl:
            inits = ", ".join(f".{mem} = {n}" for mem, (_, n) in
                              zip(CODEC["members"][letter], ps))
            out.append(f"        {{ {inits} }},")
        out.append("    };")
    out += wrap(f"return {CODEC['send']}(",
                ["c", "self", op, f"&jwl_{i.name}_interface.{table}[{op}]",
                 "jwl_a" if sl else "NULL"], ");", "    ")
    out += ["}", ""]
    return out


def gen_dispatch(i, kind):
    I = i.name.upper()
    msgs = getattr(i, kind)
    tag = "REQ" if kind == "requests" else "EV"
    out = c_comment([f"Calls h's handler for {i.name} {kind[:-1]} `opcode` on object `self`",
                     "(data: what the object map holds for it) with the arguments a, as",
                     "the codec decoded them. Returns the handler's status;",
                     "ERR_NOT_SUPPORTED if h has none for it (handles in a are then",
                     f"still the caller's, to close); ERR_INVALID_ARGS for an opcode {i.name}",
                     "doesn't have."])
    out += wrap(f"static inline status_t jwl_{i.name}_dispatch_{kind[:-1]}(",
                [f"const struct jwl_{i.name}_{kind} *h", "void *data", "uint32_t self",
                 "uint32_t opcode", f"const {CODEC['arg']} *a"], ")")
    out.append("{")
    if not any(slots(m) for m in msgs):
        out.append("    (void)a;")
    out.append("    switch (opcode) {")
    for m in msgs:
        call = []
        for k, (letter, _) in enumerate(slots(m)):
            call += [f"a[{k}].{mem}" for mem in CODEC["members"][letter]]
        h = f"h->{cname(m.name)}"
        out += [f"    case JWL_{I}_{tag}_{m.name.upper()}:",
                f"        if (!{h})",
                "            return ERR_NOT_SUPPORTED;"]
        out += wrap(f"return {h}(", ["data", "self"] + call, ");", "        ")
    out += ["    }", "    return ERR_INVALID_ARGS;", "}", ""]
    return out


def gen_interface_header(i):
    head = f"{i.name}, upstream version {i.version}" + (f": {i.summary}" if i.summary else "")
    out = ["", f"/* ---- {comment_text(head)} ---- */", ""]
    out += defines([(f"JWL_{i.name.upper()}_VERSION", f"{i.version}u")]) + [""]
    out += gen_constants(i)
    for e in i.enums:
        out += gen_enum(i, e)
    for kind in ("requests", "events"):
        if getattr(i, kind):
            out += gen_handlers(i, kind)
    for m in i.requests + i.events:
        out += gen_sender(i, m)
    for kind in ("requests", "events"):
        if getattr(i, kind):
            out += gen_dispatch(i, kind)
    return out


def deps_of(p, where):
    """The other protocols whose interfaces p's arguments name."""
    out = set()
    for i in p.interfaces:
        for m in i.requests + i.events:
            for a in m.args:
                if a.iface is not None and where[a.iface][0] is not p:
                    out.add(where[a.iface][0].name)
    return sorted(out)


def gen_header(p, where):
    what = [f"Protocol `{p.name}`: per interface its version, opcodes, enums and",
            "typed stubs (tools/genwl.py's docstring says what each name is); the",
            f"tables are in user/lib/jwl_{p.name}.c."]
    out = banner(p, what) + ["#pragma once", "", "#include <stddef.h>", "#include <stdint.h>", "",
                             "#include <jwl.h>"]
    out += [f"#include <jwl/{d}.h>" for d in deps_of(p, where)]
    out.append("")
    out += [f"extern const struct jwl_interface jwl_{i.name}_interface;" for i in p.interfaces]
    for i in p.interfaces:
        out += gen_interface_header(i)
    return "\n".join(out).rstrip("\n") + "\n"


# ---- The tables ------------------------------------------------------------------

def gen_messages(i, kind, nulls):
    msgs = getattr(i, kind)
    out = []
    for m in msgs:
        lets = m.letters()
        if any(l in "on" for l, _, _ in lets):
            out.append(f"static const struct jwl_interface *const types_{i.name}_{m.name}[] = {{")
            out += [f"    &jwl_{t}_interface," if t else "    NULL," for _, t, _ in lets]
            out.append("};")
    if not msgs:
        return out
    out.append(f"static const struct jwl_message {kind}_{i.name}[] = {{")
    for m in msgs:
        types = f"types_{i.name}_{m.name}" if any(l in "on" for l, _, _ in m.letters()) \
            else nulls
        out += wrap("{ ", [f'.name = "{m.name}"', f'.signature = "{m.signature()}"',
                           f".types = {types}"], " },", "    ")
    out += ["};"]
    return out


def gen_tables(p):
    what = [f"Protocol `{p.name}`'s tables: per interface its name, upstream's",
            "version and messages; per message its name, signature and argument",
            "interfaces (<jwl.h> says how libjwl's codec reads them)."]
    widest = max([len(m.letters()) for i in p.interfaces for m in i.requests + i.events] + [1])
    out = banner(p, what) + ["#include <stddef.h>", "", "#include <jwl.h>",
                             f"#include <jwl/{p.name}.h>", ""]
    out += c_comment(["For the messages with no object or new_id argument."])
    out.append(f"static const struct jwl_interface *const no_types[{widest}] = {{ NULL }};")
    for i in p.interfaces:
        out += ["", f"/* {i.name} */"]
        out += gen_messages(i, "requests", "no_types")
        out += gen_messages(i, "events", "no_types")
        out += [f"const struct jwl_interface jwl_{i.name}_interface = {{",
                f'    .name = "{i.name}",',
                f"    .version = {i.version},",
                f"    .nrequests = {len(i.requests)},",
                f"    .nevents = {len(i.events)},",
                f"    .requests = {f'requests_{i.name}' if i.requests else 'NULL'},",
                f"    .events = {f'events_{i.name}' if i.events else 'NULL'},",
                "};"]
    return "\n".join(out) + "\n"


# ---- gen / check ---------------------------------------------------------------

def xml_files():
    return sorted(glob.glob(os.path.join(XML_DIR, "*.xml")))


def outputs(protocols, where):
    out = {}
    for p in protocols:
        out[os.path.join(INC_DIR, p.name + ".h")] = gen_header(p, where)
        out[os.path.join(LIB_DIR, f"jwl_{p.name}.c")] = gen_tables(p)
    return out


def generated_files():
    """Files on disk that genwl wrote: all of INC_DIR, and the C files in
    LIB_DIR carrying its banner (libjwl's own jwl_*.c live there too)."""
    files = glob.glob(os.path.join(INC_DIR, "*.h"))
    for f in glob.glob(os.path.join(LIB_DIR, "jwl_*.c")):
        with open(f, encoding="utf-8") as fh:
            if MARK in fh.read(200):
                files.append(f)
    return sorted(files)


def gen_or_check(mode):
    protocols, where = load(xml_files())
    want = outputs(protocols, where)
    stale = []
    for path, text in want.items():
        try:
            old = open(path, encoding="utf-8").read()
        except FileNotFoundError:
            old = None
        if old == text:
            continue
        if mode == "gen":
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "w", encoding="utf-8").write(text)
            print(f"genwl: wrote {path}")
        else:
            stale.append(path)
    for path in generated_files():
        if path in want:
            continue
        if mode == "gen":
            os.remove(path)
            print(f"genwl: removed {path} (no protocol file for it)")
        else:
            stale.append(path + " (no protocol file for it)")
    if stale:
        sys.exit("genwl: generated files are stale or edited by hand "
                 f"({', '.join(stale)}):\nrun `make wl` and commit the result")


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("gen", "check", "selftest"):
        sys.exit(__doc__)
    try:
        if sys.argv[1] == "selftest":
            spec = importlib.util.spec_from_file_location(
                "genwl_selftest", os.path.join(TEST_DIR, "selftest.py"))
            test = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(test)
            sys.exit(test.run(sys.modules[__name__]))
        gen_or_check(sys.argv[1])
    except GenError as e:
        sys.exit(f"genwl: {e}")


if __name__ == "__main__":
    main()
