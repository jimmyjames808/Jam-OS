/* init's installer (initctl.install, abi/idl/initctl.idl; M12.7 stage 3,
 * docs/M12.7-PLAN.md "Stage 3: the installer and the boot path"): Jam OS
 * written to an internal drive so the PC boots from it without the stick.
 * The shell has shown the owner the drive and had the last four characters
 * of its serial typed (and `erase`, for a drive that isn't empty) before it
 * calls; init passes them on and does the work on a thread of its own, like
 * update.c's stick write (the loop goes on serving), with the same pieces:
 * espwrite.c's deadlines, writes with FS_GATHER, syncs and read-backs. It
 * asks volumes, on the install channel (volinstall, init's alone), for the
 * disk's partitions one at a time as writable channels in no namespace
 * (open_data, open_esp); volumes and then the driver refuse everything that
 * is not a Jam OS disk, so nothing here decides what may be written.
 *
 * The order, so that a cut at any point leaves a disk that does not boot
 * (Limine finds no menu: the firmware lists it, shows an error and goes on
 * to its next entry) rather than one that boots half a build, and the next
 * `install` finishes it:
 *   1. PREPARE  the disk made a Jam OS disk (volinstall.prepare, the shell's
 *               `disk prepare`: empty, or one cut short, or an erase the
 *               owner confirmed); a Jam OS disk is reused, its /data kept;
 *   2. READ     the sources read into VMOs only init holds, each hashed:
 *               the boot disk's /esp/EFI/BOOT/BOOTX64.EFI (Limine), its
 *               boot menu (boot/limine/limine.conf, or the spare
 *               boot/limine.conf), boot/jamos.elf and boot/bootfs.img (the
 *               build the stick or the boot disk runs);
 *   3. DATA     the boot disk's /data/etc (the settings, the update key's
 *               trust, the network address, the host key) copied to the new
 *               /data/etc, as etc.new, synced, read back, then renamed to
 *               etc: only if the disk has no /etc (a disk installed on
 *               again keeps its /data);
 *   4. ESP      the menu removed first (a reinstall), then Limine, then the
 *               build (jamos.elf, bootfs.img and the same two as
 *               prev-jamos.elf and prev-bootfs.img, so the menu's second
 *               entry boots too), each synced, read back and compared with
 *               its SHA-256, and the menu LAST.
 * The ESP and data partitions are made read-only again at the end, failed
 * or not (volumes marks them clean and they are mounts again).
 *
 * One at a time, and not while an `update` runs or a `disk prepare` waits
 * (all three use the install channel or /esp). Its progress goes to the log
 * as "init: install: step n/6 ..." lines (the tests cut a run at them). */
#include <bootmenu.h>
#include <idl/initctl.h>
#include <idl/volumes.h>
#include <update.h>
#include "espwrite.h"
#include "init.h"

#define INSTALL_STACK (96u << 10)
#define THREAD_END    (2 * NS_PER_S)     /* a finished worker's thread ends within this */
#define PREPARE_WAIT  (660 * NS_PER_S)   /* volinstall.prepare: it gives the format 10 minutes */
#define OPEN_WAIT     (60 * NS_PER_S)    /* volinstall.open_*: a stop in order, then a start */
#define CLOSE_WAIT    (60 * NS_PER_S)
#define DATA_LIMIT    (120 * NS_PER_S)   /* the /data/etc copy */
#define ESP_LIMIT     (300 * NS_PER_S)   /* the ESP's files */
#define SLACK         (1u << 20)         /* room kept spare per file (as espwrite.c) */
#define STEPS         6
#define ETC_FILE_MAX  (1u << 20)         /* a file of /data/etc, at most */
#define ETC_DEPTH     4                  /* directories below /data/etc, at most */
#define ETC_PATH      160                /* a path below /data/etc or /etc.new, at most */
#define ETC_ENTRIES   256                /* files and directories, at most */

