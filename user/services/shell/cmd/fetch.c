/* fetch: a file from a web server over plain HTTP (http:// only: https
 * needs TLS, which Jam OS doesn't have yet). The shell checks the URL and
 * opens where the body goes; bin/fetch (user/apps/fetch) does the
 * network, holding only that and the network, so a hostile server reaches
 * nothing else of the shell's. Its lines are the shell's.
 *   fetch <url>           into the URL's last name in the current folder,
 *                         or, when piped (fetch <url> | head), into the pipe
 *   fetch <url> <file>    into file (a folder: the URL's name inside it)
 *   fetch <url> -         into the output (the pipe, or the screen)
 * A file is written as <file>.part and renamed when the whole body came,
 * so a failed or stopped fetch leaves nothing (and an old <file> stays).
 * Ctrl+C stops it at once. Exit: 0, 1 (it failed, said why), 2 (usage),
 * 130 (stopped). */
#include <http.h>
#include "sh.h"

#define FETCH_PATH "bin/fetch"
#define PART       ".part"

static int usage(void)
{
    sh_tty("usage: fetch <http://host[:port]/path> [file | -]   (e.g. fetch\n"
           "       http://10.2.21.174:8000/big.bin; https is not spoken: no TLS yet)\n");
    return 2;
}

/* The URL argument, or false (said why). */
static bool parse(const char *arg, struct http_url *u)
{
    status_t st = http_url_parse(arg, u);
    if (st == ERR_NOT_SUPPORTED)
        sh_tty("fetch: %s: only http:// URLs: https needs TLS, which Jam OS doesn't have "
               "yet\n", arg);
    else if (st != OK)
        sh_tty("fetch: %s: not a URL fetch takes (http://host[:port]/path)\n", arg);
    return st == OK;
}

/* Where the file goes (abs, FS_PATH_MAX) and its .part name, or false. */
static bool target(const struct http_url *u, const char *arg, char *abs, char *part)
{
    char name[96], given[SH_PATH_MAX], path[SH_PATH_MAX];
    http_url_filename(u, name, sizeof(name));
    bool dir = false;
    uint64_t size;
    if (!sh_resolve(arg ? arg : name, given, sizeof(given))) {
        sh_tty("fetch: %s: the path is too long\n", arg ? arg : name);
        return false;
    }
    if (!(arg && sh_stat(given, &dir, &size) == OK && dir))
        memcpy(path, given, sizeof(path));
    else if (!sh_join(given, name, path, sizeof(path))) {
        sh_tty("fetch: %s/%s: the path is too long\n", arg, name);
        return false;
    }
    size_t n = strlen(path);
    if (n + sizeof(PART) > FS_PATH_MAX) {
        sh_tty("fetch: %s: the path is too long\n", path);
        return false;
    }
    memcpy(abs, path, n + 1);
    snprintf(part, FS_PATH_MAX, "%s%s", path, PART);
    return true;
}

/* The helper's result for the file: renamed into place, or removed. */
static int finish(int code, const char *abs, const char *part)
{
    if (code != 0) {
        (void)fs_unlink(part);   /* gone already, or nothing to keep */
        if (code == 130)
            sh_tty("fetch: stopped: nothing kept\n");
        return code;
    }
    bool dir = false;
    uint64_t size = 0;
    if (sh_stat(abs, &dir, &size) == OK && !dir)
        (void)fs_unlink(abs);   /* the new one replaces it */
    status_t st = fs_rename(part, abs);
    if (st != OK) {
        sh_tty("fetch: kept as %s (renaming it to %s: %s)\n", part, abs, sh_why(st));
        return 1;
    }
    sh_say("fetch: saved %s\n", abs);
    return 0;
}

static int to_file(const char *url, const struct http_url *u, const char *arg)
{
    char abs[FS_PATH_MAX], part[FS_PATH_MAX];
    struct jfile f;
    if (!target(u, arg, abs, part))
        return 1;
    status_t st = file_open(part, FS_WRITE | FS_CREATE | FS_TRUNCATE, &f);
    if (st != OK) {
        sh_tty("fetch: %s: can't write it (%s)\n", part, sh_why(st));
        return 1;
    }
    struct spawn_handle x[4];
    file_give(&f, &x[0].h, &x[1].h);   /* to bin/fetch: SR_USER + 0 and + 1 */
    x[0].role = SR_USER + 0;
    x[1].role = SR_USER + 1;
    const char *args[] = { "fetch", url, "file", NULL };
    sh_flush();
    return finish(sh_run_helper(FETCH_PATH, 3, args, x, 2), abs, part);
}

static int to_output(const char *url)
{
    handle_t mine, theirs;
    status_t st = jam_channel_create(&mine, &theirs);
    if (st != OK) {
        sh_tty("fetch: no channel for the body (%s)\n", status_str(st));
        return 1;
    }
    struct spawn_handle x[3] = { { SR_USER + 3, theirs } };
    const char *args[] = { "fetch", url, "pipe", NULL };
    sh_flush();
    int code = sh_run_helper_out(FETCH_PATH, 3, args, x, 1, mine);
    jam_handle_close(mine);
    return code;
}

SH_CMD(fetch)
{
    struct http_url u;
    if (argc < 2 || argc > 3)
        return usage();
    if (!parse(argv[1], &u))
        return 1;
    if ((argc == 2 && sh_piped()) || (argc == 3 && !strcmp(argv[2], "-")))
        return to_output(argv[1]);
    return to_file(argv[1], &u, argc == 3 ? argv[2] : NULL);
}
