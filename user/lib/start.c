/* Startup: read the startup message (<jam/startup.h>) from the channel a
 * program starts with, and turn it into argc/argv, the environment's
 * strings and a table of handles by role (startup_read). A native
 * program's start (startmain.c) then runs main; a POSIX program's crt
 * builds Linux's start block from the same strings. So this file uses no
 * heap, no printf and no thread-local storage: it runs before a POSIX
 * program's C library does.
 *
 * The message comes from whoever created the process, so it is checked
 * like any other input: a malformed one leaves the program with argc 0 and
 * no handles (and a line in the log) rather than reading past the buffer.
 *
 * A warm spare (a startup message with SR_STANDBY) waits here, before
 * main, having opened and read nothing, until its supervisor promotes it
 * with one struct standby_msg: its handles join the table (SR_STANDBY
 * goes), its strings become argv and the environment, and main runs as
 * for any start. A dismissal (the channel closed with nothing on it) ends
 * the spare with code 0; a malformed promotion with
 * STANDBY_BAD_PROMOTION, its handles closed.
 *
 * argv and the environment that don't fit the message come in a VMO
 * (SR_ARGS, <roles.h>), with the program's path when the starter names
 * it there: copied here into a VMO of our own, mapped for good, with
 * room for the pointers argv and the environment are, so that nobody
 * else can change them under us; SR_ARGS is closed and leaves the table.
 *
 * A forked child (fork.c) reads no startup message: its copy of this
 * table holds its parent's values, and startup_forked puts the fork
 * message's handles in their place (argv and the environment stay the
 * copy's). The C library's fork hooks are kept here, where every program
 * has them, so that setting them links no fork into a program that never
 * forks. */
#include <os.h>
#include <svcstate.h>
#include "fork.h"

#define STARTUP_BUF_MAX  8192   /* bytes; bigger startup messages are refused */
#define STARTUP_MAX_STRS 128    /* argv + environment strings */

#define TABLE_MAX (STARTUP_MAX_HANDLES + STANDBY_MAX_HANDLES)   /* a spare's, promoted */

_Static_assert(sizeof(struct standby_msg) == 296, "struct standby_msg has no padding");
_Static_assert(STANDBY_MAX_BYTES <= STARTUP_BUF_MAX && STANDBY_MAX_STRINGS <= STARTUP_MAX_STRS,
               "a promotion fits where the startup message was");

static _Alignas(8) uint8_t msg_buf[STARTUP_BUF_MAX];
static handle_t  msg_handles[TABLE_MAX];   /* the startup message's, then a promotion's */
static uint32_t  msg_roles[TABLE_MAX];     /* each one's enum startup_role */
static unsigned  msg_nhandles;             /* entries used */
static uint64_t  promoted_kill_ns;         /* standby_kill_ns() */
static uint64_t  promoted_at_ns;           /* standby_promoted_ns() */
static handle_t  start_channel;
static status_t  start_status = ERR_BAD_STATE;
static char     *str_ptrs[STARTUP_MAX_STRS + 2];   /* argv NULL envp NULL */
static const char *args_exe;               /* SR_ARGS's path of the program, or NULL */
static const struct jam_fork_libc *fork_hooks;   /* jam_fork_set_libc's */

void jam_fork_set_libc(const struct jam_fork_libc *libc)
{
    fork_hooks = libc;
}

const struct jam_fork_libc *jam_fork_hooks(void)
{
    return fork_hooks;
}

void startup_forked(handle_t ch, const handle_t *hs, const uint32_t *roles, unsigned n)
{
    start_channel = ch;
    start_status = OK;
    for (unsigned i = 0; i < n && i < TABLE_MAX; i++) {
        msg_handles[i] = hs[i];
        msg_roles[i] = roles[i];
    }
    msg_nhandles = n < TABLE_MAX ? n : TABLE_MAX;
}

const char *startup_exe(void)
{
    return args_exe;
}

uint64_t standby_kill_ns(void)
{
    return promoted_kill_ns;
}

uint64_t standby_promoted_ns(void)
{
    return promoted_at_ns;
}

handle_t startup_channel(void)
{
    return start_channel;
}

status_t startup_status(void)
{
    return start_status;
}

