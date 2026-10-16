/* Checked copies to and from the caller's memory (ucopy.S, ucopy.c):
 * Linux's EFAULT for a pointer the caller can't read or write, where a
 * plain load or store in a handler would fault and kill the process
 * (docs/CR-PLAN.md, "CBF1 notes").
 *
 * Each routine is a few instructions in ucopy.S, and each of their loads
 * and stores of the caller's memory has a line in its exception table: a
 * fault there (a page fault, or a general protection fault for an
 * address that isn't one) goes on at the routine's fixup, which returns
 * -EFAULT to the routine's caller. sigfault.c asks uc_fault first, in the
 * kernel's window, before anything of signals: so it works with no
 * handler installed and never touches signal state. The process's
 * exception entry is set from the start for this (SIG_ENTRY_COPY, set by
 * sig_start_state). A fault anywhere else in the layer is still SIGSEGV.
 *
 * The rule for handlers: a pointer a program gives a call is read and
 * written only through these. A struct is built in a local and copied
 * out once (Linux's way: a struct half written is never the answer), and
 * read into a local once before it is used. A NULL pointer faults like
 * any other: a handler for which NULL means "not given" says so before it
 * copies. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* n bytes from the caller's src to our dst (uc_get), or from our src to
 * the caller's dst (uc_put): 0, or -EFAULT (some bytes may have been
 * copied). */
long uc_get(void *dst, const void *src, size_t n);
long uc_put(void *dst, const void *src, size_t n);
/* An int at the caller's p (an argument as musl passes it): the answer
 * of many ioctls and calls. 0, or -EFAULT. */
static inline long uc_put_int(long p, int v)
{
    return uc_put((void *)(uintptr_t)p, &v, sizeof(v));
}
/* n zero bytes at the caller's dst: 0, or -EFAULT. */
long uc_zero(void *dst, size_t n);
/* The length of the caller's string s, max if no NUL is in its first max
 * bytes, or -EFAULT. */
long uc_strnlen(const char *s, size_t max);
/* Can the caller's [p, p + n) be read (uc_readable) or written
 * (uc_writable) now: 0, or -EFAULT. Each page is touched once, a write
 * as a locked add of 0 (it changes nothing a racing writer would lose):
 * for a buffer a kernel call reads or writes itself, and for a call that
 * only says whether it could (an empty range is 0). */
long uc_readable(const void *p, size_t n);
long uc_writable(void *p, size_t n);

/* r is the result of a kernel call given the caller's buffer [p, p + n):
 * if it is -EINVAL (lx_err's answer for ERR_INVALID_ARGS, which is the
 * kernel's for a bad pointer too), -EFAULT when the buffer can't be read
 * (or written: write), else r. The buffer is looked at only then. */
long uc_or_fault(long r, const void *p, size_t n, bool write);

/* The fault path's check (sigfault.c, in the kernel's window): if frame
 * f is a fault at one of the table's instructions, f now goes on at its
 * fixup, and true. */
struct exc_frame;
bool uc_fault(struct exc_frame *f);
