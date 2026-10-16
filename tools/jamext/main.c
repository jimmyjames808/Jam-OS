/* jamext: Jam OS's ext volumes on the Mac, from the same library the extfs
 * service runs (docs/M13.6-PLAN.md, "Ext by default"). A volume is an image
 * file, or part of one (--offset/--size, or --part N for a partition of a
 * disk image or a device), so the stick's /data gets its key and test files
 * before the first boot and nothing needs e2fsprogs:
 *
 *   jamext mkfs [options] <image> <size>          make a volume
 *   jamext mkimage [options] <image> <size|auto> <dir>
 *                                                 make a volume holding a host directory
 *   jamext import <image> <dir> [<path>]          add a host directory
 *   jamext put [-m mode] <image> <file|-> <path>  a host file as a file (parents made)
 *   jamext get <image> <path> <file|->            a file out to the host
 *   jamext ls [-l] [-R] <image> [<path>]
 *   jamext ln [-s] <image> <target> <path>        a hard link, or a symbolic link
 *   jamext mkdir [-p] [-m mode] <image> <path>
 *   jamext rm [-r] <image> <path>
 *   jamext chmod <image> <mode> <path>
 *   jamext info <image>
 *   jamext check [--replay] <image>               read-only check (ext_check.h): exit 0 clean
 *
 * Options: --offset N --size N --part N (where the volume is); --time T
 * (seconds since 1970 for every time written; SOURCE_DATE_EPOCH too) and
 * --seed S (the volume's uuid and its inodes' generations, so an image can
 * be made again byte for byte); --label, --block-size, --inode-ratio,
 * --inodes, --journal-blocks (mkfs); --keep-modes, --keep-times (import);
 * -q. Numbers take K, M, G, T. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "jamext.h"

#define MAX_POS 8

struct args {
    struct jx_opts o;
    struct ext_mkfs mk;
    const char *pos[MAX_POS];
    int npos;
    bool l, R, s, p, r;
    uint16_t mode;
    bool have_mode;
};

static void usage(void)
{
    fputs("usage: jamext [options] <command> ...\n"
          "  mkfs [--label L] [--block-size N] [--inode-ratio N] [--inodes N]\n"
          "       [--journal-blocks N] <image> <size>\n"
          "  mkimage [mkfs options] <image> <size|auto> <dir>\n"
          "  import [--keep-modes] [--keep-times] <image> <dir> [<path>]\n"
          "  put [-m mode] <image> <file|-> <path>     get <image> <path> <file|->\n"
          "  ls [-l] [-R] <image> [<path>]             ln [-s] <image> <target> <path>\n"
          "  mkdir [-p] [-m mode] <image> <path>       rm [-r] <image> <path>\n"
          "  chmod <image> <mode> <path>               info <image>\n"
          "  check [--replay] <image>\n"
          "a volume in a bigger file: --offset N --size N, or --part N (GPT or MBR)\n"
          "--time T and --seed S make an image again byte for byte; -q is quiet\n",
          stderr);
}

static bool number(const char *s, uint64_t *out, int base)
{
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, base);
    if (errno || end == s)
        return false;
    uint64_t mult = 1;
    if (base == 10 && *end) {
        switch (*end) {
        case 'k': case 'K': mult = 1ull << 10; break;
        case 'm': case 'M': mult = 1ull << 20; break;
        case 'g': case 'G': mult = 1ull << 30; break;
        case 't': case 'T': mult = 1ull << 40; break;
        default: return false;
        }
        end++;
        if (*end == 'i' || *end == 'I')
            end++;
        if (*end == 'b' || *end == 'B')
            end++;
    }
    if (*end)
        return false;
    *out = v * mult;
    return true;
}

/* A long option's value: after '=' or the next argument. */
static const char *value(int argc, char **argv, int *i, const char *eq)
{
    if (eq)
        return eq;
    if (*i + 1 >= argc) {
        fprintf(stderr, "jamext: %s needs a value\n", argv[*i]);
        return NULL;
    }
    return argv[++*i];
}

