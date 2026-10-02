/* utest: file names on the fat service. FAT stores a long name beside a
 * generated 8.3 alias, and a FAT driver gets these wrong easily: a name
 * with spaces, a lowercase name coming back in capitals, two long names
 * that share their first six letters ending up with the same alias, and
 * characters FAT forbids being accepted. Also here: names in UTF-8,
 * lookups without case, paths that are not paths, and control characters
 * that another computer wrote into a name or the label, which fat never
 * hands out as they are. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "fattest.h"
#include "utest.h"

static struct ramdisk disk;

#define LONG_NAME "a rather long file name, well past eight characters, to fill more " \
                  "than one long-name entry of thirteen letters each.text"
#define UTF8_NAME "caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac.txt"   /* "café 日本.txt" */

/* The short (8.3) names in the root directory of the FAT12/16 volume on
 * rd that start with `prefix`: how many, and whether any two are the
 * same. Long-name entries and deleted ones are skipped. */
static bool short_names(const struct ramdisk *rd, const char *prefix, unsigned *count,
                        bool *repeated)
{
    const uint8_t *b = rd->mem;
    uint32_t reserved = b[14] | (uint32_t)b[15] << 8, fat_size = b[22] | (uint32_t)b[23] << 8;
    uint32_t entries = b[17] | (uint32_t)b[18] << 8;
    const uint8_t *root = rd->mem + (uint64_t)(reserved + b[16] * fat_size) * RAMDISK_SECTOR;
    const uint8_t *seen[8];
    size_t plen = strlen(prefix);
    *count = 0;
    *repeated = false;
    CHECK(fat_size != 0 && entries != 0);   /* FAT32 has neither: not this helper's volume */
    for (uint32_t i = 0; i < entries && root[i * 32] != 0; i++) {
        const uint8_t *e = root + i * 32;
        if (e[0] == 0xe5 || e[11] == 0x0f || memcmp(e, prefix, plen))
            continue;
        for (unsigned k = 0; k < *count; k++)
            if (!memcmp(seen[k], e, 11))
                *repeated = true;
        CHECK(*count < 8);
        seen[(*count)++] = e;
    }
    return true;
}

/* Names that must round-trip exactly as given. */
static bool check_kept_names(const struct fatrun *r)
{
    static const char *const names[] = {
        "My Notes.txt", "notes.txt", "ReadMe.md", "UPPER.TXT", "holiday-photos.txt",
        "holiday-plans.txt", ".hidden", "two.dots.tar.gz", "  leading spaces", LONG_NAME,
        UTF8_NAME,
    };
    unsigned n = sizeof(names) / sizeof(names[0]), count = 0;
    char path[FS_PATH_MAX];
    bool found = false;
    for (unsigned i = 0; i < n; i++) {
        snprintf(path, sizeof(path), "/%s", names[i]);
        if (!put_file(r, path, names[i]))
            return false;
    }
    for (unsigned i = 0; i < n; i++) {
        snprintf(path, sizeof(path), "/%s", names[i]);
        if (!file_is(r, path, names[i]) || !dir_count(r, "/", names[i], &count, &found))
            return false;
        if (!found)
            FAIL("readdir has no \"%s\"", names[i]);
        CHECK_EQ(count, n);
    }
    return true;
}

/* FAT compares names without case: one file, whatever the spelling. */
static bool check_case(const struct fatrun *r)
{
    struct tfile f;
    unsigned before = 0, after = 0;
    bool found = false;
    if (!dir_count(r, "/", NULL, &before, NULL) || !file_is(r, "/NOTES.TXT", "notes.txt") ||
        !file_is(r, "/my notes.TXT", "My Notes.txt"))
        return false;
    CHECK_ST(t_open(r, "/Notes.txt", FS_WRITE | FS_CREATE, &f), OK);   /* the same file */
    CHECK_EQ(f.size, 9);
    t_close(&f);
    CHECK_ST(t_mkdir(r, "/NOTES.txt"), ERR_ALREADY_EXISTS);
    if (!dir_count(r, "/", "notes.txt", &after, &found))
        return false;
    CHECK(after == before && found);   /* and it kept its own spelling */
    /* A rename that changes only the case does change the spelling. */
    CHECK_ST(t_rename(r, "/notes.txt", "/Notes.TXT"), OK);
    if (!dir_count(r, "/", "Notes.TXT", &after, &found))
        return false;
    CHECK(after == before && found);
    CHECK_ST(t_rename(r, "/Notes.TXT", "/notes.txt"), OK);
    return true;
}

