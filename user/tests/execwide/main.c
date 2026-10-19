/* execwide: the wide one of exectest's pair (user/tests/exectest/exectest.h:
 * the same body, linked from exectest's directory). */
#include <os.h>
#include <wants.h>
#include "exectest.h"

/* deputy: utest and exectest (less than this) start it through
 * /svc/exec (init's exec.c, "who is raised"); a test, never installed. */
JAM_WANTS("svc exec\n"
          "mount /data rw\n"
          "mount /boot r\n"
          "right sysinfo\n"
          "deputy\n");

int main(int argc, char **argv)
{
    return exectest_main(argc, argv);
}
