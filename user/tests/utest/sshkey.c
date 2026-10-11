/* utest: the SSH server's keys (<sshkey.h>): fingerprints against the ones
 * OpenSSH's ssh-keygen -l prints for the same keys, and which
 * authorized_keys lines count as keys (the ones tinyssh takes). */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <sshkey.h>
#include "utest.h"

bool t_ssh_fingerprint(void)
{
    /* ssh-keygen -lf of "ssh-ed25519 <the blob> test" for these two keys
     * (OpenSSH 9, on the Mac, 2026-10-11). */
    static const struct {
        uint8_t     fill;   /* 0: the bytes 0..31; else every byte this */
        const char *fp;
    } cases[] = {
        { 0, "SHA256:ZkAslGjFiUHdGf/WUL8rQvkib4PTvQatUV0OUQSncCA" },
        { 0xff, "SHA256:48D9sC7DVJMSGpOHcSS3/AKz/VOmo26MOgXADP9uFHk" },
    };
    for (unsigned c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        uint8_t pk[SSH_PK_BYTES];
        for (unsigned i = 0; i < SSH_PK_BYTES; i++)
            pk[i] = cases[c].fill ? cases[c].fill : (uint8_t)i;
        char fp[SSH_FP_MAX];
        memset(fp, 'x', sizeof(fp));
        ssh_fingerprint(pk, fp);
        CHECK(strlen(fp) == SSH_FP_MAX - 1);
        CHECK(!strcmp(fp, cases[c].fp));
    }
    return true;
}

static unsigned counted(const char *text)
{
    return ssh_count_keys(text, strlen(text));
}

bool t_ssh_count_keys(void)
{
    CHECK(counted("") == 0);
    CHECK(counted("ssh-ed25519 AAAAC3Nz me@mac\n") == 1);
    CHECK(counted("ssh-ed25519 AAAAC3Nz") == 1);                     /* no newline at the end */
    CHECK(counted("ssh-ed25519 AAAA a\r\nssh-ed25519 BBBB b\r\n") == 2);   /* written on Windows */
    CHECK(counted("# ssh-ed25519 AAAA\n\nssh-ed25519 AAAA\n") == 1); /* a comment, a blank */
    CHECK(counted("ssh-rsa AAAAB3Nza me\n") == 0);                   /* tinyssh: ed25519 only */
    CHECK(counted("ssh-ed25519\nssh-ed25519 \nssh-ed25519  AAAA\n") == 0);   /* no key */
    CHECK(counted(" ssh-ed25519 AAAA\n") == 0);                      /* tinyssh wants it first */
    CHECK(counted("no-pty ssh-ed25519 AAAA\n") == 0);                /* options: not taken */
    CHECK(counted("ssh-ed25519AAAA\n") == 0);
    return true;
}
