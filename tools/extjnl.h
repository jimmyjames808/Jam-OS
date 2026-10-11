/* extcheck's journal tests (tools/extjnl.c) and what they run on
 * (tools/extcut.c): the cut device, a block device that stops after its
 * Nth block written, and the storm, a fixed list of requests that change
 * a fixture's metadata through the journal. tools/extjnl.c's header has
 * the model. */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <ext_jnl.h>
#include "extcheck.h"

/* ---- the cut device ------------------------------------------------------------------- */

#define CUT_UNIT 1024u   /* the device's bytes are tracked in kibibytes */

struct cutdev {
    uint8_t *base;           /* the image as it starts */
    uint8_t *bytes;          /* the device's bytes */
    uint64_t size;
    uint32_t *touched;       /* kibibytes that differ from base: put back by cut_reset */
    uint8_t  *mark;          /* one byte per kibibyte: listed in touched */
    uint64_t ntouched;
    uint32_t block_size;     /* a write's blocks are counted in these */
    uint64_t units;          /* blocks written since the reset */
    uint64_t cut_at;         /* 0: never cut; else the device dies after this many blocks */
    bool     reorder;        /* a torn write lands any of its blocks, not only the first */
    bool     dead;           /* cut: every write since failed */
    bool     watch;          /* record writes into the journal's log */
    uint64_t jstart, jlen;   /* the journal's blocks (one run): `watch` and the commit count */
    uint64_t bad_writes;     /* writes `watch` saw land where they must not */
    uint64_t commits_alone;  /* writes into the journal that were one commit block alone */
    uint64_t commits_with;   /* ... and that held a commit block and other blocks */
    bool     die_on_commit;  /* the device dies after the first commit block lands */
};

/* A device over a copy of m's bytes; m is not changed. */
void cut_init(struct cutdev *cd, const struct img *m);
void cut_free(struct cutdev *cd);
/* The bytes back to base, the counts to 0, no cut. */
void cut_reset(struct cutdev *cd);
/* Make the base what the device holds now (cut_reset then returns here),
 * the counts 0. */
void cut_rebase(struct cutdev *cd);
/* Change n bytes at off (cut_reset puts them back). */
void cut_poke(struct cutdev *cd, uint64_t off, const void *p, size_t n);
struct ext_dev cut_dev(struct cutdev *cd);

/* ---- the storm ------------------------------------------------------------------------- */

#define STORM_OBJS   64
#define STORM_REQS   128
#define STORM_OPS    64
#define STORM_SNAPS  512

/* Something the storm changes and the checks read back. */
enum obj_kind {
    OBJ_INODE,           /* a file's inode: its mtime and mode */
    OBJ_SWAP,            /* two names in one directory: which inode the first names */
    OBJ_BLOCK,           /* a block of /holes written as metadata (journaled) or as data */
    OBJ_WTIME,           /* the superblock's s_wtime: the request's number */
    OBJ_FILE,            /* a file's size, blocks and bytes (U2a's writers change it) */
    OBJ_LISTING,         /* the whole volume's listing (the names storm) */
};

struct obj {
    uint8_t  kind;            /* enum obj_kind */
    uint32_t ino;             /* OBJ_INODE: the inode; OBJ_SWAP: the directory */
    char     na[64], nb[64];  /* OBJ_SWAP: the two names */
    uint64_t blk;             /* OBJ_BLOCK */
};

enum op_kind {
    OP_TOUCH,            /* an inode's mtime and ctime := value */
    OP_CHMOD,            /* its permission bits := value */
    OP_SWAP,             /* the two names exchange their inodes */
    OP_META,             /* a block written whole through the cache with value's pattern */
    OP_FREE,             /* the block freed: ext_jnl_freed */
    OP_DATA,             /* the block written around the cache (file data) with value's pattern */
    OP_STEP,             /* ext_jnl_step(value) */
    OP_APPEND,           /* value's bytes (1 to 3 blocks' worth) written at the file's end */
    OP_CUT,              /* the file truncated to half its size, in whole blocks */
};

