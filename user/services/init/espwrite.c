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
 * The order, so that the stick boots whenever it is pulled or the power
 * goes (or the ESP's service dies):
 *   ROOM    an earlier write's leftovers (*.new) removed; room for the
 *           stick's build and the new one checked (else nothing changes);
 *   PREV    the older previous build removed; the stick's build copied
 *           (not moved) as boot/prev-jamos.elf.new and prev-bootfs.img.new,
 *           read back and compared with what was read, then renamed: the
 *           boot menu's "Jam OS (previous build)" boots the stick's build
 *           now, and the default entry still does;
 *   NEW     the new build written from init's own checked copies as
 *           boot/jamos.elf.new and bootfs.img.new, synced, read back, and
 *           their SHA-256s compared with the manifest's;
 *   SWITCH  boot/jamos.elf removed and jamos.elf.new renamed to it, then
 *           the same for bootfs.img. A FAT rename can't replace a file, so
 *           for those few directory writes the default entry has no
 *           kernel, or the new kernel with the old boot image; the
 *           previous-build entry boots the old build throughout.
 * A failure stops the write where it is and removes its temporary files.
 * After the switch has begun, the default entry is put back to the old
 * build (copied again from the previous build's files) if the ESP's
 * service still answers; if it doesn't, the old build boots from "Jam OS
 * (previous build)". The answer says which (enum update_stick).
 *
 * The read-back goes through the ESP's service, which may answer from its
 * cache: it proves what the service was given and wrote, not the flash's
 * cells.
 *
 * Time. Every call waits at most until the write's deadline (until()):
 * the steps get WRITE_LIMIT in all, the clean-up after a failure
 * RECOVER_LIMIT more, and making the ESP read-only again ESP_WAIT, so a
 * stick that stops answering ends in "not written" well inside the 300 s
 * bin/update waits. Each step and file logs its time as it goes (say(),
 * a long file every SAY_EVERY), so a slow stick shows in the log. */
#include <devmgr.h>
#include <fs_idl.h>
#include <update.h>
#include "init.h"

#define CHUNK      (1u << 20)         /* bytes read, then written, at a time: as much as
                                       * fat holds of an FS_GATHER file (its hold.c) */
#define ESP_WAIT   (30 * NS_PER_S)    /* devmgr's ESP_WRITE: a stop in order, then a start */
#define WRITE_LIMIT   (120 * NS_PER_S)   /* the steps, all of them */
#define RECOVER_LIMIT (60 * NS_PER_S)    /* after a failure: put back, leftovers removed */
#define SLACK      (1u << 20)         /* room kept spare per file: clusters, directory */
#define SAY_EVERY  (2 * NS_PER_S)     /* a long file's progress: a line at most this often */

/* The ESP's files, by the paths fat takes (the mount's root is "/"). */
static const char *const cur[UPDATE_FILES] = { "/boot/jamos.elf", "/boot/bootfs.img" };
static const char *const prev[UPDATE_FILES] = { "/boot/prev-jamos.elf", "/boot/prev-bootfs.img" };

/* A write under way. */
struct writer {
    struct esp_write *job;
    handle_t fs;                          /* the ESP's writable `fs` channel */
    uint8_t *buf;                         /* CHUNK bytes */
    uint64_t old_size[UPDATE_FILES];      /* the stick's build: its files' sizes */
    uint8_t  old_sha[UPDATE_FILES][SHA256_BYTES];   /* ... and SHA-256s, as read */
    uint64_t said;                        /* uptime ns of the last progress line */
    uint64_t deadline;                    /* no call may wait past this (uptime ns) */
};

/* A call's deadline: FS_CALL_TIMEOUT from now, never past the write's. A
 * stick (or ESP service) that stops answering ends the write within
 * WRITE_LIMIT, then the clean-up within RECOVER_LIMIT, then the remount
 * back within ESP_WAIT: `update -w` (bin/update waits 300 s) always gets
 * its answer, and the answer says the write failed. */
static uint64_t until(const struct writer *w)
{
    uint64_t d = now() + FS_CALL_TIMEOUT;
    return d < w->deadline ? d : w->deadline;
}

/* A progress line. Every step and file says how long it took, and a long
 * file how far it is every SAY_EVERY, so the log (streamed to the Mac on
 * the PC) always shows where a slow write is. */
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("init: update: write: %s\n", line);
}

static unsigned long ms_since(uint64_t t0)
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
    say("%s%s: %lu of %lu KiB %s, %lu ms so far", path + 1, suffix, (unsigned long)(off >> 10),
        (unsigned long)(size >> 10), what, ms_since(t0));
}

/* fs.idl's path field for path, with suffix (".new" or ""). */
static void field(uint8_t out[FS_PATH_MAX], const char *path, const char *suffix)
{
    memset(out, 0, FS_PATH_MAX);
    snprintf((char *)out, FS_PATH_MAX, "%s%s", path, suffix);
}

