/* Running a program from /data from its pages (sh_allow.c has the
 * approvals, their page tables and the whole-read way).
 *
 * The file comes as a VMO of the shell's own from /data's filesystem
 * service (file.map with FILE_MAP_EXEC; no other process holds a handle of
 * it), whose pages that service supplies as they are touched. Before
 * anything reads it, the kernel is handed the approval's page table
 * (vmo_make_exec_verified, RIGHT_ROOT_VMEX: this shell's root, no
 * program's), and from then on it publishes a page only if its SHA-256 is
 * the one `allow` wrote for it. So the bytes that run are the bytes the
 * owner approved, every one of them, whenever it arrives (first touch, or
 * again after the cache evicted it), while a page nobody touches is never
 * read, and two instances share every page.
 *
 * What is checked, and when:
 * - at the start: the approval's line (the path's hash) and its table's
 *   header (that hash, the file's size now). A file of another size has
 *   changed since it was allowed: it is refused before any of it is read
 *   from its pages (the whole read then says why, as before tables);
 * - the program's list (<wants.h>), read from the verified VMO with
 *   vmo_read, and everything libos's spawn reads (the ELF header, the
 *   program headers, the writable segments), also with vmo_read: never
 *   through a mapping of the shell's, so a page that fails its hash is
 *   ERR_IO there, a refusal, where a load would kill the shell;
 * - every page the program itself touches, by the kernel: one that doesn't
 *   match kills the program ("a page doesn't match the approved program").
 * A file changed after its approval (the same size, on another computer:
 * fat refuses to write a file while a VMO made for running it lives) thus
 * fails at the start or at the changed page, and never runs.
 *
 * The other ways: an approval written before page tables is checked once
 * against the whole file (sh_program_file: read, sealed, hashed), its
 * table written from those very bytes, then run from its pages; a /data
 * whose service has no pages to give (file.map ERR_NOT_SUPPORTED), or a
 * table that can't be written, runs the whole-read way (sh_allowed_whole).
 *
 * Sharing: the shell keeps each verified VMO (IMAGES at most) while a job
 * of an instance started from it still has a thread; a start of the same
 * path under the same approval runs from that VMO, so its pages are
 * fetched, checked and held once for all of them (Chromium starts its one
 * binary many times). When no instance runs any more (the foreground one
 * reaped, the background one finished: sh_data_prune) the shell lets it
 * go, and the file can be written or removed again. A kept VMO whose
 * filesystem went (the stick pulled: its pages are dropped and every
 * fetch fails) fails the list's read at the next start: it is let go and
 * a fresh one made from the mount that is there now. */
#include <dataprog.h>
#include <fs_idl.h>
#include <wants.h>
#include "sh.h"

#define IMAGES     8    /* verified VMOs kept while their instances run */
#define IMAGE_JOBS 16   /* instances of one recorded (more run, unrecorded) */

struct image {
    handle_t vmo;                   /* verified and executable; HANDLE_INVALID: a free slot */
    uint64_t koid;                  /* vmo's: how a caller's handle of it is known */
    uint64_t size;                  /* the file's bytes */
    char     path[SH_PATH_MAX];     /* the file */
    char     hex[2 * SHA256_BYTES + 1];   /* its approval's hash */
    handle_t jobs[IMAGE_JOBS];      /* its instances' jobs (RIGHT_INSPECT copies) */
    unsigned njobs;                 /* entries of jobs */
};

static struct image images[IMAGES];

static uint64_t koid_of(handle_t h)
{
    struct handle_info hi;
    return jam_handle_info(h, &hi) == OK ? hi.koid : 0;
}

static void image_drop(struct image *im)
{
    for (unsigned i = 0; i < im->njobs; i++)
        jam_handle_close(im->jobs[i]);
    jam_handle_close(im->vmo);
    memset(im, 0, sizeof(*im));
}

