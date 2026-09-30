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

/* time ------------------------------------------------------------------------ */

#define NS_PER_US 1000ull
#define NS_PER_MS 1000000ull
#define NS_PER_S  1000000000ull

/* Nanoseconds since boot: the clock deadlines (jam_object_wait_one,
 * jam_port_wait, jam_nanosleep, ...) are measured on. */
static inline uint64_t now(void)
{
    return (uint64_t)jam_clock_get();
}

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

/* memory ------------------------------------------------------------------------ */

#define PAGE_SIZE 4096ull   /* mappings, protections and VMO sizes are whole pages */

/* heap: a VMO mapped into our address space on first use ---------------------- */

#define HEAP_SIZE (16u << 20)   /* address space reserved; pages commit on touch */

void *malloc(size_t n);         /* 16-byte aligned; NULL when out of memory */
void *calloc(size_t n, size_t size);
void  free(void *p);

/* bootfs: read files from the SR_BOOTFS VMO ----------------------------------- */

struct bootfs_view {
    const uint8_t *base;   /* the whole image, mapped read-only */
    uint64_t       size;   /* its size in bytes */
    uint32_t       count;  /* files in its entry table */
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
    handle_t h;      /* the handle (moved) */
};

struct spawn_args {
    const char                *path;     /* in bootfs, e.g. "bin/utest" */
    const char                *name;     /* process name; NULL: the path's last part */
    int                        argc;     /* entries in argv */
    const char *const         *argv;     /* argv[0] is the program's name as it sees it */
    handle_t                   job;      /* needs JOB_RIGHTS_OWN */
    const struct spawn_handle *extra;    /* moved into the startup message */
    unsigned                   nextra;   /* entries in extra */
    /* NULL, or nextra entries: what the child's copy of extra[i] gets (a
     * subset of its rights, e.g. without RIGHT_TRANSFER); 0 or
     * RIGHT_SAME: the same as extra[i].h. */
    const rights_t            *extra_rights;
    /* NULL, or a NULL-terminated list of "KEY=value" strings: the child's
     * environment (environ). */
    const char *const         *envp;
};

/* Load a program from bootfs into a new process and start it. Its startup
 * message has argv, SELF_PROCESS, SELF_VMAR, SELF_THREAD, JOB (a duplicate
 * of a->job with JOB_RIGHTS_OWN: the child can't change its own limits),
 * BOOTFS (a duplicate of ours) and the extras, which are
 * consumed whatever happens. *proc gets the process handle. */
status_t spawn(const struct spawn_args *a, handle_t *proc);
/* Drivers: the startup role that hands a driver process a handle with
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

/* files ---------------------------------------------------------------------------
 * The namespace (M8): every program gets mount points (SR_NS, set up by
 * whoever starts it; init gives /boot, /esp and /data) and these calls
 * resolve a path to the longest matching mount, then talk to that mount's
 * `fs` service (abi/idl/fs.idl, file.idl). Paths are absolute, at most
 * FS_PATH_MAX - 1 bytes. Errors are the servers' ERR_* (fs.idl lists them);
 * a path under no mount is ERR_NOT_FOUND. */

#define FS_PATH_MAX 256
#define FS_READ     1u    /* fs.open flags (fs.idl) */
#define FS_WRITE    2u
#define FS_CREATE   4u    /* create it if missing */
#define FS_TRUNCATE 8u    /* empty it on open */
#define FS_APPEND   16u   /* every write goes to the end */

struct jfile {
    handle_t ch;          /* the file protocol channel */
    handle_t buf_vmo;     /* the transfer buffer */
    uint8_t *buf;         /* ... mapped */
    uint32_t buf_size;    /* its size in bytes */
    uint64_t size;        /* the file's size at open (file_stat for now) */
};

struct fs_entry {
    char     name[FS_PATH_MAX];   /* NUL-terminated */
    bool     is_dir;              /* a directory */
    uint64_t size;                /* bytes; 0 for a directory */
};

status_t file_open(const char *path, uint32_t flags, struct jfile *out);
/* Read / write up to n bytes at offset; *done gets how many (a read short
 * of n only at the end of the file). Split into buffer-sized calls. */
status_t file_read(struct jfile *f, uint64_t offset, void *dst, size_t n, size_t *done);
status_t file_write(struct jfile *f, uint64_t offset, const void *src, size_t n, size_t *done);
status_t file_sync(struct jfile *f);
void     file_close(struct jfile *f);
/* A path's size, whether it is a directory, and its modification time
 * (Unix seconds; 0 if unknown). Any of the outputs may be NULL. */
status_t fs_stat(const char *path, uint64_t *size, bool *is_dir, uint64_t *mtime);
/* Entry `index` of directory `path` ("." and ".." left out); past the last:
 * ERR_NOT_FOUND. */
status_t fs_readdir(const char *path, uint32_t index, struct fs_entry *out);
status_t fs_mkdir(const char *path);
status_t fs_unlink(const char *path);
status_t fs_rename(const char *from, const char *to);   /* both on one mount */
status_t fs_sync(const char *path);                      /* the mount holding path */

/* devices ------------------------------------------------------------------------ */

/* The config-space offset of PCI capability `id` of the function dev (a
 * RES_PCI_DEV handle with RIGHT_READ), or 0 if it has none. The list is
 * walked at most 48 steps, inside the standard config space. */
uint32_t pci_find_cap(handle_t dev, uint32_t id);

/* the CPU ------------------------------------------------------------------------ */

/* CPUID leaf `leaf`, subleaf `sub`: r = eax, ebx, ecx, edx. */
static inline void cpu_cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                     : "a"(leaf), "c"(sub));
}

/* XCR0: the register state the kernel saves for us (bit 1 SSE, bit 2 AVX).
 * Only where CPUID.1:ECX.OSXSAVE (bit 27) is set. */
static inline uint64_t cpu_xcr0(void)
{
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (uint64_t)hi << 32 | lo;
}

/* The TSC, fenced on both sides so nothing moves across the read. */
static inline uint64_t cpu_tsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(lo), "=d"(hi) :: "memory");
    return (uint64_t)hi << 32 | lo;
}