/* The disk's files, by the paths fat takes (the ESP's root is "/"). */
static const char LIMINE[] = "/EFI/BOOT/BOOTX64.EFI";
static const char MENU[] = "/boot/limine/limine.conf";
static const char SPARE[] = "/boot/limine.conf";
static const char *const cur[UPDATE_FILES] = { "/boot/jamos.elf", "/boot/bootfs.img" };
static const char *const prev[UPDATE_FILES] = { "/boot/prev-jamos.elf", "/boot/prev-bootfs.img" };
#define NEW  ".new"
#define OLD  ".old"
#define PREV ".prev"
#define SRC_ESP "/esp"   /* the boot disk's ESP, in our namespace */

/* A file read from the boot disk, held in a VMO only init has. */
struct src {
    handle_t vmo;
    uint64_t size;
    uint8_t  sha[SHA256_BYTES];
};

/* An install under way. */
struct inst {
    struct idl_txn txn;      /* where the answer goes: a duplicate of the shell's channel */
    handle_t       vm;       /* a duplicate of the install channel */
    uint32_t       id;       /* the disk (voldisks' id) */
    char           tail[20], word[8];
    uint32_t       tail_len, word_len;
    struct esp_write job;    /* the writer's job: no test hooks, nothing given */
    struct writer  w;
    struct src     efi, menu, file[UPDATE_FILES];
    uint32_t       etc_files;   /* files copied to /data/etc */
    bool           etc_kept;    /* the disk had a /etc already */
    status_t       st;
    uint64_t       t0;
};

static struct inst *busy;        /* the install on the worker (NULL: none) */
static handle_t     worker;      /* its thread */
static bool         worker_stuck;
static uint8_t      worker_stack[INSTALL_STACK];

bool install_busy(void)
{
    return __atomic_load_n(&busy, __ATOMIC_ACQUIRE) != NULL || worker_stuck;
}

static void inst_say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void inst_say(const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printf("init: install: %s\n", line);
}

static void inst_step(unsigned n, const char *what)
{
    inst_say("step %u/%u: %s", n, STEPS, what);
}

/* ---- the sources ------------------------------------------------------------------ */

static void src_free(struct src *s)
{
    if (s->vmo)
        jam_handle_close(s->vmo);
    *s = (struct src){ 0 };
}

/* The file at path (1 .. max bytes; 0 too if empty_ok) into s, and its SHA-256. */
static status_t src_load(struct src *s, const char *path, uint64_t max, bool empty_ok)
{
    status_t st = file_read_vmo(path, max, &s->vmo, &s->size);
    if (st == OK && !s->size && !empty_ok)
        st = ERR_IO;
    uint8_t *buf = st == OK ? malloc(ESP_CHUNK) : NULL;
    if (st == OK && !buf)
        st = ERR_NO_MEMORY;
    struct sha256 h;
    sha256_init(&h);
    for (uint64_t off = 0; st == OK && off < s->size; off += ESP_CHUNK) {
        size_t n = s->size - off < ESP_CHUNK ? (size_t)(s->size - off) : ESP_CHUNK;
        st = jam_vmo_read(s->vmo, off, buf, n);
        if (st == OK)
            sha256_add(&h, buf, n);
    }
    free(buf);
    if (st == OK)
        sha256_done(&h, s->sha);
    inst_say("%s: %lu KiB read (%s)", path, (unsigned long)(s->size >> 10), status_str(st));
    if (st != OK)
        src_free(s);
    return st;
}

/* bootmenu_check's question: is the file on the new disk? */
static bool on_disk(void *ctx, const char *path)
{
    return esp_exists(ctx, path, "");
}

/* The menu of s against <bootmenu.h>'s check, as `update` applies it to a
 * new menu, said in the log. It is only said: the menu is the one this
 * boot's loader read (the stick's, or the boot disk's), copied as it is, and
 * the tests' menus (one entry, `timeout: 0`) don't pass the check but must
 * be installed too. */
