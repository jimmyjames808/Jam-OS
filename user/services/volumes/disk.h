/* volumes: what disk.c (the disks and the steps) and fsvc.c (their
 * filesystem services) share. internal.h has the rest of volumes. */
#pragma once

#include "internal.h"

#define MAX_DISKS      8
#define MAX_PARTS      16                  /* partitions looked at (storage.idl lists as many
                                            * as the driver's library: PT_MAX_PARTS) */
#define MAX_USB_MOUNTS 4                   /* /usb0 .. /usb3 (and as many /usbN-test) */
#define MAX_NVME_MOUNTS 4                  /* /nvme0 .. /nvme3 */
#define LABEL_MAX 108                 /* a GPT partition's name, UTF-8 (storage.idl) */
#define FAT_PATH       "bin/fat"
#define EXTFS_PATH     "bin/extfs"
#define BOOT_FILE      "/boot/jamos.elf"   /* on the ESP: what makes it the boot disk */
#define TYPE_ESP       0xef                /* MBR partition types */
#define TYPE_FAT32_LBA 0x0c
#define TYPE_LINUX     0x83                /* ... a Linux filesystem: ext, here */
#define FAT_BLOCK      512                 /* the only block size bin/fat reads */
#define INFO_WAIT      (30 * NS_PER_S)     /* storage.info: the stick may be spinning up */
#define ESP_WAIT       (10 * NS_PER_S)     /* the ESP's service: start, mount, one stat */
#define CALL_WAIT      (2 * NS_PER_S)      /* a call to a driver that has answered info */
#define SYNC_WAIT      (5 * NS_PER_S)      /* fs.sync before a disk's driver or volumes stops */
#define TEST_ID        0x80000000u         /* a test disk's id: this | a counter */
/* A Jam OS disk with another MBR disk id than the boot's waits for the
 * one it booted from until HELD_FROM_START after volumes started, and at
 * least HELD_MIN after it came; then it may be the boot disk after all
 * (that stick was swapped for this one, or its id changed). After a boot
 * from a GPT drive (named by its GUID) it is then another disk, never the
 * boot disk: disk.c's held_over. */
#define HELD_FROM_START (10 * NS_PER_S)
#define HELD_MIN        (3 * NS_PER_S)

enum disk_state {
    DISK_FREE,    /* an unused slot */
    DISK_DOWN,    /* its driver isn't running (its restart is due): its `storage` channel
                   * closed, and the disk waits for devmgr's next ATTACH or its GONE */
    DISK_INFO,    /* storage.info asked, no answer yet */
    DISK_PARTS,   /* storage.partition asked for partition part_next, no answer yet */
    DISK_IDENT,   /* an NVMe drive: storage.ident asked, no answer yet (listing.c) */
    DISK_HELD,    /* a Jam OS disk, but not the one the machine booted from (its MBR
                   * disk id): it waits a while for that one to come */
    DISK_ESP,     /* the ESP's service asked for BOOT_FILE, no answer yet */
    DISK_BOOT,    /* the boot disk: its running services are mounts */
    DISK_OTHER,   /* not the boot disk: its FAT partitions' services are mounts (/usbN) */
    DISK_ALONE,   /* it didn't answer: left alone */
};

/* What storage.partition said of one partition (the listing's, listing.c). */
struct part_info {
    uint8_t  type_guid[16];     /* GPT type, on-disk bytes (zeros on MBR) */
    uint64_t start, blocks;
    char     label[LABEL_MAX + 1];   /* GPT name, UTF-8 */
};

