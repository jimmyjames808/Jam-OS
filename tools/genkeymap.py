#!/usr/bin/env python3
"""Make the keyboard tables from abi/keymap/ (<keymap.h>).

    genkeymap.py gen          write the generated files
    genkeymap.py check        exit 1 (and name them) if any is missing or
                              stale; then the self-test; then, if
                              xkbcommon's compiler (`xkbcli`) or X's
                              (`xkbcomp`) is installed, compile each
                              layout's XKB text with it (else say so)
    genkeymap.py xkb <name>   print layout <name>'s XKB keymap
    genkeymap.py selftest     the parser refuses what it must, and every
                              keymap's XKB text holds together

Inputs: abi/keymap/keys.txt (the keys: XKB name, HID usage, evdev code)
and every other abi/keymap/<name>.txt (a layout; its own header says the
format). Outputs, committed like genidl's (so they can be read and
grepped):
    user/lib/keymap_keys.c     evdev code by HID usage and the reverse
    user/lib/keymap_<name>.c   per layout: the C table by evdev code (type,
                               keysyms, characters, repeat) and the XKB
                               text keymap, whose first line names the
                               layout: "// jamos-keymap <name>: ..."

The XKB text is a whole keymap (xkb_keymap with xkb_keycodes, xkb_types,
xkb_compatibility, xkb_symbols), written from these files alone: the
format as xkbcommon documents it, nothing taken from xkeyboard-config.
Its key numbers are evdev + 8; its modifiers are XKB's eight real ones in
their fixed order (Shift, Lock, Control, Mod1-Mod5), with the virtual
NumLock, Alt and Super bound to Mod2, Mod1 and Mod4 by the keys' modifier
map: the bits <keymap.h> names KEYMAP_MOD_*.

Keysym numbers are X's (keysymdef.h and XF86keysym.h, MIT), used as
facts. Run from the repository root (the Makefile does)."""
import os
import re
import shutil
import subprocess
import sys
import tempfile

KEYMAP_DIR = "abi/keymap"
KEYS_FILE = os.path.join(KEYMAP_DIR, "keys.txt")
OUT_DIR = "user/lib"
HID_USAGES = 0xE8
CODES = 256
XKB_OFFSET = 8
NAME_RE = re.compile(r"^[a-z0-9_-]{1,15}$")
KEYNAME_RE = re.compile(r"^[A-Za-z0-9+\-]{1,4}$")

# ---- keysyms ---------------------------------------------------------------

# X's names for printable ASCII other than letters and digits, by code.
ASCII_NAMES = {
    " ": "space", "!": "exclam", '"': "quotedbl", "#": "numbersign", "$": "dollar",
    "%": "percent", "&": "ampersand", "'": "apostrophe", "(": "parenleft",
    ")": "parenright", "*": "asterisk", "+": "plus", ",": "comma", "-": "minus",
    ".": "period", "/": "slash", ":": "colon", ";": "semicolon", "<": "less",
    "=": "equal", ">": "greater", "?": "question", "@": "at", "[": "bracketleft",
    "\\": "backslash", "]": "bracketright", "^": "asciicircum", "_": "underscore",
    "`": "grave", "{": "braceleft", "|": "bar", "}": "braceright", "~": "asciitilde",
}

