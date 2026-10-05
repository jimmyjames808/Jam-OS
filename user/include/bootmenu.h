/* The boot menu's check: Limine's limine.conf, as Jam OS writes it
 * (boot/limine.conf). init asks it of a new menu before `update -w`
 * writes that menu to the stick (espmenu.c), and `make check` asks it of
 * boot/limine.conf (build/host/menucheck, tools/menucheck.c), so a menu
 * the PC would refuse never reaches a commit unnoticed. A menu that fails
 * is not written: the stick keeps the menu it has. The code
 * (user/lib/bootmenu.c) uses no library at all, so the Mac's tool builds
 * the same file.
 *
 * What passes is a subset of Limine's syntax (Limine 11's CONFIG.md), the
 * part Jam OS's menu uses and this check can vouch for; anything else is
 * refused, never skipped:
 *   - printable ASCII but '$' (no macros), each line ended by '\n', at
 *     most BOOTMENU_LINE_MAX bytes a line, BOOTMENU_MAX in all;
 *   - blank lines, and comments: a line whose first byte is '#';
 *   - before the first entry, global options, unindented `key: value`:
 *     only `timeout`, exactly once, 1..BOOTMENU_TIMEOUT_MAX seconds or
 *     `no` (0 would boot the default at once: no "Jam OS (previous
 *     build)" to pick);
 *   - entries: `/<name>` at the top, `//<name>` inside a directory (a `/`
 *     entry followed by `//` ones, with no options of its own; `/+<name>`
 *     shows it open). A name is 1..BOOTMENU_NAME_MAX bytes;
 *   - an entry's options, indented `key: value` (one space after the
 *     colon, a value of at least one byte): `protocol` (only `limine`),
 *     `path` or `kernel_path`, `module_path` (one or more), `cmdline` or
 *     `kernel_cmdline`, `comment`; each once but module_path;
 *   - a path is `boot():` and a file on the boot partition (the ESP):
 *     `/`, then names of a-z 0-9 . _ - (lower case: how Limine's FAT
 *     driver matches case never matters) separated by single slashes, no
 *     name starting with '.', at most BOOTMENU_PATH_MAX bytes;
 *   - every entry that isn't a directory has a protocol, a kernel whose
 *     name ends in `jamos.elf` (a Jam OS kernel: the PC vouches for no
 *     other) and, as its first module, the boot image beside it (the same
 *     name with `bootfs.img` for `jamos.elf`: the kernel finds its modules
 *     by those endings);
 *   - the first entry (Limine's default; no `default_entry` line can move
 *     it) boots BOOTMENU_KERNEL; an entry at the top named
 *     BOOTMENU_PREV_NAME boots BOOTMENU_PREV_KERNEL;
 *   - every file a path names is there: exists() says so for the stick as
 *     it will be when the menu is in place (init calls it after the
 *     build's files are in theirs). It is called once per file, and only
 *     for a menu that passed everything else.
 * The check can't know what each kernel does with its command line, nor
 * whether Limine's own build has a menu checksum enrolled (espmenu.c
 * looks at that on the stick). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOTMENU_MAX         (64u << 10)   /* bytes of a menu, at most (<update.h>'s
                                            * UPDATE_MENU_MAX) */
#define BOOTMENU_LINE_MAX    255u          /* bytes of a line, '\n' not counted */
#define BOOTMENU_NAME_MAX    100u          /* bytes of an entry's name */
#define BOOTMENU_PATH_MAX    63u           /* bytes of a file's path on the ESP */
#define BOOTMENU_ENTRIES     64u           /* entries (directories too), at most */
#define BOOTMENU_FILES       8u            /* different files the menu names, at most */
#define BOOTMENU_MODULES     4u            /* module_path lines in one entry, at most */
#define BOOTMENU_TIMEOUT_MAX 600u          /* seconds */
#define BOOTMENU_KERNEL      "/boot/jamos.elf"
#define BOOTMENU_PREV_NAME   "Jam OS (previous build)"
#define BOOTMENU_PREV_KERNEL "/boot/prev-jamos.elf"

