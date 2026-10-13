/* gptcheck's checks of the maker (pt_jamos_make, drivers/lib/parttable_make.c):
 * the table storage.create_table writes onto a blank disk, laid on an
 * image in memory and read back by pt_read with every check of
 * gptcheck.c (read_checked: the second reading included).
 *   - at 512 and 4096 bytes a block, on a small disk, the size of
 *     tools/mkgpt.py's "jamos" images, and one big enough for the data
 *     partition's cap: the disk reads as Jam OS's (pt_kind), its two
 *     partitions are where pt_jamos_plan says and where mkgpt.py puts
 *     them (the same rules, written twice), and every block the table
 *     takes lies inside the blank rule's zeros;
 *   - written in create_table's order (backup array, backup header,
 *     primary array, primary header, then the protective MBR): never
 *     Jam OS's before the MBR, blank (in progress) once a header is
 *     there, so a cut prepare runs again;
 *   - refused: a disk too small, a block size that isn't one, GUIDs all
 *     zeros or the same, a buffer too small. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <jam/parttable.h>
#include "gptcheck.h"

static const uint8_t disk_g[16] = { 0x10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
static const uint8_t esp_g[16] = { 0x20, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
static const uint8_t data_g[16] = { 0x30, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

static uint8_t made[64 * 1024];   /* the maker's buffer: 35 blocks of 512, 7 of 4096 */

/* An image of `bytes` bytes held as its two ends (the table lies there;
 * the middle is zeros, and nothing reads it). */
static void mem_img(struct img *m, uint32_t bs, uint64_t bytes)
{
    memset(m, 0, sizeof(*m));
    snprintf(m->name, sizeof(m->name), "made-%u-%llu", bs, (unsigned long long)bytes);
    m->fd = -1;
    m->bytes = bytes;
    m->hlen = HEAD_MAX;
    m->toff = bytes - HEAD_MAX;
    m->head = calloc(1, HEAD_MAX);
    m->tail = calloc(1, HEAD_MAX + 1);
    if (!m->head || !m->tail)
        fail("out of memory");
    m->bs0 = m->bs = bs;
    m->blocks = bytes / bs;
}

static void mem_free(struct img *m)
{
    free(m->head);
    free(m->tail);
}

/* n blocks of the made buffer from block `from` onto the image at `lba`. */
static void lay(struct img *m, uint64_t lba, uint32_t from, uint32_t n)
{
    uint64_t off = lba * m->bs, len = (uint64_t)n * m->bs;
    const uint8_t *src = made + (size_t)from * m->bs;
    if (off + len <= m->hlen)
        memcpy(m->head + off, src, len);
    else if (off >= m->toff)
        memcpy(m->tail + (off - m->toff), src, len);
    else
        fail("a table block at %llu outside the image's ends", (unsigned long long)lba);
}

static enum pt_kind kind_now(struct img *m, struct pt_table *t)
{
    if (read_checked(m, t) != OK)
        fail("pt_read failed");
    return pt_kind(t);
}

/* The two partitions as the plan says, named and typed as the mark wants. */
static void check_parts(const struct pt_table *t, const struct pt_jamos_plan *p, uint32_t bs)
{
    const struct pt_part *e = &t->parts[0], *d = &t->parts[1];
    if (t->nparts != 2 || e->start != p->esp_start || e->blocks != p->esp_blocks ||
        d->start != p->data_start || d->blocks != p->data_blocks)
        fail("partitions %llu+%llu, %llu+%llu; the plan says %llu+%llu, %llu+%llu",
             (unsigned long long)e->start, (unsigned long long)e->blocks,
             (unsigned long long)d->start, (unsigned long long)d->blocks,
             (unsigned long long)p->esp_start, (unsigned long long)p->esp_blocks,
             (unsigned long long)p->data_start, (unsigned long long)p->data_blocks);
    if (memcmp(e->unique_guid, esp_g, 16) || memcmp(d->unique_guid, data_g, 16) ||
        memcmp(t->disk_guid, disk_g, 16))
        fail("the GUIDs given aren't the table's");
    if ((d->start + d->blocks) * bs % PT_JAMOS_ALIGN || e->start * bs != PT_JAMOS_ALIGN ||
        e->blocks * bs != PT_JAMOS_ESP_BYTES)
        fail("the ESP isn't 512 MiB at 1 MiB, or the data partition doesn't end on a MiB");
}

