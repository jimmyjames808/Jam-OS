/* ramfs: the tests' RAM filesystem service (ramfs.h).
 *
 *   ramfs                serve the `fs` channel whose server end came as
 *                        SR_USER + 0 (utest starts it so and mounts the
 *                        other end), until its clients are gone
 *   ramfs shell [late]   for the shell scripts: start bin/shell on our own
 *                        console channel with our namespace plus this
 *                        filesystem at /ram, and serve it until that shell
 *                        ends. `run ramfs shell` from the shell gives a
 *                        shell with a writable /ram (Ctrl+C ends it).
 *                        late: /ram is mounted LATE_S seconds after the
 *                        shell started, the way a disk's mount reaches a
 *                        running shell
 *
 * Exits 0 when there is nobody left to serve. */
#include <wants.h>
#include "ramfs.h"

/* What it is given when the shell runs it (<wants.h>). */
JAM_WANTS("mount * rw\n");

#define LATE_S 4
#define POINT  "/ram"   /* where the shell mode mounts us */

static struct fsserver server = {
    .fs_ops = &ramfs_fs_ops, .file_ops = &ramfs_file_ops, .closed = ramfs_closed,
};

/* bin/shell with our console channel and every mount of ours; *ns: our
 * end of its namespace channel. */
static status_t start_shell(handle_t *ns)
{
    handle_t job, proc;
    status_t st = jam_job_create(startup_handle(SR_JOB), 0, &job);
    if (st != OK)
        return st;
    const char *argv[] = { "bin/shell" };
    struct spawn_handle x = { SR_CONSOLE, startup_handle(SR_CONSOLE) };
    struct spawn_args a = {
        .path = "bin/shell", .argc = 1, .argv = argv, .job = job, .extra = &x,
        .nextra = x.h ? 1 : 0, .ns = NS_ALL, .ns_out = ns,
    };
    st = spawn(&a, &proc);
    /* It runs until we are killed (its job is inside ours) or it ends by
     * itself; either way nothing here waits for it. */
    jam_handle_close(job);
    if (st == OK)
        jam_handle_close(proc);
    return st;
}

/* The shell mode: *fs gets the server end to serve. */
static status_t shell_mode(bool late, handle_t *fs)
{
    handle_t client, ns;
    status_t st = jam_channel_create(fs, &client);
    if (st != OK)
        return st;
    st = start_shell(&ns);
    if (st != OK) {
        jam_handle_close(client);
        jam_handle_close(*fs);
        return st;
    }
    if (late) {
        printf("ramfs: mounting " POINT " in %d s\n", LATE_S);
        jam_nanosleep(now() + LATE_S * NS_PER_S);
    }
    st = ns_send_one(ns, POINT, client);   /* consumes client */
    jam_handle_close(ns);
    if (late && st == OK)
        printf("ramfs: " POINT " is mounted\n");
    if (st != OK)
        jam_handle_close(*fs);
    return st;
}

int main(int argc, char **argv)
{
    bool shell = argc > 1 && !strcmp(argv[1], "shell");
    bool late = shell && argc > 2 && !strcmp(argv[2], "late");
    if (argc > 1 && (!shell || argc > (late ? 3 : 2))) {
        printf("usage: ramfs [shell [late]]\n");
        return 2;
    }
    handle_t fs = startup_handle(SR_USER + 0);
    status_t st = shell ? shell_mode(late, &fs) : fs ? OK : ERR_BAD_HANDLE;
    ramfs_init();
    if (st == OK)
        st = fsserver_init(&server);
    if (st == OK)
        st = fsserver_add_fs(&server, fs);
    if (st == OK)
        st = fsserver_run(&server);
    if (st != OK)
        printf("ramfs: %s\n", status_str(st));
    return st == OK ? 0 : 1;
}
