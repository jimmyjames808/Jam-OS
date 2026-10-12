/* The wayland section's rig (wlrig.c), shared by its checks (wayland.c)
 * and its fuzz loop (wlfuzz.c): a client Wayland socket connected to a
 * fake compositor. The test publishes /svc/wayland in its own namespace
 * (ns_svc_set: a channel whose other end it keeps), connects, and takes
 * the name away again, so the client's end is its channel's only handle;
 * the test then reads and writes the other end as the compositor would,
 * JWL2 batches and handles (<jwl.h>). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <jamos/libjam.h>
#include <jwl.h>
#include <sys/types.h>

#define WR_POOL_RIGHTS (RIGHT_READ | RIGHT_MAP | RIGHT_INSPECT | RIGHT_TRANSFER)   /* a pool's */

/* A client socket connected to a fake compositor end. */
struct rig {
    int      fd;     /* the client's Wayland socket (non-blocking) */
    handle_t comp;   /* the compositor's end of its channel */
};

/* One batch as the compositor reads it. */
struct batch {
    uint8_t  b[JWL_BATCH_MAX];
    uint32_t nb, nh;                  /* bytes (its header's included) and handles */
    handle_t h[JWL_BATCH_HANDLES];
};

/* What a recvmsg gave. */
struct rx {
    int      fds[JWL_HANDLES_QUEUED];   /* the descriptors in its SCM_RIGHTS block */
    unsigned n;                         /* how many */
    int      flags;                     /* msg_flags */
};

/* Handles charged to our job now. */
uint64_t wr_job_handles(void);
/* The 32-bit word at p, and the same written. */
uint32_t wr_rd32(const uint8_t *p);
void     wr_wr32(uint8_t *p, uint32_t v);
/* A Wayland message of size bytes at p: an id, the size and opcode, a
 * pattern after. */
void     wr_msg(uint8_t *p, uint32_t id, uint32_t size);
/* A new non-blocking AF_UNIX stream socket, and connect of one to path. */
int      wr_sock_new(void);
int      wr_connect_to(int fd, const char *path);
/* The rig (true), or false with nothing left open. */
bool     wr_open(struct rig *r);
void     wr_close(struct rig *r);
/* The compositor reads the next batch: OK, or the read's status. */
status_t wr_comp_read(struct rig *r, struct batch *bt);
/* The batch is well formed: its header, whole messages of good sizes. */
bool     wr_batch_ok(const struct batch *bt);
/* The compositor writes a batch: n bytes after a right header, and nh
 * handles (moved); and the same with the header's magic and count given. */
status_t wr_comp_write(struct rig *r, const void *bytes, uint32_t n, handle_t *h, uint32_t nh);
status_t wr_comp_write_hdr(struct rig *r, const uint32_t hdr[2], const void *bytes, uint32_t n,
                           handle_t *h, uint32_t nh);
void     wr_close_handles(const handle_t *h, uint32_t n);
/* sendmsg of n bytes at p with nfds descriptors (MSG_NOSIGNAL, MSG_DONTWAIT). */
ssize_t  wr_send(int fd, const void *p, size_t n, const int *fds, unsigned nfds);
/* recvmsg into buf (n bytes) with room for room descriptors
 * (MSG_DONTWAIT, MSG_CMSG_CLOEXEC): the count, and what came in *out. */
ssize_t  wr_recv(int fd, void *buf, size_t n, unsigned room, struct rx *out);
/* A memfd holding text; a kept one-page VMO holding text, with rights. */
int      wr_memfd_with(const char *text);
handle_t wr_vmo_with(const char *text, rights_t rights);

/* The fuzz loop (wlfuzz.c). */
void     wr_fuzz(void);
