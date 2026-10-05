/* init's boot menu write for `update` (<update.h>'s `menu` line): the
 * fetched build's boot/limine.conf, checked against the signed manifest's
 * SHA-256 by update.c, written to the stick's ESP as
 * boot/limine/limine.conf once the build itself is written (espwrite.c's
 * steps, then this, on the same worker thread and under the same
 * deadline), so new boot entries reach the stick without `make flash`.
 *
 * Safety first: a menu Limine can't read leaves a stick that boots only
 * with a hand-typed entry (Limine then shows "config file not found" and
 * its B, Blank Entry, key). So:
 *   - nothing is written if the stick's menu is the new one already;
 *   - the new menu must pass the check (<bootmenu.h>) with the files the
 *     stick has now, the build's swap done (the default entry boots
 *     boot/jamos.elf, "Jam OS (previous build)" boot/prev-jamos.elf, every
 *     file named is there), and Limine on the stick must not have a menu
 *     checksum enrolled (its EFI/BOOT/BOOTX64.EFI's
 *     "++CONFIG_B2SUM_SIGNATURE++" all zeros: a menu that isn't the
 *     enrolled one would stop it). A menu that fails is not written; the
 *     build stays written and the answer says why (UPDATE_MENU_REFUSED);
 *   - the stick keeps a whole menu Limine reads at every moment, by the
 *     order of the writes and renames below and one fact of Limine 11's
 *     (tools/update-menu-test.sh boots it): it reads the first of
 *     EFI/BOOT/limine.conf, boot/limine/limine.conf, ..., boot/limine.conf
 *     that is there, so boot/limine.conf, the spare, is read only while
 *     boot/limine/limine.conf is missing.
 *
 * Why after the build: the check is then made against the files the
 * stick really has. The other order would be as safe for Jam OS's menu
 * (every entry names one of four files that are always there once a
 * build has been written twice), but a power cut between the two would
 * leave an old build with entries made for the new one. As it is, a cut
 * between them leaves the new build with the stick's old menu, which
 * names the same four files: every entry boots.
 *
 * The changes, numbered as UPDATE_OFFER_STOP counts them after the
 * build's eight (<update.h> UPDATE_MENU_OPS), each whole before the next:
 *   1. limine.conf.new written from init's checked copy, synced, read
 *      back and its SHA-256 compared with the manifest's;
 *   2. the spare, boot/limine.conf, written with the stick's menu as read,
 *      synced, read back and compared;
 *   3. the older limine.conf.prev removed;
 *   4. limine.conf renamed limine.conf.prev (the stick's menu kept: a
 *      backup to put back by hand);
 *   5. limine.conf.new renamed limine.conf, and the ESP synced;
 *   6. the spare removed, synced.
 * Between 4 and 5 (one rename: FatFs syncs each rename, about a second
 * on the PC's stick) boot/limine/limine.conf is missing, and a power cut
 * there boots the spare: the stick's old menu, whole since change 2.
 * Before 4 the stick's menu is in its place, from 5 on the new one is. A
 * stick that had no menu at all gets no spare (there is nothing to copy)
 * and no .prev: there, and only there, nothing is read until change 5.
 * A failure stops where it is and puts things back while the ESP's fat
 * still answers: limine.conf.prev back to limine.conf if that is missing,
 * the .new removed, the spare removed once limine.conf is there; never
 * the spare without a limine.conf. The next `update` first settles
 * what a cut left (esp_menu_settle: the same rules), whatever it carries.
 * The answer says what became of the menu (enum update_menu); the
 * build's own answer doesn't depend on it. */
#include <bootmenu.h>
#include <update.h>
#include "espwrite.h"
#include "init.h"

/* The ESP's files, by the paths fat takes. */
static const char MENU[] = "/boot/limine/limine.conf";   /* where Limine reads it */
static const char SPARE[] = "/boot/limine.conf";          /* read only when MENU isn't there */
static const char LIMINE[] = "/EFI/BOOT/BOOTX64.EFI";     /* Limine itself */
#define NEW  ".new"
#define PREV ".prev"
/* Limine's enrolled menu checksum: this, then 128 hex digits, zeros if none. */
#define B2SUM_MARK   "++CONFIG_B2SUM_SIGNATURE++"
#define B2SUM_DIGITS 128u

/* A menu write under way. */
struct menu_write {
    struct writer *w;
    uint8_t       *old;                    /* the stick's menu, as read (UPDATE_MENU_MAX room) */
    uint64_t       old_len;
    bool           had;                    /* the stick had a menu */
    uint8_t        old_sha[SHA256_BYTES];
    uint8_t       *menu;                   /* the new menu, from init's copy */
};

/* The test's stop (UPDATE_OFFER_STOP) right after the menu's change k
 * (1..UPDATE_MENU_OPS), with no clean-up, as a power cut there would. */
static status_t op_done(struct writer *w, unsigned k)
{
    if (w->job->stop_at != UPDATE_SWAP_OPS + k)
        return OK;
    esp_say("stopping dead after change %u (the boot menu's %u), as the test asked",
            UPDATE_SWAP_OPS + k, k);
    w->stopped = true;
    return ERR_CANCELED;
}

