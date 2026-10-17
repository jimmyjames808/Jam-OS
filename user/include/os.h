/* libos: the runtime every Jam OS program links against (user/lib/).
 *
 * crt0 takes the startup channel handle from rdi, libos reads the startup
 * message (argv, environment, handles by role; <jam/startup.h>), then calls
 * main(argc, argv) and passes its return value to process_exit.
 *
 * libjam.a is libos without its C library: no malloc (heap.c), no string
 * functions (string.c), no printf family (stdio.c), no crt0, environ or
 * call of main (startmain.c). A POSIX program links it under musl, whose
 * functions of those names its code then calls (docs/M13-PLAN.md
 * section 2). Native programs link libos.a, which is libjam and those.
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
#include <roles.h>         /* the startup roles programs give each other */

int main(int argc, char **argv);

/* time ------------------------------------------------------------------------ */

#define NS_PER_US 1000ull
#define NS_PER_MS 1000000ull
#define NS_PER_S  1000000000ull

/* Nanoseconds since boot: the clock deadlines (jam_object_wait_one,
 * jam_port_wait, jam_sleep_until, ...) are measured on. */
static inline uint64_t now(void)
{
    return (uint64_t)jam_clock_get();
}

/* random numbers ---------------------------------------------------------------- */

/* len unpredictable bytes from the kernel's generator (the random_get
 * system call, RANDOM_GET_MAX at a time): for anything an attacker must
 * not guess (DNS query ids and ports, DHCP transaction ids). Never blocks
 * and never fails: a buf that isn't writable memory is the caller's bug,
 * and the program traps. */
void     os_random(void *buf, size_t len);
/* 32 random bits (os_random). */
uint32_t os_random_u32(void);

/* startup message ----------------------------------------------------------- */

/* Read the startup message on ch (the channel the program started with,
 * in rdi) into the table startup_handle reads; for a warm spare, wait for
 * its promotion first (which may end the process: <roles.h>). *argc and
 * *argv: its strings (argv[argc] is NULL, and the environment's
 * "KEY=value" strings follow it, NULL-terminated). Never fails: an
 * unusable message leaves argc 0 and no handles, startup_status() saying
 * why and a line in the log. libos's start calls it before main; a POSIX
 * program's crt calls it itself (it uses no heap, no printf and no
 * thread-local storage). Once, at the start. argv and an environment too
 * big for the message come in a VMO (SR_ARGS, <roles.h>), copied into our
 * own memory here. */
void        startup_read(handle_t ch, int *argc, char ***argv);
/* The program's path as the starter named it beside argv (SR_ARGS: a
 * program from a VMO given no POSIX hand-over), or NULL. */
const char *startup_exe(void);
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

/* Address space reserved: 1 GiB (M12 Q16: the job's page limit is the real bound).
 * Pages commit on touch, so a program that uses little pays for little. */
#define HEAP_SIZE (1u << 30)

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

struct posixfds_out;   /* <posixfds.h> */

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
    /* Its first thread's stack: bytes of address space (rounded up to
     * whole pages), committed as the child touches them, with a guard
     * page below. 0: SPAWN_STACK_DEFAULT. More than SPAWN_STACK_MAX:
     * ERR_OUT_OF_RANGE. Its pages are charged to the caller's job, as
     * every VMO spawn makes is. */
    uint64_t                   stack_size;
    /* NULL, or the POSIX hand-over (<posixfds.h>) for a POSIX program:
     * written on its startup channel right after the startup message,
     * before it starts (posixfds_write). Its handles are consumed
     * whatever happens, as the extras are. */
    const struct posixfds_out *handover;
    /* NULL, or called once the startup message (and the hand-over) is on
     * the child's startup channel, before the child starts: it may write
     * more there (`startup` is our end, not to be closed). A status other
     * than OK fails the spawn with it. For a starter that passes on a
     * hand-over another program wrote: init's /svc/exec (exec.c). */
    status_t                 (*before_start)(handle_t startup, void *ctx);
    void                      *before_start_ctx;
};