static bool parse(int argc, char **argv, struct args *a)
{
    memset(a, 0, sizeof(*a));
    a->o.now = time(NULL);
    const char *epoch = getenv("SOURCE_DATE_EPOCH");
    uint64_t v;
    if (epoch && number(epoch, &v, 10))
        a->o.now = (int64_t)v;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strncmp(arg, "--", 2) && arg[2]) {
            char name[32];
            const char *eq = strchr(arg + 2, '=');
            size_t nl = eq ? (size_t)(eq - arg - 2) : strlen(arg + 2);
            if (nl >= sizeof(name)) {
                fprintf(stderr, "jamext: unknown option %s\n", arg);
                return false;
            }
            memcpy(name, arg + 2, nl);
            name[nl] = '\0';
            if (eq)
                eq++;
            if (!strcmp(name, "keep-modes")) { a->o.keep_modes = true; continue; }
            if (!strcmp(name, "keep-times")) { a->o.keep_times = true; continue; }
            if (!strcmp(name, "replay"))     { a->o.replay = true; continue; }
            if (!strcmp(name, "quiet"))      { a->o.quiet = true; continue; }
            const char *val = value(argc, argv, &i, eq);
            if (!val)
                return false;
            bool ok = true;
            if (!strcmp(name, "offset"))
                ok = number(val, &a->o.offset, 10);
            else if (!strcmp(name, "size"))
                ok = number(val, &a->o.size, 10);
            else if (!strcmp(name, "part")) {
                ok = number(val, &v, 10) && v >= 1 && v <= 128;
                a->o.part = (int)v;
            } else if (!strcmp(name, "time")) {
                ok = number(val, &v, 10);
                a->o.now = (int64_t)v;
            } else if (!strcmp(name, "seed"))
                ok = number(val, &a->o.seed, 10);
            else if (!strcmp(name, "block-size")) {
                ok = number(val, &v, 10);
                a->mk.block_size = (uint32_t)v;
            } else if (!strcmp(name, "inode-ratio")) {
                ok = number(val, &v, 10);
                a->mk.inode_ratio = (uint32_t)v;
            } else if (!strcmp(name, "inodes")) {
                ok = number(val, &v, 10);
                a->mk.inodes = (uint32_t)v;
            } else if (!strcmp(name, "journal-blocks")) {
                ok = number(val, &v, 10);
                a->mk.journal_blocks = (uint32_t)v;
            } else if (!strcmp(name, "label")) {
                if (strlen(val) > 15) {
                    fprintf(stderr, "jamext: the label is at most 15 bytes\n");
                    return false;
                }
                strncpy(a->mk.label, val, sizeof(a->mk.label) - 1);
            } else {
                fprintf(stderr, "jamext: unknown option %s\n", arg);
                return false;
            }
            if (!ok) {
                fprintf(stderr, "jamext: bad value for --%s: %s\n", name, val);
                return false;
            }
            continue;
        }
        if (arg[0] == '-' && arg[1] && strcmp(arg, "-")) {
            for (const char *c = arg + 1; *c; c++) {
                switch (*c) {
                case 'l': a->l = true; break;
                case 'R': a->R = true; break;
                case 's': a->s = true; break;
                case 'p': a->p = true; break;
                case 'r': a->r = true; break;
                case 'q': a->o.quiet = true; break;
                case 'm':
                    if (c[1] || i + 1 >= argc || !number(argv[i + 1], &v, 8) || v > 07777) {
                        fprintf(stderr, "jamext: -m takes an octal mode\n");
                        return false;
                    }
                    a->mode = (uint16_t)v;
                    a->have_mode = true;
                    i++;
                    break;
                default:
                    fprintf(stderr, "jamext: unknown option -%c\n", *c);
                    return false;
                }
            }
            continue;
        }
        if (a->npos == MAX_POS) {
            fprintf(stderr, "jamext: too many arguments\n");
            return false;
        }
        a->pos[a->npos++] = arg;
    }
    return true;
}

static int failed(const char *what, status_t st)
{
    jx_fail(what, st);
    return 1;
}

/* ---- the commands ------------------------------------------------------------------------ */

