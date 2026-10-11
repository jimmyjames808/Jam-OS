"""The self-test's fake ports: their sources and recipes, written into a
temporary tree (selftest.py runs them; nothing here touches ports/).

Five small ports whose builds prove the builder's rules with the real
compiler and sysroot:

    liba    style make; a patch that adds A_PATCHED to a.h
    libb    style configure-plain (a hand-written configure); needs liba;
            installs libb.pc, whose paths must come out under the sysroot
    app     style custom; needs libb, so it links libb and liba; a program
            with a list, an executable nobody named, a data file and a
            symlink to it
    libx    a library nobody but sneaky mentions; an override of x.c and
            a new file (extra.h) it needs
    sneaky  needs liba only, but includes libx's header: must not build

Each source is a .tar.gz made here with fixed times, and each recipe
names it by a file:// URL with its real size and hash.
"""
import gzip
import hashlib
import io
import os
import shutil
import tarfile

EPOCH = 1700000000

LIB_MAKEFILE = """PREFIX ?= /usr/local
all: lib{n}.a
lib{n}.a: {n}.o
\t$(AR) rc $@ {n}.o
\t$(RANLIB) $@
{n}.o: {n}.c {n}.h
\t$(CC) $(CFLAGS) -c {n}.c
install: lib{n}.a
\tmkdir -p $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include
\tcp lib{n}.a $(DESTDIR)$(PREFIX)/lib/
\tcp {n}.h $(DESTDIR)$(PREFIX)/include/
"""

SOURCES = {
    "liba-1.0": {
        "a.c": "#include \"a.h\"\nint a(void) { return 40; }\n",
        "a.h": "#pragma once\nint a(void);\n",
        "Makefile": LIB_MAKEFILE.format(n="a"),
        "LICENSE": "liba: MIT, for the test\n",
    },
    "libx-1.0": {
        "x.c": "#include \"x.h\"\nint x(void) { return 7; }\n",
        "x.h": "#pragma once\nint x(void);\n",
        "Makefile": LIB_MAKEFILE.format(n="x"),
        "LICENSE": "libx: MIT, for the test\n",
    },
    "libb-1.0": {
        "configure": ("#!/bin/sh\n# a hand-written configure: the prefix into config.mk\n"
                      "prefix=/usr/local\nfor a; do case $a in --prefix=*) prefix=${a#*=};; "
                      "esac; done\nprintf 'PREFIX = %s\\n' \"$prefix\" > config.mk\n"),
        "b.c": ("#include <a.h>\n#include \"b.h\"\n#ifndef A_PATCHED\n"
                "#error liba's patch was not applied\n#endif\nint b(void) { return a() + 2; }\n"),
        "b.h": "#pragma once\nint b(void);\n",
        "libb.pc.in": ("prefix=@PREFIX@\nincludedir=${prefix}/include\nlibdir=${prefix}/lib\n"
                    "Name: libb\nDescription: libb\nVersion: 1.0\nCflags: -I${includedir}\n"
                    "Libs: -L${libdir} -lb -la\n"),
        "Makefile": ("include config.mk\nall: libb.a libb.pc\nlibb.a: b.o\n\t$(AR) rc $@ b.o\n"
                     "\t$(RANLIB) $@\nb.o: b.c b.h\n\t$(CC) $(CFLAGS) -c b.c\n"
                     "libb.pc: libb.pc.in\n\tsed 's,@PREFIX@,$(PREFIX),' libb.pc.in > libb.pc\n"
                     "install: all\n\tmkdir -p $(DESTDIR)$(PREFIX)/lib/pkgconfig "
                     "$(DESTDIR)$(PREFIX)/include\n\tcp libb.a $(DESTDIR)$(PREFIX)/lib/\n"
                     "\tcp b.h $(DESTDIR)$(PREFIX)/include/\n"
                     "\tcp libb.pc $(DESTDIR)$(PREFIX)/lib/pkgconfig/\n"),
        "COPYING": "libb: BSD-2-Clause, for the test\n",
    },
    "app-1.0": {
        "app.c": ("#include <stdio.h>\n#include <b.h>\n"
                  "int main(void) { printf(\"%d\\n\", b()); return 0; }\n"),
        "helper.c": "int main(void) { return 0; }\n",
        "data.txt": "app's data\n",
        "LICENSE": "app: ISC, for the test\n",
    },
    "sneaky-1.0": {
        "s.c": "#include <a.h>\n#include <x.h>\nint main(void) { return a() + x(); }\n",
        "LICENSE": "sneaky: MIT, for the test\n",
    },
}

APP_BUILD = """# app's build: links libb (and liba, which libb needs) from its sysroot.
build() {
    if "$PKG_CONFIG" --version > /dev/null 2>&1; then
        case $("$PKG_CONFIG" --cflags libb) in
        *"$JAMOS_SYSROOT/include"*) ;;
        *) echo "libb.pc's paths are not under the sysroot: $("$PKG_CONFIG" --cflags libb)"; exit 1 ;;
        esac
    fi
    "$CC" -O2 -o app app.c -lb -la
    "$CC" -O2 -o helper helper.c
}

install() {
    mkdir -p "$destdir$prefix/bin" "$destdir$prefix/share/app"
    cp app helper "$destdir$prefix/bin/"
    cp data.txt "$destdir$prefix/share/app/data.txt"
    ln -s data.txt "$destdir$prefix/share/app/alias.txt"
}
"""

