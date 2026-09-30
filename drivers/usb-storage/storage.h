/* usb-storage: what its files share (main.c: the driver and its loop;
 * bot.c: the Bulk-Only Transport; scsi.c: the SCSI commands; block.c: the
 * partition table and the `block` channels). See main.c for the driver.
 *
 * One thread; nothing is locked. One SCSI command runs at a time, inside
 * the request that needs it. */
#pragma once

#include <jam/driver.h>

#define MAX_PARTS  4        /* an MBR's primary partitions */
#define MAX_BLKS   8        /* `block` channels open at once */
#define BLOCK_BUF  65536u   /* a block channel's buffer (block.idl: 64 KiB) */
#define XFER_TAIL  4096u    /* the end of the bulk buffer, kept for the CBW, CSW and sense */

static inline uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

/* One partition of the disk, as the MBR lists it. */
struct part {
    uint8_t  type;     /* the MBR type byte */
    uint64_t start;    /* its first block */
    uint64_t blocks;   /* its length */
};

/* The disk: one mass-storage interface, LUN 0. */
struct disk {
    handle_t usb;             /* DR_USB: the interface's `usb` channel */
    uint16_t vid, pid;        /* USB ids, for the log */
    uint8_t  ifnum;           /* bInterfaceNumber (the class requests' wIndex) */
    uint8_t  ep_in, ep_out;   /* the bulk endpoints' addresses */
    uint8_t *xbuf;            /* usb-bus's bulk buffer, mapped */
    uint32_t xsize;           /* its size */
    uint32_t data_max;        /* bytes of data one command may move (xsize - XFER_TAIL) */
    uint32_t tag;             /* the last CBW's tag */
    bool     gone;            /* the device left (ERR_PEER_CLOSED from usb-bus) */
    uint32_t resets;          /* reset recoveries so far */
    uint32_t failures;        /* failed commands so far (the first few are logged) */
    uint8_t  key, asc, ascq;  /* the last failed command's sense (key 0xff: unknown) */
    bool     no_sync;         /* the device refused SYNCHRONIZE CACHE: not sent again */

    uint8_t  vendor[8], product[16];   /* INQUIRY's, space-padded */
    uint32_t block_size;      /* bytes per block (0: no medium) */
    uint64_t blocks;          /* blocks on the medium */
    struct part parts[MAX_PARTS];      /* its partitions, in table order */
    uint8_t  nparts;          /* entries in parts[] */
};

/* ---- bot.c -------------------------------------------------------------------- */

/* One command for the transport. Its data is in (or comes into) the bulk
 * buffer at `off`. */
struct bot_cmd {
    uint8_t  cdb[16];      /* the SCSI command block */
    uint8_t  cdb_len;      /* its length: 6, 10, 12 or 16 */
    bool     in;           /* the data phase's direction */
    uint32_t off;          /* where in the bulk buffer its data is */
    uint32_t len;          /* bytes of data, 0: no data phase */
    uint32_t timeout_ms;   /* for the data phase and for the status */
};

/* Run c: CBW, data, CSW. OK: the device answered; *failed says whether it
 * failed the command (then REQUEST SENSE says why), *moved the data bytes.
 * ERR_IO: the transport broke and was reset (reset recovery): the command
 * may be tried again. ERR_TIMED_OUT: the same, after a phase timed out.
 * ERR_PEER_CLOSED: the device is gone (k->gone). Never waits longer than
 * the command's timeouts plus the recovery's. */
status_t bot_run(struct disk *k, const struct bot_cmd *c, uint32_t *moved, bool *failed);
/* GET MAX LUN: the number of logical units (1 if the device doesn't say). */
unsigned bot_luns(struct disk *k);

/* ---- scsi.c ------------------------------------------------------------------- */

/* INQUIRY, TEST UNIT READY (waiting while the medium spins up), READ
 * CAPACITY(10): fills the vendor, product, block size and count. No
 * medium: OK with block_size 0. Else ERR_NOT_SUPPORTED (not a disk, or a
 * block size we can't use), ERR_IO, ERR_TIMED_OUT, ERR_PEER_CLOSED. */
status_t scsi_bring_up(struct disk *k);
/* READ(10) / WRITE(10) of `count` blocks at `lba`, to / from the bulk
 * buffer's start; count * block_size <= data_max. ERR_IO, ERR_TIMED_OUT,
 * ERR_PEER_CLOSED; a write-protected medium: ERR_ACCESS_DENIED. */
status_t scsi_rw(struct disk *k, bool write, uint64_t lba, uint32_t count);
/* SYNCHRONIZE CACHE(10). A device without the command has no cache: OK. */
status_t scsi_sync(struct disk *k);

/* ---- block.c ------------------------------------------------------------------ */

/* Read the MBR into k->parts. OK with nparts 0 when there is no table. */
status_t parts_read(struct disk *k);
/* A new `block` channel for partition `index`, watched on `port`: the
 * client's end. ERR_OUT_OF_RANGE, ERR_NO_RESOURCES. */
status_t blk_open(struct disk *k, handle_t port, uint8_t index, bool read_only, handle_t *out);
/* A port packet with a block channel's key: mark it pending. */
void blk_packet(const struct port_packet *p);
/* Serve every pending block channel; true if any had something. */
bool blk_serve(void);
void blk_close_all(void);

#define KEY_SERVE 1ull
#define KEY_USB   2ull
#define KEY_BLK   (1ull << 32)   /* | gen << 8 (16 bits) | index (8 bits) */
