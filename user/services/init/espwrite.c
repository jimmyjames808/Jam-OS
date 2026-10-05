/* init's stick write for `update -w` (update.c, <update.h>
 * UPDATE_OFFER_WRITE): a fetched build, already checked (signature, sizes,
 * SHA-256s) and loaded as the stored kernel, written to the boot stick's
 * ESP too, so it survives a power-off. It runs on update.c's worker
 * thread, never in init's loop: it takes seconds of the stick's time.
 *
 * Who may write the ESP: nobody but init, and init only here. /esp is
 * read-only below the filesystem (devmgr opens its partition read-only),
 * for every program and for init too. devmgr opens it read-write only when
 * asked on the ESP channel (DEVMGR_ESP_WRITE), whose one client end init
 * made when it started devmgr and gives to nobody (services.c). While it
 * is writable /esp is no mount at all, so the writable channel exists only
 * here: it is never put in a namespace. At the end, failed or not, it is
 * made read-only again (its service stops in order: everything on the
 * stick, the volume marked clean), and comes back as /esp.
 *
 * The order, so that one of the boot menu's two entries boots a whole
 * build whenever the stick is pulled or the power goes (or the ESP's
 * service dies): its default entry (boot/jamos.elf with boot/bootfs.img)
 * or "Jam OS (previous build)" (boot/prev-jamos.elf with prev-bootfs.img).
 * The stick's build is renamed, not copied (but below): a rename is a
 * directory write, a copy most of a write's time on a USB 2 stick.
 *   ROOM    an earlier write's renames settled (one that stopped part way:
 *           finished if the new build had both its names, else undone,
 *           settle()); its leftovers (*.new) removed; if the stick's build
 *           is the new one already, nothing is written; room for the new
 *           build checked (else nothing changes);
 *   NEW     the new build written from init's own checked copies as
 *           boot/jamos.elf.new and bootfs.img.new, synced, read back, and
 *           their SHA-256s compared with the manifest's;
 *   SWITCH  1. boot/jamos.elf and bootfs.img renamed to *.old, synced: the
 *              default entry boots nothing now, the previous-build entry
 *              still boots the build before;
 *           2. the *.new renamed to jamos.elf and bootfs.img, synced: the
 *              default entry boots the new build;
 *   PREV    3. prev-jamos.elf and prev-bootfs.img removed, synced: the
 *              default entry still boots the new build;
 *           4. the *.old renamed to prev-jamos.elf and prev-bootfs.img,
 *              synced: the previous-build entry boots the old build.
 * A FAT rename can't replace a file, so a build can't move from one pair
 * of names to the other without a moment when neither pair holds it; the
 * order puts that moment where the other pair holds a whole build (1, 2:
 * the build before; 3, 4: the new one). The cost: a power cut in 1 or 2
 * leaves the stick to boot the build before the old one, from "Jam OS
 * (previous build)", until the next `update -w` settles it. A stick with
 * no whole previous build (none was ever written, or a failure removed it)
 * gets a copy of its build as the previous one first (copy_previous: the
 * one copy left, and only then). A stick whose build isn't whole keeps its
 * previous build (3 and 4 are skipped).
 * A failure stops the write where it is and removes its temporary files;
 * after the switch has begun, the stick's old build is renamed back into
 * the default entry's names (put_back), so the stick boots it as before
 * (with no previous build, if the failure came after 3); if the ESP's
 * service doesn't answer for that, the answer says which entry boots
 * (enum update_stick).
 *
 * The read-back goes through the ESP's service. Its cache keeps only lines
 * a read made (a write never makes one), so for files this size it is
 * mostly the stick's answer; it can't tell the stick's own cache from its
 * flash cells.
 *
 * Time. Every call waits at most until the write's deadline (esp_until()):
 * the steps get WRITE_LIMIT in all, the clean-up after a failure
 * RECOVER_LIMIT more, and making the ESP read-only again ESP_WAIT, so a
 * stick that stops answering ends in "not written" well inside the 300 s
 * bin/update waits. Each step and file logs its time as it goes (esp_say(),
 * a long file every SAY_EVERY), so a slow stick shows in the log. */
#include <devmgr.h>
#include <fs_idl.h>
#include <update.h>
#include "espwrite.h"
#include "init.h"

