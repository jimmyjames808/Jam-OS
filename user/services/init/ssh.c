/* init's SSH server (docs/M13-PLAN.md section 15 and question 8, "X2
 * notes"): bin/sshd, the tinyssh port's listener (user/ports/tinyssh), runs
 * as a service on port 22 whenever /data/etc/authorized_keys holds a key
 * and the settings don't say `ssh = off`. A public build (no key file)
 * listens on nothing.
 *
 * Before its first start this boot, init looks at /data:
 *   - the setting `ssh`: off, and sshd is not started this boot;
 *   - the keys in /data/etc/authorized_keys (<sshkey.h>'s count): none,
 *     the same;
 *   - the host key in /data/etc/ssh: made if it is missing or broken, with
 *     Monocypher's Ed25519 from an os_random seed, in tinyssh's files, by
 *     init itself (so no ported program ever holds /data's etc writable);
 *     a key that can't be read for another reason (an I/O error) is never
 *     replaced. Its fingerprint goes into the log, as the Mac's `ssh`
 *     prints it at a first connection.
 * Those are reads (and at most two writes) of /data, which on a slow
 * stick wait seconds behind logd's writes: a thread of its own makes them
 * (the loop never waits for the stick: net.c's settings reader is the
 * same) and wakes the loop with KEY_SSHD. Once a boot: a restarted sshd
 * (it crashed, or `kill sshd`) goes by the same look. A key added later
 * counts from the next boot.
 *
 * sshd's namespace is shell.c's grants (its list's): /svc/net and
 * /svc/net-low (port 22), /svc/session (each login's shell) and /data
 * read-only (the host key and authorized_keys, for its tinysshds). */
#include <monocypher-ed25519.h>
#include <monocypher.h>
#include <settings.h>
#include <sshkey.h>
#include "init.h"

#define LOOK_STACK (64u << 10)   /* the look's thread: settings_get, file calls */

/* What the look found. */
enum look_result {
    LOOK_OFF,       /* `ssh = off` */
    LOOK_NO_FILE,   /* no /data/etc/authorized_keys */
    LOOK_NO_KEYS,   /* the file has no key tinyssh takes */
    LOOK_FAILED,    /* the keys or the host key can't be read or made: st */
    LOOK_OK,        /* sshd may start */
};

static struct {
    handle_t         port;         /* the loop's (KEY_SSHD) */
    handle_t         thread;       /* the look's thread (0: not started) */
    bool             done;         /* the look is over (atomic: the thread sets it) */
    bool             said;         /* ssh_event has said what it found */
    enum look_result result;
    status_t         st;           /* LOOK_FAILED: why */
    const char      *what;         /* LOOK_FAILED: what failed */
    unsigned         keys;         /* LOOK_OK: the keys that may log in */
    bool             made;         /* LOOK_OK: the host key was made this boot */
    status_t         old;          /* ... in place of: ERR_NOT_FOUND (none), else broken */
    char             fp[SSH_FP_MAX];   /* LOOK_OK: the host key's fingerprint */
} look;
static uint8_t look_stack[LOOK_STACK] __attribute__((aligned(16)));

/* The whole file at path replaced by n bytes of data, synced. */
static status_t write_file(const char *path, const uint8_t *data, size_t n)
{
    struct jfile f;
    status_t st = file_open(path, FS_WRITE | FS_CREATE | FS_TRUNCATE, &f);
    if (st != OK)
        return st;
    size_t done = 0;
    st = file_write(&f, 0, data, n, &done);
    if (st == OK && done != n)
        st = ERR_IO;
    if (st == OK)
        st = file_sync(&f);
    file_close(&f);
    return st;
}

/* The host key's public half into pk: the one in SSH_DIR, or a new pair
 * made and written there (look.made, look.old). */
