"""tools/genwl.py's own tests: `python3 tools/genwl.py selftest` (`make check`
runs it). Each part prints nothing when it passes; the run ends with PASS or
FAIL and one line per failure.

  sample     genwl's own small protocol (jwltest.xml): the names the header
             gives (renamed parameters, enums, since, destructor masks)
  rejects    XML the generator must refuse, each for the reason it names
  literals   opcodes, signatures, argument interfaces, versions and
             destructors of upstream's messages, written out by hand from
             the XML
  wire       a reference encoder and decoder for Wayland's wire format,
             driven by the generated signatures, against bytes written out by
             hand from the wire format's rules
  c          the generated C, built with the Mac's compiler ($HOSTCC or cc,
             -Wall -Wextra -Werror) against a stand-in <jwl.h>: harness.c
             prints every table (compared with the XML), then sends the
             `wire` messages and the sample's through the generated stubs and
             back through the generated dispatchers (compared with the
             arguments that went in)
"""
import os
import re
import shutil
import struct
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLE = os.path.join(HERE, "jwltest.xml")

# A stand-in for libjwl's <jwl.h>: the contract's two structs as the plan
# writes them, and what the stubs use of the codec (genwl.py's CODEC).
STUB_JWL_H = """#pragma once
#include <stdint.h>
typedef int32_t status_t;
typedef uint32_t handle_t;
#define OK 0
#define ERR_NOT_SUPPORTED (-2)
#define ERR_INVALID_ARGS (-10)
struct jwl_message;
struct jwl_interface { const char *name; uint32_t version; uint16_t nrequests, nevents;
                       const struct jwl_message *requests, *events;
                       uint32_t request_destructors, event_destructors; };
struct jwl_message { const char *name; const char *signature;
                     const struct jwl_interface *const *types; };
struct jwl_conn;
union jwl_arg { int32_t i; uint32_t u; int32_t f; const char *s; uint32_t o; uint32_t n;
                handle_t h; struct { const void *data; uint32_t size; } a; };
status_t jwl_send(struct jwl_conn *c, uint32_t id, uint32_t opcode, const struct jwl_message *m,
                  const union jwl_arg *args);
"""