static int cmd_mkfs(struct args *a, bool image_dir)
{
    int want = image_dir ? 3 : 2;
    bool size_option = !image_dir && a->npos == 2 && a->o.size;   /* mkfs --size N <image> */
    if (a->npos != want + 1 && !size_option) {
        usage();
        return 2;
    }
    a->o.image = a->pos[1];
    uint64_t size = 0;
    uint32_t inodes = 0;
    if (size_option) {
        size = a->o.size;
    } else if (image_dir && !strcmp(a->pos[2], "auto")) {
        if (!jx_size_for(a->pos[3], &size, &inodes))
            return 1;
        if (a->mk.inodes < inodes)
            a->mk.inodes = inodes;
    } else if (!number(a->pos[2], &size, 10) || !size) {
        fprintf(stderr, "jamext: bad size: %s\n", a->pos[2]);
        return 2;
    }
    if (!a->mk.label[0])
        strcpy(a->mk.label, "jam");
    if (!jx_mkfs(&a->o, size, &a->mk))
        return 1;
    struct jx x;
    if (!jx_open(&x, &a->o, true))
        return 1;
    int rc = 0;
    if (image_dir) {
        uint64_t count = 0;
        status_t st = jx_import(&x, a->pos[3], "/", &count);
        if (st != OK)
            rc = failed(a->pos[3], st);
        else if (!a->o.quiet)
            printf("%s: %llu names from %s\n", a->o.image, (unsigned long long)count, a->pos[3]);
    }
    if (rc == 0 && !a->o.quiet)
        printf("%s: \"%s\", %llu MiB, %u-byte blocks, %llu blocks free\n", a->o.image,
               a->mk.label, (unsigned long long)(x.bytes >> 20), x.fs.block_size,
               (unsigned long long)ext_free_blocks(&x.fs));
    if (!jx_close(&x))
        rc = 1;
    return rc;
}

static void *check_alloc(void *ctx, size_t n)
{
    (void)ctx;
    return calloc(1, n);
}

static void check_release(void *ctx, void *p)
{
    (void)ctx;
    free(p);
}

/* ctx is the image's name. */
static void check_report(void *ctx, const char *fmt, va_list ap)
{
    printf("%s: ", (const char *)ctx);
    vprintf(fmt, ap);
    printf("\n");
}

static int cmd_check(struct args *a)
{
    if (a->npos != 2) {
        usage();
        return 2;
    }
    a->o.image = a->pos[1];
    struct jx x;
    if (!jx_open(&x, &a->o, a->o.replay))
        return 1;
    struct ext_check_io io = {
        .ctx = (void *)a->o.image, .alloc = check_alloc, .release = check_release,
        .report = check_report,
    };
    struct ext_check_result res;
    status_t st = ext_check(&x.fs, &io, &res);
    int rc = 0;
    if (st != OK) {
        rc = failed("check", st);
    } else if (res.problems) {
        printf("%s: %u problem%s%s\n", a->o.image, res.problems, res.problems == 1 ? "" : "s",
               res.stopped ? " (stopped there)" : "");
        rc = 1;
    } else if (!a->o.quiet) {
        printf("%s: clean: %u inodes in use, %u directories, %u names, %llu of %llu blocks free%s\n",
               a->o.image, res.inodes, res.dirs, res.names, (unsigned long long)res.free_blocks,
               (unsigned long long)x.fs.blocks, res.exact ? "" : " (blocks not all attributed)");
    }
    if (!jx_close(&x) && rc == 0)
        rc = 1;
    return rc;
}

static int cmd_info(struct args *a)
{
    if (a->npos != 2) {
        usage();
        return 2;
    }
    a->o.image = a->pos[1];
    struct jx x;
    if (!jx_open(&x, &a->o, false))
        return 1;
    const struct ext_fs *fs = &x.fs;
    char label[17] = "";
    memcpy(label, fs->sb.volume_name, 16);
    printf("label: %s\nuuid: ", label);
    for (int i = 0; i < 16; i++)
        printf("%02x%s", fs->sb.uuid[i], i == 3 || i == 5 || i == 7 || i == 9 ? "-" : "");
    printf("\nblock size: %u\nblocks: %llu (%llu free)\ninodes: %u (%u free)\n"
           "journal: %s%s\nread-only: %s\n",
           fs->block_size, (unsigned long long)fs->blocks, (unsigned long long)ext_free_blocks(fs),
           fs->inodes, fs->sb.free_inodes_count, fs->has_journal ? "yes" : "no",
           fs->needs_recovery ? ", needs recovery" : "", fs->ro ? "yes" : "no");
    return jx_close(&x) ? 0 : 1;
}