SNEAKY_BUILD = """build() {
    "$CC" -O2 -o sneaky s.c -la
}

install() {
    mkdir -p "$destdir$prefix/bin"
    cp sneaky "$destdir$prefix/bin/"
}
"""

PATCH_A = """Description: mark a.h as patched, so libb can tell
Forwarded: not-needed (a test)
--- a/a.h
+++ b/a.h
@@ -1,2 +1,3 @@
 #pragma once
+#define A_PATCHED 1
 int a(void);
"""


def make_tar(path, top, files):
    """A .tar.gz of files (name -> text) under top/, with fixed times."""
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        d = tarfile.TarInfo(top)
        d.type, d.mode, d.mtime = tarfile.DIRTYPE, 0o755, EPOCH
        tar.addfile(d)
        for name in sorted(files):
            data = files[name].encode()
            info = tarfile.TarInfo("%s/%s" % (top, name))
            info.size, info.mtime = len(data), EPOCH
            info.mode = 0o755 if name == "configure" else 0o644
            tar.addfile(info, io.BytesIO(data))
    with open(path, "wb") as f:
        with gzip.GzipFile(fileobj=f, mode="wb", mtime=0) as gz:
            gz.write(raw.getvalue())


def source_line(path):
    with open(path, "rb") as f:
        data = f.read()
    return "source file://%s %d %s" % (path, len(data), hashlib.sha256(data).hexdigest())


def recipe(name, style, licence, src, extra):
    lines = ["name %s" % name, "version 1.0", "revision 1", "summary %s, for the self-test" % name,
             "licence %s" % licence, "licence-file %s" % ("COPYING" if name == "libb" else "LICENSE"),
             src, "place package", "style %s" % style]
    return "\n".join(lines + extra) + "\n"


RECIPES = {
    "liba": ("make", "MIT", ["library lib/liba.a include/a.h"]),
    "libx": ("make", "MIT", ["library lib/libx.a include/x.h"]),
    "libb": ("configure-plain", "BSD-2-Clause", ["needs liba >= 1.0",
                                                 "library lib/libb.a include/b.h lib/pkgconfig/libb.pc"]),
    "app": ("custom", "ISC", ["needs libb", "program bin/app", "files share/app",
                              "subpackage app-extra share/app/alias.txt"]),
    "sneaky": ("custom", "MIT", ["needs liba", "program bin/sneaky"]),
}


def write_tree(root, up, real_ports):
    """The fake ports tree at root, their sources' tarballs in up."""
    os.makedirs(up, exist_ok=True)
    for f in ("config.site", "jamos.cmake", "jamos.meson"):
        os.makedirs(root, exist_ok=True)
        shutil.copyfile(os.path.join(real_ports, f), os.path.join(root, f))
    for top, files in SOURCES.items():
        make_tar(os.path.join(up, top + ".tar.gz"), top, files)
    for name, (style, lic, extra) in RECIPES.items():
        d = os.path.join(root, name)
        os.makedirs(d)
        src = source_line(os.path.join(up, "%s-1.0.tar.gz" % name))
        with open(os.path.join(d, "recipe"), "w") as f:
            f.write(recipe(name, style, lic, src, extra))
    os.makedirs(os.path.join(root, "liba", "patches"))
    with open(os.path.join(root, "liba", "patches", "0001-mark.patch"), "w") as f:
        f.write(PATCH_A)
    for name, sh in (("app", APP_BUILD), ("sneaky", SNEAKY_BUILD)):
        with open(os.path.join(root, name, "build.sh"), "w") as f:
            f.write(sh)
    os.makedirs(os.path.join(root, "app", "wants"))
    with open(os.path.join(root, "app", "wants", "app"), "w") as f:
        f.write("posix\nmount /data r\n")
    overrides(root)


def overrides(root, digest=None):
    """libx's overrides: x.c replaced (by upstream's hash, or digest) and
    extra.h added (a file upstream doesn't have); libx builds only with
    both in."""
    d = os.path.join(root, "libx")
    os.makedirs(os.path.join(d, "override"), exist_ok=True)
    up = hashlib.sha256(SOURCES["libx-1.0"]["x.c"].encode()).hexdigest()
    with open(os.path.join(d, "OVERRIDES"), "w") as f:
        f.write("# x.c returns EXTRA\nx.c %s override/x.c\nextra.h - override/extra.h\n" %
                (digest or up))
    with open(os.path.join(d, "override", "x.c"), "w") as f:
        f.write("#include \"x.h\"\n#include \"extra.h\"\nint x(void) { return EXTRA; }\n")
    with open(os.path.join(d, "override", "extra.h"), "w") as f:
        f.write("#define EXTRA 8\n")
