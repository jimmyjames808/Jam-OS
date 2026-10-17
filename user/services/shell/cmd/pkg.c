/* pkg: packages (docs/M13.7-PLAN.md sections 8 and 9; the program is
 * user/services/pkg, bin/pkg).
 *   pkg update | install <name>... | install <file.jpk>... | upgrade [<name>...]
 *       | remove [-r] <name>... | list [-u] | info <name> | files <name> | audit | check
 * The repository is pkg.repo in /data/etc/settings ("http://host[:port][/dir]"),
 * else port 8080 of net.host (tools/ports-server.py on the Mac). The shell
 * starts bin/pkg as a helper (sh_run_helper_tick) and gives it what it
 * needs and nothing else: for the commands that change things, init's pkgctl
 * channel (initctl.pkg_channel: init checks and approves each package's
 * programs itself), and a channel for the question. Plans are shown by bin/pkg;
 * when it asks "Install 3 packages, 2 programs?" the question comes here, is
 * shown on the terminal with [y/N], and one key is the answer: bin/pkg never
 * reads the keys, so the yes is the owner's. A path given to `install` is made
 * absolute here (bin/pkg has no working directory). */
#include <idl/initctl.h>
#include <settings.h>
#include "sh.h"

#define PKG_PATH     "bin/pkg"
#define CHANNEL_WAIT (5 * NS_PER_S)
#define ANSWER_WAIT  (300 * NS_PER_S)   /* the owner's y or n */
#define QUESTION_MAX 256u
#define WORDS_MAX    32       /* words of one command */

/* Does this command change what is installed (bin/pkg then needs init)? */
static bool changes(const char *cmd)
{
    return !strcmp(cmd, "install") || !strcmp(cmd, "upgrade") || !strcmp(cmd, "remove") ||
           !strcmp(cmd, "check");
}

/* The owner's answer to the question shown: true for y; Ctrl+C or no answer is no. */
static bool answer(void)
{
    for (;;) {
        int k = sh_poll_key(now() + ANSWER_WAIT);
        if (k == 'y' || k == 'Y') {
            sh_say("y\n");
            return true;
        }
        /* Ctrl+C is 3 (not sh_interrupted(): it would take the keys
         * typed after this one off to the next command line). */
        if (k == 'n' || k == 'N' || k == '\r' || k == '\n' || k < 0 || k == 3) {
            sh_say("n\n");
            return false;
        }
    }
}

/* A question bin/pkg wrote on its channel: shown after the lines it wrote
 * before it, answered with one byte. */
static void tick(void *ctx, handle_t out)
{
    handle_t ask = *(handle_t *)ctx;
    signals_t seen;
    char q[QUESTION_MAX + 1];
    uint32_t got = 0, nh = 0;
    struct channel_read_args r = {
        .h = ask, .rd_cap = QUESTION_MAX, .rd_bytes = (uint64_t)(uintptr_t)q,
        .rd_actual = (uint64_t)(uintptr_t)&got, .rd_hactual = (uint64_t)(uintptr_t)&nh,
    };
    if (jam_object_wait_one(ask, SIG_READABLE, 0, &seen) != OK || !(seen & SIG_READABLE) ||
        jam_channel_read(&r) != OK)
        return;
    q[got] = '\0';
    while (sh_copy_output(out, true, sh_put))   /* what it said before the question */
        ;
    while (sh_poll_key(now()) >= 0)   /* keys typed before the question are not its answer */
        if (sh_interrupted())
            break;
    sh_drop_typeahead();
    sh_say("%s [y/N] ", q);
    sh_flush();
    uint8_t yes = answer() ? 'y' : 'n';
    (void)jam_channel_write(ask, &yes, 1, NULL, 0);   /* pkg gone: nobody to answer */
}

/* The repository's address: pkg.repo, else port 8080 of net.host; "" if neither. */
static void repo_address(char *out, size_t cap)
{
    char host[SETTINGS_VALUE_MAX];
    out[0] = '\0';
    if (settings_get(SETTINGS_FILE, "pkg.repo", out, cap) == OK)
        return;
    if (settings_get(SETTINGS_FILE, "net.host", host, sizeof(host)) == OK)
        snprintf(out, cap, "http://%s:8080/", host);
}

SH_CMD(pkg)
{
    if (argc < 2) {
        sh_tty("usage: pkg update | install <name>... | install <file.jpk>... | upgrade "
               "[<name>...] |\n  remove [-r] <name>... | list [-u] | info <name> | files <name> | "
               "audit | check\n");
        return 2;
    }
    char repo[SETTINGS_VALUE_MAX + 24], paths[WORDS_MAX][SH_PATH_MAX];
    const char *args[WORDS_MAX + 4];
    struct spawn_handle x[4];
    unsigned nx = 0, n = 0;
    handle_t ch = HANDLE_INVALID, ask_mine, ask_theirs;
    if (argc > WORDS_MAX) {
        sh_tty("pkg: too many words\n");
        return 2;
    }
    repo_address(repo, sizeof(repo));
    int first = 1;   /* `pkg -R <address> <command>`: another repository, for this command */
    if (argc > 3 && !strcmp(argv[1], "-R")) {
        snprintf(repo, sizeof(repo), "%s", argv[2]);
        first = 3;
    }
    /* `pkg [-R ..] -H <point> <command>`: for tools/pkg-test.sh, bin/pkg stands
     * still when its journal reaches the point (bin/pkg's main.c). */
    const char *hold = NULL;
    if (argc > first + 2 && !strcmp(argv[first], "-H")) {
        hold = argv[first + 1];
        first += 2;
    }
    if (changes(argv[first])) {
        status_t st = sh_initctl() ? initctl_pkg_channel_until(sh_initctl(), now() + CHANNEL_WAIT,
                                                               &ch) : ERR_NOT_FOUND;
        if (st != OK) {
            sh_tty("pkg: the system can't take a package now (%s)\n", status_str(st));
            return 1;
        }
        x[nx++] = (struct spawn_handle){ PKG_SR_CTL, ch };
    }
    if (jam_channel_create(&ask_mine, &ask_theirs) != OK) {
        if (nx)
            jam_handle_close(ch);
        sh_tty("pkg: can't make a channel for the question\n");
        return 1;
    }
    x[nx++] = (struct spawn_handle){ PKG_SR_ASK, ask_theirs };
    args[n++] = "pkg";
    if (repo[0]) {
        args[n++] = "-R";
        args[n++] = repo;
    }
    if (hold) {
        args[n++] = "-H";
        args[n++] = hold;
    }
    for (int i = first; i < argc; i++) {
        bool file = i > first && !strcmp(argv[first], "install") && (strchr(argv[i], '/') ||
                    (strlen(argv[i]) > 4 && !strcmp(argv[i] + strlen(argv[i]) - 4, ".jpk")));
        if (file && sh_resolve(argv[i], paths[i], sizeof(paths[i])))
            args[n++] = paths[i];
        else
            args[n++] = argv[i];
    }
    args[n] = NULL;
    sh_flush();
    int code = sh_run_helper_tick(PKG_PATH, (int)n, args, x, nx, tick, &ask_mine);
    jam_handle_close(ask_mine);
    return code;
}