# (interface, request or event, name) -> (opcode, signature, interfaces per letter)
LITERALS = {
    ("wl_display", "request", "sync"): (0, "n", ["wl_callback"]),
    ("wl_display", "request", "get_registry"): (1, "n", ["wl_registry"]),
    ("wl_display", "event", "error"): (0, "ous", [None, None, None]),
    ("wl_display", "event", "delete_id"): (1, "u", [None]),
    ("wl_registry", "request", "bind"): (0, "usun", [None, None, None, None]),
    ("wl_registry", "event", "global"): (0, "usu", [None, None, None]),
    ("wl_registry", "event", "global_remove"): (1, "u", [None]),
    ("wl_callback", "event", "done"): (0, "u", [None]),
    ("wl_shm", "request", "create_pool"): (0, "nhi", ["wl_shm_pool", None, None]),
    ("wl_shm", "request", "release"): (1, "2", []),
    ("wl_shm", "event", "format"): (0, "u", [None]),
    ("wl_surface", "request", "attach"): (1, "?oii", ["wl_buffer", None, None]),
    ("wl_surface", "request", "frame"): (3, "n", ["wl_callback"]),
    ("wl_surface", "request", "set_opaque_region"): (4, "?o", ["wl_region"]),
    ("wl_surface", "request", "commit"): (6, "", []),
    ("wl_surface", "request", "set_buffer_transform"): (7, "2i", [None]),
    ("wl_surface", "request", "set_buffer_scale"): (8, "3i", [None]),
    ("wl_surface", "request", "damage_buffer"): (9, "4iiii", [None] * 4),
    ("wl_surface", "event", "enter"): (0, "o", ["wl_output"]),
    ("wl_surface", "event", "preferred_buffer_transform"): (3, "6u", [None]),
    ("wl_seat", "request", "get_pointer"): (0, "n", ["wl_pointer"]),
    ("wl_seat", "request", "get_keyboard"): (1, "n", ["wl_keyboard"]),
    ("wl_seat", "request", "release"): (3, "5", []),
    ("wl_seat", "event", "capabilities"): (0, "u", [None]),
    ("wl_seat", "event", "name"): (1, "2s", [None]),
    ("wl_pointer", "request", "set_cursor"): (0, "u?oii", [None, "wl_surface", None, None]),
    ("wl_pointer", "request", "release"): (1, "3", []),
    ("wl_pointer", "event", "enter"): (0, "uoff", [None, "wl_surface", None, None]),
    ("wl_pointer", "event", "motion"): (2, "uff", [None] * 3),
    ("wl_pointer", "event", "axis"): (4, "uuf", [None] * 3),
    ("wl_pointer", "event", "frame"): (5, "5", []),
    ("wl_pointer", "event", "axis_discrete"): (8, "5ui", [None] * 2),
    ("wl_keyboard", "request", "release"): (0, "3", []),
    ("wl_keyboard", "event", "keymap"): (0, "uhu", [None] * 3),
    ("wl_keyboard", "event", "enter"): (1, "uoa", [None, "wl_surface", None]),
    ("wl_keyboard", "event", "modifiers"): (4, "uuuuu", [None] * 5),
    ("wl_keyboard", "event", "repeat_info"): (5, "4ii", [None] * 2),
    ("wl_output", "request", "release"): (0, "3", []),
    ("wl_output", "event", "geometry"): (0, "iiiiissi", [None] * 8),
    ("wl_output", "event", "mode"): (1, "uiii", [None] * 4),
    ("wl_output", "event", "done"): (2, "2", []),
    ("wl_output", "event", "scale"): (3, "2i", [None]),
    ("wl_data_offer", "request", "accept"): (0, "u?s", [None, None]),
    ("wl_data_offer", "request", "receive"): (1, "sh", [None, None]),
    ("xdg_wm_base", "request", "destroy"): (0, "", []),
    ("xdg_wm_base", "request", "create_positioner"): (1, "n", ["xdg_positioner"]),
    ("xdg_wm_base", "request", "get_xdg_surface"): (2, "no", ["xdg_surface", "wl_surface"]),
    ("xdg_wm_base", "request", "pong"): (3, "u", [None]),
    ("xdg_wm_base", "event", "ping"): (0, "u", [None]),
    ("xdg_surface", "request", "get_toplevel"): (1, "n", ["xdg_toplevel"]),
    ("xdg_surface", "request", "get_popup"): (2, "n?oo",
                                              ["xdg_popup", "xdg_surface", "xdg_positioner"]),
    ("xdg_surface", "request", "set_window_geometry"): (3, "iiii", [None] * 4),
    ("xdg_surface", "request", "ack_configure"): (4, "u", [None]),
    ("xdg_surface", "event", "configure"): (0, "u", [None]),
    ("xdg_toplevel", "request", "set_parent"): (1, "?o", ["xdg_toplevel"]),
    ("xdg_toplevel", "request", "set_title"): (2, "s", [None]),
    ("xdg_toplevel", "request", "move"): (5, "ou", ["wl_seat", None]),
    ("xdg_toplevel", "request", "resize"): (6, "ouu", ["wl_seat", None, None]),
    ("xdg_toplevel", "request", "set_fullscreen"): (11, "?o", ["wl_output"]),
    ("xdg_toplevel", "request", "set_minimized"): (13, "", []),
    ("xdg_toplevel", "event", "configure"): (0, "iia", [None] * 3),
    ("xdg_toplevel", "event", "close"): (1, "", []),
    ("xdg_toplevel", "event", "configure_bounds"): (2, "4ii", [None] * 2),
    ("xdg_toplevel", "event", "wm_capabilities"): (3, "5a", [None]),
}

# interface -> (version, request destructor mask, event destructor mask)
VERSIONS = {
    "wl_display": (1, 0, 0), "wl_registry": (1, 0, 0), "wl_callback": (1, 0, 1),
    "wl_compositor": (7, 1 << 2, 0), "wl_shm": (3, 1 << 1, 0), "wl_surface": (7, 1, 0),
    "wl_seat": (11, 1 << 3, 0), "wl_pointer": (11, 1 << 1, 0), "wl_keyboard": (11, 1, 0),
    "wl_output": (4, 1, 0), "wl_region": (7, 1, 0), "xdg_wm_base": (7, 1, 0),
    "xdg_surface": (7, 1, 0), "xdg_toplevel": (7, 1, 0), "xdg_popup": (7, 1, 0),
}