static status_t host_key(uint8_t pk[SSH_PK_BYTES])
{
    uint8_t sk[SSH_SK_BYTES];
    status_t st = ssh_file_read(SSH_HOST_SK, sk, sizeof(sk));
    if (st == OK)
        st = ssh_file_read(SSH_HOST_PK, pk, SSH_PK_BYTES);
    if (st == OK && memcmp(sk + SSH_SK_BYTES - SSH_PK_BYTES, pk, SSH_PK_BYTES))
        st = ERR_BAD_STATE;   /* not a pair: the secret key's second half is its public key */
    if (st == OK || (st != ERR_NOT_FOUND && st != ERR_OUT_OF_RANGE && st != ERR_BAD_STATE)) {
        crypto_wipe(sk, sizeof(sk));
        return st;   /* the key; or one we couldn't read, which is never replaced */
    }
    look.made = true;
    look.old = st;
    uint8_t seed[32];
    os_random(seed, sizeof(seed));
    crypto_ed25519_key_pair(sk, pk, seed);   /* wipes the seed */
    st = fs_mkdir(SSH_DIR);
    if (st == ERR_ALREADY_EXISTS)
        st = OK;
    if (st == OK)
        st = write_file(SSH_HOST_SK, sk, sizeof(sk));   /* the secret first: a pk alone is broken */
    if (st == OK)
        st = write_file(SSH_HOST_PK, pk, SSH_PK_BYTES);
    crypto_wipe(sk, sizeof(sk));
    return st;
}

/* The look itself, on its thread: the settings, the keys, the host key. */
static enum look_result look_at_data(void)
{
    char v[16];
    if (settings_get(SETTINGS_FILE, "ssh", v, sizeof(v)) == OK && !strcmp(v, "off"))
        return LOOK_OFF;
    status_t st = ssh_keys_in_file(SSH_AUTH_KEYS, &look.keys);
    if (st == ERR_NOT_FOUND)
        return LOOK_NO_FILE;
    if (st != OK) {
        look.st = st;
        look.what = SSH_AUTH_KEYS " can't be read";
        return LOOK_FAILED;
    }
    if (!look.keys)
        return LOOK_NO_KEYS;
    uint8_t pk[SSH_PK_BYTES];
    st = host_key(pk);
    if (st != OK) {
        look.st = st;
        look.what = look.made ? "the host key can't be made" : "the host key can't be read";
        return LOOK_FAILED;
    }
    ssh_fingerprint(pk, look.fp);
    return LOOK_OK;
}

static void look_main(void *arg)
{
    (void)arg;
    look.result = look_at_data();
    __atomic_store_n(&look.done, true, __ATOMIC_RELEASE);
    struct port_packet pkt = { .key = KEY_SSHD, .type = PORT_PACKET_USER };
    for (int i = 0; i < 100 && jam_port_queue(look.port, &pkt) != OK; i++)
        jam_sleep_until(now() + 10 * NS_PER_MS);   /* a full port: the loop is far behind */
}

void ssh_init(handle_t port)
{
    look.port = port;
}

bool ssh_ready(void)
{
    if (__atomic_load_n(&look.done, __ATOMIC_ACQUIRE))
        return true;
    if (!look.thread) {
        status_t st = thread_spawn("ssh", look_main, NULL, look_stack, sizeof(look_stack),
                                   &look.thread);
        if (st != OK) {
            printf("init: no thread to look at /data for the SSH server (%s): looked at in the "
                   "loop\n", status_str(st));
            look_main(NULL);
        }
    }
    return __atomic_load_n(&look.done, __ATOMIC_ACQUIRE);
}

void ssh_event(void)
{
    if (!__atomic_load_n(&look.done, __ATOMIC_ACQUIRE) || look.said)
        return;
    look.said = true;
    switch (look.result) {
    case LOOK_OFF:
        printf("init: ssh = off in the settings: no SSH server this boot\n");
        break;
    case LOOK_NO_FILE:
        printf("init: no %s: no SSH server this boot\n", SSH_AUTH_KEYS);
        break;
    case LOOK_NO_KEYS:
        printf("init: no ssh-ed25519 key in %s: no SSH server this boot\n", SSH_AUTH_KEYS);
        break;
    case LOOK_FAILED:
        printf("init: %s (%s): no SSH server this boot\n", look.what, status_str(look.st));
        break;
    case LOOK_OK:
        if (look.made)
            printf("init: SSH host key made in %s%s\n", SSH_DIR,
                   look.old == ERR_NOT_FOUND ? "" : " in place of a broken one");
        printf("init: SSH host key %s (ed25519); %u key%s may log in\n", look.fp, look.keys,
               look.keys == 1 ? "" : "s");
        break;
    }
}

status_t ssh_start(void)
{
    ssh_event();   /* said before the first start, whichever came first */
    if (look.result != LOOK_OK) {
        svcs[SSHD].given_up = true;   /* this boot: the look is made once */
        return OK;
    }
    return svc_start1(SSHD, NULL, 0);
}