#define SPAWN_STACK_DEFAULT (128ull << 10)   /* a native program's */
#define SPAWN_STACK_MAX     (1ull << 30)     /* 1 GiB of address space */

/* Load a program (spawn_args.path) into a new process and start it. Its
 * startup message has argv, SELF_PROCESS, SELF_VMAR, SELF_THREAD, JOB (a
 * duplicate of a->job with JOB_RIGHTS_OWN: the child can't change its own
 * limits), BOOTFS (a duplicate of ours), NS (when a->ns is set),
 * ARGS (argv and the environment when they don't fit the message's 2 KiB
 * or 128 strings, and the program's path for one given SELF_IMAGE and no
 * hand-over: <roles.h>), SELF_IMAGE (a program from a->vmo at offset 0
 * that can run: its own VMO, <roles.h>; when the extras leave room) and
 * the extras, which are consumed whatever happens (and so are the
 * hand-over's, whose program's path spawn fills in when it has none).
 * *proc gets the process handle. ERR_OUT_OF_RANGE: too many extras
 * (STARTUP_MAX_HANDLES - 6 fit next to a namespace, one more without; one
 * fewer with ARGS), argv and the environment past STARTUP_ARGS_MAX (their
 * bytes and 8 a string), or a stack bigger than SPAWN_STACK_MAX. A
 * hand-over refused by posixfds_write fails the spawn with its error. */
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
/* A lock for our threads (user/lib/lock.c): a zeroed lock_t is free. A
 * waiter spins briefly, then sleeps on a futex until the holder gives it.
 * Short holds only; never taken twice by one thread. */
typedef struct {
    uint32_t word;   /* 0 free, 1 held, 2 held with sleepers (lock.c) */
} lock_t;
void     lock_take(lock_t *l);
void     lock_give(lock_t *l);

/* fork ----------------------------------------------------------------------------
 * A copy of this process (docs/FORK-PLAN.md section 1; user/lib/fork.c):
 * the same program in a new process in our job (SR_JOB), with our address
 * space copied at one instant (vmar_clone: copy-on-write, shared or left
 * out by each mapping's clone mode), and one thread, the calling one,
 * which goes on from the call. The child holds only what libjam and the C
 * library hand it, as duplicates: its own process, address space and
 * thread, our job, our namespace (each entry duplicated), our SR_STDOUT,
 * SR_CONSOLE, SR_BOOTFS and SR_SELF_IMAGE, a new signal channel if asked;
 * never our root (SR_RESOURCE) nor a service's roles. Every other handle
 * value in its copy of our memory names nothing there (its table starts
 * at random generations): libjam's own caches of handles (the namespace,
 * svc_get's channels, printf's SR_STDOUT) are made again in the child. */

/* What a fork passes besides libjam's own roles. */
struct jam_fork_args {
    /* The calling thread's FS base (its thread pointer), which the
     * child's thread gets before anything runs there; 0: none (libos
     * itself uses no thread-local storage). */
    uint64_t        fs_base;
    /* The child process's name; NULL: ours. */
    const char     *name;
    /* n VMOs of ours mapped as copies whose children (vmar_clone) we want
     * handles to, at most VMAR_CLONE_MAX; children[i] gets the one made
     * for vmos[i]. They are ours: the C library's give moves them to the
     * child and zeroes their entries, and whatever is left non-zero after
     * jam_fork is ours to close. A clone that fails leaves them all 0.
     * They are read after the C library's prepare, which may set them
     * (the POSIX layer's table of mappings is whole only under its lock). */
    const handle_t *vmos;
    handle_t       *children;
    uint32_t        n;
    /* NULL, or where the client end of the child's new signal channel
     * goes (its server end is the child's SR_SIGNALS, <roles.h>), on
     * success only. */
    handle_t       *signals;
    /* The C library's, passed to each of its hooks. */
    void           *ctx;
};