NAMED = {
    "BackSpace": 0xFF08, "Tab": 0xFF09, "Return": 0xFF0D, "Pause": 0xFF13,
    "Scroll_Lock": 0xFF14, "Sys_Req": 0xFF15, "Escape": 0xFF1B, "Delete": 0xFFFF,
    "Home": 0xFF50, "Left": 0xFF51, "Up": 0xFF52, "Right": 0xFF53, "Down": 0xFF54,
    "Prior": 0xFF55, "Next": 0xFF56, "End": 0xFF57, "Begin": 0xFF58,
    "Print": 0xFF61, "Insert": 0xFF63, "Menu": 0xFF67, "Break": 0xFF6B,
    "Num_Lock": 0xFF7F, "ISO_Left_Tab": 0xFE20,
    "KP_Space": 0xFF80, "KP_Enter": 0xFF8D, "KP_Home": 0xFF95, "KP_Left": 0xFF96,
    "KP_Up": 0xFF97, "KP_Right": 0xFF98, "KP_Down": 0xFF99, "KP_Prior": 0xFF9A,
    "KP_Next": 0xFF9B, "KP_End": 0xFF9C, "KP_Begin": 0xFF9D, "KP_Insert": 0xFF9E,
    "KP_Delete": 0xFF9F, "KP_Equal": 0xFFBD, "KP_Multiply": 0xFFAA, "KP_Add": 0xFFAB,
    "KP_Separator": 0xFFAC, "KP_Subtract": 0xFFAD, "KP_Decimal": 0xFFAE,
    "KP_Divide": 0xFFAF,
    "Shift_L": 0xFFE1, "Shift_R": 0xFFE2, "Control_L": 0xFFE3, "Control_R": 0xFFE4,
    "Caps_Lock": 0xFFE5, "Alt_L": 0xFFE9, "Alt_R": 0xFFEA, "Super_L": 0xFFEB,
    "Super_R": 0xFFEC,
    "XF86AudioLowerVolume": 0x1008FF11, "XF86AudioMute": 0x1008FF12,
    "XF86AudioRaiseVolume": 0x1008FF13, "XF86PowerOff": 0x1008FF2A,
}
for _i in range(10):
    NAMED[f"KP_{_i}"] = 0xFFB0 + _i
for _i in range(1, 36):
    NAMED[f"F{_i}"] = 0xFFBE + _i - 1

KEYSYMS = dict(NAMED)
for _c in range(0x20, 0x7F):
    _ch = chr(_c)
    KEYSYMS[ASCII_NAMES.get(_ch, _ch)] = _c

# The modifier keysyms: the real modifier each key's modifier map gives,
# and the virtual modifier bound to it. They never repeat; nor do the lock
# keys.
MODIFIERS = {
    "Shift_L": ("Shift", None), "Shift_R": ("Shift", None),
    "Caps_Lock": ("Lock", None),
    "Control_L": ("Control", None), "Control_R": ("Control", None),
    "Alt_L": ("Mod1", "Alt"), "Alt_R": ("Mod1", "Alt"),
    "Num_Lock": ("Mod2", "NumLock"),
    "Super_L": ("Mod4", "Super"), "Super_R": ("Mod4", "Super"),
}
NO_REPEAT = set(MODIFIERS) | {"Scroll_Lock"}
REAL_MODS = ["Shift", "Lock", "Control", "Mod1", "Mod2", "Mod3", "Mod4", "Mod5"]

# The characters of the keysyms that aren't Latin-1 or Unicode keysyms.
SPECIAL_CP = {
    "BackSpace": 0x08, "Tab": 0x09, "ISO_Left_Tab": 0x09, "Return": 0x0A,
    "KP_Enter": 0x0A, "Escape": 0x1B, "KP_Space": 0x20, "KP_Multiply": 0x2A,
    "KP_Add": 0x2B, "KP_Separator": 0x2C, "KP_Subtract": 0x2D, "KP_Decimal": 0x2E,
    "KP_Divide": 0x2F, "KP_Equal": 0x3D,
}
for _i in range(10):
    SPECIAL_CP[f"KP_{_i}"] = 0x30 + _i


def codepoint(name):
    """The character keysym `name` types, 0 for none (the layout file's rule)."""
    sym = KEYSYMS[name]
    if 0x20 <= sym <= 0x7E or 0xA0 <= sym <= 0xFF:
        return sym
    if 0x01000000 <= sym <= 0x0110FFFF:
        return sym - 0x01000000
    return SPECIAL_CP.get(name, 0)


# ---- types -----------------------------------------------------------------

# Per type: its levels, the C enum, and its XKB definition (modifiers, map).
TYPES = {
    "ONE_LEVEL": (1, "KEYMAP_ONE_LEVEL", "none", [], ["Any"]),
    "TWO_LEVEL": (2, "KEYMAP_TWO_LEVEL", "Shift", [("Shift", 2)], ["Base", "Shift"]),
    "ALPHABETIC": (2, "KEYMAP_ALPHABETIC", "Shift+Lock", [("Shift", 2), ("Lock", 2)],
                   ["Base", "Caps"]),
    "KEYPAD": (2, "KEYMAP_KEYPAD", "NumLock", [("NumLock", 2)], ["Base", "Number"]),
}
TYPE_NOTES = {
    "ALPHABETIC": "Shift or Caps Lock, not both, picks level 2",
    "KEYPAD": "Num Lock alone picks level 2: Shift changes nothing here",
}


