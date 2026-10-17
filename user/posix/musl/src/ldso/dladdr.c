/* musl's static dladdr on Jam OS: overrides upstream's src/ldso/dladdr.c
 * (user/posix/musl/OVERRIDES). Upstream's finds nothing, as it does in any
 * static program. A static program does have one module, its own image,
 * and programs that profile or print stack traces (Chromium's ModuleCache,
 * base::debug::StackTrace) ask which file an address is in: the layer
 * (user/posix/sys/procfs.c) answers for the image, with its base and the
 * path /proc/self/exe names. There are no symbols to give: dli_sname and
 * dli_saddr stay null. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include "glue.h"

static int stub_dladdr(const void *addr, Dl_info *info)
{
	const char *file;
	void *base;
	if (!__jam_dladdr(addr, &file, &base))
		return 0;
	info->dli_fname = file;
	info->dli_fbase = base;
	info->dli_sname = 0;
	info->dli_saddr = 0;
	return 1;
}

weak_alias(stub_dladdr, dladdr);
