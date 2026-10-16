/* stat: what a name is: its kind, size, mode, owner, links, inode and
 * times. A symbolic link is shown as itself ("File: b -> a"); -L shows the
 * file it leads to. A volume without modes or links (the stick's FAT)
 * shows the ones it makes up, and times of 0 as "-". */
#include "sh.h"

static const char *kind(uint32_t mode)
{
    switch (mode & SH_MODE_FMT) {
    case SH_MODE_REG:  return "regular file";
    case SH_MODE_DIR:  return "directory";
    case SH_MODE_LNK:  return "symbolic link";
    case SH_MODE_CHR:  return "character device";
    case SH_MODE_BLK:  return "block device";
    case SH_MODE_FIFO: return "fifo";
    case SH_MODE_SOCK: return "socket";
    default:           return "unknown kind";
    }
}

static const char *when(int64_t ns, const struct tz *tz, char *buf, size_t cap)
{
    if (ns <= 0)
        return "-";
    time_format(ns / 1000000000, tz, buf, cap, true);
    return buf;
}

static int stat_one(const char *arg, bool follow, const struct tz *tz)
{
    char abs[SH_PATH_MAX];
    struct fs_attr a;
    status_t st = sh_resolve(arg, abs, sizeof(abs)) ? OK : ERR_INVALID_ARGS;
    if (st == OK)
        st = sh_getattr(abs, follow ? 0 : FS_NOFOLLOW, &a);
    if (st != OK) {
        sh_tty("stat: %s: %s\n", arg, sh_why(st));
        return 1;
    }
    static char target[4097];
    char mode[11], oct[5], o[12], g[12], t1[64], t2[64], t3[64];
    sh_mode_str(a.mode, mode);
    for (int k = 0; k < 4; k++)   /* libos's printf has no %o */
        oct[k] = (char)('0' + (a.mode >> (9 - 3 * k) & 7));
    oct[4] = '\0';
    sh_say("  File: %s", arg);
    if ((a.mode & SH_MODE_FMT) == SH_MODE_LNK && !follow &&
        fs_readlink(abs, target, sizeof(target)) == OK)
        sh_say(" -> %s", target);
    sh_say("\n  Size: %llu\tBlocks: %llu\t%s\n", (unsigned long long)a.size,
           (unsigned long long)a.blocks, kind(a.mode));
    sh_say("  Mode: %s (%s)\tUid: %u (%s)\tGid: %u (%s)\n", oct, mode,
           (unsigned)a.uid, sh_owner(a.uid, o, sizeof(o)), (unsigned)a.gid,
           sh_owner(a.gid, g, sizeof(g)));
    sh_say(" Links: %u\tInode: %llu\n", (unsigned)a.nlink, (unsigned long long)a.id);
    sh_say("Access: %s\nModify: %s\nChange: %s\n", when(a.atime_ns, tz, t1, sizeof(t1)),
           when(a.mtime_ns, tz, t2, sizeof(t2)), when(a.ctime_ns, tz, t3, sizeof(t3)));
    return 0;
}

SH_CMD(stat)
{
    int first = 1;
    bool follow = false;
    if (first < argc && !strcmp(argv[first], "-L")) {
        follow = true;
        first++;
    }
    if (first >= argc || (argv[first][0] == '-' && argv[first][1])) {
        sh_tty("usage: stat [-L] <path>...\n");
        return 2;
    }
    struct tz tz;
    sh_local_tz(&tz, "stat");
    int status = 0;
    for (int i = first; i < argc; i++)
        status |= stat_one(argv[i], follow, &tz);
    return status;
}
