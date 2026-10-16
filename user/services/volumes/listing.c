/* volumes: what the disk listing knows and asks (voldisks, abi/idl/volumes.idl;
 * request.c has the handlers). `kind` and `read_only` come from volumes' own
 * mark check (mark.c).
 *
 * The listing answers from what the steps of disk.c already asked each
 * driver (storage.info, storage.partition), plus one more question for an
 * NVMe drive, storage.ident (its model, serial number and firmware: a
 * step of its own, DISK_IDENT, after the partitions). A stick has only the
 * INQUIRY's vendor and product. The digest and the health log are asked of
 * the driver each time, in a task (the caller answers later).
 *
 * Nothing here changes a disk or a mount. */
#include <idl/storage.h>
#include "disk.h"

#define IDENT_WAIT  CALL_WAIT
#define DIGEST_WAIT (60 * NS_PER_S)   /* up to 1 GiB hashed in the driver, software SHA-256 */
#define HEALTH_WAIT (10 * NS_PER_S)

/* The real disk called id, or NULL (a test disk is no disk to the listing). */
struct disk *disk_named(uint32_t id)
{
    for (unsigned i = 0; i < MAX_DISKS; i++)
        if (disks[i].state != DISK_FREE && !disks[i].test && disks[i].id == id)
            return &disks[i];
    return NULL;
}

void disk_number(struct disk *d)
{
    for (unsigned n = 0;; n++) {
        bool used = false;
        for (unsigned i = 0; i < MAX_DISKS && !used; i++)
            used = &disks[i] != d && disks[i].state != DISK_FREE && !disks[i].test &&
                   disks[i].bus == d->bus && disks[i].number == n;
        if (!used) {
            d->number = (uint8_t)n;
            return;
        }
    }
}

const char *disk_label(const struct disk *d)
{
    static char s[16];
    snprintf(s, sizeof(s), "%s%u", d->bus == DISKS_BUS_NVME ? "nvme" : "usb", d->number);
    return s;
}

/* ---- storage.ident: an NVMe drive's identity, a step after the partitions ------------- */

bool ident_ask(struct disk *d)
{
    if (d->bus != DISKS_BUS_NVME)
        return false;
    d->txid = call_txid();
    d->deadline = now() + IDENT_WAIT;
    d->state = DISK_IDENT;
    if (storage_ident_send(d->ch, d->txid) == OK)
        return true;
    d->deadline = now();   /* disk_run_due goes on without it */
    return true;
}

void ident_answer(struct disk *d, const void *rep, struct idl_msg *m)
{
    status_t st = storage_ident_result(rep, m, d->model, NULL, d->serial, NULL, d->firmware,
                                       NULL);
    if (st != OK) {
        say(false, "volumes: %s: storage.ident failed (%s)", disk_name(d), status_str(st));
        d->model[0] = d->serial[0] = d->firmware[0] = '\0';
    }
}

/* ---- the listing ------------------------------------------------------------------- */

/* Has d answered what the steps asked? Else everything but its identity is
 * zero in the listing. */
static bool disk_ready(const struct disk *d)
{
    return d->known && d->state != DISK_DOWN;
}

static uint8_t disk_kind(const struct disk *d)
{
    if (d->state == DISK_BOOT)
        return VOLDISKS_KIND_JAMOS;
    if (d->scheme == STORAGE_SCHEME_MBR && d->partitions >= 2 && d->type[0] == TYPE_ESP &&
        d->type[1] == TYPE_FAT32_LBA)
        return VOLDISKS_KIND_JAMOS;
    if (disk_jamos_gpt(d))
        return VOLDISKS_KIND_JAMOS;   /* an NVMe drive prepared by Jam OS (mark.c) */
    if (d->scheme == STORAGE_SCHEME_NONE && d->partitions == 0)
        return VOLDISKS_KIND_NO_TABLE;
    return VOLDISKS_KIND_OTHER;
}

static uint32_t put_str(char *out, uint32_t max, const char *s)
{
    uint32_t n = 0;
    while (n < max && s[n])
        out[n] = s[n], n++;
    return n;
}

