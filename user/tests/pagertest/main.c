/* pagertest: a mapped file as a program sees it (file.map, the pager the
 * mount's filesystem service answers from). The pager tests
 * (tools/pager-test.sh) type it into the shell; every line it prints
 * starts with "pagertest:".
 *
 *   pagertest self                          the mapping code over a VMO that
 *                                           is no pager's (no mount needed)
 *   pagertest map <path>                    map the file, say what came
 *   pagertest read <path> [offset [len [passes]]]
 *                                           hash a range of the file through
 *                                           the mapping, and the same range
 *                                           through file_read; len 0: to the
 *                                           end; each pass is timed
 *   pagertest hash <path> [passes]          read <path> 0 0 [passes]
 *   pagertest xhash <path>                  the same once through a VMO of
 *                                           our own (FILE_MAP_EXEC), read with
 *                                           vmo_read instead of a mapping
 *   pagertest touch <path> <offset> load|copy [cue]
 *                                           map the file, then (after the file
 *                                           <cue> appears, if one is named)
 *                                           touch the byte at <offset>: a load
 *                                           in this process, or a copy made by
 *                                           a system call (vmo_write from the
 *                                           mapping). The test script pulls the
 *                                           stick or writes the file between
 *                                           the map and the cue.
 *   pagertest write <path>                  make the file, map it, change it
 *                                           every way through an open of its
 *                                           own and check the mapping after
 *                                           each change (write.c)
 *   pagertest pull <path> <cue> load|copy   map the file, read its first half
 *                                           through the mapping, wait for the
 *                                           cue (the script pulls the stick
 *                                           first), then copy a byte from the
 *                                           mapping at 0 (read before) and at
 *                                           the half (never read), each timed,
 *                                           or load the byte at the half
 *   pagertest rec make|put|check ...        records stored through a writable
 *                                           shared mapping, msynced or not,
 *                                           and checked by read and by a new
 *                                           mapping (rec.c's header)
 *
 * Exit codes: 0 done and, where there is something to compare, equal; 1 a
 * mismatch or a failed check; 2 the file could not be mapped; 3 the cue
 * never came; 4 the copy failed (the touch's report says with what). A
 * load the pager can't satisfy ends the process in the kernel with a line
 * of its own, before any report.
 *
 * Its list gives it every mount read-write, so a file on /data or a stick
 * can be mapped (and, for `write`, made and changed), and a cue is a file
 * somewhere in them. */
#include <fs_idl.h>
#include <os.h>
#include <sha256.h>
#include <wants.h>
#include "pagertest.h"

JAM_WANTS("mount * rw\n");

#define STEP      (64u << 10)   /* hashed at a time */
#define CUE_WAIT  (120 * NS_PER_S)
#define CUE_POLL  (50 * NS_PER_MS)

static bool number(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    if (!*s)
        return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' || v > (UINT64_MAX - 9) / 10)
            return false;
        v = v * 10 + (uint64_t)(*s - '0');
    }
    *out = v;
    return true;
}

static int usage(void)
{
    printf("usage: pagertest self | map <path> | read <path> [offset [len [passes]]] | "
           "hash <path> [passes] | xhash <path> | touch <path> <offset> load|copy [cue] | "
           "write <path> | pull <path> <cue> load|copy | rec make|put|check ...\n");
    return 64;
}

/* The mapping of [off, off + len) as digest, one STEP at a time so a
 * failed page is the only thing a pass loses. */
static void hash_mapped(const struct mapped_file *mf, uint64_t off, uint64_t len,
                        uint8_t digest[SHA256_BYTES])
{
    struct sha256 s;
    sha256_init(&s);
    for (uint64_t at = 0; at < len; at += STEP)
        sha256_add(&s, mf->data + off + at, len - at < STEP ? len - at : STEP);
    sha256_done(&s, digest);
}

/* The same range from file_read (the service's own read path). */
static status_t hash_read(const char *path, uint64_t off, uint64_t len,
                          uint8_t digest[SHA256_BYTES])
{
    struct jfile f;
    status_t st = file_open(path, FS_READ, &f);
    uint8_t *buf = st == OK ? malloc(STEP) : NULL;
    if (st == OK && !buf)
        st = ERR_NO_MEMORY;
    struct sha256 s;
    sha256_init(&s);
    for (uint64_t at = 0; st == OK && at < len;) {
        size_t want = len - at < STEP ? (size_t)(len - at) : STEP, got = 0;
        st = file_read(&f, off + at, buf, want, &got);
        if (st == OK && got != want)
            st = ERR_IO;   /* the file ended early */
        sha256_add(&s, buf, got);
        at += got;
    }
    sha256_done(&s, digest);
    free(buf);
    if (f.ch)
        file_close(&f);
    return st;
}

static int cmd_map(const char *path)
{
    struct mapped_file mf;
    status_t st = mapped_file_open(path, &mf);
    printf("pagertest: map %s: %s", path, status_str(st));
    if (st != OK) {
        printf("\n");
        return 2;
    }
    printf(" (%lu bytes)\n", (unsigned long)mf.size);
    mapped_file_close(&mf);
    return 0;
}

