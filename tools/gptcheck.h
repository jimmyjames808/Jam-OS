/* gptcheck's own declarations, shared by its two files: gptcheck.c (the
 * disk, the checks every table must pass, the second reading, the cases,
 * main) and gptmutate.c (the mutations, and each header rule broken in
 * turn). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jam/parttable.h>

#define HEAD_MAX  (1u << 20)       /* bytes of each end of an image kept in memory */
#define BUF_MAX   (192u * 1024)    /* the largest buffer the library is given */
#define READS_MAX 1000             /* reads one pt_read may make, at most */

/* One image: its ends in memory (mutations change them), the middle read
 * from the file when asked; past the file's end, zeros. */
struct img {
    char     name[64];
    int      fd;
    uint64_t bytes;          /* the file's size */
    uint8_t *head, *tail;    /* working copies of the first hlen and last bytes - toff */
    uint8_t *head0, *tail0;  /* as made */
    uint64_t hlen, toff;     /* the head's length, the tail's offset in the file */
    uint32_t bs0;            /* the image's own block size */
    /* the disk as the library is told it is: */
    uint32_t bs;
    uint64_t blocks;
    uint32_t fail_at;        /* the read that fails (1: the first), 0: none */
    uint32_t reads;          /* reads made by this pt_read */
    bool     failed;         /* one of them failed (fail_at) */
    uint32_t buf_size;       /* the buffer it was given */
};

/* Where the image's GPT structures are, from the image as made. */
struct geo {
    bool     gpt;
    uint64_t hdr[2];         /* byte offsets of the primary and backup header */
    uint64_t arr[2];         /* and of their entry arrays */
    uint32_t count, size;    /* the arrays' entries and their size */
};

/* What the mutated runs gave, counted. */
struct tally {
    uint64_t runs, gpt, mbr, none, listed, failed;
};

extern uint64_t rng;          /* the seeded generator's state */
extern const char *where;     /* what is being checked, for failures */

void fail(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
uint64_t rnd(void);
uint64_t below(uint64_t n);   /* 0 to n - 1 (0 if n is 0) */
/* Bytes of m's file (zeros past its end); blocks of the disk as told. */
void read_bytes(struct img *m, uint64_t off, uint64_t len, uint8_t *dst);
void read_blocks(struct img *m, uint64_t lba, uint64_t count, uint8_t *dst);
/* pt_read on m with two buffer sizes: every check passed, the same table. */
status_t read_checked(struct img *m, struct pt_table *t);

/* gptmutate.c: where m's GPT is; one run of `ops` mutations, counted;
 * each header rule broken alone on m[0] and m[1] (the Windows-like disks). */
void find_geo(const struct img *m, struct geo *g);
void mutated_run(struct img *m, const struct geo *g, int ops, struct tally *y);
unsigned check_edits(struct img *m, const struct geo *g);
/* gptmake.c: Jam OS's table as pt_jamos_make makes it, read back (the
 * cases' "jamos" images among imgs[0..n) to compare with). */
void check_make(const struct img *imgs, int n);

static inline void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> 8 * i);
}

static inline void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> 8 * i);
}

static inline uint64_t get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static inline uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}