# Messages with their bytes on the wire, written out by hand from the wire
# format (little-endian u32s: the object's id; the size << 16 | the opcode;
# the arguments, strings and arrays as a u32 length then the bytes padded to
# 4; a string's length counts its NUL, a null string is length 0 alone; fds
# are not in the bytes). In the order harness.c's upstream() sends them:
# (interface, kind, name, object id, arguments, bytes in hex).
WIRE = [
    ("wl_display", "request", "get_registry", 1, [2],
     "01000000 01000c00 02000000"),
    ("wl_registry", "request", "bind", 2, [1, "wl_compositor", 4, 3],
     "02000000 00002800 01000000 0e000000 776c5f63 6f6d706f 7369746f 72000000 04000000"
     " 03000000"),
    ("wl_surface", "request", "attach", 5, [0, -1, 2],
     "05000000 01001400 00000000 ffffffff 02000000"),
    ("wl_shm", "request", "create_pool", 4, [6, 77, 4096],
     "04000000 00001000 06000000 00100000"),
    ("xdg_toplevel", "event", "configure", 7, [800, 600, bytes([1, 0, 0, 0, 4, 0, 0, 0])],
     "07000000 00001c00 20030000 58020000 08000000 01000000 04000000"),
    ("wl_pointer", "event", "motion", 8, [1000, 0xa80, -256],
     "08000000 02001400 e8030000 800a0000 00ffffff"),
    ("wl_display", "event", "error", 1, [5, 2, "bad"],
     "01000000 00001800 05000000 02000000 04000000 62616400"),
    ("wl_data_offer", "request", "accept", 10, [9, None],
     "0a000000 00001000 09000000 00000000"),
    ("wl_keyboard", "event", "keymap", 11, [1, 78, 4096],
     "0b000000 00001000 01000000 00100000"),
    ("xdg_toplevel", "request", "set_title", 7, ["Jam"],
     "07000000 02001000 04000000 4a616d00"),
    ("wl_seat", "event", "name", 12, ["seat0"],
     "0c000000 01001400 06000000 73656174 30000000"),
    ("xdg_toplevel", "event", "wm_capabilities", 7, [b""],
     "07000000 03000c00 00000000"),
]

# What harness.c's sample() sends: (id, opcode, name, signature, arguments).
SAMPLE_SENDS = [
    (1, 0, "make", "nu", [4, 0x40000000]),
    (1, 1, "bind", "usun", [5, "t_child", 1, 6]),
    (7, 2, "every", "2iufs?so?ooah", [-5, 7, 256, "hi", None, 3, 0, 9, b"abc", 42]),
    (7, 3, "destroy", "3", []),
    (4, 0, "poke", "iiuu", [1, 2, 3, 4]),
    (1, 0, "ping", "u", [77]),
    (1, 1, "gone", "2?o", [0]),
    (4, 0, "done", "u", [9]),
    (8, 0, "hush", "", []),
]
SAMPLE_TAIL = ["status -2", "status -10"]   # no handler; an opcode t_root lacks