static void menu_note(const struct src *s, struct writer *w)
{
    uint8_t *text = s->size <= BOOTMENU_MAX ? malloc((size_t)s->size) : NULL;
    struct bootmenu_result r;
    if (!text || jam_vmo_read(s->vmo, 0, text, (size_t)s->size) != OK) {
        free(text);
        return;
    }
    if (bootmenu_check(text, (size_t)s->size, on_disk, w, &r))
        inst_say("the boot menu passes the update check: %u entries boot", r.entries);
    else
        inst_say("the boot menu would not pass the update check (%s, line %u %s): copied as it is",
                 bootmenu_why_str(r.why), r.line, r.path);
    free(text);
}

/* Step 2: the four sources. */
static status_t read_sources(struct inst *in)
{
    static const char *const build[UPDATE_FILES] = { SRC_ESP "/boot/jamos.elf",
                                                     SRC_ESP "/boot/bootfs.img" };
    status_t st = src_load(&in->efi, SRC_ESP "/EFI/BOOT/BOOTX64.EFI", 8u << 20, false);
    if (st == OK) {
        st = src_load(&in->menu, SRC_ESP "/boot/limine/limine.conf", BOOTMENU_MAX, false);
        if (st != OK)
            st = src_load(&in->menu, SRC_ESP "/boot/limine.conf", BOOTMENU_MAX, false);   /* the spare */
    }
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++)
        st = src_load(&in->file[f], build[f], UPDATE_FILE_MAX, false);
    return st;
}

/* ---- the disk's partitions ------------------------------------------------------- */

/* Partition part (0: the ESP, 1: data) made writable: in->w.fs. */
static status_t open_part(struct inst *in, unsigned part, uint64_t limit)
{
    uint64_t t0 = now();
    status_t st = part ? volinstall_open_data_until(in->vm, now() + OPEN_WAIT, in->id, &in->w.fs)
                       : volinstall_open_esp_until(in->vm, now() + OPEN_WAIT, in->id, &in->w.fs);
    inst_say("the %s is %s (%s) in %lu ms", part ? "data partition" : "ESP",
             st == OK ? "writable" : "not writable", status_str(st), esp_ms_since(t0));
    in->w.deadline = now() + limit;
    return st;
}

/* ... and read-only again (always asked for after an open that worked). */
static status_t close_part(struct inst *in, unsigned part)
{
    uint64_t t0 = now();
    if (in->w.fs)
        jam_handle_close(in->w.fs);
    in->w.fs = HANDLE_INVALID;
    status_t st = part ? volinstall_close_data_until(in->vm, now() + CLOSE_WAIT, in->id)
                       : volinstall_close_esp_until(in->vm, now() + CLOSE_WAIT, in->id);
    inst_say("the %s is read-only again in %lu ms (%s)", part ? "data partition" : "ESP",
             esp_ms_since(t0), status_str(st));
    return st;
}

/* ---- /data/etc ---------------------------------------------------------------------- */

static bool name_ok(const char *name, size_t room)
{
    return name[0] && strlen(name) < room && strcmp(name, ".") && strcmp(name, "..");
}

/* Is path on the new disk (a file or a directory)? */
static bool there(struct inst *in, const char *path)
{
    uint64_t size, mtime;
    uint8_t dir;
    return fs_stat_until(in->w.fs, esp_until(&in->w), path, (uint32_t)strlen(path), &size, &dir,
                         &mtime, NULL) == OK;
}

/* Everything below the directory path of the new disk removed, and the
 * directory itself (what an earlier install cut short left as etc.new). */
