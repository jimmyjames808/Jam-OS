#!/usr/bin/env python3
"""The driver build check (ARCHITECTURE.md "The migration rule").

    checkdriver.py <nm> <driver> <object> -- <header>...

Fails (exit 1, naming every offender) if the driver's object uses a symbol
it may not. A driver is compiled with nothing but <jam/driver.h>,
<jam/task.h>, <jam/abi.h>, <jam/status.h>, <jam/netframe.h> and
<jam/netdev.h> (static inline functions only), the generated <idl/*.h> and
the compiler's freestanding headers on its include path (-nostdinc), so neither a kernel
nor a libos header can even be included; this check closes the other door:
declaring a function yourself (`void *kmalloc(size_t);`, libos's
`printf`) and calling it, or calling into another driver. The Makefile
links each driver's objects into one relocatable object first (`ld -r`),
so references between the driver's own files are resolved and only what
it needs from outside is left undefined.

Allowed undefined symbols:
  - every function declared (not defined) in the given headers: driver.h's
    drv_* and driver_main, task.h's task_*, status.h's status_str; parsed
    from the headers, so the list can't drift from the surface;
  - memcpy, memmove, memset, memcmp: GCC may emit calls to these for
    struct copies, initialisers and loops even with -ffreestanding
    -fno-builtin (they are the freestanding environment's contract), and
    libos provides them. No allowed header declares them, and an implicit
    declaration is an error, so a driver can't call them by name by
    accident.
Nothing else: no libgcc helpers (they are not part of the surface), no
__stack_chk_fail (drivers build with -fno-stack-protector).

A driver also may not define a function of that surface itself (it would
shadow libos's)."""
import re
import subprocess
import sys

COMPILER_HELPERS = {"memcpy", "memmove", "memset", "memcmp"}

# Sections a driver object may have: its code and data. Anything else
# (.ktests, __ex_table, .limine_requests, .init_array, ...) is a table that
# something outside the driver (a linker script, a startup routine) would
# read and obey, and has no business in a driver.
ALLOWED_SECTION = re.compile(
    r"^(\.(text|rodata|data|bss)(\..*)?|\.debug_.*|\.comment|\.eh_frame|"
    r"\.note\.GNU-stack|\.note\.gnu\.property|\.group)$")


def declared_functions(path):
    """Names of the functions a header declares (prototypes, not inline
    definitions or macros)."""
    text = open(path).read()
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    # Drop preprocessor lines (with continuations).
    text = re.sub(r"^[ \t]*#(?:[^\n]*\\\n)*[^\n]*", " ", text, flags=re.M)
    # Drop the bodies of inline functions, structs and unions.
    while True:
        new = re.sub(r"\{[^{}]*\}", ";", text)
        if new == text:
            break
        text = new
    names = set()
    for stmt in text.split(";"):
        stmt = " ".join(stmt.split())
        if "(" not in stmt or re.match(r"(typedef|static|extern\s+inline)\b", stmt):
            continue   # types, and inline definitions (their bodies are gone)
        # The declarator's name: the identifier before the first '(' that
        # opens a parameter list (skipping __attribute__ and the like).
        m = re.match(r"^(?:[A-Za-z_][A-Za-z0-9_]*[\s\*]+)+\**\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(", stmt)
        if m and not m.group(1).startswith("__"):
            names.add(m.group(1))
    return names


def sections(nm_tool, obj):
    """Section names of obj, read with the objdump next to nm_tool."""
    tool = nm_tool[:-2] + "objdump" if nm_tool.endswith("nm") else "objdump"
    r = subprocess.run([tool, "-h", "-w", obj], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"checkdriver: {tool} -h {obj} failed:\n{r.stderr}")
    names = []
    for line in r.stdout.splitlines():
        f = line.split()
        if len(f) > 2 and f[0].isdigit():
            names.append(f[1])
    return names


def nm(tool, obj, *flags):
    r = subprocess.run([tool, *flags, obj], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"checkdriver: {tool} {' '.join(flags)} {obj} failed:\n{r.stderr}")
    return r.stdout.splitlines()


def main():
    if len(sys.argv) < 6 or "--" not in sys.argv:
        sys.exit(__doc__)
    sep = sys.argv.index("--")
    tool, driver, objs, headers = sys.argv[1], sys.argv[2], sys.argv[3:sep], sys.argv[sep + 1:]
    if not objs or not headers:
        sys.exit(__doc__)
    surface = set()
    for h in headers:
        surface |= declared_functions(h)
    if "drv_log" not in surface or "driver_main" not in surface:
        sys.exit(f"checkdriver: couldn't read the driver surface from {' '.join(headers)}")
    allowed = surface | COMPILER_HELPERS
    bad = []
    for obj in objs:
        for sec in sections(tool, obj):
            if not ALLOWED_SECTION.match(sec):
                bad.append(f"  {obj}: has section '{sec}' (drivers get .text/.rodata/.data/.bss only)")
        for line in nm(tool, obj, "-u"):
            sym = line.split()[-1]
            if sym not in allowed:
                bad.append(f"  {obj}: uses '{sym}', which <jam/driver.h> doesn't provide")
        for line in nm(tool, obj, "-g", "--defined-only"):
            sym = line.split()[-1]
            if sym in surface and sym != "driver_main":
                bad.append(f"  {obj}: defines '{sym}' itself (it belongs to <jam/driver.h>)")
    if bad:
        sys.exit(f"checkdriver: driver '{driver}' breaks the driver rules "
                 "(ARCHITECTURE.md, \"The migration rule\"):\n" + "\n".join(bad))


if __name__ == "__main__":
    main()