# ---- the input files ---------------------------------------------------------

class Bad(Exception):
    """A problem in an input file: file:line: what."""


def lines_of(path, text=None):
    """(line number, words) of each statement line."""
    if text is None:
        try:
            with open(path) as f:
                text = f.read()
        except OSError as e:
            raise Bad(f"{path}: {e.strerror}") from None
    for n, raw in enumerate(text.split("\n"), 1):
        line = raw.split("#", 1)[0].strip()
        if line:
            yield n, line.split()


def read_keys(path=KEYS_FILE, text=None):
    """keys.txt: (keys by name -> evdev, evdev by usage, usage by evdev)."""
    by_name, by_usage, usage_of = {}, {}, {}
    for n, w in lines_of(path, text):
        if len(w) < 3:
            raise Bad(f"{path}:{n}: want <name> <usage> <evdev>")
        name = w[0]
        try:
            usage, code = int(w[1], 16), int(w[2], 10)
        except ValueError:
            raise Bad(f"{path}:{n}: bad number") from None
        if not KEYNAME_RE.match(name):
            raise Bad(f"{path}:{n}: bad key name {name!r}")
        if not 0 < usage < HID_USAGES or not 0 < code < CODES or not w[1].startswith("0x"):
            raise Bad(f"{path}:{n}: usage or code out of range")
        if usage in by_usage:
            raise Bad(f"{path}:{n}: usage {usage:#x} listed twice")
        if by_name.get(name, code) != code:
            raise Bad(f"{path}:{n}: {name} has two codes")
        if any(c == code and k != name for k, c in by_name.items()):
            raise Bad(f"{path}:{n}: code {code} has two names")
        by_name[name] = code
        by_usage[usage] = code
        usage_of.setdefault(code, usage)
    return by_name, by_usage, usage_of


def read_layout(path, keys, text=None):
    """A layout file: {name, description, keys: [(key, type, [syms])]}."""
    lay = {"name": None, "description": None, "keys": [], "file": path}
    seen = set()
    for n, w in lines_of(path, text):
        where = f"{path}:{n}"
        if w[0] in ("layout", "description"):
            field = "name" if w[0] == "layout" else w[0]
            if lay[field] is not None or len(w) < 2:
                raise Bad(f"{where}: one {w[0]} line, with a value")
            lay[field] = " ".join(w[1:])
            continue
        key, rest = w[0], w[1:]
        if key not in keys:
            raise Bad(f"{where}: no key {key!r} in {KEYS_FILE}")
        if key in seen:
            raise Bad(f"{where}: {key} given twice")
        if not rest or rest[0] not in TYPES:
            raise Bad(f"{where}: {key}: want a type ({', '.join(TYPES)})")
        syms = rest[1:]
        if len(syms) != TYPES[rest[0]][0]:
            raise Bad(f"{where}: {key}: {rest[0]} has {TYPES[rest[0]][0]} level(s)")
        for s in syms:
            if s not in KEYSYMS:
                raise Bad(f"{where}: {key}: unknown keysym {s!r}")
        seen.add(key)
        lay["keys"].append((key, rest[0], syms))
    if not lay["name"] or not NAME_RE.match(lay["name"]):
        raise Bad(f"{path}: a `layout <name>` line ([a-z0-9_-], at most 15) is needed")
    if not lay["description"] or '"' in lay["description"] or "\\" in lay["description"]:
        raise Bad(f"{path}: a `description` line without quotes or backslashes is needed")
    if os.path.basename(path) != lay["name"] + ".txt":
        raise Bad(f"{path}: layout {lay['name']} belongs in {lay['name']}.txt")
    return lay


def layout_files():
    return sorted(os.path.join(KEYMAP_DIR, f) for f in os.listdir(KEYMAP_DIR)
                  if f.endswith(".txt") and f != "keys.txt")


# ---- the XKB text --------------------------------------------------------------

def xkb_text(lay, keys):
    """The whole XKB keymap of a layout, its first line naming it."""
    name = lay["name"]
    used = {k: (t, s) for k, t, s in lay["keys"]}
    out = [f"// jamos-keymap {name}: {lay['description']}, from {lay['file']} by "
           f"tools/genkeymap.py", "xkb_keymap {"]
    out += xkb_keycodes(name, keys, used)
    out += xkb_types(name)
    out += xkb_compat(name)
    out += xkb_symbols(lay, used)
    out.append("};")
    return "\n".join(out) + "\n"