struct op {
    uint8_t  kind;            /* enum op_kind */
    uint16_t obj;
    uint64_t value;
};

struct request {
    struct op ops[STORM_OPS];
    uint32_t nops;
};

/* A storm's settings. */
struct storm_cfg {
    const char *name;         /* for failures and the PC's images */
    uint32_t fixture;         /* index into extjnl.c's fixtures */
    uint32_t jflags;          /* ext_jnl_init's flags */
    bool     reorder;         /* torn writes land any of their blocks */
    uint32_t maxlen;          /* the journal's length (0: as the fixture has it) */
    uint32_t tail_every;      /* 0: the default */
    uint32_t nreq;            /* requests */
    uint64_t seed;
    uint32_t mkfs_bs;         /* 0: a fixture storm; else the names storm (extnstorm.c) on a
                               * volume mkfs made with blocks of this size */
};

/* One mount's state while a storm runs on it. */
struct run {
    struct ext_fs fs;
    struct ext_jnl j;
    uint8_t *cache, *jmem;
    uint32_t tid0;            /* the first commit's id */
    uint32_t last_tid;        /* the last id on_commit saw */
    bool     attempted;       /* on_commit was called */
    uint64_t landed[STORM_OBJS];   /* a block's last data write that landed (its hash) */
    bool     has_landed[STORM_OBJS];
    status_t why_stop;        /* the error the storm stopped at (OK: it finished) */
};

struct storm {
    const struct storm_cfg *cfg;
    struct cutdev cd;
    struct obj objs[STORM_OBJS];
    uint32_t nobj;
    uint32_t o_root, n_root;  /* where each kind of object starts in objs, and how many */
    uint32_t o_swap, o_many, o_block, o_wtime, o_file, n_file, o_listing;
    uint32_t holes_ino;       /* the file whose blocks the block objects are */
    struct request req[STORM_REQS];
    uint32_t nreq;
    /* The run without a cut: the state after each commit (0: after the
     * mount), and the blocks it wrote. */
    uint64_t snap[STORM_SNAPS][STORM_OBJS];
    uint32_t nsnap;
    uint64_t units;
};

/* Pick the storm's objects on the fixture and write its requests. */
void storm_make(struct storm *s, const struct storm_cfg *cfg, const struct img *m);
/* Mount the device's image with the journal (fresh memory in r). */
status_t run_mount(struct storm *s, struct run *r);
void run_free(struct run *r);
/* Run the storm on a mounted r until it ends or a call fails (the cut);
 * `snap`: record the state after each commit into s->snap. */
void storm_run(struct storm *s, struct run *r, bool snap);
/* Record the state after every commit made since the last record. */
void take_snaps(struct storm *s, struct run *r);
/* The objects' values now (blocks from the device's bytes). */
void storm_state(struct storm *s, struct run *r, uint64_t *out);
/* Up to max free blocks (every step-th of each group's), from the bitmaps. */
uint32_t free_blocks(struct ext_fs *fs, uint64_t *out, uint32_t max, uint32_t step);
/* A block's pattern for value, and its hash. */
void storm_pattern(uint8_t *block, uint32_t size, uint64_t value);
uint64_t block_hash(const uint8_t *block, uint32_t size);

#define PATTERN_MAGIC (1ull << 62)   /* a value whose pattern starts like a jbd2 commit block */

/* ---- extnstorm.c: the names storm ---------------------------------------------------------- */

void names_make(struct storm *s, const struct storm_cfg *cfg);
void names_run(struct storm *s, struct run *r, bool snap);
/* A hash of the volume's whole listing. */
uint64_t listing_hash(struct ext_fs *fs);

/* ---- extjnl.c ------------------------------------------------------------------------------ */

/* Every journal test on the two writable fixtures (fixtures[0] jam4k,
 * fixtures[1] linux1k); `images`: where to write the PC's cut images
 * (NULL: none). */
void check_journal(struct img *fixtures, const char *listdir, const char *images);