#define CHUNK      ESP_CHUNK
#define ESP_WAIT   (30 * NS_PER_S)    /* devmgr's ESP_WRITE: a stop in order, then a start */
#define WRITE_LIMIT   (120 * NS_PER_S)   /* the steps, all of them */
#define SLACK      (1u << 20)         /* room kept spare per file: clusters, directory */
#define SAY_EVERY  (2 * NS_PER_S)     /* a long file's progress: a line at most this often */

/* The ESP's files, by the paths fat takes (the mount's root is "/"), and
 * the suffixes of the new build's while it is written and the old build's
 * while the swap moves it. */
static const char *const cur[UPDATE_FILES] = { "/boot/jamos.elf", "/boot/bootfs.img" };
static const char *const prev[UPDATE_FILES] = { "/boot/prev-jamos.elf", "/boot/prev-bootfs.img" };
#define NEW ".new"
#define OLD ".old"

/* Where the old build's file is while the swap moves it. */
enum { AT_NONE, AT_CUR, AT_OLD, AT_PREV };

/* A call's deadline: FS_CALL_TIMEOUT from now, never past the write's. A
 * stick (or ESP service) that stops answering ends the write within
 * WRITE_LIMIT, then the clean-up within RECOVER_LIMIT, then the remount
 * back within ESP_WAIT: `update -w` (bin/update waits 300 s) always gets
 * its answer, and the answer says the write failed. */
uint64_t esp_until(const struct writer *w)
{
    uint64_t d = now() + FS_CALL_TIMEOUT;
    return d < w->deadline ? d : w->deadline;
}

/* A progress line. Every step and file says how long it took, and a long
 * file how far it is every SAY_EVERY, so the log (streamed to the Mac on
 * the PC) always shows where a slow write is. */
void esp_say(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("init: update: write: %s\n", line);
}

unsigned long esp_ms_since(uint64_t t0)
{
    return (unsigned long)((now() - t0) / NS_PER_MS);
}

/* off of size bytes of path + suffix done (what: "written", "read"): a
 * line, if the last one was SAY_EVERY ago. */
static void say_progress(struct writer *w, const char *path, const char *suffix, const char *what,
                         uint64_t off, uint64_t size, uint64_t t0)
{
    if (now() - w->said < SAY_EVERY)
        return;
    w->said = now();
    esp_say("%s%s: %lu of %lu KiB %s, %lu ms so far", path + 1, suffix,
            (unsigned long)(off >> 10), (unsigned long)(size >> 10), what, esp_ms_since(t0));
}

/* fs.idl's path field for path, with suffix (".new" or ""). */
static void field(uint8_t out[FS_PATH_MAX], const char *path, const char *suffix)
{
    memset(out, 0, FS_PATH_MAX);
    snprintf((char *)out, FS_PATH_MAX, "%s%s", path, suffix);
}

status_t esp_inject(struct writer *w, uint32_t step)
{
    if (step == UPDATE_WRITE_NONE || w->job->fail_at != step)
        return OK;
    w->job->fail_at = UPDATE_WRITE_NONE;
    printf("init: update: the stick write fails here, as the test asked (%s)\n",
           update_write_step_str(step));
    return ERR_IO;
}

status_t esp_unlink(struct writer *w, const char *path, const char *suffix)
{
    uint8_t p[FS_PATH_MAX];
    field(p, path, suffix);
    status_t st = fs_unlink_until(w->fs, esp_until(w), p);
    return st == ERR_NOT_FOUND ? OK : st;
}

status_t esp_rename(struct writer *w, const char *path, const char *suffix, const char *to,
                    const char *to_suffix)
{
    uint8_t from[FS_PATH_MAX], dst[FS_PATH_MAX];
    field(from, path, suffix);
    field(dst, to, to_suffix);
    uint64_t t0 = now();
    status_t st = fs_rename_until(w->fs, esp_until(w), from, dst);
    esp_say("%s%s renamed %s%s in %lu ms (%s)", path + 1, suffix, to + 1, to_suffix,
            esp_ms_since(t0), status_str(st));
    return st;
}

status_t esp_sync(struct writer *w)
{
    uint64_t t0 = now();
    status_t st = fs_sync_until(w->fs, esp_until(w));
    esp_say("the ESP synced in %lu ms (%s)", esp_ms_since(t0), status_str(st));
    return st;
}

