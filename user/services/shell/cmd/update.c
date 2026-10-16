/* update: take the build the Mac serves (tools/update-server.py), without
 * moving the stick (docs/HISTORY.md, M9).
 *   update [-n | -m | -w] [-r] [-f] [server address]
 * The server is net.host in /data/etc/settings unless given. The shell
 * takes an offer channel from init (initctl.update_offer) and starts
 * bin/update (user/services/update) with it: the fetcher holds only that
 * channel and /svc/net, offers what it fetched, and init checks it
 * against the manifest (signed by the key in this build's boot image) and
 * makes it the stored kernel (<update.h>), the one `reboot` starts.
 * Plain `update`: init also writes the build (and its boot menu) to the
 * stick, the stick's own build kept as "Jam OS (previous build)", so
 * `reboot -f` and a power-on start it too. Then it stops: nothing
 * reboots, the last line says what `reboot` and `reboot -f` start now.
 * If the stick write fails, init says how far it got and that the stick
 * still boots; the build stays loaded (`reboot` runs it). -w: the same as
 * plain `update` (its spelling when the plain one wrote nothing). -m: into
 * memory only, the stick untouched: `reboot` runs it, a power-off (or
 * `reboot -f`) brings back the stick's (for a build you'd rather try
 * first). -n: fetched and checked, nothing loaded or written. -r: once it
 * is loaded (and written, without -m), reboot into it at once (kexec), as
 * `update` did before it stopped. Only init can write the stick: the shell
 * and bin/update just ask. A build without a key fetches nothing: init
 * would refuse every build. -f: taken even if its network default
 * (vlan21, untagged: its build.txt) isn't this build's, which init
 * otherwise refuses. The shell also tells bin/update this build's ABI
 * stamp (build.txt's `abi` line) and how many packages are installed, so
 * that after a build with another stamp it can say they were built for the
 * old system (docs/M13.7-PLAN.md section 11). */
#include <idl/initctl.h>
#include <ipv4.h>
#include <settings.h>
#include <update.h>
#include "sh.h"

#define UPDATE_PATH "bin/update"
#define OFFER_WAIT  (5 * NS_PER_S)

/* Does this build have an update key (<update.h> UPDATE_KEY_FILE in its
 * boot image)? Without one init refuses every build, so nothing is
 * fetched: false (said). */
static bool has_key(void)
{
    if (fs_stat("/boot/" UPDATE_KEY_FILE, NULL, NULL, NULL) == OK)
        return true;
    sh_tty("update: this build has no update key: updates are off.\n"
           "  On the Mac, once: make, then build/host/jamos-sign keygen (the key goes in\n"
           "  ~/.config/jamos), then make and make flash: the first build with the key\n"
           "  goes on the stick by hand; after that `update` takes the builds it signs.\n");
    return false;
}

/* The server's address: given, or net.host. false (said) if none. */
static bool server(const char *given, char *out, size_t cap)
{
    if (given) {
        snprintf(out, cap, "%s", given);
    } else if (settings_get(SETTINGS_FILE, "net.host", out, cap) != OK) {
        sh_tty("update: no server: set net.host in /data/etc/settings to the Mac's address\n"
               "  (e.g. net.host = 10.2.21.174), or give it: update 10.2.21.174\n");
        return false;
    }
    uint32_t a;
    const char *end;
    if (!ipv4_parse(out, &a, &end) || *end) {
        sh_tty("update: %s is not an IPv4 address\n", out);
        return false;
    }
    return true;
}

/* How many packages are installed: the files of /data/pkg/db that are
 * manifests (not serial or journal, nor a `.new` or `.sum` of a transaction:
 * the names bin/pkg's pk_name_reserved keeps). bin/update says how many
 * were built for the old system when a new build's ABI stamp differs. */
