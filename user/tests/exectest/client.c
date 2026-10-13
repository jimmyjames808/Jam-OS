/* exectest_start (exectest.h): a native program's start of an approved
 * /data program through init's /svc/exec, as libc's posix_spawn does it
 * (user/posix/sys/spawn.c), without a hand-over. utest links it too. */
#include <idl/exec.h>
#include <os.h>
#include "exectest.h"

status_t exectest_start(const char *path, int argc, const char *const *argv, handle_t *proc,
                        handle_t *job, handle_t *sig, int *step)
{
    static uint8_t strings[EXEC_STRINGS_BYTES];   /* one start at a time per program */
    handle_t svc = HANDLE_INVALID, file, buf, mine = HANDLE_INVALID, theirs, prog = HANDLE_INVALID;
    handle_t ours_job = HANDLE_INVALID, sig_theirs = HANDLE_INVALID;
    struct jfile f;
    uint8_t posix;
    char wants[1024];   /* exec.open's list as shown (unread here) */
    uint32_t wl, len = 0;
    *step = 1;
    status_t st = svc_open(SVC_EXEC, &svc);
    if (st == OK) {
        *step = 2;
        st = file_open(path, FS_READ, &f);
    }
    if (st == OK) {
        file_give(&f, &file, &buf);
        jam_handle_close(buf);
        *step = 5;
        st = jam_channel_create(&mine, &theirs);
        if (st != OK)
            jam_handle_close(file);
    }
    if (st == OK) {
        *step = 3;
        st = exec_open(svc, path, (uint32_t)strlen(path), file, theirs, &prog, &posix, wants, &wl);
    }
    for (int i = 0; st == OK && i < argc; i++) {
        size_t l = strlen(argv[i]) + 1;
        *step = 5;
        if (l > sizeof(strings) - len)
            st = ERR_OUT_OF_RANGE;
        else
            memcpy(strings + len, argv[i], l);
        len += (uint32_t)l;
    }
    if (st == OK)
        st = jam_handle_duplicate(startup_handle(SR_JOB), RIGHT_SAME, &ours_job);
    if (st == OK)
        st = jam_channel_create(sig, &sig_theirs);
    if (st == OK) {
        *step = 4;
        st = exec_prog_start(prog, (uint16_t)argc, strings, len, ours_job, sig_theirs, proc, job);
        ours_job = sig_theirs = HANDLE_INVALID;   /* init's now */
        if (st != OK)
            jam_handle_close(*sig);
    }
    handle_t left[] = { svc, mine, prog, ours_job, sig_theirs };
    for (unsigned i = 0; i < sizeof(left) / sizeof(left[0]); i++)
        if (left[i])
            jam_handle_close(left[i]);
    return st;
}
