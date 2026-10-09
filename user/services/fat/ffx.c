/* fat: FatFs itself (third_party/fatfs, vendored unmodified), compiled
 * here with three functions of fat's own beside its code, which need what
 * FatFs keeps static: where a directory entry is. An entry's place (the
 * sector that holds it and its slot there) is what fat knows a file by:
 * the key of its open files and their lock (fileops.c), and the id fs.stat,
 * fs.readdir and file.stat answer. The Makefile compiles this file instead
 * of ff.c (the staged copy, with fat's ffconf.h).
 *
 * Each is a copy of the FatFs function its comment names, with the place
 * read where that function has the entry in hand. */
#include "ff.c"

#include "ffx.h"

uint64_t ffx_place(LBA_t sect, UINT ofs)
{
    return (uint64_t)sect << 4 | (ofs / SZDIRE & 15u);
}

/* f_stat, and the place of the entry it read. */
FRESULT ffx_stat(const TCHAR *path, FILINFO *fno, uint64_t *place)
{
    FRESULT res;
    DIR dj;
    DEF_NAMEBUFF

    res = mount_volume(&path, &dj.obj.fs, 0);
    if (res == FR_OK) {
        INIT_NAMEBUFF(dj.obj.fs);
        res = follow_path(&dj, path);
        if (res == FR_OK) {
            if (dj.fn[NSFLAG] & NS_NONAME) {   /* the root: it has no entry */
                res = FR_INVALID_NAME;
            } else {
                get_fileinfo(&dj, fno);
                *place = ffx_place(dj.sect, (UINT)(dj.dir - dj.obj.fs->win));
            }
        }
        FREE_NAMEBUFF();
    }
    if (res != FR_OK)
        fno->fname[0] = 0;
    LEAVE_FF(dj.obj.fs, res);
}

/* f_readdir (with an entry to fill: never a rewind), and the place of the
 * entry it read: dir_read leaves dp on it, before dir_next moves on. */
FRESULT ffx_readdir(DIR *dp, FILINFO *fno, uint64_t *place)
{
    FRESULT res;
    FATFS *fs;
    DEF_NAMEBUFF

    res = validate(&dp->obj, &fs);
    if (res == FR_OK) {
        INIT_NAMEBUFF(fs);
        fno->fname[0] = 0;
        res = DIR_READ_FILE(dp);
        if (res == FR_OK) {
            get_fileinfo(dp, fno);
            *place = ffx_place(dp->sect, (UINT)(dp->dir - fs->win));
            res = dir_next(dp, 0);
            if (res == FR_NO_FILE)
                res = FR_OK;   /* the end comes at the next call */
        } else if (res == FR_NO_FILE) {
            res = FR_OK;       /* the end: fname stays "" */
        }
        FREE_NAMEBUFF();
    }
    if (res != FR_OK)
        fno->fname[0] = 0;
    LEAVE_FF(fs, res);
}

/* The modification date and time in the entry at `place` (f_stat's fdate
 * and ftime, read through the volume's window as f_sync writes them). */
FRESULT ffx_entry_time(FATFS *fs, uint64_t place, WORD *date, WORD *time)
{
    FRESULT res = move_window(fs, (LBA_t)(place >> 4));
    if (res == FR_OK) {
        const BYTE *dir = fs->win + (place & 15u) * SZDIRE;
        *time = ld_16(dir + DIR_ModTime);
        *date = ld_16(dir + DIR_ModTime + 2);
    }
    return res;
}