status_t esp_open(struct writer *w, const char *path, const char *suffix, uint32_t flags,
                  struct jfile *f)
{
    uint8_t p[FS_PATH_MAX];
    handle_t ch, buf;
    uint64_t size;
    field(p, path, suffix);
    status_t st = fs_open_until(w->fs, esp_until(w), p, flags, &ch, &buf, &size);
    return st == OK ? file_adopt(ch, buf, flags, f) : st;
}

/* Exactly n bytes of f at off into dst (ERR_IO: the file is shorter), a
 * transfer buffer's worth per call, each call by esp_until(w). */
status_t esp_read_at(struct writer *w, struct jfile *f, uint64_t off, uint8_t *dst, size_t n)
{
    for (size_t done = 0; done < n;) {
        uint32_t want = n - done < f->buf_size ? (uint32_t)(n - done) : f->buf_size, got = 0;
        status_t st = file_read_until(f->ch, esp_until(w), off + done, want, &got);
        if (st == OK && got != want)
            st = ERR_IO;
        if (st != OK)
            return st;
        memcpy(dst + done, f->buf, want);
        done += want;
    }
    return OK;
}

/* n bytes from src written to f at off, the same way. */
static status_t write_at(struct writer *w, struct jfile *f, uint64_t off, const uint8_t *src,
                         size_t n)
{
    for (size_t done = 0; done < n;) {
        uint32_t want = n - done < f->buf_size ? (uint32_t)(n - done) : f->buf_size, put = 0;
        memcpy(f->buf, src + done, want);
        status_t st = file_write_until(f->ch, esp_until(w), off + done, want, &put);
        if (st == OK && put != want)
            st = ERR_IO;
        if (st != OK)
            return st;
        done += want;
    }
    return OK;
}

status_t esp_hash(struct writer *w, const char *path, const char *suffix, uint64_t size,
                  uint8_t digest[SHA256_BYTES])
{
    struct jfile f;
    status_t st = esp_open(w, path, suffix, FS_READ, &f);
    if (st != OK)
        return st;
    struct sha256 h;
    sha256_init(&h);
    uint64_t t0 = now();
    for (uint64_t off = 0; st == OK && off < size; off += CHUNK) {
        size_t n = size - off < CHUNK ? (size_t)(size - off) : CHUNK;
        say_progress(w, path, suffix, "read", off, size, t0);
        st = esp_read_at(w, &f, off, w->buf, n);   /* ERR_IO: shorter than it was written */
        if (st == OK)
            sha256_add(&h, w->buf, n);
    }
    uint64_t now_size = 0;
    if (st == OK)
        st = file_stat_until(f.ch, esp_until(w), &now_size, NULL);
    if (st == OK && now_size != size)
        st = ERR_IO;   /* longer than it was written */
    file_close(&f);
    if (st == OK)
        sha256_done(&h, digest);
    esp_say("%s%s: %lu KiB read and hashed in %lu ms (%s)", path + 1, suffix,
            (unsigned long)(size >> 10), esp_ms_since(t0), status_str(st));
    return st;
}

status_t esp_write_file(struct writer *w, const char *path, const char *suffix, uint64_t size,
                        uint32_t step, source_t next, void *ctx)
{
    struct jfile f;
    /* FS_GATHER: fat sends the file in 64 KiB writes, not a write per
     * sector (a cluster, on the ESP): what makes a real stick quick. */
    status_t st = esp_open(w, path, suffix, FS_WRITE | FS_CREATE | FS_TRUNCATE | FS_GATHER, &f);
    if (st != OK)
        return st;
    uint64_t t0 = now(), done = 0;
    for (uint64_t off = 0; st == OK && off < size; off += CHUNK) {
        size_t n = size - off < CHUNK ? (size_t)(size - off) : CHUNK;
        say_progress(w, path, suffix, "written", off, size, t0);
        if (off >= size / 2 && off < size / 2 + CHUNK)
            st = esp_inject(w, step);   /* half of it written: a test's failure */
        if (st == OK)
            st = next(w, ctx, off, w->buf, n);
        if (st == OK)
            st = write_at(w, &f, off, w->buf, n);
        done += st == OK ? n : 0;
    }
    uint64_t written = now();
    if (st == OK)
        st = file_sync_until(f.ch, esp_until(w));
    file_close(&f);
    esp_say("%s%s: %lu of %lu KiB written in %lu ms, synced and closed in %lu ms (%s)", path + 1,
            suffix, (unsigned long)(done >> 10), (unsigned long)(size >> 10),
            (unsigned long)((written - t0) / NS_PER_MS), esp_ms_since(written), status_str(st));
    return st;
}

