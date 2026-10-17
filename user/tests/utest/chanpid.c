/* utest: channel_read's CHANNEL_READ_PID (abi/syscalls.def): every
 * message carries the pid of the process that wrote it, stamped as it is
 * written, which the POSIX layer's SCM_CREDENTIALS reports (a message
 * from another process: posixtest's scm section, Mojo's pattern).
 *
 *   chan_writer_pid   a message we wrote says our pid, in the uint32_t
 *                     after the handles' count, read whole or left queued
 *                     (ERR_BUFFER_TOO_SMALL: the sizes and the pid, the
 *                     message kept); without the flag that word is never
 *                     written; an unknown flag, or the flag without
 *                     rd_hactual, is ERR_INVALID_ARGS with the message
 *                     still queued */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

#define NOT_WRITTEN 0xdeadbeefu

/* One channel_read of h into b (cap bytes) with flags; sizes[0..2] get
 * the bytes, the handles and (asked for) the writer's pid. */
static status_t read_with(handle_t h, void *b, uint32_t cap, uint32_t flags, uint32_t *sizes,
                          bool hactual)
{
    struct channel_read_args a = {
        .h = h, .rd_cap = cap, .rd_bytes = (uint64_t)(uintptr_t)b, .rd_flags = flags,
        .rd_actual = (uint64_t)(uintptr_t)&sizes[0],
        .rd_hactual = hactual ? (uint64_t)(uintptr_t)&sizes[1] : 0,
    };
    return jam_channel_read(&a);
}

bool t_chan_writer_pid(void)
{
    struct process_info pi;
    CHECK_ST(jam_process_get_info(startup_handle(SR_SELF_PROCESS), &pi), OK);
    handle_t a, b;
    CHECK_ST(jam_channel_create(&a, &b), OK);
    char buf[16];
    uint32_t s[3];
    /* read whole: the bytes, no handles, our pid */
    CHECK_ST(jam_channel_write(a, "hello", 5, NULL, 0), OK);
    s[0] = s[1] = s[2] = NOT_WRITTEN;
    CHECK_ST(read_with(b, buf, sizeof(buf), CHANNEL_READ_PID, s, true), OK);
    CHECK(s[0] == 5 && s[1] == 0 && s[2] == pi.pid);
    /* left queued: the sizes and the pid, then read whole */
    CHECK_ST(jam_channel_write(a, "12345678", 8, NULL, 0), OK);
    s[0] = s[1] = s[2] = NOT_WRITTEN;
    CHECK_ST(read_with(b, NULL, 0, CHANNEL_READ_PID, s, true), ERR_BUFFER_TOO_SMALL);
    CHECK(s[0] == 8 && s[1] == 0 && s[2] == pi.pid);
    /* refused before anything: an unknown flag, the flag without rd_hactual */
    CHECK_ST(read_with(b, buf, sizeof(buf), 1u << 1, s, true), ERR_INVALID_ARGS);
    CHECK_ST(read_with(b, buf, sizeof(buf), CHANNEL_READ_PID, s, false), ERR_INVALID_ARGS);
    /* without the flag the third word is not touched */
    s[0] = s[1] = s[2] = NOT_WRITTEN;
    CHECK_ST(read_with(b, buf, sizeof(buf), 0, s, true), OK);
    CHECK(s[0] == 8 && s[1] == 0 && s[2] == NOT_WRITTEN && !memcmp(buf, "12345678", 8));
    /* nothing queued: ERR_SHOULD_WAIT */
    CHECK_ST(read_with(b, buf, sizeof(buf), CHANNEL_READ_PID, s, true), ERR_SHOULD_WAIT);
    jam_handle_close(a);
    jam_handle_close(b);
    return true;
}