handle_t startup_handle(uint32_t role)
{
    for (unsigned i = 0; i < msg_nhandles; i++)
        if (msg_roles[i] == role)
            return msg_handles[i];
    return HANDLE_INVALID;
}

unsigned startup_handle_count(void)
{
    return msg_nhandles;
}

handle_t startup_handle_at(unsigned i, uint32_t *role)
{
    if (i >= msg_nhandles)
        return HANDLE_INVALID;
    if (role)
        *role = msg_roles[i];
    return msg_handles[i];
}

const char *startup_role_name(uint32_t role)
{
    switch (role) {
    case SR_NONE:         return "none";
    case SR_SELF_PROCESS: return "process";
    case SR_SELF_VMAR:    return "vmar";
    case SR_SELF_THREAD:  return "thread";
    case SR_JOB:          return "job";
    case SR_STDOUT:       return "stdout";
    case SR_BOOTFS:       return "bootfs";
    case SR_RESOURCE:     return "resource";
    case SR_DEVMGR:       return "devmgr";
    case SR_CONSOLE:      return "console";
    case SR_DEVMGR_CTL:   return "devmgr-ctl";
    case SR_NS:           return "ns";
    case SR_AUDIO:        return "audio";
    case SR_AUDIO_CTL:    return "audio-ctl";
    case SR_CRASHLOG:     return "crashlog";
    case SR_DEVMGR_DEVICE: return "devmgr-device";
    case SR_STATE:        return "state";
    case SR_KEEP:         return "keep";
    case SR_STANDBY:      return "standby";
    case SR_SIGNALS:      return "signals";
    case SR_SELF_IMAGE:   return "self-image";
    case SR_ARGS:         return "args";
    }
    return role >= SR_USER ? "user" : "?";
}

/* "libos: <what> (<status>)" as a line in the log, written with
 * debug_write itself: no printf here (above). */
static void say(const char *what, status_t st)
{
    const char *parts[] = { "libos: ", what, " (", status_str(st), ")\n" };
    char line[128];
    size_t n = 0;
    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        size_t l = strnlen(parts[i], sizeof(line) - n);
        memcpy(line + n, parts[i], l);
        n += l;
    }
    jam_debug_write(line, n);
}

/* Read the one message, waiting for it if it isn't there yet. */
static status_t read_message(handle_t ch, uint32_t *nbytes, uint32_t *nhandles)
{
    struct channel_read_args a = {
        .h = ch,
        .rd_cap = sizeof(msg_buf),
        .rd_bytes = (uint64_t)(uintptr_t)msg_buf,
        .rd_actual = (uint64_t)(uintptr_t)nbytes,
        .rd_handles = (uint64_t)(uintptr_t)msg_handles,
        .rd_hcap = STARTUP_MAX_HANDLES,
        .rd_hactual = (uint64_t)(uintptr_t)nhandles,
    };
    status_t st = jam_channel_read(&a);
    if (st == ERR_SHOULD_WAIT) {
        signals_t seen;
        st = jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER, &seen);
        if (st == OK)
            st = jam_channel_read(&a);
    }
    return st;
}

/* Point argv and the environment (ptrs: room for max + 2) at argc + envc
 * NUL-terminated strings, all inside [s, s + len). */
static status_t point_strings(char *s, uint32_t len, uint32_t argc, uint32_t envc, char **ptrs,
                              uint32_t max)
{
    if (argc > max || envc > max - argc)
        return ERR_INVALID_ARGS;
    char *end = s + len;
    unsigned n = argc + envc, slot = 0;
    for (unsigned i = 0; i < n; i++) {
        size_t l = strnlen(s, (size_t)(end - s));
        if (s + l == end)
            return ERR_INVALID_ARGS;   /* runs off the end */
        if (i == argc)
            slot++;                    /* skip argv's NULL */
        ptrs[slot++] = s;
        s += l + 1;
    }
    if (argc == n)
        slot++;                        /* argv's NULL when there is no env */
    ptrs[argc] = NULL;
    ptrs[slot] = NULL;
    return OK;
}

/* SR_ARGS's strings (h: its VMO) copied into a VMO of our own and mapped,
 * after argc + envc + 2 pointers, which become argv and the environment;
 * the program's path, if it names one, into args_exe. *argv_out: the
 * pointers. */
