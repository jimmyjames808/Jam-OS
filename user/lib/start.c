/* Startup: read the startup message (<jam/startup.h>) from the channel in
 * rdi, turn it into argc/argv/environ and a table of handles by role, run
 * main, exit with its result.
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
 * STANDBY_BAD_PROMOTION, its handles closed. */
#include <os.h>
#include <svcstate.h>

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
char           **environ = &str_ptrs[1];

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
    }
    return role >= SR_USER ? "user" : "?";
}

/* Read the one message, waiting for it if it isn't there yet. */
static status_t read_message(handle_t ch, uint32_t *nbytes, uint32_t *nhandles)
{
    struct channel_read_args a = {
        .h = ch,
        .bytes_cap = sizeof(msg_buf),
        .bytes = (uint64_t)(uintptr_t)msg_buf,
        .actual_bytes = (uint64_t)(uintptr_t)nbytes,
        .handles = (uint64_t)(uintptr_t)msg_handles,
        .handles_cap = STARTUP_MAX_HANDLES,
        .actual_handles = (uint64_t)(uintptr_t)nhandles,
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

/* Point argv/environ at argc + envc NUL-terminated strings, all inside
 * [s, s + len). */
static status_t point_strings(char *s, uint32_t len, uint32_t argc, uint32_t envc)
{
    if (argc > STARTUP_MAX_STRS || envc > STARTUP_MAX_STRS - argc)
        return ERR_INVALID_ARGS;
    char *end = s + len;
    unsigned n = argc + envc, slot = 0;
    for (unsigned i = 0; i < n; i++) {
        size_t l = strnlen(s, (size_t)(end - s));
        if (s + l == end)
            return ERR_INVALID_ARGS;   /* runs off the end */
        if (i == argc)
            slot++;                    /* skip argv's NULL */
        str_ptrs[slot++] = s;
        s += l + 1;
    }
    if (argc == n)
        slot++;                        /* argv's NULL when there is no env */
    str_ptrs[argc] = NULL;
    str_ptrs[slot] = NULL;
    environ = &str_ptrs[argc + 1];
    return OK;
}

/* Check the message and point argv/environ into it. */
static status_t parse_message(uint32_t nbytes, uint32_t nhandles, int *argc)
{
    const struct startup_msg *m = (const struct startup_msg *)msg_buf;
    if (nbytes < sizeof(*m) || m->magic != STARTUP_MAGIC || m->version != STARTUP_VERSION)
        return ERR_INVALID_ARGS;
    if (m->nhandles != nhandles || nhandles > STARTUP_MAX_HANDLES)
        return ERR_INVALID_ARGS;
    if (m->strings_len != nbytes - sizeof(*m))
        return ERR_INVALID_ARGS;
    status_t st = point_strings((char *)msg_buf + sizeof(*m), m->strings_len, m->argc, m->envc);
    if (st != OK)
        return st;
    for (unsigned i = 0; i < nhandles; i++)
        msg_roles[i] = m->roles[i];
    msg_nhandles = nhandles;
    *argc = (int)m->argc;
    return OK;
}

/* Check a promotion (nbytes in msg_buf, nhandles at the table's end) and
 * point argv/environ into it. */
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
    status_t st = point_strings((char *)msg_buf + sizeof(*m), m->strings_len, m->argc, m->envc);
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
        .bytes_cap = sizeof(msg_buf),
        .bytes = (uint64_t)(uintptr_t)msg_buf,
        .actual_bytes = (uint64_t)(uintptr_t)&nbytes,
        .handles = (uint64_t)(uintptr_t)&msg_handles[msg_nhandles],
        .handles_cap = STANDBY_MAX_HANDLES,
        .actual_handles = (uint64_t)(uintptr_t)&nhandles,
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
        printf("libos: a spare given no usable promotion (%s)\n", status_str(st));
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

_Noreturn void libos_start(handle_t ch)
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
        environ = &str_ptrs[1];
        printf("libos: no usable startup message (%s)\n", status_str(start_status));
    }
    unsigned sb = entry_of(SR_STANDBY);
    if (sb < msg_nhandles)
        argc = await_promotion(msg_handles[sb], sb);
    jam_process_exit(main(argc, str_ptrs));
}
