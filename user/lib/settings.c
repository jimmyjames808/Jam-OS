/* Settings that survive a reboot (<settings.h> says what the file is). */
#include <os.h>
#include <settings.h>

/* ---- the text ------------------------------------------------------------------ */

static bool key_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '.' || c == '_' || c == '-';
}

bool settings_key_ok(const char *key)
{
    size_t n = strnlen(key, SETTINGS_KEY_MAX);
    if (!n || n == SETTINGS_KEY_MAX)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!key_char(key[i]))
            return false;
    return true;
}

/* n bytes with no control character (a tab included) and no space at
 * either end: what a value may be. */
static bool value_bytes_ok(const char *v, size_t n)
{
    if (n >= SETTINGS_VALUE_MAX || (n && (v[0] == ' ' || v[n - 1] == ' ')))
        return false;
    for (size_t i = 0; i < n; i++)
        if ((uint8_t)v[i] < 0x20 || v[i] == 0x7f)
            return false;
    return true;
}

bool settings_value_ok(const char *value)
{
    return value_bytes_ok(value, strnlen(value, SETTINGS_VALUE_MAX));
}

/* One line of the text: [*k, +*kn) its key and [*v, +*vn) its value, if
 * it is a setting. */
struct line {
    const char *key, *value;
    size_t      kn, vn;
};

static bool blank(char c)
{
    return c == ' ' || c == '\t';
}

/* s[0..n) (a line without its newline) as a setting, or false. */
static bool parse_line(const char *s, size_t n, struct line *l)
{
    if (n && s[n - 1] == '\r')
        n--;   /* written on Windows */
    size_t a = 0;
    while (a < n && blank(s[a]))
        a++;
    if (a == n || s[a] == '#')
        return false;
    size_t eq = a;
    while (eq < n && s[eq] != '=')
        eq++;
    if (eq == n)
        return false;
    size_t ke = eq;
    while (ke > a && blank(s[ke - 1]))
        ke--;
    size_t vb = eq + 1, ve = n;
    while (vb < ve && blank(s[vb]))
        vb++;
    while (ve > vb && blank(s[ve - 1]))
        ve--;
    if (ke == a || ke - a >= SETTINGS_KEY_MAX || !value_bytes_ok(s + vb, ve - vb))
        return false;
    for (size_t i = a; i < ke; i++)
        if (!key_char(s[i]))
            return false;
    *l = (struct line){ s + a, s + vb, ke - a, ve - vb };
    return true;
}

/* Where the line starting at b ends (its newline, or n). */
static size_t line_end(const char *text, size_t b, size_t n)
{
    while (b < n && text[b] != '\n')
        b++;
    return b;
}

static bool is_key(const struct line *l, const char *key)
{
    return l->kn == strlen(key) && !memcmp(l->key, key, l->kn);
}

bool settings_lookup(const char *text, size_t n, const char *key, char *out, size_t cap)
{
    bool found = false;
    for (size_t b = 0; b < n;) {
        size_t e = line_end(text, b, n);
        struct line l;
        if (parse_line(text + b, e - b, &l) && is_key(&l, key) && l.vn < cap) {
            memcpy(out, l.value, l.vn);   /* the last one counts: go on */
            out[l.vn] = '\0';
            found = true;
        }
        b = e + 1;
    }
    return found;
}

/* Append n bytes to out (cap) at *o; false if they don't fit. */
static bool put(char *out, size_t cap, size_t *o, const char *s, size_t n)
{
    if (n > cap - *o)
        return false;
    memcpy(out + *o, s, n);
    *o += n;
    return true;
}

static bool put_setting(char *out, size_t cap, size_t *o, const char *key, const char *value)
{
    return put(out, cap, o, key, strlen(key)) && put(out, cap, o, " = ", 3) &&
           put(out, cap, o, value, strlen(value)) && put(out, cap, o, "\n", 1);
}

status_t settings_edit(const char *text, size_t n, const char *key, const char *value, char *out,
                       size_t cap, size_t *out_n)
{
    if (!settings_key_ok(key) || !settings_value_ok(value))
        return ERR_INVALID_ARGS;
    size_t o = 0;
    bool done = false, ok = true;
    for (size_t b = 0; b < n && ok;) {
        size_t e = line_end(text, b, n);
        struct line l;
        bool mine = parse_line(text + b, e - b, &l) && is_key(&l, key);
        if (!mine) {
            ok = put(out, cap, &o, text + b, e - b) && put(out, cap, &o, "\n", 1);
        } else if (!done) {   /* a later line with the key is dropped */
            ok = put_setting(out, cap, &o, key, value);
            done = true;
        }
        b = e + 1;
    }
    if (ok && !done)
        ok = put_setting(out, cap, &o, key, value);
    if (!ok)
        return ERR_BUFFER_TOO_SMALL;
    *out_n = o;
    return OK;
}