/* What FAT can't store is refused, never stored as something else. */
static bool check_refused(const struct fatrun *r)
{
    static const char *const bad[] = {
        "/a*b.txt", "/a?b.txt", "/a:b.txt", "/a<b.txt", "/a>b.txt", "/a|b.txt", "/a\"b.txt",
        "/a\\b.txt", "/tab\there", "/bell\x07", "/del\x7f", "/trailing.", "/trailing ",
        "/dir*/x.txt", "relative.txt", "0:/drive.txt",
    };
    struct tfile f;
    unsigned before = 0, after = 0;
    uint8_t full[FS_PATH_MAX];
    if (!dir_count(r, "/", NULL, &before, NULL))
        return false;
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (t_open(r, bad[i], FS_WRITE | FS_CREATE, &f) != ERR_INVALID_ARGS)
            FAIL("creating \"%s\" was not refused ERR_INVALID_ARGS", bad[i]);
        if (t_mkdir(r, bad[i]) != ERR_INVALID_ARGS)
            FAIL("mkdir \"%s\" was not refused ERR_INVALID_ARGS", bad[i]);
        if (t_rename(r, "/notes.txt", bad[i]) != ERR_INVALID_ARGS)
            FAIL("renaming to \"%s\" was not refused ERR_INVALID_ARGS", bad[i]);
        CHECK_ST(t_stat(r, bad[i], NULL, NULL, NULL), ERR_INVALID_ARGS);
    }
    /* A path field with no NUL in its 256 bytes. */
    memset(full, 'a', sizeof(full));
    full[0] = '/';
    CHECK_ST(fs_stat_until(r->fs, now() + FAT_CALL_NS, full, NULL, NULL, NULL), ERR_INVALID_ARGS);
    CHECK_ST(fs_mkdir_until(r->fs, now() + FAT_CALL_NS, full), ERR_INVALID_ARGS);
    /* Not UTF-8. */
    CHECK_ST(t_mkdir(r, "/bad\xff\xfeutf8"), ERR_INVALID_ARGS);
    if (!dir_count(r, "/", NULL, &after, NULL))
        return false;
    CHECK_EQ(after, before);   /* nothing was made */
    return file_is(r, "/notes.txt", "notes.txt");
}

bool t_fat_names(void)
{
    struct fatrun r;
    unsigned count = 0;
    bool repeated = true, found = false;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false))
        return false;
    if (!check_kept_names(&r) || !check_case(&r) || !check_refused(&r))
        return false;
    /* The two holiday files: two entries, two different aliases. */
    CHECK_ST(t_sync(&r), OK);
    if (!short_names(&disk, "HOLIDA", &count, &repeated))
        return false;
    CHECK(count == 2 && !repeated);
    if (!fat_stop(&r))
        return false;

    /* The names are on the disk, not in fat's memory. */
    if (!fat_start(&r, &disk, false) || !file_is(&r, "/My Notes.txt", "My Notes.txt") ||
        !file_is(&r, "/holiday-plans.txt", "holiday-plans.txt") ||
        !file_is(&r, "/" UTF8_NAME, UTF8_NAME) || !dir_count(&r, "/", "notes.txt", &count, &found))
        return false;
    CHECK(found);
    CHECK_ST(t_unlink(&r, "/My Notes.txt"), OK);
    CHECK_ST(t_unlink(&r, "/" LONG_NAME), OK);
    if (!dir_count(&r, "/", "My Notes.txt", &count, &found))
        return false;
    CHECK(!found && count == 9);
    return fat_stop(&r) && ramdisk_destroy(&disk);
}

/* The first place in rd's sectors holding the n bytes at pat, or NULL. */
static uint8_t *find_bytes(const struct ramdisk *rd, const void *pat, size_t n)
{
    uint64_t size = rd->blocks * RAMDISK_SECTOR;
    for (uint64_t i = 0; i + n <= size; i++)
        if (!memcmp(rd->mem + i, pat, n))
            return rd->mem + i;
    return NULL;
}

/* A name with ESC in it and a label with ESC in it, written straight onto
 * the disk as another computer could: readdir and statfs say '?' instead. */
bool t_fat_names_shown(void)
{
    struct fatrun r;
    char name[FS_PATH_MAX];
    bool is_dir;
    if (!ramdisk_create(&disk, 16 * MIB_SECTORS) || !fat_start(&r, &disk, false) ||
        !put_file(&r, "/esc-Q-name.txt", "x") || !fat_stop(&r))
        return false;
    /* The long name's "c-Q" in UTF-16, and the label's "JAMOS" in the root
     * directory's volume-label entry. */
    uint8_t *q = find_bytes(&disk, "c\0-\0Q\0", 6), *l = find_bytes(&disk, "JAMOS-DATA ", 11);
    CHECK(q != NULL && l != NULL);
    q[4] = 0x1b;
    while ((l = find_bytes(&disk, "JAMOS-DATA ", 11)) != NULL)
        l[0] = 0x1b;   /* the boot sector's copy too */
    CHECK(fat_start_plain(&r, &disk, true));
    CHECK_ST(t_readdir(&r, "/", 0, name, &is_dir), OK);
    if (strcmp(name, "esc-?-name.txt"))
        FAIL("readdir gives \"%s\"", name);
    uint8_t label[16] = { 0 };
    CHECK_ST(fs_statfs_until(r.fs, now() + FAT_CALL_NS, NULL, NULL, NULL, label), OK);
    if (strcmp((const char *)label, "?AMOS-DATA"))
        FAIL("statfs's label is \"%s\"", (const char *)label);
    return fat_stop(&r) && ramdisk_destroy(&disk);
}
