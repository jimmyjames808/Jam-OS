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
char  *strrchr(const char *s, int c);                 /* the last c, or NULL */
char  *strstr(const char *hay, const char *needle);   /* the first needle, or NULL */

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
    /* The program: a bootfs name ("bin/utest"), or an absolute path read
     * through our namespace ("/data/x"; see files below). With `vmo` set,
     * the program is bytes [offset, offset + size) of it instead, and path
     * only names it. Code runs only from a VMO whose handle has RIGHT_EXEC,
     * which only the bootfs image's has: a program from anywhere else is
     * refused (ERR_ACCESS_DENIED) until the kernel can make a VMO
     * executable. */
    const char                *path;
    handle_t                   vmo;      /* 0: path says where the program is */
    uint64_t                   offset;   /* of the program in vmo, page-aligned */
    uint64_t                   size;     /* its bytes */
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
    /* Its namespace (SR_NS): NULL gives it none; else these mount points of
     * ours, NULL-terminated (NS_ALL: every one), sent as its first ns
     * message right after the start. */
    const char *const         *ns;
    /* NULL: our end of its SR_NS channel is closed once that message is
     * sent (its namespace never changes). Else our end goes here after a
     * successful spawn, for ns_update (or ns_send) later. */
    handle_t                  *ns_out;
    /* With ns_out: NULL, or where a duplicate of the child's own end goes
     * (RIGHT_READ only), for ns_update to take back what it hasn't read. */
    handle_t                  *ns_back_out;
};

/* Load a program (spawn_args.path) into a new process and start it. Its
 * startup message has argv, SELF_PROCESS, SELF_VMAR, SELF_THREAD, JOB (a
 * duplicate of a->job with JOB_RIGHTS_OWN: the child can't change its own
 * limits), BOOTFS (a duplicate of ours), NS (when a->ns is set) and the
 * extras, which are consumed whatever happens. *proc gets the process
 * handle. ERR_OUT_OF_RANGE: too many extras (STARTUP_MAX_HANDLES - 6 fit
 * next to a namespace, one more without). */
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
 * The namespace: the mount points a program was given, each with its `fs`
 * channel (abi/idl/fs.idl, file.idl), and the services (below: "/svc").
 * A mount point is "/" and one name
 * ("/boot", "/data"). The calls below take absolute paths of at most
 * FS_PATH_MAX - 1 bytes (longer, or not starting with '/':
 * ERR_INVALID_ARGS), find the mount by the path's first name and send the
 * rest to its `fs` service. Inside a mount "." and ".." are resolved here,
 * and ".." stops at the mount's root: "/data/../boot/x" is /data/boot/x,
 * never /boot/x. "/" itself is a directory that lists the mount points
 * (and "svc" when there are services).
 * Errors: a path under no mount is ERR_NOT_FOUND; a mount whose service
 * died gives ERR_PEER_CLOSED; a call the service doesn't answer within
 * FS_CALL_TIMEOUT is ERR_TIMED_OUT; the rest are the service's ERR_*
 * (fs.idl lists them).
 *
 * SR_NS, the encoding: a channel end. Whoever started the program holds
 * the other end and writes struct ns_msg messages on it; an NS_MOUNT's or
 * NS_SET's handles are the `fs` channels and the services' channels, one
 * per path, in order (a path "/svc/<name>" is a service). The
 * first message (an NS_MOUNT, possibly of nothing) comes right after the
 * start; libos waits for it (up to NS_FIRST_WAIT) before its first lookup.
 * Later ones change the running program's namespace: NS_MOUNT adds mounts,
 * or replaces those with the same path; NS_UNMOUNT (no handles) removes
 * them; NS_SET makes the starter's mounts and services exactly the ones
 * it lists (the starter's others go; the program's own, from ns_mount and
 * ns_svc_set, stay unless one of the same path replaces them). Files already open stay open either
 * way: each has its own channel. A starter that closes its end leaves the
 * namespace as it is. A malformed message is dropped and its handles
 * closed.
 *
 * libos reads SR_NS only when it next needs the namespace (a lookup), so
 * a program that never looks up a path again never reads it. A starter
 * that follows its own mounts for a running program (init, for the shell
 * and logd) therefore keeps, beside its end, a duplicate of the program's
 * end (spawn_args.ns_back_out) and sends with ns_update: an NS_SET of the
 * whole namespace, after taking back whatever the program hasn't read
 * yet. However many changes there are, the program's end then holds at
 * most one message (a mount that came and went before it looked leaves
 * nothing), its next lookup sees the starter's namespace as of the last
 * change, and the starter never finds the queue full.
 *
 * The namespace may be used from several threads at once. */

