/* updtest's boot menu cases (main.c's header has the modes): a build
 * offered with a boot menu (<update.h>'s `menu` line) to be written to
 * the stick, stopped at each change of the menu's swap; the offers with a
 * menu init must refuse; and which menu Limine reads on /esp afterwards.
 * tools/update-menu-test.sh runs them and reads their lines. */
#include <os.h>
#include <update.h>
#include "updtest.h"

#define ESP_WAIT (10 * NS_PER_S)   /* /esp comes back after a stick write */

/* enum update_menu, one word each (the test script greps them). */
static const char *const menu_words[UPDATE_MENU_STATES] = {
    [UPDATE_MENU_NONE] = "none",           [UPDATE_MENU_WRITTEN] = "written",
    [UPDATE_MENU_SAME] = "same",           [UPDATE_MENU_REFUSED] = "refused",
    [UPDATE_MENU_NOT_WRITTEN] = "notwritten", [UPDATE_MENU_SKIPPED] = "skipped",
};

static void close_build(struct build *b)
{
    for (unsigned f = 0; f < UPDATE_PARTS; f++)
        if (b->vmo[f])
            jam_handle_close(b->vmo[f]);
}

/* The build in DIR with folder dir's manifest, and its menu (dir
 * "limine.conf") if that manifest names one. */
static status_t load_menu(struct build *b, const char *dir)
{
    char p[80];
    handle_t m;
    uint64_t n = 0;
    memset(b, 0, sizeof(*b));
    status_t st = load_dir(b, DIR);
    snprintf(p, sizeof(p), "%smanifest", dir);
    if (st == OK)
        st = file_read_vmo(p, UPDATE_MANIFEST_MAX, &m, &n);
    if (st == OK) {
        st = jam_vmo_read(m, 0, b->manifest, n);
        jam_handle_close(m);
        b->manifest_len = (uint32_t)n;
    }
    struct update_manifest man;
    if (st != OK || update_manifest_parse(b->manifest, b->manifest_len, &man) != OK ||
        !man.has_menu)
        return st;
    snprintf(p, sizeof(p), "%slimine.conf", dir);
    st = file_read_vmo(p, UPDATE_MENU_MAX, &b->vmo[UPDATE_MENU], &b->bytes[UPDATE_MENU]);
    b->parts = UPDATE_PARTS;
    return st;
}

static void menu_write(const char *dir, uint32_t stop)
{
    struct build b;
    struct update_answer a;
    memset(&a, 0, sizeof(a));
    status_t st = load_menu(&b, dir);
    b.flags = UPDATE_OFFER_WRITE | (stop ? UPDATE_OFFER_STOP(stop) : 0);
    if (st == OK)
        st = offer(&b, b.parts, UPDATE_OFFER_MAGIC, &a);
    close_build(&b);
    a.menu_why[sizeof(a.menu_why) - 1] = '\0';
    failures += st != OK;
    const char *menu = a.menu < UPDATE_MENU_STATES ? menu_words[a.menu] : "?";
    printf("updtest: menuwrite %s stop %u: build %s (%s), %s, menu %s (%s)%s%s\n", dir, stop,
           st == OK ? update_why_str(a.why) : "-", status_str(st == OK ? a.status : st),
           update_write_step_str(a.write_step), menu, status_str(a.menu_status),
           a.menu_why[0] ? ": " : "", a.menu_why);
}

/* A copy of the menu's VMO (n bytes of room), its byte `flip` changed
 * (none if flip >= len), into *out. */
static status_t menu_copy(const struct build *b, uint64_t n, uint64_t flip, handle_t *out)
{
    uint64_t len = b->bytes[UPDATE_MENU];
    uint8_t *buf = calloc(1, n);
    status_t st = buf ? jam_vmo_read(b->vmo[UPDATE_MENU], 0, buf, len) : ERR_NO_MEMORY;
    if (st == OK && flip < len)
        buf[flip] ^= 0x20;
    if (st == OK)
        st = jam_vmo_create(n, 0, HANDLE_INVALID, out);
    if (st == OK)
        st = jam_vmo_write(*out, 0, buf, n);
    free(buf);
    return st;
}

/* The offers with a menu that init must refuse, then the two that take
 * the build but must leave the stick's menu alone. */