/* The test's injected failure (UPDATE_OFFER_FAIL): ERR_IO once, at that step. */
static status_t inject(struct writer *w, uint32_t step)
{
    if (step == UPDATE_WRITE_NONE || w->job->fail_at != step)
        return OK;
    w->job->fail_at = UPDATE_WRITE_NONE;
    printf("init: update: the stick write fails here, as the test asked (%s)\n",
           update_write_step_str(step));
    return ERR_IO;
}

static status_t unlink_file(struct writer *w, const char *path, const char *suffix)
{
    uint8_t p[FS_PATH_MAX];
    field(p, path, suffix);
    status_t st = fs_unlink_until(w->fs, until(w), p);
    return st == ERR_NOT_FOUND ? OK : st;
}

static status_t rename_to(struct writer *w, const char *path, const char *suffix,
                          const char *to)
{
    uint8_t from[FS_PATH_MAX], dst[FS_PATH_MAX];
    field(from, path, suffix);
    field(dst, to, "");
    uint64_t t0 = now();
    status_t st = fs_rename_until(w->fs, until(w), from, dst);
    say("%s%s renamed %s in %lu ms (%s)", path + 1, suffix, to + 1, ms_since(t0), status_str(st));
    return st;
}

static status_t sync_esp(struct writer *w)
{
    uint64_t t0 = now();
    status_t st = fs_sync_until(w->fs, until(w));
    say("the ESP synced in %lu ms (%s)", ms_since(t0), status_str(st));
    return st;
}

/* path + suffix opened with flags into *f. */
static status_t open_file(struct writer *w, const char *path, const char *suffix, uint32_t flags,
                          struct jfile *f)
{
    uint8_t p[FS_PATH_MAX];
    handle_t ch, buf;
    uint64_t size;
    field(p, path, suffix);
    status_t st = fs_open_until(w->fs, until(w), p, flags, &ch, &buf, &size);
    return st == OK ? file_adopt(ch, buf, flags, f) : st;
}

/* Exactly n bytes of f at off into dst (ERR_IO: the file is shorter), a
 * transfer buffer's worth per call, each call by until(w). */
static status_t read_at(struct writer *w, struct jfile *f, uint64_t off, uint8_t *dst, size_t n)
{
    for (size_t done = 0; done < n;) {
        uint32_t want = n - done < f->buf_size ? (uint32_t)(n - done) : f->buf_size, got = 0;
        status_t st = file_read_until(f->ch, until(w), off + done, want, &got);
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
        status_t st = file_write_until(f->ch, until(w), off + done, want, &put);
        if (st == OK && put != want)
            st = ERR_IO;
        if (st != OK)
            return st;
        done += want;
    }
    return OK;
}

/* path + suffix, which must be exactly size bytes: its SHA-256. */
static status_t hash_file(struct writer *w, const char *path, const char *suffix, uint64_t size,
                          uint8_t digest[SHA256_BYTES])
{
    struct jfile f;
    status_t st = open_file(w, path, suffix, FS_READ, &f);
    if (st != OK)
        return st;
    struct sha256 h;
    sha256_init(&h);
    uint64_t t0 = now();
    for (uint64_t off = 0; st == OK && off < size; off += CHUNK) {
        size_t n = size - off < CHUNK ? (size_t)(size - off) : CHUNK;
        say_progress(w, path, suffix, "read back", off, size, t0);
        st = read_at(w, &f, off, w->buf, n);   /* ERR_IO: shorter than it was written */
        if (st == OK)
            sha256_add(&h, w->buf, n);
    }
    uint64_t now_size = 0;
    if (st == OK)
        st = file_stat_until(f.ch, until(w), &now_size, NULL);
    if (st == OK && now_size != size)
        st = ERR_IO;   /* longer than it was written */
    file_close(&f);
    if (st == OK)
        sha256_done(&h, digest);
    say("%s%s: %lu KiB read back in %lu ms (%s)", path + 1, suffix, (unsigned long)(size >> 10),
        ms_since(t0), status_str(st));
    return st;
}

/* Where write_file's bytes come from: next(w, ctx, off, buf, n) fills buf. */
typedef status_t (*source_t)(struct writer *, void *, uint64_t, uint8_t *, size_t);

/* The bytes `next` gives, size of them, written to path + ".new" (created
 * or emptied first) and synced. */