/* One disk: made, laid in create_table's order, read after each write. */
static void one(uint32_t bs, uint64_t bytes, const struct img *mkgpt)
{
    static struct pt_table t;
    struct img m;
    struct pt_jamos_plan p;
    mem_img(&m, bs, bytes);
    where = m.name;
    if (pt_jamos_plan(bs, m.blocks, &p) != OK)
        fail("no plan");
    if (p.head_blocks * bs > PT_BLANK_BYTES || (m.blocks - p.tail_start) * bs > PT_BLANK_BYTES)
        fail("the table reaches past the blank rule's ends");
    if (pt_jamos_make(bs, m.blocks, disk_g, esp_g, data_g, made, sizeof(made)) != OK)
        fail("pt_jamos_make refused a good disk");
    if (kind_now(&m, &t) != PT_KIND_BLANK || !t.blank)
        fail("the empty image isn't blank");
    lay(&m, p.tail_start, PT_JAMOS_AT_ARRAY, p.array_blocks);   /* the backup array */
    if (kind_now(&m, &t) != PT_KIND_BLANK || !t.blank_in_progress || t.blank || t.nparts)
        fail("not blank-in-progress after the backup array alone (kind %d)", pt_kind(&t));
    /* A second prepare (other GUIDs) writes the same blocks again: still one cut disk. */
    static const uint8_t esp2[16] = { 0x21, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    static const uint8_t data2[16] = { 0x31, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    if (pt_jamos_make(bs, m.blocks, disk_g, esp2, data2, made, sizeof(made)) != OK)
        fail("pt_jamos_make refused other GUIDs");
    lay(&m, p.tail_start, PT_JAMOS_AT_ARRAY, p.array_blocks);   /* the same blocks, other GUIDs */
    if (kind_now(&m, &t) != PT_KIND_BLANK || !t.blank_in_progress)
        fail("not blank-in-progress after a second backup array");
    if (pt_jamos_make(bs, m.blocks, disk_g, esp_g, data_g, made, sizeof(made)) != OK)
        fail("pt_jamos_make refused a good disk");
    lay(&m, p.tail_start, PT_JAMOS_AT_ARRAY, p.array_blocks);   /* the first prepare's again */
    lay(&m, m.blocks - 1, pt_jamos_at_backup(&p), 1);           /* the backup header */
    if (kind_now(&m, &t) != PT_KIND_BLANK || !t.blank_in_progress)
        fail("not blank-in-progress after the backup header");
    lay(&m, PT_JAMOS_AT_ARRAY, PT_JAMOS_AT_ARRAY, p.array_blocks);   /* the primary array */
    if (kind_now(&m, &t) != PT_KIND_BLANK || !t.blank_in_progress)
        fail("not blank-in-progress after the primary array");
    lay(&m, 1, PT_JAMOS_AT_PRIMARY, 1);                          /* the primary header */
    if (kind_now(&m, &t) != PT_KIND_BLANK || !t.blank_in_progress)
        fail("not blank-in-progress with both headers and no MBR");
    lay(&m, 0, PT_JAMOS_AT_MBR, 1);                              /* the protective MBR */
    if (kind_now(&m, &t) != PT_KIND_JAMOS || t.hybrid || t.primary || t.backup)
        fail("not Jam OS's once whole (kind %d)", pt_kind(&t));
    check_parts(&t, &p, bs);
    if (mkgpt) {   /* mkgpt.py's jamos layout of the same disk: the same partitions */
        static struct pt_table w;
        struct img copy = *mkgpt;
        if (read_checked(&copy, &w) != OK || w.nparts != 2)
            fail("%s unreadable", mkgpt->name);
        for (int i = 0; i < 2; i++)
            if (w.parts[i].start != t.parts[i].start || w.parts[i].blocks != t.parts[i].blocks ||
                strcmp(w.parts[i].name, t.parts[i].name) ||
                memcmp(w.parts[i].type_guid, t.parts[i].type_guid, 16))
                fail("partition %d isn't %s's", i + 1, mkgpt->name);
    }
    mem_free(&m);
}

/* The made table's CRCs again, after an edit of its entries: the array's
 * in both headers, then each header's own. */
static void reseal(const struct pt_jamos_plan *p, uint32_t bs)
{
    uint32_t crc = pt_crc32(0, made + (size_t)PT_JAMOS_AT_ARRAY * bs, PT_JAMOS_ARRAY_BYTES);
    uint8_t *hdrs[2] = { made + (size_t)PT_JAMOS_AT_PRIMARY * bs,
                         made + (size_t)pt_jamos_at_backup(p) * bs };
    for (int i = 0; i < 2; i++) {
        put32(hdrs[i] + 88, crc);
        put32(hdrs[i] + 16, 0);
        put32(hdrs[i] + 16, pt_crc32(0, hdrs[i], 92));
    }
}

/* Jam OS's two partitions by type and name, but not where the plan puts
 * them on this disk (the ESP one block short), both headers good, block 0
 * zero: a create_table cut short would have the plan's places, so this is
 * someone's table, and the blocks create_table writes weren't seen to be
 * its own or zeros. Not blank, with either header alone or both. */
static void wrong_geometry(uint32_t bs, uint64_t bytes)
{
    static struct pt_table t;
    struct img m;
    struct pt_jamos_plan p;
    mem_img(&m, bs, bytes);
    where = "made-wrong-geometry";
    if (pt_jamos_plan(bs, m.blocks, &p) != OK ||
        pt_jamos_make(bs, m.blocks, disk_g, esp_g, data_g, made, sizeof(made)) != OK)
        fail("no table");
    uint8_t *esp = made + (size_t)PT_JAMOS_AT_ARRAY * bs;
    put64(esp + 40, get64(esp + 40) - 1);   /* the ESP's EndingLBA */
    reseal(&p, bs);
    lay(&m, p.tail_start, PT_JAMOS_AT_ARRAY, p.array_blocks);
    lay(&m, m.blocks - 1, pt_jamos_at_backup(&p), 1);
    if (kind_now(&m, &t) != PT_KIND_OTHER || t.blank_in_progress)
        fail("the backup alone, at other places: kind %d", pt_kind(&t));
    lay(&m, PT_JAMOS_AT_ARRAY, PT_JAMOS_AT_ARRAY, p.array_blocks);
    lay(&m, 1, PT_JAMOS_AT_PRIMARY, 1);
    if (kind_now(&m, &t) != PT_KIND_OTHER || t.blank_in_progress)
        fail("both headers, at other places: kind %d", pt_kind(&t));
    mem_free(&m);
}

/* What the maker refuses. */
static void refusals(void)
{
    static const uint8_t zero[16];
    struct pt_jamos_plan p;
    uint64_t gib = (1ull << 30) / 512;
    where = "made-refusals";
    if (pt_jamos_plan(512, (513ull << 20) / 512 + 34, &p) != ERR_NO_SPACE ||
        pt_jamos_plan(4096, 1, &p) != ERR_NO_SPACE ||
        pt_jamos_plan(0, gib, &p) != ERR_INVALID_ARGS ||
        pt_jamos_plan(1024 + 512, gib, &p) != ERR_INVALID_ARGS ||
        pt_jamos_plan(8192, gib, &p) != ERR_INVALID_ARGS)
        fail("a disk the plan should refuse");
    const uint8_t *bad[][3] = { { zero, esp_g, data_g }, { disk_g, zero, data_g },
                                { disk_g, esp_g, zero }, { disk_g, disk_g, data_g },
                                { disk_g, esp_g, esp_g }, { data_g, esp_g, data_g } };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (pt_jamos_make(512, gib, bad[i][0], bad[i][1], bad[i][2], made, sizeof(made)) !=
            ERR_INVALID_ARGS)
            fail("GUIDs %u accepted", i);
    if (pt_jamos_make(512, gib, disk_g, esp_g, data_g, made, 34 * 512) != ERR_INVALID_ARGS ||
        pt_jamos_make(512, 1000, disk_g, esp_g, data_g, made, sizeof(made)) != ERR_NO_SPACE)
        fail("a buffer too small, or a disk too small, accepted");
}

void check_make(const struct img *imgs, int n)
{
    const struct img *j512 = NULL, *j4k = NULL;
    for (int i = 0; i < n; i++) {
        if (!strcmp(imgs[i].name, "jamos"))
            j512 = &imgs[i];
        if (!strcmp(imgs[i].name, "jamos-4k"))
            j4k = &imgs[i];
    }
    if (!j512 || !j4k)
        fail("no jamos images among the cases");
    one(512, j512->bytes, j512);
    one(4096, j4k->bytes, j4k);
    one(512, 600ull << 20, NULL);              /* about the smallest: 64 MiB of data and more */
    one(4096, 600ull << 20, NULL);
    one(512, 3ull << 40, NULL);                /* the data partition capped at 2 TiB */
    struct pt_jamos_plan p;
    if (pt_jamos_plan(512, (3ull << 40) / 512, &p) != OK ||
        p.data_blocks != PT_JAMOS_DATA_MAX / 512)
        fail("the data partition of a 3 TiB disk isn't 2 TiB");
    wrong_geometry(512, j512->bytes);
    wrong_geometry(4096, j4k->bytes);
    refusals();
}
