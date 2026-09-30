/* sysmon: what the readings (sample.c), the figures made from them
 * (model.c), the layout (layout.c), the picture (draw.c), the loop
 * (main.c) and the self-test (selftest.c) share. */
#pragma once
#include <fun.h>

#define MAX_CPUS  64
#define MAX_PROCS 512
#define HIST      120    /* readings of history kept: a minute at two a second */
#define TOP_MAX   16     /* process rows at most */
#define NCARDS    4
#define NGROUPS   3      /* CPU tiles come in groups: P-cores, E-cores, the rest */

/* ---- readings (sample.c) ---- */

/* One reading of the kernel's figures: sys_info, cpu_stat and proc_list
 * as they came (<jam/abi.h>). */
struct sample {
    uint64_t         t;                 /* when it was taken (uptime ns) */
    struct sys_info  si;
    struct cpu_stat  cpu[MAX_CPUS];
    uint32_t         ncpu;              /* filled */
    struct proc_stat proc[MAX_PROCS];
    uint32_t         nproc;             /* filled */
};

/* A reading through `root`, which must be the root resource with
 * RIGHT_READ: the error of the first call that fails otherwise. */
status_t sample_take(handle_t root, struct sample *s);
/* A made-up machine for tests and screenshots (`fake=N`): N CPUs (from 8
 * up a hybrid part, 4/7 of them P threads, as the PC's 16 + 12), their
 * loads wandering, a dozen processes. Each call moves *s on to time t;
 * the first call (s->t == 0) sets it up. */
void     sample_fake(struct sample *s, uint32_t ncpu, uint64_t t, uint64_t *rng);

/* ---- figures (model.c) ---- */

/* The last HIST values of something, a ring. */
struct history {
    uint32_t v[HIST];   /* the values */
    int      n;         /* how many are valid (up to HIST) */
    int      at;        /* where the next one goes */
};
void hist_push(struct history *h, uint32_t v);
/* The value k readings back: 0 the newest (k < h->n). */
static inline uint32_t hist_back(const struct history *h, int k)
{
    return h->v[(h->at + HIST - 1 - k) % HIST];
}
uint32_t hist_max(const struct history *h);

struct cpu_view {
    uint32_t       index;   /* the CPU's number */
    uint32_t       type;    /* CPU_TYPE_* */
    uint32_t       pm;      /* busy in the last interval, 0..1000 */
    struct history h;       /* ... and before */
};

struct proc_view {
    char     name[32];
    uint64_t koid;          /* the process id shown */
    uint64_t cpu_ns;        /* CPU time ever */
    uint64_t mem;           /* bytes charged to its job */
    uint32_t pm;            /* CPU use in the last interval, 1000 = one CPU (can pass it) */
    uint32_t threads;
};

struct model {
    uint32_t         ncpu;
    struct cpu_view  cpu[MAX_CPUS];   /* in tile order: the P threads, the E threads, the rest */
    uint32_t         group[NGROUPS];  /* how many tiles each of those groups has */
    uint32_t         total_pm;        /* all CPUs together, 0..1000 */
    struct history   total;
    uint64_t         switches_s;      /* context switches per second, all CPUs */
    struct history   switches;
    uint64_t         mem_total, mem_used;   /* bytes */
    uint64_t         uptime_ns;
    uint32_t         nproc, nthreads;
    struct proc_view top[TOP_MAX];    /* the busiest first; ties: the most CPU time ever */
    int              ntop;
    char             brand[48], version[32];
};

/* The tiles' order and the fixed facts, from a first reading. */
void model_init(struct model *m, const struct sample *s);
/* The figures for the interval from reading a to reading b. */
void model_update(struct model *m, const struct sample *a, const struct sample *b);
/* "1.5 GiB"; "12,345", "123 k" or "1.23 M" (a count that must stay
 * short); "1:02:03" or "2 d 03:04"; "m:ss.hh". */
char *fmt_bytes(char *buf, size_t cap, uint64_t bytes);
char *fmt_count(char *buf, size_t cap, uint64_t n);
char *fmt_uptime(char *buf, size_t cap, uint64_t ns);
char *fmt_cpu_time(char *buf, size_t cap, uint64_t ns);

/* ---- layout (layout.c) ---- */

struct layout {
    int         u;                  /* the UI scale */
    struct rect title;              /* the top line */
    struct rect card[NCARDS];       /* CPU, memory, switches, uptime */
    struct rect label[NGROUPS];     /* a group's heading; w == 0: none */
    struct rect cpu[MAX_CPUS];      /* the tiles, in the model's order */
    struct rect table;              /* the processes */
    int         rows, row_h;        /* process rows that fit; a row's height */
};
/* Everything placed on a w x h screen at UI scale ui for these groups of
 * CPUs. Nothing overlaps or leaves the screen, and at least five process
 * rows fit, from 1280x800 up and for up to MAX_CPUS CPUs. */
void layout_make(struct layout *l, int w, int h, int ui, const uint32_t group[NGROUPS]);

/* ---- the picture (draw.c) ---- */

/* The background for this model's CPUs on this screen; false if out of memory. */
bool draw_setup(const struct model *m);
/* The frame, into scr.s. */
void draw(const struct model *m);

/* selftest.c: `run sysmon --selftest`. */
int sysmon_selftest(void);
