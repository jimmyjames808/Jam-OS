/* exectest: the narrow one of exectest.h's pair. */
#include <os.h>
#include <wants.h>
#include "exectest.h"

JAM_WANTS("svc exec\n"
          "mount /data r\n");

int main(int argc, char **argv)
{
    return exectest_main(argc, argv);
}
