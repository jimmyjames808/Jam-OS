/* utest: FatFs's structs as fat's state relies on them (docs/M11.6-PLAN.md,
 * "FatFs without re-mounting"; user/services/fat/fat.h, struct fat_state).
 *
 * fat keeps FatFs's volume (FATFS) and every open file's FIL in its state
 * VMO, at a fixed address, so that a successor can take them over without
 * mounting: it registers the volume with f_mount(fs, "", 0), which only
 * records the pointer, and copies the committed FATFS back over it; the
 * FILs it finds as they were. That leans on FatFs's insides, which FatFs
 * doesn't promise to keep: this test pins them, as fat's build of FatFs
 * sees them (ff.h with fat's ffconf.h; FatFs is vendored unmodified at
 * R0.16, third_party/fatfs). An upgrade or a changed ffconf.h that moves
 * any of this fails here first: check the adoption code (stage F3)
 * against the new FatFs before changing a number below.
 *
 * What is relied on:
 * - the revision, and the options that shape the structs or add state
 *   FatFs keeps on its own: no lock table (FF_FS_LOCK 0: no lockid in an
 *   object, no Files[]), no mutexes (FF_FS_REENTRANT 0), one volume, the
 *   FIL's own sector buffer (FF_FS_TINY 0), no exFAT, no fast seek (no
 *   cluster table pointer), no relative paths (no current directory),
 *   512-byte sectors only, 32-bit LBAs, a static long-name buffer;
 * - FATFS: fs_type (0: not mounted, FatFs would mount again), pdrv, id
 *   (every FIL carries it: FatFs refuses a FIL whose id is not its
 *   volume's), wflag, winsect and win (the window and whether it is
 *   dirty), fsi_flag, last_clst and free_clst (the allocation hints), the
 *   geometry (n_fats, n_rootdir, csize, n_fatent, fsize, volbase,
 *   fatbase, dirbase, database), and lfnbuf, the one pointer out of the
 *   state (FatFs's static buffer in fat's own image: the same address in
 *   every instance of one build);
 * - a FIL: obj (fs, the volume, in the state; id; attr; stat; sclust;
 *   objsize), flag, err, fptr, clust, sect, dir_sect, dir_ptr (into the
 *   volume's win, in the state) and buf. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <stddef.h>
#include <check.h>
#include "ff.h"
#include "utest.h"

/* The revision and the options. */
static bool check_config(void)
{
    CHECK_EQ(FF_DEFINED, 80386);   /* R0.16 */
    CHECK_EQ(FFCONF_DEF, 80386);
    CHECK_EQ(FF_FS_LOCK, 0);
    CHECK_EQ(FF_FS_REENTRANT, 0);
    CHECK_EQ(FF_VOLUMES, 1);
    CHECK_EQ(FF_FS_TINY, 0);
    CHECK_EQ(FF_FS_EXFAT, 0);
    CHECK_EQ(FF_FS_READONLY, 0);
    CHECK_EQ(FF_USE_FASTSEEK, 0);
    CHECK_EQ(FF_FS_RPATH, 0);
    CHECK_EQ(FF_MIN_SS, 512);
    CHECK_EQ(FF_MAX_SS, 512);
    CHECK_EQ(FF_LBA64, 0);
    CHECK_EQ(FF_USE_LFN, 1);
    CHECK_EQ(sizeof(LBA_t), 4);
    CHECK_EQ(sizeof(FSIZE_t), 4);
    return true;
}

static bool check_fatfs(void)
{
    CHECK_EQ(sizeof(FATFS), 576);
    CHECK_EQ(offsetof(FATFS, fs_type), 0);
    CHECK_EQ(offsetof(FATFS, pdrv), 1);
    CHECK_EQ(offsetof(FATFS, n_fats), 3);
    CHECK_EQ(offsetof(FATFS, wflag), 4);
    CHECK_EQ(offsetof(FATFS, fsi_flag), 5);
    CHECK_EQ(offsetof(FATFS, id), 6);
    CHECK_EQ(offsetof(FATFS, n_rootdir), 8);
    CHECK_EQ(offsetof(FATFS, csize), 10);
    CHECK_EQ(offsetof(FATFS, lfnbuf), 16);
    CHECK_EQ(sizeof(((FATFS *)0)->lfnbuf), 8);
    CHECK_EQ(offsetof(FATFS, last_clst), 24);
    CHECK_EQ(offsetof(FATFS, free_clst), 28);
    CHECK_EQ(offsetof(FATFS, n_fatent), 32);
    CHECK_EQ(offsetof(FATFS, fsize), 36);
    CHECK_EQ(offsetof(FATFS, winsect), 40);
    CHECK_EQ(offsetof(FATFS, volbase), 44);
    CHECK_EQ(offsetof(FATFS, fatbase), 48);
    CHECK_EQ(offsetof(FATFS, dirbase), 52);
    CHECK_EQ(offsetof(FATFS, database), 56);
    CHECK_EQ(offsetof(FATFS, win), 60);
    CHECK_EQ(sizeof(((FATFS *)0)->win), 512);
    return true;
}

static bool check_fil(void)
{
    CHECK_EQ(sizeof(FFOBJID), 24);
    CHECK_EQ(offsetof(FFOBJID, fs), 0);
    CHECK_EQ(offsetof(FFOBJID, id), 8);
    CHECK_EQ(offsetof(FFOBJID, attr), 10);
    CHECK_EQ(offsetof(FFOBJID, stat), 11);
    CHECK_EQ(offsetof(FFOBJID, sclust), 12);
    CHECK_EQ(offsetof(FFOBJID, objsize), 16);
    CHECK_EQ(sizeof(FIL), 568);
    CHECK_EQ(offsetof(FIL, obj), 0);
    CHECK_EQ(offsetof(FIL, flag), 24);
    CHECK_EQ(offsetof(FIL, err), 25);
    CHECK_EQ(offsetof(FIL, fptr), 28);
    CHECK_EQ(offsetof(FIL, clust), 32);
    CHECK_EQ(offsetof(FIL, sect), 36);
    CHECK_EQ(offsetof(FIL, dir_sect), 40);
    CHECK_EQ(offsetof(FIL, dir_ptr), 48);
    CHECK_EQ(sizeof(((FIL *)0)->dir_ptr), 8);
    CHECK_EQ(offsetof(FIL, buf), 56);
    CHECK_EQ(sizeof(((FIL *)0)->buf), 512);
    return true;
}

bool t_fat_layout(void)
{
    return check_config() && check_fatfs() && check_fil();
}
