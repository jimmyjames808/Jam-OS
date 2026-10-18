/* The POSIX hand-over: what a POSIX program keeps across a start, sent by
 * its starter on the startup channel right after the startup message
 * (docs/M13-PLAN.md section 5). The startup message (<jam/startup.h>)
 * stays as it is; this follows it, in one or more messages:
 *
 *   - the descriptors: for each, its number, its kind, the open file
 *     description's status flags and offset, the kind's handles (a
 *     duplicate of each, or for a file a fresh open) and the kind's data
 *     (a directory's path, a /dev node's name, ...);
 *   - the process's state that exec keeps: the current directory, the
 *     umask, the blocked and the ignored signals (and, from an exec in
 *     place, the pending ones), the parent's pid, the process group and
 *     the session;
 *   - the program's path, when the starter names one: what libc's
 *     /proc/self/exe reads (docs/CR-PLAN.md section 3). The starter's
 *     word, as argv is: it chose the program;
 *   - whether the program starts as Linux's AT_SECURE: its argv and
 *     environment came from a program it doesn't trust as itself (init's
 *     /svc/exec marks every child it starts so: user/services/init/exec.c).
 *     A starter can only make its child more careful by it.
 *
 * Descriptors with FD_CLOEXEC are not sent. A message carries at most
 * CHANNEL_MAX_HANDLES handles and CHANNEL_MAX_BYTES bytes, so a big table
 * takes several messages, numbered from 0; the last one is marked
 * POSIXFDS_LAST. The first carries the process state, the current
 * directory and the program's path, the others only descriptors.
 *
 * The starter writes every message before process_start (spawn does, with
 * spawn_args.handover), so the child reads them without waiting: nothing
 * on the channel after the startup message means there is no hand-over.
 * The child (the POSIX crt, user/posix/) reads it with posixfds_read
 * before main. It is a hand-written format, not IDL: its id in
 * abi/idl/ids.txt is the top half of POSIXFDS_MAGIC.
 *
 * It crosses a process boundary, so the reader checks everything (the
 * rules are on the structs below) and takes the hand-over whole or not at
 * all: a malformed message, one missing, or anything queued after the
 * last (the starter wrote it all before the start), drops every handle the
 * hand-over carried and the child starts with no descriptors. The kinds'
 * own meaning (what a pipe's data is, what a socket's handles are for)
 * is the POSIX layer's; this header fixes only each kind's handle types
 * and the shape of its data, so a kind's reader never sees the wrong
 * thing. */
#pragma once

#include <stdint.h>
#include <os.h>

#define POSIXFDS_MAGIC    0x5853504au   /* "JPSX" */
#define POSIXFDS_VERSION  2             /* 2: the state's sigpending (FL3) */
#define POSIXFDS_LAST     1u            /* posixfds_msg.flags: the last message */
#define POSIXFDS_SECURE   2u            /* ... the first only: start as AT_SECURE (above) */
#define POSIXFDS_MSG_MAX  CHANNEL_MAX_BYTES   /* bytes of one message */
#define POSIXFDS_FD_MAX   1024          /* descriptors are 0 .. POSIXFDS_FD_MAX - 1 */
#define POSIXFDS_PATH_MAX 4096          /* the current directory, its NUL included */
#define POSIXFDS_DATA_MAX 4096          /* a descriptor's data */
#define POSIXFDS_KIND_HANDLES 4         /* the most handles one descriptor has */
#define POSIXFDS_PID_MAX  (1u << 22)    /* pids are below this (the kernel's) */
#define POSIXFDS_UMASK    0777u         /* umask's bits */

/* Status flags (posixfds_fd.status) are Linux's x86-64 O_* values, as musl
 * passes them; these are the ones a description keeps. The access mode is
 * O_RDONLY (0), O_WRONLY (1) or O_RDWR (2); 3 is refused. */
#define POSIXFDS_O_ACCMODE   0x3u
#define POSIXFDS_O_APPEND    0x400u
#define POSIXFDS_O_NONBLOCK  0x800u
#define POSIXFDS_O_DSYNC     0x1000u
#define POSIXFDS_O_ASYNC     0x2000u
#define POSIXFDS_O_DIRECT    0x4000u
#define POSIXFDS_O_LARGEFILE 0x8000u
#define POSIXFDS_O_DIRECTORY 0x10000u
#define POSIXFDS_O_NOATIME   0x40000u
#define POSIXFDS_O_SYNC      0x101000u
#define POSIXFDS_O_PATH      0x200000u
#define POSIXFDS_STATUS (POSIXFDS_O_ACCMODE | POSIXFDS_O_APPEND | POSIXFDS_O_NONBLOCK |      \
                         POSIXFDS_O_DSYNC | POSIXFDS_O_ASYNC | POSIXFDS_O_DIRECT |          \
                         POSIXFDS_O_LARGEFILE | POSIXFDS_O_DIRECTORY | POSIXFDS_O_NOATIME |  \
                         POSIXFDS_O_SYNC | POSIXFDS_O_PATH)