status_t esp_menu_settle(struct writer *w)
{
    bool cur = esp_exists(w, MENU, ""), prev = esp_exists(w, MENU, PREV);
    bool left = esp_exists(w, MENU, NEW), spare = esp_exists(w, SPARE, "");
    bool changed = false;
    status_t st = OK;
    if (!cur && prev) {   /* cut between changes 4 and 5: the stick's menu put back */
        esp_say("an earlier boot menu write stopped in its swap: undoing it");
        st = esp_rename(w, MENU, PREV, MENU, "");
        cur = changed = st == OK;
    }
    if (st == OK && left) {
        st = esp_unlink(w, MENU, NEW);
        changed = true;
    }
    if (st == OK && cur && spare) {
        st = esp_unlink(w, SPARE, "");   /* never while it is the menu Limine reads */
        changed = true;
    }
    if (st == OK && changed)
        st = esp_sync(w);
    if (!cur)
        esp_say("the stick has no boot menu in its place%s", spare ? " (the spare is read)" : "");
    return st;
}

/* The file at MENU + suffix (at most UPDATE_MENU_MAX bytes) into buf, *len. */
static status_t read_menu(struct writer *w, const char *suffix, uint8_t *buf, uint64_t *len)
{
    status_t st = esp_size_of(w, MENU, suffix, len);
    if (st == OK && *len > UPDATE_MENU_MAX)
        st = ERR_OUT_OF_RANGE;   /* not one of ours: refused, left alone */
    struct jfile f;
    if (st == OK)
        st = esp_open(w, MENU, suffix, FS_READ, &f);
    if (st != OK)
        return st;
    st = esp_read_at(w, &f, 0, buf, (size_t)*len);
    file_close(&f);
    return st;
}

/* Does Limine on the stick have a menu checksum enrolled? OK: no (its
 * mark followed by zeros); ERR_ACCESS_DENIED: yes; ERR_NOT_FOUND: no
 * mark (another Limine: can't tell); a read's error. */
static status_t limine_checks_menu(struct writer *w)
{
    uint64_t n = 0;
    status_t st = esp_size_of(w, LIMINE, "", &n);
    if (st == OK && n > ESP_CHUNK)
        st = ERR_NOT_FOUND;
    struct jfile f;
    if (st == OK)
        st = esp_open(w, LIMINE, "", FS_READ, &f);
    if (st != OK)
        return st;
    st = esp_read_at(w, &f, 0, w->buf, (size_t)n);
    file_close(&f);
    size_t m = sizeof(B2SUM_MARK) - 1;
    for (size_t i = 0; st == OK && i + m + B2SUM_DIGITS <= n; i++) {
        if (memcmp(w->buf + i, B2SUM_MARK, m))
            continue;
        for (size_t d = 0; d < B2SUM_DIGITS; d++)
            if (w->buf[i + m + d] != '0')
                return ERR_ACCESS_DENIED;
        return OK;
    }
    return st == OK ? ERR_NOT_FOUND : st;
}

/* bootmenu_check's question: is the file there on the stick? */
static bool on_stick(void *ctx, const char *path)
{
    return esp_exists(ctx, path, "");
}

/* The new menu's check, then Limine's: true if it may be written (else
 * the job's menu_why says why). */
static bool menu_ok(struct menu_write *mw)
{
    struct esp_write *j = mw->w->job;
    struct bootmenu_result r;
    if (!bootmenu_check(mw->menu, (size_t)j->menu_size, on_stick, mw->w, &r)) {
        const char *why = bootmenu_why_str(r.why), *sep = r.path[0] ? ": " : "";
        if (r.line)
            snprintf(j->menu_why, sizeof(j->menu_why), "line %u: %s%s%s", r.line, why, sep,
                     r.path);
        else
            snprintf(j->menu_why, sizeof(j->menu_why), "%s%s%s", why, sep, r.path);
        return false;
    }
    esp_say("the boot menu passes the check: %u entries boot, every file they name is here",
            r.entries);
    status_t st = limine_checks_menu(mw->w);
    if (st == OK)
        return true;
    snprintf(j->menu_why, sizeof(j->menu_why), "%s (%s)",
             st == ERR_ACCESS_DENIED ? "Limine on the stick has a menu checksum enrolled"
                                     : "can't tell whether Limine on the stick checks its menu",
             status_str(st));
    return false;
}

/* esp_write_file's source: the stick's menu as read. */
static status_t from_old(struct writer *w, void *ctx, uint64_t off, uint8_t *buf, size_t n)
{
    (void)w;
    memcpy(buf, ((struct menu_write *)ctx)->old + off, n);
    return OK;
}

/* path + suffix written from `next`, size bytes, read back: SHA-256 sha. */
static status_t write_checked(struct writer *w, const char *path, const char *suffix,
                              uint64_t size, source_t next, void *ctx, const uint8_t *sha)
{
    uint8_t back[SHA256_BYTES];
    status_t st = esp_write_file(w, path, suffix, size, UPDATE_WRITE_NONE, next, ctx);
    if (st == OK)
        st = esp_hash(w, path, suffix, size, back);
    if (st == OK && memcmp(back, sha, SHA256_BYTES))
        st = ERR_IO;   /* the stick didn't keep what was written */
    return st;
}

