/* The partition table library: a disk's MBR or GPT, read into the
 * partition records the `storage` protocol serves (abi/idl/storage.idl).
 * Its code, drivers/lib/parttable.c and drivers/lib/parttable_gpt.c, is
 * linked into a disk driver's own object (the Makefile's DRV_LIB_<name>),
 * and built for the Mac by tools/gptcheck.c, which runs it against
 * tools/mkgpt.py's images and thousands of mutated tables.
 *
 * Pure code: no I/O of its own, no allocation, no logging. The caller
 * passes a read function and a buffer it reads into; the library asks
 * only for blocks inside the disk, at most the buffer's size at a time,
 * and never writes. What it read and left out is in the table it fills
 * (the reasons a GPT header was not taken, notes on left-out entries), for
 * the driver to log.
 *
 * The table decides what a driver serves: every partition listed lies
 * inside the disk, past block 0 (except the one partition of a disk with
 * no table whose block 0 is a FAT boot sector, which is the whole disk),
 * inside GPT's usable range, and overlaps no other listed partition. A
 * hostile table (a stick, a drive from anywhere) can make the library
 * list fewer partitions, never one outside those bounds: every field is
 * read with its bounds checked first, every loop is bounded by a limit
 * here, and nothing is computed that can wrap.
 *
 * What is read, in this order (at most block 0, 2 x 64 KiB for the blank
 * rule, two headers and 2 x PT_ARRAY_MAX of entry arrays; most disks far
 * less):
 *   - block 0: the MBR. Signature 55 AA, and an entry of type EEh in use:
 *     a protective MBR, so GPT is read (an EEh entry beside others is a
 *     "hybrid" MBR: read as GPT, and flagged; the status bytes are
 *     ignored, as UEFI does, unless block 0 is also a FAT boot sector and
 *     no valid table: that is a FAT volume). Otherwise the MBR rules of
 *     usb-storage: a table is looked for before a FAT boot sector; the used
 *     entries in table order, each inside the disk, past block 0, and not
 *     overlapping an earlier one (left out otherwise).
 *   - if block 0 is all zeros: the first and last 64 KiB, for the blank
 *     rule, and if they aren't all zeros, whether the only non-zero blocks
 *     are the backup entry array create_table writes first (pt_jamos_plan's
 *     place, pt_jamos_array_head_is's bytes: the cut after that write), and
 *     if not, the GPT too (only to see whether the disk is
 *     blank-in-progress: `blank_in_progress` below; a GPT without a
 *     protective MBR is never listed, as UEFI ignores it).
 *   - GPT (UEFI 2.10, 5.3): the primary header (block 1), its entry array,
 *     the backup header (the last block), its entry array. Each header is
 *     checked alone (signature, revision 1.0, header size 92 up to the
 *     block size, its CRC32, the reserved field zero, MyLBA and
 *     AlternateLBA, the usable range inside the disk and clear of both
 *     headers and its entry array, the entry array inside the disk, 128 to
 *     1024 entries of 128 bytes times a power of two and at most
 *     PT_ARRAY_MAX bytes, the array's CRC32). Both good: they must agree
 *     (disk GUID, usable range, entry count and size, array CRC), or the
 *     backup counts as bad (PT_BAD_MISMATCH) and the primary is used. One
 *     good: it is used. Neither: scheme GPT with no partitions.
 *   - GPT entries: a zero type GUID is unused; a used one needs
 *     FirstUsableLBA <= start <= end <= LastUsableLBA and no overlap with
 *     an earlier listed one; past PT_MAX_PARTS listed, the rest are noted
 *     and not listed. Names: 36 UTF-16LE units to UTF-8, stopping at the
 *     first zero; an unpaired surrogate, and any control character, is '?'.
 *
 * GUIDs are kept as their 16 bytes on disk (the first three fields
 * little-endian); pt_guid_str makes the printed form. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/status.h>

#define PT_MAX_PARTS    16                 /* partitions listed at most */
#define PT_NAME_MAX     108                /* UTF-8 bytes of a GPT name (storage's str[<=108]) */
#define PT_MAX_NOTES    16                 /* left-out entries remembered (all are counted) */
#define PT_ARRAY_MAX    (128u * 1024)      /* bytes of a GPT entry array: 1024 entries of 128 */
#define PT_ENTRIES_MIN  128                /* a GPT entry array's entry count, at least */
#define PT_ENTRIES_MAX  1024               /* and at most */
#define PT_BLANK_BYTES  (64u * 1024)       /* the blank rule's zeros at each end of the disk */
#define PT_WHOLE        0x00               /* the mbr_type of a disk with no table, served whole */