static status_t remove_tree(struct inst *in, const char *path, unsigned depth)
{
    uint8_t *ents = malloc(8192);
    if (!ents)
        return ERR_NO_MEMORY;
    status_t st = OK;
    for (unsigned guard = 0; st == OK && guard < ETC_ENTRIES + 8; guard++) {
        uint32_t next, elen = 0, at = 0;
        uint16_t count = 0;
        st = fs_readdir_until(in->w.fs, esp_until(&in->w), path, (uint32_t)strlen(path), 0, &next,
                              &count, ents, &elen);
        struct fs_dirent e;
        bool found = false;
        while (st == OK && at < elen && fs_dirent_get(ents, elen, &at, &e)) {
            char name[ETC_PATH], sub[ETC_PATH];
            if (e.name_len >= sizeof(name))
                continue;
            memcpy(name, e.name, e.name_len);
            name[e.name_len] = '\0';
            if (!name_ok(name, sizeof(name)))
                continue;
            found = true;
            if (snprintf(sub, sizeof(sub), "%s/%s", path, name) >= (int)sizeof(sub)) {
                st = ERR_INVALID_ARGS;
            } else if (e.is_dir) {
                st = depth < ETC_DEPTH ? remove_tree(in, sub, depth + 1) : ERR_INVALID_ARGS;
            } else {
                st = fs_unlink_until(in->w.fs, esp_until(&in->w), sub, (uint32_t)strlen(sub), 0);
            }
            break;   /* the listing changed: ask again */
        }
        if (!found)
            break;
    }
    free(ents);
    if (st == OK)
        st = fs_unlink_until(in->w.fs, esp_until(&in->w), path, (uint32_t)strlen(path),
                             FS_UNLINK_DIR);
    return st == ERR_NOT_FOUND ? OK : st;
}

/* The boot disk's directory src copied to dst (made here) on the new disk:
 * each file read, written, synced and read back against its SHA-256. */
static status_t copy_tree(struct inst *in, const char *src, const char *dst, unsigned depth)
{
    status_t st = fs_mkdir_until(in->w.fs, esp_until(&in->w), dst, (uint32_t)strlen(dst),
                                 FS_MODE_DIR_DEFAULT);
    if (st == ERR_ALREADY_EXISTS)
        st = OK;
    for (uint32_t i = 0; st == OK; i++) {
        struct fs_entry e;
        if (fs_readdir(src, i, &e) != OK)
            break;   /* past the last (or unreadable: what was listed is copied) */
        char from[ETC_PATH], to[ETC_PATH];
        if (!name_ok(e.name, sizeof(from)) ||
            snprintf(from, sizeof(from), "%s/%s", src, e.name) >= (int)sizeof(from) ||
            snprintf(to, sizeof(to), "%s/%s", dst, e.name) >= (int)sizeof(to)) {
            inst_say("%s/%s: name left out (too long)", src, e.name);
            continue;
        }
        if (in->etc_files >= ETC_ENTRIES) {
            st = ERR_NO_SPACE;
        } else if (e.is_dir) {
            if (depth < ETC_DEPTH)
                st = copy_tree(in, from, to, depth + 1);
        } else {
            struct src s = { 0 };
            st = src_load(&s, from, ETC_FILE_MAX, true);
            uint8_t back[SHA256_BYTES];
            if (st == OK)
                st = esp_write_file(&in->w, to, "", s.size, UPDATE_WRITE_NONE, esp_from_vmo,
                                    &s.vmo);
            if (st == OK)
                st = esp_hash(&in->w, to, "", s.size, back);
            if (st == OK && memcmp(back, s.sha, SHA256_BYTES))
                st = ERR_IO;
            src_free(&s);
            in->etc_files++;
        }
    }
    return st;
}

/* Step 3: /data/etc onto the new disk's data partition. */
static status_t copy_etc(struct inst *in)
{
    status_t st = open_part(in, 1, DATA_LIMIT);
    if (st != OK)
        return st;
    if (there(in, "/etc")) {
        in->etc_kept = true;
        inst_say("the disk has a /etc already: its /data is kept as it is");
    } else if (fs_stat("/data/etc", NULL, NULL, NULL) != OK) {
        inst_say("this boot has no /data/etc: nothing to copy");
    } else {
        st = there(in, "/etc.new") ? remove_tree(in, "/etc.new", 0) : OK;   /* an earlier cut */
        if (st == OK)
            st = copy_tree(in, "/data/etc", "/etc.new", 0);
        if (st == OK)
            st = esp_sync(&in->w);
        if (st == OK)
            st = esp_rename(&in->w, "/etc.new", "", "/etc", "");
        if (st == OK)
            st = esp_sync(&in->w);
        inst_say("/data/etc: %u files copied (%s)", in->etc_files, status_str(st));
    }
    status_t closed = close_part(in, 1);
    return st != OK ? st : closed;
}

/* ---- the ESP ------------------------------------------------------------------------ */

