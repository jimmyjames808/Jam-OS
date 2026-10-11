/* The partition table library's maker: Jam OS's table for a blank disk
 * (storage.create_table), into the caller's buffer. <jam/parttable.h> has
 * the layout and the buffer's blocks; pt_read reads what this makes as
 * Jam OS's (tools/gptcheck.c checks it at several sizes and both block
 * sizes).
 *
 * The protective MBR (UEFI 2.10, 5.2.3): one entry of type EEh from
 * block 1 over the whole disk (0xFFFFFFFF blocks at most), CHS 000200h to
 * FFFFFFh, status 0, no disk signature. The headers (5.3.2): revision 1.0,
 * 92 bytes, the primary at block 1 with its array at block 2, the backup
 * at the last block with its array just before it, the usable range
 * between the two arrays. The entries (5.3.3): the ESP's type named
 * PT_JAMOS_ESP_NAME, then Jam OS's data type named PT_JAMOS_DATA_NAME,
 * attributes 0, names in UTF-16LE. */
#include <jam/parttable.h>

#define HDR_BYTES    92     /* the header's defined bytes */
#define MBR_TYPE_GPT 0xee   /* a protective MBR's entry type */

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v);
    put16(p + 2, v >> 16);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static void copy16(uint8_t *dst, const uint8_t *src)
{
    for (int i = 0; i < 16; i++)
        dst[i] = src[i];
}

status_t pt_jamos_plan(uint32_t block_size, uint64_t blocks, struct pt_jamos_plan *out)
{
    uint32_t bs = block_size;
    if (bs < 512 || bs > 4096 || (bs & (bs - 1)))
        return ERR_INVALID_ARGS;
    uint32_t array = PT_JAMOS_ARRAY_BYTES / bs;   /* whole blocks: 32 at 512, 4 at 4096 */
    uint64_t align = PT_JAMOS_ALIGN / bs, esp = PT_JAMOS_ESP_BYTES / bs;
    uint64_t head = PT_JAMOS_AT_ARRAY + array, tail = 1 + (uint64_t)array;
    /* The ESP's end must leave room for the minimum data partition and the
     * backup table: checked before anything below can wrap. */
    if (blocks < align + esp + PT_JAMOS_DATA_MIN / bs + align + tail)
        return ERR_NO_SPACE;
    uint64_t last = blocks - tail - 1;   /* the last usable block */
    uint64_t start = align + esp, end = (last + 1) / align * align;
    if (end - start > PT_JAMOS_DATA_MAX / bs)
        end = start + PT_JAMOS_DATA_MAX / bs;
    if (end - start < PT_JAMOS_DATA_MIN / bs)
        return ERR_NO_SPACE;
    *out = (struct pt_jamos_plan){ .array_blocks = array, .head_blocks = head,
                                   .tail_start = blocks - tail, .esp_start = align,
                                   .esp_blocks = esp, .data_start = start,
                                   .data_blocks = end - start };
    return OK;
}

/* One entry: the type, its unique GUID, its blocks, its name (ASCII). */
static void entry(uint8_t *e, const uint8_t *type, const uint8_t *uniq, uint64_t start,
                  uint64_t blocks, const char *name)
{
    copy16(e, type);
    copy16(e + 16, uniq);
    put64(e + 32, start);
    put64(e + 40, start + blocks - 1);   /* EndingLBA is inclusive */
    for (int i = 0; name[i]; i++)
        put16(e + 56 + 2 * i, (uint8_t)name[i]);
}

/* A header at block `my`, the other at `alt`, its array at `array_lba`. */
static void header(uint8_t *h, const struct pt_jamos_plan *p, uint64_t blocks, uint64_t my,
                   uint64_t alt, uint64_t array_lba, const uint8_t *guid, uint32_t array_crc)
{
    static const uint8_t sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
    for (int i = 0; i < 8; i++)
        h[i] = sig[i];
    put32(h + 8, 0x00010000);   /* revision 1.0 */
    put32(h + 12, HDR_BYTES);
    put64(h + 24, my);
    put64(h + 32, alt);
    put64(h + 40, p->head_blocks);       /* FirstUsableLBA */
    put64(h + 48, blocks - (1 + p->array_blocks) - 1);   /* LastUsableLBA */
    copy16(h + 56, guid);
    put64(h + 72, array_lba);
    put32(h + 80, PT_JAMOS_ENTRIES);
    put32(h + 84, 128);
    put32(h + 88, array_crc);
    put32(h + 16, pt_crc32(0, h, HDR_BYTES));   /* its own field zero meanwhile */
}

static void mbr(uint8_t *b, uint64_t blocks)
{
    uint8_t *e = b + 446;
    e[1] = 0x00;   /* CHS of block 1: head 0, sector 2, cylinder 0 */
    e[2] = 0x02;
    e[3] = 0x00;
    e[4] = MBR_TYPE_GPT;
    e[5] = e[6] = e[7] = 0xff;
    put32(e + 8, 1);
    put32(e + 12, blocks - 1 > 0xffffffffu ? 0xffffffffu : (uint32_t)(blocks - 1));
    b[510] = 0x55;
    b[511] = 0xaa;
}

status_t pt_jamos_make(uint32_t block_size, uint64_t blocks, const uint8_t *disk_guid,
                       const uint8_t *esp_guid, const uint8_t *data_guid, uint8_t *buf,
                       size_t buf_size)
{
    struct pt_jamos_plan p;
    status_t st = pt_jamos_plan(block_size, blocks, &p);
    if (st != OK)
        return st == ERR_NO_SPACE ? st : ERR_INVALID_ARGS;
    if (pt_guid_zero(disk_guid) || pt_guid_zero(esp_guid) || pt_guid_zero(data_guid) ||
        pt_guid_eq(disk_guid, esp_guid) || pt_guid_eq(disk_guid, data_guid) ||
        pt_guid_eq(esp_guid, data_guid))
        return ERR_INVALID_ARGS;
    size_t need = (size_t)pt_jamos_buf_blocks(&p) * block_size;
    if (buf_size < need)
        return ERR_INVALID_ARGS;
    for (size_t i = 0; i < need; i++)
        buf[i] = 0;
    uint8_t *array = buf + (size_t)PT_JAMOS_AT_ARRAY * block_size;
    entry(array, pt_type_esp, esp_guid, p.esp_start, p.esp_blocks, PT_JAMOS_ESP_NAME);
    entry(array + 128, pt_type_jamos_data, data_guid, p.data_start, p.data_blocks,
          PT_JAMOS_DATA_NAME);
    uint32_t crc = pt_crc32(0, array, PT_JAMOS_ARRAY_BYTES);
    uint64_t end = blocks - 1;
    header(buf + (size_t)PT_JAMOS_AT_PRIMARY * block_size, &p, blocks, 1, end, PT_JAMOS_AT_ARRAY,
           disk_guid, crc);
    header(buf + (size_t)pt_jamos_at_backup(&p) * block_size, &p, blocks, end, 1,
           p.tail_start, disk_guid, crc);
    mbr(buf + (size_t)PT_JAMOS_AT_MBR * block_size, blocks);
    return OK;
}
