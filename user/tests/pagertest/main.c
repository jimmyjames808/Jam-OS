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
 *   pagertest touch <path> <offset> load|copy [cue]
 *                                           map the file, then (after the file
 *                                           <cue> appears, if one is named)
 *                                           touch the byte at <offset>: a load
 *                                           in this process, or a copy made by
 *                                           a system call (vmo_write from the
 *                                           mapping). The test script pulls the
 *                                           stick or writes the file between
 *                                           the map and the cue.
 *
 * Exit codes: 0 done and, where there is something to compare, equal; 1 a
 * mismatch or a failed check; 2 the file could not be mapped; 3 the cue
 * never came; 4 the copy failed (the touch's report says with what). A
 * load the pager can't satisfy ends the process in the kernel with a line
 * of its own, before any report.
 *
 * Its list gives it every mount read-only, so a file on /data or a stick
 * can be mapped, and a cue is a file somewhere in them. */
#include <os.h>
#include <sha256.h>
#include <wants.h>
#include "pagertest.h"

JAM_WANTS("mount * r\n");

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
           "hash <path> [passes] | touch <path> <offset> load|copy [cue]\n");
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
    printf("pagertest: %s: %lu bytes: mapped sha256 %s, read %s (%s): %s\n", path,
           (unsigned long)len, hm, st == OK ? hr : "-", status_str(st),
           same ? "match" : "MISMATCH");
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
    if (!strcmp(cmd, "read") && argc >= 3 && argc <= 6 &&
        (argc < 4 || number(argv[3], &off)) && (argc < 5 || number(argv[4], &len)) &&
        (argc < 6 || (number(argv[5], &passes) && passes >= 1 && passes <= 64)))
        return cmd_read(argv[2], off, len, passes);
    if (!strcmp(cmd, "touch") && (argc == 5 || argc == 6) && number(argv[3], &off) &&
        (!strcmp(argv[4], "load") || !strcmp(argv[4], "copy")))
        return cmd_touch(argv[2], off, !strcmp(argv[4], "copy"), argc == 6 ? argv[5] : NULL);
    return usage();
}
