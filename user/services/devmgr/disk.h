/* devmgr: what disk.c (the disks and the steps) and fsvc.c (their
 * filesystem services) share. internal.h has the rest of devmgr. */
#pragma once

#include "internal.h"

#define MAX_DISKS      8
#define MAX_PARTS      4                   /* an MBR's primary partitions */
#define MAX_USB_MOUNTS 4                   /* /usb0 .. /usb3 (and as many /usbN-test) */
#define FAT_PATH       "bin/fat"
#define BOOT_FILE      "/boot/jamos.elf"   /* on the ESP: what makes it the boot disk */
#define TYPE_ESP       0xef                /* MBR partition types */
#define TYPE_FAT32_LBA 0x0c
#define INFO_WAIT      (30 * NS_PER_S)     /* storage.info: the stick may be spinning up */
#define ESP_WAIT       (10 * NS_PER_S)     /* the ESP's service: start, mount, one stat */
#define CALL_WAIT      (2 * NS_PER_S)      /* a call to a driver that has answered info */
#define SYNC_WAIT      (5 * NS_PER_S)      /* fs.sync before devmgr stops */
#define TEST_ID        0x80000000u         /* a test disk's id: this | a counter */
/* A Jam OS disk with another MBR disk id than the boot's waits for the
 * one it booted from until HELD_FROM_START after devmgr started, and at
 * least HELD_MIN after it came; then it may be the boot disk after all
 * (that stick was swapped for this one, or its id changed). */
#define HELD_FROM_START (10 * NS_PER_S)
#define HELD_MIN        (3 * NS_PER_S)

enum disk_state {
    DISK_FREE,    /* an unused slot */
    DISK_DOWN,    /* its driver isn't running (its restart is due) */
    DISK_INFO,    /* storage.info asked, no answer yet */
    DISK_HELD,    /* a Jam OS disk, but not the one the machine booted from (its MBR
                   * disk id): it waits a while for that one to come */
    DISK_ESP,     /* the ESP's service asked for BOOT_FILE, no answer yet */
    DISK_BOOT,    /* the boot disk: its running services are mounts */
    DISK_OTHER,   /* not the boot disk: its FAT partitions' services are mounts (/usbN) */
    DISK_ALONE,   /* it didn't answer: left alone */
};

struct disk {
    enum disk_state state;      /* where it is in the steps above */
    bool            test;       /* from DEVMGR_TEST_DISK */
    uint32_t        id;         /* usb-bus's device id, or TEST_ID | n */
    uint32_t        bind;       /* devs index + 1 of its driver; 0: a test disk */
    handle_t        ch;         /* a test disk: our end of its `storage` channel */
    uint16_t        gen;        /* bumped at every use of the slot: in its port key */
    uint32_t        txid;       /* DISK_INFO, DISK_ESP: the request not answered yet */
    uint64_t        deadline;   /* DISK_INFO, DISK_ESP: when it is given up on; DISK_HELD:
                                 * when it is looked at anyway */
    uint32_t        mbr_id;     /* its MBR disk id (storage.disk_id), 0: none or unknown */
    uint8_t         nparts;     /* partitions it lists (at most MAX_PARTS are looked at) */
    uint8_t         type[MAX_PARTS];   /* their MBR types */
    uint32_t        fs[MAX_PARTS];     /* devs index + 1 of each partition's service; 0: none */
    uint8_t         want;       /* DISK_OTHER: bit n: partition n waits for its service */
};

extern struct disk disks[MAX_DISKS];
extern uint32_t txids;   /* the last transaction id written without waiting */

/* disk.c */
handle_t    disk_ch(const struct disk *d);        /* its `storage` channel */
const char *disk_name(const struct disk *d);      /* "disk 7", "test disk 2" (static buffer) */
struct disk *disk_of(const struct binding *b);    /* b's disk, or NULL */
/* The next message queued on h into buf (cap bytes): OK and *n, or
 * jam_channel_read's status once nothing is left. */
status_t    next_msg(handle_t h, void *buf, uint32_t cap, uint32_t *n);
void        leave_alone(struct disk *d, const char *why);   /* its driver doesn't answer */
void        not_boot(struct disk *d, const char *why);      /* step 4 */

/* fsvc.c */
status_t    fs_start(struct disk *d, unsigned part, bool other);
/* d moves to `state` (one with nothing mounted) and its services go. */
void        drop_services(struct disk *d, enum disk_state state);
void        others_pump(void);                    /* start the next /usbN waited for */
void        mount_others(struct disk *d);         /* step 4: d's FAT partitions, read-only */
/* An fs.stat of `path` written to b's service without waiting (fs_answers
 * takes the answer); its transaction id. */
uint32_t    ask_stat(const struct binding *b, const char *path, status_t *st);
