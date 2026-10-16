/* bin/pkg: packages (docs/M13.7-PLAN.md sections 8 and 9). It fetches the
 * Mac's repository over HTTP, shows the owner what an install would do,
 * asks once, and writes the files under /data/pkg. The shell's `pkg`
 * starts it as a helper with the repository's address and the channels it
 * needs:
 *   argv: pkg [-R <repository address>] <command> [arguments]
 *   PKG_SR_CTL         init's pkgctl channel (initctl.pkg_channel): init
 *                      checks and approves a package's programs
 *   PKG_SR_ASK         where the question goes: a message with the question,
 *                      answered by one byte, 'y' or anything else for no; the
 *                      shell asks on the terminal (the answer is the owner's)
 *   PROGRAM_SR_STOP    the shell's stop channel: Ctrl+C
 * Without the channels (run by hand) the commands that only look work
 * (list, info, files, check, audit, update); nothing is installed.
 *
 * It parses what the network sends, so it holds only what it needs: the
 * network and the resolver, the boot image (the ports key and the ABI stamp,
 * read-only), a stick (to install a package from), and /data with the
 * packages' prefix writable and nothing under etc. What may run is init's to
 * decide: it checks the manifest's signature and every program itself.
 *
 * Its lines are the owner's, in plain words. Exit: 0 done, 1 failed or
 * refused, 2 usage, 3 an upgrade or removal waits for a program in use, 130
 * stopped. */
#include <update.h>
#include <wants.h>
#include "pkg.h"

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("svc net\n"
          "svc dns\n"
          "mount /boot r\n"
          "mount /usb* r\n"
          "mount /data rw pkg\n");

#define PREFIX     "/data/pkg"
#define KEY_FILE   "/boot/ports.pub"
#define BUILD_FILE "/boot/build.txt"

/* This build's ports key. false: it has none, and takes no repository. */
static bool load_key(struct pk_env *e)
{
    uint8_t *t;
    uint64_t n;
    if (pk_read(KEY_FILE, 4096, &t, &n) != OK)
        return false;
    bool ok = update_key_parse(t, (size_t)n, e->key) == OK;
    free(t);
    return ok;
}

/* The ABI stamp of this build (build.txt's "abi" line); "" if it has none. */
static void load_abi(struct pk_env *e)
{
    uint8_t *t;
    uint64_t n;
    e->abi[0] = '\0';
    if (pk_read(BUILD_FILE, 4096, &t, &n) != OK)
        return;
    for (uint64_t i = 0; i + 4 + 16 <= n; i++) {
        bool line_start = i == 0 || t[i - 1] == '\n';
        if (line_start && !memcmp(t + i, "abi ", 4) && (i + 4 + 16 == n || t[i + 4 + 16] == '\n')) {
            memcpy(e->abi, t + i + 4, 16);
            e->abi[16] = '\0';
        }
    }
    free(t);
}

static bool stopped(void *ctx)
{
    (void)ctx;
    return pk_net_stopped();
}

static const struct {
    const char *name;
    int       (*run)(struct pk_app *a, int argc, char **argv);
    const char *usage;
} commands[] = {
    { "update",  pk_cmd_update,  "update [-f]            fetch the repository's index; what is new" },
    { "install", pk_cmd_install, "install <name>...      with what they run, dependencies first" },
    { "upgrade", pk_cmd_upgrade, "upgrade [<name>...]    everything with a newer version" },
    { "remove",  pk_cmd_remove,  "remove [-r] <name>...  -r: and what runs them" },
    { "list",    pk_cmd_list,    "list [-u]              installed (-u: with upgrades waiting)" },
    { "info",    pk_cmd_info,    "info <name>            licence, programs and what they ask for" },
    { "files",   pk_cmd_files,   "files <name>           its files" },
    { "audit",   pk_cmd_audit,   "audit                  installed packages against the advisories" },
    { "check",   pk_cmd_check,   "check                  every installed file against its manifest" },
};

static int usage(void)
{
    pk_say("usage: pkg [-R <repository address>] <command>\n");
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        pk_say("  pkg %s\n", commands[i].usage);
    pk_say("  pkg install <file.jpk>  a package from a stick\n");
    return 2;
}

int main(int argc, char **argv)
{
    static struct pk_app a;
    int i = 1;
    const char *url = NULL;
    if (i + 1 < argc && !strcmp(argv[i], "-R")) {
        url = argv[i + 1];
        i += 2;
    }
    if (i >= argc)
        return usage();
    a.env.prefix = PREFIX;
    a.env.have_key = load_key(&a.env);
    load_abi(&a.env);
    a.env.stopped = stopped;
    a.ctl = startup_handle(PKG_SR_CTL);
    a.ask = startup_handle(PKG_SR_ASK);
    pk_ctl_make(&a.env.ctl, &a.ctl);
    if (url && url[0])
        a.have_repo = pk_repo_set(&a.repo, url, a.repo_why) == OK;
    for (unsigned k = 0; k < sizeof(commands) / sizeof(commands[0]); k++)
        if (!strcmp(argv[i], commands[k].name))
            return commands[k].run(&a, argc - i, argv + i);
    pk_say("pkg: %s is not a command\n", argv[i]);
    return usage();
}
