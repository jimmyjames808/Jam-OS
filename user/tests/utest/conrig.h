/* utest: a connection console to test, and its remote terminal (conrig.c):
 * a console started as init's session service will start one, its ADMIN
 * channel (SR_USER + 0), master channel (CONN_MASTER_ROLE) and control
 * channel (CONN_CTL_ROLE) ours, and the "remote terminal" this rig, typing
 * into the master and reading what it shows. conconn.c (the connection
 * back end) and contty.c (the terminal's POSIX side) use it. */
#pragma once

#include <os.h>

#define CON_WAIT   (5 * NS_PER_S)
#define STEP_MS    10000u
#define SHELL_LEVEL 1   /* console.new_client: L_SHELL (user/services/console/console.h) */
#define OUT_CAP    (1u << 16)

struct conrig {
    handle_t job, proc;       /* the console */
    handle_t admin;           /* its ADMIN client */
    handle_t master, ctl;     /* ours */
    char     out[OUT_CAP];    /* what the master gave us (the last OUT_CAP bytes) */
    size_t   len, from;       /* its length, and where the next expect looks from */
};

/* The console under test (one at a time). */
extern struct conrig rig;

/* Start the console with argv (argc entries) and the kernel log if klog. */
bool rig_start(const char *const *argv, int argc, bool klog);
/* Close our ends (and kill its job unless ok), then wait (up to CON_WAIT)
 * for the console to end and its job to hold nothing. Its job is below
 * ours, so until then our job counts the console's handles, threads and
 * message bytes: a test after this one that compares its job's counts
 * (jwlc_real_compositor does) must not see a console on its way out.
 * False if it had to be killed or its job kept something. */
bool rig_stop(bool ok);
/* One message from the master, appended to out (until deadline). */
bool rig_take(uint64_t until);
/* Wait until the bytes needle (nl of them, or a string) come on the
 * master, after the last match: false (and the master's last bytes
 * printed) if not within ms. */
bool rig_expect_n(const char *needle, size_t nl, unsigned ms);
bool rig_expect(const char *needle, unsigned ms);
/* Read the master for ms milliseconds, whatever comes. */
void rig_settle(unsigned ms);
/* Type n bytes of s at the remote terminal. */
bool rig_typed(const char *s, size_t n);
/* needle is in what came after the last match. */
bool rig_saw(const char *needle);
/* console.size on client is cols x rows. */
bool rig_size_is(handle_t client, uint16_t cols, uint16_t rows);
/* proc ended (within CON_WAIT) with code, not killed. */
bool rig_exit_code_is(handle_t proc, int code);
/* The master's last bytes, printed. */
void rig_dump_tail(void);