/* The schemes, equal to storage.idl's SCHEME_* (info's `scheme`). */
enum pt_scheme {
    PT_SCHEME_NONE = 0,   /* no table: no partition, or one that is the whole disk */
    PT_SCHEME_MBR  = 1,
    PT_SCHEME_GPT  = 2,
};

/* Why a GPT header (with its entry array) was not taken. */
enum pt_bad {
    PT_BAD_NONE = 0,      /* good: the header and its array passed every check */
    PT_BAD_ABSENT,        /* not read: block 0 is no protective MBR */
    PT_BAD_IO,            /* a read failed */
    PT_BAD_ZERO,          /* the header's block is all zeros */
    PT_BAD_SIGNATURE,     /* not "EFI PART" */
    PT_BAD_REVISION,      /* not 1.0 */
    PT_BAD_HEADER_SIZE,   /* under 92 or over the block size */
    PT_BAD_CRC,           /* the header's CRC32 */
    PT_BAD_RESERVED,      /* the reserved field isn't zero */
    PT_BAD_MY_LBA,        /* MyLBA isn't where the header was read */
    PT_BAD_ALT_LBA,       /* AlternateLBA isn't the other header's block */
    PT_BAD_USABLE,        /* the usable range: empty, outside the disk, or over a header */
    PT_BAD_ENTRIES,       /* the entry count or size */
    PT_BAD_ARRAY,         /* the entry array: outside the disk, over a header or the usable range */
    PT_BAD_ARRAY_CRC,     /* the entry array's CRC32 */
    PT_BAD_MISMATCH,      /* good alone, but disagrees with the primary (backup only) */
    PT_BAD_COUNT          /* not a reason: the number of them */
};

/* Why a used entry was left out (struct pt_note). */
enum pt_note_why {
    PT_NOTE_OUTSIDE = 1,  /* not inside the disk (MBR) or the usable range (GPT) */
    PT_NOTE_OVERLAP,      /* overlaps an earlier listed partition */
    PT_NOTE_TOO_MANY,     /* PT_MAX_PARTS were listed already */
};

struct pt_note {
    uint16_t entry;       /* the entry's index in its table (0-based) */
    uint8_t  why;         /* enum pt_note_why */
};

/* One listed partition: storage.partition's record. */
struct pt_part {
    uint64_t start;                   /* first block */
    uint64_t blocks;                  /* length in blocks, at least 1 */
    uint64_t attributes;              /* GPT attribute bits (0 on MBR) */
    uint8_t  type_guid[16];           /* GPT type (zeros on MBR) */
    uint8_t  unique_guid[16];         /* GPT unique GUID (zeros on MBR) */
    uint16_t entry;                   /* its index in the table (0-based) */
    uint8_t  mbr_type;                /* MBR type byte (0 on GPT; PT_WHOLE: no table) */
    char     name[PT_NAME_MAX + 1];   /* GPT name, UTF-8, NUL-terminated ("" on MBR) */
};

/* What pt_read found. Every field is set, whatever the disk holds. */
struct pt_table {
    uint8_t  scheme;              /* enum pt_scheme */
    uint8_t  primary;             /* enum pt_bad: the primary GPT header, PT_BAD_NONE if good */
    uint8_t  backup;              /* enum pt_bad: the backup GPT header */
    bool     hybrid;              /* GPT behind an MBR that lists more than its EEh entries */
    bool     blank;               /* block 0 and the first and last 64 KiB are all zeros */
    bool     blank_in_progress;   /* block 0 zero, and either each header block zero or a
                                   * good header of Jam OS's layout (one at least), or
                                   * (the cut after create_table's first write) both
                                   * header blocks zero and the backup entry array
                                   * exactly what pt_jamos_make writes there, nothing
                                   * else in the blank rule's ends: a create_table that
                                   * was cut before its last write */
    uint32_t mbr_id;              /* the MBR's disk id (block 0, 440), MBR scheme only; else 0 */
    uint8_t  disk_guid[16];       /* the GPT disk GUID (zeros: not GPT) */
    uint64_t first_usable;        /* the GPT usable range (0, 0: not GPT) */
    uint64_t last_usable;
    uint32_t entry_count;         /* the GPT entry array's entries and their size */
    uint32_t entry_size;
    uint32_t used;                /* used entries in the table, listed or not */
    uint32_t nparts;              /* entries of parts[] */
    struct pt_part parts[PT_MAX_PARTS];   /* in table order */
    uint32_t notes_total;         /* used entries left out */
    uint32_t nnotes;              /* entries of notes[]: the first PT_MAX_NOTES of them */
    struct pt_note notes[PT_MAX_NOTES];
};