status_t esp_from_vmo(struct writer *w, void *ctx, uint64_t off, uint8_t *buf, size_t n)
{
    (void)w;
    return jam_vmo_read(*(handle_t *)ctx, off, buf, n);
}

/* esp_write_file's source: a file on the stick, hashed as it is read. */
struct from_file {
    struct jfile  f;
    struct sha256 h;
};

static status_t from_file(struct writer *w, void *ctx, uint64_t off, uint8_t *buf, size_t n)
{
    struct from_file *s = ctx;
    status_t st = esp_read_at(w, &s->f, off, buf, n);
    if (st == OK)
        sha256_add(&s->h, buf, n);
    return st;
}

/* The file at src copied to dst + NEW and read back: the copy must have
 * the SHA-256 of what was read. */
static status_t copy_file(struct writer *w, const char *src, const char *dst)
{
    struct from_file s;
    status_t st = esp_open(w, src, "", FS_READ, &s.f);
    if (st != OK)
        return st;
    uint64_t n = s.f.size;
    sha256_init(&s.h);
    st = n && n <= UPDATE_FILE_MAX
             ? esp_write_file(w, dst, NEW, n, UPDATE_WRITE_NONE, from_file, &s)
             : ERR_IO;
    file_close(&s.f);
    uint8_t sha[SHA256_BYTES], back[SHA256_BYTES];
    if (st == OK) {
        sha256_done(&s.h, sha);
        st = esp_hash(w, dst, NEW, n, back);
    }
    if (st == OK && memcmp(back, sha, SHA256_BYTES))
        st = ERR_IO;   /* the stick didn't keep what was written */
    return st;
}

status_t esp_size_of(struct writer *w, const char *path, const char *suffix, uint64_t *size)
{
    uint8_t p[FS_PATH_MAX], dir = 0;
    uint64_t mtime;
    field(p, path, suffix);
    status_t st = fs_stat_until(w->fs, esp_until(w), p, size, &dir, &mtime);
    return st == OK && dir ? ERR_WRONG_TYPE : st;
}

bool esp_exists(struct writer *w, const char *path, const char *suffix)
{
    uint64_t size;
    return esp_size_of(w, path, suffix, &size) == OK;
}

/* Do both of the pair's names (cur or prev) hold a file? */
static bool whole(struct writer *w, const char *const pair[UPDATE_FILES])
{
    return esp_exists(w, pair[UPDATE_KERNEL], "") && esp_exists(w, pair[UPDATE_BOOTFS], "");
}

/* Does the stick's build hold exactly the new build (sizes, then SHA-256s)? */
static bool has_new(struct writer *w)
{
    struct esp_write *j = w->job;
    for (unsigned f = 0; f < UPDATE_FILES; f++) {
        uint64_t n = 0;
        if (esp_size_of(w, cur[f], "", &n) != OK || n != j->size[f])
            return false;
    }
    for (unsigned f = 0; f < UPDATE_FILES; f++) {
        uint8_t got[SHA256_BYTES];
        if (esp_hash(w, cur[f], "", j->size[f], got) != OK ||
            memcmp(got, j->sha256[f], SHA256_BYTES))
            return false;
    }
    return true;
}

/* One change of names of the swap: path + suffix renamed to + to_suffix,
 * or removed (to NULL; not there is fine). Counted: the test's stop
 * (UPDATE_OFFER_STOP) ends the write right after the stop_at-th, with
 * ERR_CANCELED and no clean-up, as a power cut there would. */
static status_t swap_op(struct writer *w, const char *path, const char *suffix, const char *to,
                        const char *to_suffix)
{
    status_t st = to ? esp_rename(w, path, suffix, to, to_suffix) : esp_unlink(w, path, suffix);
    if (st == OK && ++w->ops == w->job->stop_at) {
        esp_say("stopping dead after change %u of the swap, as the test asked", w->ops);
        w->stopped = true;
        return ERR_CANCELED;
    }
    return st;
}

/* The old build's file f back at its own name, from wherever the swap had
 * moved it (the new build's file there, if any, removed). */