static int cmd_read(const char *path, uint64_t off, uint64_t len, uint64_t passes)
{
    struct mapped_file mf;
    status_t st = mapped_file_open(path, &mf);
    if (st != OK) {
        printf("pagertest: map %s: %s\n", path, status_str(st));
        return 2;
    }
    if (off > mf.size || (len && len > mf.size - off)) {
        printf("pagertest: read %s: [%lu, +%lu) is past the end (%lu bytes)\n", path,
               (unsigned long)off, (unsigned long)len, (unsigned long)mf.size);
        mapped_file_close(&mf);
        return 1;
    }
    if (!len)
        len = mf.size - off;
    uint8_t mapped[SHA256_BYTES], read[SHA256_BYTES];
    char hm[2 * SHA256_BYTES + 1], hr[2 * SHA256_BYTES + 1];
    for (uint64_t pass = 1; pass <= passes; pass++) {
        uint64_t t0 = now();
        hash_mapped(&mf, off, len, mapped);
        sha256_hex(mapped, hm);
        printf("pagertest: pass %lu of %s: %lu bytes at %lu in %lu ms: sha256 %s\n",
               (unsigned long)pass, path, (unsigned long)len, (unsigned long)off,
               (unsigned long)((now() - t0) / NS_PER_MS), hm);
    }
    st = hash_read(path, off, len, read);
    sha256_hex(read, hr);
    mapped_file_close(&mf);
    bool same = st == OK && !memcmp(mapped, read, SHA256_BYTES);
    /* One short line when they match (a long one is cut in two in the log). */
    if (same)
        printf("pagertest: %s: %lu bytes: mapped sha256 %s: read the same\n", path,
               (unsigned long)len, hm);
    else
        printf("pagertest: %s: %lu bytes: mapped sha256 %s, read %s (%s): MISMATCH\n", path,
               (unsigned long)len, hm, st == OK ? hr : "-", status_str(st));
    return same ? 0 : 1;
}

/* The file through a FILE_MAP_EXEC VMO (the shell's kind, made for us
 * alone), read with vmo_read, which waits for the pages as a fault does
 * (a page the service can't supply is ERR_IO), against file_read. */
static int cmd_xhash(const char *path)
{
    handle_t vmo = HANDLE_INVALID;
    uint64_t size = 0;
    status_t st = file_map_path(path, FILE_MAP_EXEC, &vmo, &size);
    if (st != OK) {
        printf("pagertest: map %s: %s\n", path, status_str(st));
        return 2;
    }
    uint8_t *step = malloc(STEP), got[SHA256_BYTES], read[SHA256_BYTES];
    struct sha256 s;
    sha256_init(&s);
    st = step ? OK : ERR_NO_MEMORY;
    for (uint64_t at = 0; st == OK && at < size; at += STEP) {
        uint64_t n = size - at < STEP ? size - at : STEP;
        st = jam_vmo_read(vmo, at, step, n);
        sha256_add(&s, step, st == OK ? n : 0);
    }
    sha256_done(&s, got);
    free(step);
    jam_handle_close(vmo);
    status_t rs = hash_read(path, 0, size, read);
    char hg[2 * SHA256_BYTES + 1], hr[2 * SHA256_BYTES + 1];
    sha256_hex(got, hg);
    sha256_hex(read, hr);
    bool same = st == OK && rs == OK && !memcmp(got, read, SHA256_BYTES);
    if (same)
        printf("pagertest: %s: %lu bytes: own VMO sha256 %s: read the same\n", path,
               (unsigned long)size, hg);
    else
        printf("pagertest: %s: %lu bytes: own VMO sha256 %s (%s), read %s (%s): MISMATCH\n",
               path, (unsigned long)size, hg, status_str(st), hr, status_str(rs));
    return same ? 0 : 1;
}

/* Wait for the cue file to appear: the script's way of saying "now". */
static bool wait_cue(const char *cue)
{
    uint64_t end = now() + CUE_WAIT;
    printf("pagertest: waiting for %s\n", cue);
    while (fs_stat(cue, NULL, NULL, NULL) != OK) {
        if (now() > end)
            return false;
        jam_sleep_until(now() + CUE_POLL);
    }
    printf("pagertest: %s is there\n", cue);
    return true;
}

