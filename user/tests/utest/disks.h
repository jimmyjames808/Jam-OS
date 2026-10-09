/* utest: what the volumes tests share (disks.c): the mock disk handed to
 * volumes (diskmock.h, volumes.test_disk), and a watch of volumes' mounts
 * (volumes.watch, the `mounts` events) kept as a table. disks.c has the
 * tests of the disks and their mounts, volwatch.c those of the watch, of
 * releasing a disk and of a mount's channel made again. */
#pragma once

#include <fs_idl.h>
#include <os.h>
#include <volumes.h>
#include "diskmock.h"
#include "fattest.h"

#define ESP  "/esp-test"
#define DATA "/data-test"
#define VIEW_MAX   16                  /* mounts a view keeps */
#define WAIT_MOUNT (15 * NS_PER_S)     /* the most a test waits for a mount to come or go */
#define PART_ESP   0u                  /* the mock's partitions */
#define PART_DATA  1u

/* What a test puts on the mock's ESP. */
enum esp_kind {
    ESP_BOOT,    /* a FAT volume with boot/jamos.elf: the boot disk */
    ESP_OTHER,   /* a FAT volume without it: someone else's stick */
    ESP_BLANK,   /* no volume at all */
};

/* The mounts as a watch has told them (every mount, the test disks' too). */
struct view {
    handle_t watch;        /* our end of the watch (0: not open yet) */
    uint32_t generation;   /* of the last `settled` (0: none yet) */
    unsigned events;       /* events read so far */
    bool     settled;      /* the last event read was `settled` */
    unsigned n;
    struct {
        char     path[16];
        uint8_t  flags;    /* MOUNTS_* */
        uint32_t gen;      /* the generation it was mounted at */
        handle_t fs;       /* its channel (the view's) */
    } m[VIEW_MAX];
};

/* The mock disk of the test running. */
extern struct diskmock mock;

/* volumes' control channel (/svc/volumes, one of our own), or 0 (said). */
handle_t volumes(void);
/* Open v's watch on vm (once) and read up to the next `settled`:
 * ERR_TIMED_OUT if none comes by deadline, ERR_PEER_CLOSED if volumes
 * dropped the watch. */
status_t view_next(handle_t vm, struct view *v, uint64_t deadline);
/* Every handle of v closed. */
void     view_close(struct view *v);
/* v's mount `path` as fattest.h's calls take it (fs 0: not mounted). */
struct fatrun mount_of(const struct view *v, const char *path);
bool     mounted(const struct view *v, const char *path);
/* The generation the mount `path` came at (0: not mounted). */
uint32_t mount_gen(const struct view *v, const char *path);
/* Follow v until the test disk's /esp-test and /data-test are as wanted. */
bool     wait_mounts(handle_t vm, struct view *v, bool want_esp, bool want_data);
/* Follow v until the test disk's /usbN-test is there and answers statfs
 * with read_only == want (0 or 1), or, want < 0, is gone. */
bool     wait_usb(handle_t vm, struct view *v, int want);
/* A test disk's /usbN-test mount in v (the first), or NULL. */
const char *usb_path(const struct view *v);
/* No change comes on v for a second. */
bool     view_quiet(const struct view *v);
/* Make a mock disk (partition 2 of type type2, the ESP as kind says) and
 * hand it to volumes; *id: its id. False with *skip when bootfs has no
 * fat. */
bool     attach(handle_t vm, uint8_t type2, enum esp_kind kind, uint32_t *id, bool *skip);
/* The mock is pulled, and volumes has dropped its mounts (v closed). */
bool     detach(handle_t vm, struct view *v);
/* A mock counter reaches want within 15 s. */
bool     wait_count(const uint32_t *counter, uint32_t want);
/* volumes.remount of path, read-write or not. */
status_t remount(handle_t vm, const char *path, bool writable);
