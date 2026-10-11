/*
20140117
20241208 - reformated using clang-format
Jan Mojzis
Public domain.

Jam OS's override of third_party/tinyssh/subprocess_sign.c (listed in
user/ports/tinyssh/OVERRIDES). What changed, and why:
 - The signing child is this program started again as "tinysshd-sign"
   (helper.h), not a fork(): Jam OS has no fork. The separation upstream
   wants stays: the secret key is read and used only in the helper, never
   in the process that parses the network's bytes.
 - The child is told the key's name (argv): upstream's child inherited the
   kex's choice (sshcrypto_key_select) with the parent's memory.
 - The hash goes to the child on a pipe (its descriptor 0) and the
   signature comes back on another (its 1), both blocking (pipe2, not
   open_pipe's non-blocking ones: readall spins on EAGAIN).
 - subprocess_sign_main is the child's side (tinysshd.c calls it).
*/

#include <fcntl.h>
#include <unistd.h>
#include "bug.h"
#include "e.h"
#include "global.h"
#include "helper.h"
#include "load.h"
#include "log.h"
#include "purge.h"
#include "readall.h"
#include "sshcrypto.h"
#include "str.h"
#include "subprocess.h"
#include "writeall.h"

/*
The 'subprocess_sign' function reads secret-key from 'keydir' and signs the data
'x' of length 'xlen' and returns it in 'y'. Caller is expecting 'y' of length
'ylen'. Signing is done in a different process, so secret-key is in a separate
memory space than rest of the program.
*/
int subprocess_sign(unsigned char *y, long long ylen, const char *keydir,
                    unsigned char *x, long long xlen) {

    pid_t pid;
    int tochild[2] = {-1, -1}, fromchild[2] = {-1, -1};
    int ok = 0, code;
    char *argv[4];

    if (ylen != sshcrypto_sign_bytes) bug_inval();
    if (xlen != sshcrypto_hash_bytes) bug_inval();
    if (!y || !keydir || !x || !sshcrypto_key_name) bug_inval();

    if (pipe2(tochild, O_CLOEXEC) == -1) return -1;
    if (pipe2(fromchild, O_CLOEXEC) == -1) {
        close(tochild[0]);
        close(tochild[1]);
        return -1;
    }
    argv[0] = (char *) "tinysshd-sign";
    argv[1] = (char *) keydir;
    argv[2] = (char *) sshcrypto_key_name;
    argv[3] = 0;
    pid = helper_spawn(argv, tochild[0], fromchild[1]);
    close(tochild[0]);
    close(fromchild[1]);
    if (pid == -1) {
        close(tochild[1]);
        close(fromchild[0]);
        return -1;
    }
    if (writeall(tochild[1], x, xlen) == 0) {
        close(tochild[1]);
        tochild[1] = -1;
        ok = readall(fromchild[0], y, ylen) == 0;
    }
    if (tochild[1] != -1) close(tochild[1]);
    close(fromchild[0]);

    code = helper_wait(pid);
    return ok ? code : -1;
}

/*
The child: argv is "tinysshd-sign" keydir keyname. The hash (at most
sshcrypto_hash_MAX bytes, up to the end of descriptor 0) is signed with
keyname's secret key from keydir, and the signature written to 1. Exits 0,
or 111 (said in the log).
*/
int subprocess_sign_main(int argc, char **argv) {

    unsigned char sk[sshcrypto_sign_SECRETKEYMAX];
    unsigned char sm[sshcrypto_sign_MAX + sshcrypto_hash_MAX];
    unsigned char x[sshcrypto_hash_MAX + 1];
    unsigned long long smlen;
    long long xlen = 0, r, i;
    struct sshcrypto_key *k = 0;

    log_init(2, "tinysshd-sign", 0, 0);
    if (argc != 3) bug_inval();
    for (i = 0; sshcrypto_keys[i].name; ++i)
        if (str_equaln(argv[2], str_len(argv[2]), sshcrypto_keys[i].name))
            k = &sshcrypto_keys[i];
    if (!k) {
        log_w2("sign: no such key: ", argv[2]);
        global_die(111);
    }

    /* the hash: everything on 0, to its end */
    for (;;) {
        r = read(0, x + xlen, sizeof x - xlen);
        if (r == -1 && errno == EINTR) continue;
        if (r <= 0) break;
        xlen += r;
        if (xlen == sizeof x) break;
    }
    if (r == -1 || xlen < 1 || xlen > sshcrypto_hash_MAX) {
        log_w1("sign: no hash, or one too long, from the parent process");
        global_die(111);
    }

    /* signing starts here */
    if (chdir(argv[1]) == -1) {
        log_w2("sign: unable to change directory to ", argv[1]);
        global_die(111);
    }
    if (load(k->sign_secretkeyfilename, sk, k->sign_secretkeybytes) == -1) {
        log_w4("sign: unable to load secret-key from file ", argv[1], "/",
               k->sign_secretkeyfilename);
        purge(sk, sizeof sk);
        global_die(111);
    }
    if (k->sign(sm, &smlen, x, xlen, sk) != 0) {
        log_w4("sign: unable to sign using secret-key from file ", argv[1],
               "/", k->sign_secretkeyfilename);
        purge(sk, sizeof sk);
        global_die(111);
    }
    purge(sk, sizeof sk);
    if (writeall(1, sm, k->sign_bytes) == -1) {
        log_w1("sign: unable to write signature to parent process");
        global_die(111);
    }
    /* signing ends here */

    purge(sm, sizeof sm);
    purge(x, sizeof x);
    global_die(0);
    return 111;
}