static int cmd_touch(const char *path, uint64_t off, bool copy, const char *cue)
{
    struct mapped_file mf;
    status_t st = mapped_file_open(path, &mf);
    if (st != OK) {
        printf("pagertest: map %s: %s\n", path, status_str(st));
        return 2;
    }
    if (off >= mf.size) {
        printf("pagertest: touch %s: offset %lu is past the end (%lu bytes)\n", path,
               (unsigned long)off, (unsigned long)mf.size);
        mapped_file_close(&mf);
        return 1;
    }
    printf("pagertest: mapped %s (%lu bytes)\n", path, (unsigned long)mf.size);
    if (cue && !wait_cue(cue)) {
        printf("pagertest: the cue %s never came\n", cue);
        mapped_file_close(&mf);
        return 3;
    }
    int rc = 0;
    if (copy) {
        /* A system call reading the mapping: vmo_write's source is it. */
        handle_t sink = HANDLE_INVALID;
        st = jam_vmo_create(PAGE_SIZE, 0, &sink);
        if (st == OK)
            st = jam_vmo_write(sink, 0, mf.data + off, 1);
        if (sink)
            jam_handle_close(sink);
        printf("pagertest: touch %s at %lu by a copy: %s\n", path, (unsigned long)off,
               status_str(st));
        rc = st == OK ? 0 : 4;
    } else {
        printf("pagertest: touching %s at %lu\n", path, (unsigned long)off);
        volatile const uint8_t *p = mf.data + off;
        uint8_t b = *p;   /* a page the pager can't supply ends this process here */
        printf("pagertest: touched %s at %lu: byte %02x\n", path, (unsigned long)off, b);
    }
    mapped_file_close(&mf);
    return rc;
}

/* A system call's copy of the mapping's byte at off: its status, and how
 * long it took, said. */
static status_t timed_copy(const char *path, const struct mapped_file *mf, uint64_t off)
{
    handle_t sink = HANDLE_INVALID;
    status_t st = jam_vmo_create(PAGE_SIZE, 0, &sink);
    uint64_t t0 = now();
    if (st == OK)
        st = jam_vmo_write(sink, 0, mf->data + off, 1);
    uint64_t ms = (now() - t0) / NS_PER_MS;
    if (sink)
        jam_handle_close(sink);
    printf("pagertest: pull %s: a copy at %lu: %s in %lu ms\n", path, (unsigned long)off,
           status_str(st), (unsigned long)ms);
    return st;
}

static int cmd_pull(const char *path, const char *cue, bool copy)
{
    struct mapped_file mf;
    status_t st = mapped_file_open(path, &mf);
    if (st != OK) {
        printf("pagertest: map %s: %s\n", path, status_str(st));
        return 2;
    }
    uint64_t half = (mf.size / 2) & ~(uint64_t)(PAGE_SIZE - 1);
    uint8_t digest[SHA256_BYTES];
    char hex[2 * SHA256_BYTES + 1];
    hash_mapped(&mf, 0, half, digest);
    sha256_hex(digest, hex);
    printf("pagertest: pull %s: read %lu bytes: sha256 %s\n", path, (unsigned long)half, hex);
    if (!half || !wait_cue(cue)) {
        mapped_file_close(&mf);
        return 3;
    }
    int rc = 0;
    if (copy) {
        /* The stick is gone: both fail (ERR_IO), the page read before too. */
        rc = timed_copy(path, &mf, 0) == ERR_IO && timed_copy(path, &mf, half) == ERR_IO ? 0 : 4;
    } else {
        printf("pagertest: pull %s: loading at %lu\n", path, (unsigned long)half);
        volatile const uint8_t *p = mf.data + half;
        uint8_t b = *p;   /* the stick is gone: this ends the process */
        printf("pagertest: pull %s: loaded byte %02x: still readable\n", path, b);
        rc = 1;
    }
    mapped_file_close(&mf);
    return rc;
}

int main(int argc, char **argv)
{
    uint64_t off = 0, len = 0, passes = 1;
    const char *cmd = argc > 1 ? argv[1] : "";
    if (!strcmp(cmd, "self") && argc == 2)
        return pagertest_self();
    if (!strcmp(cmd, "map") && argc == 3)
        return cmd_map(argv[2]);
    if (!strcmp(cmd, "hash") && argc >= 3 && argc <= 4 &&
        (argc == 3 || (number(argv[3], &passes) && passes >= 1 && passes <= 64)))
        return cmd_read(argv[2], 0, 0, passes);
    if (!strcmp(cmd, "xhash") && argc == 3)
        return cmd_xhash(argv[2]);
    if (!strcmp(cmd, "read") && argc >= 3 && argc <= 6 &&
        (argc < 4 || number(argv[3], &off)) && (argc < 5 || number(argv[4], &len)) &&
        (argc < 6 || (number(argv[5], &passes) && passes >= 1 && passes <= 64)))
        return cmd_read(argv[2], off, len, passes);
    if (!strcmp(cmd, "touch") && (argc == 5 || argc == 6) && number(argv[3], &off) &&
        (!strcmp(argv[4], "load") || !strcmp(argv[4], "copy")))
        return cmd_touch(argv[2], off, !strcmp(argv[4], "copy"), argc == 6 ? argv[5] : NULL);
    if (!strcmp(cmd, "write") && argc == 3)
        return pagertest_write(argv[2]);
    if (!strcmp(cmd, "rec") && argc >= 3)
        return pagertest_rec(argc - 2, argv + 2);
    if (!strcmp(cmd, "pull") && argc == 5 &&
        (!strcmp(argv[4], "load") || !strcmp(argv[4], "copy")))
        return cmd_pull(argv[2], argv[3], !strcmp(argv[4], "copy"));
    return usage();
}