/* Signals in the two masks: bit n - 1 for signal n (Linux's numbers, 1 to
 * 64). SIGKILL and SIGSTOP can be neither blocked nor ignored: a mask with
 * either is refused. */
#define POSIXFDS_SIG(n)       (1ull << ((n) - 1))
#define POSIXFDS_SIG_NEVER    (POSIXFDS_SIG(9) | POSIXFDS_SIG(19))   /* SIGKILL, SIGSTOP */

/* A descriptor's kind: which handles it has (in this order) and its data.
 * The numbers are the format's; never reuse one. */
enum posixfds_kind {
    POSIXFDS_FILE = 1,        /* `file` channel, buffer VMO; data: any (the layer's) */
    POSIXFDS_DIR = 2,         /* no handles; data: its absolute path, a string */
    POSIXFDS_PIPE = 3,        /* a channel end; data: any (bytes read but not taken) */
    POSIXFDS_SOCKET = 4,      /* channel, ring VMO, two events (<sockring.h>); data: any */
    POSIXFDS_LISTENER = 5,    /* a `net_listener` channel; data: any */
    POSIXFDS_TTY = 6,         /* `tty` channel, data channel (console.open_tty); data: any */
    POSIXFDS_PTY_MASTER = 7,  /* the master channel, its `session_ctl`; data: any */
    POSIXFDS_DEV = 8,         /* no handles; data: the node's name ("null"), a string */
    POSIXFDS_ETC = 9,         /* no handles; data: the file's path ("/etc/passwd"), a string */
    POSIXFDS_MEMFD = 10,      /* a VMO; data: any (its size, its name) */
    POSIXFDS_EVENTFD = 11,    /* an event; data: any (its counter) */
    POSIXFDS_DUP = 12,        /* no handles; data: a u32, the number of a descriptor sent
                               * before this one: this is the same open file description
                               * (flags, offset, handles), as dup2 makes it; status 0 */
    POSIXFDS_SEQPACKET = 13,  /* a SOCK_SEQPACKET socketpair end: a channel end; data: any
                               * (the message a peek kept, whole) */
    POSIXFDS_PROC = 14,       /* one of libc's /proc files; no handles; data: its path
                               * ("/proc/<pid>/statm"), a string, opened again by the
                               * receiver, whose /proc it names (sent over socketpairs
                               * only: a child's hand-over leaves /proc files out) */
    POSIXFDS_KIND_COUNT,
};

/* The process state exec keeps (in the first message). */
struct posixfds_state {
    uint32_t umask;         /* 0 .. POSIXFDS_UMASK */
    uint32_t ppid;          /* the starter's pid; below POSIXFDS_PID_MAX */
    uint32_t pgid;          /* the process group; below POSIXFDS_PID_MAX */
    uint32_t sid;           /* the session; below POSIXFDS_PID_MAX */
    uint64_t sigmask;       /* blocked signals (POSIXFDS_SIG); never POSIXFDS_SIG_NEVER */
    uint64_t sigignore;     /* ignored signals; never POSIXFDS_SIG_NEVER */
    uint64_t sigpending;    /* signals pending at an exec in place (execve keeps them, as
                             * Linux does); 0 for a new process; never POSIXFDS_SIG_NEVER */
};

/* A message: this header, then (in the first message only) cwd_len bytes
 * of the current directory and exe_len bytes of the program's path, then
 * nfds records, each a struct posixfds_fd and its data_len bytes; the
 * directory, the path and each record's data are padded with zero bytes
 * to a multiple of 8, and the message ends right after the last one. The
 * handles come in the records' order, each record's in its kind's order. */
struct posixfds_msg {
    uint32_t txid;          /* channel convention: first 4 bytes; 0 here */
    uint32_t magic;         /* POSIXFDS_MAGIC */
    uint16_t version;       /* POSIXFDS_VERSION */
    uint16_t flags;         /* POSIXFDS_LAST on the last message, POSIXFDS_SECURE
                             * (or not) on the first; no other bit */
    uint16_t seq;           /* this message's place: 0, 1, 2, ... */
    uint16_t nfds;          /* records; at least 1 but in the first message */
    uint32_t nhandles;      /* handles carried: the records' handles, all of them */
    uint32_t cwd_len;       /* first message: the directory's bytes with its NUL
                             * (an absolute path, 2 .. POSIXFDS_PATH_MAX); else 0 */
    struct posixfds_state state;   /* first message: as above; else all 0 */
    uint32_t exe_len;       /* first message: the program's path's bytes with its NUL
                             * (an absolute path, 2 .. POSIXFDS_PATH_MAX), or 0 (not
                             * named); else 0 */
    uint32_t reserved;      /* 0 */
};