def xkb_keycodes(name, keys, used):
    out = [f'xkb_keycodes "jamos-{name}" {{', "    minimum = 8;", "    maximum = 255;"]
    for key, code in sorted(keys.items(), key=lambda kc: kc[1]):
        if key in used:
            out.append(f"    <{key}> = {code + XKB_OFFSET};")
    out += ['    indicator 1 = "Caps Lock";', '    indicator 2 = "Num Lock";',
            '    indicator 3 = "Scroll Lock";', "};"]
    return out


def xkb_types(name):
    out = [f'xkb_types "jamos-{name}" {{', "    virtual_modifiers NumLock;"]
    for tname, (_, _, mods, maps, names) in TYPES.items():
        if tname in TYPE_NOTES:
            out.append(f"    // {TYPE_NOTES[tname]}")
        out.append(f'    type "{tname}" {{')
        out.append(f"        modifiers = {mods};")
        for mod, level in maps:
            out.append(f"        map[{mod}] = Level{level};")
        for i, lname in enumerate(names, 1):
            out.append(f'        level_name[Level{i}] = "{lname}";')
        out.append("    };")
    out.append("};")
    return out


def xkb_compat(name):
    out = [f'xkb_compatibility "jamos-{name}" {{', "    virtual_modifiers NumLock,Alt,Super;",
           "    interpret.useModMapMods = AnyLevel;", "    interpret.repeat = False;",
           "    interpret.locking = False;"]
    for sym, (real, virt) in MODIFIERS.items():
        out.append(f"    interpret {sym}+AnyOfOrNone(all) {{")
        if virt:
            out.append(f"        virtualModifier = {virt};")
        mods = virt or real
        if sym in ("Caps_Lock", "Num_Lock"):
            out.append(f"        action = LockMods(modifiers={mods});")
        else:
            out.append(f"        action = SetMods(modifiers={mods},clearLocks);")
        out.append("    };")
    out += ['    indicator "Caps Lock" {', "        whichModState = Locked;",
            "        modifiers = Lock;", "    };",
            '    indicator "Num Lock" {', "        whichModState = Locked;",
            "        modifiers = NumLock;", "    };", "};"]
    return out


def xkb_symbols(lay, used):
    out = [f'xkb_symbols "jamos-{lay["name"]}" {{',
           f'    name[Group1] = "{lay["description"]}";']
    for key, tname, syms in lay["keys"]:
        rep = ", repeat = No" if syms[0] in NO_REPEAT else ""
        out.append(f'    key <{key}> {{ type = "{tname}"{rep}, [ {", ".join(syms)} ] }};')
    for real in REAL_MODS:
        ks = [k for k, (_, s) in used.items() if s[0] in MODIFIERS and MODIFIERS[s[0]][0] == real]
        if ks:
            out.append(f"    modifier_map {real} {{ {', '.join(f'<{k}>' for k in ks)} }};")
    out.append("};")
    return out


# ---- the C files -----------------------------------------------------------------

HEADER = """/* {what}
 * Made by tools/genkeymap.py from {src}:
 * don't edit it; change what it is made from and run `make keymap` (`make
 * check` fails while it is stale). <keymap.h> says how the tables are used. */
#include <keymap.h>
"""


def c_keys(keys_by_usage, usage_of):
    out = [HEADER.format(what="The keys: evdev code by HID usage, and back (keymap_keys.c).",
                         src=KEYS_FILE),
           "const uint8_t keymap_evdev_by_hid[KEYMAP_HID_USAGES] = {"]
    for usage in sorted(keys_by_usage):
        out.append(f"    [0x{usage:02x}] = {keys_by_usage[usage]},")
    out += ["};", "", "const uint8_t keymap_hid_by_evdev[KEYMAP_CODES] = {"]
    for code in sorted(usage_of):
        out.append(f"    [{code}] = 0x{usage_of[code]:02x},")
    out.append("};")
    return "\n".join(out) + "\n"


def c_string(text):
    """text as C string literal lines, one per line of text."""
    out = []
    for line in text.split("\n")[:-1]:
        esc = line.replace("\\", "\\\\").replace('"', '\\"')
        out.append(f'    "{esc}\\n"')
    return out