static status_t write_file(struct writer *w, const char *path, uint64_t size, uint32_t step,
                           source_t next, void *ctx)
{
    struct jfile f;
    /* FS_GATHER: fat sends the file in 64 KiB writes, not a write per
     * sector (a cluster, on the ESP): what makes a real stick quick. */
    status_t st = open_file(w, path, ".new", FS_WRITE | FS_CREATE | FS_TRUNCATE | FS_GATHER, &f);
    if (st != OK)
        return st;
    uint64_t t0 = now(), done = 0;
    for (uint64_t off = 0; st == OK && off < size; off += CHUNK) {
        size_t n = size - off < CHUNK ? (size_t)(size - off) : CHUNK;
        say_progress(w, path, ".new", "written", off, size, t0);
        if (off >= size / 2 && off < size / 2 + CHUNK)
            st = inject(w, step);   /* half of it written: a test's failure */
        if (st == OK)
            st = next(w, ctx, off, w->buf, n);
        if (st == OK)
            st = write_at(w, &f, off, w->buf, n);
        done += st == OK ? n : 0;
    }
    uint64_t written = now();
    if (st == OK)
        st = file_sync_until(f.ch, until(w));
    file_close(&f);
    say("%s.new: %lu of %lu KiB written in %lu ms, synced and closed in %lu ms (%s)", path + 1,
        (unsigned long)(done >> 10), (unsigned long)(size >> 10),
        (unsigned long)((written - t0) / NS_PER_MS), ms_since(written), status_str(st));
    return st;
}

/* write_file's source: one of init's checked copies. */
static status_t from_vmo(struct writer *w, void *ctx, uint64_t off, uint8_t *buf, size_t n)
{
    (void)w;
    return jam_vmo_read(*(handle_t *)ctx, off, buf, n);
}

/* write_file's source: a file on the stick, hashed as it is read. */
struct from_file {
    struct jfile  f;
    struct sha256 h;
};

static status_t from_file(struct writer *w, void *ctx, uint64_t off, uint8_t *buf, size_t n)
{
    struct from_file *s = ctx;
    status_t st = read_at(w, &s->f, off, buf, n);
    if (st == OK)
        sha256_add(&s->h, buf, n);
    return st;
}

/* The file at src copied to dst + ".new" and read back: *size its bytes,
 * sha the SHA-256 of what was read, which the copy must have. */
static status_t copy_file(struct writer *w, const char *src, const char *dst, uint32_t step,
                          uint64_t *size, uint8_t sha[SHA256_BYTES])
{
    struct from_file s;
    status_t st = open_file(w, src, "", FS_READ, &s.f);
    if (st != OK)
        return st;
    uint64_t n = s.f.size;
    sha256_init(&s.h);
    st = n && n <= UPDATE_FILE_MAX ? write_file(w, dst, n, step, from_file, &s) : ERR_IO;
    file_close(&s.f);
    uint8_t back[SHA256_BYTES];
    if (st == OK) {
        sha256_done(&s.h, sha);
        st = hash_file(w, dst, ".new", n, back);
    }
    if (st == OK && memcmp(back, sha, SHA256_BYTES))
        st = ERR_IO;   /* the stick didn't keep what was written */
    if (st == OK)
        *size = n;
    return st;
}

/* ROOM: an earlier write's leftovers gone; room for two builds (the new
 * one, and a copy of the stick's) once the older previous build goes. */
static status_t room(struct writer *w)
{
    status_t st = inject(w, UPDATE_WRITE_ROOM);
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++) {
        st = unlink_file(w, cur[f], ".new");
        if (st == OK)
            st = unlink_file(w, prev[f], ".new");
    }
    uint64_t total = 0, free_bytes = 0, need = 0, have = 0;
    uint8_t ro = 0, label[16];
    if (st == OK)
        st = fs_statfs_until(w->fs, until(w), &total, &free_bytes, &ro, label);
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++) {
        uint64_t size = 0, mtime;
        uint8_t dir;
        uint8_t p[FS_PATH_MAX];
        field(p, prev[f], "");
        if (fs_stat_until(w->fs, until(w), p, &size, &dir, &mtime) == OK)
            have += size;   /* the older previous build: it goes first */
        field(p, cur[f], "");
        st = fs_stat_until(w->fs, until(w), p, &size, &dir, &mtime);
        need += size + w->job->size[f] + 2 * SLACK;
    }
    if (st == OK && ro)
        st = ERR_ACCESS_DENIED;   /* devmgr said writable, the volume says not */
    if (st == OK && free_bytes + have < need)
        st = ERR_NO_SPACE;
    say("%lu KiB free, %lu KiB more from the older previous build, %lu KiB needed",
        (unsigned long)(free_bytes >> 10), (unsigned long)(have >> 10),
        (unsigned long)(need >> 10));
    return st;
}

/* PREV: the stick's build kept as the previous one. */
static status_t keep_previous(struct writer *w)
{
    status_t st = OK;
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++)
        st = unlink_file(w, prev[f], "");
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++)
        st = copy_file(w, cur[f], prev[f], UPDATE_WRITE_PREV, &w->old_size[f], w->old_sha[f]);
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++)
        st = rename_to(w, prev[f], ".new", prev[f]);
    return st == OK ? sync_esp(w) : st;
}

