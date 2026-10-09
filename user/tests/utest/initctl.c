/* utest: init's control channel (abi/idl/initctl.idl) with names as
 * strings, on the live init: every malformed form of kill's name, mount's
 * path, terminal's command and launch's app is refused (M12 P6b).
 *
 * The requests are built by hand, bytes and all, so the test says what is
 * on the wire: after the fixed fields, each string is a u16 count and its
 * bytes (tools/genidl.py). The generated server refuses a count past the
 * protocol's cap, bytes left over or missing, and a string that isn't
 * UTF-8 or holds a NUL; init then refuses what its method does not take
 * (a space or a control character in a name, a path that is no stick's,
 * an app that is not on its list). None of the requests starts, kills or
 * remounts anything: the valid forms are names that do not exist.
 *
 *   initctl_strings   kill, mount, terminal and launch against init; the
 *                     old fixed-array form of kill (40 bytes) is refused;
 *                     init answers a kill of a name it has not got at the
 *                     end
 *
 * Needs init's control channel (`svc init`, the shell's `run utest` has
 * it); started by init's own run it has none, and is skipped. */
#define CHECK_PROG "utest"
#define CHECK_CUR  utest_cur
#include <check.h>
#include <os.h>
#include "utest.h"

#define ASK_WAIT (10 * NS_PER_S)

#define INITCTL_ID      18u
#define M_KILL          1u
#define M_MOUNT         4u
#define M_TERMINAL      9u
#define M_LAUNCH        10u

/* A request by hand: txid (the kernel's), the ordinal, then body (n bytes). */
static status_t ask(handle_t ctl, uint32_t method, const void *body, uint32_t n)
{
    uint8_t q[256];
    uint8_t rep[64];
    uint32_t ordinal = (INITCTL_ID << 16) | method, got = 0;
    memset(q, 0, 4);
    memcpy(q + 4, &ordinal, 4);
    memcpy(q + 8, body, n);
    struct channel_call_args a = {
        .h = ctl, .wr_len = 8 + n, .wr_bytes = (uint64_t)(uintptr_t)q,
        .rd_cap = sizeof(rep), .rd_bytes = (uint64_t)(uintptr_t)rep,
        .rd_actual = (uint64_t)(uintptr_t)&got, .deadline_ns = now() + ASK_WAIT,
    };
    status_t st = jam_channel_call(&a);
    if (st != OK)
        return st;
    if (got < 8)
        return ERR_INTERNAL;
    int32_t status;
    memcpy(&status, rep + 4, 4);
    return status;
}

/* A string field: its u16 count, then s (len bytes). */
static uint32_t put_str(uint8_t *b, uint32_t at, const void *s, uint32_t len, uint16_t count)
{
    memcpy(b + at, &count, 2);
    memcpy(b + at + 2, s, len);
    return at + 2 + len;
}

/* kill with the string s of len bytes and a count of `count`. */
static status_t kill_raw(handle_t ctl, const void *s, uint32_t len, uint16_t count)
{
    uint8_t b[128];
    return ask(ctl, M_KILL, b, put_str(b, 0, s, len, count));
}

static status_t kill_name(handle_t ctl, const char *name)
{
    return kill_raw(ctl, name, (uint32_t)strlen(name), (uint16_t)strlen(name));
}

/* mount: the fixed byte `writable`, then the path. */
static status_t mount_raw(handle_t ctl, const char *path, uint16_t count, uint8_t writable)
{
    uint8_t b[64];
    b[0] = writable;
    return ask(ctl, M_MOUNT, b, put_str(b, 1, path, (uint32_t)strlen(path), count));
}

static status_t mount_path(handle_t ctl, const char *path, uint8_t writable)
{
    return mount_raw(ctl, path, (uint16_t)strlen(path), writable);
}

/* terminal and launch take one string and nothing else. */
static status_t str_call(handle_t ctl, uint32_t method, const void *s, uint32_t len)
{
    uint8_t b[256];
    return ask(ctl, method, b, put_str(b, 0, s, len, (uint16_t)len));
}

static bool kills(handle_t ctl)
{
    char max[33], over[34];
    memset(max, 'a', 31);
    max[31] = '\0';
    memset(over, 'a', 32);
    over[32] = '\0';
    CHECK_ST(kill_name(ctl, "nosuchproc"), ERR_NOT_FOUND);
    CHECK_ST(kill_name(ctl, max), ERR_NOT_FOUND);   /* 31 bytes: the most */
    CHECK_ST(kill_name(ctl, over), ERR_INVALID_ARGS);   /* 32: past the cap */
    CHECK_ST(kill_name(ctl, ""), ERR_INVALID_ARGS);
    CHECK_ST(kill_name(ctl, "a b"), ERR_INVALID_ARGS);
    CHECK_ST(kill_name(ctl, "a\tb"), ERR_INVALID_ARGS);
    CHECK_ST(kill_name(ctl, "a\x7f"), ERR_INVALID_ARGS);
    CHECK_ST(kill_name(ctl, "caf\xc3\xa9"), ERR_INVALID_ARGS);   /* UTF-8, but not a name */
    CHECK_ST(kill_raw(ctl, "a\xc0\x80", 3, 3), ERR_INVALID_ARGS);   /* an overlong NUL */
    CHECK_ST(kill_raw(ctl, "a\0b", 3, 3), ERR_INVALID_ARGS);        /* a NUL inside */
    CHECK_ST(kill_raw(ctl, "abcd", 4, 5), ERR_INVALID_ARGS);        /* a count past the bytes */
    CHECK_ST(kill_raw(ctl, "abcde", 5, 4), ERR_INVALID_ARGS);       /* a byte left over */
    CHECK_ST(kill_raw(ctl, "abcd", 4, 0xffff), ERR_INVALID_ARGS);
    CHECK_ST(ask(ctl, M_KILL, "\x01", 1), ERR_INVALID_ARGS);        /* half a count */
    CHECK_ST(ask(ctl, M_KILL, "", 0), ERR_INVALID_ARGS);            /* no count */
    /* the old form: a fixed 32-byte name field */
    uint8_t old[32] = "nosuchproc";
    CHECK_ST(ask(ctl, M_KILL, old, sizeof(old)), ERR_INVALID_ARGS);
    CHECK_ST(kill_name(ctl, "init"), ERR_ACCESS_DENIED);   /* never */
    return true;
}

