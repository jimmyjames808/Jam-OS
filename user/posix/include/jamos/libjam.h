/* libjam's calls (<os.h>: handles, spawn, the namespace, the startup
 * message's roles) for code built on musl: the POSIX layer itself
 * (user/posix/) and a POSIX program that also speaks Jam OS (posixtest).
 * The tree's own find <os.h> in user/include; a port finds it, and the
 * few other libos headers a port uses (<audio.h>), in the sysroot's
 * include/jamos/libos (-isystem <sysroot>/include/jamos/libos). In C++
 * they go inside extern "C", with `restrict` defined as __restrict, and
 * after the program's own headers: libos's names are macros (OK, ERR_*).
 *
 * <os.h> is written for libos, and two of its names meet musl's here:
 * its PAGE_SIZE (4096ull) and musl's <limits.h> one (PAGESIZE, 4096) are
 * the same page but different tokens, so a file including both would
 * redefine the macro. This header includes <limits.h> first, then <os.h>
 * with musl's PAGE_SIZE put aside, and leaves musl's in force: code here
 * writes PAGE_SIZE for musl's, and its value is the same either way.
 *
 * Never include <sys/syscall.h> with this: Jam OS's call numbers
 * (<jam/syscall_nums.h>, through <os.h>) and Linux's share a few names
 * (SYS_timer_create, SYS_futex_wait, ...). Only the call layer's table
 * (user/posix/sys/calls.c) uses Linux's numbers. */
#pragma once

#include <limits.h>

#pragma push_macro("PAGE_SIZE")
#undef PAGE_SIZE
#include <os.h>
#undef PAGE_SIZE
#pragma pop_macro("PAGE_SIZE")