/* A command on an open volume: write says whether it changes it. */
static int with_volume(struct args *a, bool write, status_t (*run)(struct jx *, struct args *))
{
    struct jx x;
    if (!jx_open(&x, &a->o, write))
        return 1;
    status_t st = run(&x, a);
    int rc = 0;
    if (st != OK)
        rc = failed(a->pos[0], st);
    if (!jx_close(&x) && rc == 0)
        rc = 1;
    return rc;
}

static status_t run_import(struct jx *x, struct args *a)
{
    uint64_t count = 0;
    status_t st = jx_import(x, a->pos[2], a->npos > 3 ? a->pos[3] : "/", &count);
    if (st == OK && !a->o.quiet)
        printf("%llu names\n", (unsigned long long)count);
    return st;
}

static status_t run_put(struct jx *x, struct args *a)
{
    return jx_put(x, a->pos[2], a->pos[3], a->have_mode ? a->mode : 0644);
}

static status_t run_get(struct jx *x, struct args *a)
{
    return jx_get(x, a->pos[2], a->pos[3]);
}

static status_t run_ls(struct jx *x, struct args *a)
{
    return jx_ls(x, a->npos > 2 ? a->pos[2] : "/", a->l, a->R);
}

static status_t run_ln(struct jx *x, struct args *a)
{
    return jx_ln(x, a->pos[2], a->pos[3], a->s);
}

static status_t run_mkdir(struct jx *x, struct args *a)
{
    return jx_mkdir(x, a->pos[2], a->have_mode ? a->mode : 0755, a->p);
}

static status_t run_rm(struct jx *x, struct args *a)
{
    return jx_rm(x, a->pos[2], a->r);
}

static status_t run_chmod(struct jx *x, struct args *a)
{
    uint64_t m;
    if (!number(a->pos[2], &m, 8) || m > 07777) {
        fprintf(stderr, "jamext: chmod takes an octal mode\n");
        return ERR_INVALID_ARGS;
    }
    return jx_chmod(x, a->pos[3], (uint16_t)m);
}

int main(int argc, char **argv)
{
    struct args a;
    if (argc < 2 || !parse(argc, argv, &a) || a.npos < 1) {
        usage();
        return 2;
    }
    const char *c = a.pos[0];
    /* The image is the first argument after the command in all but mkfs'
     * family, which names the file it makes. */
    if (!strcmp(c, "mkfs"))
        return cmd_mkfs(&a, false);
    if (!strcmp(c, "mkimage"))
        return cmd_mkfs(&a, true);
    if (a.npos < 2) {
        usage();
        return 2;
    }
    a.o.image = a.pos[1];
    if (!strcmp(c, "check"))
        return cmd_check(&a);
    if (!strcmp(c, "info"))
        return cmd_info(&a);
    struct {
        const char *name;
        int npos;                    /* positional arguments, the command and image included */
        int npos_max;
        bool write;
        status_t (*run)(struct jx *, struct args *);
    } table[] = {
        { "import", 3, 4, true,  run_import },
        { "put",    4, 4, true,  run_put },
        { "get",    4, 4, false, run_get },
        { "ls",     2, 3, false, run_ls },
        { "ln",     4, 4, true,  run_ln },
        { "mkdir",  3, 3, true,  run_mkdir },
        { "rm",     3, 3, true,  run_rm },
        { "chmod",  4, 4, true,  run_chmod },
    };
    for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++)
        if (!strcmp(c, table[i].name)) {
            if (a.npos < table[i].npos || a.npos > table[i].npos_max) {
                usage();
                return 2;
            }
            return with_volume(&a, table[i].write || a.o.replay, table[i].run);
        }
    fprintf(stderr, "jamext: unknown command %s\n", c);
    usage();
    return 2;
}
