/* musl's static dlopen on Jam OS: overrides upstream's src/ldso/dlopen.c
 * (user/posix/musl/OVERRIDES). Upstream's fails with "Dynamic loading not
 * supported", as it does in any static program, and so does this; it also
 * tells the ENOSYS log (user/posix/sys/enosys.c) which library was asked
 * for, so a Chromium run that tries to load one (a GPU driver, a codec)
 * shows it in the first run's log. */
#include <dlfcn.h>
#include "dynlink.h"
#include "glue.h"

static void *stub_dlopen(const char *file, int mode)
{
	__jam_enosys_dlopen(file);
	__dl_seterr("Dynamic loading not supported");
	return 0;
}

weak_alias(stub_dlopen, dlopen);