/* Changes 1 to 6 (the header's). */
static status_t swap_menu(struct menu_write *mw)
{
    struct writer *w = mw->w;
    struct esp_write *j = w->job;
    status_t st = write_checked(w, MENU, NEW, j->menu_size, esp_from_vmo, &j->menu_vmo,
                                j->menu_sha256);
    if (st == OK)
        st = op_done(w, 1);
    if (st == OK && mw->had)
        st = write_checked(w, SPARE, "", mw->old_len, from_old, mw, mw->old_sha);
    if (st == OK)
        st = op_done(w, 2);
    if (st == OK && mw->had)
        st = esp_unlink(w, MENU, PREV);
    if (st == OK)
        st = op_done(w, 3);
    if (st == OK && mw->had)
        st = esp_rename(w, MENU, "", MENU, PREV);
    if (st == OK)
        st = op_done(w, 4);
    if (st == OK)
        st = esp_inject(w, UPDATE_WRITE_MENU);   /* no menu in its place: the spare's turn */
    if (st == OK)
        st = esp_rename(w, MENU, NEW, MENU, "");
    if (st == OK)
        st = op_done(w, 5);
    if (st == OK)
        st = esp_sync(w);
    if (st == OK)
        st = esp_unlink(w, SPARE, "");
    if (st == OK)
        st = op_done(w, 6);
    return st == OK ? esp_sync(w) : st;
}

/* After a failure: the stick's menu back in its place if it isn't, the
 * temporary files removed (the spare only once a menu is in place). */
static void put_back(struct writer *w)
{
    w->deadline = now() + RECOVER_LIMIT;   /* the clean-up's own time */
    if (!esp_exists(w, MENU, "") && esp_exists(w, MENU, PREV))
        (void)esp_rename(w, MENU, PREV, MENU, "");
    (void)esp_unlink(w, MENU, NEW);
    if (esp_exists(w, MENU, ""))
        (void)esp_unlink(w, SPARE, "");
    (void)esp_sync(w);
    esp_say("the boot menu write failed (%s): %s", status_str(w->job->menu_status),
            esp_exists(w, MENU, "") ? "the stick's menu is in its place"
                                    : "no menu in its place: Limine reads the spare");
}

/* The stick's menu read (had, old, old_len, old_sha); a stick without
 * one is fine. */
static status_t read_stick_menu(struct menu_write *mw)
{
    status_t st = read_menu(mw->w, "", mw->old, &mw->old_len);
    mw->had = st == OK;
    if (st == OK)
        sha256(mw->old, (size_t)mw->old_len, mw->old_sha);
    return st == ERR_NOT_FOUND ? OK : st;
}

/* The menu's job: what became of it (enum update_menu). */
static uint32_t menu_job(struct menu_write *mw)
{
    struct esp_write *j = mw->w->job;
    status_t st = read_stick_menu(mw);
    if (st == OK)
        st = jam_vmo_read(j->menu_vmo, 0, mw->menu, (size_t)j->menu_size);
    if (st == OK && mw->had && mw->old_len == j->menu_size &&
        !memcmp(mw->old_sha, j->menu_sha256, SHA256_BYTES)) {
        esp_say("the stick has this boot menu already: nothing to write");
        return UPDATE_MENU_SAME;
    }
    if (st == OK && !menu_ok(mw)) {
        esp_say("the boot menu refused: %s; the stick keeps its menu", j->menu_why);
        return UPDATE_MENU_REFUSED;
    }
    if (st == OK)
        st = swap_menu(mw);
    if (st == OK) {
        esp_say("the boot menu written: boot/limine/limine.conf (%lu bytes)%s",
                (unsigned long)j->menu_size,
                mw->had ? ", the stick's old one boot/limine/limine.conf.prev" : "");
        return UPDATE_MENU_WRITTEN;
    }
    j->menu_status = st;
    if (!mw->w->stopped)   /* a test's power cut: nothing more is written */
        put_back(mw->w);
    return UPDATE_MENU_NOT_WRITTEN;
}

void esp_menu_write(struct writer *w)
{
    struct esp_write *j = w->job;
    if (!j->menu_vmo) {
        j->menu = UPDATE_MENU_NONE;
        return;
    }
    uint64_t t0 = now();
    uint32_t step = j->step;
    j->step = UPDATE_WRITE_MENU;
    esp_say("%s ...", update_write_step_str(j->step));
    struct menu_write mw = { .w = w, .old = malloc(UPDATE_MENU_MAX),
                             .menu = malloc(UPDATE_MENU_MAX) };
    if (mw.old && mw.menu) {
        j->menu = menu_job(&mw);
    } else {
        j->menu = UPDATE_MENU_NOT_WRITTEN;
        j->menu_status = ERR_NO_MEMORY;
    }
    free(mw.old);
    free(mw.menu);
    esp_say("%s: %s in %lu ms", update_write_step_str(j->step), update_menu_str(j->menu),
            esp_ms_since(t0));
    j->step = step;
}
