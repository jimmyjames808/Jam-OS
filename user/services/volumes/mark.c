/* volumes: Jam OS's mark on a GPT disk (M12.7 stage 2, docs/M12.7-PLAN.md
 * "What makes a disk Jam OS's"), checked with volumes' own code. The
 * driver of an NVMe drive (drivers/nvme, with the partition library) decides
 * what it will write; volumes decides what it will ask to write, and the
 * two checks are written twice on purpose: a bug in one is not a bug in
 * both. A disk volumes calls Jam OS's may be opened writable (`mount -w`,
 * the format of a prepare); any other is not asked for a writable channel,
 * whatever the driver would say.
 *
 * The mark, as far as storage.info and storage.partition show it: GPT, and
 * exactly two partitions, in table order the ESP (the ESP's type GUID,
 * named "Jam OS ESP") and the data partition (Jam OS's own type GUID,
 * 71bae565-d4a8-4315-8c81-9b35462cc158, named "Jam OS data"), the second
 * after the first. What volumes cannot see is whether both GPT headers are
 * good and agree, and whether an entry was left out: only the driver, which
 * read the table itself, knows that, and it refuses to open anything of a
 * disk that doesn't have both. */
#include <idl/storage.h>
#include "disk.h"

#define ESP_NAME  "Jam OS ESP"
#define DATA_NAME "Jam OS data"

/* The type GUIDs as their bytes on disk (the first three fields little-endian). */
static const uint8_t esp_type[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                      0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };
static const uint8_t data_type[16] = { 0x65, 0xe5, 0xba, 0x71, 0xa8, 0xd4, 0x15, 0x43,
                                       0x8c, 0x81, 0x9b, 0x35, 0x46, 0x2c, 0xc1, 0x58 };

bool disk_jamos_gpt(const struct disk *d)
{
    if (!d->known || d->scheme != STORAGE_SCHEME_GPT || d->partitions != 2 || d->nparts != 2)
        return false;
    const struct part_info *esp = &d->info[0], *data = &d->info[1];
    return !memcmp(esp->type_guid, esp_type, sizeof(esp_type)) && !strcmp(esp->label, ESP_NAME) &&
           !memcmp(data->type_guid, data_type, sizeof(data_type)) &&
           !strcmp(data->label, DATA_NAME) && esp->blocks && data->blocks &&
           data->start >= esp->start && data->start - esp->start >= esp->blocks;
}
