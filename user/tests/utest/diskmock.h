/* utest's mock usb-storage (diskmock.c): a RAM disk laid out like the
 * stick (an MBR, an ESP, a data partition), served through the `storage`
 * protocol and one `block` channel per opened partition by a thread of
 * utest, the way usb-storage serves the real one. disks.c hands its
 * `storage` channel to volumes (volumes.test_disk).
 *
 * It differs from ramdisk.h's RAM disk in being a whole disk: a partition
 * table, several `block` channels at once and one after another, and the
 * `storage` channel they are opened from. Both partitions start blank. */
#pragma once

#include <os.h>

#define DM_BLOCK       512u
#define DM_BLOCKS      8192u   /* the disk: 4 MiB */
#define DM_ESP_START   64u     /* partition 1: just under 1 MiB */
#define DM_ESP_BLOCKS  1984u
#define DM_DATA_START  2048u   /* partition 2: 3 MiB */
#define DM_DATA_BLOCKS 6144u
#define DM_MAX_OPEN    8       /* `block` channels open at once */

#define DM_TYPE_ESP    0xef    /* MBR partition types */
#define DM_TYPE_FAT32  0x0c

/* An opened partition. */
struct dm_block {
    struct diskmock *m;        /* the disk it belongs to */
    handle_t         ch;       /* our end of its `block` channel; 0: a free slot */
    handle_t         vmo;      /* its transfer buffer, once asked for */
    uint8_t          part;     /* 0: the ESP, 1: the data partition */
    bool             ro;       /* opened read-only */
};

/* One mock disk. The counters are written by the mock's thread and read
 * by the test (dm_count). */
struct diskmock {
    uint32_t        blocks;               /* the disk's size: DM_BLOCKS, or dm_start_sized's */
    uint8_t        *ram;                  /* `blocks` blocks: ram_vmo, mapped */
    handle_t        ram_vmo;              /* the disk's memory */
    handle_t        storage;              /* our end of the `storage` channel */
    handle_t        port, thread;         /* the thread's port, and the thread */
    struct dm_block blk[DM_MAX_OPEN];     /* the opened partitions */
    uint32_t        infos;                /* storage.info calls */
    uint32_t        partitions;           /* storage.partition calls */
    uint32_t        opened[2];            /* storage.open_partition calls, per partition */
    uint32_t        opened_rw;            /* ... of them, the ones not read-only */
    uint32_t        closed[2];            /* `block` channels whose client went, per partition */
    uint32_t        reads, writes, syncs; /* block requests answered OK */
    uint32_t        refused;              /* block requests refused (range, read-only) */
    uint32_t        refuse_opens;         /* the test's: refuse this many open_partition calls
                                           * to come (ERR_NO_RESOURCES), as a full driver would */
    uint32_t        hold_sync_ms;         /* the test's: each block.sync answered this late (the
                                           * mock's thread waits: nothing of this disk answers) */
    uint32_t        held;                 /* block.sync requests held so */
    uint8_t         stack[16384] __attribute__((aligned(16)));   /* the thread's */
};

/* Make the disk (partition types type1 and type2, both partitions blank)
 * and start serving; *client: the `storage` channel's client end. */
bool dm_start(struct diskmock *m, uint8_t type1, uint8_t type2, handle_t *client);
/* The same with a data partition of data_blocks blocks (the disk is that
 * and the ESP bigger): room for an ext volume (the smallest extfs makes is
 * about 6 MiB). */
bool dm_start_sized(struct diskmock *m, uint8_t type1, uint8_t type2, uint32_t data_blocks,
                    handle_t *client);
/* The stick is pulled: every channel of the mock closes, its thread ends. */
bool dm_stop(struct diskmock *m);
/* A counter of m, read from the test's thread. */
uint32_t dm_count(const uint32_t *counter);