/* Why a menu was refused (struct bootmenu_result's why). */
enum bootmenu_why {
    BOOTMENU_OK,
    BOOTMENU_EMPTY,          /* no bytes */
    BOOTMENU_TOO_BIG,        /* over BOOTMENU_MAX */
    BOOTMENU_NO_NEWLINE,     /* the last line isn't ended by '\n' */
    BOOTMENU_BAD_BYTE,       /* a byte that isn't printable ASCII, or '$' */
    BOOTMENU_LONG_LINE,      /* a line over BOOTMENU_LINE_MAX */
    BOOTMENU_UNKNOWN_GLOBAL, /* a global line that isn't `timeout: ...` */
    BOOTMENU_GLOBAL_TWICE,   /* timeout twice */
    BOOTMENU_BAD_TIMEOUT,    /* not 1..BOOTMENU_TIMEOUT_MAX or `no` */
    BOOTMENU_NO_TIMEOUT,     /* no timeout line */
    BOOTMENU_GLOBAL_IN_ENTRY,/* an unindented option line after the first entry */
    BOOTMENU_OPTION_OUTSIDE, /* an indented line before the first entry */
    BOOTMENU_BAD_ENTRY,      /* `///`, no name, a name too long */
    BOOTMENU_PARENTLESS,     /* a `//` entry with no `/` above it */
    BOOTMENU_TOO_MANY,       /* over BOOTMENU_ENTRIES, BOOTMENU_FILES or BOOTMENU_MODULES */
    BOOTMENU_UNKNOWN_OPTION, /* an entry option this check can't vouch for */
    BOOTMENU_OPTION_TWICE,   /* the same option twice in one entry */
    BOOTMENU_OPTION_IN_DIR,  /* a directory with options of its own */
    BOOTMENU_BAD_PLUS,       /* `/+` on an entry with no `//` entries */
    BOOTMENU_BAD_PROTOCOL,   /* a protocol other than limine */
    BOOTMENU_NO_PROTOCOL,    /* an entry without one */
    BOOTMENU_BAD_PATH,       /* not boot():/ and a plain path, or too long */
    BOOTMENU_NO_PATH,        /* an entry without path (or kernel_path) */
    BOOTMENU_NOT_JAMOS,      /* a kernel whose name doesn't end in jamos.elf */
    BOOTMENU_BAD_PAIR,       /* the first module isn't the kernel's own boot image */
    BOOTMENU_NO_ENTRY,       /* no entry at all */
    BOOTMENU_BAD_DEFAULT,    /* the first entry doesn't boot BOOTMENU_KERNEL */
    BOOTMENU_NO_PREVIOUS,    /* no entry named BOOTMENU_PREV_NAME at the top */
    BOOTMENU_BAD_PREVIOUS,   /* ... it doesn't boot BOOTMENU_PREV_KERNEL */
    BOOTMENU_NO_FILE,        /* a file it names isn't there (exists() said so) */
    BOOTMENU_WHYS,
};

/* The verdict. */
struct bootmenu_result {
    uint32_t why;                          /* enum bootmenu_why */
    uint32_t line;                         /* the line it is about, from 1 (0: the whole
                                            * menu) */
    char     path[BOOTMENU_PATH_MAX + 1];  /* NO_FILE: the file ("/boot/..."); else "" */
    uint32_t entries;                      /* OK: the entries that boot (not directories) */
};

/* Is there a file at path ("/boot/jamos.elf": the ESP's root is "/")? */
typedef bool (*bootmenu_exists_fn)(void *ctx, const char *path);

/* Check a menu of len bytes (not NUL-terminated; any bytes at all) as
 * the header above says; exists(ctx, path) for each file it names, once
 * the rest has passed. True if it passes; *out always says why (or how
 * many entries). */
bool bootmenu_check(const void *text, size_t len, bootmenu_exists_fn exists, void *ctx,
                    struct bootmenu_result *out);
/* why in words ("a file it names isn't on the stick"). */
const char *bootmenu_why_str(uint32_t why);
