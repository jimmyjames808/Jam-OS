/* execwide: the wide one of exectest's pair (user/tests/exectest/exectest.h:
 * the same body, linked from exectest's directory). */
#include <os.h>
#include <wants.h>
#include "exectest.h"

JAM_WANTS("svc exec\n"
          "mount /data rw\n"
          "mount /boot r\n"
          "right sysinfo\n");

int main(int argc, char **argv)
{
    return exectest_main(argc, argv);
}
