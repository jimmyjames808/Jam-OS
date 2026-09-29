/* libos: the runtime every Jam OS program links against (user/lib/).
 *
 * crt0 takes the startup channel handle from rdi, libos reads the startup
 * message (argv, environment, handles by role; <jam/startup.h>), then calls
 * main(argc, argv) and passes its return value to process_exit.
 *
 * User code sees only a few kernel headers (<jam/abi.h>, <jam/status.h>,
 * <jam/startup.h>, <jam/syscall_nums.h>, <jam/bootfs.h>): the build copies
 * them into an include directory of their own, so kernel internals are
 * simply not on the include path. */
#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <jam/abi.h>
#include <jam/startup.h>
#include <jam/status.h>
#include <jam_syscalls.h>

int main(int argc, char **argv);

/* startup message ----------------------------------------------------------- */

/* The channel the startup message came on (still open). */
handle_t    startup_channel(void);
/* The first handle with this role (enum startup_role), or HANDLE_INVALID. */
handle_t    startup_handle(uint32_t role);
/* Every handle in the message, in order. */
unsigned    startup_handle_count(void);
handle_t    startup_handle_at(unsigned i, uint32_t *role);
const char *startup_role_name(uint32_t role);
/* "KEY=value" strings, NULL-terminated. */
extern char **environ;
/* OK, or why the startup message could not be used (the program still
 * runs, with argc 0 and no handles). */
status_t    startup_status(void);

/* output: through the debug_write system call ------------------------------- */

int  printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  vprintf(const char *fmt, va_list ap);
int  snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int  vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int  puts(const char *s);   /* adds a newline */
/* The name of an ERR_* code ("ERR_NOT_FOUND"), or "?". */
const char *status_str(status_t s);

/* strings --------------------------------------------------------------------- */

void  *memcpy(void *restrict dst, const void *restrict src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);

/* heap: a VMO mapped into our address space on first use ---------------------- */

#define HEAP_SIZE (16u << 20)   /* address space reserved; pages commit on touch */

void *malloc(size_t n);         /* 16-byte aligned; NULL when out of memory */
void *calloc(size_t n, size_t size);
void  free(void *p);

/* bootfs: read files from the SR_BOOTFS VMO ----------------------------------- */

struct bootfs_view {
    const uint8_t *base;   /* the whole image, mapped read-only */
    uint64_t       size;
    uint32_t       count;
};

/* Map the bootfs image VMO (SR_BOOTFS) read-only and check its header and
 * entry table. */
status_t bootfs_open(handle_t vmo, struct bootfs_view *out);
/* A file's bytes inside the mapping. ERR_NOT_FOUND if there is none. */
status_t bootfs_lookup(const struct bootfs_view *fs, const char *name, const void **data,
                       uint64_t *size);
/* Our SR_BOOTFS image, mapped on first use and kept. */
status_t bootfs_default(const struct bootfs_view **out);

/* processes and threads ------------------------------------------------------ */

struct spawn_handle {
    uint32_t role;   /* enum startup_role, usually SR_USER + n */
    handle_t h;
};

struct spawn_args {
    const char                *path;     /* in bootfs, e.g. "bin/utest" */
    const char                *name;     /* process name; NULL: the path's last part */
    int                        argc;
    const char *const         *argv;
    handle_t                   job;      /* needs JOB_RIGHTS_OWN */
    const struct spawn_handle *extra;    /* moved into the startup message */
    unsigned                   nextra;
    /* NULL, or nextra entries: what the child's copy of extra[i] gets (a
     * subset of its rights, e.g. without RIGHT_TRANSFER, M7); 0 or
     * RIGHT_SAME: the same as extra[i].h. */
    const rights_t            *extra_rights;
};

/* Load a program from bootfs into a new process and start it. Its startup
 * message has argv, SELF_PROCESS, SELF_VMAR, SELF_THREAD, JOB (a duplicate
 * of a->job with JOB_RIGHTS_OWN: the child can't change its own limits),
 * BOOTFS (a duplicate of ours) and the extras, which are
 * consumed whatever happens. *proc gets the process handle. */
status_t spawn(const struct spawn_args *a, handle_t *proc);
/* Drivers (M6): the startup role that hands a driver process a handle with
 * driver role `r` (DR_* in <jam/driver.h>). user/lib/driver_crt.c turns
 * these into the driver's struct driver_start. */
#define SR_DRIVER(r) (SR_USER + (r))
/* Wait up to timeout_ns for proc to die (SIG_TERMINATED), then fill *info
 * (may be NULL). ERR_TIMED_OUT if it is still alive. */
status_t spawn_wait(handle_t proc, uint64_t timeout_ns, struct process_info *info);
/* A new thread in our process running fn(arg) on [stack, stack+size); it
 * exits when fn returns. *out gets its thread handle. */
status_t thread_spawn(const char *name, void (*fn)(void *), void *arg, void *stack,
                      size_t stack_size, handle_t *out);
