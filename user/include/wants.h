/* A program's list: what it wants to be given when it runs (the services
 * and mounts of its namespace, and powers on the root resource), carried
 * in its own ELF file (docs/history/M8.6-SVC.md, "The program's list").
 *
 * A program declares it once, in any one of its source files:
 *
 *     JAM_WANTS("svc music\n"
 *               "mount /data r\n");
 *
 * One want per line:
 *     svc <name>              the service /svc/<name> (<os.h> SVC_*)
 *     svc net listen          /svc/net, and the permission to listen:
 *                             /svc/net-listen, whose openers may take a
 *                             fixed port below NET_PORT_EPHEMERAL (and,
 *                             with TCP, accept connections); never
 *                             written `svc net-listen`
 *     mount <point> r|rw      a mount, read-only or writable (writable
 *                             leaves the top-level `etc` alone); <point>
 *                             is /boot, /esp, /data, /usb* (every other
 *                             stick) or * (every mount)
 *     right <name>            klog, sysinfo, clock or debug: that power on
 *                             the root resource (WANT_RIGHT_*)
 * The macro puts the text in an ELF note (name "JamOS", type 1) in the
 * section .jamos.wants, which user/linker.ld keeps in the read-only
 * segment under a PT_NOTE program header. tools/checkwants.py checks every
 * program's list at build time. A program with no list wants nothing: it
 * gets its terminal only.
 *
 * Who reads it: the shell, before it starts a program (a boot-image
 * program gets its list as it stands; one on /data only once the owner
 * allowed it), and the shell's `allow`, which shows it. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <os.h>

#define WANTS_NOTE_NAME "JamOS"
#define WANTS_NOTE_TYPE 1u
#define WANTS_TEXT_MAX  1024   /* bytes of a list's text, its NUL included */
#define WANTS_MAX       24     /* wants in one list */

/* The note: an ELF note header, the name padded to 8, the text padded to 4. */
#define JAM_WANTS(text)                                                                   \
    __attribute__((section(".jamos.wants"), used, aligned(4))) static const struct {     \
        uint32_t namesz, descsz, type;                                                    \
        char     name[8];                                                                 \
        char     desc[(sizeof(text) + 3) & ~(size_t)3];                                   \
    } jam_wants_note = { sizeof(WANTS_NOTE_NAME), sizeof(text), WANTS_NOTE_TYPE,         \
                         WANTS_NOTE_NAME, text }

/* The powers `right <name>` asks for. */
#define WANT_RIGHT_KLOG    (1u << 0)   /* read the kernel log */
#define WANT_RIGHT_SYSINFO (1u << 1)   /* the process list, CPU and memory figures */
#define WANT_RIGHT_CLOCK   (1u << 2)   /* the real-time clock */
#define WANT_RIGHT_DEBUG   (1u << 3)   /* the kernel's debug commands (tests) */

struct wants {
    bool     found;                     /* the file has a list */
    unsigned n;                         /* entries of grant */
    char     grant[WANTS_MAX][24];      /* <os.h> "grants" strings: "/svc/music", "/data:r" */
    uint32_t rights;                    /* WANT_RIGHT_* */
    char     text[WANTS_TEXT_MAX];      /* the list as the owner reads it: "music, /data (read)" */
};

/* The list in the ELF file of `size` bytes at elf (read-only, any
 * alignment), into *out. A file without one is OK with out->found false
 * and nothing in it. ERR_INVALID_ARGS: not an ELF file the loader takes,
 * or a list that breaks the rules above (a note too big, a line it
 * doesn't know, too many wants). */
status_t wants_read(const uint8_t *elf, uint64_t size, struct wants *out);
/* The same for a list's text alone (what JAM_WANTS was given). */
status_t wants_parse(const char *text, size_t len, struct wants *out);