/* The C library's part of every fork, set once at its start
 * (jam_fork_set_libc): libos's own (startmain.c: its heap's lock and
 * printf's channel) or the POSIX layer's. In the parent: prepare takes the
 * C library's locks, in its fixed order, right before the clone, so its
 * memory is whole in the child's copy; give writes its own messages on the
 * child's startup channel after libjam's, its locks still held; parent
 * gives the locks back, the fork done or failed (st). In the child, on the
 * forking thread's stack before jam_fork returns 0: child makes those locks
 * fresh and reads what give wrote, after libjam has made the child's roles
 * its own. None of them may allocate memory, nor may anything between
 * prepare and parent (the allocator's lock may be held: libos's is in
 * its prepare, musl's fork takes its own). Any of them may be NULL. */
struct jam_fork_libc {
    void     (*prepare)(void *ctx);
    /* to: our end of the child's startup channel; proc: the child, not
     * started. Another status than OK fails the fork with it. */
    status_t (*give)(void *ctx, handle_t to, handle_t proc);
    void     (*parent)(void *ctx, status_t st);
    /* from: the child's startup channel (startup_channel()). */
    void     (*child)(void *ctx, handle_t from);
};
void    jam_fork_set_libc(const struct jam_fork_libc *libc);

/* Fork: returns twice, in the parent the child's process handle (> 0,
 * ours to wait on, kill or close: closing it doesn't end a started
 * child), and 0 in the child; a status (< 0) in the parent, with no child
 * made, if it fails: ERR_NO_MEMORY when our job can't pay for the copy
 * (every page the child shares is charged once more, at the fork, and
 * nothing later), ERR_BAD_STATE when a pin or a kernel mapping covers
 * memory to copy (a driver), or a creation's or the C library's give's
 * error. The child resumes with the caller's callee-saved registers, stack
 * pointer, MXCSR and x87 control word as they were at the call. Only the
 * calling thread is in the child; a lock another thread of ours held at
 * the fork stays held there, unless the C library or libjam lists it. */
int64_t jam_fork(const struct jam_fork_args *a);

/* ports --------------------------------------------------------------------------- */

/* Another turn for h, which a loop stopped reading with messages still
 * queued (a budget a turn, so one busy channel can't hold up the rest). h
 * is bound PORT_BIND_PERSISTENT to port with key for mask, and such a
 * binding queues a packet when h's signals turn to match, not while they
 * stay so: without another turn no packet would come for what is left,
 * and the loop would never read h again. A user packet with key is queued,
 * behind what else waits; if the port takes none (PORT_MAX_USER_PACKETS
 * queued, or our job's memory), h is bound again instead, which queues a
 * packet at once while its signals match. OK, or the bind's status: h is
 * then not watched any more (port_again.c). */
status_t port_again(handle_t port, handle_t h, uint64_t key, signals_t mask);

/* files ---------------------------------------------------------------------------
 * The namespace: the mount points a program was given, each with its `fs`
 * channel (abi/idl/fs.idl, file.idl), and the services (below: "/svc").
 * A mount point is "/" and one name
 * ("/boot", "/data"). The calls below take absolute paths of at most
 * FS_PATH_MAX - 1 bytes (longer: ERR_NAME_TOO_LONG; not starting with
 * '/': ERR_INVALID_ARGS; the `fs` protocol itself carries paths of up to 4095
 * bytes, FS_PATH_LEN: libos's limit goes up to it when nothing shares it
 * any more), find the mount by the path's first name and send the
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
 * the other end and writes nschan events on it (abi/idl/nschan.idl); a
 * mount's or set's handles are the `fs` channels and the services' channels, one
 * per path, in order (a path "/svc/<name>" is a service). The
 * first message (a `mount`, possibly of nothing) comes right after the
 * start; libos waits for it (up to NS_FIRST_WAIT) before its first lookup.
 * Later ones change the running program's namespace: `mount` adds mounts,
 * or replaces those with the same path; `unmount` (no handles) removes
 * them; `set` makes the starter's mounts and services exactly the ones
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
 * end (spawn_args.ns_back_out) and sends with ns_update: a `set` of the
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
/* Writes may wait in the service (fat: about a MiB) and go to the disk
 * together, by file.sync or the close at the latest: far fewer, bigger
 * disk writes, which a stick does many times faster than sector-sized
 * ones. A stick pulled before the sync loses what waited. With FS_WRITE. */
