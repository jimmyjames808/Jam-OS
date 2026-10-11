/* ssh: the SSH server (bin/sshd, the tinyssh port; init's ssh.c): the host
 * key's fingerprint, as the Mac's `ssh` shows it at a first connection,
 * how many keys may log in, and whether the server runs, with its
 * connections (its tinysshd processes: bin/sshd's own job). Nothing is
 * changed here: a key goes into /data/etc/authorized_keys and `ssh = off`
 * into the settings, each counted from the next boot. */
#include <settings.h>
#include <sshkey.h>
#include "sh.h"

/* The server's process (the first bin/sshd: the listener, its connections'
 * tinysshds after it in its job) and how many of those run. */
static bool server(uint64_t *koid, int *conns)
{
    struct proc_stat *p = calloc(SH_MAX_PROCS, sizeof(*p));
    int n = p ? sh_procs(p, "ssh") : -1;
    int first = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(p[i].name, "sshd") && (first < 0 || p[i].koid < p[first].koid))
            first = i;
    *conns = 0;
    for (int i = 0; first >= 0 && i < n; i++)
        if (i != first && !strcmp(p[i].name, "sshd") && p[i].job_koid == p[first].job_koid)
            (*conns)++;
    if (first >= 0)
        *koid = p[first].koid;
    free(p);
    return first >= 0;
}

SH_CMD(ssh)
{
    (void)argv;
    if (argc > 1) {
        sh_tty("usage: ssh\n");
        return 2;
    }
    uint8_t pk[SSH_PK_BYTES];
    char fp[SSH_FP_MAX];
    status_t st = ssh_file_read(SSH_HOST_PK, pk, sizeof(pk));
    if (st == OK) {
        ssh_fingerprint(pk, fp);
        sh_say("host key: %s (ed25519, %s)\n", fp, SSH_DIR);
    } else if (st == ERR_NOT_FOUND) {
        sh_say("host key: none yet (init makes it at the first boot with a key to let in)\n");
    } else {
        sh_say("host key: %s can't be read (%s)\n", SSH_HOST_PK, status_str(st));
    }
    unsigned keys = 0;
    st = ssh_keys_in_file(SSH_AUTH_KEYS, &keys);
    if (st == OK)
        sh_say("keys that may log in: %u (%s, ssh-ed25519 lines)\n", keys, SSH_AUTH_KEYS);
    else
        sh_say("keys that may log in: none (%s: %s)\n", SSH_AUTH_KEYS,
               st == ERR_NOT_FOUND ? "no such file" : status_str(st));
    char v[16];
    bool off = settings_get(SETTINGS_FILE, "ssh", v, sizeof(v)) == OK && !strcmp(v, "off");
    uint64_t koid = 0;
    int conns = 0;
    if (server(&koid, &conns))
        sh_say("server: running on port 22 (bin/sshd, pid %lu), %d connection%s\n",
               (unsigned long)koid, conns, conns == 1 ? "" : "s");
    else
        sh_say("server: not running%s\n", off       ? " (ssh = off in the settings)"
                                          : !keys ? " (no key may log in)"
                                                  : "");
    return 0;
}
