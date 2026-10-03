/* FatFs's configuration for the fat service: Jam OS's copy of FatFs
 * R0.16's source/ffconf.h (third_party/fatfs), with its option names and
 * revision id, and Jam OS's values. The Makefile compiles FatFs from a
 * copy of its sources in $(BUILD)/fatfs, so this file, not the vendored
 * one, is the "ffconf.h" ff.h finds. What each option means is in the
 * vendored file.
 *
 * The choices:
 * - read-write with f_mkfs (a blank data partition is formatted), volume
 *   labels (statfs, and "JAMOS-DATA" after a format);
 * - long file names with a static working buffer (fat is single-threaded)
 *   and a UTF-8 API; code page 437 for the short aliases;
 * - no relative paths: fat resolves "." and ".." itself before FatFs sees
 *   a path (path.c);
 * - one volume, 512-byte sectors only (every stick seen so far), 32-bit
 *   LBAs, no exFAT, no TRIM;
 * - timestamps from get_fattime (disk.c);
 * - FAT32's FSInfo free-cluster count is not trusted (a volume that was
 *   not shut down cleanly has a stale one, and there is no fsck): the
 *   first statfs after a mount counts the FAT instead;
 * - FatFs's file lock off (FF_FS_LOCK 0): its table of open objects is
 *   state FatFs keeps on its own, outside the structs fat keeps in its
 *   state VMO (docs/M11.6-PLAN.md, "FatFs without re-mounting"). fat
 *   applies the same rule itself, by directory entry (fileops.c): a file
 *   open for writing can't be opened again, and an open file can't be
 *   removed or renamed;
 * - no re-entrancy (one thread). */
#pragma once

#define FFCONF_DEF 80386   /* FatFs R0.16's revision id */

/* functions */
#define FF_FS_READONLY  0
#define FF_FS_MINIMIZE  0
#define FF_USE_FIND     0
#define FF_USE_MKFS     1
#define FF_USE_FASTSEEK 0
#define FF_USE_EXPAND   0
#define FF_USE_CHMOD    0
#define FF_USE_LABEL    1
#define FF_USE_FORWARD  0
#define FF_USE_STRFUNC  0
#define FF_PRINT_LLI    0
#define FF_PRINT_FLOAT  0
#define FF_STRF_ENCODE  0

/* locale and names */
#define FF_CODE_PAGE   437
#define FF_USE_LFN     1
#define FF_MAX_LFN     255
#define FF_LFN_UNICODE 2     /* UTF-8 */
#define FF_LFN_BUF     255
#define FF_SFN_BUF     12
#define FF_FS_RPATH    0
#define FF_PATH_DEPTH  10

/* volumes */
#define FF_VOLUMES         1
#define FF_STR_VOLUME_ID   0
#define FF_VOLUME_STRS     "RAM"
#define FF_MULTI_PARTITION 0
#define FF_MIN_SS          512
#define FF_MAX_SS          512
#define FF_LBA64           0
#define FF_MIN_GPT         0x10000000
#define FF_USE_TRIM        0

/* system */
#define FF_FS_TINY     0
#define FF_FS_EXFAT    0
#define FF_FS_NORTC    0
#define FF_NORTC_MON   1
#define FF_NORTC_MDAY  1
#define FF_NORTC_YEAR  2026
#define FF_FS_CRTIME   0
#define FF_FS_NOFSINFO 1     /* count the free clusters, don't trust FSInfo's number */
#define FF_FS_LOCK     0     /* fat's own rule instead (fileops.c) */
#define FF_FS_REENTRANT 0
#define FF_FS_TIMEOUT  1000