#define FS_GATHER   32u
#define FS_EXCL     64u   /* with FS_CREATE: ERR_ALREADY_EXISTS if it is there */
/* A symbolic link as the last name is not followed (fs.open: opening one is
 * ERR_INVALID_ARGS; fs.getattr, setattr, resolve: the link itself). */
#define FS_NOFOLLOW  128u
#define FS_DIRECTORY 256u /* fs.open: the path must be a directory (ERR_NOT_DIR) */
#define FS_FLAGS    511u  /* all of them: any other bit is ERR_INVALID_ARGS */
/* The mode file_open and fs_mkdir ask to create with: what FAT's made-up
 * modes were. (file_open_mode and fs_mkdir_mode take one; a service that
 * keeps no modes ignores it.) */
#define FS_MODE_FILE_DEFAULT 0644u
#define FS_MODE_DIR_DEFAULT  0755u
#define FS_NAME_MAX 766   /* a name in a directory, with its NUL (fs.idl's NAME_LEN + 1) */
#define FS_CALL_TIMEOUT (60 * NS_PER_S)

#define NS_NAME_MAX    32  /* an entry's path with its NUL: "/data", "/svc/devmgr-ctl" */
#define NS_MAX_MOUNTS  16  /* mounts in one namespace */
#define NS_MAX_SVCS    32  /* services in one namespace */
#define NS_MAX_ENTRIES (NS_MAX_MOUNTS + NS_MAX_SVCS)   /* entries in one nschan event */
#define NS_FIRST_WAIT  (5 * NS_PER_S)
/* (The limits above are abi/idl/nschan.idl's NSCHAN_*: ns.c asserts it.) */

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
/* On `to` (our end of another program's SR_NS channel): a `mount` of
 * what `grants` names (NULL-terminated; see "grants" below), each mount
 * as a duplicate of our channel or a view made for it, each service as a
 * duplicate of ours. What we don't have is left out, and so is a mount
 * whose view its service won't make (fail closed). */
status_t ns_send(handle_t to, const char *const *grants);
/* The same for one channel of the caller's (consumed, whatever happens)
 * at path (a mount point or /svc/<name>); fs HANDLE_INVALID sends an
 * `unmount` of path instead. */
status_t ns_send_one(handle_t to, const char *path, handle_t fs);
/* Keep a running program's namespace in step with ours: on `to` an
 * `set` of what `grants` names (as ns_send), after taking back
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

#define SVC_NAME_MAX   24            /* bytes of a service's name */
/* The services init publishes (tools/checkwants.py reads this list). */
#define SVC_AUDIO      "audio"       /* the mixer: sound streams, a channel per opener (audio.idl) */
#define SVC_AUDIOCTL   "audioctl"    /* the mixer's volumes, a channel per opener (audioctl.idl) */
#define SVC_MUSIC      "music"       /* the music player, a channel per opener (music.idl) */
#define SVC_DEVMGR     "devmgr"      /* devmgr's queries, a channel per opener (<devmgr.h>) */
#define SVC_DEVMGR_CTL "devmgr-ctl"  /* devmgr's control channel, a channel per opener: tests only */
#define SVC_VOLUMES    "volumes"     /* volumes: the disks and their mounts, a channel per
                                      * opener (<volumes.h>): tests only */
#define SVC_INIT       "init"        /* init's control channel (initctl.idl): tests only */
#define SVC_LOGD       "logd"        /* logd's control channel, a channel per opener (logctl.idl) */
#define SVC_NET        "net"         /* netstack: UDP sockets and ping, a channel per opener (net.idl) */
#define SVC_NET_SYS    "net-sys"     /* netstack for the network's own services (their reserve): user/services/ only */
#define SVC_DNS        "dns"         /* the resolver: names to addresses, a channel per opener (dns.idl) */
#define SVC_DNS_SYS    "dns-sys"     /* the resolver for the network's own services (their reserve): user/services/ only */
#define SVC_NET_LISTEN "net-listen"  /* /svc/net whose openers may also listen; a list asks for it
                                      * as `svc net listen` (<wants.h>) */
