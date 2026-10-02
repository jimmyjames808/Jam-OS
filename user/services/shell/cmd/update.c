/* update: run the build the Mac serves (tools/update-server.py), without
 * moving the stick (docs/M9-PLAN.md "update: a new build from the Mac").
 *   update [-n] [server address]
 * The server is net.host in /data/etc/settings unless given. The shell
 * takes an offer channel from init (initctl.update_offer) and starts
 * bin/update (user/services/update) with it: the fetcher holds only that
 * channel and /svc/net, offers what it fetched, and init checks it
 * against the manifest (signed by the key in this build's boot image) and
 * makes it the stored kernel (<update.h>). Then the shell reboots into it,
 * the normal way (`reboot`). -n: fetched and checked, nothing loaded, no
 * reboot. Only RAM changes: `make flash` keeps a build for good. A build
 * without a key fetches nothing: init would refuse every build. */
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

SH_CMD(update)
{
    bool check_only = false;
    const char *given = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && !check_only) {
            check_only = true;
        } else if (argv[i][0] != '-' && !given) {
            given = argv[i];
        } else {
            sh_tty("usage: update [-n] [server address]   (-n: fetch and check only)\n");
            return 2;
        }
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
        sh_tty("update: init: %s\n", st == ERR_BAD_STATE ? "still checking the last one"
                                                        : status_str(st));
        return 1;
    }
    struct spawn_handle x[3] = { { SR_USER + 0, ch } };
    const char *args[] = { "update", host, check_only ? "check" : "load", s.version, git, NULL };
    sh_flush();
    int code = sh_run_helper(UPDATE_PATH, 5, args, x, 1);
    if (code || check_only)
        return code;
    char *reboot_args[] = { "reboot", NULL };
    return shc_reboot(1, reboot_args);
}