static void menu_bad(const char *dir)
{
    struct build b, v, plain;
    if (load_menu(&b, dir) != OK || b.parts != UPDATE_PARTS || load_dir(&plain, DIR) != OK) {
        failures++;
        printf("updtest: menubad: no %smanifest with a menu, or no " DIR " build: FAILED\n", dir);
        return;
    }
    uint64_t len = b.bytes[UPDATE_MENU];
    v = b;
    if (menu_copy(&b, len, len / 2, &v.vmo[UPDATE_MENU]) == OK)
        expect("a menu byte changed", &v, UPDATE_PARTS, UPDATE_OFFER_MAGIC, UPDATE_BAD_HASH,
               UPDATE_MENU);
    jam_handle_close(v.vmo[UPDATE_MENU]);
    v = b;
    v.bytes[UPDATE_MENU] = len + 1;   /* the VMO is page-rounded: it has the byte */
    expect("the menu longer than its manifest says", &v, UPDATE_PARTS, UPDATE_OFFER_MAGIC,
           UPDATE_BAD_SIZE, UPDATE_MENU);
    expect("the menu's VMO missing", &b, UPDATE_FILES, UPDATE_OFFER_MAGIC, UPDATE_BAD_OFFER, 0);
    v = plain;
    v.vmo[UPDATE_MENU] = b.vmo[UPDATE_MENU];
    v.bytes[UPDATE_MENU] = len;
    expect("a menu with a manifest that names none", &v, UPDATE_PARTS, UPDATE_OFFER_MAGIC,
           UPDATE_BAD_OFFER, 0);
    expect("a menu length with no menu", &v, UPDATE_FILES, UPDATE_OFFER_MAGIC, UPDATE_BAD_OFFER,
           0);
    b.flags = UPDATE_OFFER_CHECK_ONLY;
    expect("with its menu, check only", &b, UPDATE_PARTS, UPDATE_OFFER_MAGIC, UPDATE_ACCEPTED, 0);
    b.flags = 0;
    expect("with its menu, RAM only", &b, UPDATE_PARTS, UPDATE_OFFER_MAGIC, UPDATE_ACCEPTED, 0);
    close_build(&b);
    for (unsigned f = 0; f < UPDATE_FILES; f++)
        jam_handle_close(plain.vmo[f]);
}

/* The file at path (at most UPDATE_MENU_MAX bytes) into a new buffer
 * (*out, *len; freed by the caller). */
static status_t read_small(const char *path, uint8_t **out, uint64_t *len)
{
    handle_t v;
    status_t st = file_read_vmo(path, UPDATE_MENU_MAX, &v, len);
    if (st != OK)
        return st;
    *out = malloc(*len ? *len : 1);
    st = *out ? jam_vmo_read(v, 0, *out, *len) : ERR_NO_MEMORY;
    jam_handle_close(v);
    return st;
}

/* Which of old, new the n bytes at p are: "the old menu", "the new menu"
 * or NULL. */
static const char *which(const uint8_t *p, uint64_t n, const uint8_t *old, uint64_t old_len,
                         const uint8_t *nw, uint64_t new_len)
{
    if (n == old_len && !memcmp(p, old, n))
        return "the old menu";
    if (n == new_len && !memcmp(p, nw, n))
        return "the new menu";
    return NULL;
}

static bool esp_has(const char *path)
{
    uint64_t size, mtime;
    bool dir;
    return fs_stat(path, &size, &dir, &mtime) == OK && !dir;
}

static void menu_check(const char *old_path, const char *new_path)
{
    uint8_t *old = NULL, *nw = NULL, *menu = NULL, *prev = NULL;
    uint64_t old_len = 0, new_len = 0, n = 0, prev_len = 0;
    for (uint64_t end = now() + ESP_WAIT; now() < end && !esp_has("/esp/boot/jamos.elf");)
        jam_nanosleep(now() + 100 * NS_PER_MS);   /* /esp coming back */
    status_t st = read_small(old_path, &old, &old_len);
    if (st == OK)
        st = read_small(new_path, &nw, &new_len);
    const char *where = "boot/limine/limine.conf";
    if (st == OK && read_small("/esp/boot/limine/limine.conf", &menu, &n) != OK) {
        free(menu);
        menu = NULL;
        where = "boot/limine.conf (the spare)";
        st = read_small("/esp/boot/limine.conf", &menu, &n);
    }
    const char *what = st == OK ? which(menu, n, old, old_len, nw, new_len) : NULL;
    const char *was = "none";
    if (read_small("/esp/boot/limine/limine.conf.prev", &prev, &prev_len) == OK) {
        was = which(prev, prev_len, old, old_len, nw, new_len);
        was = was ? was : "another";
    }
    failures += !what;
    printf("updtest: menucheck: Limine reads %s: %s (prev: %s%s%s): %s\n",
           st == OK ? where : "no menu", what ? what : "neither", was,
           esp_has("/esp/boot/limine.conf") ? "; the spare is there" : "",
           esp_has("/esp/boot/limine/limine.conf.new") ? "; limine.conf.new is there" : "",
           what ? "as expected" : "FAILED");
    free(old);
    free(nw);
    free(menu);
    free(prev);
}

/* The number in s, 0..max (false: not one). */
static bool small_number(const char *s, uint32_t max, uint32_t *out)
{
    uint32_t v = 0;
    size_t n = strlen(s);
    if (!n || n > 2)
        return false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    *out = v;
    return v <= max;
}

int menu_main(int argc, char **argv)
{
    uint32_t stop = 0;
    bool write = !strcmp(argv[1], "menuwrite") && (argc == 3 || argc == 4) &&
                 (argc == 3 || small_number(argv[3], UPDATE_STOP_MAX, &stop));
    bool bad = !strcmp(argv[1], "menubad") && argc == 3;
    bool check = !strcmp(argv[1], "menucheck") && argc == 4;
    if (!write && !bad && !check) {
        printf("usage: updtest menuwrite <dir> [<0-%u>] | menubad <dir> | menucheck <old> "
               "<new>\n", UPDATE_STOP_MAX);
        return 2;
    }
    initctl = svc_get(SVC_INIT);
    if (!initctl && !check) {
        printf("updtest: no init channel\n");
        return 1;
    }
    if (write)
        menu_write(argv[2], stop);
    else if (bad)
        menu_bad(argv[2]);
    else
        menu_check(argv[2], argv[3]);
    printf("updtest: %s: %s\n", argv[1], failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
