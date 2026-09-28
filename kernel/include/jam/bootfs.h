/* bootfs: a read-only image of files (init, test programs, init.cfg)
 * loaded by Limine as a module, so userspace starts before USB and FAT32
 * (Track C owns the implementation; tools/mkbootfs.py builds the image).
 *
 * Image format (little-endian): a header, `count` entries, then file data,
 * each file starting on a 4 KiB boundary. */
#pragma once

#include <stdint.h>
#include <jam/status.h>

#define BOOTFS_MAGIC      "JAMBOOTF"
#define BOOTFS_VERSION    1
#define BOOTFS_NAME_MAX   56
#define BOOTFS_MODULE     "bootfs.img"   /* Limine module path suffix */

struct bootfs_header {
    char     magic[8];
    uint32_t version;
    uint32_t count;
    uint64_t size;       /* whole image, bytes */
};

struct bootfs_entry {
    char     name[BOOTFS_NAME_MAX];   /* NUL-terminated, e.g. "bin/init" */
    uint64_t offset;                  /* from the image start, 4 KiB aligned */
    uint64_t size;
};

struct boot_info;
struct vmo;

/* Find and validate the module; logs and leaves bootfs empty if absent or
 * malformed. */
void     bootfs_init(const struct boot_info *bi);
/* A read-only VMO over the file's pages (the caller gets a reference) and
 * its size in bytes. ERR_NOT_FOUND if there is no such file. */
status_t bootfs_find(const char *name, struct vmo **out, uint64_t *size);
/* The file's bytes, read-only, in kernel memory (for the ELF parser). */
status_t bootfs_data(const char *name, const void **data, uint64_t *size);
unsigned bootfs_count(void);
/* Name of entry i (i < bootfs_count()). */
const char *bootfs_name(unsigned i);

/* A read-only VMO over the whole image (the SR_BOOTFS handle of the
 * startup message) and the image size in bytes. ERR_NOT_FOUND if there
 * is no bootfs. */
status_t bootfs_image(struct vmo **out, uint64_t *size);

/* Check an image in memory (img[0..len), untrusted): header, every entry
 * (NUL-terminated sane names, no duplicates, page-aligned data inside the
 * image, no two files sharing a page). OK, or ERR_INVALID_ARGS with *why
 * saying what is wrong. Never reads outside img[0..len). bootfs_init uses
 * it on the module; tests use it on corrupted copies. */
#define BOOTFS_MAX_FILES 256
status_t bootfs_validate(const void *img, uint64_t len, const char **why);