/* path (no suffix) written from s, synced and read back against s->sha. */
static status_t put(struct inst *in, const char *path, struct src *s)
{
    status_t st = esp_unlink(&in->w, path, "");
    uint8_t back[SHA256_BYTES];
    if (st == OK)
        st = esp_write_file(&in->w, path, "", s->size, UPDATE_WRITE_NONE, esp_from_vmo, &s->vmo);
    if (st == OK)
        st = esp_sync(&in->w);
    if (st == OK)
        st = esp_hash(&in->w, path, "", s->size, back);
    if (st == OK && memcmp(back, s->sha, SHA256_BYTES))
        st = ERR_IO;   /* the drive didn't keep what was written */
    return st;
}

static void make_dir(struct inst *in, const char *path)
{
    (void)fs_mkdir_until(in->w.fs, esp_until(&in->w), path, (uint32_t)strlen(path),
                         FS_MODE_DIR_DEFAULT);   /* there already is fine; a real failure shows at the write */
}

/* Room for the files: the leftovers of an earlier install or update gone first. */
static status_t room(struct inst *in)
{
    uint64_t total = 0, free_bytes = 0, need = 0;
    uint8_t ro = 0, label[16];
    status_t st = fs_statfs_until(in->w.fs, esp_until(&in->w), &total, &free_bytes, &ro, label,
                                  NULL, NULL, NULL);
    for (unsigned f = 0; f < UPDATE_FILES; f++)
        need += 2 * (in->file[f].size + SLACK);
    need += in->efi.size + in->menu.size + 2 * SLACK;
    if (st == OK && ro)
        st = ERR_ACCESS_DENIED;
    if (st == OK && free_bytes < need)
        st = ERR_NO_SPACE;
    inst_say("%lu KiB free, %lu KiB needed (%s)", (unsigned long)(free_bytes >> 10),
             (unsigned long)(need >> 10), status_str(st));
    return st;
}

/* Step 4: the ESP. */
static status_t write_esp(struct inst *in)
{
    status_t st = open_part(in, 0, ESP_LIMIT);
    if (st != OK)
        return st;
    /* The menu goes first (a disk installed on again): from here on nothing
     * boots this disk until the menu is written, the last thing. */
    st = esp_unlink(&in->w, MENU, "");
    for (unsigned i = 0; st == OK && i < 3; i++)
        st = esp_unlink(&in->w, MENU, i == 0 ? NEW : i == 1 ? PREV : OLD);
    if (st == OK)
        st = esp_unlink(&in->w, SPARE, "");
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++) {   /* an update's leftovers */
        st = esp_unlink(&in->w, cur[f], NEW);
        if (st == OK)
            st = esp_unlink(&in->w, cur[f], OLD);
        if (st == OK)
            st = esp_unlink(&in->w, prev[f], NEW);
    }
    if (st == OK)
        st = esp_sync(&in->w);
    if (st == OK)
        st = room(in);
    if (st == OK) {
        make_dir(in, "/EFI");
        make_dir(in, "/EFI/BOOT");
        make_dir(in, "/boot");
        make_dir(in, "/boot/limine");
        inst_step(4, "Limine and the build written");
        st = put(in, LIMINE, &in->efi);
    }
    for (unsigned f = 0; st == OK && f < UPDATE_FILES; f++) {
        st = put(in, cur[f], &in->file[f]);
        if (st == OK)
            st = put(in, prev[f], &in->file[f]);
    }
    if (st == OK) {
        menu_note(&in->menu, &in->w);   /* against the files the disk has now */
        inst_step(5, "the boot menu, last");
        st = put(in, MENU, &in->menu);
    }
    status_t closed = close_part(in, 0);
    return st != OK ? st : closed;
}

/* ---- the worker ----------------------------------------------------------------------- */

