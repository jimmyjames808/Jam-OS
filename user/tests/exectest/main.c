/* exectest: the narrow one of exectest.h's pair. */
#include <os.h>
#include <wants.h>
#include "exectest.h"

/* deputy: utest starts it through /svc/exec, a caller init can't vouch
 * for (init's exec.c, "who is raised"); a test, never installed. */
JAM_WANTS("svc exec\n"
          "mount /data r\n"
          "deputy\n");

int main(int argc, char **argv)
{
    return exectest_main(argc, argv);
}
