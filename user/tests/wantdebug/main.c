/* wantdebug: a program whose list asks for `right debug` (the kernel's
 * debug commands, which can panic the machine) and nothing else. It only
 * says whether it was given that power. The shell's allow test
 * (tools/shell-tests/allow.txt) copies it to /data, where `allow` and `run`
 * must refuse it: no program from /data may have that power. From /boot
 * the shell gives it as asked.
 *
 * Startup handles: SR_RESOURCE, the root resource with RIGHT_ROOT_DEBUG
 * when the shell gave it. Exits 0 when it holds the power, 1 when not. */
#include <os.h>
#include <wants.h>

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("right debug\n");

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    handle_t root = startup_handle(SR_RESOURCE), h;
    bool held = root && jam_handle_duplicate(root, RIGHT_ROOT_DEBUG, &h) == OK;
    if (held)
        jam_handle_close(h);
    printf("wantdebug: right debug %s\n", held ? "held" : "not held");
    return held ? 0 : 1;
}