def c_layout(lay, keys):
    name = lay["name"]
    out = [HEADER.format(what=f"The {lay['description']} layout: the C table and the XKB keymap "
                              f"(keymap_{name}.c).", src=f"{lay['file']} and {KEYS_FILE}"),
           "/* By evdev code: type, repeats, reserved, keysyms, characters. */",
           "static const struct keymap_key keys[KEYMAP_CODES] = {"]
    for key, tname, syms in sorted(lay["keys"], key=lambda k: keys[k[0]]):
        both = syms if len(syms) == 2 else [syms[0], None]
        sym = [f"0x{KEYSYMS[s]:x}" if s else "0" for s in both]
        cp = [f"0x{codepoint(s):x}" if s and codepoint(s) else "0" for s in both]
        rep = "false" if syms[0] in NO_REPEAT else "true"
        out.append(f"    /* {key}: {' '.join(syms)} */")
        out.append(f"    [{keys[key]}] = {{ {TYPES[tname][1]}, {rep}, 0, {{ {sym[0]}, {sym[1]} }}, "
                   f"{{ {cp[0]}, {cp[1]} }} }},")
    out += ["};", "", "/* The XKB keymap (format xkb_v1), sent as wl_keyboard.keymap. */",
            "static const char xkb[] ="]
    out += c_string(xkb_text(lay, keys))
    out[-1] += ";"
    out += ["", f"const struct keymap keymap_{name} = {{",
            f'    "{name}", "{lay["description"]}", keys, xkb, sizeof(xkb),', "};"]
    return "\n".join(out) + "\n"


def outputs():
    """{path: text} of every generated file."""
    keys, by_usage, usage_of = read_keys()
    files = {os.path.join(OUT_DIR, "keymap_keys.c"): c_keys(by_usage, usage_of)}
    for path in layout_files():
        lay = read_layout(path, keys)
        files[os.path.join(OUT_DIR, f"keymap_{lay['name']}.c")] = c_layout(lay, keys)
    return files


# ---- checks ------------------------------------------------------------------------

def xkb_holds(text, lay, keys):
    """Problems of one generated XKB text (its structure, not XKB's grammar)."""
    problems = []
    first = text.split("\n", 1)[0]
    if not first.startswith(f"// jamos-keymap {lay['name']}: "):
        problems.append(f"first line {first!r} doesn't name {lay['name']}")
    if text.count("{") != text.count("}"):
        problems.append("braces don't balance")
    defined = set(re.findall(r"^    <([^>]+)> = (\d+);$", text, re.M))
    names = {n for n, _ in defined}
    for n, num in defined:
        if keys.get(n) is None or int(num) != keys[n] + XKB_OFFSET:
            problems.append(f"<{n}> = {num} is not {KEYS_FILE}'s code + 8")
    types = set(re.findall(r'^    type "([A-Z_]+)" \{$', text, re.M))
    key_re = r'^    key <([^>]+)> \{ type = "([A-Z_]+)"[^[]*\[ ([^]]*) \] \};$'
    for key, tname, syms in re.findall(key_re, text, re.M):
        if key not in names:
            problems.append(f"<{key}> has symbols but no key code")
        if tname not in types:
            problems.append(f"<{key}> uses type {tname}, not defined")
        elif len(syms.split(", ")) != TYPES[tname][0]:
            problems.append(f"<{key}>: {len(syms.split(', '))} symbol(s) for {tname}")
    if len(re.findall(r"^    key <", text, re.M)) != len(lay["keys"]):
        problems.append("not every key of the layout has its symbols")
    for real, ks in re.findall(r"^    modifier_map (\w+) \{ ([^}]*) \};$", text, re.M):
        if real not in REAL_MODS:
            problems.append(f"modifier_map {real}: not a real modifier")
        for k in re.findall(r"<([^>]+)>", ks):
            if k not in names:
                problems.append(f"modifier_map {real}: <{k}> has no key code")
    return problems