static status_t old_back(struct writer *w, unsigned f)
{
    if (w->old_at[f] == AT_CUR || w->old_at[f] == AT_NONE)
        return OK;
    const char *from = w->old_at[f] == AT_OLD ? cur[f] : prev[f];
    const char *sfx = w->old_at[f] == AT_OLD ? OLD : "";
    status_t st = esp_unlink(w, cur[f], "");
    if (st == OK)
        st = esp_rename(w, from, sfx, cur[f], "");
    if (st == OK)
        w->old_at[f] = AT_CUR;
    return st;
}

/* An earlier write that stopped part way through its swap (a power cut,
 * a test's stop) left *.old files: with both the default entry's names
 * there the new build was in, so its renames are finished (the old files
 * become the previous build); else they are undone (the old files back
 * in the default entry's names). Either way a whole build per pair. */
static status_t settle(struct writer *w)
{
    bool old[UPDATE_FILES], whole = true, any = false;
    for (unsigned f = 0; f < UPDATE_FILES; f++) {
        old[f] = esp_exists(w, cur[f], OLD);
        any |= old[f];
        whole &= esp_exists(w, cur[f], "");
    }
    if (!any)
        return OK;
    esp_say("an earlier write stopped in its swap: %s it", whole ? "finishing" : "undoing");
    status_t st = OK;
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++) {
        if (!old[f])
            continue;
        if (whole) {
            st = esp_unlink(w, prev[f], "");
            if (st == OK)
                st = esp_rename(w, cur[f], OLD, prev[f], "");
        } else {
            w->old_at[f] = AT_OLD;
            st = old_back(w, f);
        }
    }
    return st == OK ? esp_sync(w) : st;
}

/* ROOM: an earlier write settled and its leftovers gone; nothing to write
 * if the stick has the new build already; room for the new build. */
static status_t room(struct writer *w)
{
    status_t st = esp_inject(w, UPDATE_WRITE_ROOM);
    if (st == OK)
        st = settle(w);
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++) {
        st = esp_unlink(w, cur[f], NEW);
        if (st == OK)
            st = esp_unlink(w, prev[f], NEW);
    }
    if (st == OK && has_new(w)) {
        esp_say("the stick has this build already: nothing to write");
        w->already = true;
        return OK;
    }
    uint64_t total = 0, free_bytes = 0, need = 0;
    uint8_t ro = 0, label[16];
    if (st == OK)
        st = fs_statfs_until(w->fs, esp_until(w), &total, &free_bytes, &ro, label);
    bool copy = st == OK && !whole(w, prev) && whole(w, cur);   /* copy_previous's */
    for (unsigned f = 0; f < UPDATE_FILES; f++) {
        uint64_t n = 0;
        need += w->job->size[f] + SLACK;
        if (copy && esp_size_of(w, cur[f], "", &n) == OK)
            need += n + SLACK;
    }
    if (st == OK && ro)
        st = ERR_ACCESS_DENIED;   /* devmgr said writable, the volume says not */
    if (st == OK && free_bytes < need)
        st = ERR_NO_SPACE;
    esp_say("%lu KiB free, %lu KiB needed", (unsigned long)(free_bytes >> 10),
            (unsigned long)(need >> 10));
    return st;
}

/* NEW: the new build under temporary names, each checked against the
 * manifest's SHA-256 as the stick gives it back. */
static status_t write_new(struct writer *w)
{
    struct esp_write *j = w->job;
    status_t st = OK;
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++)
        st = esp_write_file(w, cur[f], NEW, j->size[f], UPDATE_WRITE_NEW, esp_from_vmo,
                            &j->vmo[f]);
    if (st == OK)
        st = esp_sync(w);
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++) {
        uint8_t back[SHA256_BYTES];
        st = esp_hash(w, cur[f], NEW, j->size[f], back);
        if (st == OK && memcmp(back, j->sha256[f], SHA256_BYTES))
            st = ERR_IO;   /* the stick didn't keep what was written */
    }
    return st;
}

/* No whole previous build (none was ever written, or a failure after its
 * removal): the stick's build is copied as it first, and read back, so
 * that the previous-build entry boots a whole build while the renames move
 * the stick's (the one case the renames alone can't keep bootable). */