/* One descriptor. */
struct posixfds_fd {
    int32_t  fd;            /* 0 .. POSIXFDS_FD_MAX - 1, once in the whole hand-over */
    uint16_t kind;          /* enum posixfds_kind */
    uint16_t nhandles;      /* exactly the kind's count (posixfds_kind_handles) */
    uint32_t status;        /* POSIXFDS_STATUS bits; access mode 0, 1 or 2 */
    uint32_t data_len;      /* bytes of data after the record, 0 .. POSIXFDS_DATA_MAX
                             * (a string kind's: its NUL included, at least 2) */
    uint64_t offset;        /* the description's offset: a file's or a directory's
                             * (where its listing has got to); 0 for any other kind */
};

/* One descriptor to send (posixfds_write). */
struct posixfds_desc {
    int32_t         fd;         /* as in struct posixfds_fd */
    uint16_t        kind;
    uint32_t        status;
    uint64_t        offset;
    const void     *data;       /* data_len bytes (a string kind's with its NUL) */
    uint32_t        data_len;
    const handle_t *handles;    /* the kind's count of them, moved */
};

/* A whole hand-over to send. */
struct posixfds_out {
    struct posixfds_state       state;
    const char                 *cwd;    /* absolute, shorter than POSIXFDS_PATH_MAX */
    const struct posixfds_desc *fds;    /* nfds of them, in any order */
    unsigned                    nfds;
    /* NULL, or the program's path, as cwd (spawn names it when this is
     * NULL: spawn_args.path, a boot-image name as /boot/<name>) */
    const char                 *exe;
    bool                        secure;   /* the program starts as AT_SECURE */
};

/* Where posixfds_read puts the descriptors, one at a time, each only
 * after its whole message was checked. */
struct posixfds_sink {
    void *ctx;
    /* A descriptor: its record, its data (rec->data_len bytes; gone after
     * the call) and its handles (rec->nhandles of them, the sink's from
     * now on whatever it returns). Anything but OK refuses the hand-over:
     * reset is called and posixfds_read returns that status. */
    status_t (*fd)(void *ctx, const struct posixfds_fd *rec, const void *data,
                   const handle_t *hs);
    /* The hand-over failed after fd() was called: close every handle it
     * was given and forget those descriptors. */
    void (*reset)(void *ctx);
    /* NULL, or given the program's path (a string, absolute; gone after
     * the call) once the whole hand-over was taken, if it names one. */
    void (*exe)(void *ctx, const char *path);
    /* NULL, or called once the whole hand-over was taken, if it says the
     * program starts as AT_SECURE (POSIXFDS_SECURE). */
    void (*secure)(void *ctx);
};

/* How many handles a descriptor of kind has, and of which object types
 * (OBJ_*, in order, into types if not NULL; at most POSIXFDS_KIND_HANDLES).
 * -1 for a kind that isn't one. */
int      posixfds_kind_handles(uint32_t kind, uint32_t types[POSIXFDS_KIND_HANDLES]);

/* Send out on ch (the starter's end of the child's startup channel, right
 * after the startup message): as many messages as the handles and bytes
 * need. Every handle in out->fds is consumed whatever happens. Errors:
 * ERR_INVALID_ARGS, a descriptor or the state breaks the rules above
 * (nothing sent); ERR_OUT_OF_RANGE, more than POSIXFDS_FD_MAX descriptors;
 * a write's error (messages already written stay, and the child, missing
 * the last one, refuses them). Context: any thread (a lock guards the
 * message buffer). */
status_t posixfds_write(handle_t ch, const struct posixfds_out *out);
/* Close every handle in out->fds (a spawn that never got as far as
 * sending them). */
void     posixfds_discard(const struct posixfds_out *out);

/* Read the hand-over from ch (the startup channel, after the startup
 * message) without waiting, giving each descriptor to sink and the state
 * and the current directory to *state and cwd (written only on success).
 * ERR_NOT_FOUND: nothing queued (no hand-over; nothing changed).
 * ERR_INVALID_ARGS: a malformed or missing message: every handle of the
 * hand-over is closed (the sink's through reset, the rest here, every
 * message then queued on ch read and dropped) and the child has no
 * descriptors. Or the error sink->fd returned, the same way. Context:
 * any thread (the lock as above); no heap, no thread-local storage, no
 * printf, so the POSIX crt can call it before the C library runs. */
status_t posixfds_read(handle_t ch, const struct posixfds_sink *sink,
                       struct posixfds_state *state, char cwd[POSIXFDS_PATH_MAX]);
