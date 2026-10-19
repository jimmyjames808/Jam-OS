/* The POSIX hand-over's tests (<posixfds.h>): what posixfds.c and
 * posixfds_bad.c share. A collecting sink, handles of each kind made
 * fresh, the process's handle count, and raw messages built by hand
 * (for the malformed forms and the fuzz loop). */
#pragma once

#include <os.h>
#include <posixfds.h>

#define PFD_KEEP    80     /* descriptors a collect keeps */
#define PFD_DATA    64     /* data bytes it keeps of each */
#define PFD_RAW     2048   /* bytes of a hand-built message */

/* One descriptor a sink was given. */
struct pfd_got {
    struct posixfds_fd rec;                       /* its record */
    uint8_t  data[PFD_DATA];                      /* its data (the first PFD_DATA bytes) */
    handle_t hs[POSIXFDS_KIND_HANDLES];           /* its handles, ours */
};

/* A sink (struct posixfds_sink's ctx) that keeps up to PFD_KEEP
 * descriptors; past that, or with refuse set, it refuses (closing what
 * it was given). */
struct pfd_collect {
    struct pfd_got got[PFD_KEEP];   /* what it was given */
    unsigned n;                     /* entries of got */
    unsigned total;                 /* descriptors given, kept or not */
    unsigned resets;                /* reset calls */
    unsigned secures;               /* secure calls (POSIXFDS_SECURE) */
    status_t refuse;                /* OK, or what fd() answers */
    unsigned cwds;                  /* cwd_dir calls (POSIXFDS_CWD) */
    handle_t cwd;                   /* ... the last one's channel, ours */
    uint8_t  cwd_data[PFD_DATA];    /* ... and its data (the first PFD_DATA bytes) */
    uint32_t cwd_len;
};

/* A sink over c (emptied first). */
struct posixfds_sink pfd_sink(struct pfd_collect *c);
/* Close every handle c holds and empty it. */
void     pfd_collect_close(struct pfd_collect *c);
/* The handles this process holds now (its job's count: utest's job holds
 * only us while a test runs). */
uint64_t pfd_handles(void);
/* Fresh handles of kind's types into hs (posixfds_kind_handles' count). */
status_t pfd_make(uint32_t kind, handle_t hs[POSIXFDS_KIND_HANDLES]);
/* A message built by hand: the header, the directory (seq 0) and
 * records, with offsets kept so a test can break one field. */
struct pfd_raw {
    _Alignas(8) uint8_t b[PFD_RAW];   /* the bytes */
    uint32_t n;                       /* used */
    handle_t h[CHANNEL_MAX_HANDLES];  /* its handles */
    uint32_t nh;                      /* used */
    uint32_t rec_at[16];              /* where record i starts */
    unsigned nrec;                    /* records */
};
/* Start message seq (seq 0 with cwd and a fixed valid state). */
void     pfd_raw_begin(struct pfd_raw *m, uint16_t seq, bool last, const char *cwd);
/* Start message 0 with the current directory as a directory channel
 * (POSIXFDS_CWD): its data (len bytes) and ch, the message's first handle. */
void     pfd_raw_begin_dir(struct pfd_raw *m, bool last, const void *data, uint32_t len,
                           handle_t ch);
/* A directory channel's data (<posixfds.h>) into out: ids id and id + 1,
 * the mount m and the text path (NULL: none); its length. */
uint32_t pfd_dirch(uint8_t *out, uint64_t id, const char *m, const char *path);
/* Append a record of kind with fresh handles and data. */
status_t pfd_raw_fd(struct pfd_raw *m, int32_t fd, uint16_t kind, uint32_t status,
                    uint64_t offset, const void *data, uint32_t len);
/* The header of m, to change. */
struct posixfds_msg *pfd_hdr(struct pfd_raw *m);
/* Write m on ch, its handles moved. */
status_t pfd_raw_send(handle_t ch, struct pfd_raw *m);
/* Close m's handles (a message never sent). */
void     pfd_raw_drop(struct pfd_raw *m);
/* Messages queued on ch. */
uint32_t pfd_queued(handle_t ch);