# XML the generator must refuse: (protocol body, a piece of the error).
X = '<interface name="x" version="{v}">{body}</interface>'
REJECTS = [
    (X.format(v=1, body='<request name="r"><arg name="a" type="int" colour="red"/></request>'),
     "unknown attribute 'colour'"),
    (X.format(v=1, body='<request name="r"><frob/></request>'), "can't hold <frob>"),
    (X.format(v=1, body='<request name="r"><arg name="a" type="int">hi</arg></request>'),
     "has text"),
    (X.format(v=2, body='<request name="a" since="2"/><request name="b"/>'), "comes after"),
    (X.format(v=1, body='<request name="a" since="2"/>'), "since '2' is not a number in 1..1"),
    (X.format(v=1, body='<request name="a" type="constructor"/>'), "type='constructor'"),
    (X.format(v=1, body='<request name="a"/><request name="a"/>'), "request 'a' twice"),
    (X.format(v=1, body='<request name="a-b"/>'), "bad or missing name 'a-b'"),
    (X.format(v=1, body='<request name="r"><arg name="a" type="int" allow-null="true"/>'
                        '</request>'), "allow-null on a int"),
    (X.format(v=1, body='<request name="r"><arg name="a" type="int" interface="x"/>'
                        '</request>'), "interface= on a int"),
    (X.format(v=1, body='<request name="r"><arg name="a" type="object" interface="nope"/>'
                        '</request>'), "interface 'nope' is in no protocol file"),
    (X.format(v=1, body='<request name="r"><arg name="a" type="string" enum="e"/></request>'),
     "enum= on a string"),
    (X.format(v=1, body='<request name="r"><arg name="a" type="uint" enum="nope"/></request>'),
     "enum 'nope' is in no interface"),
    (X.format(v=1, body='<request name="r"><arg name="a" type="int" enum="e"/></request>'
                        '<enum name="e" bitfield="true"><entry name="k" value="1"/></enum>'),
     "bitfield enum 'e' on a int"),
    (X.format(v=1, body='<enum name="e"><entry name="k" value="0x80000000"/></enum>'),
     "0..0x7fffffff"),
    (X.format(v=1, body='<enum name="e"><entry name="k" value="-1"/></enum>'), "0..0x7fffffff"),
    (X.format(v=1, body='<request name="r">' + "".join(
        f'<arg name="a{k}" type="int"/>' for k in range(21)) + '</request>'), "21 arguments"),
    (X.format(v=1, body='<request name="y_z"/>'
                        '<enum name="req_y"><entry name="z" value="0"/></enum>'),
     "the generated name JWL_X_REQ_Y_Z"),
    (X.format(v=1, body='<request name="r"><arg name="interface" type="string"/>'
                        '<arg name="id" type="new_id"/></request>'),
     "two parameters named 'interface'"),
    (X.format(v=1, body='<request name="r"/>') + X.format(v=1, body=''), "interface 'x' is also"),
    (X.format(v=1, body='<request name="r">'), "not well-formed"),
    ('<interface name="x" version="0"/>', "version '0' is not a number"),
    ("", "no interfaces"),
]


def load_text(g, text):
    p = g.parse_text("test.xml", f'<protocol name="t">{text}</protocol>')
    where = g.resolve([p])
    g.check_names([p])
    return p, where


def t_rejects(g, fails):
    for body, want in REJECTS:
        try:
            load_text(g, body)
            fails.append(f"rejects: accepted {body[:70]}...")
        except g.GenError as e:
            if want not in str(e):
                fails.append(f"rejects: {body[:60]}...: said '{e}', want '{want}'")


def t_sample(g, fails):
    protocols, where = g.load([SAMPLE])
    p = protocols[0]
    h, c = g.gen_header(p, where), g.gen_tables(p)
    want_h = [
        "#define JWL_T_ROOT_VERSION 3u",
        r"#define JWL_T_ROOT_REQ_EVERY\s+2u",
        r"#define JWL_T_ROOT_REQ_EVERY_SINCE\s+2u",
        r"#define JWL_T_ROOT_REQ_DESTRUCTORS\s+0x00000008u",
        r"#define JWL_T_ROOT_EV_DESTRUCTORS\s+0x00000000u",
        r"#define JWL_T_CHILD_EV_DESTRUCTORS\s+0x00000001u",
        r"JWL_T_ROOT_ERROR_WORSE = 0x7,\s+/\* it was worse \*/",
        r"#define JWL_T_ROOT_ERROR_WORSE_SINCE 2u",
        r"JWL_T_CHILD_FLAGS_1ST = 1,",
        r"enum jwl_t_child_flags \{",
        r"/\* t_child.flags \(a bitfield\) \*/",
        r"jwl_t_child_poke\(struct jwl_conn \*c, uint32_t self, int32_t self_, "
        r"int32_t default_, uint32_t c_, uint32_t jwl_a_\)",
        r"status_t \(\*done\)\(void \*data, uint32_t self, uint32_t data_\);",
        r"jwl_t_root_bind\(struct jwl_conn \*c, uint32_t self, uint32_t name, "
        r"const char \*interface, uint32_t version, uint32_t id\)",
        r"const void \*bytes, uint32_t bytes_size, handle_t fd",
        r"\{ \.a\.data = bytes, \.a\.size = bytes_size \},",
        r"jwl_t_root_send_gone\(struct jwl_conn \*c, uint32_t self, uint32_t child\)",
        r"\(void\)a;",
    ]
    flat = " ".join(h.split())     # the patterns don't care where lines break
    for pat in want_h:
        if not re.search(pat.replace(r"\s+", " "), flat):
            fails.append(f"sample: header lacks /{pat}/")
    for pat in ("struct jwl_t_quiet_events", "jwl_t_quiet_dispatch_event", "JWL_T_QUIET_EV_"):
        if pat in h:
            fails.append(f"sample: header has {pat} for an interface with no events")
    for pat in ('.signature = "2iufs?so?ooah"', ".events = NULL,", '.signature = "usun"',
                "no_types[10] = { NULL };"):
        if pat not in c:
            fails.append(f"sample: tables lack {pat}")


