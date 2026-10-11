/*
20181206
Jan Mojzis
Public domain.

Jam OS's override of third_party/tinyssh/tinysshd.c (listed in
user/ports/tinyssh/OVERRIDES): the multi-call wrapper picks bin/sshd's
mode by the name it was started under (argv[0]'s last part):
  tinysshd        one connection on descriptors 0 and 1 (main_tinysshd)
  tinysshd-sign   the signing helper (subprocess_sign.c)
  tinysshd-auth   the key check (subprocess_auth.c)
  anything else   the listener (sshd.c): init starts bin/sshd
What changed, and why: one program for the listener and every helper
(each POSIX program carries musl and the layer, a few hundred KiB, and
the boot image is near its limit), so the two helper modes and the
listener are new; tinysshd-makekey (init makes the host key:
user/services/init/sshd.c), tinysshd-printkey (the shell's `ssh` shows
the fingerprint) and tinysshnoneauthd (a login without a key: never) are
left out.
*/

#include <unistd.h>
#include "helper.h"
#include "main.h"
#include "str.h"

static char *basename(char *str) {

    char *s;
    char *ret = str;

    for (s = str; *s; ++s) {
        if (*s == '/') ret = s + 1;
    }
    return ret;
}

int main(int argc, char **argv) {

    char *x;
    long long xlen;

    if (argc < 1) _exit(100);
    if (!argv[0]) _exit(100);

    x = basename(argv[0]);
    xlen = str_len(x);

    if (str_equaln(x, xlen, "tinysshd")) {
        return main_tinysshd(argc, argv, "tinysshd");
    }
    if (str_equaln(x, xlen, "tinysshd-sign")) {
        return subprocess_sign_main(argc, argv);
    }
    if (str_equaln(x, xlen, "tinysshd-auth")) {
        return subprocess_auth_main(argc, argv);
    }
    return sshd_main(argc, argv);
}
