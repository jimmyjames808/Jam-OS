/* posixtest's raw section: the calls Chromium and its libraries make by
 * their Linux numbers (syscall() or musl's thin wrappers), each answered
 * or failing the way Chromium tolerates, and the ENOSYS log for the ones
 * that are not there (rawlog.c). The log is checked first; the calls'
 * checks follow, one function per call. */
#include <stdio.h>
#include "pt.h"

void pt_raw(char **argv)
{
    (void)argv;
    pt_raw_log();
}