/* NEW: the new build under temporary names, each checked against the
 * manifest's SHA-256 as the stick gives it back. */
static status_t write_new(struct writer *w)
{
    struct esp_write *j = w->job;
    status_t st = OK;
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++)
        st = write_file(w, cur[f], j->size[f], UPDATE_WRITE_NEW, from_vmo, &j->vmo[f]);
    if (st == OK)
        st = sync_esp(w);
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++) {
        uint8_t back[SHA256_BYTES];
        st = hash_file(w, cur[f], ".new", j->size[f], back);
        if (st == OK && memcmp(back, j->sha256[f], SHA256_BYTES))
            st = ERR_IO;   /* the stick didn't keep what was written */
    }
    return st;
}

/* SWITCH: the new files take the names the boot menu's default entry reads. */
static status_t switch_names(struct writer *w)
{
    status_t st = OK;
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++) {
        st = unlink_file(w, cur[f], "");
        if (st == OK)
            st = rename_to(w, cur[f], ".new", cur[f]);
        if (st == OK && f == UPDATE_KERNEL)
            st = inject(w, UPDATE_WRITE_SWITCH);   /* the new kernel, the old boot image */
    }
    return st == OK ? sync_esp(w) : st;
}

/* After a failed switch: the default entry's files put back to the old
 * build, copied again from the previous build's (checked against what was
 * read from the stick before). */
static status_t put_back(struct writer *w)
{
    status_t st = OK;
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++) {
        uint64_t size = 0;
        uint8_t sha[SHA256_BYTES];
        st = copy_file(w, prev[f], cur[f], UPDATE_WRITE_NONE, &size, sha);
        if (st == OK && (size != w->old_size[f] || memcmp(sha, w->old_sha[f], SHA256_BYTES)))
            st = ERR_IO;
    }
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++) {
        st = unlink_file(w, cur[f], "");
        if (st == OK)
            st = rename_to(w, cur[f], ".new", cur[f]);
    }
    return st == OK ? sync_esp(w) : st;
}

/* The steps from ROOM on, in order; j->step and j->st say where it
 * stopped; j->stick what the stick boots now. */
static void steps(struct writer *w)
{
    struct esp_write *j = w->job;
    static status_t (*const step[])(struct writer *) = {
        [UPDATE_WRITE_ROOM] = room, [UPDATE_WRITE_PREV] = keep_previous,
        [UPDATE_WRITE_NEW] = write_new, [UPDATE_WRITE_SWITCH] = switch_names,
    };
    j->stick = UPDATE_STICK_OLD;
    for (j->step = UPDATE_WRITE_ROOM; j->step <= UPDATE_WRITE_SWITCH; j->step++) {
        uint64_t t0 = now();
        say("%s ...", update_write_step_str(j->step));
        j->st = step[j->step](w);
        say("%s: %s in %lu ms", update_write_step_str(j->step), status_str(j->st), ms_since(t0));
        if (j->st != OK)
            break;
    }
    if (j->st == OK) {
        j->stick = UPDATE_STICK_NEW;
        return;
    }
    if (j->st == ERR_TIMED_OUT)
        say("no time left (the write may take %lu s): stopping here",
            (unsigned long)(WRITE_LIMIT / NS_PER_S));
    w->deadline = now() + RECOVER_LIMIT;   /* the clean-up's own time */
    if (j->step == UPDATE_WRITE_SWITCH) {
        status_t back = put_back(w);
        j->stick = back == OK ? UPDATE_STICK_OLD : UPDATE_STICK_PREVIOUS;
        printf("init: update: the stick write failed while switching the names (%s): the old "
               "build %s\n", status_str(j->st),
               back == OK ? "is back as the stick's" : "boots from \"Jam OS (previous build)\"");
    }
    for (unsigned f = 0; f < UPDATE_FILES; f++) {   /* best effort: ROOM removes them anyway */
        (void)unlink_file(w, cur[f], ".new");
        (void)unlink_file(w, prev[f], ".new");
    }
    (void)sync_esp(w);
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
        j->noted = fs_stat_until(w->fs, until(w), p, &j->file_size[f], &dir,
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
    say("asking devmgr for the ESP read-write ...");
    j->st = j->esp ? esp_mode(j->esp, true, &w.fs) : ERR_NOT_FOUND;
    say("the ESP is %s (%s) in %lu ms", j->st == OK ? "writable" : "not writable",
        status_str(j->st), ms_since(t0));
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
        say("the ESP read-only again in %lu ms (%s)", ms_since(t1), status_str(st));
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
