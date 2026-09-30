/* fsserver: the main loop of an `fs` service (abi/idl/fs.idl, file.idl),
 * for the programs that serve a mount: the bootfs server, and the tests'
 * RAM filesystem.
 *
 * One thread waits on one port for the `fs` channels it serves (all of a
 * mount's clients share one by duplicating its client end) and for the
 * `file` channel of every open file. The service fills in the two ops
 * tables; its fs.open handler calls fsserver_open, which makes the file's
 * channel and its transfer buffer. A file is closed when its client closes
 * the channel (or dies): the loop then calls `closed` and frees the slot.
 *
 * Bounded: at most FSSERVER_MAX_FILES files are open at once
 * (ERR_NO_RESOURCES past that), each with a buffer of FSSERVER_BUF_SIZE
 * bytes charged to the service's job. A client that floods one channel
 * gets FSSERVER_ROUND requests answered, then the other channels their
 * turn. */
#pragma once

#include <fs_idl.h>

#define FSSERVER_MAX_FS    4            /* `fs` channels served at once */
#define FSSERVER_MAX_FILES 64           /* files open at once, over all clients */
#define FSSERVER_BUF_SIZE  (64u << 10)  /* a file's transfer buffer, bytes */
#define FSSERVER_ROUND     32           /* requests taken from one channel in a row */

struct fsserver_file {
    handle_t ch;      /* our end of its `file` channel (0: a free slot) */
    handle_t buf;     /* its transfer buffer: read with jam_vmo_read, filled with jam_vmo_write */
    void    *ctx;     /* the service's own state for the file */
};

struct fsserver {
    /* The service sets these before fsserver_init. */
    const struct fs_ops   *fs_ops;     /* called with ctx = this struct fsserver */
    const struct file_ops *file_ops;   /* called with ctx = the file's struct fsserver_file */
    /* The file's client is gone: release f->ctx. NULL: nothing to release. */
    void                 (*closed)(struct fsserver_file *f);
    void                  *ctx;        /* the service's own */
    /* The loop's. */
    handle_t              port;                        /* what it waits on */
    handle_t              fs[FSSERVER_MAX_FS];         /* our ends (0: a free slot) */
    struct fsserver_file  files[FSSERVER_MAX_FILES];   /* the open files */
};

/* Make the loop's port; the ops and ctx are set already, the rest zero. */
status_t fsserver_init(struct fsserver *s);
/* Serve the `fs` channel whose server end is ch (consumed, whatever
 * happens). ERR_NO_RESOURCES: FSSERVER_MAX_FS of them already. */
status_t fsserver_add_fs(struct fsserver *s, handle_t ch);
/* For the fs.open handler: a new open file with the service's state ctx.
 * *client and *client_buf are fs.open's `file` and `buffer` results (the
 * buffer without RIGHT_WRITE unless writable); *out is its slot.
 * ERR_NO_RESOURCES: FSSERVER_MAX_FILES are open. On an error nothing is
 * kept (ctx stays the caller's). */
status_t fsserver_open(struct fsserver *s, void *ctx, bool writable, handle_t *client,
                       handle_t *client_buf, struct fsserver_file **out);
/* Serve until no `fs` channel has a client left and no file is open (OK),
 * or the port fails (that status). */
status_t fsserver_run(struct fsserver *s);