#define FS_PATH_MAX 256
#define FS_READ     1u    /* fs.open flags (fs.idl) */
#define FS_WRITE    2u
#define FS_CREATE   4u    /* create it if missing */
#define FS_TRUNCATE 8u    /* empty it on open */
#define FS_APPEND   16u   /* every write goes to the end */
#define FS_FLAGS    31u   /* all of them: any other bit is ERR_INVALID_ARGS */
#define FS_CALL_TIMEOUT (60 * NS_PER_S)

#define NS_NAME_MAX    16  /* an entry's path with its NUL: "/data", "/svc/devmgr-ctl" */
#define NS_MAX_MOUNTS  8   /* mounts in one namespace */
#define NS_MAX_SVCS    16  /* services in one namespace */
#define NS_MAX_ENTRIES (NS_MAX_MOUNTS + NS_MAX_SVCS)   /* entries in one ns_msg */
#define NS_FIRST_WAIT  (5 * NS_PER_S)
#define NS_MOUNT       1u
#define NS_UNMOUNT     2u
#define NS_SET         3u

struct ns_msg {
    uint32_t txid;                              /* 0: not a call */
    uint32_t kind;                              /* NS_MOUNT, NS_UNMOUNT or NS_SET */
    uint32_t count;                             /* paths used, at most NS_MAX_ENTRIES */
    uint32_t connect;                           /* bit i: service i hands out channels */
    char     path[NS_MAX_ENTRIES][NS_NAME_MAX]; /* mount points and /svc/<name>, NUL-terminated */
};
/* An ns_msg is sent only as long as the paths it uses. */
#define NS_MSG_SIZE(count) (16u + (count) * NS_NAME_MAX)

/* "Everything we have, as we have it": every mount (unrestricted) and
 * every service, for spawn_args.ns and ns_send. */
extern const char *const NS_ALL[];

/* Mount the `fs` channel fs (consumed, whatever happens) at path in our
 * own namespace, replacing what was mounted there (its channel is closed).
 * ERR_INVALID_ARGS: path isn't "/name" (a name of 1 to NS_NAME_MAX - 2
 * bytes without '/', not ".", ".." or "svc"); ERR_NO_RESOURCES:
 * NS_MAX_MOUNTS mounts already. */
status_t ns_mount(const char *path, handle_t fs);
/* ERR_NOT_FOUND if nothing is mounted there. */
status_t ns_unmount(const char *path);
/* Our i-th mount point (in the order they were mounted) into out; false
 * past the last. Services are not mounts: ns_svc_at lists them. */
bool     ns_mount_at(unsigned i, char out[NS_NAME_MAX]);
/* A duplicate of the `fs` channel mounted at path ("/data"), for calls of
 * one's own (the caller closes it). ERR_NOT_FOUND: no such mount. */
status_t ns_channel(const char *path, handle_t *out);
/* On `to` (our end of another program's SR_NS channel): an NS_MOUNT of
 * what `grants` names (NULL-terminated; see "grants" below), each mount
 * as a duplicate of our channel or a view made for it, each service as a
 * duplicate of ours. What we don't have is left out, and so is a mount
 * whose view its service won't make (fail closed). */
status_t ns_send(handle_t to, const char *const *grants);
/* The same for one channel of the caller's (consumed, whatever happens)
 * at path (a mount point or /svc/<name>); fs HANDLE_INVALID sends an
 * NS_UNMOUNT of path instead. */
status_t ns_send_one(handle_t to, const char *path, handle_t fs);
/* Keep a running program's namespace in step with ours: on `to` an
 * NS_SET of what `grants` names (as ns_send), after taking back
 * through `back` (a duplicate of the program's end: spawn_args.ns_back_out;
 * HANDLE_INVALID: none) every message the program hasn't read yet, their
 * handles closed. With `back`, the program's end holds at most one message
 * from us however often this is called, and it is our namespace as of the
 * last call. */