static status_t copy_previous(struct writer *w)
{
    if (whole(w, prev) || !whole(w, cur))
        return OK;
    esp_say("no whole previous build: the stick's build copied as it first");
    status_t st = OK;
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++) {
        st = esp_unlink(w, prev[f], "");   /* half a previous build */
        if (st == OK)
            st = copy_file(w, cur[f], prev[f]);
    }
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++)
        st = esp_rename(w, prev[f], NEW, prev[f], "");
    return st == OK ? esp_sync(w) : st;
}

/* SWITCH: the stick's build renamed aside (1), the new one into its names
 * (2); copied as the previous build first if there is none (copy_previous). */
static status_t switch_names(struct writer *w)
{
    status_t st = copy_previous(w);
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++) {
        w->old_at[f] = esp_exists(w, cur[f], "") ? AT_CUR : AT_NONE;
        if (w->old_at[f] == AT_CUR)
            st = swap_op(w, cur[f], "", cur[f], OLD);
        if (st == OK && w->old_at[f] == AT_CUR)
            w->old_at[f] = AT_OLD;
    }
    if (st == OK)
        st = esp_sync(w);
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++) {
        st = swap_op(w, cur[f], NEW, cur[f], "");
        if (st == OK && f == UPDATE_KERNEL)
            st = esp_inject(w, UPDATE_WRITE_SWITCH);   /* the new kernel, no boot image */
    }
    return st == OK ? esp_sync(w) : st;
}

/* PREV: the older previous build removed (3), the old build renamed into
 * its names (4). Only if the old build was whole: else the previous build
 * stays as it is. */
static status_t keep_previous(struct writer *w)
{
    if (w->old_at[UPDATE_KERNEL] != AT_OLD || w->old_at[UPDATE_BOOTFS] != AT_OLD) {
        esp_say("the stick had no whole build to keep: the previous build stays");
        for (unsigned f = 0; f < UPDATE_FILES; f++)
            (void)esp_unlink(w, cur[f], OLD);   /* half a build: nothing boots it */
        return OK;
    }
    status_t st = OK;
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++)
        st = swap_op(w, prev[f], "", NULL, NULL);
    if (st == OK)
        st = esp_sync(w);
    if (st == OK)
        st = esp_inject(w, UPDATE_WRITE_PREV);   /* no previous build now, the new one in */
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++) {
        st = swap_op(w, cur[f], OLD, prev[f], "");
        if (st == OK)
            w->old_at[f] = AT_PREV;
    }
    return st == OK ? esp_sync(w) : st;
}

/* After a failure in the swap: the old build back in the default entry's
 * names (what the stick boots then is the answer's). */
static uint32_t put_back(struct writer *w)
{
    bool new_in = w->ops >= 2 * UPDATE_FILES;   /* the swap's step 2 was done */
    status_t st = OK;
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++)
        st = old_back(w, f);
    if (st == OK)
        st = esp_sync(w);
    printf("init: update: the stick write failed in its swap (%s): the old build %s\n",
           status_str(w->job->st), st == OK ? "is back as the stick's" : "couldn't be put back");
    if (st == OK)
        return UPDATE_STICK_OLD;
    return new_in ? UPDATE_STICK_NEW_ALONE : UPDATE_STICK_PREVIOUS;
}

/* What the stick boots after a stop with no clean-up (a test's), by how
 * far the swap got: the default entry from change 4 on (the new build),
 * the previous-build entry before. */
static uint32_t stopped_stick(const struct writer *w)
{
    if (w->ops >= 2 * UPDATE_FILES)
        return w->ops >= 4 * UPDATE_FILES ? UPDATE_STICK_NEW : UPDATE_STICK_NEW_ALONE;
    return w->ops ? UPDATE_STICK_PREVIOUS : UPDATE_STICK_OLD;
}

/* The steps in order (ROOM, NEW, SWITCH, PREV); j->step and j->st say
 * where it stopped; j->stick what the stick boots now. */
