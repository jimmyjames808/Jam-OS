/* wantsession: a program whose list asks for `svc session` (init's
 * sessions: a whole shell) and nothing else. It only says whether it was
 * given /svc/session. The sessions test (tools/shell-tests/sessions.txt)
 * runs it from /boot, where the shell gives it as asked, and copies it to
 * /data, where `allow` must refuse it: no program from /data may have a
 * shell of its own.
 *
 * Exits 0 when it can open /svc/session, 1 when not. */
#include <os.h>
#include <wants.h>

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("svc session\n");

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    handle_t ch;
    status_t st = svc_open(SVC_SESSION, &ch);
    if (st == OK)
        jam_handle_close(ch);
    printf("wantsession: /svc/session %s\n", st == OK ? "given" : status_str(st));
    return st == OK ? 0 : 1;
}
