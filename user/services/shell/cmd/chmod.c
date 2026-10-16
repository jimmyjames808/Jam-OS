/* chmod: set a file's permission bits, in octal (chmod 755 x) or by
 * clauses ([ugoa][+-=][rwxXst], comma separated: u+x, go-w, a=r). The bits
 * are what ported programs expect to see and set; they are not what keeps a
 * program out of a file (handles are). A volume that keeps no bits (the
 * stick's FAT) says so, rather than accepting and forgetting. */
#include "sh.h"

/* The bits of class mask `who` (u 4, g 2, o 1 → their places) for r, w, x. */
static uint32_t class_bits(unsigned who, unsigned rwx)
{
    uint32_t m = 0;
    if (who & 4)
        m |= (uint32_t)rwx << 6;
    if (who & 2)
        m |= (uint32_t)rwx << 3;
    if (who & 1)
        m |= rwx;
    return m;
}

/* spec applied to mode (permission bits and setuid, setgid, sticky). */
static bool apply(const char *spec, uint32_t mode, bool dir, uint32_t *out)
{
    mode &= 07777u;
    if (!*spec)
        return false;
    for (const char *p = spec; *p;) {
        unsigned who = 0;
        for (; *p == 'u' || *p == 'g' || *p == 'o' || *p == 'a'; p++)
            who |= *p == 'u' ? 4u : *p == 'g' ? 2u : *p == 'o' ? 1u : 7u;
        if (!who)
            who = 7;
        if (*p != '+' && *p != '-' && *p != '=')
            return false;
        for (; *p == '+' || *p == '-' || *p == '=';) {
            char op = *p++;
            unsigned rwx = 0;
            uint32_t extra = 0;
            for (; *p && *p != ',' && *p != '+' && *p != '-' && *p != '='; p++) {
                switch (*p) {
                case 'r': rwx |= 4; break;
                case 'w': rwx |= 2; break;
                case 'x': rwx |= 1; break;
                case 'X': rwx |= (dir || (mode & 0111)) ? 1u : 0u; break;
                case 's': extra |= ((who & 4) ? 04000u : 0) | ((who & 2) ? 02000u : 0); break;
                case 't': extra |= 01000u; break;
                default: return false;
                }
            }
            uint32_t bits = class_bits(who, rwx) | extra;
            if (op == '+')
                mode |= bits;
            else if (op == '-')
                mode &= ~bits;
            else
                mode = (mode & ~class_bits(who, 7)) | bits;
        }
        if (*p == ',')
            p++;
        else if (*p)
            return false;
    }
    *out = mode;
    return true;
}

static bool octal(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    size_t n = 0;
    for (; *s; s++, n++) {
        if (*s < '0' || *s > '7' || n >= 4)
            return false;
        v = v * 8 + (uint32_t)(*s - '0');
    }
    *out = v;
    return n > 0;
}

SH_CMD(chmod)
{
    if (argc < 3 || (argv[1][0] == '-' && argv[1][1] && !strchr("ugoa", argv[1][1]))) {
        sh_tty("usage: chmod <mode> <path>...\n");
        return 2;
    }
    const char *spec = argv[1];
    uint32_t fixed;
    bool is_octal = octal(spec, &fixed);
    int status = 0;
    for (int i = 2; i < argc; i++) {
        char abs[SH_PATH_MAX];
        struct fs_attr a;
        uint32_t features = 0, mode = 0;
        status_t st = sh_resolve(argv[i], abs, sizeof(abs)) ? OK : ERR_INVALID_ARGS;
        if (st == OK && fs_statfs_kind(abs, NULL, NULL, &features) == OK &&
            !(features & SH_FEAT_MODES)) {
            sh_tty("chmod: %s: this volume keeps no permission bits (only an ext volume "
                   "such as /jam does)\n", argv[i]);
            status = 1;
            continue;
        }
        if (st == OK)
            st = sh_getattr(abs, 0, &a);
        if (st == OK) {
            bool dir = (a.mode & SH_MODE_FMT) == SH_MODE_DIR;
            if (is_octal)
                mode = fixed;
            else if (!apply(spec, a.mode, dir, &mode)) {
                sh_tty("chmod: %s: not a mode (octal like 755, or u+x, go-w, a=r)\n", spec);
                return 2;
            }
            struct fs_attr_set set = { .mask = SH_ATTR_MODE, .mode = mode };
            st = fs_setattr(abs, 0, &set);
        }
        if (st != OK) {
            sh_tty("chmod: %s: %s\n", argv[i], sh_why(st));
            status = 1;
        }
    }
    return status;
}
