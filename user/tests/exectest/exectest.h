/* exectest and execwide: the programs utest's /svc/exec tests copy to
 * /data, approve and start (user/tests/utest/execsvc.c; init's exec.c).
 * One body (probe.c) with two lists: exectest's narrow one (`svc exec`,
 * /data read-only), execwide's wide one (`svc exec`, /data writable,
 * /boot, `right sysinfo`). A test starts each from a parent with more and
 * from one with less, and checks it got exactly its own approval.
 *   <prog> probe                   exits with what it was given: EXT_* bits
 *   <prog> launch <path> [args]    starts <path> through /svc/exec with
 *                                  args, waits for it, exits with its code
 *                                  (EXT_LAUNCH_ERR + the step that failed:
 *                                  it couldn't) */
#pragma once

#include <os.h>

/* What `probe` was given (its exit code: these bits). */
#define EXT_EXEC     (1u << 0)   /* /svc/exec */
#define EXT_DATA     (1u << 1)   /* /data */
#define EXT_DATA_W   (1u << 2)   /* /data takes a new file */
#define EXT_BOOT     (1u << 3)   /* /boot */
#define EXT_ROOT     (1u << 4)   /* a root resource (SR_RESOURCE) */
#define EXT_SYSINFO  (1u << 5)   /* ... with RIGHT_ROOT_SYSINFO */
#define EXT_ROOT_TX  (1u << 6)   /* ... with RIGHT_TRANSFER (never) */
#define EXT_DEVMGR   (1u << 7)   /* /svc/devmgr (no program from /data) */
#define EXT_VOLUMES  (1u << 8)   /* /svc/volumes (no program from /data) */
#define EXT_SIGNALS  (1u << 9)   /* an SR_SIGNALS channel */

/* exectest's approval and execwide's, as probe reports them. */
#define EXT_NARROW (EXT_EXEC | EXT_DATA | EXT_SIGNALS)
#define EXT_WIDE   (EXT_EXEC | EXT_DATA | EXT_DATA_W | EXT_BOOT | EXT_ROOT | EXT_SYSINFO | \
                    EXT_SIGNALS)

#define EXT_LAUNCH_ERR 1000   /* launch's codes: this plus the step */
#define EXT_PROBE_FILE "/data/exectest.probe"   /* probe's try at a new file */

/* A child through /svc/exec (client.c): the program at path (absolute,
 * cleaned, opened here FS_READ through our own namespace) sealed, then
 * started with argv (argc strings; no environment) in a job inside ours.
 * *proc, *job and *sig (our end of its signal channel) on OK. A refusal's
 * status as /svc/exec gave it; *step says which call (1 the service, 2 the
 * file, 3 exec.open, 4 exec_prog.start, 5 anything else). */
status_t exectest_start(const char *path, int argc, const char *const *argv, handle_t *proc,
                        handle_t *job, handle_t *sig, int *step);
/* The program a probe ran as: its bits (main.c's). */
int      exectest_main(int argc, char **argv);
