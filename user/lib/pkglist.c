/* A package program's list (<wants.h>) as the ports formats carry it
 * (<pkgfmt.h>): the manifest's encoded field, what a package may not ask
 * for, and the note inside the program's ELF file that must say the same.
 * Pure code: every offset of the ELF file is checked against its size. */
#include "pkgint.h"

status_t pkg_list_decode(struct pkg_str field, uint8_t out[PKG_LIST_MAX], size_t *n)
{
    size_t k = 0;
    for (uint32_t i = 0; i < field.n;) {
        uint8_t c = (uint8_t)field.p[i];
        if (k == PKG_LIST_MAX)
            return ERR_INVALID_ARGS;
        if (c == '\\') {
            if (field.n - i < 4 || memcmp(field.p + i, "\\x0a", 4))
                return ERR_INVALID_ARGS;   /* the one escape there is */
            out[k++] = '\n';
            i += 4;
        } else {
            out[k++] = c;
            i++;
        }
    }
    if (k == 0 || out[k - 1] != '\n')
        return ERR_INVALID_ARGS;
    size_t line = 0;   /* bytes in the line being read */
    for (size_t i = 0; i < k; i++) {
        if (out[i] == '\n') {
            if (!line)
                return ERR_INVALID_ARGS;   /* no empty line */
            line = 0;
        } else if (out[i] < 0x20 || out[i] > 0x7e) {
            return ERR_INVALID_ARGS;
        } else {
            line++;
        }
    }
    *n = k;
    return OK;
}

/* The wants no program from /data may be given: dp_wants_refused's
 * (<dataprog.h>), as list lines. */
static const char *const refused_lines[] = {
    "svc devmgr", "svc devmgr-ctl", "svc volumes", "svc init", "svc net-sys", "svc dns-sys",
    "svc serve", "svc session", "svc net listen low", "svc net-low", "right debug",
};

const char *pkg_list_refused(const uint8_t *list, size_t n)
{
    bool posix = false;
    size_t at = 0;
    while (at < n) {
        size_t e = at;
        while (e < n && list[e] != '\n')
            e++;
        struct pkg_str line = { (const char *)list + at, (uint32_t)(e - at) };
        for (unsigned i = 0; i < sizeof(refused_lines) / sizeof(refused_lines[0]); i++)
            if (pkg_str_eq(line, refused_lines[i]))
                return refused_lines[i];
        posix = posix || pkg_str_eq(line, "posix");
        at = e + 1;
    }
    return posix ? NULL : "no 'posix' line (a ported program is a POSIX one)";
}

/* ---- the ELF note ------------------------------------------------------------------------ */

#define PT_NOTE_TYPE 4u
#define NOTES_MAX    (64u << 10)   /* bytes of notes read in all: <dataprog.h> DP_NOTES_MAX */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32;
}

/* The JamOS type-1 notes in the segment [off, off + len) of elf: how many
 * say exactly the list (*same), and how many there are (the return); or
 * -1 for a note that does not fit its segment. */
static int notes_in(const uint8_t *elf, uint64_t off, uint64_t len, const uint8_t *list,
                    size_t n, unsigned *same)
{
    int found = 0;
    uint64_t at = 0;
    while (len - at >= 12) {
        uint32_t namesz = rd32(elf + off + at), descsz = rd32(elf + off + at + 4);
        uint32_t type = rd32(elf + off + at + 8);
        uint64_t name_pad = ((uint64_t)namesz + 3) & ~(uint64_t)3;
        uint64_t desc_pad = ((uint64_t)descsz + 3) & ~(uint64_t)3;
        if (name_pad > len - at - 12 || desc_pad > len - at - 12 - name_pad)
            return -1;
        const uint8_t *name = elf + off + at + 12, *desc = name + name_pad;
        if (type == 1 && namesz == 6 && !memcmp(name, "JamOS", 6)) {
            if (descsz == 0 || desc[descsz - 1] != 0)
                return -1;
            found++;
            if (descsz - 1 == n && !memcmp(desc, list, n))
                (*same)++;
        }
        at += 12 + name_pad + desc_pad;
    }
    return found;
}

bool pkg_elf_list_is(const uint8_t *elf, uint64_t size, const uint8_t *list, size_t n)
{
    if (size < 64 || memcmp(elf, "\x7f" "ELF", 4) || elf[4] != 2 || elf[5] != 1)
        return false;
    uint64_t phoff = rd64(elf + 32);
    uint32_t entsize = rd16(elf + 54), num = rd16(elf + 56);
    if (num == 0 || entsize != 56 || phoff > size || (uint64_t)entsize * num > size - phoff)
        return false;
    unsigned same = 0;
    int found = 0;
    uint64_t notes = 0;
    for (uint32_t i = 0; i < num; i++) {
        const uint8_t *ph = elf + phoff + (uint64_t)i * entsize;
        if (rd32(ph) != PT_NOTE_TYPE)
            continue;
        uint64_t off = rd64(ph + 8), filesz = rd64(ph + 32);
        if (off > size || filesz > size - off || filesz > NOTES_MAX - notes)
            return false;
        notes += filesz;
        int f = notes_in(elf, off, filesz, list, n, &same);
        if (f < 0)
            return false;
        found += f;
    }
    return found == 1 && same == 1;
}
