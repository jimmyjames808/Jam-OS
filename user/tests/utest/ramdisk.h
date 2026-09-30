/* utest: a RAM disk behind the `block` protocol (abi/idl/block.idl): what
 * usb-storage serves for one partition, without the stick. The disk's
 * sectors are a VMO mapped here, so a test can also look at them directly
 * (the FAT's dirty bit, a directory's short names). One session at a time:
 * ramdisk_serve starts a thread that answers one client channel until the
 * client closes it (or ramdisk_unplug makes the server close its end, as a
 * pulled stick does); the sectors stay for the next session. */
#pragma once

#include <os.h>

#define RAMDISK_SECTOR 512u
#define RAMDISK_BUF    (64u << 10)   /* the transfer buffer map_buffer hands out */

struct ramdisk {
    uint8_t *mem;           /* the sectors: blocks * RAMDISK_SECTOR bytes */
    handle_t mem_vmo;       /* ... their VMO */
    uint64_t blocks;        /* sectors on the disk */

    /* The session being served. */
    handle_t ch;            /* the server's end; closed by the thread when it ends */
    handle_t buf_vmo;       /* the transfer buffer */
    uint8_t *buf;           /* ... mapped */
    handle_t thread;        /* the serving thread */
    bool     read_only;     /* info says so, and writes are refused */
    bool     buf_given;     /* map_buffer was called (a second call: ERR_BAD_STATE) */

    /* Shared with the serving thread: RELAXED atomics. */
    bool     stop;          /* the server is to close its end (an unplug) */
    bool     fail_writes;   /* every write fails ERR_IO (a disk error) */
    uint32_t reads;         /* requests served, by kind */
    uint32_t writes;
    uint32_t syncs;

    uint8_t  stack[16384] __attribute__((aligned(16)));   /* the thread's */
};

/* A zeroed disk of `blocks` sectors. */
bool     ramdisk_create(struct ramdisk *rd, uint64_t blocks);
/* Start a session; *out_client is the `block` channel for the client. */
bool     ramdisk_serve(struct ramdisk *rd, bool read_only, handle_t *out_client);
/* The session's thread must end by itself (its client closed the channel);
 * then free the session. */
bool     ramdisk_join(struct ramdisk *rd);
/* Pull the stick: the server closes its end; then as ramdisk_join. */
bool     ramdisk_unplug(struct ramdisk *rd);
bool     ramdisk_destroy(struct ramdisk *rd);
uint32_t ramdisk_writes(const struct ramdisk *rd);
uint32_t ramdisk_syncs(const struct ramdisk *rd);
void     ramdisk_fail_writes(struct ramdisk *rd, bool on);
