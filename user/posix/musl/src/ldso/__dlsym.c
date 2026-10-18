/* musl's static dlsym on Jam OS: overrides upstream's src/ldso/__dlsym.c
 * (user/posix/musl/OVERRIDES). A static program has no other module, so
 * upstream's finds nothing, and neither does this, but for one case: a
 * program that wraps a libc function asks dlsym(RTLD_NEXT, name) for the
 * real one, and gets libc's own for the few names such wrappers use.
 * Chromium's sandbox libc interceptor wraps localtime and its kin; given
 * NULL it logs "dlsym doesn't work" from inside a pthread_once, the log's
 * time stamp calls its localtime_r again, and the thread waits for itself
 * (headless_shell's start-up hang, docs/CR-PLAN.md "TD"). RTLD_DEFAULT
 * still finds nothing: the first definition is the program's own. */
#include <dlfcn.h>
#include <string.h>
#include <time.h>
#include "dynlink.h"

hidden struct tm *__localtime_r(const time_t *restrict, struct tm *restrict);

static struct tm *libc_localtime(const time_t *t)
{
	static struct tm tm;
	return __localtime_r(t, &tm);
}

/* time_t is 64 bits here: the 64 names are the same functions. */
static const struct {
	const char *name;
	void *fn;
} next[] = {
	{ "localtime", (void *)libc_localtime },
	{ "localtime_r", (void *)__localtime_r },
	{ "localtime64", (void *)libc_localtime },
	{ "localtime64_r", (void *)__localtime_r },
};

static void *stub_dlsym(void *restrict p, const char *restrict s, void *restrict ra)
{
	(void)ra;
	if (p == RTLD_NEXT)
		for (size_t i = 0; i < sizeof next / sizeof next[0]; i++)
			if (!strcmp(s, next[i].name))
				return next[i].fn;
	__dl_seterr("Symbol not found: %s", s);
	return 0;
}

weak_alias(stub_dlsym, __dlsym);

#if _REDIR_TIME64
weak_alias(stub_dlsym, __dlsym_redir_time64);
#endif