struct disk {
    enum disk_state state;      /* where it is in the steps above */
    bool            test;       /* from VOLUMES_TEST_DISK */
    uint8_t         bus;        /* DISKS_BUS_* (0: a test disk) */
    bool            known;      /* the steps got its info and partitions (the listing's `ready`) */
    uint8_t         number;     /* its N in "nvme0" / "usb0": the lowest free one of its bus */
    uint32_t        id;         /* usb-bus's device id (DISKS_ATTACH's), or TEST_ID | n */
    handle_t        ch;         /* our end of its `storage` channel (0: DISK_DOWN) */
    uint16_t        gen;        /* bumped at every use of the slot: in its port key */
    uint32_t        txid;       /* DISK_INFO, DISK_PARTS, DISK_ESP: the request not answered
                                 * yet */
    uint64_t        deadline;   /* DISK_INFO, DISK_PARTS, DISK_ESP: when it is given up on;
                                 * DISK_HELD: when it is looked at anyway */
    /* What storage.info said (DISK_PARTS asks the partitions one at a time). */
    char            vendor[9], product[17];   /* trimmed ASCII */
    uint32_t        block_size, partitions;
    uint64_t        blocks;
    uint8_t         part_next;  /* DISK_PARTS: the partition asked for */
    uint32_t        mbr_id;     /* its MBR disk id (storage.info), 0: none or unknown */
    uint8_t         disk_guid[16];   /* its GPT disk GUID (storage.info), zeros: none */
    uint8_t         scheme;     /* storage.idl's SCHEME_* */
    uint8_t         nparts;     /* partitions it lists (at most MAX_PARTS are looked at) */
    uint8_t         type[MAX_PARTS];   /* their MBR types */
    struct part_info info[MAX_PARTS];  /* ... and the rest of storage.partition's answers */
    uint32_t        fs[MAX_PARTS];     /* vols index + 1 of each partition's service; 0: none */
    uint8_t         fstype[MAX_PARTS]; /* VOLDISKS_FSTYPE_* of each partition, once known (the
                                        * listing's; stays when a service is retired) */
    uint64_t        mib;        /* its size (storage.info), MiB */
    uint16_t        told;       /* bit n: partition n's mount was on the desktop (a notice) */
    char            at[MAX_PARTS][16];   /* ... where it was mounted ("/usb0") */
    uint16_t        want;       /* DISK_OTHER: bit n: partition n waits for its service */
    uint16_t        nofat;      /* bit n: partition n's service found no FAT volume */
    uint16_t        badsize;    /* bit n: partition n's blocks are not 512 bytes: no service */
    uint16_t        fresh;      /* bit n: partition n may be formatted (prepare.c: a Jam OS
                                 * disk, by volumes' own check, being prepared): its service is
                                 * started writable with FAT_ARG_FORMAT until prepare is over */
    bool            preparing;  /* a prepare is under way on it (prepare.c): one at a time */
    char            model[41], serial[21], firmware[9];   /* storage.ident (NVMe); else empty */
    bool            boot_down;  /* DISK_DOWN: it was the boot disk when its driver stopped */
    bool            releasing;  /* volumes.release_disk's task stops its services: none
                                 * starts or restarts meanwhile */
};

extern struct disk disks[MAX_DISKS];
extern uint32_t txids;   /* the last transaction id written without waiting */

/* The listing's answers (listing.c), filled for request.c to reply with. */
struct list_disk {
    uint32_t id, block_size, partitions, name_len, model_len, serial_len, firmware_len;
    uint64_t blocks;
    uint8_t  bus, ready, kind, scheme, read_only;
    char     name[8], model[40], serial[20], firmware[8];
};
struct list_part {
    uint64_t start, blocks;
    uint32_t label_len, mount_len;
    uint8_t  mbr_type, fs, fstype, type_guid[16];
    char     label[LABEL_MAX], mount[15];
};
struct list_health {
    uint64_t units_read, units_written, host_reads, host_writes, power_cycles, unsafe_shutdowns;
    uint8_t  percent_used, critical;
};

/* listing.c */
struct disk *disk_named(uint32_t id);             /* the real disk called id, or NULL */
void        disk_number(struct disk *d);          /* d's N: the lowest free one of its bus */
const char *disk_label(const struct disk *d);     /* "nvme0", "usb0" (static buffer) */
/* DISK_IDENT: ask an NVMe drive's identity (true: asked, or given up on at once; false: not
 * an NVMe drive, go on). Its answer: ident_answer. */
bool        ident_ask(struct disk *d);
void        ident_answer(struct disk *d, const void *rep, struct idl_msg *m);
/* voldisks.disk / .partition: the index-th real disk, or partition `index` of disk `id`.
 * ERR_OUT_OF_RANGE past the last; ERR_NOT_FOUND: no such disk. */