status_t list_disk(uint32_t index, struct list_disk *o)
{
    const struct disk *d = NULL;
    for (unsigned i = 0, seen = 0; i < MAX_DISKS && !d; i++) {
        if (disks[i].state == DISK_FREE || disks[i].test)
            continue;
        if (seen++ == index)
            d = &disks[i];
    }
    if (!d)
        return ERR_OUT_OF_RANGE;
    bool ready = disk_ready(d);
    *o = (struct list_disk){ .id = d->id, .bus = d->bus, .ready = ready };
    o->name_len = put_str(o->name, sizeof(o->name), disk_label(d));
    if (!ready)
        return OK;
    o->kind = disk_kind(d);
    o->scheme = d->scheme;
    /* No writable channel is ever asked for on an NVMe drive that isn't Jam OS's by our check. */
    o->read_only = d->bus == DISKS_BUS_NVME && !disk_jamos_gpt(d);
    o->block_size = d->block_size;
    o->blocks = d->blocks;
    o->partitions = d->partitions;
    if (d->model[0]) {
        o->model_len = put_str(o->model, sizeof(o->model), d->model);
    } else {
        char both[sizeof(d->vendor) + sizeof(d->product)];
        snprintf(both, sizeof(both), "%s%s%s", d->vendor, d->vendor[0] ? " " : "", d->product);
        o->model_len = put_str(o->model, sizeof(o->model), both);
    }
    o->serial_len = put_str(o->serial, sizeof(o->serial), d->serial);
    o->firmware_len = put_str(o->firmware, sizeof(o->firmware), d->firmware);
    return OK;
}

/* What became of partition i's filesystem service; *mount its path when mounted. */
static uint8_t part_fs(const struct disk *d, unsigned i, const char **mount)
{
    uint16_t bit = (uint16_t)(1u << i);
    if (d->fs[i]) {
        const struct vol *b = &vols[d->fs[i] - 1];
        bool up = b->other ? b->ready : b->state == VOLUMES_SUP_RUNNING;
        if (up && (!b->other || b->client)) {
            *mount = fs_mount_path(b);
            return VOLDISKS_FS_MOUNTED;
        }
        return VOLDISKS_FS_WAITING;
    }
    if (d->nofat & bit)
        return VOLDISKS_FS_NO_FAT;
    if (d->badsize & bit)
        return VOLDISKS_FS_BLOCK_SIZE;
    return d->want & bit ? VOLDISKS_FS_WAITING : VOLDISKS_FS_NONE;
}

status_t list_part(uint32_t id, uint32_t index, struct list_part *o)
{
    const struct disk *d = disk_named(id);
    if (!d)
        return ERR_NOT_FOUND;
    if (!disk_ready(d) || index >= d->nparts || index >= MAX_PARTS)
        return ERR_OUT_OF_RANGE;
    const struct part_info *p = &d->info[index];
    const char *mount = "";
    *o = (struct list_part){ .mbr_type = d->type[index], .start = p->start, .blocks = p->blocks };
    memcpy(o->type_guid, p->type_guid, sizeof(o->type_guid));
    o->fs = part_fs(d, index, &mount);
    o->fstype = d->fstype[index];
    o->label_len = put_str(o->label, sizeof(o->label), p->label);
    o->mount_len = put_str(o->mount, sizeof(o->mount), mount);
    return OK;
}

/* ---- digest and health: asked of the driver, in a task ------------------------------- */

/* d's driver, ready for a question: its channel, and the task to wait in. */
static status_t asked_disk(uint32_t id, struct disk **out)
{
    struct disk *d = disk_named(id);
    if (!d)
        return ERR_NOT_FOUND;
    if (!d->ch || d->state == DISK_DOWN || !in_task())
        return ERR_BAD_STATE;
    *out = d;
    return OK;
}

status_t disk_digest(uint32_t id, uint64_t lba, uint64_t count, uint8_t sha256[32])
{
    struct disk *d;
    status_t st = asked_disk(id, &d);
    if (st != OK)
        return st;
    struct call c;
    call_begin(&c, d->ch);
    st = storage_digest_send(d->ch, c.txid, lba, count);
    if (st != OK) {
        call_end(&c);
        return st;
    }
    st = call_wait(&c, now() + DIGEST_WAIT);
    return st == OK ? storage_digest_result(c.rep, &c.m, sha256) : st;
}

status_t disk_health(uint32_t id, struct list_health *o)
{
    struct disk *d;
    status_t st = asked_disk(id, &d);
    if (st != OK)
        return st;
    struct call c;
    call_begin(&c, d->ch);
    st = storage_health_send(d->ch, c.txid);
    if (st != OK) {
        call_end(&c);
        return st;
    }
    st = call_wait(&c, now() + HEALTH_WAIT);
    if (st != OK)
        return st;
    return storage_health_result(c.rep, &c.m, &o->units_read, &o->units_written, &o->host_reads,
                                 &o->host_writes, &o->power_cycles, &o->unsafe_shutdowns,
                                 &o->percent_used, &o->critical);
}