/* Does the job still have a thread? (One we can't ask about has ended.) */
static bool job_running(handle_t job)
{
    struct job_info ji;
    return jam_job_get_info(job, &ji) == OK && ji.used[JOB_LIMIT_THREADS] > 0;
}

/* Forget im's ended instances, and im itself if none is left. */
static void image_prune(struct image *im)
{
    unsigned n = 0;
    for (unsigned i = 0; i < im->njobs; i++) {
        if (job_running(im->jobs[i]))
            im->jobs[n++] = im->jobs[i];
        else
            jam_handle_close(im->jobs[i]);
    }
    im->njobs = n;
    if (!n)
        image_drop(im);
}

void sh_data_prune(void)
{
    for (unsigned i = 0; i < IMAGES; i++)
        if (images[i].vmo)
            image_prune(&images[i]);
}

void sh_data_release(handle_t vmo, handle_t job)
{
    if (!vmo)
        return;
    uint64_t koid = koid_of(vmo);
    jam_handle_close(vmo);
    for (unsigned i = 0; i < IMAGES && koid; i++) {
        struct image *im = &images[i];
        if (!im->vmo || im->koid != koid)
            continue;
        handle_t mine;
        if (job && im->njobs < IMAGE_JOBS &&
            jam_handle_duplicate(job, RIGHT_INSPECT, &mine) == OK)
            im->jobs[im->njobs++] = mine;
        image_prune(im);   /* it didn't start, or ended already: let go now */
    }
}

static struct image *image_find(const char *path, const char *hex)
{
    for (unsigned i = 0; i < IMAGES; i++)
        if (images[i].vmo && !strcmp(images[i].path, path) && !strcmp(images[i].hex, hex))
            return &images[i];
    return NULL;
}

/* Keep a handle of x (verified, for path under approval hex) for the
 * instances to come. Not kept if every slot is in use: it runs unshared. */
static void image_keep(handle_t x, const char *path, const char *hex, uint64_t size)
{
    uint64_t koid = koid_of(x);
    for (unsigned i = 0; i < IMAGES && koid; i++) {
        struct image *im = &images[i];
        handle_t mine;
        if (im->vmo || jam_handle_duplicate(x, RIGHT_SAME, &mine) != OK)
            continue;
        *im = (struct image){ .vmo = mine, .koid = koid, .size = size };
        snprintf(im->path, sizeof(im->path), "%s", path);
        snprintf(im->hex, sizeof(im->hex), "%s", hex);
        return;
    }
}

/* x's list into *w, checked: false (said) if it can't be read from the
 * approved pages or asks for what no program from /data may have. */
static bool list_ok(const char *path, handle_t x, uint64_t size, struct wants *w)
{
    status_t st = dp_wants_vmo(x, size, w);
    if (st == ERR_IO) {
        sh_tty("run: %s changed since it was allowed (or /data can't read it): `allow %s` "
               "again\n", path, path);
        return false;
    }
    if (st != OK) {
        sh_tty("run: %s: %s\n", path, st == ERR_INVALID_ARGS ? "not a program it can run" :
                                       sh_why(st));
        return false;
    }
    const char *bad = sh_wants_refused(w);
    if (bad)
        sh_tty("run: %s: asks for %s, which no program from /data may have\n", path, bad);
    return !bad;
}

/* An approval written before page tables: the whole file checked against
 * it once (read and sealed: sh_program_file), and its table written from
 * those very bytes, then read back as a run reads it. ERR_NOT_FOUND: what
 * was read isn't what was approved; ERR_NO_SPACE and the like: the table
 * couldn't be written. */
