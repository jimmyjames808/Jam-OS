/*
20140214
20241208 - reformated using clang-format
Jan Mojzis
Public domain.

Jam OS's override of third_party/tinyssh/subprocess_auth.c (listed in
user/ports/tinyssh/OVERRIDES). What changed, and why:
 - The checking child is this program started again as "tinysshd-auth"
   (helper.h), not a fork(): Jam OS has no fork. Its answer is still its
   exit code, and it gets the account, the key's type and the key as
   arguments (upstream's child had them in the parent's memory).
 - The key list is /data/etc/authorized_keys, for every login, not
   ~/.ssh/authorized_keys (docs/M13-PLAN.md section 15: the owner's place;
   the one user's home is /data, and /data's top-level etc is the system's,
   which no ported program can write).
 - No privileges are dropped (dropuidgid): there is one user, uid 1000, and
   authority is in handles; the account must still exist (getpwnam: only
   `jam`, question 7), so any other name is refused.
 - The log lines name the file read, not upstream's dir/.ssh path.
The rest (the file's and its folders' checks, the search for the key) is
upstream's.
*/

#include <stdlib.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include "bug.h"
#include "e.h"
#include "getln.h"
#include "global.h"
#include "helper.h"
#include "limit.h"
#include "log.h"
#include "open.h"
#include "sshcrypto.h"
#include "str.h"
#include "subprocess.h"

/* Where the keys allowed to log in are: AUTH_DIR/authorized_keys. */
#define AUTH_DIR "/data/etc"

static int findnameandkey(const char *keyname, const char *key, char *x) {

    if (!str_start(x, keyname)) return 0;
    x += str_len(keyname);
    if (*x != ' ') return 0;
    x += 1;
    if (!str_start(x, key)) return 0;
    x += str_len(key);
    if (*x == ' ') return 1;
    if (*x == '\n') return 1;
    if (*x == '\r') return 1;
    if (*x == 0) return 1;
    return 0;
}

static void check(uid_t uid, const char *d, const char *f, long long *err) {

    struct stat st;
    int e = 0;

    if (f) {
        if (stat(f, &st) == -1) {
            log_w4("auth: unable to stat file: ", d, "/", f);
            e = 1;
        }
        if (e == 0 && !S_ISREG(st.st_mode)) {
            errno = EACCES;
            log_w4("auth: non-regular file: ", d, "/", f);
            e = 1;
        }
    }
    else {
        if (stat(d, &st) == -1) {
            log_w2("auth: unable to stat directory: ", d);
            e = 1;
        }
        if (e == 0 && !S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            log_w2("auth: not a directory: ", d);
            e = 1;
        }
    }
    if (e == 0 && (st.st_mode & 022) != 0) {
        errno = EACCES;
        if (f)
            log_w4("auth: bad mode: writable by group or others: ", d, "/",
                   f);
        else
            log_w2("auth: bad mode: writable by group or others: ", d);
        e = 1;
    }
    if (e == 0 && st.st_uid != uid && st.st_uid != 0) {
        errno = EACCES;
        if (f) log_w4("auth: bad owner: ", d, "/", f);
        else log_w2("auth: bad owner: ", d);
        e = 1;
    }

    if (e)
        *err = 1;
    else if (f)
        log_d4("auth: path: ok: ", d, "/", f);
    else
        log_d2("auth: path: ok: ", d);
}

int subprocess_auth_checkpath_(char *path, long long pathlen, uid_t uid,
                               int fd) {

    long long err = 0, j, i;
    char *real;
    struct stat fst, st;

    if (fstat(fd, &fst) == -1) return 0;
    if (!S_ISREG(fst.st_mode) || (fst.st_mode & 022) != 0 ||
        (fst.st_uid != uid && fst.st_uid != 0))
        return 0;

    real = realpath("authorized_keys", 0);
    if (!real) return 0;
    if (!str_copyn(path, pathlen, real)) {
        free(real);
        return 0;
    }
    free(real);

    /* realpath() is name-based; tie its result to the open descriptor */
    if (stat(path, &st) == -1 || st.st_dev != fst.st_dev ||
        st.st_ino != fst.st_ino)
        return 0;

    i = str_len(path);
    for (j = i; j >= 0; --j) {
        if (path[j] == '/') {
            path[j] = 0;
            i = j;
            break;
        }
    }

    if (i > 0) {
        do {
            check(uid, path, 0, &err);
            for (j = i; j >= 0; --j) {
                if (path[j] == '/') {
                    path[j] = 0;
                    i = j;
                    break;
                }
            }
        } while (j > 0);
    }

    /*
    Do not check /: in a chroot its ownership and permissions are controlled
    by the chroot setup, which may make authorized_keys tests fail even when
    the file and all directories below / are safe.
    */
    /* check(uid, "/", 0, &err); */

    return (err == 0);
}

int subprocess_auth_authorizedkeys_(const char *keyname, const char *key,
                                    const char *dir, char *buf,
                                    long long bufmax) {

    int fd = -1;
    int r;

    fd = open_read("authorized_keys");
    if (fd == -1) {
        log_w3("auth: unable to open file: ", dir, "/authorized_keys");
        return 0;
    }

    if (!subprocess_auth_checkpath_(buf, bufmax, geteuid(), fd)) {
        errno = EACCES;
        log_w3("auth: unable to read file: ", dir, "/authorized_keys");
        close(fd);
        return 0;
    }

    do {
        r = getln_nul_as_newline(fd, buf, bufmax);
        if (r == -1) {
            log_w3("auth: unable to read from file ", dir, "/authorized_keys");
            close(fd);
            return 0;
        }
        if (findnameandkey(keyname, key, buf)) {
            close(fd);
            return 1; /* authorized */
        }
    } while (r > 0);
    close(fd);
    log_w1("auth: unable to authorize using authorized_keys: key not found");
    return 0;
}

/*
The 'subprocess_auth' is used for authorization using
/data/etc/authorized_keys file: 0 when 'account' may log in with 'key'.
*/
int subprocess_auth(const char *account, const char *keyname, const char *key) {

    pid_t pid;
    char *argv[5];

    if (!account || !keyname || !key) bug_inval();
    argv[0] = (char *) "tinysshd-auth";
    argv[1] = (char *) account;
    argv[2] = (char *) keyname;
    argv[3] = (char *) key;
    argv[4] = 0;
    pid = helper_spawn(argv, -1, -1);
    if (pid == -1) return -1;
    return helper_wait(pid);
}

/*
The child: argv is "tinysshd-auth" account keyname key. Exits 0 when the
account exists and its key is in AUTH_DIR/authorized_keys, else 111 (said
in the log).
*/
int subprocess_auth_main(int argc, char **argv) {

#define buf global_bspace2 /* reusing global buffer */
    struct passwd *pw;

    log_init(2, "tinysshd-auth", 0, 0);
    if (argc != 4) bug_inval();
    if (sshcrypto_sign_BASE64PUBLICKEYMIN > str_len(argv[3]) + 1) bug_inval();

    pw = getpwnam(argv[1]);
    if (!pw) {
        log_w3("auth: account ", argv[1], ": not exist");
        global_die(111);
    }

    if (chdir(AUTH_DIR) == -1) {
        log_w2("auth: unable to change directory to ", AUTH_DIR);
        global_die(111);
    }

    /* authorization starts here */
    if (!subprocess_auth_authorizedkeys_(argv[2], argv[3], AUTH_DIR,
                                         (char *) buf, sizeof buf))
        global_die(111);
    /* authorization ends here */

    global_die(0);
    return 111;
}
