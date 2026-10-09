/* fat's additions to FatFs (ffx.c): what fat knows a file by, the place of
 * its directory entry. */
#pragma once

#include <stdint.h>
#include <ff.h>

/* An entry's place as one number: the sector that holds it, times 16, plus
 * its slot in that sector (32-byte entries in a 512-byte sector). Never 0
 * or 1 (sector 0 holds the boot sector, no entry): fat answers 1 for the
 * root, which has no entry. */
uint64_t ffx_place(LBA_t sect, UINT ofs);
/* f_stat, and the place of the entry it read (the root: FR_INVALID_NAME). */
FRESULT  ffx_stat(const TCHAR *path, FILINFO *fno, uint64_t *place);
/* f_readdir for the next entry (fno->fname "" at the end), and its place. */
FRESULT  ffx_readdir(DIR *dp, FILINFO *fno, uint64_t *place);
/* The modification date and time in the entry at `place`. */
FRESULT  ffx_entry_time(FATFS *fs, uint64_t place, WORD *date, WORD *time);