static status_t first_table(const char *path, const char *hex, uint64_t size, handle_t *table)
{
    static struct wants w;   /* the shell starts one program at a time */
    char got[2 * SHA256_BYTES + 1];
    handle_t whole;
    uint64_t n;
    status_t st = sh_program_file(path, &whole, &n, got, &w);
    if (st != OK)
        return ERR_NOT_FOUND;   /* the whole read says why */
    if (strcmp(got, hex) || n != size)
        st = ERR_NOT_FOUND;
    if (st == OK)
        st = sh_allow_table_write(hex, whole, n);
    jam_handle_close(whole);
    if (st == OK)
        st = dp_table_read(hex, size, table);
    if (st == OK)
        sh_tty("run: %s: its approval had no page table: the whole file matched it, and its "
               "table is written\n", path);
    else if (st != ERR_NOT_FOUND)
        sh_tty("run: %s: can't write its approval's page table (%s): it runs read whole\n",
               path, sh_why(st));
    return st;
}

/* The approved table for the pager VMO pv of path (size bytes), into
 * *table. false: run the whole-read way instead (*whole: it says why, or
 * runs it), or refused here (said). */
static bool table_for(const char *path, const char *hex, uint64_t size, handle_t *table,
                      bool *whole)
{
    status_t st = dp_table_read(hex, size, table);
    if (st == ERR_NOT_FOUND)
        st = first_table(path, hex, size, table);
    *whole = st != OK && st != ERR_INVALID_ARGS;
    if (st == ERR_INVALID_ARGS)
        sh_tty("run: %s: its approval's page table is broken: `allow %s` again\n", path, path);
    return st == OK;
}

/* path from its pages: a fresh verified VMO into *x. false: refused (said),
 * or *whole: run it the whole-read way. */
static bool fresh(const char *path, const char *hex, handle_t *x, uint64_t *size, bool *whole)
{
    handle_t pv, table;
    *whole = false;
    status_t st = file_map_path(path, FILE_MAP_EXEC, &pv, size);
    if (st == ERR_NOT_SUPPORTED) {
        *whole = true;   /* /data's service has no pages to give */
        return false;
    }
    if (st != OK) {
        sh_tty("run: %s: %s\n", path, st == ERR_BUSY ? "it is being written: try again when "
                                      "that's done" : sh_why(st));
        return false;
    }
    if (!table_for(path, hex, *size, &table, whole)) {
        jam_handle_close(pv);
        return false;
    }
    /* pv is ours alone and was never mapped or read: from here on the
     * kernel checks every page it publishes. pv is gone whatever happens. */
    st = jam_vmo_make_exec_verified(pv, sh_root(), table, 0, x);
    jam_handle_close(table);
    if (st != OK)
        sh_tty("run: %s: can't make it executable (%s)\n", path, sh_why(st));
    return st == OK;
}

bool sh_allowed_program(const char *path, handle_t *vmo, uint64_t *size, struct wants *w)
{
    if (!sh_on_data(path) || !sh_root())
        return sh_allowed_whole(path, vmo, size, w);   /* it says why not */
    sh_data_prune();
    char hex[DP_HEX_LEN + 1];
    if (dp_allow_hash(path, hex) != OK)
        return sh_allowed_whole(path, vmo, size, w);   /* not allowed, or the file unreadable */
    struct image *im = image_find(path, hex);
    if (im && dp_wants_vmo(im->vmo, im->size, w) != OK) {
        image_drop(im);   /* its mount went, or a page of it failed: a fresh one */
        im = NULL;
    }
    handle_t x;
    uint64_t n;
    bool whole = false;
    if (im) {
        status_t st = jam_handle_duplicate(im->vmo, RIGHT_SAME, &x);
        if (st != OK) {
            sh_tty("run: %s: %s\n", path, sh_why(st));
            return false;
        }
        n = im->size;   /* shared with the instances running from it */
    } else if (!fresh(path, hex, &x, &n, &whole)) {
        return whole && sh_allowed_whole(path, vmo, size, w);
    }
    if (!list_ok(path, x, n, w)) {
        jam_handle_close(x);
        return false;
    }
    if (!im)
        image_keep(x, path, hex, n);
    *vmo = x;
    *size = n;
    return true;
}