REFUSED = [
    ("no layout line", "description X\nAE01 TWO_LEVEL 1 exclam\n"),
    ("bad name", "layout US\ndescription X\n"),
    ("unknown key", "layout t\ndescription X\nXYZ1 ONE_LEVEL a\n"),
    ("unknown type", "layout t\ndescription X\nAE01 THREE_LEVEL 1 2 3\n"),
    ("too few levels", "layout t\ndescription X\nAE01 TWO_LEVEL 1\n"),
    ("too many levels", "layout t\ndescription X\nESC ONE_LEVEL Escape Return\n"),
    ("unknown keysym", "layout t\ndescription X\nESC ONE_LEVEL Esc\n"),
    ("a key twice", "layout t\ndescription X\nESC ONE_LEVEL Escape\nESC ONE_LEVEL Escape\n"),
    ("two layout lines", "layout t\nlayout t\ndescription X\n"),
    ("quote in description", 'layout t\ndescription "X"\n'),
]
REFUSED_KEYS = [
    ("a usage twice", "AE01 0x1e 2\nAE02 0x1e 3\n"),
    ("a name with two codes", "AE01 0x1e 2\nAE01 0x1f 3\n"),
    ("a code with two names", "AE01 0x1e 2\nAE02 0x1f 2\n"),
    ("a decimal usage", "AE01 30 2\n"),
    ("a code out of range", "AE01 0x1e 256\n"),
]


def selftest():
    """The parsers refuse what they must; every layout's XKB text holds."""
    failed = 0
    for what, text in REFUSED_KEYS:
        try:
            read_keys("t.txt", text)
            print(f"genkeymap selftest: {what}: accepted (it must be refused)")
            failed += 1
        except Bad:
            pass
    keys = read_keys()[0]
    for what, text in REFUSED:
        try:
            read_layout("t.txt", keys, text)
            print(f"genkeymap selftest: {what}: accepted (it must be refused)")
            failed += 1
        except Bad:
            pass
    read_layout("t.txt", keys, "layout t\ndescription Test\nAE01 TWO_LEVEL 1 exclam\n")
    for path in layout_files():
        lay = read_layout(path, keys)
        for p in xkb_holds(xkb_text(lay, keys), lay, keys):
            print(f"genkeymap selftest: {lay['name']}: {p}")
            failed += 1
    checks = len(REFUSED) + len(REFUSED_KEYS) + 1 + len(layout_files())
    print(f"genkeymap selftest: {'PASS' if not failed else 'FAILED'} ({checks} checks)")
    return failed == 0


def compile_xkb():
    """Each layout's XKB text through xkbcommon's or X's compiler, if there."""
    keys = read_keys()[0]
    xkbcli, xkbcomp = shutil.which("xkbcli"), shutil.which("xkbcomp")
    if not xkbcli and not xkbcomp:
        print("genkeymap: XKB text not compiled: no xkbcli (xkbcommon) or xkbcomp here")
        return True
    ok = True
    for path in layout_files():
        lay = read_layout(path, keys)
        with tempfile.NamedTemporaryFile("w", suffix=".xkb", delete=False) as f:
            f.write(xkb_text(lay, keys))
        try:
            if xkbcli:
                cmd = [xkbcli, "compile-keymap", "--from-xkb", f.name]
            else:
                cmd = [xkbcomp, "-w", "1", "-xkb", f.name, os.devnull]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                print(f"genkeymap: {' '.join(cmd[:2])} refused {lay['name']}'s XKB text:\n"
                      f"{r.stderr.strip()}")
                ok = False
            else:
                print(f"genkeymap: {lay['name']}'s XKB text compiles ({os.path.basename(cmd[0])})")
        finally:
            os.unlink(f.name)
    return ok


def main():
    args = sys.argv[1:]
    try:
        if args == ["gen"]:
            for path, text in outputs().items():
                with open(path, "w") as f:
                    f.write(text)
            return 0
        if args == ["check"]:
            stale = [p for p, t in outputs().items()
                     if not os.path.exists(p) or open(p).read() != t]
            for p in stale:
                print(f"genkeymap: {p} is missing or stale: run `make keymap`")
            return 0 if not stale and selftest() and compile_xkb() else 1
        if len(args) == 2 and args[0] == "xkb":
            keys = read_keys()[0]
            sys.stdout.write(xkb_text(read_layout(os.path.join(KEYMAP_DIR, args[1] + ".txt"),
                                                  keys), keys))
            return 0
        if args == ["selftest"]:
            return 0 if selftest() else 1
    except Bad as e:
        print(f"genkeymap: {e}")
        return 1
    print(__doc__.split("\n\n")[1])
    return 2


if __name__ == "__main__":
    sys.exit(main())