static void steps(struct writer *w)
{
    struct esp_write *j = w->job;
    static const struct {
        uint32_t step;
        status_t (*fn)(struct writer *);
    } order[] = {
        { UPDATE_WRITE_ROOM, room }, { UPDATE_WRITE_NEW, write_new },
        { UPDATE_WRITE_SWITCH, switch_names }, { UPDATE_WRITE_PREV, keep_previous },
    };
    j->stick = UPDATE_STICK_OLD;
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        uint64_t t0 = now();
        j->step = order[i].step;
        esp_say("%s ...", update_write_step_str(j->step));
        j->st = order[i].fn(w);
        esp_say("%s: %s in %lu ms", update_write_step_str(j->step), status_str(j->st),
                esp_ms_since(t0));
        if (j->st != OK || w->already || w->stopped)
            break;
    }
    if (j->st == OK) {
        j->stick = UPDATE_STICK_NEW;
        return;
    }
    if (w->stopped) {   /* a test's power cut: nothing more is written */
        j->stick = stopped_stick(w);
        return;
    }
    if (j->st == ERR_TIMED_OUT)
        esp_say("no time left (the write may take %lu s): stopping here",
                (unsigned long)(WRITE_LIMIT / NS_PER_S));
    w->deadline = now() + RECOVER_LIMIT;   /* the clean-up's own time */
    if (j->step == UPDATE_WRITE_SWITCH || j->step == UPDATE_WRITE_PREV)
        j->stick = put_back(w);
    for (unsigned f = 0; f < UPDATE_FILES; f++) {   /* best effort: ROOM removes them anyway */
        (void)esp_unlink(w, cur[f], NEW);
        (void)esp_unlink(w, prev[f], NEW);
    }
    (void)esp_sync(w);
}

/* The stick's kernel and boot image as they are now: noted for reboot.c,
 * which then starts the stored kernel (the new build) without reading
 * them. */
static void note_files(struct writer *w)
{
    struct esp_write *j = w->job;
    j->noted = true;
    for (unsigned f = 0; f < UPDATE_FILES && j->noted; f++) {
        uint8_t p[FS_PATH_MAX], dir = 0;
        field(p, cur[f], "");
        j->noted = fs_stat_until(w->fs, esp_until(w), p, &j->file_size[f], &dir,
                                 &j->mtime[f]) == OK && !dir;
    }
}

/* devmgr's ESP_WRITE: writable (*fs: the channel) or read-only again. */
static status_t esp_mode(handle_t esp, bool writable, handle_t *fs)
{
    struct devmgr_rep r;
    handle_t h = HANDLE_INVALID;
    uint32_t nh = 0;
    status_t st = devmgr_call(esp, DEVMGR_ESP_WRITE, 0, 0, writable ? DEVMGR_ESP_WRITABLE : 0,
                              &r, &h, 1, &nh, now() + ESP_WAIT);
    if (st == OK && writable && nh != 1)
        st = ERR_INTERNAL;
    if (st != OK && nh)
        jam_handle_close(h);
    if (st == OK && writable)
        *fs = h;
    return st;
}

void esp_write_build(struct esp_write *j)
{
    uint64_t t0 = now();
    struct writer w = { .job = j };
    j->step = UPDATE_WRITE_OPEN;
    j->stick = UPDATE_STICK_OLD;
    j->noted = false;
    esp_say("asking devmgr for the ESP read-write ...");
    j->st = j->esp ? esp_mode(j->esp, true, &w.fs) : ERR_NOT_FOUND;
    esp_say("the ESP is %s (%s) in %lu ms", j->st == OK ? "writable" : "not writable",
            status_str(j->st), esp_ms_since(t0));
    w.buf = j->st == OK ? malloc(CHUNK) : NULL;
    if (j->st == OK && !w.buf)
        j->st = ERR_NO_MEMORY;
    w.deadline = now() + WRITE_LIMIT;
    if (j->st == OK) {
        steps(&w);
        note_files(&w);
    }
    free(w.buf);
    if (w.fs) {
        jam_handle_close(w.fs);   /* ours is the only other end: nobody else has it */
        uint64_t t1 = now();
        status_t st = esp_mode(j->esp, false, NULL);
        esp_say("the ESP read-only again in %lu ms (%s)", esp_ms_since(t1), status_str(st));
        if (st != OK)
            printf("init: update: the ESP didn't go back to read-only (%s): /esp stays away "
                   "until the next boot\n", status_str(st));
        if (st != OK && j->st == OK)
            j->st = st;   /* written, but the volume isn't marked clean */
    }
    if (j->st == OK)
        j->step = UPDATE_WRITE_DONE;
    uint64_t ms = (now() - t0) / NS_PER_MS;
    j->write_ms = ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}