/* /svc/net-listen's permission on ports below 1024 too; a list asks for it as
 * `svc net listen low`: user/services/ only, never a program on /data */
#define SVC_NET_LISTEN_LOW "net-low"
#define SVC_SERVE      "serve"       /* the file server, a channel per opener (serve.idl) */
#define SVC_WAYLAND    "wayland"     /* the compositor: windows, a connection per opener (<jwl.h>) */
/* The compositor's notices: a notify.idl channel per opener (<notice.h>);
 * user/services/ only (the shell's `notify`) */
#define SVC_NOTIFY     "notify"
/* init's sessions: a Jam OS shell on a connection console, a channel per
 * opener (session.idl, docs/M13-PLAN.md section 9; init's sessions.c). It is a
 * whole shell, so only a boot-image service, port or test may ask for it
 * (tools/checkwants.py), and `allow` refuses it for a program on /data. */
#define SVC_SESSION    "session"
/* init's sealer: starts a program on /data the owner allowed, with its own
 * approval's namespace and powers, a channel per opener (exec.idl; init's
 * exec.c). Any program may ask for it (`svc exec`): it starts approved
 * programs only, each as approved. */
#define SVC_EXEC       "exec"
/* The named AF_UNIX socket broker (unix.idl; bin/unix), a channel per
 * opener: bind, listen, accept and connect on socket files and abstract
 * names. Any program may ask for it (`svc unix`). */