status_t ns_update(handle_t to, handle_t back, const char *const *grants);

/* services --------------------------------------------------------------------------
 * A service is a name under /svc in the namespace, given like a mount
 * (SR_NS, as a path "/svc/<name>") with a channel to the service. Names
 * are 1 to SVC_NAME_MAX bytes of [a-z0-9-]. svc_open gives the caller a
 * channel of its own when the service hands them out (the entry says so:
 * the svc protocol's connect, abi/idl/svc.idl), else a duplicate of the
 * shared one. "/svc" lists the names (fs_readdir), fs_stat of one says
 * it is neither a file nor a directory (is_dir false, size 0), and
 * file_open of one is ERR_WRONG_TYPE. Errors: ERR_NOT_FOUND, the name
 * isn't in our namespace (not granted); ERR_INVALID_ARGS, not a name. */

#define SVC_NAME_MAX   10            /* bytes of a service's name */
/* The services init publishes (tools/checkwants.py reads this list). */
#define SVC_AUDIO      "audio"       /* the mixer: open a sound stream (abi/idl/audio.idl) */
#define SVC_AUDIOCTL   "audioctl"    /* the mixer's volumes (audioctl.idl) */
#define SVC_MUSIC      "music"       /* the music player, a channel per opener (music.idl) */
#define SVC_DEVMGR     "devmgr"      /* devmgr's queries (<devmgr.h>) */
#define SVC_DEVMGR_CTL "devmgr-ctl"  /* devmgr's control channel: tests only */
#define SVC_INIT       "init"        /* init's control channel (initctl.idl): tests only */
#define SVC_LOGD       "logd"        /* logd's control channel (logctl.idl) */

/* A channel to service `name` for the caller, who closes it. */
status_t svc_open(const char *name, handle_t *out);
/* A channel to service `name` that libos keeps (don't close it), opened
 * on first use and opened again when its peer has gone (the service
 * restarted), so a caller that asks again after ERR_PEER_CLOSED gets the
 * new one. HANDLE_INVALID: not in our namespace. */
handle_t svc_get(const char *name);
/* The i-th service of our namespace (in the order given) into out (its
 * name, NUL-terminated); false past the last. */
bool     ns_svc_at(unsigned i, char out[SVC_NAME_MAX + 1]);
/* For a service that hands out a channel per opener: take one request
 * off ch and answer it, svc.connect by calling connect(ctx, &out) (it
 * makes the channel and keeps the server end), anything else by
 * dispatch(ctx, ...), a wrapper of the protocol's generated
 * <proto>_dispatch. Returns as a generated <proto>_serve_one does: OK once
 * a message was handled, else the read's status (ERR_SHOULD_WAIT: nothing
 * queued; ERR_PEER_CLOSED: every client is gone). One thread at a time. */
typedef uint32_t (*svc_dispatch_fn)(void *ctx, const void *req, uint32_t n, void *rep,
                                    handle_t *rhs, uint32_t *rhn);
status_t svc_serve_request(handle_t ch, svc_dispatch_fn dispatch,
                           status_t (*connect)(void *ctx, handle_t *out), void *ctx);
/* Publish service `name` in our own namespace (h consumed, whatever
 * happens), replacing one of the same name; connect: it hands out a
 * channel per opener. ERR_NO_RESOURCES: NS_MAX_SVCS already. */
status_t ns_svc_set(const char *name, handle_t h, bool connect);
/* ERR_NOT_FOUND if there is no such service. */
status_t ns_svc_remove(const char *name);

/* grants -------------------------------------------------------------------------------
 * What a starter gives a program (spawn_args.ns, ns_send, ns_update): a
 * NULL-terminated list of strings, each naming part of the starter's own
 * namespace:
 *   "*"              everything, as the starter has it (NS_ALL)
 *   "/svc/<name>"    that service
 *   "/data"          that mount, as the starter has it
 *   "/data:r"        a read-only view of it (fs.view, <fsview.h>)
 *   "/data:w"        a view that may write, but not the top-level `etc`
 *   "/usb*"          (with or without :r, :w) every mount whose point
 *                    starts with "/usb"
 *   "*:r", "*:w"     every mount, as views (no services)
 * A view is made with a call to the mount's service at the time of the
 * send (each waits at most NS_VIEW_WAIT). */