/* Read `count` blocks from `lba` into `buf`. Returns OK, or an error
 * (ERR_IO...), which pt_read passes on for block 0 and counts as a bad
 * header elsewhere. Called only with lba + count <= the disk's blocks and
 * count * block_size <= buf_size. */
typedef status_t (*pt_read_fn)(void *ctx, uint64_t lba, uint32_t count, void *buf);

/* The disk to read. */
struct pt_disk {
    uint32_t   block_size;   /* 512 to 4096, a power of two */
    uint64_t   blocks;       /* the disk's size in blocks, at least 1 */
    pt_read_fn read;
    void      *ctx;          /* read's first argument */
    void      *buf;          /* the caller's buffer, which read fills */
    uint32_t   buf_size;     /* its size: one block at least; 64 KiB reads the
                              * entry arrays in few calls */
};

/* Fill *t from the disk's partition table (above for the rules). Returns
 * OK with *t set (no table at all is OK: scheme NONE, nparts 0);
 * ERR_INVALID_ARGS if *d breaks its rules (*t then lists nothing); or
 * the read function's error if block 0 can't be read (*t lists nothing).
 * Reads only; never writes anything but *t and d->buf. */
status_t pt_read(const struct pt_disk *d, struct pt_table *t);

/* Jam OS's mark (docs/M12.7-PLAN.md, stage 2): GPT with both headers good
 * and agreeing, no hybrid MBR, and exactly two used entries, listed as
 * partitions 0 and 1: the ESP's type named PT_JAMOS_ESP_NAME, then Jam OS's
 * data type named PT_JAMOS_DATA_NAME. Anything else is someone else's disk. */
bool pt_is_jamos(const struct pt_table *t);

/* A disk's kind, from its table: who may write it. */
enum pt_kind {
    PT_KIND_OTHER = 0,    /* someone else's: never written */
    PT_KIND_BLANK,        /* blank or blank-in-progress: create_table may write it */
    PT_KIND_JAMOS,        /* Jam OS's (pt_is_jamos): its two partitions may be written */
};
enum pt_kind pt_kind(const struct pt_table *t);

#define PT_JAMOS_ESP_NAME  "Jam OS ESP"
#define PT_JAMOS_DATA_NAME "Jam OS data"

/* ---- Jam OS's table, made (drivers/lib/parttable_make.c) ------------------------------
 *
 * The bytes storage.create_table writes onto a blank disk (the driver
 * writes them; this only fills a buffer): a protective MBR, both GPT
 * headers and one entry array of PT_JAMOS_ENTRIES entries, holding
 * exactly the layout pt_is_jamos accepts. The ESP from PT_JAMOS_ALIGN,
 * PT_JAMOS_ESP_BYTES long; the data partition from its end to the last
 * usable block, at most PT_JAMOS_DATA_MAX, ending on a multiple of
 * PT_JAMOS_ALIGN. Every block the table takes lies inside the blank
 * rule's zeros (the first and last PT_BLANK_BYTES), so preparing a blank
 * disk overwrites only blocks the blank rule saw to be zeros. */

#define PT_JAMOS_ENTRIES     128                    /* entries of 128 bytes: a 16 KiB array */
#define PT_JAMOS_ARRAY_BYTES (PT_JAMOS_ENTRIES * 128u)
#define PT_JAMOS_ALIGN       (1ull << 20)           /* the ESP's start; the data partition's end */
#define PT_JAMOS_ESP_BYTES   (512ull << 20)
#define PT_JAMOS_DATA_MIN    (64ull << 20)          /* a smaller disk is refused */
#define PT_JAMOS_DATA_MAX    (2ull << 40)           /* FAT32's 32-bit sector numbers */