static status_t take_args(handle_t h, char ***argv_out, int *argc)
{
    struct startup_args hd;
    status_t st = jam_vmo_read(h, 0, &hd, sizeof(hd));
    if (st != OK)
        return st;
    uint64_t n = (uint64_t)hd.argc + hd.envc, slots = (n + 2) * sizeof(char *);
    if (hd.magic != STARTUP_ARGS_MAGIC || hd.version != STARTUP_ARGS_VERSION ||
        slots > STARTUP_ARGS_MAX || hd.strings_len > STARTUP_ARGS_MAX - slots ||
        hd.exe_len == 1 || hd.exe_len > STARTUP_ARGS_EXE_MAX)
        return ERR_INVALID_ARGS;
    uint64_t size = (slots + hd.strings_len + hd.exe_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    handle_t self = startup_handle(SR_SELF_VMAR), v;
    uint64_t addr = 0;
    st = jam_vmo_create(size, 0, &v);
    if (st != OK)
        return st;
    st = jam_vmar_map(self, v, 0, size, VMAR_READ | VMAR_WRITE, &addr);
    jam_handle_close(v);   /* the mapping keeps it */
    if (st != OK)
        return st;
    char **ptrs = (char **)(uintptr_t)addr, *s = (char *)(uintptr_t)addr + slots;
    st = jam_vmo_read(h, sizeof(hd), s, (uint64_t)hd.strings_len + hd.exe_len);
    if (st == OK)
        st = point_strings(s, hd.strings_len, hd.argc, hd.envc, ptrs, (uint32_t)n);
    const char *exe = s + hd.strings_len;
    if (st == OK && hd.exe_len &&
        (exe[0] != '/' || strnlen(exe, hd.exe_len) != hd.exe_len - 1))
        st = ERR_INVALID_ARGS;   /* a path is absolute, and ends at its NUL */
    if (st != OK) {
        jam_vmar_unmap(self, addr, size);
        return st;
    }
    args_exe = hd.exe_len ? exe : NULL;
    *argv_out = ptrs;
    *argc = (int)hd.argc;
    return OK;
}

/* Check the message and point argv and the environment into it. */
static status_t parse_message(uint32_t nbytes, uint32_t nhandles, int *argc)
{
    const struct startup_msg *m = (const struct startup_msg *)msg_buf;
    if (nbytes < sizeof(*m) || m->magic != STARTUP_MAGIC || m->version != STARTUP_VERSION)
        return ERR_INVALID_ARGS;
    if (m->nhandles != nhandles || nhandles > STARTUP_MAX_HANDLES)
        return ERR_INVALID_ARGS;
    if (m->strings_len != nbytes - sizeof(*m))
        return ERR_INVALID_ARGS;
    status_t st = point_strings((char *)msg_buf + sizeof(*m), m->strings_len, m->argc, m->envc,
                                str_ptrs, STARTUP_MAX_STRS);
    if (st != OK)
        return st;
    for (unsigned i = 0; i < nhandles; i++)
        msg_roles[i] = m->roles[i];
    msg_nhandles = nhandles;
    *argc = (int)m->argc;
    return OK;
}

/* Check a promotion (nbytes in msg_buf, nhandles at the table's end) and
 * point argv and the environment into it. */
static status_t parse_promotion(uint32_t nbytes, uint32_t nhandles, int *argc)
{
    const struct standby_msg *m = (const struct standby_msg *)msg_buf;
    if (nbytes < sizeof(*m) || m->txid || m->magic != STANDBY_MAGIC ||
        m->version != STANDBY_VERSION || m->reserved)
        return ERR_INVALID_ARGS;
    if (m->nhandles != nhandles || nhandles > STANDBY_MAX_HANDLES)
        return ERR_INVALID_ARGS;
    if (m->strings_len != nbytes - sizeof(*m))
        return ERR_INVALID_ARGS;
    for (unsigned i = 0; i < nhandles; i++)
        if (m->roles[i] == SR_STANDBY)
            return ERR_INVALID_ARGS;   /* a spare is promoted once */
    status_t st = point_strings((char *)msg_buf + sizeof(*m), m->strings_len, m->argc, m->envc,
                                str_ptrs, STARTUP_MAX_STRS);
    if (st != OK)
        return st;
    for (unsigned i = 0; i < nhandles; i++)
        msg_roles[msg_nhandles + i] = m->roles[i];
    msg_nhandles += nhandles;
    promoted_kill_ns = m->kill_ns;
    *argc = (int)m->argc;
    return OK;
}

/* Close the table's entry i and close the gap. */
static void drop_entry(unsigned i)
{
    jam_handle_close(msg_handles[i]);
    for (; i + 1 < msg_nhandles; i++) {
        msg_handles[i] = msg_handles[i + 1];
        msg_roles[i] = msg_roles[i + 1];
    }
    msg_nhandles--;
}

/* A warm spare: wait on ch (SR_STANDBY, table entry `at`) for the
 * promotion and return the new argc, or end the process. */
static int await_promotion(handle_t ch, unsigned at)
{
    uint32_t nbytes = 0, nhandles = 0;
    struct channel_read_args a = {
        .h = ch,
        .rd_cap = sizeof(msg_buf),
        .rd_bytes = (uint64_t)(uintptr_t)msg_buf,
        .rd_actual = (uint64_t)(uintptr_t)&nbytes,
        .rd_handles = (uint64_t)(uintptr_t)&msg_handles[msg_nhandles],
        .rd_hcap = STANDBY_MAX_HANDLES,
        .rd_hactual = (uint64_t)(uintptr_t)&nhandles,
    };
    status_t st;
    for (;;) {
        st = jam_channel_read(&a);
        if (st != ERR_SHOULD_WAIT)
            break;
        signals_t seen;
        st = jam_object_wait_one(ch, SIG_READABLE | SIG_PEER_CLOSED, DEADLINE_NEVER, &seen);
        if (st != OK)
            break;
    }
    if (st == ERR_PEER_CLOSED)
        jam_process_exit(0);   /* dismissed */
    int argc = 0;
    bool got = st == OK;   /* the read delivered its handles */
    if (got)
        st = parse_promotion(nbytes, nhandles, &argc);
    if (st != OK) {
        for (unsigned i = 0; got && i < nhandles && i < STANDBY_MAX_HANDLES; i++)
            jam_handle_close(msg_handles[msg_nhandles + i]);
        say("a spare given no usable promotion", st);
        jam_process_exit(STANDBY_BAD_PROMOTION);
    }
    promoted_at_ns = now();
    drop_entry(at);
    return argc;
}

/* The table entry of the first handle with role, or msg_nhandles. */
static unsigned entry_of(uint32_t role)
{
    unsigned i = 0;
    while (i < msg_nhandles && msg_roles[i] != role)
        i++;
    return i;
}

void startup_read(handle_t ch, int *argc_out, char ***argv_out)
{
    int argc = 0;
    uint32_t nbytes = 0, nhandles = 0;
    start_channel = ch;
    start_status = read_message(ch, &nbytes, &nhandles);
    if (start_status == OK) {
        start_status = parse_message(nbytes, nhandles, &argc);
        /* Handles the message carried are useless if we can't say what
         * they are for; close them rather than leak them. */
        for (unsigned i = 0; start_status != OK && i < nhandles && i < STARTUP_MAX_HANDLES; i++)
            jam_handle_close(msg_handles[i]);
    }
    if (start_status != OK) {
        msg_nhandles = 0;
        argc = 0;
        str_ptrs[0] = str_ptrs[1] = NULL;
        say("no usable startup message", start_status);
    }
    char **argv = str_ptrs;
    unsigned at = entry_of(SR_ARGS);
    if (at < msg_nhandles) {
        /* The strings are all there: the message has none of its own. */
        const struct startup_msg *m = (const struct startup_msg *)msg_buf;
        start_status = m->argc || m->envc ? ERR_INVALID_ARGS
                                          : take_args(msg_handles[at], &argv, &argc);
        drop_entry(at);
        if (start_status != OK) {
            argv = str_ptrs;
            argc = 0;
            str_ptrs[0] = str_ptrs[1] = NULL;
            say("no usable argument VMO", start_status);
        }
    }
    unsigned sb = entry_of(SR_STANDBY);
    if (sb < msg_nhandles) {
        argc = await_promotion(msg_handles[sb], sb);
        argv = str_ptrs;
    }
    *argc_out = argc;
    *argv_out = argv;
}
