/* <execinfo.h>: glibc's stack traces, which musl does not have (a new
 * header, not an override: Jam OS's libc implements them in
 * user/posix/glibc/execinfo.c, docs/CR-PLAN.md "BT notes").
 *
 * backtrace() stores the return addresses of the calling thread's frames,
 * the innermost first (array[0] is in the function that called
 * backtrace), and returns how many; it takes no lock and calls nothing,
 * so a signal handler and a crash path may use it. The trace ends where a
 * frame has no unwind table. backtrace_symbols() makes one string a frame
 * in a single malloc'd block (free() it); backtrace_symbols_fd() writes
 * the lines to a descriptor with no allocation. A program keeps no symbol
 * table in memory, so a line names the program, not a function, as glibc
 * does for a program without .dynsym: "./prog() [0x401136]", and
 * "[0x7ffc1234]" for an address outside the program. */
#ifndef _EXECINFO_H
#define _EXECINFO_H

#ifdef __cplusplus
extern "C" {
#endif

int backtrace(void **, int);
char **backtrace_symbols(void *const *, int);
void backtrace_symbols_fd(void *const *, int, int);

#ifdef __cplusplus
}
#endif

#endif