static unsigned packages_installed(void)
{
    struct fs_entry ent;
    unsigned n = 0;
    for (unsigned i = 0; fs_readdir("/data/pkg/db", i, &ent) == OK; i++) {
        size_t l = strlen(ent.name);
        bool aside = !strcmp(ent.name, "serial") || !strcmp(ent.name, "journal") ||
                     (l > 4 && (!strcmp(ent.name + l - 4, ".new") ||
                                !strcmp(ent.name + l - 4, ".sum")));
        if (!ent.is_dir && !aside)
            n++;
    }
    return n;
}

SH_CMD(update)
{
    /* bin/update's mode: write (plain, -w), load (-m: memory only), check (-n) */
    const char *mode = "write", *given = NULL;
    bool force = false, then_reboot = false, mode_given = false, bad = false;
    for (int i = 1; i < argc && !bad; i++) {
        const char *m = !strcmp(argv[i], "-n")   ? "check"
                        : !strcmp(argv[i], "-m") ? "load"
                        : !strcmp(argv[i], "-w") ? "write"
                                                 : NULL;
        if (m && !mode_given) {
            mode = m;
            mode_given = true;
        } else if (!strcmp(argv[i], "-r") && !then_reboot) {
            then_reboot = true;
        } else if (!strcmp(argv[i], "-f") && !force) {
            force = true;
        } else if (argv[i][0] != '-' && !given) {
            given = argv[i];
        } else {
            bad = true;
        }
    }
    if (bad || (then_reboot && !strcmp(mode, "check"))) {   /* -n -r: nothing to reboot into */
        sh_tty("usage: update [-n | -m | -w] [-r] [-f] [server address]\n"
               "  plain: fetch, check, write it to the stick and load it; then `reboot`\n"
               "         starts it (kexec), `reboot -f` too (through the firmware)\n"
               "  -m: load it into memory only, the stick untouched (a power-off brings\n"
               "      back the stick's); -n: fetch and check only, nothing loaded or written\n"
               "  -w: the same as plain; -r: reboot into it (kexec) once loaded;\n"
               "  -f: even if its network default (VLAN or untagged) isn't this build's\n");
        return 2;
    }
    char host[SETTINGS_VALUE_MAX], git[48];
    struct sys_info s;
    if (!has_key() || !server(given, host, sizeof(host)) || !sh_sysinfo(&s, "update"))
        return 1;
    sh_build_git(git, sizeof(git));
    handle_t ch = HANDLE_INVALID;
    status_t st = sh_initctl() ? initctl_update_offer_until(sh_initctl(), now() + OFFER_WAIT, &ch)
                               : ERR_NOT_FOUND;
    if (st != OK) {
        if (st == ERR_BAD_STATE)
            sh_tty("update: the last update is still being checked: try again in a moment\n");
        else
            sh_tty("update: the system can't take an update now (%s)\n", status_str(st));
        return 1;
    }
    struct spawn_handle x[3] = { { UPDATE_SR_OFFER, ch } };
    char abi_word[8 + UPDATE_ABI_LEN + 1], abi[UPDATE_ABI_LEN + 1], pkgs_word[24];
    const char *args[8] = { "update", host, mode, s.version, git };
    int n = 5;
    if (sh_build_abi(abi)) {   /* what bin/update compares the new build's stamp with */
        snprintf(abi_word, sizeof(abi_word), "abi=%s", abi);
        args[n++] = abi_word;
    }
    snprintf(pkgs_word, sizeof(pkgs_word), "packages=%u", packages_installed());
    args[n++] = pkgs_word;
    if (force)
        args[n++] = "force";
    args[n] = NULL;
    sh_flush();
    int code = sh_run_helper(UPDATE_PATH, n, args, x, 1);
    if (!then_reboot)
        return code;   /* bin/update's last line said what `reboot` starts now */
    if (code == 3)
        sh_tty("update: no reboot (-r): the stick write failed; `reboot` starts the loaded "
               "build\n");
    if (code)
        return code;
    sh_tty("update: rebooting into it (-r)\n");
    char *reboot_args[] = { "reboot", NULL };
    return shc_reboot(1, reboot_args);
}
