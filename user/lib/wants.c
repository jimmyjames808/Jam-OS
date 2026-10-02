/* A program's list (<wants.h>): found in its ELF file's PT_NOTE segments
 * and parsed into grants (<os.h> "grants"), root powers and a line for
 * the owner to read.
 *
 * The file is untrusted (a program on /data): every offset and size is
 * checked against the file before it is read, and the list must be
 * printable ASCII lines that each say one known thing, or the whole file
 * is refused. The bootfs programs' lists were checked when they were
 * built (tools/checkwants.py), by the same rules. */
#include <wants.h>

#define PT_NOTE      4
#define MAX_PHDRS    64
#define NOTE_HDR     12u

/* The ELF header fields read here (spawn.c reads the rest). */
struct ehdr {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum;
    uint16_t shentsize, shnum, shstrndx;
};

struct phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr;
    uint64_t filesz, memsz;
    uint64_t align;
};

static uint32_t u32_at(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

/* ---- the text --------------------------------------------------------------------- */

static void add_text(struct wants *w, const char *s)
{
    size_t have = strlen(w->text), n = strlen(s);
    if (have + n + 3 > sizeof(w->text))
        return;   /* shown cut short; the grants are whole */
    if (have) {
        memcpy(w->text + have, ", ", 2);
        have += 2;
    }
    memcpy(w->text + have, s, n + 1);
}

static bool add_grant(struct wants *w, const char *g, const char *shown)
{
    if (w->n == WANTS_MAX)
        return false;
    snprintf(w->grant[w->n++], sizeof(w->grant[0]), "%s", g);
    add_text(w, shown);
    return true;
}

/* "svc <name>". The listening form of net is only ever written `svc net
 * listen` (want_net_listen), so the owner reads it as what it is. */
static bool want_svc(struct wants *w, const char *name)
{
    size_t n = strlen(name);
    if (n < 1 || n > SVC_NAME_MAX || !strcmp(name, SVC_NET_LISTEN))
        return false;
    for (size_t i = 0; i < n; i++)
        if (!((name[i] >= 'a' && name[i] <= 'z') || (name[i] >= '0' && name[i] <= '9') ||
              name[i] == '-'))
            return false;
    char g[24];
    snprintf(g, sizeof(g), "/svc/%s", name);
    return add_grant(w, g, name);
}

/* "svc net listen": /svc/net, and /svc/net-listen, whose openers may also
 * take the ports servers are known by (and, with TCP, accept connections). */
static bool want_net_listen(struct wants *w)
{
    return add_grant(w, "/svc/" SVC_NET, SVC_NET) &&
           add_grant(w, "/svc/" SVC_NET_LISTEN, "accepting connections from the network");
}

/* "mount <point> r|rw". */
static bool want_mount(struct wants *w, const char *point, const char *mode)
{
    static const char *const points[] = { "/boot", "/esp", "/data", "/usb*", "*" };
    bool known = false;
    for (unsigned i = 0; i < sizeof(points) / sizeof(points[0]); i++)
        known |= !strcmp(point, points[i]);
    bool ro = !strcmp(mode, "r");
    if (!known || (!ro && strcmp(mode, "rw")))
        return false;
    char g[24], shown[40];
    snprintf(g, sizeof(g), "%s:%c", point, ro ? 'r' : 'w');
    snprintf(shown, sizeof(shown), "%s (%s)", !strcmp(point, "*") ? "every mount" : point,
             ro ? "read" : "write");
    return add_grant(w, g, shown);
}

/* "right <name>". */
static bool want_right(struct wants *w, const char *name)
{
    static const struct { const char *name; uint32_t bit; } rights[] = {
        { "klog", WANT_RIGHT_KLOG }, { "sysinfo", WANT_RIGHT_SYSINFO },
        { "clock", WANT_RIGHT_CLOCK }, { "debug", WANT_RIGHT_DEBUG },
    };
    for (unsigned i = 0; i < sizeof(rights) / sizeof(rights[0]); i++)
        if (!strcmp(name, rights[i].name)) {
            w->rights |= rights[i].bit;
            char shown[24];
            snprintf(shown, sizeof(shown), "the kernel's %s", name);
            add_text(w, shown);
            return true;
        }
    return false;
}

/* One line, its words split at single spaces (in place). */
static bool want_line(struct wants *w, char *line)
{
    char *word[4];
    unsigned n = 0;
    for (char *p = line; *p && n < 4;) {
        word[n++] = p;
        while (*p && *p != ' ')
            p++;
        if (*p)
            *p++ = '\0';
    }
    if (n == 2 && !strcmp(word[0], "svc"))
        return want_svc(w, word[1]);
    if (n == 3 && !strcmp(word[0], "svc") && !strcmp(word[1], SVC_NET) &&
        !strcmp(word[2], "listen"))
        return want_net_listen(w);
    if (n == 3 && !strcmp(word[0], "mount"))
        return want_mount(w, word[1], word[2]);
    if (n == 2 && !strcmp(word[0], "right"))
        return want_right(w, word[1]);
    return false;
}

status_t wants_parse(const char *text, size_t len, struct wants *out)
{
    memset(out, 0, sizeof(*out));
    out->found = true;
    char copy[WANTS_TEXT_MAX];
    if (len >= sizeof(copy))
        return ERR_INVALID_ARGS;
    memcpy(copy, text, len);
    copy[len] = '\0';
    for (size_t i = 0; i < len; i++)
        if (copy[i] != '\n' && (copy[i] < 0x20 || copy[i] > 0x7e))
            return ERR_INVALID_ARGS;
    for (char *line = copy; *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        if (*line && !want_line(out, line)) {
            memset(out, 0, sizeof(*out));
            return ERR_INVALID_ARGS;
        }
        if (!nl)
            break;
        line = nl + 1;
    }
    return OK;
}

/* ---- the file ---------------------------------------------------------------------- */

/* The notes in [off, off + len) of the file: a JamOS list, parsed. */
static status_t notes(const uint8_t *elf, uint64_t off, uint64_t len, struct wants *out)
{
    uint64_t at = 0;
    while (len - at >= NOTE_HDR) {
        const uint8_t *h = elf + off + at;
        uint64_t namesz = u32_at(h), descsz = u32_at(h + 4), type = u32_at(h + 8);
        uint64_t name_pad = (namesz + 3) & ~3ull, desc_pad = (descsz + 3) & ~3ull;
        if (name_pad > len - at - NOTE_HDR || desc_pad > len - at - NOTE_HDR - name_pad)
            return ERR_INVALID_ARGS;
        const char *name = (const char *)h + NOTE_HDR, *desc = name + name_pad;
        bool ours = namesz == sizeof(WANTS_NOTE_NAME) && type == WANTS_NOTE_TYPE &&
                    !memcmp(name, WANTS_NOTE_NAME, sizeof(WANTS_NOTE_NAME));
        if (ours) {
            if (out->found || !descsz || desc[descsz - 1] != '\0')
                return ERR_INVALID_ARGS;   /* two lists, or text without its NUL */
            status_t st = wants_parse(desc, descsz - 1, out);
            if (st != OK)
                return st;
        }
        at += NOTE_HDR + name_pad + desc_pad;
    }
    return OK;
}

status_t wants_read(const uint8_t *elf, uint64_t size, struct wants *out)
{
    memset(out, 0, sizeof(*out));
    struct ehdr eh;
    if (size < sizeof(eh))
        return ERR_INVALID_ARGS;
    memcpy(&eh, elf, sizeof(eh));
    if (memcmp(eh.ident, "\x7f" "ELF", 4) || eh.ident[4] != 2 ||
        eh.phentsize != sizeof(struct phdr) || eh.phnum > MAX_PHDRS || eh.phoff > size ||
        eh.phnum * sizeof(struct phdr) > size - eh.phoff)
        return ERR_INVALID_ARGS;
    for (unsigned i = 0; i < eh.phnum; i++) {
        struct phdr ph;
        memcpy(&ph, elf + eh.phoff + i * sizeof(ph), sizeof(ph));
        if (ph.type != PT_NOTE)
            continue;
        if (ph.offset > size || ph.filesz > size - ph.offset)
            return ERR_INVALID_ARGS;
        status_t st = notes(elf, ph.offset, ph.filesz, out);
        if (st != OK) {
            memset(out, 0, sizeof(*out));
            return st;
        }
    }
    return OK;
}