/* Where Jam OS's table and partitions go on a disk. */
struct pt_jamos_plan {
    uint32_t array_blocks;        /* an entry array's blocks */
    uint64_t head_blocks;         /* blocks [0, head_blocks): MBR, primary header and array */
    uint64_t tail_start;          /* blocks [tail_start, the end): backup array, backup header */
    uint64_t esp_start, esp_blocks;
    uint64_t data_start, data_blocks;
};

/* The plan for a disk of `blocks` blocks of `block_size` bytes. OK;
 * ERR_INVALID_ARGS: a block size that isn't 512 to 4096 and a power of
 * two; ERR_NO_SPACE: the data partition would be under PT_JAMOS_DATA_MIN. */
status_t pt_jamos_plan(uint32_t block_size, uint64_t blocks, struct pt_jamos_plan *out);

/* The buffer pt_jamos_make fills, in blocks: block PT_JAMOS_AT_MBR the
 * protective MBR, PT_JAMOS_AT_PRIMARY the primary header,
 * PT_JAMOS_AT_ARRAY on the entry array (array_blocks), then the backup
 * header (pt_jamos_at_backup). */
#define PT_JAMOS_AT_MBR     0u
#define PT_JAMOS_AT_PRIMARY 1u
#define PT_JAMOS_AT_ARRAY   2u
static inline uint32_t pt_jamos_at_backup(const struct pt_jamos_plan *p)
{
    return PT_JAMOS_AT_ARRAY + p->array_blocks;
}
static inline uint32_t pt_jamos_buf_blocks(const struct pt_jamos_plan *p)
{
    return pt_jamos_at_backup(p) + 1;
}

/* Fill buf (buf_size bytes, pt_jamos_buf_blocks blocks at least) with
 * Jam OS's table for the disk, its disk GUID and the two partitions'
 * unique GUIDs as given (16 bytes each, as on disk). OK;
 * ERR_INVALID_ARGS: a GUID all zeros, two the same, a buffer too small,
 * or pt_jamos_plan's errors. Writes nothing but buf. */
status_t pt_jamos_make(uint32_t block_size, uint64_t blocks, const uint8_t *disk_guid,
                       const uint8_t *esp_guid, const uint8_t *data_guid, uint8_t *buf,
                       size_t buf_size);

/* The cut between create_table's first two writes leaves the backup entry
 * array alone on the disk (the backup header is the second write). Does
 * `head`, the first PT_JAMOS_HEAD_BYTES of an entry array read from the
 * place pt_jamos_plan gives it on a disk of `blocks` blocks, hold exactly
 * the two entries pt_jamos_make writes there? The two unique GUIDs are
 * taken from the bytes themselves (they were random; the maker only needs
 * them non-zero and different), everything else is made the way the maker
 * makes it and compared byte for byte. The rest of an entry array is zeros
 * (the maker writes no more than two entries): the caller checks that. */
#define PT_JAMOS_HEAD_BYTES 256
bool pt_jamos_array_head_is(uint32_t block_size, uint64_t blocks, const uint8_t *head);

/* Well-known type GUIDs, as their bytes on disk. */
extern const uint8_t pt_type_esp[16];          /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
extern const uint8_t pt_type_msr[16];          /* E3C9E316-0B5C-4DB8-817D-F92DF00215AE */
extern const uint8_t pt_type_basic_data[16];   /* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */
extern const uint8_t pt_type_recovery[16];     /* DE94BBA4-06D1-4D40-A16A-BFD50179D6AC */
extern const uint8_t pt_type_jamos_data[16];   /* 71BAE565-D4A8-4315-8C81-9B35462CC158 */

/* Are the two GUIDs (16 bytes each) the same? Is g all zeros? */
bool pt_guid_eq(const uint8_t *a, const uint8_t *b);
bool pt_guid_zero(const uint8_t *g);
/* g's printed form, "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", and a NUL
 * (37 bytes). */
void pt_guid_str(const uint8_t *g, char *out);
/* A reason in a few words ("header CRC"), for the log. */
const char *pt_bad_str(uint8_t why);
const char *pt_note_str(uint8_t why);

/* CRC32 as GPT (and zlib) computes it: start with crc 0, feed the bytes
 * in order, each call's result into the next. */
uint32_t pt_crc32(uint32_t crc, const void *p, size_t n);