#define SVC_UNIX       "unix"

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
 * queued; ERR_PEER_CLOSED: every client is gone). One thread at a time.
 * As with <proto>_serve_one, the unread bound is the caller's: ask the
 * protocol's <proto>_unread_full(ch) first (tools/genidl.py, "The unread
 * bound"). */
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
 *   "/data:w"        a view that may write
 *                    (neither view has the top-level `etc` at all: not
 *                    listed, opened, read, made or changed, FS_VIEW_NO_ETC;
 *                    every program's mount, whatever its list says);
 *                    the writable one leaves the top-level `pkg` alone
 *                    too (FS_VIEW_GUARD_PKG: the packages' prefix)
 *   "/data:wp"       a view that may write, the packages' prefix too
 *   "/jam:wp"        (still no etc): bin/pkg's (`mount /data rw pkg`,
 *                    `mount /jam rw pkg`)
 *   "/data:rk"       a read-only view that shows `etc/ssh` (the SSH host
 *                    key) and `etc/authorized_keys`, nothing else of etc:
 *                    only C code writes it (init's grants for sshd, sshd's
 *                    for its children); no list can ask for it
 *   "/data:ra"       the same without `etc/ssh`: sshd's key check's
 *   "/usb*"          (with or without :r, :w, :rk, :ra) every mount whose
 *                    point starts with "/usb"
 *   "*:r", "*:w"     every mount, as views (no services)
 * A view is made with a call to the mount's service at the time of the
 * send (each waits at most NS_VIEW_WAIT). A service that counts what a
 * program holds by the channel it calls on (/svc/unix: the broker's
 * listeners) is given by name as a channel of the program's own, asked
 * of ours with svc.connect at the send, which hands out no more: the
 * program can't open another to get past its limits. "*" gives ours. */
#define NS_VIEW_WAIT (2 * NS_PER_S)

/* For fs services: a path inside a mount (fs.idl's str: len bytes, no NUL)
 * as the names it walks, joined by '/' with none leading or trailing (""
 * for the mount's root): "/a/./b/../c" is "a/c", and ".." at the root
 * stays there. out has room for len + 1 bytes (the result, NUL-terminated,
 * is never longer). ERR_INVALID_ARGS: a NUL in the path. */
status_t fs_path_clean(const char *path, uint32_t len, char *out);

struct jfile {
    handle_t ch;          /* the file protocol channel */
    handle_t buf_vmo;     /* the transfer buffer */
    uint8_t *buf;         /* ... mapped (read-only unless opened FS_WRITE) */
    uint32_t buf_size;    /* its size in bytes */
    uint32_t flags;       /* the FS_* flags it was opened with */
    uint64_t size;        /* the file's size at open (file_stat for now) */
};

struct fs_entry {
    char     name[FS_NAME_MAX];   /* NUL-terminated */
    bool     is_dir;              /* a directory */
    uint64_t size;                /* bytes; 0 for a directory */
    uint64_t mtime;               /* Unix seconds; 0 if unknown */
    uint64_t id;                  /* fs.stat's id (fs.idl) */
    uint8_t  type;                /* FS_DT_* (fs_idl.h): the entry's kind, 0 if the service
                                   * doesn't say */
};

/* A path's or an open file's attributes (fs.getattr, file.getattr): POSIX's
 * stat, with times in nanoseconds since 1970 (fs.idl says what FAT and the
 * boot image make up). */
struct fs_attr {
    uint32_t mode;                /* kind (FS_MODE_*) and permission bits */
    uint32_t nlink, uid, gid;
    uint64_t size;                /* bytes */
    uint64_t blocks;              /* 512-byte units */
    uint64_t id;                  /* fs.stat's id */
    int64_t  atime_ns, mtime_ns, ctime_ns;
};

/* What fs.setattr and file.setattr set: the fields `mask` names (FS_ATTR_*,
 * fs_idl.h), the rest ignored. */
struct fs_attr_set {
    uint32_t mask;
    uint32_t mode;                /* the permission bits */
    uint32_t uid, gid;
    int64_t  atime_ns, mtime_ns;
};

/* Open a file (FS_* flags), not a directory. *out is ours until file_close.
 * A file FS_CREATE makes gets FS_MODE_FILE_DEFAULT; file_open_mode gives
 * its permission bits (07777, the umask applied by the caller). */
status_t file_open(const char *path, uint32_t flags, struct jfile *out);
status_t file_open_mode(const char *path, uint32_t flags, uint32_t mode, struct jfile *out);
/* The open file's attributes (file.getattr), and set some (file.setattr). */
status_t file_getattr(struct jfile *f, struct fs_attr *out);
status_t file_setattr(struct jfile *f, const struct fs_attr_set *set);
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
 * ERR_NOT_FOUND. The service answers entries in batches (fs.readdir): a
 * listing asked in order (0, 1, 2, ...) costs a call per batch (libos keeps
 * the last batch it was given); index 0 always asks afresh. */
status_t fs_readdir(const char *path, uint32_t index, struct fs_entry *out);
status_t fs_mkdir(const char *path);
/* The same with the new directory's permission bits (FS_MODE_DIR_DEFAULT above). */
status_t fs_mkdir_mode(const char *path, uint32_t mode);
/* The calls of a filesystem with links and modes (fs.idl: getattr, setattr,
 * symlink, readlink, link, resolve, subview), by path through the
 * namespace. A service without them (fat, the boot image) answers
 * ERR_NOT_SUPPORTED, except getattr, which it makes up, and setattr, which
 * it accepts and ignores. They send the path as it is: ERR_LINK_OUT is the
 * caller's to follow (libc does; fs_resolve says where). "/" and "/svc" are
 * directories the namespace makes up (getattr: mode 0555, one user). */
status_t fs_getattr(const char *path, uint32_t flags, struct fs_attr *out);
status_t fs_setattr(const char *path, uint32_t flags, const struct fs_attr_set *set);
status_t fs_symlink(const char *target, const char *path);
/* The link's target, NUL-terminated, into buf of cap bytes (ERR_BUFFER_TOO_SMALL:
 * 4096 holds any). */
status_t fs_readlink(const char *path, char *buf, size_t cap);
/* Both names on one mount, else ERR_NOT_SUPPORTED (as fs_rename). */
status_t fs_link(const char *from, const char *to);
/* fs.resolve: *kind (FS_RESOLVE_*), *used, and next (4096 bytes, NUL-terminated). */
status_t fs_resolve(const char *path, uint32_t flags, uint8_t *kind, uint16_t *used,
                    char next[4096]);
/* A channel on the directory at path, as its mount's service serves it
 * (fs.subview; flags are fs.view's): *out is ours, to serve as a mount
 * (ns_send, a spawn's namespace) or to close. */
status_t fs_subview(const char *path, uint32_t flags, handle_t *out);
/* Remove a file, or an empty directory (C's remove()). */
status_t fs_unlink(const char *path);
/* Both on one mount, else ERR_NOT_SUPPORTED. Something at `to`:
 * ERR_ALREADY_EXISTS. */
status_t fs_rename(const char *from, const char *to);
/* Everything written to the mount holding path is on its medium. */
status_t fs_sync(const char *path);
/* The same, giving up at deadline_ns (ERR_TIMED_OUT). */
status_t fs_sync_by(const char *path, uint64_t deadline_ns);
/* The mount holding path: its size and free space in bytes, whether it is
 * read-only, and its volume label (NUL-terminated). Outputs may be NULL. */
status_t fs_statfs(const char *path, uint64_t *total, uint64_t *free_bytes, bool *read_only,
                   char label[17]);
/* What kind of filesystem path's mount is (statfs's other results): its
 * magic (Linux's f_type), the longest name it holds, and its FS_FEAT_*
 * (fs_idl.h). Outputs may be NULL. */
status_t fs_statfs_kind(const char *path, uint32_t *magic, uint32_t *name_max,
                        uint32_t *features);
/* The whole file at path in a new VMO (whole pages, the rest zero) and its
 * size in bytes. ERR_OUT_OF_RANGE: bigger than max_size. */
status_t file_read_vmo(const char *path, uint64_t max_size, handle_t *vmo, uint64_t *size);

/* Mapped files (file.map, abi/idl/file.idl): a VMO whose pages the file's
 * service supplies as they are touched. flags: FILE_MAP_READ (<fs_idl.h>),
 * the file's one shared VMO, read and map only, or FILE_MAP_EXEC, a VMO of
 * the caller's own for vmo_make_exec_verified. fs_map_file maps the open
 * file f, file_map_path the file at path (opened and closed here: the VMO
 * outlives the file); the VMO into *vmo (ours) and the file's size in
 * bytes into *size. ERR_NOT_SUPPORTED: the mount's service has no pages to
 * give; ERR_ACCESS_DENIED: f is not open FS_READ; ERR_IS_DIR: a
 * directory; ERR_INVALID_ARGS: flags is neither. */
status_t fs_map_file(struct jfile *f, uint32_t flags, handle_t *vmo, uint64_t *size);
status_t file_map_path(const char *path, uint32_t flags, handle_t *vmo, uint64_t *size);

/* A file mapped read-only into our own address space. A read of data[i]
 * (i < size) waits for the service to supply the page; if it can't (the
 * disk went, the file was cut short) the access ends the thread's process
 * with a line saying why (a system call's copy from the mapping gets
 * ERR_IO instead). */
struct mapped_file {
    handle_t       vmo;      /* the file's VMO (FILE_MAP_READ) */
    const uint8_t *data;     /* its bytes, as of the mapping; NULL for an empty file */
    uint64_t       size;     /* the file's size in bytes when mapped */
    uint64_t       map_len;  /* bytes mapped: size rounded up to whole pages */
};

/* Map the file at path (file_map_path, then mapped_file_from_vmo). *out is
 * ours until mapped_file_close; errors as file_map_path's, and ERR_INTERNAL if
 * the service's VMO is smaller than the file. */
status_t mapped_file_open(const char *path, struct mapped_file *out);
/* The same for a VMO already in hand and the file's size (consumed
 * whatever happens). */
status_t mapped_file_from_vmo(handle_t vmo, uint64_t size, struct mapped_file *out);
/* Unmap it and close the VMO; *mf is zeroed. */
void     mapped_file_close(struct mapped_file *mf);

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