static status_t install_run(struct inst *in)
{
    inst_step(1, "the disk made a Jam OS disk (or found to be one)");
    status_t st = volinstall_prepare_until(in->vm, now() + PREPARE_WAIT, in->id, in->tail,
                                           in->tail_len, in->word, in->word_len);
    if (st == ERR_ALREADY_EXISTS) {
        inst_say("the disk is a Jam OS disk already: its /data is kept, its ESP's files replaced");
        st = OK;
    }
    inst_say("prepare: %s", status_str(st));
    if (st != OK)
        return st;
    inst_step(2, "the build, Limine and the boot menu read");
    st = read_sources(in);
    if (st != OK)
        return st;
    inst_step(3, "/data/etc copied");
    st = copy_etc(in);
    if (st != OK)
        return st;
    inst_step(4, "the ESP");
    st = write_esp(in);
    if (st == OK)
        inst_step(6, "done");
    return st;
}

static void worker_main(void *arg)
{
    struct inst *in = arg;
    in->st = install_run(in);
    inst_say("disk %08x: %s in %lu ms", in->id, in->st == OK ? "installed" : "NOT installed",
             esp_ms_since(in->t0));
    if (in->st != OK)
        inst_say("failed: %s%s", status_str(in->st),
                 "; the disk has no boot menu until `install` is run again and finishes");
    (void)initctl_reply_install(in->txn, in->st);
    jam_handle_close(in->txn.ch);
    jam_handle_close(in->vm);
    src_free(&in->efi);
    src_free(&in->menu);
    for (unsigned f = 0; f < UPDATE_FILES; f++)
        src_free(&in->file[f]);
    free(in->w.buf);
    free(in);
    __atomic_store_n(&busy, NULL, __ATOMIC_RELEASE);   /* the thread ends right after */
}

status_t install_start(handle_t ch, uint32_t txid, uint32_t id, const char *tail, uint32_t tail_len,
                       const char *word, uint32_t word_len)
{
    handle_t vol = shell_volumes_install();
    if (!vol)
        return ERR_NOT_FOUND;
    if (install_busy() || update_busy())
        return ERR_BAD_STATE;
    if (worker) {   /* the last one's thread: ended by now, or its stack can't be used */
        signals_t seen;
        status_t st = jam_object_wait_one(worker, SIG_TERMINATED, now() + THREAD_END, &seen);
        jam_handle_close(worker);
        worker = HANDLE_INVALID;
        if (st != OK) {
            worker_stuck = true;
            init_say("init: install: the last install's thread didn't end (%s): no more "
                     "installs this boot", status_str(st));
            return ERR_BAD_STATE;
        }
    }
    if (tail_len > sizeof(((struct inst *)0)->tail) || word_len > sizeof(((struct inst *)0)->word))
        return ERR_INVALID_ARGS;
    struct inst *in = calloc(1, sizeof(*in));
    if (!in)
        return ERR_NO_MEMORY;
    in->id = id;
    in->tail_len = tail_len;
    in->word_len = word_len;
    memcpy(in->tail, tail, tail_len);
    memcpy(in->word, word, word_len);
    in->job = (struct esp_write){ .fail_at = UPDATE_WRITE_NONE };
    in->w.job = &in->job;
    in->w.buf = malloc(ESP_CHUNK);
    in->t0 = now();
    in->txn.txid = txid;
    status_t st = in->w.buf ? OK : ERR_NO_MEMORY;
    if (st == OK)
        st = jam_handle_duplicate(ch, RIGHT_SAME, &in->txn.ch);
    if (st == OK && (st = jam_handle_duplicate(vol, RIGHT_SAME, &in->vm)) != OK)
        jam_handle_close(in->txn.ch);
    if (st == OK) {
        __atomic_store_n(&busy, in, __ATOMIC_RELEASE);
        st = thread_spawn("install", worker_main, in, worker_stack, sizeof(worker_stack), &worker);
        if (st != OK) {
            __atomic_store_n(&busy, NULL, __ATOMIC_RELEASE);
            jam_handle_close(in->txn.ch);
            jam_handle_close(in->vm);
        }
    }
    if (st != OK) {
        free(in->w.buf);
        free(in);
        return st;
    }
    printf("init: install: disk %08x%s\n", id, word_len ? " (with an erase's confirmation)" : "");
    return IDL_LATER;
}
