/* extfs: the disk under the ext library. Its device (struct ext_dev: read
 * and write by byte offset) becomes `block` calls on the one partition
 * channel extfs was given, through the channel's shared buffer (mapped
 * once), a buffer's worth per call, each waited for: the journal's order
 * (data, then the log, then its commit block, then the blocks home) is
 * the order the calls are made in.
 *
 * An answered write is on the medium: drv/nvme writes with FUA (M12.7).
 * usb-storage doesn't, which is one reason an ext volume on a stick is
 * served read-only (the plan's Q2 and Q9); a writable one there would need
 * a block.sync after each commit.
 *
 * The library reads in multiples of 1 KiB at multiples of 1 KiB and
 * writes whole blocks. A channel whose blocks are bigger than that (a 4 KiB
 * NVMe namespace) gets each read rounded out to its blocks and copied
 * from the buffer; a write that isn't whole blocks of the channel's (a
 * volume of 1 KiB blocks on 4 KiB sectors, which Linux's mkfs never makes)
 * is refused.
 *
 * Every call has a time limit (BLOCK_WAIT), so a disk that stops answering
 * fails the request with ERR_IO instead of hanging extfs; a call that finds
 * the channel closed sets vol.disk_gone, and main.c winds extfs down. */
#include <idl/block.h>
#include "extfs.h"

#define BLOCK_WAIT (30 * NS_PER_S)
#define BUF_WANT   (256u << 10)     /* the buffer asked for: a journal's staged run in one call */

static uint64_t writes;

static status_t failed(const char *what, uint64_t lba, uint32_t count, status_t st)
{
    if (st == ERR_PEER_CLOSED) {
        vol.disk_gone = true;
        return st;
    }
    printf("extfs %s: %s of %u blocks at %lu failed: %s\n", vol.name, what, count,
           (unsigned long)lba, status_str(st));
    return ERR_IO;
}

static status_t dev_read(void *ctx, uint64_t off, uint32_t len, void *buf)
{
    (void)ctx;
    uint8_t *out = buf;
    uint32_t sec = vol.sector;
    uint32_t per = vol.bbuf_size / sec;
    if (per > vol.max_sectors)
        per = vol.max_sectors;
    while (len) {
        uint64_t lba = off / sec;
        uint32_t skip = (uint32_t)(off % sec);
        uint64_t want = ((uint64_t)skip + len + sec - 1) / sec;
        uint32_t count = want > per ? per : (uint32_t)want;
        if (lba >= vol.sectors || count > vol.sectors - lba)
            return ERR_OUT_OF_RANGE;
        status_t st = block_read_within(vol.block, BLOCK_WAIT, lba, count, 0);
        if (st != OK)
            return failed("read", lba, count, st);
        uint32_t n = count * sec - skip;
        if (n > len)
            n = len;
        memcpy(out, vol.bbuf + skip, n);
        out += n;
        off += n;
        len -= n;
    }
    return OK;
}

static status_t dev_write(void *ctx, uint64_t off, uint32_t len, const void *buf)
{
    (void)ctx;
    const uint8_t *in = buf;
    uint32_t sec = vol.sector;
    if (vol.read_only)
        return ERR_ACCESS_DENIED;
    if (off % sec || len % sec)
        return ERR_NOT_SUPPORTED;
    uint32_t per = vol.bbuf_size / sec;
    if (per > vol.max_sectors)
        per = vol.max_sectors;
    while (len) {
        uint64_t lba = off / sec;
        uint32_t count = len / sec > per ? per : len / sec;
        if (lba >= vol.sectors || count > vol.sectors - lba)
            return ERR_OUT_OF_RANGE;
        memcpy(vol.bbuf, in, (size_t)count * sec);
        status_t st = block_write_within(vol.block, BLOCK_WAIT, lba, count, 0);
        if (st != OK)
            return failed("write", lba, count, st);
        writes++;
        test_die(XF_DIE_WRITE);
        in += (size_t)count * sec;
        off += (uint64_t)count * sec;
        len -= count * sec;
    }
    return OK;
}

uint64_t disk_writes(void)
{
    return writes;
}

status_t disk_open(handle_t block, struct ext_dev *dev)
{
    uint32_t bs = 0, size = 0, maxb = 0;
    uint8_t ro = 0;
    handle_t vmo = HANDLE_INVALID;
    status_t st = block_info_within(block, BLOCK_WAIT, &bs, &vol.sectors, &ro, &maxb, NULL,
                                   NULL);
    if (st == OK && (bs < 512 || bs > 4096 || (bs & (bs - 1)))) {
        printf("extfs %s: %u-byte blocks are not supported\n", vol.name, bs);
        return ERR_NOT_SUPPORTED;
    }
    if (st == OK)
        st = block_map_buffer_within(block, BLOCK_WAIT, BUF_WANT, &vmo, &size);
    if (st == ERR_PEER_CLOSED)
        vol.disk_gone = true;
    if (st != OK)
        return st;
    size -= size % PAGE_SIZE;
    uint64_t addr = 0;
    uint32_t perms = ro ? VMAR_READ : VMAR_READ | VMAR_WRITE;
    st = size >= bs ? jam_vmar_map(startup_handle(SR_SELF_VMAR), vmo, 0, size, perms, &addr)
                    : ERR_INVALID_ARGS;
    jam_handle_close(vmo);
    if (st != OK)
        return st;
    vol.block = block;
    vol.bbuf = (uint8_t *)(uintptr_t)addr;
    vol.bbuf_size = size;
    vol.sector = bs;
    vol.max_sectors = maxb ? maxb : size / bs;
    vol.read_only = ro != 0;
    *dev = (struct ext_dev){
        .ctx = NULL, .bytes = vol.sectors * bs, .read = dev_read,
        .write = ro ? NULL : dev_write,
    };
    return OK;
}

status_t disk_is_blank(bool *out)
{
    static uint8_t first[64u << 10];
    uint64_t n = vol.sectors * vol.sector < sizeof(first) ? vol.sectors * vol.sector
                                                          : sizeof(first);
    status_t st = dev_read(NULL, 0, (uint32_t)n, first);
    if (st != OK)
        return st;
    *out = true;
    for (uint64_t i = 0; i < n && *out; i++)
        *out = first[i] == 0;
    return OK;
}