def find_iface(protocols, name):
    return next((i for p in protocols for i in p.interfaces if i.name == name), None)


def find_message(protocols, iface, kind, name):
    i = find_iface(protocols, iface)
    msgs = (i.requests if kind == "request" else i.events) if i else []
    return next((m for m in msgs if m.name == name), None)


def t_literals(g, fails):
    protocols, _ = g.load(g.xml_files())
    for (iface, kind, name), (op, sig, types) in LITERALS.items():
        m = find_message(protocols, iface, kind, name)
        if m is None:
            fails.append(f"literals: no {iface}.{name}")
            continue
        got = (m.opcode, m.signature(), [t for _, t, _ in m.letters()])
        if got != (op, sig, types):
            fails.append(f"literals: {iface}.{name} is {got}, want {(op, sig, types)}")
    for iface, (version, rmask, emask) in VERSIONS.items():
        i = find_iface(protocols, iface)
        got = (i.version, sum(1 << m.opcode for m in i.requests if m.destructor),
               sum(1 << m.opcode for m in i.events if m.destructor))
        if got != (version, rmask, emask):
            fails.append(f"literals: {iface} version, destructors {got}, want "
                         f"{(version, rmask, emask)}")


def sig_letters(sig):
    out, null = [], False
    for ch in sig.lstrip("0123456789"):
        if ch == "?":
            null = True
            continue
        out.append((ch, null))
        null = False
    return out


def pad4(b):
    return b + bytes(-len(b) % 4)


def encode(obj, opcode, sig, args):
    """Wayland's bytes for one message, and the handles beside them."""
    body, handles = b"", []
    for (l, null), v in zip(sig_letters(sig), args):
        if l in "if":
            body += struct.pack("<i", v)
        elif l in "uon":
            body += struct.pack("<I", v)
        elif l == "s":
            if v is None and not null:
                raise ValueError("a null string where the signature allows none")
            if v is None:
                body += struct.pack("<I", 0)
            else:
                b = v.encode() + b"\0"
                body += struct.pack("<I", len(b)) + pad4(b)
        elif l == "a":
            body += struct.pack("<I", len(v)) + pad4(v)
        elif l == "h":
            handles.append(v)
    return struct.pack("<II", obj, (8 + len(body)) << 16 | opcode) + body, handles


def decode(data, sig, handles):
    """The object id, opcode and arguments of one message's bytes."""
    obj, word = struct.unpack_from("<II", data)
    if word >> 16 != len(data):
        raise ValueError(f"size {word >> 16} but {len(data)} bytes")
    at, args, handles = 8, [], list(handles)
    for l, _ in sig_letters(sig):
        if l == "h":
            args.append(handles.pop(0))
            continue
        (v,) = struct.unpack_from("<i" if l in "if" else "<I", data, at)
        at += 4
        if l == "s":
            args.append(None if v == 0 else data[at:at + v - 1].decode())
            at += v + (-v % 4)
        elif l == "a":
            args.append(data[at:at + v])
            at += v + (-v % 4)
        else:
            args.append(v)
    if at != len(data) or handles:
        raise ValueError("bytes or handles left over")
    return obj, word & 0xffff, args


def t_wire(g, fails):
    protocols, _ = g.load(g.xml_files())
    for iface, kind, name, obj, args, hexs in WIRE:
        m = find_message(protocols, iface, kind, name)
        want = bytes.fromhex(hexs.replace(" ", ""))
        try:
            data, handles = encode(obj, m.opcode, m.signature(), args)
            back = decode(want, m.signature(), handles)
        except (ValueError, struct.error, IndexError) as e:
            fails.append(f"wire: {iface}.{name}: {e}")
            continue
        if data != want:
            fails.append(f"wire: {iface}.{name} encodes to {data.hex()}, want {want.hex()}")
        if back != (obj, m.opcode, args):
            fails.append(f"wire: {iface}.{name} decodes to {back}, want {(obj, m.opcode, args)}")


