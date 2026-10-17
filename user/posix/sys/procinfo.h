/* The text of /proc's generated files (procinfo.c and cpuinfo.c; the
 * names, opens and descriptions are procfs.c's). Docs: CR-PLAN.md, "PRC
 * notes" and "CBF4 notes". */
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
    PF_STAT = 1,   /* /proc/<pid>/stat (and /proc/<pid>/task/<tid>/stat) */
    PF_STATM,      /* /proc/<pid>/statm */
    PF_STATUS,     /* /proc/<pid>/status */
    PF_MAPS,       /* /proc/<pid>/maps */
    PF_CMDLINE,    /* /proc/<pid>/cmdline */
    PF_MEM,        /* /proc/<pid>/mem: not text, procfs.c reads it */
    PF_CPUINFO,    /* /proc/cpuinfo */
    PF_MEMINFO,    /* /proc/meminfo */
    PF_SYSSTAT,    /* /proc/stat */
    PF_VERSION,    /* /proc/version */
    PF_OSRELEASE,  /* /proc/sys/kernel/osrelease */
    PF_OSTYPE,     /* /proc/sys/kernel/ostype */
    PF_KVERSION,   /* /proc/sys/kernel/version */
    PF_HOSTNAME,   /* /proc/sys/kernel/hostname */
};

/* File f's text now, malloc'd (*text, *len; the caller frees). pid names
 * the process of the process files: ours, or one of our children's; tid,
 * if not 0, one of our threads (the file is its own: stat's times and
 * state, status's ids). 0, or -errno (-ESRCH: that child or thread is
 * gone; -ENOMEM). */
long procinfo_text(enum pfile f, int32_t pid, uint32_t tid, char **text, size_t *len);

/* /proc/cpuinfo's text, appended to b. */
void cpuinfo_text(struct sb *b);

/* An address on the main thread's stack (procfs.c's constructor took one
 * before main): the mapping that holds it is [stack]. 0 if none. */
uintptr_t proc_stack_addr(void);

/* The program's argument strings as start.c laid them out: argv's from
 * *arg_start to *arg_end (past the last one's NUL), then the environment's
 * to *env_end. All NULL before the start has run. */
void lx_arg_area(const char **arg_start, const char **arg_end, const char **env_end);

/* What the machine calls itself: uname's fields (misc.c). */
struct utsname;
void lx_uname_fields(struct utsname *u);