/* ---- the file -------------------------------------------------------------------- */

/* The whole of `path` into a new buffer (*out, NUL-terminated, free it). */
static status_t read_all(const char *path, char **out, size_t *n)
{
    struct jfile f;
    status_t st = file_open(path, FS_READ, &f);
    if (st != OK)
        return st;
    uint64_t size = 0;
    st = file_stat(&f, &size, NULL);
    char *buf = NULL;
    size_t got = 0;
    if (st == OK && size > SETTINGS_MAX)
        st = ERR_OUT_OF_RANGE;
    if (st == OK && !(buf = malloc((size_t)size + 1)))
        st = ERR_NO_MEMORY;
    if (st == OK)
        st = file_read(&f, 0, buf, (size_t)size, &got);
    file_close(&f);
    if (st != OK) {
        free(buf);
        return st;
    }
    buf[got] = '\0';
    *out = buf;
    *n = got;
    return OK;
}

/* The settings' text: the file, or settings.new if a write was cut short
 * between the two names. ERR_NOT_FOUND: neither is there. */
static status_t read_settings(const char *file, char **out, size_t *n)
{
    status_t st = read_all(file, out, n);
    if (st != ERR_NOT_FOUND)
        return st;
    char tmp[FS_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.new", file);
    return read_all(tmp, out, n);
}

status_t settings_get(const char *file, const char *key, char *out, size_t cap)
{
    char *text;
    size_t n;
    status_t st = read_settings(file, &text, &n);
    if (st != OK)
        return st;
    char v[SETTINGS_VALUE_MAX];
    st = settings_lookup(text, n, key, v, sizeof(v)) ? OK : ERR_NOT_FOUND;
    free(text);
    if (st == OK && strlen(v) >= cap)
        st = ERR_BUFFER_TOO_SMALL;
    if (st == OK)
        memcpy(out, v, strlen(v) + 1);
    return st;
}

/* The folder `file` is in, made if missing (one level: /data/etc). */
static status_t make_folder(const char *file)
{
    char dir[FS_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", file);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir)
        return OK;
    *slash = '\0';
    status_t st = fs_mkdir(dir);
    return st == ERR_ALREADY_EXISTS ? OK : st;
}

/* text into file.new, synced; then file.new takes file's name. A
 * file.new left alone by a write cut short after the old file went is
 * the settings: it gets the name back first, so that this write, cut
 * short in turn, can't lose them. */
status_t settings_write(const char *file, const char *text, size_t n)
{
    char tmp[FS_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.new", file);
    struct jfile f;
    status_t st = make_folder(file);
    if (st == OK && fs_stat(file, NULL, NULL, NULL) == ERR_NOT_FOUND &&
        fs_stat(tmp, NULL, NULL, NULL) == OK)
        st = fs_rename(tmp, file);
    if (st == OK)
        st = file_open(tmp, FS_WRITE | FS_CREATE | FS_TRUNCATE, &f);
    if (st != OK)
        return st;
    size_t done = 0;
    st = file_write(&f, 0, text, n, &done);
    if (st == OK && done != n)
        st = ERR_IO;
    if (st == OK)
        st = file_sync(&f);
    file_close(&f);
    if (st == OK)
        st = fs_unlink(file);
    if (st == ERR_NOT_FOUND)
        st = OK;
    if (st == OK)
        st = fs_rename(tmp, file);
    if (st == OK)
        st = fs_sync(file);
    return st;
}

status_t settings_set(const char *file, const char *key, const char *value)
{
    if (!settings_key_ok(key) || !settings_value_ok(value))
        return ERR_INVALID_ARGS;
    char *text = NULL;
    size_t n = 0;
    status_t st = read_settings(file, &text, &n);
    if (st == ERR_NOT_FOUND)
        st = OK;   /* no file yet: settings_write makes it */
    if (st != OK) {
        free(text);
        return st;
    }
    char have[SETTINGS_VALUE_MAX];
    if (text && settings_lookup(text, n, key, have, sizeof(have)) && !strcmp(have, value)) {
        free(text);
        return OK;   /* already so */
    }
    size_t cap = n + SETTINGS_KEY_MAX + SETTINGS_VALUE_MAX + 8, out_n = 0;
    char *out = malloc(cap);
    st = out ? settings_edit(text ? text : "", n, key, value, out, cap, &out_n) : ERR_NO_MEMORY;
    if (st == OK && out_n > SETTINGS_MAX)
        st = ERR_OUT_OF_RANGE;
    if (st == OK)
        st = settings_write(file, out, out_n);
    free(out);
    free(text);
    return st;
}