status_t    list_disk(uint32_t index, struct list_disk *o);
status_t    list_part(uint32_t id, uint32_t index, struct list_part *o);
/* In a task: storage.digest / storage.health of disk id (waited for, the loop goes on).
 * ERR_NOT_FOUND: no such disk; ERR_BAD_STATE: its driver isn't there. */
status_t    disk_digest(uint32_t id, uint64_t lba, uint64_t count, uint8_t sha256[32]);
status_t    disk_health(uint32_t id, struct list_health *o);
/* A read-only `block` channel on partition `index` of disk id, in a task
 * (voldisks.open_partition, for the shell's `disk check`). */
status_t    disk_open_ro(uint32_t id, uint32_t index, handle_t *out);

/* mark.c: Jam OS's mark on a GPT disk, checked with volumes' own code (the
 * driver has its own, in the partition library). */
bool        disk_jamos_gpt(const struct disk *d);
/* prepare.c, in a task: voldisks.prepare_check (*plan VOLDISKS_PLAN_*, *refused
 * VOLDISKS_REFUSED_*), and volinstall.prepare with the owner's confirmation of an
 * erase (`tail`, `word`: looked at only when the drive isn't empty). */
status_t    disk_prepare_check(uint32_t id, uint8_t *plan, uint8_t *refused);
status_t    disk_prepare(uint32_t id, const char *tail, uint32_t tail_len, const char *word,
                         uint32_t word_len);

/* install.c, in a task: volinstall's open_esp / open_data (hold: partition `part`
 * of disk `id` made writable for init alone; *fs a channel to it) and close_esp /
 * close_data (back to read-only). */
status_t    disk_install_hold(uint32_t id, unsigned part, bool hold, handle_t *fs);
/* prepare.c, in a task: what d's driver found the drive to be (storage.mode). */
status_t    driver_mode(struct disk *d, uint8_t *mode, uint8_t *ro, uint8_t *readable);

/* disk.c */
const char *disk_name(const struct disk *d);      /* "disk 7", "test disk 2" (static buffer) */
/* Step 1 again, for a disk whose partition table has just changed (prepare.c):
 * its services go and its driver is asked what it holds. */
void        disk_relearn(struct disk *d);
struct disk *disk_of(const struct vol *b);    /* b's disk, or NULL */
void        leave_alone(struct disk *d, const char *why);   /* its driver doesn't answer */
void        not_boot(struct disk *d, const char *why);      /* step 4 */
/* A GUID's printed form (36 characters, 8-4-4-4-12, either case, as the
 * word bootdisk=gpt:<GUID> has it, and nothing after) as its bytes on disk
 * (the first three fields little-endian). False if s isn't one, or is all
 * zeros. */
bool        disk_guid_parse(const char *s, uint8_t out[16]);

/* probe.c, in a task: does the partition b serves hold an ext superblock?
 * Looked at through a read-only `block` channel of its own, closed again.
 * OK with *is_ext; else why it couldn't be looked at (*is_ext false). */
status_t    fs_probe_ext(struct vol *b, bool *is_ext);

/* fsvc.c */
/* Start the filesystem service on partition `part` of d (other: d is not
 * the boot disk, the service gets a /usbN, or /jam). The start itself runs
 * as a task (it looks at what the partition holds, and waits for a `block`
 * channel); `done` is told how it went, in that task, unless d moved on
 * meanwhile (the service was retired). An error returned here: nothing
 * started, `done` is not called. */
status_t    fs_start(struct disk *d, unsigned part, bool other,
                     void (*done)(struct disk *d, unsigned part, status_t st));
/* d moves to `state` (one with nothing mounted) and its services go. */
void        drop_services(struct disk *d, enum disk_state state);
/* b, another stick's partition d holds, is mounted now: the desktop's
 * notice, once a mount (notices). */
void        disk_told_added(struct disk *d, const struct vol *b);
/* The desktop's notice that the boot disk d went (came: false) or came
 * back after it went (came: true); nothing else is news. */
void        disk_told_boot(const struct disk *d, bool came);
void        others_pump(void);                    /* start the next /usbN waited for */
void        mount_others(struct disk *d);         /* step 4: d's FAT partitions, read-only */
/* An fs.stat of `path` written to b's service without waiting (fs_answers
 * takes the answer); its transaction id. */
uint32_t    ask_stat(const struct vol *b, const char *path, status_t *st);