static bool mounts(handle_t ctl)
{
    CHECK_ST(mount_path(ctl, "/data", 1), ERR_ACCESS_DENIED);   /* not a stick's */
    CHECK_ST(mount_path(ctl, "/boot", 0), ERR_ACCESS_DENIED);
    CHECK_ST(mount_path(ctl, "/esp", 1), ERR_ACCESS_DENIED);
    CHECK_ST(mount_path(ctl, "/usb", 1), ERR_ACCESS_DENIED);    /* no number */
    CHECK_ST(mount_path(ctl, "/usb0x", 1), ERR_ACCESS_DENIED);
    CHECK_ST(mount_path(ctl, "usb0", 1), ERR_INVALID_ARGS);     /* not absolute */
    CHECK_ST(mount_path(ctl, "", 1), ERR_INVALID_ARGS);
    CHECK_ST(mount_path(ctl, "/usb0", 2), ERR_INVALID_ARGS);    /* writable is 0 or 1 */
    CHECK_ST(mount_path(ctl, "/usb1234567890123", 1), ERR_INVALID_ARGS);   /* 16 bytes: past 15 */
    CHECK_ST(mount_raw(ctl, "/usb0", 6, 1), ERR_INVALID_ARGS);
    CHECK_ST(mount_raw(ctl, "/usb0", 4, 1), ERR_INVALID_ARGS);
    /* the old form: a fixed 16-byte path, then the byte */
    uint8_t old[17] = "/data";
    old[16] = 1;
    CHECK_ST(ask(ctl, M_MOUNT, old, sizeof(old)), ERR_INVALID_ARGS);
    return true;
}

static bool terminals_and_launches(handle_t ctl)
{
    char over[130];
    memset(over, 'a', 128);   /* 128 bytes: past the cap of 127 */
    CHECK_ST(str_call(ctl, M_TERMINAL, over, 128), ERR_INVALID_ARGS);
    CHECK_ST(str_call(ctl, M_TERMINAL, "ab\x01", 3), ERR_INVALID_ARGS);   /* one line, as typed */
    CHECK_ST(str_call(ctl, M_TERMINAL, "a\nb", 3), ERR_INVALID_ARGS);
    CHECK_ST(str_call(ctl, M_TERMINAL, "a\x7f", 2), ERR_INVALID_ARGS);
    CHECK_ST(str_call(ctl, M_TERMINAL, "\xc0\xaf", 2), ERR_INVALID_ARGS);   /* not UTF-8 */
    CHECK_ST(str_call(ctl, M_TERMINAL, "a\0b", 3), ERR_INVALID_ARGS);
    CHECK_ST(ask(ctl, M_TERMINAL, "", 0), ERR_INVALID_ARGS);
    CHECK_ST(str_call(ctl, M_LAUNCH, "nosuchapp", 9), ERR_NOT_FOUND);
    CHECK_ST(str_call(ctl, M_LAUNCH, "terminal", 8), ERR_NOT_FOUND);   /* init's own, not a program */
    CHECK_ST(str_call(ctl, M_LAUNCH, "a234567890123456", 16), ERR_INVALID_ARGS);   /* 16: past 15 */
    CHECK_ST(str_call(ctl, M_LAUNCH, "", 0), ERR_INVALID_ARGS);
    CHECK_ST(str_call(ctl, M_LAUNCH, "Jam", 3), ERR_INVALID_ARGS);   /* lower case only */
    CHECK_ST(str_call(ctl, M_LAUNCH, "a b", 3), ERR_INVALID_ARGS);
    CHECK_ST(str_call(ctl, M_LAUNCH, "../x", 4), ERR_INVALID_ARGS);
    /* the old forms: fixed arrays of 128 and 16 */
    uint8_t cmd[128] = "ls";
    CHECK_ST(ask(ctl, M_TERMINAL, cmd, sizeof(cmd)), ERR_INVALID_ARGS);
    uint8_t app[16] = "nosuchapp";
    CHECK_ST(ask(ctl, M_LAUNCH, app, sizeof(app)), ERR_INVALID_ARGS);
    return true;
}

bool t_initctl_strings(void)
{
    handle_t ctl = svc_get(SVC_INIT);
    if (!ctl) {
        printf("utest: %s: no init control channel (not started by the shell?): skipped\n",
               utest_cur);
        return true;
    }
    if (!kills(ctl) || !mounts(ctl) || !terminals_and_launches(ctl))
        return false;
    CHECK_ST(kill_name(ctl, "nosuchproc"), ERR_NOT_FOUND);   /* init is still answering */
    return true;
}
