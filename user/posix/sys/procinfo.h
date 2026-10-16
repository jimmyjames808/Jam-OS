/* The text of /proc's generated files (procinfo.c and cpuinfo.c; the
 * names, opens and descriptions are procfs.c's). Docs: CR-PLAN.md, "PRC
 * notes". */
#pragma once

#include "layer.h"

/* A growing text. */
struct sb {
    char  *p;
    size_t len, cap;
    bool   bad;   /* no memory: the text is not whole */
};
void sb_put(struct sb *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

enum pfile {
    PF_STAT = 1,   /* /proc/<pid>/stat */
    PF_STATM,      /* /proc/<pid>/statm */
    PF_STATUS,     /* /proc/<pid>/status */
    PF_MAPS,       /* /proc/<pid>/maps */
    PF_CPUINFO,    /* /proc/cpuinfo */
    PF_MEMINFO,    /* /proc/meminfo */
    PF_SYSSTAT,    /* /proc/stat */
};

/* File f's text now, malloc'd (*text, *len; the caller frees). pid names
 * the process of the first four: ours, or one of our children's. 0, or
 * -errno (-ESRCH: that child is gone; -ENOMEM). */
long procinfo_text(enum pfile f, int32_t pid, char **text, size_t *len);

/* /proc/cpuinfo's text, appended to b. */
void cpuinfo_text(struct sb *b);
