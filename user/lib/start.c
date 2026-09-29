/* Startup: read the startup message (<jam/startup.h>) from the channel in
 * rdi, turn it into argc/argv/environ and a table of handles by role, run
 * main, exit with its result.
 *
 * The message comes from whoever created the process, so it is checked
 * like any other input: a malformed one leaves the program with argc 0 and
 * no handles (and a line in the log) rather than reading past the buffer. */
#include <os.h>

#define STARTUP_BUF_MAX  8192   /* bytes; bigger startup messages are refused */
#define STARTUP_MAX_STRS 128    /* argv + environment strings */

static _Alignas(8) uint8_t msg_buf[STARTUP_BUF_MAX];
static handle_t  msg_handles[STARTUP_MAX_HANDLES];
static uint32_t  msg_roles[STARTUP_MAX_HANDLES];
static unsigned  msg_nhandles;
static handle_t  start_channel;
static status_t  start_status = ERR_BAD_STATE;
static char     *str_ptrs[STARTUP_MAX_STRS + 2];   /* argv NULL envp NULL */
char           **environ = &str_ptrs[1];

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

/* Check the message and point argv/environ into it. */
static status_t parse_message(uint32_t nbytes, uint32_t nhandles, int *argc)
{
    const struct startup_msg *m = (const struct startup_msg *)msg_buf;
    if (nbytes < sizeof(*m) || m->magic != STARTUP_MAGIC || m->version != STARTUP_VERSION)
        return ERR_INVALID_ARGS;
    if (m->nhandles != nhandles || nhandles > STARTUP_MAX_HANDLES)
        return ERR_INVALID_ARGS;
    if (m->argc > STARTUP_MAX_STRS || m->envc > STARTUP_MAX_STRS - m->argc)
        return ERR_INVALID_ARGS;
    if (m->strings_len != nbytes - sizeof(*m))
        return ERR_INVALID_ARGS;

    /* argc + envc NUL-terminated strings, all inside strings_len. */
    char *s = (char *)msg_buf + sizeof(*m);
    char *end = s + m->strings_len;
    unsigned n = m->argc + m->envc, slot = 0;
    for (unsigned i = 0; i < n; i++) {
        size_t len = strnlen(s, (size_t)(end - s));
        if (s + len == end)
            return ERR_INVALID_ARGS;   /* runs off the end */
        if (i == m->argc)
            slot++;                    /* skip argv's NULL */
        str_ptrs[slot++] = s;
        s += len + 1;
    }
    if (m->argc == n)
        slot++;                        /* argv's NULL when there is no env */
    str_ptrs[m->argc] = NULL;
    str_ptrs[slot] = NULL;
    environ = &str_ptrs[m->argc + 1];

    for (unsigned i = 0; i < nhandles; i++)
        msg_roles[i] = m->roles[i];
    msg_nhandles = nhandles;
    *argc = (int)m->argc;
    return OK;
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
    jam_process_exit(main(argc, str_ptrs));
}