#define NS_VIEW_WAIT (2 * NS_PER_S)

/* For fs services: a path inside a mount (fs.idl's 256-byte field, which
 * must hold a NUL) as the names it walks, joined by '/' with none leading
 * or trailing ("" for the mount's root): "/a/./b/../c" is "a/c", and ".."
 * at the root stays there. out has FS_PATH_MAX bytes. ERR_INVALID_ARGS: no
 * NUL in the field. */
status_t fs_path_clean(const uint8_t path[FS_PATH_MAX], char out[FS_PATH_MAX]);

struct jfile {
    handle_t ch;          /* the file protocol channel */
    handle_t buf_vmo;     /* the transfer buffer */
    uint8_t *buf;         /* ... mapped (read-only unless opened FS_WRITE) */
    uint32_t buf_size;    /* its size in bytes */
    uint32_t flags;       /* the FS_* flags it was opened with */
    uint64_t size;        /* the file's size at open (file_stat for now) */
};

struct fs_entry {
    char     name[FS_PATH_MAX];   /* NUL-terminated */
    bool     is_dir;              /* a directory */
    uint64_t size;                /* bytes; 0 for a directory */
};

/* Open a file (FS_* flags), not a directory. *out is ours until file_close. */
status_t file_open(const char *path, uint32_t flags, struct jfile *out);
/* Read / write up to n bytes at offset; *done gets how many (a read short
 * of n only at the end of the file). Split into buffer-sized calls. A file
 * not opened FS_READ / FS_WRITE: ERR_ACCESS_DENIED; a closed one:
 * ERR_BAD_STATE. */
status_t file_read(struct jfile *f, uint64_t offset, void *dst, size_t n, size_t *done);
status_t file_write(struct jfile *f, uint64_t offset, const void *src, size_t n, size_t *done);
status_t file_sync(struct jfile *f);
/* The file's size now and its modification time (either may be NULL). */
status_t file_stat(struct jfile *f, uint64_t *size, uint64_t *mtime);
/* Cut the file to size bytes, or grow it with zeros. */
status_t file_truncate(struct jfile *f, uint64_t size);
/* Close it and unmap its buffer; *f is zeroed. */
void     file_close(struct jfile *f);
/* Hand an open file to another program: its `file` channel and buffer VMO
 * into *ch and *buf (ours no more: send them), our mapping of the buffer
 * gone; *f is zeroed. */
void     file_give(struct jfile *f, handle_t *ch, handle_t *buf);
/* Take a file another program gave us (file_give's two handles, consumed
 * whatever happens), opened with `flags` (FS_*): map its buffer and ask
 * its size. *out is ours until file_close. */
status_t file_adopt(handle_t ch, handle_t buf, uint32_t flags, struct jfile *out);
/* A path's size, whether it is a directory, and its modification time
 * (Unix seconds; 0 if unknown). Any of the outputs may be NULL. */
status_t fs_stat(const char *path, uint64_t *size, bool *is_dir, uint64_t *mtime);
/* Entry `index` of directory `path` ("." and ".." left out); past the last:
 * ERR_NOT_FOUND. */
status_t fs_readdir(const char *path, uint32_t index, struct fs_entry *out);
status_t fs_mkdir(const char *path);
status_t fs_unlink(const char *path);
/* Both on one mount, else ERR_NOT_SUPPORTED. */
status_t fs_rename(const char *from, const char *to);
/* Everything written to the mount holding path is on its medium. */
status_t fs_sync(const char *path);
/* The same, giving up at deadline_ns (ERR_TIMED_OUT). */
status_t fs_sync_by(const char *path, uint64_t deadline_ns);
/* The mount holding path: its size and free space in bytes, whether it is
 * read-only, and its volume label (NUL-terminated). Outputs may be NULL. */
status_t fs_statfs(const char *path, uint64_t *total, uint64_t *free_bytes, bool *read_only,
                   char label[17]);
/* The whole file at path in a new VMO (whole pages, the rest zero) and its
 * size in bytes. ERR_OUT_OF_RANGE: bigger than max_size. */
status_t file_read_vmo(const char *path, uint64_t max_size, handle_t *vmo, uint64_t *size);

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