def fmt_line(what, obj, opcode, name, sig, args):
    parts = []
    for (l, _), v in zip(sig_letters(sig), args):
        if l == "s":
            parts.append("s:null" if v is None else f's:"{v}"')
        elif l == "a":
            parts.append("a:" + v.hex())
        else:
            parts.append(f"{l}:{v}")
    return " ".join([what, str(obj), str(opcode), name, sig or "-"] + parts)


def want_dump(protocols):
    out = []
    for p in protocols:
        for i in p.interfaces:
            masks = [sum(1 << m.opcode for m in msgs if m.destructor)
                     for msgs in (i.requests, i.events)]
            out.append(f"I {i.name} {i.version} {len(i.requests)} {len(i.events)} "
                       f"{masks[0]:x} {masks[1]:x}")
            for tag, msgs in (("R", i.requests), ("E", i.events)):
                for m in msgs:
                    types = [t or "-" for _, t, _ in m.letters()]
                    out.append(" ".join([tag, str(m.opcode), m.name, m.signature() or "-"] +
                                        types))
    return out


def want_rounds(protocols):
    out = []
    for obj, op, name, sig, args in SAMPLE_SENDS:
        out += [fmt_line(w, obj, op, name, sig, args) for w in ("send", "got")] + ["status 0"]
    out += SAMPLE_TAIL
    for iface, kind, name, obj, args, _ in WIRE:
        m = find_message(protocols, iface, kind, name)
        out += [fmt_line(w, obj, m.opcode, name, m.signature(), args)
                for w in ("send", "got")] + ["status 0"]
    return out


def build(g, tmp, protocols, where):
    inc = os.path.join(tmp, "inc")
    os.makedirs(os.path.join(inc, "jwl"))
    open(os.path.join(inc, "jwl.h"), "w").write(STUB_JWL_H)
    srcs = [os.path.join(HERE, "harness.c")]
    for p in protocols:
        open(os.path.join(inc, "jwl", p.name + ".h"), "w").write(g.gen_header(p, where))
        srcs.append(os.path.join(tmp, f"jwl_{p.name}.c"))
        open(srcs[-1], "w").write(g.gen_tables(p))
        # Each header on its own: it includes what it uses.
        srcs.append(os.path.join(tmp, f"alone_{p.name}.c"))
        open(srcs[-1], "w").write(f"#include <jwl/{p.name}.h>\n")
    names = [i.name for p in protocols for i in p.interfaces]
    all_c = "".join(f"#include <jwl/{p.name}.h>\n" for p in protocols)
    all_c += "const struct jwl_interface *const genwl_all[] = {\n"
    all_c += "".join(f"    &jwl_{n}_interface,\n" for n in names) + "    NULL,\n};\n"
    srcs.append(os.path.join(tmp, "all.c"))
    open(srcs[-1], "w").write(all_c)
    exe = os.path.join(tmp, "harness")
    cc = os.environ.get("HOSTCC", "cc")
    cmd = [cc, "-std=gnu17", "-O1", "-Wall", "-Wextra", "-Werror", "-I", inc, "-o", exe] + srcs
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{' '.join(cmd)} failed:\n{r.stdout}{r.stderr}")
    return exe


def t_c(g, fails):
    protocols, where = g.load([SAMPLE] + g.xml_files())
    tmp = tempfile.mkdtemp(prefix="genwl-")
    try:
        exe = build(g, tmp, protocols, where)
        r = subprocess.run([exe], capture_output=True, text=True, timeout=30)
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as e:
        fails.append(f"c: {e}")
        return
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    got = r.stdout.splitlines()
    want = want_dump(protocols) + want_rounds(protocols)
    if r.returncode != 0:
        fails.append(f"c: the harness exited {r.returncode}")
    for k in range(max(len(got), len(want))):
        a = got[k] if k < len(got) else "(nothing)"
        b = want[k] if k < len(want) else "(nothing)"
        if a != b:
            fails.append(f"c: line {k + 1}: the harness printed\n    {a}\n  want\n    {b}")
            break


def run(g):
    fails = []
    for t in (t_sample, t_rejects, t_literals, t_wire, t_c):
        try:
            t(g, fails)
        except g.GenError as e:
            fails.append(f"{t.__name__[2:]}: {e}")
    for f in fails:
        print("genwl selftest: " + f)
    print("genwl selftest: %s" % ("FAIL" if fails else "PASS"))
    return 1 if fails else 0
